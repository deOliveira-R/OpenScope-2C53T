/*
 * OpenScope 2C53T - oscilloscope screen: the spectrum
 *
 * The spectrum of a record, its reference level and notes: the FFT view,
 * and the spectrum as a half of split (scope_split.c), both through
 * fft_pane_draw.
 */

#include "scope_ui_parts.h"
#include "canvas.h"
#include "view_table.h"         /* the focus mark */

/* Text laid out in column order: ctext_add records a string with its
 * position and colours (used by the spectrum and waterfall views), and
 * ctext_px answers which pixel a streamed column takes from it. Strings that
 * are empty or that do not fit in the set (CTEXT_MAX, CTEXT_LEN) are dropped.
 * opaque: the glyph box is filled with bg, else only the glyph pixels count. */
void ctext_add(ctext_set_t *T, int x, int y, const char *s,
                      uint16_t fg, uint16_t bg, bool opaque)
{
    if (T->n >= CTEXT_MAX || s == NULL || s[0] == '\0') return;
    typeof(T->t[0]) *e = &T->t[T->n];
    strncpy(e->s, s, CTEXT_LEN - 1);
    e->s[CTEXT_LEN - 1] = '\0';
    T->box[T->n] = (label_box_t){ (int16_t)x, (int16_t)y,
                                  (int16_t)font_string_width(e->s, &font_small), (int16_t)font_small.height };
    e->fg = fg;
    e->bg = bg;
    e->opaque = opaque;
    T->n++;
}

/* The label goes in only where its box is free (label_place.h): the labels
 * added before it are the ones it gives way to. */
bool ctext_place(ctext_set_t *T, const label_box_t *area, int x, int y, const char *s,
                 uint16_t fg, uint16_t bg, bool opaque)
{
    const uint8_t n = T->n;
    ctext_add(T, x, y, s, fg, bg, opaque);
    if (T->n == n) return false;                         /* empty, or the set is full */
    if (label_place(area, &T->box[n], 1u, T->box, n) == 0) return true;
    T->n = n;                                            /* its box is taken: left out */
    return false;
}

/* The colour of the text pixel at (x, y), written to *c; false when no string
 * covers that pixel (the caller then draws its own background). */
bool ctext_px(const ctext_set_t *T, int x, int y, uint16_t *c)
{
    for (uint8_t k = 0; k < T->n; k++) {
        const int dx = x - T->box[k].x, dy = y - T->box[k].y;
        if (dx < 0 || dx >= T->box[k].w || dy < 0 || dy >= T->box[k].h) continue;
        if (font_text_px(T->t[k].s, &font_small, dx, dy)) { *c = T->t[k].fg; return true; }
        if (T->t[k].opaque) { *c = T->t[k].bg; return true; }
    }
    return false;
}

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

/* Where the spectrum's labels sit (fft_frame_build_plot). The header row:
 * FFT_HEADER_DOWN rows under the pane's top, FFT_TEXT_INSET columns in from
 * either edge, FFT_HEADER_GAP clear rows under it before a peak tag. The dB
 * scale: a number every FFT_DB_LABEL_STEP dB at column FFT_DB_LABEL_X, its
 * top FFT_DB_LABEL_UP rows over its grid row (the 12-row font's middle). The
 * axis ends: FFT_AXIS_LABEL_UP rows over the plot's bottom (the font and one
 * clear row under it), FFT_AXIS_LABEL_INSET columns in from the bars' ends.
 * A peak tag starts FFT_TAG_LEFT columns left of its caret, so a two-letter
 * tag (12 columns) sits over it. */
#define FFT_HEADER_DOWN       2u
#define FFT_TEXT_INSET        4
#define FFT_HEADER_GAP        1u
#define FFT_DB_LABEL_STEP     20.0f
#define FFT_DB_LABEL_X        1
#define FFT_DB_LABEL_UP       5u
#define FFT_AXIS_LABEL_UP     11u
#define FFT_AXIS_LABEL_INSET  2
#define FFT_TAG_LEFT          8u

/* The first row under the header of a spectrum whose pane starts at y_top:
 * the highest a peak tag goes, and the cursor labels' clear row. */
static int16_t fft_under_header(uint16_t y_top)
{
    return (int16_t)(y_top + FFT_HEADER_DOWN + font_small.height + FFT_HEADER_GAP);
}

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

/* The scale (the dB labels and the axis ends) is drawn on a plot of at least
 * FFT_SCALE_LINES font_small lines: the header, two dB labels (a scale is
 * two numbers) and the axis ends. A shorter plot (a half of split over a
 * tall strip) shows the bars and the grid alone. */
#define FFT_SCALE_LINES  4u

static bool fft_scale_fits(uint16_t height)
{
    return height >= FFT_SCALE_LINES * font_small.height;
}

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

/* mark: it draws the focus mark (split's half the keys are on). */
static void fft_frame_build(fft_frame_t *f, uint16_t y_top, uint16_t height, bool mark)
{
    fft_frame_build_plot(f, y_top, height);
    f->mark = mark;
    f->mark_c = theme_get()->text_primary;
    fft_axes_publish(&f->ax);
    f->curs = f->ax.on && scope_state_get()->cursor.mode != CURSOR_OFF;
    if (f->curs) cursor_layer_build_spectrum(scope_state_get(), &f->ax, &f->T, fft_under_header(y_top),
                                             theme_get(), &f->cl);
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
        ctext_add(&f->T, FFT_TEXT_INSET, y_top + FFT_HEADER_DOWN,
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
    /* The labels, the most important first; each later one goes in only
     * where it meets none placed before it (ctext_place), and the cursor
     * labels give way to all of them (cursor_layer_build_spectrum).
     * The header: the source on the left (the peak is a measurement, in the
     * strip), the window on the right. */
    {
        char hdr[16];
        const uint16_t col = live ? COLOR_WHITE : COLOR_ORANGE;
        if (live) snprintf(hdr, sizeof hdr, "LIVE %s", sel.label);
        else      snprintf(hdr, sizeof hdr, "DEMO SIGNAL");
        ctext_add(&f->T, FFT_TEXT_INSET, y_top + FFT_HEADER_DOWN, hdr, col, col, false);
        static const char *const wn[] = { "Rect", "Hann", "Hamm", "BHar", "Flat" };
        const char *w = (cfg->window < FFT_WINDOW_COUNT) ? wn[cfg->window] : "?";
        ctext_add(&f->T, LCD_WIDTH - FFT_TEXT_INSET - (int)font_string_width(w, &font_small),
                  y_top + FFT_HEADER_DOWN, w, COLOR_GRAY, COLOR_GRAY, false);
    }
    const label_box_t pane = { 0, (int16_t)y_top, (int16_t)LCD_WIDTH, (int16_t)height };
    if (fft_scale_fits(height)) {
        /* The axis ends, then the dB scale in the left margin. */
        char l[12];
        const int axis_y = (int)y_bot - (int)FFT_AXIS_LABEL_UP;
        fft_live_axis_label(fs, FFT_SIZE, zoom_start, l, sizeof l);
        ctext_add(&f->T, FFT_LMARGIN + FFT_AXIS_LABEL_INSET, axis_y, l, COLOR_WHITE, COLOR_BLACK, true);
        fft_live_axis_label(fs, FFT_SIZE, zoom_end, l, sizeof l);
        ctext_add(&f->T, LCD_WIDTH - FFT_AXIS_LABEL_INSET - (int)font_string_width(l, &font_small), axis_y, l,
                  COLOR_WHITE, COLOR_BLACK, true);
        for (float db = ref_db; db > ref_db - range_db; db -= FFT_DB_LABEL_STEP) {
            const int y = (int)y_top + (int)(((ref_db - db) / range_db) * (float)height);
            char d[8];
            snprintf(d, sizeof d, "%d", (int)db);
            (void)ctext_place(&f->T, &pane, FFT_DB_LABEL_X, y - (int)FFT_DB_LABEL_UP, d,
                              COLOR_GRAY, COLOR_BLACK, true);
        }
    }

    /* Peaks, strongest first: a red caret and the harmonic tag (ratios
     * between bins, true whatever the rate) over the caret, under the header
     * line, on the plot right of the dB scale. */
    const label_box_t plot = { (int16_t)FFT_LMARGIN, (int16_t)y_top, (int16_t)W, (int16_t)height };
    const int tag_min = fft_under_header(y_top);
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
        int tag_y = py - (int)font_small.height;
        if (tag_y < tag_min) tag_y = tag_min;
        (void)ctext_place(&f->T, &plot, px - (int)FFT_TAG_LEFT, tag_y, fft_result.peaks[p].label,
                          COLOR_ORANGE, COLOR_ORANGE, false);
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

/* The spectrum in pane p (its rows down to band_bot): one canvas frame in
 * column strips on the pane's canvas, so nothing is blanked and only the
 * columns that changed are sent; the focus mark when the pane is marked, the
 * cursor lines when the cursors are on. all: send every column. The popup
 * box is a hole of the frame while it has rows in the pane; returns true
 * when it kept one. drawn: what the spectrum's furniture needs. */
bool fft_pane_draw(const plot_pane_t *p, bool all, fft_drawn_t *drawn)
{
    const uint16_t height = (uint16_t)(p->band_bot - p->top);
    fft_frame_t f;
    fft_frame_build(&f, (uint16_t)p->top, height, p->marked);
    memcpy(drawn->meas, f.meas, sizeof f.meas);
    drawn->ch2 = f.ch2;
    drawn->ax = f.ax;

    const bool hole = popup_hole_in(p->top, p->band_bot);
    const canvas_rect_t popup = { POPUP_X, POPUP_Y, POPUP_W, POPUP_H };
    const canvas_frame_t cf = {
        .id = p->canvas,
        .rect = { 0, (uint16_t)p->top, LCD_WIDTH, height },
        .holes = &popup, .nholes = hole ? 1u : 0u,
        .all = all,
        .paint = fft_paint_column, .ctx = &f,
    };
    (void)canvas_draw_columns(&cf);
    return hole;
}

/* The spectrum's furniture under band_bot: its measurement strip and, with
 * the cursors on, their readout in the strip's AUX row. refresh: in place. */
void fft_furniture_draw(const scope_state_t *ss, const fft_drawn_t *d, uint16_t band_bot, bool refresh)
{
    draw_fft_strip(ss, theme_get(), d->meas, d->ch2, refresh);
    if (ss->cursor.mode != CURSOR_OFF) spectrum_cursor_row_paint(ss, &d->ax, band_bot, !refresh);
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
    const int16_t top = (int16_t)(SCOPE_TOP + FFT_VIEW_MARGIN), end = (int16_t)(bot - FFT_VIEW_MARGIN);
    const plot_pane_t pane = plot_pane_make(CANVAS_PLOT, top, end, end);
    fft_drawn_t d;
    popup_hole_last = fft_pane_draw(&pane, full, &d);
    fft_furniture_draw(ss, &d, bot, !full);
    if (full) draw_popup(theme_get());
}
