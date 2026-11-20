/*
 * OpenScope 2C53T - oscilloscope screen: spectrum and split views
 *
 * The spectrum of a record, its reference level and notes, and the split
 * view (trace above, spectrum below).
 */

#include "scope_ui_parts.h"

#if FEATURE_FFT
/* ═══════════════════════════════════════════════════════════════════
 * FFT views: the spectrum, its source and reference level
 * ═══════════════════════════════════════════════════════════════════ */
/* ── Display reference auto-ranging ─────────────────────────────────
 *
 * fft_process() reports magnitude in dB relative to ONE ADC COUNT
 * (20*log10(|X| * 2/N), |X| in counts), not dBFS. A full-scale signal
 * therefore peaks near 20*log10(32767) = 90 dB. A fixed reference of 0 dB
 * would put the top of the display about 90 dB below the signal, and every
 * populated bin would clamp to normalized = 0, the top colour of the ramp.
 *
 * So the reference tracks the observed peak. It follows frame_max + headroom,
 * moving up immediately (so a growing signal never clips against the top)
 * and easing down slowly through a dead band (so a jittering peak does not
 * make the colours breathe). cfg->ref_level_db is a user trim applied on
 * top of it; fft_adjust_ref_level() changes that trim.
 *
 * Limit: this ranges off the peak only. A spectrum with no dynamic range
 * (broadband noise and nothing else) has its mean within about 10-15 dB of
 * its peak and renders as a flat, fairly hot field. Telling it apart would
 * need a floor estimator as well.
 */
#define FFT_REF_HEADROOM_DB   6.0f   /* keep the peak just below the top    */

#define FFT_REF_DEAD_BAND_DB  3.0f   /* ignore downward wobble below this   */

#define FFT_REF_DECAY         0.10f  /* per-frame fraction on the way down  */

static float fft_ref_tracked = 0.0f;

static bool  fft_ref_primed  = false;

/* ── Where the spectrum views get their samples ──────────────────────
 *
 * Every FFT view (spectrum, split, waterfall) analyses the live record when
 * there is one, and the view is LABELLED with what it got, so a user can
 * tell the spectrum of the probe from a built-in demo signal. The record is
 * the ACTIVE channel's (the one MOVE selects, ui.ach): dsp/fft_source.h
 * decides the buffer, the rate and the label, and says "CH2 is off" instead
 * of a spectrum when that channel is off. The gate is the trace's
 * (fpga_data_ready()), and the tear-check is the measurement badges' (the
 * frame generation counter).
 *
 *  - The whole record goes into the transform (1024 samples), windowed over
 *    the record rather than over the transform length (fft.c FFT_WINDOW_AT).
 *  - The frequency axis is real only on measured timebase codes: the rate
 *    comes from scope_timebase_sample_rate() for the code in force, and is
 *    refused (rate 0: the axis shows bins, "--") when the code is unmeasured
 *    or when the display's code and the FPGA's reg-0x01 code disagree. This
 *    is the rule `fpga scope freq` applies. A missing number is better than
 *    a confident wrong one.
 *  - The synthetic demo source exists only in the emulator build (see
 *    FFT_DEMO_REACHABLE). On hardware, with no capture yet, the views say so
 *    and draw nothing.
 */
static uint16_t g_fft_last_n;                /* record samples the last live spectrum analysed (shell screen diagnostic) */

uint16_t scope_ui_fft_last_n(void) { return g_fft_last_n; }

#define FFT_DEMO_SAMPLE_RATE_HZ  44100.0f

#ifdef EMULATOR_BUILD
/* The synthetic demo source: the emulator build only (it has no FPGA, so no
 * record ever arrives). On hardware the spectrum is of the live record or
 * it is blank. */
#define FFT_DEMO_REACHABLE 1

#else
#define FFT_DEMO_REACHABLE 0

#endif
/* Sample rate the live spectrum may claim, 0 when it may not. */
static float fft_live_rate_in_force(void)
{
    const scope_state_t *ss = scope_state_get();
    if (fpga_acq_rate_idx_get() != ss->timebase_idx)
        return 0.0f;                         /* display and hardware disagree */
    return fpga_record_sample_rate();        /* x2 for an interleaved record */
}

/* The published record's facts and the UI's channel state, as one snapshot
 * for fft_source_select(). Called inside the generation check of the copy. */
static void fft_source_now(fft_source_t *sel)
{
    const scope_state_t *ss = scope_state_get();
    fft_source_in_t in;
    in.active_ch   = active_channel ? 1u : 0u;
    in.enabled[0]  = ss->ch1.enabled;
    in.enabled[1]  = ss->ch2.enabled;
    in.rec_has[0]  = fpga_acq_record_has_ch(0u);
    in.rec_has[1]  = fpga_acq_record_has_ch(1u);
    in.rec_merged  = fpga_acq_record_interleaved();
    in.rec_rate_hz = fft_live_rate_in_force();
    fft_source_select(&in, sel);
}

/* Averaging, max-hold, the tracked reference and the waterfall rows are
 * history of ONE source. When the source changes (MOVE to the other
 * channel, the channel switched off, live <-> demo) they are dropped before
 * the next transform, on the task that runs fft_process, so two channels are
 * never mixed. No fft_config_t field is touched: the scope's redraw epoch
 * mixes those. */
static uint8_t fft_hist_key = 0xFFu;       /* fft_source_key(), 3 = demo */

bool    waterfall_restart;

static void fft_history_track(uint8_t key)
{
    if (key == fft_hist_key) return;
    fft_hist_key = key;
    fft_reset_history();
    fft_ref_primed = false;
    waterfall_restart = true;
}

static const volatile uint8_t *fft_source_buf(const fft_source_t *sel)
{
    return sel->buf ? fpga_get_ch2_buf() : fpga_get_ch1_buf();
}

/* Fill sbuf with the samples to transform and report their count (*n_out) and
 * sample rate (*fs_out). Returns FFT_SRC_LIVE for the live record,
 * FFT_SRC_DEMO for the demo signal (emulator build only), FFT_SRC_NONE for
 * nothing to show. *sel says which channel, and why not in sel->note when
 * the answer is FFT_SRC_NONE because of the channel. Changes the FFT sample
 * rate, the history key and the FFT AUTO request. */
fft_src_t fft_prepare_input(int16_t *sbuf, uint16_t *n_out,
                                   float *fs_out, fft_source_t *sel)
{
    *n_out = 0;
    *fs_out = 0.0f;

    fft_source_now(sel);
    if (sel->state == FFT_SEL_OFF) {             /* in every build, demo included */
        fft_history_track(0u);
        return FFT_SRC_NONE;
    }

    if (fpga_data_ready() && fft_source_buf(sel) != NULL) {
        uint16_t n = 0;
        for (int attempt = 0; attempt < 3; attempt++) {
            uint32_t g0 = fpga_acq_frame_generation();
            fft_source_now(sel);
            n = 0;
            if (sel->state == FFT_SEL_OK)
                n = fft_live_prepare((const uint8_t *)fft_source_buf(sel), FPGA_ADC_BUF_SIZE,
                                     sbuf, FFT_SIZE);
            if ((g0 & 1u) == 0u && fpga_acq_frame_generation() == g0)
                break;
        }
        if (sel->state != FFT_SEL_OK) {          /* off, or no record of its own yet */
            fft_history_track(0u);
            return FFT_SRC_NONE;
        }
        fft_history_track(fft_source_key(sel));
        *fs_out = sel->rate_hz;
        fft_set_sample_rate(*fs_out);
        *n_out = n;
        g_fft_last_n = n;
        /* FFT AUTO (the key, `fft auto`): on the record this frame analyses. */
        if (n > 0 && fft_auto_take()) fft_auto_configure(sbuf, n);
        return FFT_SRC_LIVE;
    }
    sel->note[0] = '\0';                         /* no capture yet: "waiting", not the channel */

#if FFT_DEMO_REACHABLE
    fft_history_track(3u);
    fft_set_sample_rate(FFT_DEMO_SAMPLE_RATE_HZ);
    test_signal_generate(TEST_SIG_SQUARE, sbuf,
                         FFT_SIZE, FFT_DEMO_SAMPLE_RATE_HZ,
                         1000.0f, 0.0f, 0.8f);
    *fs_out = FFT_DEMO_SAMPLE_RATE_HZ;
    *n_out = FFT_SIZE;
    if (fft_auto_take()) fft_auto_configure(sbuf, FFT_SIZE);
    return FFT_SRC_DEMO;
#else
    return FFT_SRC_NONE;
#endif
}

float fft_display_ref(const fft_config_t *cfg, const float *data,
                             uint16_t start_bin, uint16_t end_bin)
{
    if (end_bin >= FFT_BINS) end_bin = FFT_BINS - 1;
    if (start_bin > end_bin) start_bin = end_bin;

    float peak = data[start_bin];
    uint16_t b;
    for (b = (uint16_t)(start_bin + 1); b <= end_bin; b++)
        if (data[b] > peak) peak = data[b];

    float target = peak + FFT_REF_HEADROOM_DB;

    if (!fft_ref_primed) {
        fft_ref_tracked = target;
        fft_ref_primed = true;
    } else if (target > fft_ref_tracked) {
        fft_ref_tracked = target;                       /* attack: immediate */
    } else if (fft_ref_tracked - target > FFT_REF_DEAD_BAND_DB) {
        fft_ref_tracked += (target - fft_ref_tracked) * FFT_REF_DECAY;
    }

    return fft_ref_tracked + cfg->ref_level_db;
}

/* Left margin of the spectrum for its dB scale, so the numbers never sit on
 * the bars. */
#define FFT_LMARGIN  22u

/* A one-line message (msg) in [y_top, y_top + height) over black: every pixel
 * is composed per column and streamed, never blanked first, so the region
 * does not flash. */
void fft_note_compose(uint16_t y_top, uint16_t height, const char *msg)
{
    const uint16_t y_bot = (uint16_t)(y_top + height);
    ctext_set_t T;
    T.n = 0;
    ctext_add(&T, 4, y_top + 2, msg, COLOR_GRAY, COLOR_BLACK, false);
    for (uint16_t x = 0; x < LCD_WIDTH; x++) {
        lcd_set_window(x, y_top, 1, height);
        for (uint16_t y = y_top; y < y_bot; y++) {
            uint16_t c = COLOR_BLACK;
            (void)ctext_px(&T, x, y, &c);
            lcd_write_data(c);
        }
    }
}

/* CH2's spectrum: cyan at the top like its trace, a darker teal where CH1's
 * bars use cyan, so the bands stay distinct. */
#define FFT_BAR_LOW_CH2  0x0410u

/* The spectrum in [y_top, y_top + height): every pixel composed per column
 * and streamed, never blanked first. Takes the input from fft_prepare_input(),
 * runs fft_process(), then draws dB grid and labels, peak carets with their
 * harmonic tags, the source and peak header, the frequency axis ends, the bars
 * and the max-hold dots. When there is nothing to show it draws a message. */
static void fft_region_compose(uint16_t y_top, uint16_t height)
{
    const fft_config_t *cfg = fft_get_config();
    const uint16_t y_bot = (uint16_t)(y_top + height);
    ctext_set_t T;
    T.n = 0;

    int16_t *sbuf = fft_get_sample_buf();
    uint16_t nsamp = 0;
    float fs = 0.0f;
    fft_source_t sel;
    sel.note[0] = '\0';
    const fft_src_t src = sbuf ? fft_prepare_input(sbuf, &nsamp, &fs, &sel) : FFT_SRC_NONE;
    if (cfg == NULL || src == FFT_SRC_NONE || nsamp == 0) {
        /* An engine that failed its init says so; never a spectrum from it.
         * A channel that is off (or has no record yet) is named. */
        fft_note_compose(y_top, height,
                         (fft_is_initialized() && !fft_engine_ok()) ? "FFT: init failed"
                         : sel.note[0] ? sel.note : "FFT: waiting");
        return;
    }
    const bool live = (src == FFT_SRC_LIVE);
    /* CH2's spectrum in CH2's colour; the demo keeps CH1's. */
    const bool ch2 = live && sel.ch == 1u;
    const uint16_t bar_hot = ch2 ? COLOR_CH2 : COLOR_CH1;
    const uint16_t bar_low = ch2 ? FFT_BAR_LOW_CH2 : COLOR_CYAN;
    fft_process(sbuf, nsamp, &fft_result);
    const float *draw_data = (fft_result.avg_db != NULL) ? fft_result.avg_db
                                                         : fft_result.level_db;
    const uint16_t zoom_start = cfg->zoom_start_bin, zoom_end = cfg->zoom_end_bin;
    uint16_t zoom_span = (uint16_t)(zoom_end - zoom_start);
    if (zoom_span == 0) zoom_span = 1;
    const uint16_t W = (uint16_t)(LCD_WIDTH - FFT_LMARGIN);
    /* The same auto-ranged reference the waterfall uses. The dB scale is
     * relative to one count, not to full scale, so a fixed 0 dB reference
     * would pin every bar to full height. See fft_display_ref(). */
    const float ref_db = fft_display_ref(cfg, draw_data, zoom_start, zoom_end);
    const float range_db = cfg->db_range;

    /* Grid rows every 10 dB, labels every 20 dB in the left margin. */
    uint16_t gy[16];
    uint8_t ng = 0;
    for (float db = ref_db; db > ref_db - range_db && ng < 16u; db -= 10.0f) {
        const uint16_t y = (uint16_t)(y_top + (uint16_t)(((ref_db - db) / range_db) * (float)height));
        if (y > y_top && y < y_bot) gy[ng++] = y;
    }
    if (height > 80) {
        for (float db = ref_db; db > ref_db - range_db; db -= 20.0f) {
            const uint16_t y = (uint16_t)(y_top + (uint16_t)(((ref_db - db) / range_db) * (float)height));
            if (y > y_top + 8 && y < y_bot - 8) {
                char l[8];
                snprintf(l, sizeof l, "%d", (int)db);
                ctext_add(&T, 1, y - 5, l, COLOR_GRAY, COLOR_BLACK, true);
            }
        }
    }

    /* Peaks: a red caret and the harmonic tag (ratios between bins, true
     * whatever the rate). The tag stays under the header line. */
    int16_t pkx[3], pky[3];
    uint8_t np = 0;
    for (uint8_t p = 0; p < fft_result.num_peaks && p < 3; p++) {
        const uint16_t pb = fft_result.peaks[p].bin;
        if (pb < zoom_start || pb > zoom_end) continue;
        const int px = (int)FFT_LMARGIN + (int)((uint32_t)(pb - zoom_start) * W / zoom_span);
        float norm = (ref_db - fft_result.peaks[p].level_db) / range_db;
        if (norm < 0.0f) norm = 0.0f;
        const int py = (int)y_top + (int)(norm * (float)height);
        if (py >= y_top + 4 && px > (int)FFT_LMARGIN + 2 && px < LCD_WIDTH - 2) {
            pkx[np] = (int16_t)px;
            pky[np] = (int16_t)py;
            np++;
        }
        if (fft_result.peaks[p].label[0] != '\0' && px > (int)FFT_LMARGIN + 8 && px < LCD_WIDTH - 30) {
            int tag_y = py - 12;
            const int tag_min = (int)y_top + 2 + (int)font_small.height + 1;
            if (tag_y < tag_min) tag_y = tag_min;
            ctext_add(&T, px - 8, tag_y, fft_result.peaks[p].label, COLOR_ORANGE, COLOR_ORANGE, false);
        }
    }

    /* Source and peak header; the peak in Hz only when the rate is known. */
    {
        char hdr[40], f[16];
        const uint16_t col = live ? COLOR_WHITE : COLOR_ORANGE;
        char tag[12];
        if (live) snprintf(tag, sizeof tag, "LIVE %s", sel.label);
        else      snprintf(tag, sizeof tag, "DEMO sq");
        if (fft_result.num_peaks == 0) {
            snprintf(hdr, sizeof hdr, "%s", live ? tag : "DEMO SIGNAL");
        } else if (fft_live_axis_known(fs)) {
            fft_live_format_hz(fft_result.peaks[0].freq_hz, f, sizeof f);
            snprintf(hdr, sizeof hdr, "%s pk %s", tag, f);
        } else {
            snprintf(hdr, sizeof hdr, "%s bin %u", tag, (unsigned)fft_result.peaks[0].bin);
        }
        ctext_add(&T, 4, y_top + 2, hdr, col, col, false);
        static const char *const wn[] = { "Rect", "Hann", "Hamm", "BHar", "Flat" };
        const char *w = (cfg->window < FFT_WINDOW_COUNT) ? wn[cfg->window] : "?";
        ctext_add(&T, LCD_WIDTH - 4 - (int)font_string_width(w, &font_small), y_top + 2, w,
                  COLOR_GRAY, COLOR_GRAY, false);
    }
    if (height > 80) {
        char l[12];
        fft_live_axis_label(fs, FFT_SIZE, zoom_start, l, sizeof l);
        ctext_add(&T, FFT_LMARGIN + 2, y_bot - 11, l, COLOR_WHITE, COLOR_BLACK, true);
        fft_live_axis_label(fs, FFT_SIZE, zoom_end, l, sizeof l);
        ctext_add(&T, LCD_WIDTH - 2 - (int)font_string_width(l, &font_small), y_bot - 11, l,
                  COLOR_WHITE, COLOR_BLACK, true);
    }

    for (uint16_t x = 0; x < LCD_WIDTH; x++) {
        int bar_top = y_bot, mh_y = -1;
        uint16_t bar_c = COLOR_GRID;
        if (x >= FFT_LMARGIN) {
            uint16_t bin = (uint16_t)(zoom_start + (uint16_t)((uint32_t)(x - FFT_LMARGIN) * zoom_span / W));
            if (bin >= FFT_BINS) bin = FFT_BINS - 1;
            float n = (ref_db - draw_data[bin]) / range_db;
            if (n < 0.0f) n = 0.0f;
            if (n > 1.0f) n = 1.0f;
            bar_top = (int)y_top + (int)(n * (float)height);
            bar_c = (n < 0.25f) ? bar_hot : (n < 0.5f) ? COLOR_GREEN
                  : (n < 0.75f) ? bar_low : COLOR_GRID;
            if (fft_result.max_hold_db != NULL && (x & 1u) == 0u) {
                const float m = (ref_db - fft_result.max_hold_db[bin]) / range_db;
                if (m >= 0.0f && m <= 1.0f) mh_y = (int)y_top + (int)(m * (float)height);
            }
        }
        lcd_set_window(x, y_top, 1, height);
        for (uint16_t y = y_top; y < y_bot; y++) {
            uint16_t c = COLOR_BLACK;
            bool done = ctext_px(&T, x, y, &c);
            for (uint8_t k = 0; !done && k < np; k++) {
                const int dx = (int)x - pkx[k], dy = (int)y - pky[k];
                if ((dy == -3 && dx == 0) || (dy == -2 && dx >= -1 && dx <= 1)) {
                    c = COLOR_RED;
                    done = true;
                }
            }
            if (!done && (int)y == mh_y) { c = COLOR_RED; done = true; }
            if (!done && x >= FFT_LMARGIN && (x & 3u) == 0u)
                for (uint8_t k = 0; k < ng; k++)
                    if (gy[k] == y) { c = COLOR_GRID; done = true; break; }
            if (!done && (int)y >= bar_top) c = bar_c;
            lcd_write_data(c);
        }
    }
}

/* The full-screen spectrum view. full: also paint the 2-row black margins. */
void draw_fft_screen(bool full)
{
    if (full) {                     /* the 2-row margins above and below */
        lcd_fill_rect(0, SCOPE_TOP, LCD_WIDTH, 2, COLOR_BLACK);
        lcd_fill_rect(0, SCOPE_TOP + SCOPE_H - 2, LCD_WIDTH, 2, COLOR_BLACK);
    }
    fft_region_compose(SCOPE_TOP + 2, SCOPE_H - 4);
}

/* Height of one division of the split view's Y-T half, in pixels: half the
 * height of a full-view division, one division still being the range's
 * volts/div. */
#define SPLIT_DIV_PX  13

/* Split view: the Y-T window on top (8 divisions of SPLIT_DIV_PX), the
 * spectrum below. Both composed per column. Needs the live record for the
 * traces ("no record" otherwise). full: also paint the bottom margin row.
 * The frame argument is unused. */
void draw_split_screen(uint32_t frame, bool full)
{
    (void)frame;
    const theme_t *th = theme_get();
    const scope_state_t *ss = scope_state_get();
    const uint16_t top = SCOPE_TOP, divider = SCOPE_TOP + SCOPE_H / 2;
    const uint16_t mid = (uint16_t)(top + 4 * SPLIT_DIV_PX);       /* 8 divisions of 13 px */
    const volatile uint8_t *r1 = fpga_get_ch1_buf(), *r2 = fpga_get_ch2_buf();
    const bool live = fpga_data_ready() && r1 != NULL && r2 != NULL;
    autofit_t m[2];
    bool on[2] = { false, false };
    if (live) {
        view_build(ss, r1, r2);
        for (uint8_t ch = 0; ch < 2u; ch++) {
            const uint8_t r = ch ? ss->ch2.vdiv_idx : ss->ch1.vdiv_idx;
            on[ch] = ch ? ss->ch2.enabled : ss->ch1.enabled;
            if (!on[ch]) continue;
            if (ss->true_scale && scope_cal_true_scale_ok((uint8_t)(ch + 1u), r)) {
                yt_vmap_true(&m[ch], (int16_t)top, (int16_t)divider, (int16_t)mid, (int8_t)ch,
                             yt_centre_code(), scope_cal_codes_per_div((uint8_t)(ch + 1u), r));
                m[ch].h = SPLIT_DIV_PX * 100;           /* half-height divisions */
            } else {
                autofit_prep(&m[ch], yt_cols[ch], (int16_t)top, (int16_t)divider, (int8_t)ch);
            }
        }
    }
    static const char nrec[] = "no record";
    int16_t prev[2] = { 0, 0 };
    for (uint16_t x = 0; x < LCD_WIDTH; x++) {
        const bool inwin = (x < SCOPE_GEOM_COLS);
        const bool tr = live && x >= g_view_first && x < g_view_end;
        int16_t lo[2] = { 1, 1 }, hi[2] = { 0, 0 };
        for (uint8_t ch = 0; ch < 2u; ch++) {
            if (!on[ch] || !tr) continue;
            const int16_t y = autofit_y(&m[ch], yt_cols[ch][x]);
            lo[ch] = y; hi[ch] = y;
            if (x > g_view_first) {
                if (prev[ch] < lo[ch]) lo[ch] = prev[ch];
                if (prev[ch] > hi[ch]) hi[ch] = prev[ch];
            }
            prev[ch] = y;
        }
        lcd_set_window(x, top, 1, (uint16_t)(divider - top + 1));
        for (uint16_t y = top; y <= divider; y++) {
            uint16_t c;
            if (y == divider)
                c = th->grid_center;
            else if (!live && x >= 4u && y >= top + 2u && y < top + 2u + font_small.height &&
                     font_text_px(nrec, &font_small, (int)x - 4, (int)y - (int)top - 2))
                c = COLOR_ORANGE;
            else if (on[1] && (int16_t)y >= lo[1] && (int16_t)y <= hi[1])
                c = th->ch2;
            else if (on[0] && (int16_t)y >= lo[0] && (int16_t)y <= hi[0])
                c = th->ch1;
            else if (inwin && (y == mid || x == SCOPE_GEOM_CENTER_COL))
                c = th->grid_center;
            else if (inwin && ((scope_geom_vgrid((int)x) && (y & 1u) == 0) ||
                               (((y - top) % SPLIT_DIV_PX) == 0 && (x & 1u) == 0)))
                c = th->grid;
            else
                c = th->background;
            lcd_write_data(c);
        }
    }
    if (full)
        lcd_fill_rect(0, SCOPE_TOP + SCOPE_H - 1, LCD_WIDTH, 1, COLOR_BLACK);
    fft_region_compose((uint16_t)(divider + 1), SCOPE_H / 2 - 2);
}

#endif
