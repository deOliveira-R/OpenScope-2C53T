/*
 * OpenScope 2C53T - oscilloscope screen: spectrum and split views
 *
 * The spectrum of a record, its reference level and notes, and the split
 * view (trace above, spectrum below).
 */

#include "scope_ui_parts.h"
#include "canvas.h"
#include "key_model.h"         /* key_focus_get, the view table */

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

/* The dB grid: a row every FFT_GRID_DB, dotted (every 4th column), at most
 * FFT_GRID_ROWS_MAX of them; and at most FFT_CARETS_MAX peak carets. */
#define FFT_GRID_DB        10.0f
#define FFT_GRID_ROWS_MAX  16u
#define FFT_GRID_DOT(x)    (((x) & 3u) == 0u)
#define FFT_CARETS_MAX     3u

/* One frame of the spectrum in rows [y_top, y_bot), built once
 * (fft_frame_build). Without something to show (bars false) it holds only a
 * message in T. */
typedef struct {
    uint16_t     y_top, y_bot, height;
    bool         bars;                      /* a spectrum is drawn */
    fft_axes_t   ax;                        /* what its columns and rows mean (on: bars) */
    cur_layer_t  cl;                        /* the cursor lines on those axes */
    bool         curs;                      /* it has something to draw */
    bool         mark;                      /* the focus mark frames the region */
    uint16_t     mark_c;
    ctext_set_t  T;                         /* every label: dB scale, header, axis ends, peak tags */
    uint16_t     gy[FFT_GRID_ROWS_MAX];     /* rows of the dB grid */
    uint8_t      ng;
    int16_t      pkx[FFT_CARETS_MAX], pky[FFT_CARETS_MAX];   /* peak carets */
    uint8_t      np;
    const float *draw_data;                 /* level per bin, dB */
    bool         ch2;                       /* the spectrum is CH2's */
    fft_meas_t   meas[FFT_MEAS_COUNT];      /* what the strip shows of it; all refused without bars */
    uint16_t     bar_hot, bar_low;
} fft_frame_t;

/* What one column shows. */
typedef struct {
    int      bar_top;                       /* first row of the bar; y_bot: none */
    uint16_t bar_c;
    int      mh_y;                          /* row of the max-hold dot, -1: none */
} fft_col_t;

/* The axes of the spectrum last built, for the keys (scope_ui_fft_axes). */
static fft_axes_t g_fft_axes;

void scope_ui_fft_axes(fft_axes_t *out)
{
    taskENTER_CRITICAL();
    *out = g_fft_axes;
    taskEXIT_CRITICAL();
}

static void fft_axes_publish(const fft_axes_t *ax)
{
    taskENTER_CRITICAL();
    g_fft_axes = *ax;
    taskEXIT_CRITICAL();
}

/* Build the frame: take the input (fft_prepare_input), run fft_process(),
 * then lay out the dB grid and its labels, the peak carets with their
 * harmonic tags, the source and peak header and the frequency axis ends.
 * When there is nothing to show the frame holds a message instead. */
static void fft_frame_build_plot(fft_frame_t *f, uint16_t y_top, uint16_t height);

/* cursors: the frame draws the cursor lines (the full-screen spectrum).
 * mark: it draws the focus mark (the split view's half the keys are on). */
static void fft_frame_build(fft_frame_t *f, uint16_t y_top, uint16_t height, bool cursors, bool mark)
{
    fft_frame_build_plot(f, y_top, height);
    f->mark = mark;
    f->mark_c = theme_get()->text_primary;
    fft_axes_publish(&f->ax);
    f->curs = cursors && f->ax.on && scope_state_get()->cursor.mode != CURSOR_OFF;
    if (f->curs) cursor_layer_build_spectrum(scope_state_get(), &f->ax, theme_get(), &f->cl);
}

static void fft_frame_build_plot(fft_frame_t *f, uint16_t y_top, uint16_t height)
{
    const fft_config_t *cfg = fft_get_config();
    const uint16_t y_bot = (uint16_t)(y_top + height);
    f->y_top = y_top;
    f->y_bot = y_bot;
    f->height = height;
    f->bars = false;
    f->ax.on = false;
    f->T.n = 0;
    f->ng = 0;
    f->np = 0;
    f->ch2 = false;
    fft_live_measure(NULL, 0u, 0u, 0.0f, 0u, f->meas);

    int16_t *sbuf = fft_get_sample_buf();
    uint16_t nsamp = 0;
    float fs = 0.0f;
    fft_source_t sel;
    sel.note[0] = '\0';
    const fft_src_t src = sbuf ? fft_prepare_input(sbuf, &nsamp, &fs, &sel) : FFT_SRC_NONE;
    if (cfg == NULL || src == FFT_SRC_NONE || nsamp == 0) {
        /* An engine that failed its init says so; never a spectrum from it.
         * A channel that is off (or has no record yet) is named. */
        ctext_add(&f->T, 4, y_top + 2,
                  (fft_is_initialized() && !fft_engine_ok()) ? "FFT: init failed"
                  : sel.note[0] ? sel.note : "FFT: waiting", COLOR_GRAY, COLOR_BLACK, false);
        return;
    }
    const bool live = (src == FFT_SRC_LIVE);
    /* CH2's spectrum in CH2's colour; the demo keeps CH1's. */
    const bool ch2 = f->ch2 = live && sel.ch == 1u;
    f->bar_hot = ch2 ? COLOR_CH2 : COLOR_CH1;
    f->bar_low = ch2 ? FFT_BAR_LOW_CH2 : COLOR_CYAN;
    fft_process(sbuf, nsamp, &fft_result);
    const float *draw_data = (fft_result.avg_db != NULL) ? fft_result.avg_db
                                                         : fft_result.level_db;
    fft_live_measure(draw_data, FFT_BINS, fft_result.num_peaks ? fft_result.peaks[0].bin : 0u,
                     fs, FFT_SIZE, f->meas);
    const uint16_t zoom_start = cfg->zoom_start_bin, zoom_end = cfg->zoom_end_bin;
    uint16_t zoom_span = (uint16_t)(zoom_end - zoom_start);
    if (zoom_span == 0) zoom_span = 1;
    const uint16_t W = (uint16_t)(LCD_WIDTH - FFT_LMARGIN);
    f->bars = true;
    f->draw_data = draw_data;
    /* The same auto-ranged reference the waterfall uses. The dB scale is
     * relative to one count, not to full scale, so a fixed 0 dB reference
     * would pin every bar to full height. See fft_display_ref(). */
    const float ref_db = fft_display_ref(cfg, draw_data, zoom_start, zoom_end);
    const float range_db = cfg->db_range;
    f->ax = (fft_axes_t){ .on = true, .x0 = FFT_LMARGIN, .w = W, .bin0 = zoom_start, .bins = zoom_span,
                          .y_top = y_top, .height = height, .ref_db = ref_db, .range_db = range_db,
                          .sample_rate_hz = fs, .fft_size = FFT_SIZE };

    /* Grid rows every 10 dB, labels every 20 dB in the left margin. */
    for (float db = ref_db; db > ref_db - range_db && f->ng < FFT_GRID_ROWS_MAX; db -= FFT_GRID_DB) {
        const uint16_t y = (uint16_t)(y_top + (uint16_t)(((ref_db - db) / range_db) * (float)height));
        if (y > y_top && y < y_bot) f->gy[f->ng++] = y;
    }
    if (height > 80) {
        for (float db = ref_db; db > ref_db - range_db; db -= 20.0f) {
            const uint16_t y = (uint16_t)(y_top + (uint16_t)(((ref_db - db) / range_db) * (float)height));
            if (y > y_top + 8 && y < y_bot - 8) {
                char l[8];
                snprintf(l, sizeof l, "%d", (int)db);
                ctext_add(&f->T, 1, y - 5, l, COLOR_GRAY, COLOR_BLACK, true);
            }
        }
    }

    /* Peaks: a red caret and the harmonic tag (ratios between bins, true
     * whatever the rate). The tag stays under the header line. */
    for (uint8_t p = 0; p < fft_result.num_peaks && p < FFT_CARETS_MAX; p++) {
        const uint16_t pb = fft_result.peaks[p].bin;
        const int px = fft_axes_x(&f->ax, pb);
        if (px < 0) continue;
        float norm = (ref_db - fft_result.peaks[p].level_db) / range_db;
        if (norm < 0.0f) norm = 0.0f;
        const int py = (int)y_top + (int)(norm * (float)height);
        if (py >= y_top + 4 && px > (int)FFT_LMARGIN + 2 && px < LCD_WIDTH - 2) {
            f->pkx[f->np] = (int16_t)px;
            f->pky[f->np] = (int16_t)py;
            f->np++;
        }
        if (fft_result.peaks[p].label[0] != '\0' && px > (int)FFT_LMARGIN + 8 && px < LCD_WIDTH - 30) {
            int tag_y = py - 12;
            const int tag_min = (int)y_top + 2 + (int)font_small.height + 1;
            if (tag_y < tag_min) tag_y = tag_min;
            ctext_add(&f->T, px - 8, tag_y, fft_result.peaks[p].label, COLOR_ORANGE, COLOR_ORANGE, false);
        }
    }

    /* The header: the source on the left (the peak is a measurement, in the
     * strip), the window on the right. */
    {
        char hdr[16];
        const uint16_t col = live ? COLOR_WHITE : COLOR_ORANGE;
        if (live) snprintf(hdr, sizeof hdr, "LIVE %s", sel.label);
        else      snprintf(hdr, sizeof hdr, "DEMO SIGNAL");
        ctext_add(&f->T, 4, y_top + 2, hdr, col, col, false);
        static const char *const wn[] = { "Rect", "Hann", "Hamm", "BHar", "Flat" };
        const char *w = (cfg->window < FFT_WINDOW_COUNT) ? wn[cfg->window] : "?";
        ctext_add(&f->T, LCD_WIDTH - 4 - (int)font_string_width(w, &font_small), y_top + 2, w,
                  COLOR_GRAY, COLOR_GRAY, false);
    }
    if (height > 80) {
        char l[12];
        fft_live_axis_label(fs, FFT_SIZE, zoom_start, l, sizeof l);
        ctext_add(&f->T, FFT_LMARGIN + 2, y_bot - 11, l, COLOR_WHITE, COLOR_BLACK, true);
        fft_live_axis_label(fs, FFT_SIZE, zoom_end, l, sizeof l);
        ctext_add(&f->T, LCD_WIDTH - 2 - (int)font_string_width(l, &font_small), y_bot - 11, l,
                  COLOR_WHITE, COLOR_BLACK, true);
    }

}

/* Work out column x: the bin it shows, its bar and its max-hold dot. */
static void fft_column(const fft_frame_t *f, uint16_t x, fft_col_t *c)
{
    c->bar_top = f->y_bot;
    c->mh_y = -1;
    c->bar_c = COLOR_GRID;
    if (!f->bars || x < FFT_LMARGIN) return;
    uint16_t bin = fft_axes_bin(&f->ax, x);
    if (bin >= FFT_BINS) bin = FFT_BINS - 1;
    float n = fft_axes_depth(&f->ax, f->draw_data[bin]);
    if (n < 0.0f) n = 0.0f;
    if (n > 1.0f) n = 1.0f;
    c->bar_top = (int)f->y_top + (int)(n * (float)f->height);
    c->bar_c = (n < 0.25f) ? f->bar_hot : (n < 0.5f) ? COLOR_GREEN
             : (n < 0.75f) ? f->bar_low : COLOR_GRID;
    if (fft_result.max_hold_db != NULL && (x & 1u) == 0u) {
        const float m = fft_axes_depth(&f->ax, fft_result.max_hold_db[bin]);
        if (m >= 0.0f && m <= 1.0f) c->mh_y = (int)f->y_top + (int)(m * (float)f->height);
    }
}

/* A peak caret covers pixel (dx, dy) from its tip. */
static inline bool fft_caret_px(int dx, int dy)
{
    return (dy == -3 && dx == 0) || (dy == -2 && dx >= -1 && dx <= 1);
}

#if FEATURE_BENCHSHELL
/* `fpga scope livecheck` in the spectrum: every pixel of column x against the
 * per-pixel rule, top layer first: the cursors, labels, peak carets, the
 * max-hold dot, the focus mark, the dB grid, the bar, black. */
static void fft_check_column(const fft_frame_t *f, const fft_col_t *c, uint16_t x, const canvas_strip_t *s)
{
    for (int y = s->lo; y < s->hi; y++) {
        uint16_t want = COLOR_BLACK;
        bool done = f->curs && cursor_layer_px(&f->cl, (int)x, y, &want);
        if (!done) done = ctext_px(&f->T, x, y, &want);
        for (uint8_t k = 0; !done && k < f->np; k++)
            if (fft_caret_px((int)x - f->pkx[k], y - f->pky[k])) { want = COLOR_RED; done = true; }
        if (!done && y == c->mh_y) { want = COLOR_RED; done = true; }
        if (!done && f->mark && focus_mark_px((int)x, y, 0, f->y_top, LCD_WIDTH, f->height)) {
            want = f->mark_c; done = true;
        }
        if (!done && f->bars && x >= FFT_LMARGIN && FFT_GRID_DOT(x))
            for (uint8_t k = 0; k < f->ng; k++)
                if (f->gy[k] == y) { want = COLOR_GRID; done = true; break; }
        if (!done && y >= c->bar_top) want = c->bar_c;
        const uint16_t got = s->px[y - s->lo];
        live_check_pixels++;
        if (want != got && live_check_diffs++ == 0) {
            live_check_first_x = x; live_check_first_y = (uint16_t)y;
            live_check_first_want = want; live_check_first_got = got;
        }
    }
}
#endif

/* Paint column x of the spectrum into s: the canvas's paint function. The
 * elements, lowest first: black, the bar, the dB grid, the focus mark, the
 * max-hold dot, the peak carets, the labels, the cursors. */
static void fft_paint_column(void *ctx, uint16_t x, const canvas_strip_t *s)
{
    const fft_frame_t *const f = ctx;
    fft_col_t c;
    fft_column(f, x, &c);

    for (int i = 0; i < s->hi - s->lo; i++) s->px[i] = COLOR_BLACK;
    strip_span(s, c.bar_top, s->hi - 1, c.bar_c);
    if (f->bars && x >= FFT_LMARGIN && FFT_GRID_DOT(x))
        for (uint8_t k = 0; k < f->ng; k++) strip_put(s, f->gy[k], COLOR_GRID);
    if (f->mark) {
        if (x < FOCUS_MARK_PX || x >= LCD_WIDTH - FOCUS_MARK_PX) {
            strip_span(s, f->y_top, f->y_bot - 1, f->mark_c);
        } else {
            strip_span(s, f->y_top, f->y_top + FOCUS_MARK_PX - 1, f->mark_c);
            strip_span(s, f->y_bot - FOCUS_MARK_PX, f->y_bot - 1, f->mark_c);
        }
    }
    strip_put(s, c.mh_y, COLOR_RED);
    for (int k = (int)f->np - 1; k >= 0; k--)
        for (int dy = -3; dy <= -2; dy++)
            if (fft_caret_px((int)x - f->pkx[k], dy)) strip_put(s, f->pky[k] + dy, COLOR_RED);
    for (int y = s->lo; y < s->hi; y++) {
        uint16_t color;
        if (ctext_px(&f->T, x, y, &color)) s->px[y - s->lo] = color;
    }
    if (f->curs) cursor_layer_paint(&f->cl, x, s);
#if FEATURE_BENCHSHELL
    if (live_check_frames) fft_check_column(f, &c, x, s);
#endif
}

/* The spectrum in [y_top, y_top + height): one canvas frame in column strips,
 * so nothing is blanked and only the columns that changed are sent. all:
 * send every column. hole: step around the popup box while it lives (the
 * full-screen spectrum; the split view puts the box back over each frame). */
/* What the furniture of a spectrum needs of the frame that was drawn. */
typedef struct {
    fft_meas_t meas[FFT_MEAS_COUNT];
    bool       ch2;                         /* the spectrum is CH2's */
    fft_axes_t ax;
} fft_drawn_t;

static void fft_region_compose(uint16_t y_top, uint16_t height, bool all, bool hole, bool cursors,
                               bool mark, fft_drawn_t *drawn)
{
    fft_frame_t f;
    fft_frame_build(&f, y_top, height, cursors, mark);
    if (drawn) {
        memcpy(drawn->meas, f.meas, sizeof f.meas);
        drawn->ch2 = f.ch2;
        drawn->ax = f.ax;
    }

    const canvas_rect_t popup = { POPUP_X, POPUP_Y, POPUP_W, POPUP_H };
    const canvas_frame_t cf = {
        .rect = { 0, y_top, LCD_WIDTH, height },
        .holes = &popup, .nholes = hole ? 1u : 0u,
        .all = all,
        .paint = fft_paint_column, .ctx = &f,
    };
    (void)canvas_draw_columns(&cf);
#if FEATURE_BENCHSHELL
    if (live_check_frames) live_check_frames--;
#endif
}

/* The 2-row margins above and below the full-screen spectrum. */
#define FFT_VIEW_MARGIN  2u

/* The full-screen spectrum view. full: every column, the margins and the
 * popup box; otherwise the columns that changed. The popup box is a hole of
 * the frame, as in Y-T. */
void draw_fft_screen(bool full)
{
    const scope_state_t *ss = scope_state_get();
    /* The spectrum ends at its measurement strip's top, as a trace does. */
    const uint16_t bot = fft_strip_top(ss);
    if (full) {
        lcd_fill_rect(0, SCOPE_TOP, LCD_WIDTH, FFT_VIEW_MARGIN, COLOR_BLACK);
        lcd_fill_rect(0, bot - FFT_VIEW_MARGIN, LCD_WIDTH, FFT_VIEW_MARGIN, COLOR_BLACK);
    }
    const bool hole = popup_live();
    popup_hole_last = hole;
    fft_drawn_t d;
    fft_region_compose(SCOPE_TOP + FFT_VIEW_MARGIN, (uint16_t)(bot - SCOPE_TOP - 2u * FFT_VIEW_MARGIN),
                       full, hole, true, false, &d);
    draw_fft_strip(ss, theme_get(), d.meas, d.ch2, !full);
    if (ss->cursor.mode != CURSOR_OFF) spectrum_cursor_row_paint(ss, &d.ax, bot, full);
    if (full) draw_popup(theme_get());
}

/* Height of one division of the split view's Y-T half, in pixels: half the
 * height of a full-view division, one division still being the range's
 * volts/div. */
#define SPLIT_DIV_PX  13

/* The split view's upper half, as a frame of its canvas: the Y-T window (8
 * divisions of SPLIT_DIV_PX), the divider row under it, the focus mark when
 * the keys are on this half. */
typedef struct {
    const theme_t *th;
    uint16_t top, divider, mid;
    bool     live, time_focus;
    bool     on[2];
    autofit_t m[2];
    int16_t  prev[2];           /* the last column's row, per channel: columns come in order */
} split_time_frame_t;

static const char split_nrec[] = "no record";

/* Pixel (x, y) of the upper half, given column x's trace spans lo..hi. */
static uint16_t split_time_px(const split_time_frame_t *f, uint16_t x, uint16_t y,
                              const int16_t lo[2], const int16_t hi[2])
{
    const theme_t *th = f->th;
    const bool inwin = (x < SCOPE_GEOM_COLS);
    if (y == f->divider)
        return th->grid_center;
    if (!f->live && x >= 4u && y >= f->top + 2u && y < f->top + 2u + font_small.height &&
        font_text_px(split_nrec, &font_small, (int)x - 4, (int)y - (int)f->top - 2))
        return COLOR_ORANGE;
    if (f->on[1] && (int16_t)y >= lo[1] && (int16_t)y <= hi[1])
        return th->ch2;
    if (f->on[0] && (int16_t)y >= lo[0] && (int16_t)y <= hi[0])
        return th->ch1;
    if (f->time_focus && focus_mark_px((int)x, (int)y, 0, f->top, LCD_WIDTH, f->divider - f->top))
        return th->text_primary;
    if (inwin && (y == f->mid || x == SCOPE_GEOM_CENTER_COL))
        return th->grid_center;
    if (inwin && ((scope_geom_vgrid((int)x) && (y & 1u) == 0) ||
                  (((y - f->top) % SPLIT_DIV_PX) == 0 && (x & 1u) == 0)))
        return th->grid;
    return th->background;
}

/* The canvas's paint function for the upper half. */
static void split_time_paint(void *ctx, uint16_t x, const canvas_strip_t *s)
{
    split_time_frame_t *const f = ctx;
    const bool tr = f->live && x >= g_view_first && x < g_view_end;
    int16_t lo[2] = { 1, 1 }, hi[2] = { 0, 0 };
    for (uint8_t ch = 0; ch < 2u; ch++) {
        if (!f->on[ch] || !tr) continue;
        const int16_t y = autofit_y(&f->m[ch], yt_cols[ch][x]);
        lo[ch] = y; hi[ch] = y;
        if (x > g_view_first) {
            if (f->prev[ch] < lo[ch]) lo[ch] = f->prev[ch];
            if (f->prev[ch] > hi[ch]) hi[ch] = f->prev[ch];
        }
        f->prev[ch] = y;
    }
    for (int y = s->lo; y < s->hi; y++)
        s->px[y - s->lo] = split_time_px(f, x, (uint16_t)y, lo, hi);
}

/* Split view: the Y-T window on top, the spectrum below, each its own canvas,
 * so each sends only the columns that changed; the half the keys are on in a
 * frame (the focus mark, under the traces). The popup box is a hole of both.
 * Needs the live record for the traces ("no record" otherwise). full: every
 * column, the bottom margin row and the popup box. The frame argument is
 * unused. */
void draw_split_screen(uint32_t frame, bool full)
{
    (void)frame;
    const scope_state_t *ss = scope_state_get();
    split_time_frame_t f = {
        .th = theme_get(),
        .top = SCOPE_TOP, .divider = SCOPE_TOP + SCOPE_H / 2,
        .mid = (uint16_t)(SCOPE_TOP + 4 * SPLIT_DIV_PX),        /* 8 divisions of 13 px */
    };
    const volatile uint8_t *r1 = fpga_get_ch1_buf(), *r2 = fpga_get_ch2_buf();
    f.live = fpga_data_ready() && r1 != NULL && r2 != NULL;
    /* The half the keys and AUTO act on wears the focus mark. */
    f.time_focus = view_key_domain(VIEW_MODE_SPLIT, (key_focus_t)key_focus_get()) == VIEW_DOM_TIME;
    if (f.live) {
        view_build(ss, r1, r2);
        for (uint8_t ch = 0; ch < 2u; ch++) {
            const uint8_t r = ch ? ss->ch2.vdiv_idx : ss->ch1.vdiv_idx;
            f.on[ch] = ch ? ss->ch2.enabled : ss->ch1.enabled;
            if (!f.on[ch]) continue;
            if (ss->true_scale && scope_cal_true_scale_ok((uint8_t)(ch + 1u), r)) {
                yt_vmap_true(&f.m[ch], (int16_t)f.top, (int16_t)f.divider, (int16_t)f.mid, (int8_t)ch,
                             yt_centre_code(), scope_cal_codes_per_div((uint8_t)(ch + 1u), r));
                f.m[ch].h = SPLIT_DIV_PX * 100;         /* half-height divisions */
            } else {
                autofit_prep(&f.m[ch], yt_cols[ch], (int16_t)f.top, (int16_t)f.divider, (int8_t)ch);
            }
        }
    }
    const bool hole = popup_live();
    popup_hole_last = hole;
    const canvas_rect_t popup = { POPUP_X, POPUP_Y, POPUP_W, POPUP_H };
    const canvas_frame_t cf = {
        .id = CANVAS_SPLIT_TIME,
        .rect = { 0, f.top, LCD_WIDTH, (uint16_t)(f.divider - f.top + 1u) },
        .holes = &popup, .nholes = hole ? 1u : 0u,
        .all = full,
        .paint = split_time_paint, .ctx = &f,
    };
    (void)canvas_draw_columns(&cf);
    if (full)
        lcd_fill_rect(0, SCOPE_TOP + SCOPE_H - 1, LCD_WIDTH, 1, COLOR_BLACK);
    fft_region_compose((uint16_t)(f.divider + 1), SCOPE_H / 2 - 2, full, hole, false, !f.time_focus, NULL);
    if (full) draw_popup(f.th);
}
