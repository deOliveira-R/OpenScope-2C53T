/*
 * OpenScope 2C53T - oscilloscope screen: Y-T elements
 *
 * The elements of the voltage-against-time plot: grid, trigger line,
 * markers, mask overlay, traces, persistence, math, and the demo waveform.
 */

#include "scope_ui_parts.h"

/* ═══════════════════════════════════════════════════════════════════
 * Grid
 * ═══════════════════════════════════════════════════════════════════ */

void draw_scope_grid(void)
{
    const theme_t *th = theme_get();
    uint16_t x, y;

    /* Stock's graticule (parity C5, scope_geom.h): 12 divisions of 25 px
     * over window columns 0..300, centre at column 150. Columns 301..319
     * stay clear for the trigger-level arrow and badge. */
    /* Vertical grid lines (dotted) */
    for (x = 0; x < SCOPE_GEOM_COLS; x += SCOPE_GEOM_DIV_PX) {
        for (y = SCOPE_TOP; y < SCOPE_BOT; y += 2)
            lcd_set_pixel(x, y, th->grid);
    }
    /* Horizontal grid lines (dotted) */
    for (y = SCOPE_TOP; y < SCOPE_BOT; y += 26) {
        for (x = 0; x < SCOPE_GEOM_COLS; x += 2)
            lcd_set_pixel(x, y, th->grid);
    }

    /* Center crosshair (solid) */
    for (x = 0; x < SCOPE_GEOM_COLS; x++)
        lcd_set_pixel(x, SCOPE_GRID_MID_Y, th->grid_center);
    for (y = SCOPE_TOP; y < SCOPE_BOT; y++)
        lcd_set_pixel(SCOPE_GEOM_CENTER_COL, y, th->grid_center);
}

/* ═══════════════════════════════════════════════════════════════════
 * Trigger level line
 * ═══════════════════════════════════════════════════════════════════ */

/* The dotted line across the plot at the trigger level; denser while UP/DOWN
 * are adjusting it, so the user can see which control the arrows own. The
 * tags at the plot edges are drawn by draw_plot_markers() below. */
void draw_trigger_line(const theme_t *th, int16_t trig_y)
{
    if (trig_y < SCOPE_TOP + 2) trig_y = SCOPE_TOP + 2;
    if (trig_y > SCOPE_BOT - 3) trig_y = SCOPE_BOT - 3;
    for (uint16_t x = 0; x < LCD_WIDTH - 6; x += (scope_trig_level_focus ? 2u : 4u))
        lcd_set_pixel(x, (uint16_t)trig_y, th->trigger);
}

vband_t g_vband[2];

/* The math trace's own scale, as drawn this frame (draw_math_waveform): the
 * cursors read it instead of the channel bands while the overlay is up, since
 * it spans the whole glass over them. */
g_math_cur_t g_math_cur;

void vband_clear(void)
{
    g_vband[0].on = false;
    g_vband[1].on = false;
    g_math_cur.on = false;
}

/* channel 1/2, a: the map the band was drawn with. True scale anchors the
 * channel's 0 V code to a row, so absolute Y is known (mid_counts = 0 at the
 * ground tag's row); autofit re-anchors per frame and leaves it unknown. */
void vband_note(uint8_t slot, uint8_t channel, uint8_t range_idx,
                       const autofit_t *a, float counts_per_pixel)
{
    if (slot > 1u) return;
    vband_t *b = &g_vband[slot];
    b->on               = (counts_per_pixel > 0.0f);
    b->y_top            = a->y_top;
    b->y_bot            = a->y_bot;
    b->counts_per_pixel = counts_per_pixel;
    b->channel          = channel;
    b->range_idx        = range_idx;
    b->mid_known        = b->on && a->fx_ch >= 0;
    b->mid_row          = b->mid_known
        ? (int16_t)autofit_y_raw(a, rec_zero(scope_state_get(), (uint8_t)(channel - 1u)))
        : 0;
    b->mid_counts       = 0.0f;
}

/*
 * Where the HARDWARE trigger fires, on the glass.
 *
 * The FPGA compares the ADC against reg 0x08 but the record carries ADC - 28,
 * so the crossing that fires sits at (code - 28) in record units: measured on
 * ranges 5 and 7 at six levels (EXP-55/56, median 28) and the same constant
 * as stock's ADC offset (ADC_REC_OFFSET). That record code is mapped through the SAME
 * map the trigger-source trace is drawn with, so the line sits on the
 * trace where NORMAL would fire. Outside the band it clamps to the band edge
 * (autofit_y's clamp), which on screen reads as "level is above / below the
 * signal" -- in autofit exactly the case where NORMAL holds.
 *
 * The code is the one fpga.c records as written (fpga_acq_trig_code_get),
 * not recomputed from the UI level: report the wire, not the intent.
 *
 * In true scale the same code goes through the fixed map (yt_vmap.h), so
 * the line sits at the trigger level's place on the graticule. A band with
 * no data falls back to the old pixel convention.
 */
uint16_t scope_soft_trigger_offset(const scope_state_t *ss,
                                          const volatile uint8_t *src_buf, uint8_t L,
                                          int16_t *back_q8);

void view_build(const scope_state_t *ss, const volatile uint8_t *b1,
                       const volatile uint8_t *b2);

void yt_fits(const scope_state_t *ss, autofit_t *a1, autofit_t *a2);

int16_t trigger_marker_y(const scope_state_t *ss, const autofit_t *a)
{
    if (a == NULL || !a->on)
        return (int16_t)(SCOPE_MID_Y - ss->trigger.level);
    return autofit_y(a, trig_level_rec((adc_raw_t){ fpga_acq_trig_code_get() }).v);
}

/* Full-path variant: the trigger source's map exactly as
 * draw_demo_waveform() will build it (same window, same yt_fits). */
int16_t trigger_marker_y_full(const scope_state_t *ss)
{
    const volatile uint8_t *b1 = fpga_get_ch1_buf();
    const volatile uint8_t *b2 = fpga_get_ch2_buf();
    if (!fpga_data_ready() || b1 == NULL || b2 == NULL)
        return trigger_marker_y(ss, NULL);
    view_build(ss, b1, b2);
    autofit_t a1, a2;
    yt_fits(ss, &a1, &a2);
    return trigger_marker_y(ss, (ss->trigger.source == TRIG_SRC_CH2) ? &a2 : &a1);
}

/* The two Y-T maps, the only place that decides them (from the window
 * view_build() last filled into yt_cols). True scale where the range has a
 * real volts/div, over the full plot; autofit elsewhere (a NONE range, or
 * view.truescale off), split into halves when both channels autofit. */
void yt_fits(const scope_state_t *ss, autofit_t *a1, autofit_t *a2)
{
    autofit_t *a[2] = { a1, a2 };
    /* A channel is drawn only when the published record holds its own
     * samples (fpga_acq_record_has_ch, dsp/acq_cfg.h): right after CH2 is
     * switched back on, the record on screen was captured with CH1 alone and
     * its CH2 buffer is the second converter's copy of CH1 (bench
     * 2026-11-14). The band appears with the first record captured with it. */
    const bool en[2] = { ss->ch1.enabled && fpga_acq_record_has_ch(0u),
                         ss->ch2.enabled && fpga_acq_record_has_ch(1u) };
    const bool fx[2] = {
        en[0] && ss->true_scale && scope_cal_true_scale_ok(1u, ss->ch1.vdiv_idx),
        en[1] && ss->true_scale && scope_cal_true_scale_ok(2u, ss->ch2.vdiv_idx) };
    const bool split = yt_vmap_split(en[0], en[1], fx[0], fx[1]);
    /* While running, keep the ranges of the record on screen latched, so a
     * stop by any route (key, view.run, shell) rescales from the right one. */
    if (ss->running)
        scope_ui_paused_latch();
    for (uint8_t ch = 0; ch < 2u; ch++) {
        *a[ch] = (autofit_t){ .fx_ch = -1, .span = YT_VMAP_MIN_SPAN };
        if (fx[ch]) {
            const uint8_t r = ch ? ss->ch2.vdiv_idx : ss->ch1.vdiv_idx;
            yt_vmap_true(a[ch], SCOPE_TOP, SCOPE_BOT, SCOPE_GRID_MID_Y, (int8_t)ch,
                         yt_centre_code(), scope_cal_codes_per_div((uint8_t)(ch + 1u), r));
        } else if (en[ch]) {
            int16_t t, b;
            yt_vmap_band(SCOPE_TOP, SCOPE_BOT, split, ch, &t, &b);
            autofit_prep(a[ch], yt_cols[ch], t, b, (int8_t)ch);
        }
    }
}

static void tag_put(pm_tag_t *t, int row, int top, int bot, pm_dir_t horiz,
                    int x0, pm_label_t label, uint16_t color)
{
    t->on = (bot - top) >= PM_TAG_H;
    if (!t->on)
        return;
    int y0;
    t->dir   = plot_marker_place(row, top, bot, horiz, &y0);
    t->label = (t->dir == horiz) ? label : PM_LABEL_NONE;
    t->x0    = (int16_t)x0;
    t->y0    = (int16_t)y0;
    t->color = color;
}

/* a1/a2: the maps as drawn (NULL = no record: the demo/no-data convention,
 * position and level in pixels about the centre). band_bot: the strip's top. */
void markers_build(const scope_state_t *ss, const theme_t *th,
                          const autofit_t *a1, const autofit_t *a2,
                          int band_bot, pm_set_t *m)
{
    memset(m, 0, sizeof(*m));
    const uint8_t act = active_channel ? 1u : 0u;
    for (uint8_t ch = 0; ch < 2u; ch++) {
        const channel_state_t *c = ch ? &ss->ch2 : &ss->ch1;
        pm_tag_t *t = &m->t[(ch == act) ? 1 : 2];
        if (!c->enabled)
            continue;
        const autofit_t *a = ch ? a2 : a1;
        int top = SCOPE_TOP, bot = band_bot, row;
        if (a != NULL && a->on) {
            top = a->y_top;
            if (a->y_bot < bot) bot = a->y_bot;
            row = autofit_y_raw(a, rec_zero(ss, ch));
        } else {
            row = SCOPE_MID_Y - c->position;
        }
        tag_put(t, row, top, bot, PM_POINT_RIGHT, 0,
                ch ? PM_LABEL_2 : PM_LABEL_1, ch ? th->ch2 : th->ch1);
    }

    /* Trigger level: the record code where the hardware fires
     * (trigger_marker_y), on the trigger source's band. */
    const autofit_t *src = (ss->trigger.source == TRIG_SRC_CH2) ? a2 : a1;
    int top = SCOPE_TOP, bot = band_bot, row;
    if (src != NULL && src->on) {
        row = autofit_y_raw(src, (int)trig_level_rec((adc_raw_t){ fpga_acq_trig_code_get() }).v);
        top = src->y_top;
        if (src->y_bot < bot) bot = src->y_bot;
    } else {
        row = SCOPE_MID_Y - ss->trigger.level;
    }
    tag_put(&m->t[0], row, top, bot, PM_POINT_LEFT, LCD_WIDTH - PM_TAG_W,
            PM_LABEL_T, th->trigger);
}

/* Pixel colour of the marker set at (x, y), or false: tags in priority order. */
bool markers_px(const pm_set_t *m, int x, int y, uint16_t bg,
                              uint16_t *c)
{
    for (int i = 0; i < 3; i++) {
        const pm_tag_t *t = &m->t[i];
        if (!t->on || x < t->x0 || x >= t->x0 + PM_TAG_W ||
            y < t->y0 || y >= t->y0 + PM_TAG_H)
            continue;
        const pm_px_t p = plot_marker_px(t->dir, t->label, x - t->x0, y - t->y0);
        if (p == PM_PX_NONE)
            continue;
        *c = (p == PM_PX_KNOCK) ? bg : t->color;
        return true;
    }
    return false;
}

/* Full path: paint the set (lowest priority first, so the order matches
 * markers_px). */
void draw_plot_markers(const pm_set_t *m, const theme_t *th)
{
    for (int i = 2; i >= 0; i--) {
        const pm_tag_t *t = &m->t[i];
        if (!t->on)
            continue;
        for (int dy = 0; dy < PM_TAG_H; dy++)
            for (int dx = 0; dx < PM_TAG_W; dx++) {
                const pm_px_t p = plot_marker_px(t->dir, t->label, dx, dy);
                if (p != PM_PX_NONE)
                    lcd_set_pixel((uint16_t)(t->x0 + dx), (uint16_t)(t->y0 + dy),
                                  (p == PM_PX_KNOCK) ? th->background : t->color);
            }
    }
}

static bool     g_mask_ov_on;

bool     g_mask_bounds_on;

static uint32_t g_mask_ov_gen;

void mask_overlay_begin(void)
{
    /* One column per sample only (R == 1000): the mask indexes record
     * SAMPLES by column offset from the anchor (scope_mask_column), which
     * holds only then. A zoomed fast code (C5) or a re-sampled held record
     * (C14) draws the trace without the overlay rather than colouring columns
     * off the samples that were judged. */
    g_mask_ov_on = (g_trig_anchor == 2u) && g_trig_x_actual >= 0 &&
                   g_view.L == 1u &&
                   scope_mask_state() != MASK_PF_EMPTY;
    /* The bounds depend on the alignment, not on which record was judged,
     * so they are not gated on the generation: gating them would blink them
     * off for the frame between a commit and its verdict. */
    g_mask_bounds_on = g_mask_ov_on && scope_mask_state() == MASK_PF_READY;
    g_mask_ov_gen = fpga_acq_frame_generation();
}

bool mask_px_fail(int8_t ch, int x)
{
    return g_mask_ov_on && ch >= 0 &&
           scope_mask_column((uint8_t)ch, x, g_trig_x_actual, g_mask_ov_gen) == SCOPE_MASK_COL_FAIL;
}

/* Strip colour for column x, or 0 for none; *dotted = paint every other px. */
uint16_t mask_strip_color(int x, const theme_t *th, bool *dotted)
{
    *dotted = false;
    if (!g_mask_ov_on) return 0;
    scope_mask_col_t a = scope_mask_column(0, x, g_trig_x_actual, g_mask_ov_gen);
    scope_mask_col_t b = scope_mask_column(1, x, g_trig_x_actual, g_mask_ov_gen);
    scope_mask_col_t k = (a > b) ? a : b;        /* enum order = severity */
    switch (k) {
    case SCOPE_MASK_COL_PASS:       return th->success;
    case SCOPE_MASK_COL_FAIL:       return th->warning;
    case SCOPE_MASK_COL_FAIL_FRAME: *dotted = true; return th->warning;
    case SCOPE_MASK_COL_SKIP:       *dotted = true; return th->highlight;
    case SCOPE_MASK_COL_TEACH:      return th->highlight;
    default:                        return 0;
    }
}

/* Full-path strip (the compositor paints the same rows per pixel). */
void draw_mask_strip(const theme_t *th, uint16_t band_bot)
{
    if (!g_mask_ov_on) return;
    for (int x = 0; x < LCD_WIDTH; x++) {
        bool dotted;
        uint16_t c = mask_strip_color(x, th, &dotted);
        if (!c) continue;
        for (uint16_t y = band_bot - MASK_STRIP_H; y < band_bot; y++)
            if (!dotted || ((x + y) & 1u) == 0u)
                lcd_set_pixel((uint16_t)x, y, c);
    }
}

/* Persistence (C12) is fed from the channel renderers as they draw each
 * column, so a ghost is exactly where the trace was, through whichever
 * map (true scale or autofit) drew it, and costs no per-column storage (RAM
 * is tight). prev_y < 0: first column. */

bool     g_math_live;    /* a real record was drawn this frame (yt_cols valid) */

void persist_note(uint16_t x, int16_t prev_y, int16_t y)
{
    if (persist_enabled && persist_is_initialized())
        persist_add_column(x, prev_y < 0 ? -1 : (int)prev_y - SCOPE_TOP,
                           (int)y - SCOPE_TOP);
}

/* Draw one channel through its map (yt_fits). Returns the ADC counts one
 * screen row is worth, for the cursor readout.
 *
 * True scale is the graticule-honesty path (M3), in stock's geometry: 0 V
 * at position 0 on the centre line, one 26 px division = the range's nominal
 * volts/div (scope_cal_codes_per_div codes). An offset shifts the trace and
 * may clip it at the plot edge, as on any scope at a fixed volts/div. */
static float draw_channel(const volatile uint8_t *buf, uint16_t color,
                          const autofit_t *a, int8_t mask_ch)
{
    const uint16_t fail_c = theme_get()->warning;
    const uint16_t bnd_c  = theme_get()->text_secondary;
    int16_t prev_y = -1;
    for (uint16_t x = g_view_first; x < g_view_end; x++) {   /* blank columns stay blank */
        int16_t y = autofit_y(a, buf[x]);
        persist_note(x, prev_y, y);
        uint16_t cc = mask_px_fail(mask_ch, x) ? fail_c : color;
        uint8_t blo, bhi;
        if (g_mask_bounds_on && mask_ch >= 0 && MASK_BOUND_DOT(x) &&
            scope_mask_bound_at((uint8_t)mask_ch, x, g_trig_x_actual, &blo, &bhi)) {
            lcd_set_pixel(x, (uint16_t)autofit_y(a, blo), bnd_c);
            lcd_set_pixel(x, (uint16_t)autofit_y(a, bhi), bnd_c);
        }
        if (prev_y >= 0) {                  /* connect prev..cur vertically */
            int16_t lo = prev_y < y ? prev_y : y;
            int16_t hi = prev_y < y ? y : prev_y;
            for (int16_t v = lo; v <= hi; v++)
                lcd_set_pixel(x, (uint16_t)v, cc);
        } else {
            lcd_set_pixel(x, (uint16_t)y, cc);
        }
        prev_y = y;
    }
    return yt_vmap_counts_per_px(a);
}

void draw_demo_waveform(uint32_t frame)
{
    const theme_t *th = theme_get();
    const scope_state_t *ss = scope_state_get();

    /* Forget last frame's vertical scales before anything is drawn. A band
     * that is not plotted this frame — no capture, channel off, or the demo
     * waveform below, whose amplitude is decorative — must leave the cursor
     * with NO transform, so it reports pixels instead of converting through a
     * scale that belongs to a trace no longer on the screen. */
    vband_clear();
    g_mask_ov_on = false;          /* re-armed by mask_overlay_begin() on live data */
    g_math_live = false;

    /* Freeze waveform when stopped — hold the last running frame */
    static uint32_t frozen_frame = 0;
    if (ss->running) {
        frozen_frame = frame;
    }
    uint32_t draw_frame = ss->running ? frame : frozen_frame;

    /*
     * If FPGA has delivered real ADC data, render from the sample buffers.
     * Each buffer holds 512 samples (normal mode) as unsigned 8-bit values
     * centered around 128 (after ADC offset calibration).
     * We map the 0-255 range into the waveform display area.
     */
    if (fpga_data_ready()) {
        /* Latch: once we've ever seen real ADC data, disable the
         * synthetic demo fallback permanently. */
        scope_mark_acquisition_ready();

        const volatile uint8_t *ch1_buf = fpga_get_ch1_buf();
        const volatile uint8_t *ch2_buf = fpga_get_ch2_buf();

        /* Re-triggering DISABLED — see note in demo waveform fallback.
         * TODO: re-enable once SPI3 protocol is confirmed working. */

        /* One map per channel (yt_fits): true scale over the full plot, or
         * autofit, split into halves when both channels autofit. */
        bool c1 = ss->ch1.enabled && ch1_buf != NULL;
        bool c2 = ss->ch2.enabled && ch2_buf != NULL;

        /* Software trigger: find the phase on the trigger SOURCE channel and
         * shift both channels' render windows by the same offset, so the trace
         * stops free-running (dancing) while staying time-aligned. The window
         * (the fast-code zoom, C5, or the re-sampled held record, C14) is
         * built once into yt_cols. */
        view_build(ss, ch1_buf, ch2_buf);
        mask_overlay_begin();
        const volatile uint8_t *b1 = (ch1_buf != NULL) ? yt_cols[0] : NULL;
        const volatile uint8_t *b2 = (ch2_buf != NULL) ? yt_cols[1] : NULL;
        /* The same window and channel set for the math trace and persistence
         * (C12): they must be drawn from exactly what the traces were. */
        g_math_live = true;

        /* The map each band is drawn with is recorded for the cursor
         * readout — see vband_note(). */
        autofit_t a1, a2;
        yt_fits(ss, &a1, &a2);
        uint8_t slot = 0;
        if (c1 && a1.on) {
            vband_note(slot, 1u, ss->ch1.vdiv_idx, &a1, draw_channel(b1, th->ch1, &a1, 0));
            slot++;
        }
        if (c2 && a2.on)
            vband_note(slot, 2u, ss->ch2.vdiv_idx, &a2, draw_channel(b2, th->ch2, &a2, 1));
        return;
    }

    /* ── Fallback: synthetic demo waveform (no FPGA data yet) ──────
     *
     * Only rendered if acquisition has NEVER produced real data. Once
     * the latch flips, we leave the grid empty instead — otherwise a
     * transient FPGA stall would flash fake waveforms on top of a
     * stopped trace, which is worse than an empty screen. */
    if (scope_acquisition_ready()) {
        return;
    }

    /* SPI3 acquisition triggers are now fired from main.c display loop
     * with 500ms warmup delay and early-abort safety. See main.c and
     * fpga_acquisition_task() for the crash-protection logic. */

    /* Mark the trace as synthetic ON the trace. The badge legend says the
     * same thing at the bottom of the screen, but the waveform is what
     * catches the eye and it is the thing that is not real. */
    font_draw_string(4, SCOPE_TOP + 14, "DEMO", th->warning, th->warning,
                     &font_small);

    static const int8_t sin_lut[64] = {
         0, 10, 19, 29, 38, 47, 56, 63, 71, 77, 83, 88, 92, 96, 98, 99,
        100, 99, 98, 96, 92, 88, 83, 77, 71, 63, 56, 47, 38, 29, 19, 10,
         0,-10,-19,-29,-38,-47,-56,-63,-71,-77,-83,-88,-92,-96,-98,-99,
       -100,-99,-98,-96,-92,-88,-83,-77,-71,-63,-56,-47,-38,-29,-19,-10,
    };

    /* CH1: sine wave — scale based on vdiv index */
    if (ss->ch1.enabled) {
        int16_t ch1_center = SCOPE_MID_Y - ss->ch1.position;
        int16_t amplitude = 40 - (int16_t)(ss->ch1.vdiv_idx * 2);
        if (amplitude < 10) amplitude = 10;

        for (uint16_t x = 0; x < LCD_WIDTH; x++) {
            uint8_t idx = (uint8_t)((x * 4 + draw_frame) & 0x3F);
            int16_t y = ch1_center - (sin_lut[idx] * amplitude / 100);
            if (y >= SCOPE_TOP && y < SCOPE_BOT) {
                lcd_set_pixel(x, (uint16_t)y, th->ch1);
                if (y + 1 < SCOPE_BOT)
                    lcd_set_pixel(x, (uint16_t)(y + 1), th->ch1);
            }
        }
    }

    /* CH2: square wave */
    if (ss->ch2.enabled) {
        int16_t ch2_center = SCOPE_MID_Y + 50 - ss->ch2.position;

        for (uint16_t x = 0; x < LCD_WIDTH; x++) {
            uint8_t phase = (uint8_t)((x * 4 + draw_frame) & 0x3F);
            int16_t y = ch2_center + (phase < 32 ? -25 : 25);
            if (y >= SCOPE_TOP && y < SCOPE_BOT)
                lcd_set_pixel(x, (uint16_t)y, th->ch2);

            /* Vertical edges */
            if (x > 0) {
                uint8_t prev = (uint8_t)(((x - 1) * 4 + draw_frame) & 0x3F);
                if ((prev < 32) != (phase < 32)) {
                    int16_t y1 = ch2_center - 25;
                    int16_t y2 = ch2_center + 25;
                    for (int16_t yy = y1; yy <= y2; yy++) {
                        if (yy >= SCOPE_TOP && yy < SCOPE_BOT)
                            lcd_set_pixel(x, (uint16_t)yy, th->ch2);
                    }
                }
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * Persistence overlay (C12: live traces)
 *
 * Layer 1 draws what earlier frames left in the buffer, BEFORE this frame's
 * trace goes on top; the channel renderers add this frame's trace to the
 * buffer as they draw it (persist_note), for the next frames. The
 * fade runs on wall time (persist_decay_ms), so 500 ms / 1 s mean that
 * whatever the frame rate. Dual-channel mode splits the glass into a CH1 band
 * above and a CH2 band below SCOPE_MID_Y, so a ghost's row says whose it is
 * and takes that channel's theme colour.
 * ═══════════════════════════════════════════════════════════════════ */

/* Before the ghosts are read, every frame and in either renderer: clear
 * them if the signal-to-screen mapping moved (persistence.h geometry guard:
 * one place that sees keys, shell sets, autoset and loads alike), then fade
 * by the wall time since the last frame. */
void persist_frame_decay(void)
{
    const scope_state_t *gss = scope_state_get();
#if FEATURE_FFT
    const uint8_t gview = (uint8_t)scope_view;
#else
    const uint8_t gview = 0u;
#endif
    (void)persist_follow_geometry(persist_geometry_key(gss, gview, view_merged(gss)),
                                  gss->running);
    static uint32_t last_tick;
    static bool     have_tick;
    uint32_t now = (uint32_t)xTaskGetTickCount() * (1000u / configTICK_RATE_HZ);
    persist_decay_ms(have_tick ? (uint32_t)(now - last_tick) : 0u);
    last_tick = now;
    have_tick = true;
}

/* A ghost's colour at screen row y: the channel's own, by band when the two
 * channels are split, neutral when they are overlaid (one buffer cannot say
 * whose a ghost is). */
uint16_t persist_base(const scope_state_t *ss, const theme_t *th, uint16_t y)
{
    if (!(ss->ch1.enabled && ss->ch2.enabled))
        return ss->ch1.enabled ? th->ch1 : th->ch2;
    const bool split = yt_vmap_split(true, true,
        ss->true_scale && scope_cal_true_scale_ok(1u, ss->ch1.vdiv_idx),
        ss->true_scale && scope_cal_true_scale_ok(2u, ss->ch2.vdiv_idx));
    if (!split) return th->text_secondary;
    return (y < SCOPE_MID_Y) ? th->ch1 : th->ch2;
}

void draw_persistence_overlay(void)
{
    if (!persist_enabled || !persist_is_initialized()) return;
    persist_frame_decay();

    const theme_t *th = theme_get();
    const scope_state_t *ss = scope_state_get();
    const uint8_t *buf = persist_get_buffer();
    if (!buf) return;

    for (uint16_t y = 0; y < PERSIST_HEIGHT; y++) {
        const uint16_t base = persist_base(ss, th, (uint16_t)(y + SCOPE_TOP));
        for (uint16_t x = 0; x < PERSIST_WIDTH; x++) {
            uint8_t intensity = buf[y * PERSIST_WIDTH + x];
            if (intensity > 0) {
                uint16_t color = persist_scale_rgb565(base, intensity);
                if (color != 0x0000)
                    lcd_set_pixel(x, y + SCOPE_TOP, color);
            }
        }
    }
}

void math_prepare(const scope_state_t *ss, math_frame_t *mf)
{
    memset(mf, 0, sizeof(*mf));
    mf->op = ((math_op_t)math_op < MATH_COUNT) ? (math_op_t)math_op : MATH_ADD;
    const math_op_t op = mf->op;
    const volatile uint8_t *s1 = fpga_get_ch1_buf();
    const volatile uint8_t *s2 = fpga_get_ch2_buf();
    mf->ok = g_math_live
        && (!math_op_uses_a(op) || (ss->ch1.enabled && s1 != NULL))
        && (!math_op_uses_b(op) || (ss->ch2.enabled && fpga_acq_record_has_ch(1u) && s2 != NULL));
    if (!mf->ok) {
        /* No record, or a channel the op reads is off: say so, draw nothing. */
        snprintf(mf->label, sizeof(mf->label), "M:%s --", math_channel_name(op));
        return;
    }
    mf->in.zero_a = rec_zero(ss, 0u);
    mf->in.zero_b = rec_zero(ss, 1u);
    mf->in.k_a = rec_vpc(ss, 0u);
    mf->in.k_b = rec_vpc(ss, 1u);
    const math_unit_t unit = math_unit_for(op, &mf->in);

    /* The peak over the window, in 64-column chunks (no 301-float scratch). */
    enum { CH = 64 };
    uint8_t a[CH], b[CH];
    float v[CH];
    float peak = 0.0f;
    for (uint16_t x0 = g_view_first; x0 < g_view_end; x0 += CH) {
        const uint16_t nc = (g_view_end - x0 < CH) ? (uint16_t)(g_view_end - x0) : CH;
        for (uint16_t i = 0; i < nc; i++) {
            a[i] = yt_cols[0][x0 + i];
            b[i] = yt_cols[1][x0 + i];
        }
        math_compute(op, a, b, nc, &mf->in, v);
        const float pk = math_peak(v, nc);
        if (pk > peak) peak = pk;
    }

    /* Scale choice keeps its last step while it still fits (no flicker). */
    static int scale_idx = -1;
    static uint8_t scale_key = 0xFFu;
    const uint8_t key = (uint8_t)(op | ((uint8_t)unit << 4));
    if (key != scale_key) { scale_idx = -1; scale_key = key; }
    scale_idx = math_scale_pick(peak, scale_idx);
    /* Never finer than the source channel's own division, so ADC noise on a
     * quiet result (A-B of one signal) stays a flat line, not a full screen. */
    const int floor_idx = math_scale_floor_idx(
        math_step_floor(op, &mf->in, SCOPE_CAL_COUNTS_PER_DIV));
    if (scale_idx < floor_idx) scale_idx = floor_idx;
    mf->step = math_scale_value(scale_idx);

    /* The cursors' view of this scale: `step` per SCOPE_DIV_PX rows. */
    g_math_cur.on = true;
    g_math_cur.direct = (unit == MATH_UNIT_VOLTS)  ? SCOPE_CURSOR_DIRECT_VOLTS
                      : (unit == MATH_UNIT_COUNTS) ? SCOPE_CURSOR_DIRECT_COUNTS
                                                   : SCOPE_CURSOR_DIRECT_NONE;
    g_math_cur.unit_per_px = mf->step / (float)SCOPE_DIV_PX;
    {
        scope_cursor_conf_t c = SCOPE_CURSOR_MEASURED;
        if (math_op_uses_a(op)) {
            scope_cursor_conf_t ca = scope_cursor_cal_conf(1u, ss->ch1.vdiv_idx);
            if (ca < c) c = ca;
        }
        if (math_op_uses_b(op)) {
            scope_cursor_conf_t cb = scope_cursor_cal_conf(2u, ss->ch2.vdiv_idx);
            if (cb < c) c = cb;
        }
        g_math_cur.conf = c;
    }
    char sc[16];
    math_scale_label(scale_idx, unit, sc, sizeof(sc));
    snprintf(mf->label, sizeof(mf->label), "M:%s %s", math_channel_name(op), sc);
}

/* The math trace's row at column x (within [g_view_first, g_view_end)). */
int16_t math_row(const math_frame_t *mf, uint16_t x)
{
    const uint8_t a = yt_cols[0][x], b = yt_cols[1][x];
    float v;
    int16_t row;
    math_compute(mf->op, &a, &b, 1u, &mf->in, &v);
    math_to_rows(&v, 1u, mf->step, SCOPE_DIV_PX, SCOPE_GRID_MID_Y, SCOPE_TOP, SCOPE_BOT, &row);
    return row;
}

void draw_math_waveform(void)
{
    if (!math_enabled) return;
    const theme_t *th = theme_get();
    math_frame_t mf;
    math_prepare(scope_state_get(), &mf);
    if (mf.ok) {
        int16_t prev = -1;
        for (uint16_t x = g_view_first; x < g_view_end; x++) {
            const int16_t r = math_row(&mf, x);
            int16_t lo = r, hi = r;
            if (prev >= 0) {
                if (prev < lo) lo = prev;
                if (prev > hi) hi = prev;
            }
            for (int16_t y = lo; y <= hi; y++)
                lcd_set_pixel(x, (uint16_t)y, th->warning);
            prev = r;
        }
        /* Zero marker of the math trace: a tick at the left edge. */
        for (uint16_t x = 0; x < 6; x++)
            lcd_set_pixel(x, SCOPE_GRID_MID_Y, th->warning);
    }
    font_draw_string(MATH_LABEL_X, SCOPE_TOP, mf.label, th->warning, th->warning, &font_small);
}
