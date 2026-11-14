/*
 * OpenScope 2C53T - Oscilloscope UI
 *
 * Waveform display with:
 *   - Plot-edge markers (plot_markers.h): CH1/CH2 0 V tags at the left
 *     edge, the trigger-level tag at the right edge + dotted line, the
 *     trigger-position pointer at the top
 *   - Trigger status badge (Auto/Trig'd/Ready/Stop)
 *   - Run/Stop box on the plot (SCOPE_PLOT_STOP_BOX)
 *   - The measurement strip (measure_strip.h): stock's 12 per channel,
 *     selected by meas_select_get(), 3 columns in the channel colour
 *   - Quick-change popup overlay
 */

#include "ui.h"
#include "lcd.h"
#include "font.h"
#include "theme.h"
#include "scope_state.h"
#include "scope_measure.h"
#include "measure_strip.h"
#include "plot_markers.h"
#include "scope_cal.h"
#include "scope_timebase.h"
#include "scope_roll.h"
#include "scope_cursor.h"
#include "scope_freq.h"
#include "scope_record.h"
#include "scope_geom.h"
#include "yt_vmap.h"
#include "math_channel.h"
#include "paused_view.h"
#include "persistence.h"
#include "fpga.h"
#include "scope_vpos.h"
#include "scope_roll_meas.h"
#include "FreeRTOS.h"   /* pvPortMalloc — the X-Y snapshot buffers */
#include "task.h"       /* xTaskGetTickCount — the out-of-scope popup overlay */
#include "scope_trigger.h"
#include "trig_edge.h"
#include "trig_policy.h"
#include "osc_menu_model.h"
#include "meas_select.h"
#include "scope_mask.h"
#ifdef FEATURE_FFT
#include "fft_live.h"
#include "fft_test_signals.h"
#endif
#include "at32f403a_407.h"  /* GPIO port reads in the debug overlay */
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "can_scope.h"

/* ═══════════════════════════════════════════════════════════════════
 * Layout constants
 * ═══════════════════════════════════════════════════════════════════ */

#define SCOPE_TOP       18      /* Below status bar */
#define SCOPE_BOT       (LCD_HEIGHT - 16)  /* Above info bar */
#define SCOPE_H         (SCOPE_BOT - SCOPE_TOP)
#define SCOPE_MID_Y     (SCOPE_TOP + SCOPE_H / 2)
#define SCOPE_DIV_PX    26      /* graticule division, rows (grid + math scale) */
/* The graticule's centre line: the 4th of the lines ruled every SCOPE_DIV_PX
 * rows from SCOPE_TOP. True scale puts 0 V (position 0) on it. */
#define SCOPE_GRID_MID_Y (SCOPE_TOP + 4 * SCOPE_DIV_PX)
#ifdef SCOPE_DEBUG_OVERLAY
#define SCOPE_DBG_Y     (LCD_HEIGHT - 60)  /* top of the debug strip */
#endif

/* The red STOP box on the plot (top centre). UI round 1: R1-B adds a RUN/STOP
 * tab to the status bar; until that is merged this box is the only STOP
 * indicator, so it defaults ON. Build with -DSCOPE_PLOT_STOP_BOX=0 once the
 * bar's tab is in, so no build is ever without one. */
#ifndef SCOPE_PLOT_STOP_BOX
#define SCOPE_PLOT_STOP_BOX 1
#endif
#define STOP_BOX_W      40
#define STOP_BOX_H      16
#define STOP_BOX_X      ((LCD_WIDTH - STOP_BOX_W) / 2)
#define STOP_BOX_Y      (SCOPE_TOP + PM_TPOS_H + 1)   /* under the trigger-position pointer */

_Static_assert(MSTRIP_PRM_COUNT == SCOPE_PRM_COUNT,
               "measure_strip's mask width must be stock's 12 measurements");

/* Bottom of the measurement strip (measure_strip.h anchors its rows there).
 *
 * BENCH 2026-08-14: with SCOPE_DEBUG_OVERLAY the badges were INVISIBLE, and had
 * been in every build — `-DSCOPE_DEBUG_OVERLAY` is unconditional in the
 * Makefile's C_DEFS, so there was no configuration in which they showed. The
 * overlay's four lines start at SCOPE_DBG_Y (=180) and run to ~219, painting
 * straight over rows that sat at 193 and 208.
 *
 * Debug builds therefore stack the strip ABOVE the debug strip. That costs
 * waveform height, which is the right trade: an overlay build is a bench build,
 * and a measurement you cannot see is worth less than the pixels it hides. */
#ifdef SCOPE_DEBUG_OVERLAY
#define STRIP_BOTTOM    SCOPE_DBG_Y
#else
#define STRIP_BOTTOM    SCOPE_BOT
#endif

/*
 * How many consecutive refusals the Freq badge holds its last reading for.
 * At the measured ~46% refusal rate a hold of 8 frames makes a dropout
 * visible only when the signal genuinely goes away, while still expiring
 * fast enough that a removed probe blanks the badge within a second.
 */
#define FREQ_HOLD_FRAMES  8u

/* Quick-change popup */
#define POPUP_W         200
#define POPUP_H         36
#define POPUP_X         ((LCD_WIDTH - POPUP_W) / 2)
#define POPUP_Y         ((LCD_HEIGHT - POPUP_H) / 2)

/* ═══════════════════════════════════════════════════════════════════
 * Quick-change popup state (shared with main.c via extern)
 * ═══════════════════════════════════════════════════════════════════ */

static char popup_text[32] = "";
/* A scope popup lives POPUP_MS of wall clock, running or stopped; popup_on
 * stays set until a frame has been drawn without the box (the erase). */
static bool     popup_on;
static uint32_t popup_until;
static bool     popup_hole_last;  /* the last compositor frame kept the box */

/* Has the box been painted for THIS popup? The incremental path paints it
 * once and then leaves it alone (see draw_scope_live_frame's hole); the full
 * path repaints it every frame anyway. Without this, every live frame would
 * repaint the box and the text would strobe at the capture rate. */
static bool popup_painted = false;

#define POPUP_MS 500u

static bool popup_live(void)
{
    return popup_on && (int32_t)(xTaskGetTickCount() - popup_until) < 0;
}

/* Popups outside the scope (2026-10-06, softkey UI P0; findings F15, F20).
 * The scope's popup is counted in scope frames, and only the scope renderer
 * counted them -- so a popup raised in the meter, siggen or settings never
 * appeared there, and turned up stale on the next scope visit. Outside the
 * scope it is now a tick-timed overlay the display task services, sized to
 * its text instead of a fixed 200 px box. */
#define POPUP_OVERLAY_MS 1200u
static bool     ov_pending;
static bool     ov_drawn;
static uint32_t ov_until;

void scope_show_popup(const char *text)
{
    int i = 0;
    while (text[i] && i < (int)sizeof(popup_text) - 1) {
        popup_text[i] = text[i];
        i++;
    }
    popup_text[i] = '\0';
    /* The scope's own popup rides on the plot; over the Oscilloscope Settings
     * page there is no plot being drawn, so it takes the overlay path the
     * other screens use (main.c services it while the page is up). */
    if (current_mode == MODE_OSCILLOSCOPE && !osc_menu_is_open()) {
        popup_on = true;
        popup_until = xTaskGetTickCount() + pdMS_TO_TICKS(POPUP_MS);
        popup_painted = false;
    } else {
        popup_on = false;
        ov_pending = true;
        ov_drawn = false;
        ov_until = xTaskGetTickCount() + pdMS_TO_TICKS(POPUP_OVERLAY_MS);
    }
}

static void draw_overlay_box(void)
{
    const theme_t *th = theme_get();
    const font_t *f = &font_large;
    uint16_t tw = font_string_width(popup_text, f);
    if (tw > LCD_WIDTH - 28) { f = &font_medium; tw = font_string_width(popup_text, f); }
    uint16_t w = tw + 24;
    if (w > LCD_WIDTH - 4) w = LCD_WIDTH - 4;
    uint16_t h = (uint16_t)(f->height + 12);
    uint16_t x = (uint16_t)((LCD_WIDTH - w) / 2);
    uint16_t y = (uint16_t)((LCD_HEIGHT - h) / 2);
    lcd_fill_rect(x, y, w, 1, th->highlight);
    lcd_fill_rect(x, y + h - 1, w, 1, th->highlight);
    lcd_fill_rect(x, y + 1, 1, h - 2, th->highlight);
    lcd_fill_rect(x + w - 1, y + 1, 1, h - 2, th->highlight);
    lcd_fill_rect(x + 1, y + 1, w - 2, 5, th->background);
    font_draw_string_box(x + 1, y + 6, w - 2, popup_text, th->text_primary,
                         th->background, f, FONT_ALIGN_CENTER);
    lcd_fill_rect(x + 1, y + 6 + f->height, w - 2, (uint16_t)(h - 7 - f->height), th->background);
}

int ui_popup_overlay_service(bool screen_repainted)
{
    if (!ov_pending) return 0;
    if ((int32_t)(xTaskGetTickCount() - ov_until) >= 0) {
        ov_pending = false;
        ov_drawn = false;
        return 2;                       /* expired: caller repaints the screen */
    }
    if (!ov_drawn || screen_repainted) {
        draw_overlay_box();
        ov_drawn = true;
        return 1;
    }
    return 0;
}

void ui_popup_overlay_cancel(void)
{
    ov_pending = false;
    ov_drawn = false;
}

/* The measurement strip between full frames. The live frame never touches
 * it, and plain Y-T has no full frames in a steady state, so without this
 * the badges (Freq included) kept whatever the last full frame showed. The
 * cells are repainted in place (draw_strip_cell boxed), never blanked. The
 * measurement passes are tear-checked against the acquisition's generation
 * (measure_live_channel, freq_held), as in the full path that already runs
 * them from this task; the 2026-08-14 acquisition stall predates that. */
static void draw_measurement_badges(const scope_state_t *ss, const theme_t *th, bool refresh);
void scope_ui_strip_refresh(void)
{
    const scope_state_t *ss = scope_state_get();
    if (ss->roll || !fpga_data_ready()) return;
    draw_measurement_badges(ss, theme_get(), true);
}

/* A change the live frame can show by itself (cursor moved or reselected):
 * the display task runs one live frame instead of a clear-and-repaint. */
static volatile bool g_live_req;
void scope_ui_request_live(void) { g_live_req = true; }
bool scope_ui_take_live_request(void)
{
    const bool r = g_live_req;
    g_live_req = false;
    return r;
}

bool scope_popup_active(void)
{
    return popup_on;
}

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
static void draw_trigger_line(const theme_t *th, int16_t trig_y)
{
    if (trig_y < SCOPE_TOP + 2) trig_y = SCOPE_TOP + 2;
    if (trig_y > SCOPE_BOT - 3) trig_y = SCOPE_BOT - 3;
    for (uint16_t x = 0; x < LCD_WIDTH - 6; x += (scope_trig_level_focus ? 2u : 4u))
        lcd_set_pixel(x, (uint16_t)trig_y, th->trigger);
}

/* ═══════════════════════════════════════════════════════════════════
 * Run/Stop indicator (SCOPE_PLOT_STOP_BOX; R1-B's bar tab replaces it)
 * ═══════════════════════════════════════════════════════════════════ */

static void draw_run_stop(const scope_state_t *ss, const theme_t *th)
{
#if SCOPE_PLOT_STOP_BOX
    if (!ss->running) {
        lcd_fill_rect(STOP_BOX_X, STOP_BOX_Y, STOP_BOX_W, STOP_BOX_H, th->warning);
        font_draw_string_center(LCD_WIDTH / 2, STOP_BOX_Y + 2, "STOP",
                                th->text_primary, th->warning, &font_small);
    }
#else
    (void)ss; (void)th;
#endif
}

/* ═══════════════════════════════════════════════════════════════════
 * Measurement strip (measure_strip.h)
 * ═══════════════════════════════════════════════════════════════════ */

/* The full-width AUX row (above the measurement rows) is reserved while the
 * CAN analyser's line, the cursor readout or the mask pass/fail readout has
 * something to say; it shows the first of them, in that order. */
static bool strip_aux(void)
{
    return can_scope_screen_on() || scope_state_get()->cursor.mode != CURSOR_OFF ||
           scope_mask_state() != MASK_PF_EMPTY;
}
static void cursor_row_paint(const scope_state_t *ss, bool force);

/* Every user of the strip's geometry (the live band, the roll band, the mask
 * strip, the cursor readout, the AUX row) reads this one function, so they
 * cannot disagree about where the strip starts. */
static uint16_t strip_top(const scope_state_t *ss)
{
    return measure_strip_top(meas_select_get(0), ss->ch1.enabled,
                             meas_select_get(1), ss->ch2.enabled,
                             ss->prm_hidden, strip_aux(), STRIP_BOTTOM);
}

static void strip_layout(const scope_state_t *ss, mstrip_layout_t *L)
{
    measure_strip_layout(meas_select_get(0), ss->ch1.enabled,
                         meas_select_get(1), ss->ch2.enabled,
                         ss->prm_hidden, strip_aux(), STRIP_BOTTOM, L);
}

/* One cell: label left in the dimmed channel colour, value right in the
 * channel colour (text_secondary "--" when refused). measure_strip_place()
 * guarantees the two never overlap; when they would not fit it keeps the
 * value only (the host test shows no stock label/value pair gets there). */
static void draw_strip_cell(const mstrip_cell_t *c, const scope_prm_t *p,
                            const theme_t *th, bool boxed)
{
    char buf[16];
    const uint16_t chc = c->ch ? th->ch2 : th->ch1;
    uint16_t vc = chc;
    if (p == NULL || p->unit == SCOPE_PRM_UNIT_NONE) {
        snprintf(buf, sizeof(buf), "--");   /* cannot measure it today */
        vc = th->text_secondary;
    } else {
        scope_prm_format(p, buf, sizeof(buf));
    }
    const char *label = scope_prm_label(c->prm);
    uint16_t lx, vx;
    const bool both = measure_strip_place(c->x, font_string_width(label, &font_small),
                                          font_string_width(buf, &font_small), &lx, &vx);
    if (both)
        font_draw_string(lx, (uint16_t)(c->y + 1), label,
                         measure_strip_dim(chc, th->background), th->background, &font_small);
    if (!boxed) {
        font_draw_string(vx, (uint16_t)(c->y + 1), buf, vc, th->background, &font_small);
        return;
    }
    /* In-place update: the value's whole box is painted (glyphs, gaps and
     * the rest of the box), so a shorter value leaves nothing behind and the
     * row is never blanked first. */
    const uint16_t end = (uint16_t)(c->x + MSTRIP_CELL_W - MSTRIP_PAD);
    const uint16_t bx = both ? vx : (uint16_t)(c->x + MSTRIP_PAD);
    if (end > bx)
        font_draw_string_box(bx, (uint16_t)(c->y + 1), (uint16_t)(end - bx), buf, vc,
                             th->background, &font_small,
                             both ? FONT_ALIGN_LEFT : FONT_ALIGN_RIGHT);
}

/* Paint the measurement rows: clear them (full width, so the live band and
 * the strip meet on one row edge), then every cell. p[ch] NULL = no record. */
static void draw_strip_cells(const mstrip_layout_t *L, const scope_params_t *p1,
                             const scope_params_t *p2, const theme_t *th, bool refresh)
{
    if (L->rows == 0u)
        return;
    const uint16_t y0 = (uint16_t)(L->bottom - L->rows * MSTRIP_ROW_H);
    if (!refresh)
        lcd_fill_rect(0, y0, LCD_WIDTH, (uint16_t)(L->rows * MSTRIP_ROW_H), th->background);
    for (unsigned k = 0; k < L->n; k++) {
        const mstrip_cell_t *c = &L->cell[k];
        const scope_params_t *pp = c->ch ? p2 : p1;
        draw_strip_cell(c, pp ? &pp->v[c->prm] : NULL, th, refresh);
    }
}


/* Rate for anything timed off the record on screen (Freq, widths, cursors):
 * the PUBLISHED record's rate — doubled when it is the interleaved merge
 * (fpga_record_sample_rate) — while the hardware is running the code the UI
 * shows; the table rate for the selected code otherwise. One copy, so the
 * badges and the cursors cannot disagree. */
static float record_rate_for_display(const scope_state_t *ss)
{
    /* A held record keeps the rate it was captured at, whatever the code now. */
    if (!ss->running && !ss->roll && fpga_data_ready())
        return fpga_record_sample_rate();
    return (fpga_acq_rate_idx_get() == ss->timebase_idx)
         ? fpga_record_sample_rate()
         : scope_timebase_sample_rate(ss->timebase_idx);
}

/* A record is HELD (STOP, zoom while paused, C14): no new record will be
 * published, so the screen re-samples the one it has. */
static inline bool view_held(const scope_state_t *ss)
{
    return !ss->running && !ss->roll && fpga_data_ready();
}

/* Is the view drawn as the two-converter merge? Running: what the published
 * record IS (fpga_acq_record_interleaved). Held (STOP, C14): what the code
 * now on the status bar would capture (fpga_interleave_active: enabled, one
 * channel, code <= 0x03), because the held record keeps its own rate
 * (fpga_record_sample_rate) and the screen is re-sampled to the code's
 * column rate. Stock's display time per pixel depends on the code alone
 * (0x08030A70), so a held merged record viewed at 0x04 reads 200 ns/div and is
 * zoomed out 2x, not relabelled 100 ns. At 0x00-0x03 either flag gives the same
 * column rate (124.8 MS/s x 20 == 249.6 MS/s x 10), so only that case differs.
 * The status bar, the timebase popup and the shell label read the same flag
 * (scope_ui_view_merged), so the label and the column rate cannot disagree. */
static bool view_merged(const scope_state_t *ss)
{
    return view_held(ss) ? fpga_interleave_active() : fpga_acq_record_interleaved();
}

bool scope_ui_view_merged(void) { return view_merged(scope_state_get()); }

/* The display zoom for the code on the status bar (parity C5,
 * scope_timebase_interp_factor): 1 everywhere but 0x00-0x03. The label
 * (status_bar.c) and the window plan (view_plan) derive from the same call. */
static uint8_t view_factor(const scope_state_t *ss)
{
    return scope_timebase_interp_factor(ss->timebase_idx, view_merged(ss));
}

/*
 * Measure one channel's live capture record.
 *
 * Returns false — and leaves *m zeroed — unless the FPGA has actually
 * delivered samples. fpga_data_ready() is the same gate the waveform plot
 * uses (see draw_demo_waveform), so the badges and the trace can never
 * disagree about whether what is on screen is real.
 */
static bool measure_live_channel(const volatile uint8_t *buf,
                                 scope_measure_t *m)
{
    memset(m, 0, sizeof(*m));
    if (!fpga_data_ready() || buf == NULL)
        return false;

    /* Tear-checked read (audit 2026-08-20, P0.2). fpga.c now stages each
     * capture and commits it in a ~10 µs window bracketed by the frame
     * generation counter (odd = commit in progress); a commit landing inside
     * our analysis pass would splice two frames through every statistic, so
     * re-run the pass when the generation moved. Commits arrive at ≤ ~34 Hz,
     * so the second attempt virtually always stands; after three, accept the
     * torn result (pre-fix behavior) rather than spin. The volatile cast is
     * safe because the generation check is what detects concurrent writes. */
    for (int attempt = 0; attempt < 3; attempt++) {
        uint32_t g0 = fpga_acq_frame_generation();
        scope_measure_record((const uint8_t *)buf, FPGA_ADC_BUF_SIZE, m);
        if ((g0 & 1u) == 0u && fpga_acq_frame_generation() == g0)
            break;
    }
    return m->valid;
}

/* No float formatting: the firmware links newlib-nano without
 * -u _printf_float, so "%f" prints nothing. Tenths are assembled by hand
 * here, and in scope_cursor.c for the cursor readout. */
__attribute__((unused))   /* the C6 params format via scope_prm_format */
static void fmt_tenths(char *b, size_t n, float v, const char *unit)
{
    if (v < 0.0f) v = 0.0f;
    unsigned t = (unsigned)(v * 10.0f + 0.5f);
    snprintf(b, n, "%u.%u%s", t / 10u, t % 10u, unit);
}

/*
 * Frequency with automatic Hz/kHz/MHz ranging, integer math only.
 * Same constraint as fmt_volts: newlib-nano's "%f" prints nothing.
 */
__attribute__((unused))
static void fmt_hz(char *b, size_t n, float hz)
{
    if (hz < 0.0f) hz = 0.0f;
    if (hz < 1000.0f) {
        const unsigned tenths = (unsigned)(hz * 10.0f + 0.5f);
        snprintf(b, n, "%u.%uHz", tenths / 10u, tenths % 10u);
    } else if (hz < 1000000.0f) {
        const unsigned hundredths = (unsigned)(hz / 10.0f + 0.5f);
        snprintf(b, n, "%u.%02ukHz", hundredths / 100u, hundredths % 100u);
    } else {
        const unsigned hundredths = (unsigned)(hz / 10000.0f + 0.5f);
        snprintf(b, n, "%u.%02uMHz", hundredths / 100u, hundredths % 100u);
    }
}

/*
 * A duration in seconds, ranged us/ms/s. Integer math, as above.
 */
__attribute__((unused))
static void fmt_seconds(char *b, size_t n, float s)
{
    if (s < 0.0f) s = 0.0f;
    if (s < 1e-3f) {
        snprintf(b, n, "%uus", (unsigned)(s * 1e6f + 0.5f));
    } else if (s < 1.0f) {
        const unsigned hundredths_ms = (unsigned)(s * 1e5f + 0.5f);
        snprintf(b, n, "%u.%02ums", hundredths_ms / 100u, hundredths_ms % 100u);
    } else {
        const unsigned hundredths = (unsigned)(s * 100.0f + 0.5f);
        snprintf(b, n, "%u.%02us", hundredths / 100u, hundredths % 100u);
    }
}


/*
 * Vertical calibration now lives in scope_cal.c, per CHANNEL as well as per
 * range, because the two channels have different frontends and measure
 * different gains on the same range index.
 *
 * WITHDRAWN 2026-08-18 — what used to be here was a two-case switch:
 *     case 2: 1.0f/347.0f   ("20mV/div")
 *     case 8: 1.0f/154.0f   ("2V/div")
 * The five-amplitude sweep of 2026-08-17 contradicts both. Range 2 rails on
 * both channels and yields no span at all; range 8 measures 279 mV/count, not
 * 6.49. The full provenance and the reason the replacement is tiered rather
 * than a flat table is in scope_cal.h.
 *
 * The volts/div label is the range's nominal value; true scale draws one
 * SCOPE_DIV_PX-row division as scope_cal_codes_per_div() codes (yt_vmap.h),
 * so the grid means what the label says. The renderer and the map must agree
 * on the division height.
 */
_Static_assert(SCOPE_DIV_PX == YT_VMAP_DIV_PX,
               "grid pitch no longer matches the true-scale map's division");

/* The cal table is indexed by the frontend range index, so its length and
 * the UI's range count are the same quantity counted twice. */
_Static_assert(SCOPE_CAL_RANGE_COUNT == VDIV_COUNT,
               "scope_cal range table length must match VDIV_COUNT");

/* The horizontal counterpart: one sample per screen column at zoom 1,
 * vertical grid every SCOPE_GEOM_DIV_PX (25, stock's) px, so a division is 25
 * samples; at codes 0x00-0x03 the zoom divides it (scope_timebase_view_label).
 * If either changes, the seconds/div label and every frequency silently start
 * meaning something else. */
_Static_assert(SCOPE_TIMEBASE_SAMPLES_PER_DIV == (float)SCOPE_GEOM_DIV_PX,
               "grid pitch no longer matches SCOPE_TIMEBASE_SAMPLES_PER_DIV");
_Static_assert(SCOPE_GEOM_COLS <= LCD_WIDTH, "the Y-T window must fit the glass");
_Static_assert(SCOPE_TIMEBASE_CODE_COUNT == TIMEBASE_COUNT,
               "timebase rate table length must match TIMEBASE_COUNT");

/* The cursor readout converts a pixel delta into a time with this and the
 * rate it is handed. Since the window can be zoomed -- the fast-code zoom
 * (C5) and the re-sampled held record (C14) -- that rate is the COLUMN rate,
 * 1000 x record rate / R (column_rate_for_display), so one pixel stays one
 * unit of the rate passed in. Handing the cursor the record rate while the
 * plot is zoomed would silently rescale every dt by the zoom. */
_Static_assert(SCOPE_CURSOR_SAMPLES_PER_PIXEL == 1.0f,
               "the cursor is fed the column rate, one column per unit — "
               "cursor dt and 1/dt would be wrong");

/*
 * Mask readout: the strip's AUX row (measure_strip.h), reserved while a mask
 * exists. Below the live band, so the compositor never paints over it,
 * and refreshed on its own epoch from main.c (scope_ui_mask_slot_refresh) --
 * the measurement badges' "stale until the next full repaint" is exactly the
 * defect a pass/fail counter must not have. A SKIP is shown as such: a
 * frozen PASS count over records that are no longer being judged would be
 * the project's signature stable-plausible-wrong number.
 */
#define MASK_SLOT_X 4u

static const char *mask_skip_short(mask_pf_reason_t r)
{
    switch (r) {
    case MASK_PF_R_STALE:       return "stale";
    case MASK_PF_R_UNTRIGGERED: return "free-run";
    case MASK_PF_R_NOT_ORDERED: return "no seam";
    case MASK_PF_R_NO_ANCHOR:   return "no trig";
    case MASK_PF_R_SETTINGS:    return "settings";
    case MASK_PF_R_SPAN:        return "span";
    case MASK_PF_R_CLIPPED:     return "clipped";
    default:                    return "?";
    }
}

static void draw_mask_slot(const theme_t *th)
{
    scope_mask_summary_t m;
    scope_mask_summary(&m);
    char b[32];
    uint16_t c = th->text_secondary;
    const uint16_t x = MASK_SLOT_X, y = strip_top(scope_state_get());   /* the AUX row */

    if (m.state == MASK_PF_TEACHING) {
        if (m.teach_last_reject != MASK_PF_R_OK)
            snprintf(b, sizeof(b), "MASK teach %u/%u %s", (unsigned)m.teach_got,
                     (unsigned)m.teach_target, mask_skip_short(m.teach_last_reject));
        else
            snprintf(b, sizeof(b), "MASK teach %u/%u", (unsigned)m.teach_got,
                     (unsigned)m.teach_target);
        c = th->highlight;
    } else if (m.hold) {
        snprintf(b, sizeof(b), "FAIL %lu/%lu HOLD-OK", (unsigned long)m.failed,
                 (unsigned long)m.tested);
        c = th->warning;
    } else if (m.last.verdict == MASK_PF_V_SKIP) {
        snprintf(b, sizeof(b), "MASK skip: %s", mask_skip_short(m.last.reason));
        c = th->highlight;
    } else if (m.tested == 0u) {
        snprintf(b, sizeof(b), "MASK ready");
        c = th->success;
    } else {
        snprintf(b, sizeof(b), "P%lu F%lu", (unsigned long)m.passed,
                 (unsigned long)m.failed);
        c = (m.last.verdict == MASK_PF_V_FAIL) ? th->warning : th->success;
    }
    lcd_fill_rect(0, y, LCD_WIDTH, MSTRIP_ROW_H, th->background);
    font_draw_string(x, y + 1, b, c, th->background, &font_small);
}

/* CAN readout: the strip's AUX row while `can screen on` (it wins the row
 * over the mask readout, as it won badge row 2 before). Below the live band
 * like the mask slot, refreshed from main.c at 5 Hz. Green = every frame
 * acknowledged with a good CRC; warning colour = NAK, CRC failure or bus error. */
void scope_ui_can_row_refresh(void)
{
    const theme_t *th = theme_get();
    char line[56];
    int ok = can_scope_screen_line(line, sizeof(line));
    const uint16_t y = strip_top(scope_state_get());          /* the AUX row */
    lcd_fill_rect(0, y, LCD_WIDTH, MSTRIP_ROW_H, th->background);
    font_draw_string(4, (uint16_t)(y + 1), line,
                     ok ? th->success : th->warning, th->background, &font_small);
}

void scope_ui_mask_slot_refresh(void)
{
    if (can_scope_screen_on() || scope_state_get()->cursor.mode != CURSOR_OFF)
        return;                                  /* the CAN line / cursor readout owns the AUX row */
    if (scope_mask_state() == MASK_PF_EMPTY) {
        /* Mask just cleared: the AUX row is released. Blank the row it had
         * (the top row a strip WITH aux would have); the live band extends
         * over it on its next frame, the next full repaint redraws the grid. */
        const scope_state_t *ss = scope_state_get();
        const uint16_t y = measure_strip_top(meas_select_get(0), ss->ch1.enabled,
                                             meas_select_get(1), ss->ch2.enabled,
                                             ss->prm_hidden, true, STRIP_BOTTOM);
        lcd_fill_rect(0, y, LCD_WIDTH, MSTRIP_ROW_H, theme_get()->background);
    } else {
        draw_mask_slot(theme_get());
    }
}

/*
 * Measurement strip (was: the measurement badges).
 *
 * Until 2026-08-13 this function printed the string literals "1.00kHz",
 * "707mV", "50.0%" and "1.00ms" — four numbers that had never touched an
 * ADC sample, on a screen a user reads as measurements. They are gone.
 *
 * What is printed now is measured from the live record, in the units this
 * instrument can actually defend (see scope_measure.h):
 *
 *   Vpp / Vrms   VOLTS wherever scope_cal.c has a bench-measured
 *                volts-per-count for that CHANNEL on that range: ranges 4-9,
 *                with 5/6/7 cross-validated three ways and 4/8/9 provisional.
 *                Ranges 0-3 rail on both channels and have no gain at all, so
 *                they still show honest ADC COUNTS ("Ncnt") rather than a
 *                plausible-but-invented voltage.
 *   Duty         a pure ratio; invariant under any affine counts->volts
 *                mapping, so it is already correct and stays correct once
 *                calibration lands. Asserted in tests/test_scope_measure.c.
 *   Freq         Hz from the SPECTRAL estimator (scope_freq.c) on measured
 *                timebase codes; declines rather than guess, with a short
 *                hold (see below). "--" on unmeasured codes.
 *   Per          SECONDS as 1/Freq while the spectral estimate stands;
 *                otherwise SAMPLES between mid-level crossings, never
 *                converted, because that crossing period is 5-50% out
 *                (EXP-13) and a bad number in real units reads as true.
 *
 * The wiring is Vpp = pp * k, Vrms = ac_rms * k, with k from
 * scope_cal_volts_per_count(ch, vdiv_idx), k == 0 meaning "show counts";
 * Hz needs the measured rate table in scope_timebase.c (EXP-17/18) and the
 * reg-0x01 code actually in force. `fpga scope measure` prints these same
 * quantities raw for the bench, from the same sources, so what EXP-19
 * validates is what the screen shows.
 */
/* Freq for one channel, from the spectral estimator, HELD across a short run
 * of refusals. `need` false (no Freq/Peri cell for that channel) skips the
 * estimate and forgets the hold, so a later selection never shows an old
 * reading. */
/*
 * Freq — spectral, not edge-counted, and it declines rather than guess.
 *
 * History, because it is the whole justification for the shape of this:
 * Freq = fs / period_samples was wired up on 2026-08-18 and removed the
 * same evening, measuring +1.6% to +112% against a known drive (EXP-13).
 * The sample rate was never the problem; the rising-crossing period
 * estimator found spurious crossings.
 *
 * EXP-16 then showed it was not the estimator either. Edge counting,
 * autocorrelation and a spectral peak all failed on the SAME bench records
 * and all succeeded on the same others: roughly a fifth to a half of
 * capture records are TORN, with their spectral energy smeared over ~6
 * bins. One record scored 0.380 coherence whole and 0.999 over its last
 * half. Tearing does not track signal amplitude, so it cannot be screened
 * out in advance — it has to be detected per record, which is exactly what
 * scope_freq does. Against 72 bench captures it answers 54% of the time,
 * is within 3.4% when it does, and is never wrong.
 *
 * The reading is HELD across a short run of refusals so the badge does not
 * strobe at the tearing rate; after FREQ_HOLD_FRAMES consecutive refusals
 * it blanks, because a stale number with no expiry is just a wrong number
 * that used to be right.
 */
static float freq_held(uint8_t ch, bool need, bool have,
                       const volatile uint8_t *buf, float fs)
{
    static float    freq_hz[2];
    static uint16_t freq_stale[2];
    if (ch > 1u)
        return 0.0f;
    if (!need) {
        freq_hz[ch] = 0.0f;
        freq_stale[ch] = FREQ_HOLD_FRAMES;
        return 0.0f;
    }
    /* The acquisition buffer is `volatile` because the acq task fills it;
     * the estimator only reads, and its coherence gate refuses torn
     * records anyway — so an MCU-side tear was never able to produce a
     * WRONG number here, only a refusal. The generation retry (P0.2)
     * recovers those refusals: a commit landing mid-estimate re-runs the
     * estimate on the settled frame instead of burning a hold frame. */
    _Static_assert(FPGA_ADC_BUF_SIZE == SCOPE_FREQ_MAX_N,
                   "estimator window must match the capture buffer");
    scope_freq_t fr;
    bool fe = false;
    if (have && buf != NULL) {
        for (int attempt = 0; attempt < 3; attempt++) {
            uint32_t g0 = fpga_acq_frame_generation();
            fe = scope_freq_estimate((const uint8_t *)buf, FPGA_ADC_BUF_SIZE, fs, &fr);
            if ((g0 & 1u) == 0u && fpga_acq_frame_generation() == g0)
                break;
        }
    }
    if (fe) {
        freq_hz[ch] = fr.hz;
        freq_stale[ch] = 0u;
    } else if (freq_stale[ch] < FREQ_HOLD_FRAMES) {
        freq_stale[ch]++;
    } else {
        freq_hz[ch] = 0.0f;
    }
    return freq_hz[ch];
}

static float   rec_vpc(const scope_state_t *ss, uint8_t ch);
static uint8_t rec_zero(const scope_state_t *ss, uint8_t ch);

/* refresh: in place (boxed cells, no row clear, AUX row untouched), for the
 * steady-state update between full frames (scope_ui_strip_refresh). */
static void draw_measurement_badges(const scope_state_t *ss, const theme_t *th, bool refresh)
{
    mstrip_layout_t L;
    strip_layout(ss, &L);   /* long PRM (prm_hidden) = no rows; the AUX row stays */

    if (L.rows > 0u) {
        /* Which channels have cells, and whether any of them is time-based on
         * the spectral estimate (Freq, Peri). */
        bool cells[2] = { false, false }, need_f[2] = { false, false };
        for (unsigned k = 0; k < L.n; k++) {
            cells[L.cell[k].ch] = true;
            if (L.cell[k].prm == SCOPE_PRM_FREQ || L.cell[k].prm == SCOPE_PRM_PERI)
                need_f[L.cell[k].ch] = true;
        }
        scope_measure_t m1, m2;
        const volatile uint8_t *b1 = fpga_get_ch1_buf(), *b2 = fpga_get_ch2_buf();
        const bool have1 = cells[0] && measure_live_channel(b1, &m1);
        const bool have2 = cells[1] && measure_live_channel(b2, &m2);

        /* The 12 measurements (C6), per channel. All arithmetic is in
         * scope_params_compute(): time uses the PUBLISHED record's rate
         * (doubled when interleaved), volts the scope_cal gain with k == 0
         * meaning counts, 0 V the channel's scope_measure_zero_for(). Freq/Per
         * come from the spectral estimate above, with its hold; below four
         * periods from the checked crossing period of a whole record, else
         * "--" (scope_params_compute). A channel with no record refuses
         * all twelve ("--"). */
        const float rec_fs = record_rate_for_display(ss);
        scope_params_t p1, p2;
        scope_prm_ctx_t ctx;
        ctx.sample_rate     = rec_fs;
        ctx.record_whole    = fpga_acq_record_time_ordered();   /* un-rotated: one stretch */
        ctx.volts_per_count = rec_vpc(ss, 0u);
        ctx.freq_hz         = freq_held(0u, need_f[0], have1, b1, rec_fs);
        ctx.zero_count      = rec_zero(ss, 0u);
        scope_params_compute(have1 ? &m1 : NULL, &ctx, &p1);
        ctx.volts_per_count = rec_vpc(ss, 1u);
        ctx.freq_hz         = freq_held(1u, need_f[1], have2, b2, rec_fs);
        ctx.zero_count      = rec_zero(ss, 1u);
        scope_params_compute(have2 ? &m2 : NULL, &ctx, &p2);

        draw_strip_cells(&L, &p1, &p2, th, refresh);
    }

    /* The AUX row: the CAN analyser's line, else the cursor readout, else
     * the mask readout. */
    if (L.aux && !refresh) {
        if (can_scope_screen_on())
            scope_ui_can_row_refresh();
        else if (ss->cursor.mode != CURSOR_OFF)
            cursor_row_paint(ss, true);
        else
            draw_mask_slot(th);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * Quick-change popup overlay
 * ═══════════════════════════════════════════════════════════════════ */

static void draw_popup_box(const theme_t *th)
{
    /* Dark box with border */
    lcd_fill_rect(POPUP_X, POPUP_Y, POPUP_W, POPUP_H, th->background);
    /* Top/bottom border */
    lcd_fill_rect(POPUP_X, POPUP_Y, POPUP_W, 1, th->highlight);
    lcd_fill_rect(POPUP_X, POPUP_Y + POPUP_H - 1, POPUP_W, 1, th->highlight);
    /* Left/right border */
    lcd_fill_rect(POPUP_X, POPUP_Y, 1, POPUP_H, th->highlight);
    lcd_fill_rect(POPUP_X + POPUP_W - 1, POPUP_Y, 1, POPUP_H, th->highlight);

    /* Centered text */
    font_draw_string_center(LCD_WIDTH / 2, POPUP_Y + 10, popup_text,
                            th->text_primary, th->background, &font_large);
    popup_painted = true;
}

static void draw_popup(const theme_t *th)
{
    if (!popup_on) return;
    if (!popup_live()) {          /* expired: this full frame drew without it */
        popup_on = false;
        return;
    }
    draw_popup_box(th);
}

/*
 * Popup upkeep for the flicker-free path.
 *
 * Until 2026-08-19 an active popup set `q.force`, which pinned the renderer
 * to the full clear-then-redraw for the popup's whole ~500 ms life — so the
 * cost of showing a two-word box was ten full-screen blanks, and the box
 * itself strobed along with the trace. The popup was not flickering despite
 * the redraw; it was causing it.
 *
 * Now the compositor treats the popup rectangle as a hole (it is the one
 * layer it cannot recompute per pixel, because of the glyphs), paints the box
 * exactly once, and repaints the band underneath on the first frame after the
 * countdown expires. No full redraw is needed at either end.
 */
/* The FFT / split / waterfall / X-Y views: their frames compose the plot
 * with no hole for the box, so the box is put back over each frame while it
 * lives, and the popup ends when it expires (it used to stay "active" in
 * these views for good, as nothing there counted it down). */
void scope_popup_view_tick(void)
{
    if (!popup_on) return;
    if (popup_live()) draw_popup_box(theme_get());
    else popup_on = false;
}

void scope_popup_overlay_tick(void)
{
    if (!popup_on) return;
    if (!popup_hole_last) {       /* this frame painted the band over it */
        popup_on = false;
        return;
    }
    if (!popup_painted)
        draw_popup_box(theme_get());
}

/* ═══════════════════════════════════════════════════════════════════
 * Demo waveform (sine + square)
 * ═══════════════════════════════════════════════════════════════════ */

/* Render one channel's live ADC buffer into the vertical band [y_top, y_bot),
 * AUTO-FITTING the trace to that band from its own min/max (2026-08-14). This
 * makes any signal fill the screen centred, regardless of the frontend gain,
 * the DC offset (DAC1), or amplitude — no clipping, no manual centring. It is
 * a visualisation autoscale, NOT a volts reference; the Vpp/Vrms badges carry
 * the calibrated volts separately. Consecutive samples are connected with a
 * vertical span so the trace reads as a continuous line, not dots. */
/*
 * THE vertical transform. Both renderers go through this.
 *
 * They did not, until 2026-08-19. The full path autoscaled from the buffer's
 * own min/max while the compositor used a fixed (v-128)/256 about mid-screen,
 * over a shorter band, with the channel position offset applied in one and
 * ignored in the other. Same samples, two different places on the glass — so
 * the trace JUMPED whenever the renderer changed, which on the bench looked
 * like "the trace goes back to the bottom after the popup". It hid for a week
 * because a popup or a cursor forced the full path and everything else was
 * the demo waveform.
 *
 * Two copies of a transform that are supposed to agree will not stay agreeing.
 * One copy, two callers.
 */
/* Display-task scratch shared by draw paths that never run at once: the roll
 * view's FIFO copy (~610 B), the FFT waterfall's one-row pixel line (640 B)
 * and the Y-T window (2 x 301 B, parity C5: what each column of each channel
 * shows, interpolated at the fast codes). Each is filled and consumed on the
 * display task inside one view's draw calls, and the views exclude each
 * other (scope_display_mode). Sharing them keeps the coldtrace build inside
 * RAM (it overflowed by 56 B when the round-2 parity branches were merged;
 * the Y-T window is smaller than the waterfall line, so it costs nothing). */
static union {
    scope_roll_fifo_t roll;
    uint16_t          waterfall[SHMEM_FFT_WATERFALL_COLS];
    uint8_t           yt[2][SCOPE_GEOM_COLS];
} ui_draw_scratch;
#define yt_cols (ui_draw_scratch.yt)

/* One channel's vertical transform, true scale or autofit (yt_vmap.h). */
typedef yt_vmap_t autofit_t;

/* ── The Y-T window's horizontal mapping (parity C5 x C14) ───────────────
 * ONE window, yt_cols, built once per frame by view_build() and read by every
 * consumer (full path, live compositor, trigger marker, math trace; the
 * cursors read the column rate it implies, the mask overlay is gated on it).
 * Its mapping comes from one plan (pv_view_plan, src/dsp/paused_view.c):
 *
 *   ratio R   record samples per column x 1000. Running: 1000 / L, L the
 *             fast-code zoom of parity C5 (10/5/2/1 merged, 20/10/4/2 per
 *             converter at 0x00-0x03, 1 elsewhere). Held (STOP): stock
 *             re-renders the held record when the timebase changes
 *             (0x08021178..0x08021348, docs/re/parity2_static.md s3; it never
 *             re-arms), so R = 1000 fs_rec / fs_col, the held record's own
 *             rate (fpga_record_sample_rate = rate at commit) over the column
 *             rate of the code now selected.
 *   L > 0     R == 1000 / L with L dividing 20: the C5 integer window
 *             (scope_geom.c), trigger fitted to 1/L sample, window clamped
 *             inside the record. Covers running, and STOP at (or between) fast
 *             codes of one record rate.
 *   L == 0    any other held ratio (zoom while paused, C14): pv_fill, the
 *             trigger column stays put, nearest sample for R >= 1000, the same
 *             16-tap sinc as C5 at any phase below it, columns outside the
 *             record blank as in stock: [g_view_first, g_view_end) is the
 *             span that shows the record, and every consumer stays inside it.
 *
 * Vertical (C14): in true scale only (autofit is range-blind by
 * construction), a held record is re-drawn as stock does (0x080212BC):
 * code' = zero_now + k (code - zero_cap), k = volts/count at capture over
 * volts/count now (pv_vscale), so it reads true at the range and position now
 * selected. Measurements of a held record use the capture's volts/count and
 * 0 V (rec_vpc / rec_zero).
 * Stock's position term is not applied: a position key moves the offset DAC
 * for the NEXT capture, so the record already carries it.
 * Cursor and badge rates: the cursors read the column rate (1000 fs_rec / R);
 * the measurements read the record's own (record_rate_for_display). */
/* The span of the last build that shows the record, [first, end): all 301
 * columns but on the general path. Zero-initialised on purpose (bss, not
 * .data: the warm-handoff builds have no RAM to spare); view_build() sets it
 * before any consumer reads it. */
static struct { uint16_t first, end; } g_span;
#define g_view_first (g_span.first)
#define g_view_end   (g_span.end)
/* What the record's codes mean: volts/count and the 0 V code of each channel
 * when it was captured. Latched every running frame and at STOP. */
static struct {
    float   vpc[2];
    uint8_t zero[2];
    bool    valid;
} g_cap;
static int chan_zero_code(const scope_state_t *ss, uint8_t ch);

/* The plan for the current state: returns L (0 = general path), *ratio = R,
 * *fs_rec = the rate R was taken against. A pure function of the state, so
 * the cursor shell command and the readouts answer without a draw having run. */
static uint8_t view_plan(const scope_state_t *ss, uint32_t *ratio, float *fs_rec)
{
    const bool held = view_held(ss);
    const uint8_t L_code = view_factor(ss);
    float fr, fc = 0.0f;
    if (held) {
        fr = fpga_record_sample_rate();
        fc = scope_timebase_sample_rate(ss->timebase_idx) *
             (view_merged(ss) ? 2.0f : 1.0f) * (float)L_code;
    } else {
        fr = record_rate_for_display(ss);
    }
    const uint8_t L = pv_view_plan(held, fr, fc, L_code, ratio);
    if (fs_rec) *fs_rec = fr;
    return L;
}

/* What one COLUMN is worth for the cursors: 1000 fs_rec / R, which is the
 * record rate x L on the integer window (C5) and the selected code's column
 * rate on a re-sampled held record (C14, up to the 100x clamp). Only the
 * column-domain readouts (cursor dt, 1/dt, X) use it; everything measured on
 * the record itself (Freq, the C6 parameters) keeps the record rate, so a
 * zoom can never leak into a measurement. */
static float column_rate_for_display(const scope_state_t *ss)
{
    uint32_t r = PV_RATIO_ONE;
    float fs = 0.0f;
    (void)view_plan(ss, &r, &fs);
    return (r > 0u) ? fs * (float)PV_RATIO_ONE / (float)r : fs;
}

/* Latch the capture tuple (every running frame, and the RUN/STOP key). */
void scope_ui_paused_latch(void)
{
    const scope_state_t *ss = scope_state_get();
    for (uint8_t ch = 0; ch < 2u; ch++) {
        g_cap.vpc[ch]  = scope_cal_volts_per_count((uint8_t)(ch + 1u),
                                                   ch ? ss->ch2.vdiv_idx : ss->ch1.vdiv_idx);
        g_cap.zero[ch] = (uint8_t)chan_zero_code(ss, ch);
    }
    g_cap.valid = true;
}

/* Volts per count and 0 V code of channel ch's record: the capture's for a
 * held record (the record does not change with the range or position now
 * selected), the current ones otherwise. */
static bool rec_held_cap(const scope_state_t *ss)
{
    return g_cap.valid && !ss->running && !ss->roll && fpga_data_ready();
}
static float rec_vpc(const scope_state_t *ss, uint8_t ch)
{
    return rec_held_cap(ss) ? g_cap.vpc[ch]
                            : scope_cal_volts_per_count((uint8_t)(ch + 1u),
                                                        ch ? ss->ch2.vdiv_idx : ss->ch1.vdiv_idx);
}
static uint8_t rec_zero(const scope_state_t *ss, uint8_t ch)
{
    return rec_held_cap(ss) ? g_cap.zero[ch] : (uint8_t)chan_zero_code(ss, ch);
}

int16_t scope_ui_lowest_visible_pos(uint8_t ch)
{
    const scope_state_t *ss = scope_state_get();
    const uint8_t r = ch ? ss->ch2.vdiv_idx : ss->ch1.vdiv_idx;
    const float cpd = scope_cal_codes_per_div((uint8_t)(ch + 1u), r);
    if (!ss->true_scale || !(cpd > 0.0f) || !scope_vpos_active())
        return -100;
    /* rows from the centre line down to half a division above the strip */
    const int rows = (int)strip_top(ss) - SCOPE_GRID_MID_Y - SCOPE_DIV_PX / 2;
    int p = -(int)((float)rows * cpd / (float)SCOPE_DIV_PX);
    if (p < -100) p = -100;
    if (p > 0) p = 0;
    return (int16_t)p;
}

uint32_t scope_ui_view_ratio(void)
{
    uint32_t r = PV_RATIO_ONE;
    (void)view_plan(scope_state_get(), &r, NULL);
    return r;
}

/* The true-scale path's held-record rescale (stock 0x080212BC):
 * zero_now + k (v - zero_cap), k = volts/count at capture / now. Identity
 * while running, or when range and position are the capture's. */
static inline uint8_t pv_vscale(uint8_t v, int8_t ch)
{
    const scope_state_t *ss = scope_state_get();
    if (ch < 0 || ch > 1 || !rec_held_cap(ss))
        return v;
    const float kc = g_cap.vpc[ch];
    const float kn = scope_cal_volts_per_count((uint8_t)(ch + 1),
                                               ch ? ss->ch2.vdiv_idx : ss->ch1.vdiv_idx);
    const int zc = g_cap.zero[ch], zn = chan_zero_code(ss, (uint8_t)ch);
    if (!(kc > 0.0f) || !(kn > 0.0f) || (kc == kn && zc == zn))
        return v;
    return (uint8_t)pv_y_map((int)v, 0, zc, zn, kc, kn, 0, 255);
}

/* mask_ch: the channel this band draws (0/1), or -1. When a finished mask
 * covers it, the fit also spans the mask's extent, so the bounds drawn over
 * the trace stay on the glass instead of clamping to the band edge. Every
 * caller passes the channel it draws -- trace, compositor, trigger marker --
 * so all three keep agreeing on the scale. */
static void autofit_prep(autofit_t *a, const volatile uint8_t *buf,
                         int16_t y_top, int16_t y_bot, int8_t mask_ch)
{
    /* Over the window as DRAWN (the interpolated columns, not the source
     * samples): sin(x)/x overshoots a fast edge by a few counts, and fitting
     * the source would clip that flat at the band edge. */
    uint8_t mn = 255, mx = 0;
    for (uint16_t x = g_view_first; x < g_view_end; x++) {   /* not the blank columns */
        const uint8_t sv = buf[x];
        if (sv < mn) mn = sv;
        if (sv > mx) mx = sv;
    }
    uint8_t elo, ehi;
    if (mask_ch >= 0 && scope_mask_extent((uint8_t)mask_ch, &elo, &ehi)) {
        if (elo < mn) mn = elo;
        if (ehi > mx) mx = ehi;
    }
    yt_vmap_autofit(a, mn, mx, y_top, y_bot);
}

/* Where code v lands, unclipped (the tags) and clipped (everything drawn).
 * True scale rescales a held record to the range now selected first. */
static inline int autofit_y_raw(const autofit_t *a, int v)
{
    if (a->fx_ch >= 0)
        v = pv_vscale((uint8_t)v, a->fx_ch);
    return yt_vmap_raw(a, v);
}

static inline int16_t autofit_y(const autofit_t *a, uint8_t v)
{
    return yt_vmap_clip(a, autofit_y_raw(a, v));
}

/* The record code true scale puts on the centre line: 0 V at position 0
 * (raw 128, stock's zero, record 100 with the calibration page; the legacy
 * centring targets record 128). Position moves the signal in the record. */
static uint8_t yt_centre_code(void)
{
    return scope_measure_zero_for(scope_vpos_active(), 0, (int)FPGA_ADC_OFFSET);
}

/* Record code of the channel's 0 V (ch 0/1): the ground tag, the badges and
 * the cursors' absolute Y all use it. */
static int chan_zero_code(const scope_state_t *ss, uint8_t ch)
{
    return scope_measure_zero_for(scope_vpos_active(),
                                  ch ? ss->ch2.position : ss->ch1.position,
                                  (int)FPGA_ADC_OFFSET);
}

/*
 * What the renderer ACTUALLY did to the vertical axis, recorded per band for
 * the cursor readout.
 *
 * The cursor cannot assume the true-scale transform, because a band
 * may be autofit (a range without calibration, or view.truescale off), and
 * autofit's pixels-per-count is a property of the frame that was drawn — it
 * rescales from the record's own min/max, every frame. Reading the scale off the drawing rather than off an assumption is
 * the same "one copy, two callers" rule that autofit_prep() itself exists to
 * enforce; the two-copies version of that bug (full path vs compositor)
 * already cost this project a week.
 *
 * on == false means "nothing was plotted in this band", which the cursor
 * reports as pixels rather than converting.
 */
typedef struct {
    bool    on;
    int16_t y_top, y_bot;
    float   counts_per_pixel;
    uint8_t channel;            /* 1 or 2 */
    uint8_t range_idx;
    bool    mid_known;          /* mid_row is the channel's 0 V (true scale) */
    int16_t mid_row;
    float   mid_counts;         /* that code relative to 0 V (centred = 0)   */
} vband_t;

static vband_t g_vband[2];

/* The math trace's own scale, as drawn this frame (draw_math_waveform): the
 * cursors read it instead of the channel bands while the overlay is up, since
 * it spans the whole glass over them. */
static struct {
    bool                on;
    uint8_t             direct;        /* SCOPE_CURSOR_DIRECT_* */
    float               unit_per_px;
    scope_cursor_conf_t conf;
} g_math_cur;

static void vband_clear(void)
{
    g_vband[0].on = false;
    g_vband[1].on = false;
    g_math_cur.on = false;
}

/* channel 1/2, a: the map the band was drawn with. True scale anchors the
 * channel's 0 V code to a row, so absolute Y is known (mid_counts = 0 at the
 * ground tag's row); autofit re-anchors per frame and leaves it unknown. */
static void vband_note(uint8_t slot, uint8_t channel, uint8_t range_idx,
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
 * as stock's FPGA_ADC_OFFSET. That record code is mapped through the SAME
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
static uint16_t scope_soft_trigger_offset(const scope_state_t *ss,
                                          const volatile uint8_t *src_buf, uint8_t L,
                                          int16_t *back_q8);
static void view_build(const scope_state_t *ss, const volatile uint8_t *b1,
                       const volatile uint8_t *b2);
static void yt_fits(const scope_state_t *ss, autofit_t *a1, autofit_t *a2);

static int16_t trigger_marker_y(const scope_state_t *ss, const autofit_t *a)
{
    if (a == NULL || !a->on)
        return (int16_t)(SCOPE_MID_Y - ss->trigger.level);
    int rec = (int)fpga_acq_trig_code_get() + (int)FPGA_ADC_OFFSET;
    if (rec < 0)   rec = 0;
    if (rec > 255) rec = 255;
    return autofit_y(a, (uint8_t)rec);
}

/* Full-path variant: the trigger source's map exactly as
 * draw_demo_waveform() will build it (same window, same yt_fits). */
static int16_t trigger_marker_y_full(const scope_state_t *ss)
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

/* ═══════════════════════════════════════════════════════════════════
 * Plot-edge markers (plot_markers.h, UI_STUDY S10)
 *
 * Ground (0 V) tag per channel at the left edge, trigger-level tag at the
 * right edge, both on the row where that level is ON THE TRACE: mapped through
 * the same map the band was drawn with (yt_fits), not
 * `SCOPE_MID_Y - position`: the position is already in the record. Off the band, the tag sits on the band edge pointing up or
 * down. Both renderers build the set with markers_build() and ask
 * plot_marker_px() for every pixel, so the live frames keep the tags (before
 * round 1 the compositor painted over them) and draw the same shapes.
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    bool       on;
    pm_dir_t   dir;
    pm_label_t label;
    int16_t    x0, y0;          /* top-left of the 9x9 tag */
    uint16_t   color;
} pm_tag_t;

/* Drawing priority: [0] the trigger tag, [1] the active channel's ground,
 * [2] the other channel's ground (the active one wins an overlap). */
typedef struct { pm_tag_t t[3]; } pm_set_t;

/* The two Y-T maps, the only place that decides them (from the window
 * view_build() last filled into yt_cols). True scale where the range has a
 * real volts/div, over the full plot; autofit elsewhere (a NONE range, or
 * view.truescale off), split into halves when both channels autofit. */
static void yt_fits(const scope_state_t *ss, autofit_t *a1, autofit_t *a2)
{
    autofit_t *a[2] = { a1, a2 };
    const bool en[2] = { ss->ch1.enabled, ss->ch2.enabled };
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
static void markers_build(const scope_state_t *ss, const theme_t *th,
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
        int rec = (int)fpga_acq_trig_code_get() + (int)FPGA_ADC_OFFSET;
        if (rec < 0)   rec = 0;
        if (rec > 255) rec = 255;
        row = autofit_y_raw(src, rec);
        top = src->y_top;
        if (src->y_bot < bot) bot = src->y_bot;
    } else {
        row = SCOPE_MID_Y - ss->trigger.level;
    }
    tag_put(&m->t[0], row, top, bot, PM_POINT_LEFT, LCD_WIDTH - PM_TAG_W,
            PM_LABEL_T, th->trigger);
}

/* Pixel colour of the marker set at (x, y), or false: tags in priority order. */
static inline bool markers_px(const pm_set_t *m, int x, int y, uint16_t bg,
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
static void draw_plot_markers(const pm_set_t *m, const theme_t *th)
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

/* The trigger-position pointer: over the column where the trigger point
 * actually landed (g_trig_x_actual), at the top of the plot. */
static inline bool tpos_px(int trig_x, int x, int y)
{
    return trig_x >= 0 &&
           plot_marker_tpos_px(x - (trig_x - PM_TPOS_W / 2), y - SCOPE_TOP) == PM_PX_FILL;
}

/*
 * Mask pass/fail overlay (docs/specs/scope/mask-pass-fail.md).
 *
 * The verdict is computed in the acquisition task (scope_mask.c); this only
 * PAINTS it, per column, from the result already recorded -- no analysis on
 * the display path. Painted only when the window is on the HARDWARE anchor
 * (g_trig_anchor == 2: the alignment the mask was taught on) and only for the
 * frame generation that was actually judged; anything else draws as if there
 * were no mask, rather than colouring a frame the verdict does not describe.
 *
 *   trace   CH1/CH2 trace pixels turn th->warning on columns that were
 *           outside the mask in that record.
 *   strip   3 px at the bottom of the live band over the masked span:
 *           green = passed, red = this column failed, dotted red = the record
 *           failed elsewhere, dotted amber = refused (SKIP), amber = teaching.
 */
static uint8_t  g_trig_anchor = 0;        /* see scope_soft_trigger_offset() */
static int16_t  g_trig_x_actual = -1;
/* The window last placed (scope_geom.h). L == 0 marks the general held-record
 * path of view_build (C14), so L == 1 means exactly "one column per sample". */
static scope_window_t g_view;
static bool     g_mask_ov_on;
static bool     g_mask_bounds_on;
static uint32_t g_mask_ov_gen;

static void mask_overlay_begin(void)
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

/* Mask bounds for screen column x of channel ch, as two y values through the
 * band's own transform (yfn). Dotted: every other column. */
#define MASK_BOUND_DOT(x) (((x) & 1u) == 0u)

static inline bool mask_px_fail(int8_t ch, int x)
{
    return g_mask_ov_on && ch >= 0 &&
           scope_mask_column((uint8_t)ch, x, g_trig_x_actual, g_mask_ov_gen) == SCOPE_MASK_COL_FAIL;
}

/* Strip colour for column x, or 0 for none; *dotted = paint every other px. */
static uint16_t mask_strip_color(int x, const theme_t *th, bool *dotted)
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

#define MASK_STRIP_H 3u

/* Full-path strip (the compositor paints the same rows per pixel). */
static void draw_mask_strip(const theme_t *th, uint16_t band_bot)
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

static bool     g_math_live;    /* a real record was drawn this frame (yt_cols valid) */
static inline void persist_note(uint16_t x, int16_t prev_y, int16_t y)
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

/* Software display trigger — the fix for the free-running "dancing" trace.
 *
 * Real scopes hold a waveform still by starting every sweep at the same point
 * on the wave: the moment the signal crosses a chosen level going a chosen
 * direction. We do that in software here — find that crossing in the trigger
 * source buffer and return its index, so the render window starts on a
 * repeatable phase. The offset is applied to BOTH channels so they stay
 * time-aligned. It never distorts the samples: it only chooses WHERE the drawn
 * window begins, so amplitude and frequency are untouched.
 *
 * 0 = free-run (disabled, NULL buffer, or no crossing found). "No crossing" is
 * the normal outcome when the level sits outside the signal, and free-running
 * then is exactly AUTO-mode behaviour.
 */
/* Where the trigger point landed on the glass in the last computed window:
 * column 0..319, or -1 when free-running. g_trig_anchor: 0 none, 1 soft
 * (midline crossing), 2 hardware (the level crossing nearest index 512 of a
 * time-ordered record). Read by the position marker and by `spi3 frame`. */
/* g_trig_x_actual / g_trig_anchor are declared with the mask overlay above
 * (it reads them); they start at -1 / 0 via scope_soft_trigger_offset(). */

/* The hardware trigger point of a time-ordered record: the crossing of the
 * level (code - 28, EXP-55/56) in the chosen direction nearest index 512,
 * within +/-48 (EXP-53: 501..520). Schmitt-armed like the soft trigger.
 * Returns the index or -1. */
static int hw_trigger_anchor(const scope_state_t *ss, const volatile uint8_t *buf)
{
    /* Moved to trig_edge_anchor() 2026-10-03 so the mask test aligns on the
     * same crossing the glass does; behaviour unchanged. */
    int c = (int)fpga_acq_trig_code_get() + (int)FPGA_ADC_OFFSET;
    return trig_edge_anchor(buf, c, ss->trigger.edge == TRIG_RISING);
}

/* Free-run window: from index 0 at one column per sample, as before C5. A
 * zoomed window is only 15-151 samples wide, so it starts past the record's
 * invalid head instead of drawing nothing but that head. */
static uint16_t view_free_run(uint8_t L)
{
    g_view.L  = L;
    g_view.u0 = (L == 1u) ? 0 : (int32_t)SCOPE_RECORD_HEAD_SKIP * (int32_t)L;
    return (uint16_t)scope_geom_first_sample(&g_view);
}

/* Returns the record index window column 0 shows (the old `toff`, which is
 * what `spi3 frame` prints); the window itself is left in g_view, placed at
 * zoom L (the plan's; 1 for the general held-record path, which pivots on the
 * column this leaves in g_trig_x_actual), and in *back_q8 (may be NULL) how
 * far before the anchor sample the crossing lies, in 1/256 sample (0 when
 * nothing was locked). */
static uint16_t scope_soft_trigger_offset(const scope_state_t *ss,
                                          const volatile uint8_t *src_buf, uint8_t L,
                                          int16_t *back_q8)
{
    g_trig_x_actual = -1;
    g_trig_anchor = 0;
    if (back_q8) *back_q8 = 0;
    if (!ss->soft_trigger || src_buf == NULL)
        return view_free_run(L);

    const int32_t N = (int32_t)FPGA_ADC_BUF_SIZE;

    /* Horizontal position: the column the trigger point should land on,
     * stock's 150 + hpos with hpos +-150 (scope_geom.h). */
    int tx = ss->trig_x;
    if (tx < SCOPE_GEOM_TRIG_COL_MIN) tx = SCOPE_GEOM_TRIG_COL_MIN;
    if (tx > SCOPE_GEOM_TRIG_COL_MAX) tx = SCOPE_GEOM_TRIG_COL_MAX;
    int32_t before, after;           /* record samples left / right of tx */
    scope_geom_span(L, tx, &before, &after);

    /* 1. Hardware anchor (2026-09-22): a time-ordered record carries the
     *    hardware trigger at index 512, and the crossing nearest it IS the
     *    event that fired. CH1 only -- the level register was measured there.
     *    Tested on the trigger SOURCE, not the buffer address: `spi3 frame`
     *    passes a snapshot copy, and an address test made the bench check
     *    exercise the fallback while the screen used the anchor (v17).
     *    Zoomed (L > 1), the crossing is placed to 1/L sample from the two
     *    samples around it -- stock re-finds it on the interpolated copy for
     *    the same reason (0x08021EF0): at 10 ns/div one sample is 10-20 px. */
    if (ss->trigger.source != TRIG_SRC_CH2 && fpga_acq_record_time_ordered()) {
        int a = hw_trigger_anchor(ss, src_buf);
        if (a >= 1) {
            const int c = (int)fpga_acq_trig_code_get() + (int)FPGA_ADC_OFFSET;
            const int32_t back = scope_geom_back_q8(src_buf[a - 1], src_buf[a], c);
            if (back_q8) *back_q8 = (int16_t)back;
            g_trig_x_actual = (int16_t)scope_geom_fit(&g_view, L, a, back, tx, 0, N);
            g_trig_anchor = 2;
            return (uint16_t)scope_geom_first_sample(&g_view);
        }
    }

    /* SEAM GUARD (EXP-22, 2026-09-03): the acquisition record is NOT always
     * time-contiguous at its edges. Saved-frame analysis found ~9-14-sample
     * phase discontinuities clustered at indices 32..96 (a stale head —
     * residue roughly one read cadence old, the same defect family as the
     * "reads are off by one byte" finding) and 864..928. A trigger crossing
     * fabricated at such a seam locks the trace to an arbitrary phase — at
     * 500 Hz it showed as frames flipping 180 degrees, ~11-sample p95 jitter,
     * indistinguishable from free-run. Every frame whose seam fell OUTSIDE
     * the drawn window locked to <0.1 sample, so the algorithm is sound and
     * the guard just keeps it off the bad data: start the search at
     * SEAM_GUARD, which also keeps the drawn window clear of the
     * late seams for any signal above ~35 Hz at the slow timebases. The
     * underlying acquisition defect is filed, not fixed, by this.
     * The number itself lives in scope_record.h so the spectrum views skip
     * the same head (2026-09-15). */
    const uint16_t SEAM_GUARD = SCOPE_RECORD_HEAD_SKIP;

    /* Auto-level: base the threshold at the SIGNAL'S OWN midline, not a fixed
     * ADC 128. The capture is frequently not centred (e.g. mean 84, range
     * 34..136 on the bench), so a fixed 128 sits up near the peak where the
     * slope is shallow and the crossing jitters frame to frame — which reads on
     * screen as the trace dancing. The 50% level (min+max)/2 is the steep
     * mid-slope crossing and locks cleanly regardless of DC offset;
     * trigger.level then shifts it up/down from there (true-scale slope:
     * SCOPE_H px == 256 counts). Min/max over the guarded region only, so a
     * seam's glitch cannot skew the threshold either. */
    uint8_t mn = 255, mx = 0;
    for (uint16_t i = SEAM_GUARD; i < FPGA_ADC_BUF_SIZE; i++) {
        uint8_t s = src_buf[i];
        if (s < mn) mn = s;
        if (s > mx) mx = s;
    }
    if ((int)mx - (int)mn < 8)
        return view_free_run(L);                  /* ~flat: nothing to lock to */

    int thr = ((int)mn + (int)mx) / 2 + ((int)ss->trigger.level * 256) / SCOPE_H;
    if (thr < (int)mn + 1) thr = (int)mn + 1;     /* keep it inside the signal */
    if (thr > (int)mx - 1) thr = (int)mx - 1;

    /* 2. Soft anchor: the midline crossing, searched so that the window
     *    (`before` samples left of the crossing, `after` right) stays inside
     *    the record and clear of the guarded head. */
    /*    Stock's three-point edge confirm (0x08028DE4, parity C3) on top of
     *    the Schmitt arming this always had: a crossing whose next sample
     *    falls straight back is a one-sample spike, not the edge. SINGLE
     *    keeps two points, as stock does. The measured case for adding it
     *    is in trig_edge.h (tests/test_trig_edge.c [7]-[9]). */
    const int lo = (int)SEAM_GUARD + (int)before;
    const int hi = (int)N - (int)after;           /* last start that fits, +1 */
    int a = trig_edge_search(src_buf, (int)FPGA_ADC_BUF_SIZE, lo, hi, thr,
                             ss->trigger.edge == TRIG_RISING, 3u,
                             ss->trigger.mode == TRIG_SINGLE ? 0u : 1u);
    /* The old search returned index 0 of its span as "nothing": keep that
     * (a crossing on the very first sample has no "before"). */
    if (a <= lo)
        return view_free_run(L);
    const int32_t back = scope_geom_back_q8(src_buf[a - 1], src_buf[a], thr);
    if (back_q8) *back_q8 = (int16_t)back;
    g_trig_x_actual = (int16_t)scope_geom_fit(&g_view, L, a, back, tx,
                                              (int32_t)SEAM_GUARD, N);
    g_trig_anchor = 1;
    return (uint16_t)scope_geom_first_sample(&g_view);
}

/* Place the window on the trigger source and fill yt_cols for BOTH channels
 * through it (a channel with no buffer reads zeros). One copy for every
 * caller -- full path, live compositor, trigger marker, math trace -- so a
 * change of renderer can never move or re-phase the trace. The mapping is
 * view_plan()'s: the C5 integer window, or (L == 0) the general ratio of a
 * held record about the trigger column, with the blank span recorded. */
static void view_build(const scope_state_t *ss, const volatile uint8_t *b1,
                       const volatile uint8_t *b2)
{
    const volatile uint8_t *src =
        (ss->trigger.source == TRIG_SRC_CH2 && b2) ? b2 : (b1 ? b1 : b2);
    const int32_t N = (int32_t)FPGA_ADC_BUF_SIZE;
    uint32_t r = PV_RATIO_ONE;
    const uint8_t L = view_plan(ss, &r, NULL);
    g_view_first = 0;
    g_view_end = SCOPE_GEOM_COLS;
    if (L != 0u) {
        (void)scope_soft_trigger_offset(ss, src, L, NULL);
        scope_geom_fill(b1, N, &g_view, yt_cols[0], SCOPE_GEOM_COLS);
        scope_geom_fill(b2, N, &g_view, yt_cols[1], SCOPE_GEOM_COLS);
        return;
    }
    /* General (C14): find and place the trigger as the one-sample-per-column
     * window would, then keep that column (the trigger's, else the centre)
     * on that sample -- on the crossing itself, to 1/1000 sample, when
     * zoomed in -- and scale time about it. */
    int16_t back = 0;
    (void)scope_soft_trigger_offset(ss, src, 1u, &back);
    const int32_t pc = (g_trig_x_actual >= 0) ? g_trig_x_actual : SCOPE_GEOM_CENTER_COL;
    int32_t pos0 = (g_view.u0 + pc) * 1000 - pc * (int32_t)r;
    if (r < PV_RATIO_ONE && g_trig_x_actual >= 0)
        pos0 -= ((int32_t)back * 1000 + 128) >> 8;
    uint16_t f = 0, e = 0;
    /* The span from a channel that has a buffer (both: same record length,
     * same span; a NULL buffer reads blank throughout). */
    pv_fill(b1, 0, N, pos0, r, yt_cols[0], SCOPE_GEOM_COLS, b1 ? &f : NULL, b1 ? &e : NULL);
    pv_fill(b2, 0, N, pos0, r, yt_cols[1], SCOPE_GEOM_COLS, b1 ? NULL : &f, b1 ? NULL : &e);
    g_view_first = f;
    g_view_end = e;
    g_view.L = 0u;              /* not one column per sample: no mask overlay */
}

int16_t scope_ui_trig_x_actual(void) { return g_trig_x_actual; }
/* The plan's L: the integer zoom, 0 on the general held-record path. */
uint8_t scope_ui_view_factor(void)
{
    return view_plan(scope_state_get(), NULL, NULL);
}
uint8_t scope_ui_trig_anchor(void)   { return g_trig_anchor; }

/* Exported for bench validation (`spi3 frame`): the offset the renderer would
 * start the drawn window at for the CURRENT scope state. Exporting the real
 * function rather than letting the host re-implement it is deliberate — a
 * host replica passing while the on-screen algorithm fails is exactly the
 * class of stable-wrong-number this project keeps paying for. On the general
 * held-record path (L 0) it is the one-sample-per-column window the pivot is
 * taken from. */
uint16_t scope_ui_soft_trigger_offset(const volatile uint8_t *src_buf)
{
    const scope_state_t *ss = scope_state_get();
    const uint8_t L = view_plan(ss, NULL, NULL);
    return scope_soft_trigger_offset(ss, src_buf, L ? L : 1u, NULL);
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
 * Cursor measurement drawing
 * ═══════════════════════════════════════════════════════════════════ */

/* The vertical transform of the band that contains BOTH rows, as the
 * renderer last drew it; counts_per_pixel 0 when there is none (see the
 * delta readout in draw_cursors for why a straddle is refused). */
static scope_cursor_vmap_t cursor_vmap_for(uint16_t y_a, uint16_t y_b)
{
    scope_cursor_vmap_t vm;
    memset(&vm, 0, sizeof(vm));
    if (math_enabled && g_math_cur.on) {
        /* Volts (or counts) on the math scale about the graticule centre;
         * any other math unit leaves counts_per_pixel 0 -> pixels. */
        vm.channel   = 0u;                 /* named "m" by draw_cursors */
        vm.mid_known = 1u;
        vm.mid_row   = (int16_t)SCOPE_GRID_MID_Y;
        vm.direct         = g_math_cur.direct;
        vm.direct_conf    = g_math_cur.conf;
        vm.unit_per_pixel = g_math_cur.unit_per_px;
        return vm;
    }
    /* Overlaid true-scale bands both contain the cursors: the active
     * channel's is tried first. */
    const uint8_t first = (g_vband[1].on &&
                           g_vband[1].channel == (active_channel ? 2u : 1u)) ? 1u : 0u;
    for (uint8_t k = 0u; k < 2u; k++) {
        const vband_t *b = &g_vband[k ^ first];
        if (!b->on) continue;
        if ((int16_t)y_a < b->y_top || (int16_t)y_a > b->y_bot) continue;
        if ((int16_t)y_b < b->y_top || (int16_t)y_b > b->y_bot) continue;
        vm.counts_per_pixel = b->counts_per_pixel;
        vm.channel          = b->channel;
        vm.range_idx        = b->range_idx;
        vm.mid_known        = b->mid_known ? 1u : 0u;
        vm.mid_row          = b->mid_row;
        vm.mid_counts       = b->mid_counts;
        break;
    }
    return vm;
}

/* Shell/bench view of the cursors (`fpga scope cursor`): X1 X2 X1-X2 Y1 Y2
 * Y1-Y2 and 1/|dX|, from the SAME functions the screen uses. Strings are the
 * formatted readings, "--" where the instrument refuses. */
void scope_ui_cursor_report(char x1[16], char x2[16], char dx[16], char ix[16],
                            char y1[16], char y2[16], char dy[16], float *fs_out)
{
    const scope_state_t *ss = scope_state_get();
    const cursor_state_t *c = &ss->cursor;
    const float fs = column_rate_for_display(ss);   /* columns, not samples */
    const scope_cursor_conf_t cf = scope_cursor_tb_conf(ss->timebase_idx);
    const int16_t ta = scope_ui_trig_x_actual();
    const int32_t ref = (ta >= 0) ? (int32_t)ta : (int32_t)ss->trig_x;
    /* The readouts carry stock's labels (0x0801CA7C: X1-X2, Y1-Y2), so they
     * are FIRST minus SECOND: the on-screen dt/dV are second minus first
     * (v2-v1, and h1-h2 rows = Y2-Y1) and read with the opposite sign. */
    const int32_t ddx = (int32_t)c->v1_x - (int32_t)c->v2_x;
    scope_cursor_reading_t r;

    r = scope_cursor_x_fs(fs, cf, c->v1_x, ref);      scope_cursor_format(&r, x1, 16);
    r = scope_cursor_x_fs(fs, cf, c->v2_x, ref);      scope_cursor_format(&r, x2, 16);
    r = scope_cursor_delta_t_fs(fs, cf, ddx);         scope_cursor_format(&r, dx, 16);
    r = scope_cursor_one_over_dt_fs(fs, cf, ddx);     scope_cursor_format(&r, ix, 16);

    scope_cursor_vmap_t vm = cursor_vmap_for(c->h1_y, c->h2_y);
    r = scope_cursor_y(&vm, c->h1_y);                 scope_cursor_format(&r, y1, 16);
    r = scope_cursor_y(&vm, c->h2_y);                 scope_cursor_format(&r, y2, 16);
    r = scope_cursor_delta_v(&vm, (int32_t)c->h2_y - (int32_t)c->h1_y);   /* Y1-Y2 */
    scope_cursor_format(&r, dy, 16);
    if (fs_out) *fs_out = fs;
}

/* The cursor layer, as both renderers draw it: dashed lines over the live
 * band (the selected one solid) and their V1/V2/H1/H2 labels, transparent
 * text. Built once per frame; cursor_layer_px() is the compositor's form. */
typedef struct {
    bool     v, h;
    int16_t  vx[2], hy[2];
    uint16_t vc[2], hc[2];
    bool     vsolid[2], hsolid[2];
    uint8_t  nlab;
    struct { int16_t x, y, w; char s[3]; uint16_t c; } lab[4];
} cur_layer_t;

static void cursor_layer_build(const scope_state_t *ss, const theme_t *th, cur_layer_t *L)
{
    memset(L, 0, sizeof(*L));
    const cursor_state_t *c = &ss->cursor;
    if (c->mode == CURSOR_OFF) return;
    if (c->mode == CURSOR_VERTICAL || c->mode == CURSOR_BOTH) {
        L->v = true;
        for (uint8_t i = 0; i < 2u; i++) {
            const bool act = (c->active == (i ? CURSOR_SEL_V2 : CURSOR_SEL_V1));
            L->vx[i] = (int16_t)(i ? c->v2_x : c->v1_x);
            L->vc[i] = act ? th->highlight : th->text_secondary;
            L->vsolid[i] = act;
            typeof(L->lab[0]) *t = &L->lab[L->nlab++];
            t->s[0] = 'V'; t->s[1] = (char)('1' + i); t->s[2] = '\0';
            t->w = (int16_t)font_string_width(t->s, &font_small);
            t->x = (int16_t)(L->vx[i] > 12 ? L->vx[i] - 10 : 0);
            t->y = SCOPE_TOP;
            t->c = L->vc[i];
        }
    }
    if (c->mode == CURSOR_HORIZONTAL || c->mode == CURSOR_BOTH) {
        L->h = true;
        for (uint8_t i = 0; i < 2u; i++) {
            const bool act = (c->active == (i ? CURSOR_SEL_H2 : CURSOR_SEL_H1));
            L->hy[i] = (int16_t)(i ? c->h2_y : c->h1_y);
            L->hc[i] = act ? th->highlight : th->text_secondary;
            L->hsolid[i] = act;
            typeof(L->lab[0]) *t = &L->lab[L->nlab++];
            t->s[0] = 'H'; t->s[1] = (char)('1' + i); t->s[2] = '\0';
            t->w = (int16_t)font_string_width(t->s, &font_small);
            t->x = (int16_t)(LCD_WIDTH - 1 - t->w);
            t->y = (int16_t)(L->hy[i] > SCOPE_TOP + 2 ? L->hy[i] - 13 : L->hy[i] + 2);
            t->c = L->hc[i];
        }
    }
}

/* Labels over lines, as drawn. */
static inline bool cursor_layer_px(const cur_layer_t *L, int x, int y, uint16_t *c)
{
    for (uint8_t k = 0; k < L->nlab; k++) {
        const int dx = x - L->lab[k].x, dy = y - L->lab[k].y;
        if (dx >= 0 && dx < L->lab[k].w && dy >= 0 && dy < (int)font_small.height &&
            font_text_px(L->lab[k].s, &font_small, dx, dy)) {
            *c = L->lab[k].c;
            return true;
        }
    }
    for (uint8_t i = 0; i < 2u; i++) {
        if (L->v && x == L->vx[i] && (L->vsolid[i] || (y & 7) < 4)) { *c = L->vc[i]; return true; }
        if (L->h && y == L->hy[i] && (L->hsolid[i] || (x & 7) < 4)) { *c = L->hc[i]; return true; }
    }
    return false;
}

/* Full path: the same layer, painted over the live band [SCOPE_TOP, band_bot). */
static void draw_cursors(uint16_t band_bot)
{
    cur_layer_t L;
    cursor_layer_build(scope_state_get(), theme_get(), &L);
    if (!L.v && !L.h) return;
    for (uint8_t i = 0; i < 2u; i++) {
        if (L.v && L.vx[i] >= 0 && L.vx[i] < LCD_WIDTH)
            for (uint16_t y = SCOPE_TOP; y < band_bot; y++)
                if (L.vsolid[i] || (y & 7u) < 4u) lcd_set_pixel((uint16_t)L.vx[i], y, L.vc[i]);
        if (L.h && L.hy[i] >= SCOPE_TOP && L.hy[i] < (int16_t)band_bot)
            for (uint16_t x = 0; x < LCD_WIDTH; x++)
                if (L.hsolid[i] || (x & 7u) < 4u) lcd_set_pixel(x, (uint16_t)L.hy[i], L.hc[i]);
    }
    for (uint8_t k = 0; k < L.nlab; k++)
        if (L.lab[k].y >= SCOPE_TOP && L.lab[k].y + (int)font_small.height <= (int)band_bot)
            font_draw_string((uint16_t)L.lab[k].x, (uint16_t)L.lab[k].y, L.lab[k].s,
                             L.lab[k].c, L.lab[k].c, &font_small);
}

/*
 * The cursor readout: one line in the strip's AUX row (strip_aux), off the
 * trace and off the badges, repainted only when its text changes.
 *
 * Nothing here converts anything itself. The pixel deltas are turned into
 * real units by scope_cursor.c, which reads the SAME measured tables the
 * status bar and the measurement badges read (scope_timebase.c for the rate,
 * scope_cal.c for the gain) and refuses to name a unit without an entry.
 *   horizontal: dx columns at the column rate (record rate x zoom, C5/C14);
 *   vertical:   the counts per row the renderer recorded for the band that
 *               holds both cursors (vband_note), or the math trace's scale.
 */
static void cursor_readout(const scope_state_t *ss, char *out, size_t n)
{
    const cursor_state_t *c = &ss->cursor;
    char buf[24];
    size_t k = 0;
    out[0] = '\0';
    if (c->mode == CURSOR_OFF) return;
    k += (size_t)snprintf(out + k, n - k, "%s",
                          c->mode == CURSOR_VERTICAL ? "CUR:T" :
                          c->mode == CURSOR_HORIZONTAL ? "CUR:V" : "CUR:TV");
    if (k < n && (c->mode == CURSOR_VERTICAL || c->mode == CURSOR_BOTH)) {
        const int32_t dx = (int32_t)c->v2_x - (int32_t)c->v1_x;
        const float cfs = column_rate_for_display(ss);
        const scope_cursor_conf_t cconf = scope_cursor_tb_conf(ss->timebase_idx);
        scope_cursor_reading_t r = scope_cursor_delta_t_fs(cfs, cconf, dx);
        scope_cursor_format(&r, buf, sizeof(buf));
        k += (size_t)snprintf(out + k, n - k, "  dt=%s", buf);
        if (k < n) {
            r = scope_cursor_one_over_dt_fs(cfs, cconf, dx);
            scope_cursor_format(&r, buf, sizeof(buf));
            k += (size_t)snprintf(out + k, n - k, "  1/dt=%s", buf);
        }
    }
    if (k < n && (c->mode == CURSOR_HORIZONTAL || c->mode == CURSOR_BOTH)) {
        /* Second minus first, as dt; screen y grows downward. A delta that
         * straddles two differently-scaled bands stays in pixels. */
        const int32_t dy = (int32_t)c->h1_y - (int32_t)c->h2_y;
        scope_cursor_vmap_t vm = cursor_vmap_for(c->h1_y, c->h2_y);
        scope_cursor_reading_t dv = scope_cursor_delta_v(&vm, dy);
        scope_cursor_format(&dv, buf, sizeof(buf));
        if (dv.unit == SCOPE_CURSOR_UNIT_VOLTS && vm.direct != SCOPE_CURSOR_DIRECT_NONE)
            (void)snprintf(out + k, n - k, "  dVm=%s", buf);
        else if (dv.unit == SCOPE_CURSOR_UNIT_VOLTS)
            (void)snprintf(out + k, n - k, "  dV%u=%s", vm.channel, buf);
        else
            (void)snprintf(out + k, n - k, "  dV=%s", buf);
    }
}

static uint32_t g_cur_row_hash;   /* hash of the text the AUX row shows now */

static void cursor_row_paint(const scope_state_t *ss, bool force)
{
    char t[64];
    cursor_readout(ss, t, sizeof(t));
    uint32_t h = 2166136261u;                         /* FNV-1a */
    for (const char *p = t; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    if (!force && h == g_cur_row_hash) return;
    g_cur_row_hash = h;
    const theme_t *th = theme_get();
    const uint16_t y = strip_top(ss);                 /* the AUX row */
    lcd_fill_rect(0, y, LCD_WIDTH, MSTRIP_ROW_H, th->background);
    font_draw_string(4, (uint16_t)(y + 1), t, th->highlight, th->background, &font_small);
}

/* Main loop, every tick in the time view: the row follows the cursors and
 * the data (dV depends on the drawn scale) without a full repaint. */
void scope_ui_cursor_row_refresh(void)
{
    const scope_state_t *ss = scope_state_get();
    if (ss->cursor.mode == CURSOR_OFF || can_scope_screen_on() || ss->roll) return;
    cursor_row_paint(ss, false);
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
static void persist_frame_decay(void)
{
    const scope_state_t *gss = scope_state_get();
#ifdef FEATURE_FFT
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
static uint16_t persist_base(const scope_state_t *ss, const theme_t *th, uint16_t y)
{
    if (!(ss->ch1.enabled && ss->ch2.enabled))
        return ss->ch1.enabled ? th->ch1 : th->ch2;
    const bool split = yt_vmap_split(true, true,
        ss->true_scale && scope_cal_true_scale_ok(1u, ss->ch1.vdiv_idx),
        ss->true_scale && scope_cal_true_scale_ok(2u, ss->ch2.vdiv_idx));
    if (!split) return th->text_secondary;
    return (y < SCOPE_MID_Y) ? th->ch1 : th->ch2;
}

static void draw_persistence_overlay(void)
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

/* ═══════════════════════════════════════════════════════════════════
 * Math channel overlay (C12: stock's 8 ops on the real records)
 *
 * Reads the same 301-column window the traces were drawn from (yt_cols,
 * placed by view_build: trigger offset and, at the fast codes, the zoom). Volts where the range is calibrated
 * (scope_cal gain, 0 V = scope_measure_zero_for), counts where not. Drawn
 * about the graticule centre on its OWN scale (a 1-2-5 step per division
 * chosen from the result, shown in the label): A*B and A/B have no common
 * graticule with A and B.
 * ═══════════════════════════════════════════════════════════════════ */

/* One frame of the math trace, shared by the full path and the live frame:
 * the inputs, the 1-2-5 scale chosen from this window, and the label. */
typedef struct {
    bool       ok;
    math_op_t  op;
    math_in_t  in;
    float      step;
    char       label[24];
} math_frame_t;

static void math_prepare(const scope_state_t *ss, math_frame_t *mf)
{
    memset(mf, 0, sizeof(*mf));
    mf->op = ((math_op_t)math_op < MATH_COUNT) ? (math_op_t)math_op : MATH_ADD;
    const math_op_t op = mf->op;
    const volatile uint8_t *s1 = fpga_get_ch1_buf();
    const volatile uint8_t *s2 = fpga_get_ch2_buf();
    mf->ok = g_math_live
        && (!math_op_uses_a(op) || (ss->ch1.enabled && s1 != NULL))
        && (!math_op_uses_b(op) || (ss->ch2.enabled && s2 != NULL));
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
static int16_t math_row(const math_frame_t *mf, uint16_t x)
{
    const uint8_t a = yt_cols[0][x], b = yt_cols[1][x];
    float v;
    int16_t row;
    math_compute(mf->op, &a, &b, 1u, &mf->in, &v);
    math_to_rows(&v, 1u, mf->step, SCOPE_DIV_PX, SCOPE_GRID_MID_Y, SCOPE_TOP, SCOPE_BOT, &row);
    return row;
}

#define MATH_LABEL_X  130u

static void draw_math_waveform(void)
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

/* ═══════════════════════════════════════════════════════════════════
 * Main scope screen compositor
 * ═══════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════
 * SPI3 Debug Overlay
 * Shows raw acquisition status for diagnosing scope data pipeline.
 * Off by default. Enable with -DSCOPE_DEBUG_OVERLAY at build time.
 * ═══════════════════════════════════════════════════════════════════ */

#ifdef SCOPE_DEBUG_OVERLAY

/* SCOPE_DBG_Y is defined with the layout constants at the top of this file,
 * because the badge rows must be positioned relative to it. */
#define SCOPE_DBG_H   58

/*
 * GPIO pin scanner — REMOVED 2026-08-13.
 *
 * It accumulated per-port "this pin has toggled since boot" masks
 * (gpio_toggle_a..e) every frame, and NOTHING read them: the overlay line
 * that printed them was replaced during the config-entry experiments, and
 * the two derived counters (pb3/pb4_toggle_count) were assigned and never
 * used, which is why they showed up as unused statics in the build. An
 * instrument with no readout is not an instrument; this file has enough
 * history of numbers that could not mean what they appeared to mean.
 *
 * It is in git if it is ever wanted again: `git log -S gpio_toggle_a --
 * firmware/src/ui/scope_ui.c` (added in 5b7437a, last read before 134fa83).
 * Note the in-context MISO finder it once carried was already deleted in
 * 2026-04-06 because it drove SPI3 from the display task and corrupted the
 * acquisition task's transfers.
 */

/* Set by draw_scope_screen (whose full-area clear wipes 180-224) so the strip
 * background is refilled exactly when needed; the live incremental path never
 * dirties it, so per-frame text updates go straight over their own opaque
 * cells with no blank state — that blank-then-text sequence at frame rate was
 * half of the visible full-screen flashing. */
static uint8_t scope_dbg_strip_dirty = 1;

/* Pad to a fixed width so opaque redraw erases a previously-longer line
 * (counters shrink after a reset; P0 flips H/L width-stable but SS does not). */
static void dbg_pad(char *s, unsigned cap, unsigned width)
{
    unsigned l = 0;
    while (s[l] != '\0') l++;
    while (l < width && l + 1 < cap) s[l++] = ' ';
    s[l] = '\0';
}

static void draw_scope_debug(const theme_t *th)
{
    /* Dark background strip */
#if (defined(FPGA_PIN_SWEEP_BUILD) && FPGA_PIN_SWEEP_BUILD) || \
    (defined(FPGA_CFG_TRACE_BUILD) && FPGA_CFG_TRACE_BUILD) || \
    (defined(FPGA_IDCODE_PROBE) && FPGA_IDCODE_PROBE)
    /* Variant overlays draw variable-length content — keep the full refill. */
    lcd_fill_rect(0, SCOPE_DBG_Y, LCD_WIDTH, SCOPE_DBG_H, 0x0000);
#else
    if (scope_dbg_strip_dirty) {
        lcd_fill_rect(0, SCOPE_DBG_Y, LCD_WIDTH, SCOPE_DBG_H, 0x0000);
        scope_dbg_strip_dirty = 0;
    }
#endif

    char buf[64];

#if defined(FPGA_PIN_SWEEP_BUILD) && FPGA_PIN_SWEEP_BUILD
    /* ── RECONFIG_N pin sweep results ───────────────────────────────────────
     * Press SAVE in scope mode to run it. Read HIT first: it is the only line
     * that can say "we found something".
     *   HIT:-- H:0 AF:0  -> every candidate pulsed, nothing moved the STATUS
     *                       register. No MCU-owned pin stock drives is RECONFIG_N.
     *   HIT:PCn          -> that pin changed the status after CONFIG_ENABLE.
     *   AF non-zero      -> that many candidates failed the IDCODE anchor and
     *                       were DISCARDED, not counted as hits. Worth a look
     *                       anyway: a pin that CLOSES the config port would also
     *                       land here (Exp L behaviour). */
    {
        const char *st = (fpga.sweep_state == 0) ? "IDLE" :
                         (fpga.sweep_state == 1) ? "RUN"  : "DONE";
        snprintf(buf, sizeof(buf), "SWEEP:%s %u/%u", st,
                 (unsigned)fpga.sweep_tested, (unsigned)fpga.sweep_total);
        font_draw_string(2, SCOPE_DBG_Y + 2, buf, 0x07E0, 0x0000, &font_small);

        snprintf(buf, sizeof(buf), "BASE:%08lX",
                 (unsigned long)fpga.sweep_baseline);
        font_draw_string(2, SCOPE_DBG_Y + 15, buf, 0x07FF, 0x0000, &font_small);

        /* PH distinguishes two very different findings, which Exp O's single
         * snapshot could not tell apart:
         *   P = the PULSE ALONE moved the status — the pin triggered something
         *       on its own, which is what a real RECONFIG_N does (reload from NV).
         *   C = the status only moved after CONFIG_ENABLE — the pulse made 0x15
         *       land, i.e. it unlocked config entry. This is the jackpot. */
        const char *ph = (fpga.sweep_hit_phase == 1) ? "P" :
                         (fpga.sweep_hit_phase == 2) ? "C" : "-";
        snprintf(buf, sizeof(buf), "HIT:%s/%s %08lX",
                 fpga_sweep_pin_name(fpga.sweep_first_hit), ph,
                 (unsigned long)fpga.sweep_hit_status);
        font_draw_string(2, SCOPE_DBG_Y + 28, buf, 0xFFE0, 0x0000, &font_small);

        snprintf(buf, sizeof(buf), "H:%u AF:%u  press SAVE",
                 (unsigned)fpga.sweep_hits, (unsigned)fpga.sweep_anchor_fail);
        font_draw_string(2, SCOPE_DBG_Y + 41, buf, 0xF81F, 0x0000, &font_small);
    }
    return;
#endif

#if defined(FPGA_CFG_TRACE_BUILD) && FPGA_CFG_TRACE_BUILD
    /* ── Step-resolved anchored status trace ────────────────────────────────
     * Six checkpoints through the config sequence, each anchored on the IDCODE.
     * Checked in this order before drawing any conclusion:
     *
     *   A: line     the anchor at each checkpoint. '0' = IDCODE found aligned,
     *               'x' = anchor FAILED and that Tn is NOT a measurement. An 'x'
     *               appearing partway through is itself the finding: the config
     *               port closed at that step (Exp L behaviour).
     *   T0..T5      anchor-corrected STATUS. FFFFFFFF means "not measured".
     *
     * All six identical => the part ignores every config command while still
     * answering reads. Any step where it MOVES is where the sequence first has
     * an effect, and is where to aim next. */
    {
        snprintf(buf, sizeof(buf), "T0:%08lX T1:%08lX",
                 (unsigned long)fpga.cfg_trace[0], (unsigned long)fpga.cfg_trace[1]);
        font_draw_string(2, SCOPE_DBG_Y + 2, buf, 0x07E0, 0x0000, &font_small);

        snprintf(buf, sizeof(buf), "T2:%08lX T3:%08lX",
                 (unsigned long)fpga.cfg_trace[2], (unsigned long)fpga.cfg_trace[3]);
        font_draw_string(2, SCOPE_DBG_Y + 15, buf, 0x07FF, 0x0000, &font_small);

        snprintf(buf, sizeof(buf), "T4:%08lX T5:%08lX",
                 (unsigned long)fpga.cfg_trace[4], (unsigned long)fpga.cfg_trace[5]);
        font_draw_string(2, SCOPE_DBG_Y + 28, buf, 0xFFE0, 0x0000, &font_small);

        char a[FPGA_CFG_TRACE_N + 1];
        for (unsigned i = 0; i < FPGA_CFG_TRACE_N; i++) {
            int8_t o = fpga.cfg_trace_anchor[i];
            a[i] = (o < 0) ? 'x' : (char)('0' + (o % 10));
        }
        a[FPGA_CFG_TRACE_N] = '\0';
        /* MV = the count of checkpoints whose status differs from T0. 0 means the
         * register never moved anywhere in the sequence. */
        unsigned moved = 0;
        for (unsigned i = 1; i < FPGA_CFG_TRACE_N; i++)
            if (fpga.cfg_trace[i] != fpga.cfg_trace[0]) moved++;
        snprintf(buf, sizeof(buf), "A:%s MV:%u H2:%c", a, moved,
                 fpga.h2_upload_done ? 'Y' : 'N');
        font_draw_string(2, SCOPE_DBG_Y + 41, buf, 0xF81F, 0x0000, &font_small);
    }
    return;
#endif

#if defined(FPGA_IDCODE_PROBE) && FPGA_IDCODE_PROBE
    /* ── Experiment J overlay: the anchored opcode-discrimination probe ──────
     * Replaces the normal overlay entirely for this build. Four opcodes read at
     * /256 on a pristine bus before the prelude, 8 bytes each.
     *
     * READ IT LIKE THIS:
     *   ID / NP identical, and each showing a repeated 4-byte group
     *     => the FPGA free-runs a fixed pattern and ignores MOSI. Confirms the
     *        2026-06-13 conclusion on a valid measurement for the first time;
     *        SSPI cannot reach the config engine and JTAG is the route.
     *   ID differs from NP, ID@ >= 0
     *     => the FPGA DOES decode SSPI opcodes. The "not in config-receive mode"
     *        conclusion collapses and the config-entry search reopens.
     *   ID@ >= 0 but not 0
     *     => IDCODE present but phase-shifted by that many bits; the reply is
     *        real and our sampling alignment is off (see Exp I).
     */
    {
        const volatile uint8_t *p;

        p = fpga.probe_idcode;
        snprintf(buf, sizeof(buf), "ID:%02X%02X%02X%02X %02X%02X%02X%02X",
                 p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        font_draw_string(2, SCOPE_DBG_Y + 2, buf, 0x07E0, 0x0000, &font_small);

        p = fpga.probe_noop;
        snprintf(buf, sizeof(buf), "NP:%02X%02X%02X%02X %02X%02X%02X%02X",
                 p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        font_draw_string(2, SCOPE_DBG_Y + 15, buf, 0x07FF, 0x0000, &font_small);

        snprintf(buf, sizeof(buf), "ST:%02X%02X%02X%02X US:%02X%02X%02X%02X",
                 fpga.probe_status[0], fpga.probe_status[1],
                 fpga.probe_status[2], fpga.probe_status[3],
                 fpga.probe_user[0], fpga.probe_user[1],
                 fpga.probe_user[2], fpga.probe_user[3]);
        font_draw_string(2, SCOPE_DBG_Y + 28, buf, 0xFFE0, 0x0000, &font_small);

        /* ID@ = bit offset where 0x0120681B was found, -1 = absent.
         * SM  = 0x11 and 0x00 returned identical bytes (Y => no opcode decode).
         * RP  = the 0x11 reply repeats every 4 bytes (Y => free-running pattern).
         * PO@ = same IDCODE search on the 0x11 read taken AFTER CONFIG_ENABLE.
         * CL@ = same again AFTER the full upload and the 0x3A close. Per Exp L a
         *       configured part stops answering 0x11, so CL@ >= 0 means the config
         *       port never closed => we are definitively NOT configured. */
        snprintf(buf, sizeof(buf), "ID@%d SM:%c RP:%c PO@%d CL@%d",
                 (int)fpga.probe_id_bit,
                 fpga.probe_all_same ? 'Y' : 'N',
                 fpga.probe_repeats  ? 'Y' : 'N',
                 (int)fpga.probe_id_bit_post,
                 (int)fpga.probe_id_bit_close);
        font_draw_string(2, SCOPE_DBG_Y + 41, buf, 0xF81F, 0x0000, &font_small);
    }
    return;
#endif

    /* Line 1 (green): transport counters from the real FPGA path */
    /* Line 1 (green): post-upload scope-engine config readback.
     * SS = fpga.scope_status[] from the 0x03 status read — stock capture
     * returns 00 01 42 2E. CL = 0x3A close status — stock returns F8.
     * R  = first 4 raw CH1 bytes (pre-cal), to see if samples vary at all. */
    snprintf(buf, sizeof(buf), "SS:%02X%02X%02X%02X CL:%02X R:%02X%02X%02X%02X",
             fpga.scope_status[0], fpga.scope_status[1],
             fpga.scope_status[2], fpga.scope_status[3],
             fpga.h2_close_status,
             fpga.diag_ch1_raw[0], fpga.diag_ch1_raw[1],
             fpga.diag_ch1_raw[2], fpga.diag_ch1_raw[3]);
    dbg_pad(buf, sizeof(buf), 38);
    font_draw_string(2, SCOPE_DBG_Y + 2, buf,
                     0x07E0, 0x0000, &font_small);  /* green */

    /* Line 2 (cyan): acquisition state and critical control pins */
    snprintf(buf, sizeof(buf), "OK:%u TO:%u 1:%02X P0:%c P6:%c B:%c",
             (unsigned)fpga.spi3_ok_count,
             (unsigned)fpga.spi3_total_timeouts,
             fpga.spi3_first_byte,
             (GPIOC->idt & (1 << 0)) ? 'H' : 'L',
             (GPIOC->odt & (1 << 6)) ? 'H' : 'L',
             (GPIOB->odt & (1 << 11)) ? 'H' : 'L');
    dbg_pad(buf, sizeof(buf), 38);
    font_draw_string(2, SCOPE_DBG_Y + 15, buf,
                     0x07FF, 0x0000, &font_small);  /* cyan */

    /* Line 3 (yellow): Init handshake responses + raw PB4 state
     * G1[0-3] = sync+0x05+pad, G2[4-6] = 0x12+pad, G3[7-10] = 0x15+pad+0x3B
     * PB4: raw GPIO read of the MISO pin (1=HIGH/floating, 0=driven LOW) */
    /* L/H = min/max across the whole CH1 acquisition buffer. If L==H the
     * samples are a stuck constant (analog/trigger problem); if they spread,
     * real signal is arriving and the fault is downstream in rendering. */
    {
        const volatile uint8_t *cb = fpga_get_ch1_buf();
        uint8_t lo = 0xFF, hi = 0x00;
        for (uint32_t i = 0; i < FPGA_ADC_BUF_SIZE; i++) {
            uint8_t v = cb[i];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        /* CFG = Gowin STATUS_REGISTER (opcode 0x41) read right after the 0x3A
         * close — the authoritative "did the upload take?" verdict. Read at a
         * forced /256 since 2026-07-28; every earlier value was clocked at /2
         * and slipped one bit (the persistent 80 01 C8 10 is 0x00039020 read
         * one bit early — see fpga.c [6b]).
         *
         * D = DONE_FINAL (bit 13) — 1 means configuration completed. This, not
         *     SYSTEM_EDIT_MODE, is the success signal (Exp H showed stock reads
         *     the same 0x00039020 at the config-enable instant that we do, so
         *     ED cannot discriminate).
         * E = the error nibble, bits 3..0 = TIMEOUT | ID_VERIFY_FAILED |
         *     BAD_COMMAND | CRC_ERROR. Non-zero means bytes DID reach the config
         *     engine and were rejected → a content/transport bug, localizable.
         *     Zero with D=0 means the engine never received anything at all. */
        uint32_t sr = ((uint32_t)fpga.cfg_status_reg[0] << 24) |
                      ((uint32_t)fpga.cfg_status_reg[1] << 16) |
                      ((uint32_t)fpga.cfg_status_reg[2] << 8)  |
                      ((uint32_t)fpga.cfg_status_reg[3]);
        /* A = the post-0x3A IDCODE anchor: the bit offset at which 0x0120681B was
         *     found in the read taken immediately before CFG, at the same clock.
         *     A>=0  -> CFG was read on a validated path AND the config port is
         *             still open, i.e. we are definitively NOT configured.
         *     A=-1  -> either the port closed (Exp L: what a successfully
         *             configured part does) or the bus is dead. CFG is then
         *             UNTRUSTWORTHY on its own — corroborate with a live trace. */
        snprintf(buf, sizeof(buf), "CFG:%08lX D%u E%X A%d L%u H%u",
                 (unsigned long)sr,
                 (unsigned)((sr >> 13) & 1u),
                 (unsigned)(sr & 0x0Fu),
                 (int)fpga.probe_id_bit_close,
                 (unsigned)lo, (unsigned)hi);
    }
    dbg_pad(buf, sizeof(buf), 38);
    font_draw_string(2, SCOPE_DBG_Y + 28, buf,
                     0xFFE0, 0x0000, &font_small);  /* yellow */

    /* Line 4 (magenta): SPI3 CTRL1 + the EDIT_MODE probe + H2 upload flag.
     * S1 = SPI3 CTRL1 as latched during the config frame. Bits[5:3] are BR, so
     *      0347 = /2 (stock, Exp F fidelity build) and 037F = /256. This is the
     *      on-device proof of which clock the config frame actually ran at.
     * ED = STATUS_REGISTER (0x41) read at /256 immediately after 0x15.
     *      SYSTEM_EDIT_MODE is bit 7 of the ASSEMBLED 32-bit word, i.e. bit 7 of
     *      ED[3] — NOT bit 7 of ED[0], as this comment and scripts/swd_fpga_status.sh
     *      both said until 2026-07-28. (Same verdict either way for 0x00039020:
     *      ED[3]=0x20, bit 7 clear. But the test was reading bit 31.)
     *      Exp H demoted this from "the wall" to a non-discriminator: stock reads
     *      the same 0x00039020 here and still configures. Compare CFG's D flag.
     * Dropped S2/ST here — CTRL2 and STS were static (0003 / 0002) across every
     * run so far, and ED is the number this experiment turns on.
     * H2T means bytes streamed, not recovered FPGA acceptance. */
    snprintf(buf, sizeof(buf), "S1:%04X ED:%02X%02X%02X%02X H2T:%c",
             (uint16_t)fpga.diag_spi_ctrl1,
             fpga.edit_mode_status[0], fpga.edit_mode_status[1],
             fpga.edit_mode_status[2], fpga.edit_mode_status[3],
             fpga.h2_upload_done ? 'Y' : 'N');
    dbg_pad(buf, sizeof(buf), 38);
    font_draw_string(2, SCOPE_DBG_Y + 41, buf,
                     0xF81F, 0x0000, &font_small);  /* magenta */
}

#endif /* SCOPE_DEBUG_OVERLAY */

/* ═══════════════════════════════════════════════════════════════════
 * X-Y (Lissajous) view — CH1 drives X, CH2 drives Y
 *
 * Plots the two ADC buffers against each other instead of against time:
 * op04 (CH1) -> horizontal, op05 (CH2) -> vertical. The buffers come from
 * one synchronised acquisition and were bench-confirmed time-aligned
 * (~20 deg phase spread, EXP-21 2026-08-21), so (ch1[i], ch2[i]) is a
 * single instant and a Lissajous figure renders coherently.
 *
 * CH2's readout is a dead buffer until its vertical-offset reference
 * (TMR13 CH1 PWM-DAC on PA6) is armed — the same thing `trig2 raw` does
 * from the shell — so this view arms it once on first entry. Square plot
 * area (side = SCOPE_H) so frequency ratios are not distorted.
 *
 * Reads the volatile buffers directly (no tear-check copy): a frame the
 * acq task is mid-updating just displaces a few points, cosmetically
 * negligible here. Consecutive samples are connected so the locus reads as
 * a continuous trace, the way an analog scope draws X-Y.
 * ═══════════════════════════════════════════════════════════════════ */

/* ── Text for the column compositors ─────────────────────────────────
 * A view that composes each column's final pixels cannot draw text over a
 * finished image, so its labels are a small list it asks per pixel
 * (font_text_px). Opaque labels also own their box (glyph gaps in bg). */
#define CTEXT_MAX  14
#define CTEXT_LEN  24
typedef struct {
    uint8_t n;
    struct {
        int16_t  x, y, w;
        uint16_t fg, bg;
        bool     opaque;
        char     s[CTEXT_LEN];
    } t[CTEXT_MAX];
} ctext_set_t;

static void ctext_add(ctext_set_t *T, int x, int y, const char *s,
                      uint16_t fg, uint16_t bg, bool opaque)
{
    if (T->n >= CTEXT_MAX || s == NULL || s[0] == '\0') return;
    typeof(T->t[0]) *e = &T->t[T->n++];
    strncpy(e->s, s, CTEXT_LEN - 1);
    e->s[CTEXT_LEN - 1] = '\0';
    e->x = (int16_t)x;
    e->y = (int16_t)y;
    e->w = (int16_t)font_string_width(e->s, &font_small);
    e->fg = fg;
    e->bg = bg;
    e->opaque = opaque;
}

static inline bool ctext_px(const ctext_set_t *T, int x, int y, uint16_t *c)
{
    for (uint8_t k = 0; k < T->n; k++) {
        const int dx = x - T->t[k].x, dy = y - T->t[k].y;
        if (dx < 0 || dx >= T->t[k].w || dy < 0 || dy >= (int)font_small.height) continue;
        if (font_text_px(T->t[k].s, &font_small, dx, dy)) { *c = T->t[k].fg; return true; }
        if (T->t[k].opaque) { *c = T->t[k].bg; return true; }
    }
    return false;
}

/* Bresenham segment written into the persistence buffer (buffer coords). */
static void xy_line_persist(int x0, int y0, int x1, int y1)
{
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int err = (dx > dy ? dx : -dy) / 2, e2;
    for (;;) {
        persist_add_point((uint16_t)x0, (uint16_t)y0);
        if (x0 == x1 && y0 == y1) break;
        e2 = err;
        if (e2 > -dx) { err -= dy; x0 += sx; }
        if (e2 <  dy) { err += dx; y0 += sy; }
    }
}

/* One channel's X-Y axis: code -> pixels from the centre, true scale where
 * the range has a volts/div (26 px per division, 0 V at position 0 on the
 * axis, as the Y-T plot), else autoscaled to the record. */
typedef struct { bool fixed; int c0; float px_per_code; } xy_axis_t;

static void xy_axis(const scope_state_t *ss, uint8_t ch, uint8_t mn, uint8_t mx,
                    int half, xy_axis_t *a)
{
    const uint8_t r = ch ? ss->ch2.vdiv_idx : ss->ch1.vdiv_idx;
    const float cpd = scope_cal_codes_per_div((uint8_t)(ch + 1u), r);
    if (ss->true_scale && cpd > 0.0f) {
        a->fixed = true;
        a->c0 = yt_centre_code();
        a->px_per_code = (float)SCOPE_DIV_PX / cpd;
    } else {
        int span = (int)mx - (int)mn;
        if (span < 8) span = 8;
        a->fixed = false;
        a->c0 = ((int)mn + (int)mx) / 2;
        a->px_per_code = (float)(2 * half) / (float)span;
    }
}

void draw_xy_screen(bool full)
{
    const theme_t *th = theme_get();
    const scope_state_t *ss = scope_state_get();
    const uint16_t side = SCOPE_H;                 /* square side, 206 px */
    const uint16_t x0   = (LCD_WIDTH - side) / 2;  /* centred: 57 */
    const uint16_t y0   = SCOPE_TOP;               /* 18 */
    const int cxr = side / 2, cyr = SCOPE_GRID_MID_Y - SCOPE_TOP;   /* centre in the square */

    /* The phosphor trail lives in the shared persistence buffer (X-Y and the
     * time-domain persistence overlay never run at once). Without the
     * calibration page the old bring-up of the offset outputs still runs;
     * with it, the page's offset codes (scope_vpos) are left alone. */
    static bool xy_init = false;
    if (!xy_init || !persist_is_initialized()) {
        if (!scope_vpos_active()) {
            scope_trigger_dac_init();
            scope_trigger_ch2_init();
        }
        if (!persist_is_initialized()) persist_init();
        persist_set_mode(PERSIST_LOW);             /* short phosphor tail */
        persist_clear();
        xy_init = true;
    }

    if (full) {                                    /* margins and title, once */
        lcd_fill_rect(0, SCOPE_TOP, x0, SCOPE_H, th->background);
        lcd_fill_rect((uint16_t)(x0 + side), SCOPE_TOP,
                      (uint16_t)(LCD_WIDTH - (x0 + side)), SCOPE_H, th->background);
        font_draw_string(4, SCOPE_TOP + 2, "X-Y", th->text_secondary, th->background, &font_small);
        font_draw_string(4, SCOPE_TOP + 16, "X=CH1", th->ch1, th->background, &font_small);
        font_draw_string(4, SCOPE_TOP + 30, "Y=CH2", th->ch2, th->background, &font_small);
    }

    const volatile uint8_t *xb = fpga_get_ch1_buf();
    const volatile uint8_t *yb = fpga_get_ch2_buf();
    bool have = fpga_data_ready() && xb != NULL && yb != NULL && persist_is_initialized();

    /* Snapshot buffers on the FreeRTOS heap, not in .bss (#29); allocated on
     * the first frame with data and kept. */
    static uint8_t *xy_xs, *xy_ys;
    if (have && (xy_xs == NULL || xy_ys == NULL)) {
        if (xy_xs == NULL) xy_xs = pvPortMalloc(FPGA_ADC_BUF_SIZE);
        if (xy_ys == NULL) xy_ys = pvPortMalloc(FPGA_ADC_BUF_SIZE);
        if (xy_xs == NULL || xy_ys == NULL) have = false;
    }

    persist_decay();                     /* fade the whole buffer once per frame */

    if (have) {
        uint8_t *const xs = xy_xs;
        uint8_t *const ys = xy_ys;
        for (int t = 0; t < 4; t++) {                /* coherent snapshot */
            uint32_t g0 = fpga_acq_frame_generation();
            memcpy(xs, (const void *)xb, FPGA_ADC_BUF_SIZE);
            memcpy(ys, (const void *)yb, FPGA_ADC_BUF_SIZE);
            if ((g0 & 1u) == 0u && fpga_acq_frame_generation() == g0)
                break;
        }
        uint8_t xmin = 255, xmax = 0, ymin = 255, ymax = 0;
        for (uint16_t i = SCOPE_RECORD_HEAD_SKIP; i < FPGA_ADC_BUF_SIZE; i++) {
            if (xs[i] < xmin) xmin = xs[i];
            if (xs[i] > xmax) xmax = xs[i];
            if (ys[i] < ymin) ymin = ys[i];
            if (ys[i] > ymax) ymax = ys[i];
        }
        const int half = side / 2 - 6;
        xy_axis_t ax, ay;
        xy_axis(ss, 0u, xmin, xmax, half, &ax);
        xy_axis(ss, 1u, ymin, ymax, half, &ay);
        /* Every sample joined to the next: a square's fast edge is a faint
         * line between its two levels, as on an analog X-Y display. */
        int pbx = 0, pby = 0;
        for (uint16_t i = SCOPE_RECORD_HEAD_SKIP; i < FPGA_ADC_BUF_SIZE; i++) {
            int bx = (int)x0 + cxr + (int)lroundf((float)((int)xs[i] - ax.c0) * ax.px_per_code);
            int by = cyr - (int)lroundf((float)((int)ys[i] - ay.c0) * ay.px_per_code);
            if (bx < (int)x0) bx = x0; else if (bx > (int)x0 + side - 1) bx = x0 + side - 1;
            if (by < 0) by = 0; else if (by > side - 1) by = side - 1;
            if (i > SCOPE_RECORD_HEAD_SKIP) xy_line_persist(pbx, pby, bx, by);
            else persist_add_point((uint16_t)bx, (uint16_t)by);
            pbx = bx; pby = by;
        }
    }

    /* Stream the square: the trail over a graticule of 26 px divisions about
     * the centre (0 V at position 0 on both axes in true scale), no blank
     * phase. A "no signal" note is composed into it, not drawn over it. */
    static const char nosig[] = "no signal";
    const int nsx = cxr - 30, nsy = cyr - 16;      /* in the square */
    const uint8_t *pb = persist_get_buffer();
    for (int rx = 0; rx < (int)side; rx++) {
        const int dxc = rx - cxr;
        const bool vline = (dxc % SCOPE_DIV_PX) == 0;
        lcd_set_window((uint16_t)(x0 + rx), y0, 1, side);
        for (int ry = 0; ry < (int)side; ry++) {
            const int dyc = ry - cyr;
            const uint8_t inten = pb ? pb[ry * PERSIST_WIDTH + (int)x0 + rx] : 0;
            uint16_t c;
            if (!have && rx >= nsx && ry >= nsy && ry < nsy + (int)font_small.height &&
                font_text_px(nosig, &font_small, rx - nsx, ry - nsy))
                c = th->warning;
            else if (inten)
                c = persist_intensity_to_color_ch1(inten);
            else if (dxc == 0 || dyc == 0)
                c = th->grid_center;
            else if ((vline && (ry & 1) == 0) || ((dyc % SCOPE_DIV_PX) == 0 && (rx & 1) == 0))
                c = th->grid;
            else
                c = th->background;
            lcd_write_data(c);
        }
    }
}

void draw_scope_screen(uint32_t frame)
{
    const theme_t *th = theme_get();
    const scope_state_t *ss = scope_state_get();

    /* Clear waveform area */
    lcd_fill_rect(0, SCOPE_TOP, LCD_WIDTH, SCOPE_H, th->background);

    /* Layer 1: Persistence overlay (under everything) */
    draw_persistence_overlay();

    /* Layer 2: Grid */
    draw_scope_grid();

    /* The measurement strip's top: the live band, the mask strip and the
     * tags all stop there. */
    const uint16_t band_bot = strip_top(ss);

    /* Layer 3: Trigger level line (under the trace) */
    draw_trigger_line(th, trigger_marker_y_full(ss));

    /* Layer 5: Waveform */
    draw_demo_waveform(frame);

    /* Mask strip at the bottom of the live band (the compositor paints the
     * same rows), only when draw_demo_waveform() armed the overlay. */
    draw_mask_strip(th, band_bot);

    /* Layer 6: Math channel overlay */
    draw_math_waveform();

    /* Layer 6b: plot-edge markers over the traces, as stock: ground tags,
     * trigger tag, and the trigger-position pointer over the column where the
     * trigger point actually landed (not where it was asked to land -- the
     * fallback may not honour it). Same builder and shapes as the compositor. */
    {
        pm_set_t pm;
        if (fpga_data_ready()) {        /* draw_demo_waveform's own gate: yt_cols is this frame's */
            autofit_t a1, a2;
            yt_fits(ss, &a1, &a2);
            markers_build(ss, th, &a1, &a2, band_bot, &pm);
        } else {
            markers_build(ss, th, NULL, NULL, band_bot, &pm);
        }
        draw_plot_markers(&pm, th);
        for (int dy = 0; dy < PM_TPOS_H; dy++)
            for (int x = g_trig_x_actual - PM_TPOS_W / 2; x <= g_trig_x_actual + PM_TPOS_W / 2; x++)
                if (x >= 0 && x < LCD_WIDTH && tpos_px(g_trig_x_actual, x, SCOPE_TOP + dy))
                    lcd_set_pixel((uint16_t)x, (uint16_t)(SCOPE_TOP + dy), th->trigger);
    }

    /* Layer 7: Cursor lines and labels (the readout is in the AUX row) */
    draw_cursors(band_bot);

    /* Layer 8 (the trigger status badge) is gone since the UI round 1 merge:
     * the status bar's trigger dot and RUN/STOP/ARM tab carry it. */

    /* Layer 9: Run/Stop */
    draw_run_stop(ss, th);

    /* Layer 10: Measurement badges */
    draw_measurement_badges(ss, th, false);

    /* Layer 11: Quick-change popup (on top of everything) */
    draw_popup(th);

    /* No "CH1"/"CH2" text top-left any more: the status bar's focus glyph
     * shows the active channel in its colour (UI round 1). */

#if !defined(EMULATOR_BUILD) && defined(SCOPE_DEBUG_OVERLAY)
    /* SPI3 acquisition diagnostic — small text overlay top-center. Developer
     * text: debug-overlay builds only since UI round 1 (it is in the bottom
     * strip of those builds anyway). Shows probing status so we can see
     * FPGA data flow without serial. */
    {
        char spi3_buf[24];
        uint16_t spi3_color;
        if (fpga.spi3_ok_count > 0) {
            snprintf(spi3_buf, sizeof(spi3_buf), "SPI3:OK %u",
                     (unsigned)fpga.spi3_ok_count);
            spi3_color = th->success;
        } else if (fpga.spi3_total_timeouts > 0) {
            snprintf(spi3_buf, sizeof(spi3_buf), "SPI3:-- %u [%02X]",
                     (unsigned)fpga.spi3_total_timeouts,
                     fpga.spi3_first_byte);
            spi3_color = th->warning;
        } else if (fpga.initialized) {
            snprintf(spi3_buf, sizeof(spi3_buf), "SPI3:wait");
            spi3_color = th->text_secondary;
        } else {
            snprintf(spi3_buf, sizeof(spi3_buf), "SPI3:off");
            spi3_color = th->text_secondary;
        }
        font_draw_string(40, SCOPE_TOP + 2, spi3_buf,
                         spi3_color, th->background, &font_small);
    }
#endif

#ifdef SCOPE_DEBUG_OVERLAY
    /* Layer 12: SPI3 debug overlay (bottom of screen) */
    scope_dbg_strip_dirty = 1;   /* the :1050-style full clear wiped 180-224 */
    draw_scope_debug(th);
#endif
}

/* ═══════════════════════════════════════════════════════════════════
 * Live incremental frame (2026-08-12 rendering pass)
 *
 * Flicker-free trace update for the real-data path: repaints the waveform
 * band column-by-column, compositing each pixel's FINAL color (trace over
 * trigger line over crosshair over grid over background) into one streamed
 * window write per column — the screen never holds a blank state, unlike
 * draw_scope_screen's clear-then-redraw, which is the visible flash. Grid
 * pixels are re-plotted arithmetically (there is no framebuffer to restore
 * from; pattern mirrors draw_scope_grid exactly). Cost: one lcd_set_window
 * (~13 us) per column + streamed pixel writes ≈ 6-8 ms per frame — 20 fps
 * capable, vs the full path's blank-flash at any rate.
 *
 * Decorations (the measurement strip, cursors, popups) are NOT drawn here;
 * the plot-edge tags and the trigger-position pointer ARE (markers_build),
 * because this band covers the plot edges they sit on. main.c falls back to draw_scope_screen whenever a popup
 * is active or cursors are on, and button DCMDs still force full redraws,
 * so those layers refresh exactly when they can change.
 * ═══════════════════════════════════════════════════════════════════ */
void draw_scope_live_frame(void)
{
    const theme_t *th = theme_get();
    const scope_state_t *ss = scope_state_get();
    const volatile uint8_t *b1 = fpga_get_ch1_buf();
    const volatile uint8_t *b2 = fpga_get_ch2_buf();

    if (!b1 || !b2) return;

    /* Software trigger — THIS is the per-frame live path, so this is where the
     * free-run "dancing" actually happens. Shift both channels' render window
     * to the trigger source's crossing, using the same helper and offset as the
     * full path (draw_demo_waveform) so the two renderers stay in phase. Both
     * channels shift by the same amount to stay time-aligned. */
    view_build(ss, b1, b2);
    b1 = yt_cols[0];
    b2 = yt_cols[1];
    mask_overlay_begin();          /* per-column mask colours, see draw_mask_strip */
    g_math_live = true;            /* yt_cols is this frame's window */

    /* The live band stops where fixed furniture begins: the debug strip
     * (debug builds) or the measurement badge rows. Those regions repaint
     * themselves; streaming over them would z-fight at frame rate. */
    /* Always the badge rows: they are the topmost fixed furniture in BOTH
     * configurations. In overlay builds the badges now sit ABOVE the debug
     * strip (see STRIP_BOTTOM), so stopping at SCOPE_DBG_Y — as this did until
     * 2026-08-14 — streamed the waveform straight over them at frame rate.
     * That was the second occlusion bug in a row here: first the debug strip
     * painted over the badges, then moving the badges up put them inside the
     * live band. Anchor to the badges and both cases are covered. */
    /* Round 1: the strip's top, which moves with the selection
     * (measure_strip.h); strip_top() is the one copy every user reads. */
    const uint16_t band_bot = strip_top(ss);


    /* The maps exactly as the full path builds them (yt_fits: true scale or
     * autofit, the band split), so a change of renderer never moves the
     * trace. They run against SCOPE_BOT even though this function only
     * PAINTS down to band_bot: the full path draws the trace over that strip
     * and then paints the badges on top, so matching its geometry means
     * matching its y_bot, not its visible extent.
     *
     * No software position term, in either path: the offset DAC already
     * moved the signal in the record. */
    autofit_t a1, a2;
    yt_fits(ss, &a1, &a2);           /* b1/b2 ARE yt_cols: the one prep */

    /* The tags, on the same maps. */
    pm_set_t pm;
    markers_build(ss, th, &a1, &a2, band_bot, &pm);

    /* What this frame drew, for the cursor readout (as draw_channel does). */
    vband_clear();
    {
        uint8_t slot = 0;
        if (a1.on) vband_note(slot++, 1u, ss->ch1.vdiv_idx, &a1, yt_vmap_counts_per_px(&a1));
        if (a2.on) vband_note(slot, 2u, ss->ch2.vdiv_idx, &a2, yt_vmap_counts_per_px(&a2));
    }

    /* Cursors: one more layer of the column composition. */
    cur_layer_t cl;
    cursor_layer_build(ss, th, &cl);
    const bool curs = cl.v || cl.h;

    /* Math trace (C12): the same frame helpers as the full path, one row per
     * column, its label as per-pixel text. */
    math_frame_t mf;
    const bool mth = math_enabled;
    if (mth) math_prepare(ss, &mf);
    const int16_t mlab_w = mth ? (int16_t)font_string_width(mf.label, &font_small) : 0;
    int16_t mp = 0;

    /* Persistence (C12): the ghosts are read from the shared-pool buffer
     * (320 x 206 intensities, not .bss) under the grid, and this frame's
     * traces are added to it column by column after the column is sent,
     * exactly as draw_channel() feeds it in the full path. */
    const uint8_t *pbuf = (persist_enabled && persist_is_initialized()) ? persist_get_buffer() : NULL;
    if (pbuf) persist_frame_decay();
    const uint16_t pbase_top = persist_base(ss, th, SCOPE_TOP);
    const uint16_t pbase_bot = persist_base(ss, th, (uint16_t)(SCOPE_BOT - 1));

    /* Trigger dotted line, placed exactly as the full path places it: on the
     * trigger source's map, at the record code where the hardware fires. */
    int16_t trig_y = trigger_marker_y(ss, (ss->trigger.source == TRIG_SRC_CH2) ? &a2 : &a1);
    if (trig_y < SCOPE_TOP + 2) trig_y = SCOPE_TOP + 2;
    if (trig_y > SCOPE_BOT - 3) trig_y = SCOPE_BOT - 3;
    const uint16_t trig_step = scope_trig_level_focus ? 1u : 3u;   /* dot mask */

    int16_t p1 = 0, p2 = 0;  /* previous column's y — vertical continuity */

    /* Popup hole, clamped into the band so a future move of either rectangle
     * cannot produce an inverted or out-of-band window. */
    bool     hole     = popup_live();
    uint16_t hole_top = POPUP_Y;
    uint16_t hole_bot = POPUP_Y + POPUP_H;
    if (hole_top < SCOPE_TOP) hole_top = SCOPE_TOP;
    if (hole_bot > band_bot)  hole_bot = band_bot;
    if (hole_top >= hole_bot) hole = false;
    popup_hole_last = hole;

    for (uint16_t x = 0; x < LCD_WIDTH; x++) {
        /* The trace lives in the 301-column window (scope_geom.h); right of
         * it the column carries only the trigger furniture and background.
         * Inside it, a re-sampled held record (C14) may not reach every
         * column: [g_view_first, g_view_end) is what view_build drew. */
        const bool inwin = (x < SCOPE_GEOM_COLS);
        const bool trace = (x >= g_view_first && x < g_view_end);
        const bool on1 = a1.on && trace, on2 = a2.on && trace;
        /* One transform, shared with the full path — see yt_fits. */
        int16_t y1 = on1 ? autofit_y(&a1, b1[x]) : (int16_t)SCOPE_MID_Y;
        int16_t y2 = on2 ? autofit_y(&a2, b2[x]) : (int16_t)SCOPE_MID_Y;

        /* 2-px dot plus a span to the previous sample, so steep edges draw
         * as connected verticals instead of the full path's dotted gaps. */
        int16_t lo1 = y1, hi1 = y1 + 1, lo2 = y2, hi2 = y2 + 1;
        const int16_t pp1 = (x > g_view_first) ? p1 : -1;   /* persistence feed */
        const int16_t pp2 = (x > g_view_first) ? p2 : -1;
        if (x > g_view_first) {             /* no span from a blank column */
            if (p1 < lo1) lo1 = p1;
            if (p1 > hi1) hi1 = p1;
            if (p2 < lo2) lo2 = p2;
            if (p2 > hi2) hi2 = p2;
        }
        p1 = y1;
        p2 = y2;

        /* Math: this column's row and the span from the previous one. */
        int16_t mlo = -1, mhi = -2;
        if (mth && mf.ok && trace) {
            const int16_t r = math_row(&mf, x);
            mlo = mhi = r;
            if (x > g_view_first) {
                if (mp < mlo) mlo = mp;
                if (mp > mhi) mhi = mp;
            }
            mp = r;
        }
        const bool mcol = mth && (int)x >= (int)MATH_LABEL_X && (int)x < (int)MATH_LABEL_X + mlab_w;

        /* Mask overlay for this column: the strip colour and whether either
         * trace is painted as outside the mask. */
        bool     mdot = false;
        uint16_t mstrip = mask_strip_color((int)x, th, &mdot);
        uint16_t tc1 = mask_px_fail(0, (int)x) ? th->warning : th->ch1;
        uint16_t tc2 = mask_px_fail(1, (int)x) ? th->warning : th->ch2;
        /* Mask bounds (dotted, every other column), through each channel's
         * own map -- the one transform, as everywhere else. -1 = none. */
        int16_t bl1 = -1, bh1 = -1, bl2 = -1, bh2 = -1;
        if (g_mask_bounds_on && MASK_BOUND_DOT(x)) {
            uint8_t lo, hi;
            if (on1 && scope_mask_bound_at(0, (int)x, g_trig_x_actual, &lo, &hi)) {
                bl1 = autofit_y(&a1, lo); bh1 = autofit_y(&a1, hi);
            }
            if (on2 && scope_mask_bound_at(1, (int)x, g_trig_x_actual, &lo, &hi)) {
                bl2 = autofit_y(&a2, lo); bh2 = autofit_y(&a2, hi);
            }
        }

        /* The popup is a HOLE in the live band, not a layer: its glyphs are
         * the one thing this compositor cannot recompute per pixel, so the
         * column is emitted as up to two segments that step around the box.
         * When the countdown ends the hole closes and the very next live
         * frame paints the band back over it — no full repaint at either
         * end, which is the whole point of the change. */
        /* Only the edge columns can carry a tag, and only the 9 columns
         * round the trigger point the pointer: gate both per column, so the
         * per-pixel shape calls (plot_markers.c) run inside their boxes only. */
        const bool edge = (x < PM_TAG_W) || (x >= LCD_WIDTH - PM_TAG_W);
        const bool tcol = g_trig_x_actual >= 0 &&
                          (int)x + PM_TPOS_W / 2 >= (int)g_trig_x_actual &&
                          (int)x <= (int)g_trig_x_actual + PM_TPOS_W / 2;

        uint16_t seg_top[2], seg_bot[2];
        uint8_t  nseg = 1;
        seg_top[0] = SCOPE_TOP;
        seg_bot[0] = band_bot;
        if (hole && x >= POPUP_X && x < POPUP_X + POPUP_W) {
            seg_bot[0] = hole_top;
            seg_top[1] = hole_bot;
            seg_bot[1] = band_bot;
            nseg = 2;
        }

        for (uint8_t sg = 0; sg < nseg; sg++) {
        if (seg_bot[sg] <= seg_top[sg]) continue;
        lcd_set_window(x, seg_top[sg], 1, (uint16_t)(seg_bot[sg] - seg_top[sg]));
        for (uint16_t y = seg_top[sg]; y < seg_bot[sg]; y++) {
            uint16_t c;
            /* Z-order matches the full path: cursors over everything, CH2
             * painted after CH1 there, so CH2 wins here; trace over trigger
             * line over grid. */
            if (curs && cursor_layer_px(&cl, (int)x, (int)y, &c))
                ;                                      /* cursor lines and labels */
            else if (tcol && y < SCOPE_TOP + PM_TPOS_H &&
                tpos_px(g_trig_x_actual, (int)x, (int)y))  /* trigger-position pointer */
                c = th->trigger;
            else if (edge && markers_px(&pm, (int)x, (int)y, th->background, &c))
                ;                                      /* ground / trigger tags */
            else if (mcol && y < SCOPE_TOP + font_small.height &&
                     font_text_px(mf.label, &font_small, (int)x - (int)MATH_LABEL_X,
                                  (int)y - SCOPE_TOP))
                c = th->warning;                       /* math label */
            else if ((int16_t)y >= mlo && (int16_t)y <= mhi)
                c = th->warning;                       /* math trace */
            else if (mth && mf.ok && x < 6u && y == SCOPE_GRID_MID_Y)
                c = th->warning;                       /* math zero tick */
            else if (mstrip && y >= band_bot - MASK_STRIP_H &&
                     (!mdot || ((x + y) & 1u) == 0u))
                c = mstrip;
            else if (on2 && (int16_t)y >= lo2 && (int16_t)y <= hi2)
                c = tc2;
            else if (on1 && (int16_t)y >= lo1 && (int16_t)y <= hi1)
                c = tc1;
            else if ((int16_t)y == bl1 || (int16_t)y == bh1 ||
                     (int16_t)y == bl2 || (int16_t)y == bh2)
                c = th->text_secondary;
            else if ((int16_t)y == trig_y && (x & trig_step) == 0 && x < LCD_WIDTH - 6)
                c = th->trigger;
            else if (inwin && (y == SCOPE_GRID_MID_Y || x == SCOPE_GEOM_CENTER_COL))
                c = th->grid_center;
            else if (inwin &&
                     ((scope_geom_vgrid((int)x) && (y & 1u) == 0) ||
                      (((y - SCOPE_TOP) % 26u) == 0 && (x & 1u) == 0)))
                c = th->grid;
            else {
                c = th->background;
                if (pbuf) {
                    const uint8_t in = pbuf[(y - SCOPE_TOP) * PERSIST_WIDTH + x];
                    if (in) {
                        const uint16_t g = persist_scale_rgb565(
                            (y < SCOPE_MID_Y) ? pbase_top : pbase_bot, in);
                        if (g != 0x0000) c = g;
                    }
                }
            }
            lcd_write_data(c);
        }
        }
        if (pbuf && trace) {
            if (on1) persist_note(x, pp1, y1);
            if (on2) persist_note(x, pp2, y2);
        }
    }

    /* ⚠ Measurement badges are NOT refreshed here — REVERTED 2026-08-14.
     * DO NOT REINSTATE WITHOUT READING THIS.
     *
     * Calling draw_measurement_badges() from this function KILLED ACQUISITION
     * on the bench: OK: reached exactly 20 and stopped, TO: climbed forever,
     * and it survived two full FPGA power cycles and a pinhole reset.
     * Reproduced on BOTH guest-persist and guest-coldtrace, so it was not the
     * settings-persistence work. Before the edit, OK: ran past 191,000.
     *
     * Why: the measurement passes read 1024 samples from BOTH channel buffers,
     * three times each, from the DISPLAY task — while the acquisition task is
     * writing those same buffers over SPI3.
     *
     * The badges do need refreshing (they otherwise latch "--" from scope
     * entry, where fpga.c has just cleared data_ready, and B2's redraw gating
     * means no further full repaint is triggered). But the fix belongs on the
     * other side: give the redraw epoch a measurement-generation term so a
     * full repaint happens when new samples land, instead of doing 6 KB of
     * analysis inline on the capture-rate path.
     *
     * Cost of leaving it out: stale badges — a display bug. The version that
     * was here was a data-loss bug. Prefer the display bug. */

#ifdef SCOPE_DEBUG_OVERLAY
    /* Keep the counters moving; opaque fixed-width lines, no strip refill. */
    draw_scope_debug(th);
#endif
}

/* ═══════════════════════════════════════════════════════════════════
 * Roll view (parity C11, scope_roll.h)
 *
 * Stock draws its 301-point roll FIFO right-to-left, newest at the right
 * edge (0x0801B6D2, Update Log V1.1.0). This is the same column compositor
 * as draw_scope_live_frame (each pixel's final colour streamed once per
 * column, so a point arriving every 2 ms never blanks the band), fed from
 * a snapshot of the FIFO instead of the 1024-sample record:
 *  - column x shows the point of age scope_roll_age_at_column(x, 301): 25
 *    points per 25 px division (one point per column, the whole 301-point FIFO
 *    on the glass), so the stock label (50 ms..20 s) is what one grid
 *    division means;
 *  - the vertical transform is the Y-T one: true volts/div about mid-scale
 *    when `fpga scope graticule true` and the range is calibrated, else the
 *    visible points' own min..max. Codes are drawn raw, as the Y-T record
 *    is; the unit's g = (m-28)/150 sits in the volts/div label;
 *  - the 12 measurements are the badges (draw_roll_badges), on the roll point
 *    rate; no cursors or trigger line (they describe the record). Stock's
 *    roll trigger runs in the roll service (fpga.c, scope_roll.h).
 * `full` adds the decorations the compositor cannot recompute per pixel
 * (labels, STOP, popup); the band itself is identical either way.
 * ═══════════════════════════════════════════════════════════════════ */
int scope_display_mode(void)
{
#ifdef FEATURE_FFT
    if (scope_view == SCOPE_VIEW_XY)
        return (int)SCOPE_DISP_XY;
#endif
    return scope_state_get()->roll ? (int)SCOPE_DISP_ROLL : (int)SCOPE_DISP_YT;
}

/* ui_draw_scratch (the roll FIFO copy) is defined with the Y-T window it
 * shares storage with, above the Y-T maps (autofit_t). */
#define roll_snap      (ui_draw_scratch.roll)
#define ROLL_LABEL_W 96u                /* "ROLL 301/301" in font_small */
#define ROLL_LABEL_H 16u

/* The 12 measurements over the roll FIFO (scope_roll_meas.h): the same badge
 * slots, order and units as the Y-T badges (draw_measurement_badges), the time
 * base being the roll point rate. Zero and scale are per channel, exactly the
 * Y-T inputs: scope_measure_zero_for(position) and scope_cal gain. `roll_snap`
 * is linearised here, after the trace has been drawn from it. Repainted when
 * `full` or every ROLL_BADGE_EVERY new points, so a 2 ms roll does not
 * repaint the badges at frame rate. */
#define ROLL_BADGE_EVERY 8u
static void draw_roll_badges(const scope_state_t *ss, const theme_t *th,
                             uint16_t nvis, bool full)
{
    static uint32_t last_total = 0xFFFFFFFFu;
    static uint8_t  last_step = 0xFFu;
    if (ss->prm_hidden)
        return;
    if (!full && last_step == ss->roll_step &&
        (uint32_t)(roll_snap.total - last_total) < ROLL_BADGE_EVERY)
        return;
    last_total = roll_snap.total;
    last_step = ss->roll_step;

    scope_roll_fifo_linearize(&roll_snap);
    scope_params_t p1, p2;
    const uint8_t z1 = (uint8_t)chan_zero_code(ss, 0u);   /* the one zero (chan_zero_code) */
    const uint8_t z2 = (uint8_t)chan_zero_code(ss, 1u);
    const bool have1 = ss->ch1.enabled &&
        scope_roll_measure(&roll_snap, 0u, nvis, ss->roll_step,
                           scope_cal_volts_per_count(1u, ss->ch1.vdiv_idx), z1, &p1, NULL);
    const bool have2 = ss->ch2.enabled &&
        scope_roll_measure(&roll_snap, 1u, nvis, ss->roll_step,
                           scope_cal_volts_per_count(2u, ss->ch2.vdiv_idx), z2, &p2, NULL);

    mstrip_layout_t L;
    strip_layout(ss, &L);
    draw_strip_cells(&L, have1 ? &p1 : NULL, have2 ? &p2 : NULL, th, !full);
}

void draw_roll_screen(bool full)
{
    const theme_t *th = theme_get();
    const scope_state_t *ss = scope_state_get();
    (void)scope_roll_fifo_snapshot(fpga_roll_fifo(), &roll_snap);
    const uint16_t nvis = scope_roll_visible(&roll_snap, SCOPE_GEOM_COLS);

    const bool en1 = ss->ch1.enabled, en2 = ss->ch2.enabled;
    const bool fx1 = ss->true_scale && scope_cal_true_scale_ok(1u, ss->ch1.vdiv_idx);
    const bool fx2 = ss->true_scale && scope_cal_true_scale_ok(2u, ss->ch2.vdiv_idx);
    scope_roll_vmap_t m1 = { false, 0, 255, 128, SCOPE_TOP, SCOPE_BOT, SCOPE_H };
    scope_roll_vmap_t m2 = m1;
    if (yt_vmap_split(en1, en2, fx1, fx2)) {    /* the Y-T band rule */
        m1.y_bot = SCOPE_MID_Y - 1;
        m2.y_top = SCOPE_MID_Y + 1;
    }
    bool on1 = en1, on2 = en2;
    /* True scale: the Y-T map itself (yt_vmap.h), so a switch between the
     * views never moves the trace. */
    yt_vmap_t t1, t2;
    if (fx1)
        yt_vmap_true(&t1, SCOPE_TOP, SCOPE_BOT, SCOPE_GRID_MID_Y, 0, yt_centre_code(),
                     scope_cal_codes_per_div(1u, ss->ch1.vdiv_idx));
    else on1 = on1 && scope_roll_fit(&roll_snap, 0u, nvis, &m1);
    if (fx2)
        yt_vmap_true(&t2, SCOPE_TOP, SCOPE_BOT, SCOPE_GRID_MID_Y, 1, yt_centre_code(),
                     scope_cal_codes_per_div(2u, ss->ch2.vdiv_idx));
    else on2 = on2 && scope_roll_fit(&roll_snap, 1u, nvis, &m2);

    const uint16_t band_bot = strip_top(ss);

    /* Popup hole: as the live compositor, never paint over the box. */
    bool     hole     = popup_live();
    uint16_t hole_top = POPUP_Y;
    uint16_t hole_bot = POPUP_Y + POPUP_H;
    if (hole_top < SCOPE_TOP) hole_top = SCOPE_TOP;
    if (hole_bot > SCOPE_BOT) hole_bot = SCOPE_BOT;
    if (hole_top >= hole_bot) hole = false;
    popup_hole_last = hole;

    int16_t p1 = -1, p2 = -1;
    for (uint16_t x = 0; x < LCD_WIDTH; x++) {
        /* C5 geometry: the 301-column window, one point per column (25 per
         * 25 px division); right of it only the furniture and background. */
        const bool inwin = (x < SCOPE_GEOM_COLS);
        const uint16_t age = scope_roll_age_at_column(x, SCOPE_GEOM_COLS);
        uint8_t v;
        int16_t lo1 = -1, hi1 = -2, lo2 = -1, hi2 = -2;
        if (on1 && inwin && scope_roll_fifo_get(&roll_snap, 0u, age, &v)) {
            int16_t y = fx1 ? yt_vmap_clip(&t1, yt_vmap_raw(&t1, v)) : scope_roll_y(&m1, v);
            lo1 = y; hi1 = (int16_t)(y + 1);
            if (p1 >= 0) { if (p1 < lo1) lo1 = p1; if (p1 > hi1) hi1 = p1; }
            p1 = y;
        } else {
            p1 = -1;
        }
        if (on2 && inwin && scope_roll_fifo_get(&roll_snap, 1u, age, &v)) {
            int16_t y = fx2 ? yt_vmap_clip(&t2, yt_vmap_raw(&t2, v)) : scope_roll_y(&m2, v);
            lo2 = y; hi2 = (int16_t)(y + 1);
            if (p2 >= 0) { if (p2 < lo2) lo2 = p2; if (p2 > hi2) hi2 = p2; }
            p2 = y;
        } else {
            p2 = -1;
        }

        uint16_t seg_top[2], seg_bot[2];
        uint8_t  nseg = 1;
        /* The fill label (top left) and the STOP badge (top centre) are
         * holes too, repainted opaque below, so they never strobe. */
        const bool in_label = (x < ROLL_LABEL_W);
#if SCOPE_PLOT_STOP_BOX
        const bool in_stop = !ss->running && x >= STOP_BOX_X && x < STOP_BOX_X + STOP_BOX_W;
#else
        const bool in_stop = false;
#endif
        seg_top[0] = in_label ? (uint16_t)(SCOPE_TOP + ROLL_LABEL_H) :
                     in_stop  ? (uint16_t)(STOP_BOX_Y + STOP_BOX_H) : (uint16_t)SCOPE_TOP;
        seg_bot[0] = band_bot;           /* the strip repaints itself, as in Y-T */
        if (hole && x >= POPUP_X && x < POPUP_X + POPUP_W) {
            seg_bot[0] = hole_top;
            seg_top[1] = hole_bot;
            seg_bot[1] = band_bot;
            nseg = 2;
        }
        for (uint8_t sg = 0; sg < nseg; sg++) {
            if (seg_bot[sg] <= seg_top[sg]) continue;
            lcd_set_window(x, seg_top[sg], 1, (uint16_t)(seg_bot[sg] - seg_top[sg]));
            for (uint16_t y = seg_top[sg]; y < seg_bot[sg]; y++) {
                uint16_t c;
                if ((int16_t)y >= lo2 && (int16_t)y <= hi2)
                    c = th->ch2;
                else if ((int16_t)y >= lo1 && (int16_t)y <= hi1)
                    c = th->ch1;
                else if (inwin && (y == SCOPE_GRID_MID_Y ||
                                   x == (uint16_t)SCOPE_GEOM_CENTER_COL))
                    c = th->grid_center;
                else if (inwin && ((scope_geom_vgrid((int)x) && (y & 1u) == 0) ||
                                   (((y - SCOPE_TOP) % 26u) == 0 && (x & 1u) == 0)))
                    c = th->grid;
                else
                    c = th->background;
                lcd_write_data(c);
            }
        }
    }

    /* Fill counter, top left: how much of the 301-point history is in. */
    char buf[24];
    snprintf(buf, sizeof(buf), "ROLL %3u/%u", (unsigned)roll_snap.count,
             (unsigned)SCOPE_ROLL_POINTS);
    if (full)
        lcd_fill_rect(0, SCOPE_TOP, ROLL_LABEL_W, ROLL_LABEL_H, th->background);
    /* Boxed: the whole label box is painted every time. font_draw_string()
     * paints glyph cells only, so the spaces of "ROLL   0/301" and the gaps
     * between glyphs kept the previous count's pixels (the "drawn twice"
     * header seen on entering roll). */
    font_draw_string_box(4, SCOPE_TOP + 2, (uint16_t)(ROLL_LABEL_W - 4), buf,
                         th->highlight, th->background, &font_small, FONT_ALIGN_LEFT);
    draw_roll_badges(ss, th, nvis, full);
    if (!full)
        return;
    draw_run_stop(ss, th);
    if (hole) draw_popup_box(th);      /* the same decision the band was painted with */
    else      popup_on = false;
}

/* ═══════════════════════════════════════════════════════════════════
 * FFT views (unchanged — reference scope_state for consistency)
 * ═══════════════════════════════════════════════════════════════════ */

#ifdef FEATURE_FFT

/* ── Display reference auto-ranging ─────────────────────────────────
 *
 * fft_process() reports magnitude in dB relative to ONE ADC COUNT
 * (20*log10(|X| * 2/N), |X| in counts) — not dBFS. A full-scale signal
 * therefore peaks near 20*log10(32767) = 90 dB, and the configured default
 * ref_level_db = 0.0 puts the top of the display ~90 dB BELOW the signal:
 * every populated bin clamps to normalized = 0, i.e. the top colour of the
 * ramp. Measured on the host with the same synthetic 1 kHz square the UI
 * feeds itself (scratch probe, 2026-08-13): peak = 84.4 dB, and 318 of the
 * 320 waterfall columns land in the RED band. That is the "hot red field
 * with thin nulls" seen on the bench the same day.
 *
 * Fix: track the observed peak instead of hard-coding a magic 90. The
 * reference follows frame_max + headroom, moving up immediately (so a
 * growing signal never clips against the top) and easing down slowly
 * through a dead band (so a jittering peak does not make the colours
 * breathe). cfg->ref_level_db is preserved as a USER TRIM applied on top,
 * which keeps fft_adjust_ref_level() meaningful.
 *
 * Known limit, stated rather than hidden: this ranges off the peak only. A
 * spectrum with no dynamic range (broadband noise and nothing else) has its
 * mean within ~10-15 dB of its peak and will still render as a flat, fairly
 * hot field. That is a true statement about such a signal; distinguishing it
 * would need a floor estimator as well, which is not warranted until real
 * captured data (rather than the synthetic square below) drives this view.
 */
#define FFT_REF_HEADROOM_DB   6.0f   /* keep the peak just below the top    */
#define FFT_REF_DEAD_BAND_DB  3.0f   /* ignore downward wobble below this   */
#define FFT_REF_DECAY         0.10f  /* per-frame fraction on the way down  */

static float fft_ref_tracked = 0.0f;
static bool  fft_ref_primed  = false;

/* ── Where the spectrum views get their samples ──────────────────────
 *
 * Until 2026-08-13 every FFT view — spectrum, split and waterfall — called
 * test_signal_generate() unconditionally and analysed a synthetic 1 kHz
 * square. Cold-boot capture started working that same day and none of them
 * lit up, because none of them was ever connected to the ADC. Worse, the
 * views did not say so: a user could not tell the spectrum of their probe
 * from the spectrum of a constant compiled into the firmware.
 *
 * Since then: the live CH1 record if there is one, and the view is LABELLED
 * with what it got. Same gate as the trace (fpga_data_ready()), same
 * tear-check as the measurement badges (frame generation counter).
 *
 * 2026-09-15 (FFT-live S1):
 *  - The record's invalid head is skipped (SCOPE_RECORD_HEAD_SKIP, the
 *    trace's SEAM_GUARD): 896 samples of static body go into the transform,
 *    windowed over the record rather than over the transform length
 *    (fft.c FFT_WINDOW_AT).
 *  - The frequency axis is REAL on measured timebase codes: the rate comes
 *    from scope_timebase_sample_rate() for the code in force, and is
 *    refused (0 -> bins, "--") when the code is unmeasured OR when the
 *    display's code and the FPGA's reg-0x01 code disagree — the same rule
 *    `fpga scope freq` applies. A confident wrong number is the failure
 *    mode this project keeps finding; "--" is not.
 *  - In the live-capture builds (FPGA_WARM_HANDOFF_TEST) the synthetic
 *    source is compiled OUT: with no capture yet the views say so and draw
 *    nothing, so the demo signal is unreachable there by construction
 *    (spec S1). Plain `guest` and `emu` keep the demo, labelled DEMO.
 */
typedef enum {
    FFT_SRC_NONE = 0,   /* live build, no capture yet: nothing to analyse */
    FFT_SRC_DEMO,       /* synthetic 1 kHz square at FFT_DEMO_SAMPLE_RATE_HZ */
    FFT_SRC_LIVE        /* CH1 acquisition record, head skipped */
} fft_src_t;

#define FFT_DEMO_SAMPLE_RATE_HZ  44100.0f

#if defined(FPGA_WARM_HANDOFF_TEST) && FPGA_WARM_HANDOFF_TEST
#define FFT_DEMO_REACHABLE 0
#else
#define FFT_DEMO_REACHABLE 1
#endif

/* Sample rate the live spectrum may claim, 0 when it may not. */
static float fft_live_rate_in_force(void)
{
    const scope_state_t *ss = scope_state_get();
    if (fpga_acq_rate_idx_get() != ss->timebase_idx)
        return 0.0f;                         /* display and hardware disagree */
    return fpga_record_sample_rate();        /* x2 for an interleaved record */
}

static fft_src_t fft_prepare_input(int16_t *sbuf, uint16_t *n_out,
                                   float *fs_out)
{
    const volatile uint8_t *b = fpga_get_ch1_buf();
    *n_out = 0;
    *fs_out = 0.0f;

    if (fpga_data_ready() && b != NULL) {
        uint16_t n = 0;
        for (int attempt = 0; attempt < 3; attempt++) {
            uint32_t g0 = fpga_acq_frame_generation();
            n = fft_live_prepare((const uint8_t *)b, FPGA_ADC_BUF_SIZE,
                                 sbuf, FFT_SIZE);
            if ((g0 & 1u) == 0u && fpga_acq_frame_generation() == g0)
                break;
        }
        *fs_out = fft_live_rate_in_force();
        fft_set_sample_rate(*fs_out);
        *n_out = n;
        return FFT_SRC_LIVE;
    }

#if FFT_DEMO_REACHABLE
    fft_set_sample_rate(FFT_DEMO_SAMPLE_RATE_HZ);
    test_signal_generate(TEST_SIG_SQUARE, sbuf,
                         FFT_SIZE, FFT_DEMO_SAMPLE_RATE_HZ,
                         1000.0f, 0.0f, 0.8f);
    *fs_out = FFT_DEMO_SAMPLE_RATE_HZ;
    *n_out = FFT_SIZE;
    return FFT_SRC_DEMO;
#else
    return FFT_SRC_NONE;
#endif
}

static float fft_display_ref(const fft_config_t *cfg, const float *data,
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
 * the bars (they did, at x = 2, over the lowest bins). */
#define FFT_LMARGIN  22u

/* The spectrum in [y_top, y_top + height): every pixel composed per column
 * and streamed, never blanked first (the FFT views' 3-4 Hz flash). */
static void fft_region_compose(uint16_t y_top, uint16_t height)
{
    const fft_config_t *cfg = fft_get_config();
    const uint16_t y_bot = (uint16_t)(y_top + height);
    ctext_set_t T;
    T.n = 0;

    int16_t *sbuf = fft_get_sample_buf();
    uint16_t nsamp = 0;
    float fs = 0.0f;
    const fft_src_t src = sbuf ? fft_prepare_input(sbuf, &nsamp, &fs) : FFT_SRC_NONE;
    if (cfg == NULL || src == FFT_SRC_NONE || nsamp == 0) {
        ctext_add(&T, 4, y_top + 2, "FFT: waiting", COLOR_GRAY, COLOR_BLACK, false);
        for (uint16_t x = 0; x < LCD_WIDTH; x++) {
            lcd_set_window(x, y_top, 1, height);
            for (uint16_t y = y_top; y < y_bot; y++) {
                uint16_t c = COLOR_BLACK;
                (void)ctext_px(&T, x, y, &c);
                lcd_write_data(c);
            }
        }
        return;
    }
    const bool live = (src == FFT_SRC_LIVE);
    fft_process(sbuf, nsamp, &fft_result);
    const float *draw_data = (fft_result.avg_db != NULL) ? fft_result.avg_db
                                                         : fft_result.level_db;
    const uint16_t zoom_start = cfg->zoom_start_bin, zoom_end = cfg->zoom_end_bin;
    uint16_t zoom_span = (uint16_t)(zoom_end - zoom_start);
    if (zoom_span == 0) zoom_span = 1;
    const uint16_t W = (uint16_t)(LCD_WIDTH - FFT_LMARGIN);
    /* Same auto-ranged reference the waterfall uses: with the configured
     * ref_level_db = 0 every bar pinned to full height, because the dB scale
     * is relative to one count, not to full scale. See fft_display_ref(). */
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
        const char *tag = live ? "LIVE CH1" : "DEMO sq";
        if (fft_result.num_peaks == 0) {
            snprintf(hdr, sizeof hdr, "%s", live ? "LIVE CH1" : "DEMO SIGNAL");
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
            bar_c = (n < 0.25f) ? COLOR_CH1 : (n < 0.5f) ? COLOR_GREEN
                  : (n < 0.75f) ? COLOR_CYAN : COLOR_GRID;
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

void draw_fft_screen(bool full)
{
    if (full) {                     /* the 2-row margins above and below */
        lcd_fill_rect(0, SCOPE_TOP, LCD_WIDTH, 2, COLOR_BLACK);
        lcd_fill_rect(0, SCOPE_TOP + SCOPE_H - 2, LCD_WIDTH, 2, COLOR_BLACK);
    }
    fft_region_compose(SCOPE_TOP + 2, SCOPE_H - 4);
}

/* Split view: the Y-T window on top (half-height divisions, 13 px, in the
 * same stock geometry: one division = the range's volts/div), the spectrum
 * below. Both composed per column. */
#define SPLIT_DIV_PX  13
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

/* Waterfall
 *
 * The 20 KB history buffer lives in the shared pool as a sub-tenant of the
 * FFT region (see SHMEM_FFT_WATERFALL_OFFSET) rather than in .bss. It cannot
 * be a pool owner of its own: this function calls fft_process(), so it only
 * ever runs while SHMEM_OWNER_FFT holds the pool, and claiming it separately
 * would evict the FFT it depends on.
 *
 * Only waterfall_row_idx, the generation counter and one assembled screen
 * line stay static — 645 bytes instead of 20,480.
 */
#define WATERFALL_ROWS  SHMEM_FFT_WATERFALL_ROWS
#define WATERFALL_COLS  SHMEM_FFT_WATERFALL_COLS
static uint8_t waterfall_row_idx = 0;
static uint32_t waterfall_pool_generation = 0;

/* The row blit below sets one window across the full plot width per history
 * row and streams into it, so a history row must be exactly one screen line
 * wide. */
_Static_assert(WATERFALL_COLS == LCD_WIDTH,
               "waterfall row blit assumes one history column per screen pixel");

/*
 * One assembled screen line, 640 bytes. Colours are converted once per
 * history row and pushed row_height times, instead of being recomputed for
 * every sub-row. It is deliberately NOT the 20 KB history buffer's problem:
 * that lives in the shared pool (see shared_mem.h) precisely because it is
 * large, whereas one line is small enough that the pool's ownership
 * lifetime rules would cost more than they save.
 */
#define waterfall_line (ui_draw_scratch.waterfall)   /* shared, see ui_draw_scratch */

/*
 * Intensity -> colour, with a BLACK floor.
 *
 * intensity 0 = at/above the display reference, 255 = at/below the floor
 * (reference - db_range). The old ramp ended at pure blue, so the noise
 * floor painted the plot solid blue and only the very top 20% of the scale
 * was red; combined with the reference bug (see fft_display_ref) that made
 * the whole plot one flat colour. This one runs black -> blue -> cyan ->
 * yellow -> red in four equal 64-step segments, which is the conventional
 * spectrogram ordering and puts "nothing here" at black.
 */
static uint16_t intensity_to_color(uint8_t intensity)
{
    uint16_t level = (uint16_t)(255u - intensity);   /* 0 = floor, 255 = hot */
    uint8_t  t = (uint8_t)((level & 63u) * 4u);      /* position in segment  */

    if (level < 64)  return RGB565(0, 0, t);                        /* black->blue  */
    if (level < 128) return RGB565(0, t, 255);                      /* blue ->cyan  */
    if (level < 192) return RGB565(t, 255, (uint8_t)(255 - t));     /* cyan ->yellow*/
    return RGB565(255, (uint8_t)(255 - t), 0);                      /* yellow->red  */
}

/* What the last waterfall frame did, for `ui dump` (scope_ui_waterfall_status). */
static uint8_t g_wf_status;   /* WF_ST_* */
enum { WF_ST_NEVER = 0, WF_ST_DRAWN, WF_ST_NO_FFT, WF_ST_WAITING, WF_ST_NOT_OWNER };

uint8_t scope_ui_waterfall_status(void) { return g_wf_status; }
uint8_t scope_ui_waterfall_row(void)    { return waterfall_row_idx; }

void draw_waterfall_screen(void)
{
    const fft_config_t *cfg = fft_get_config();

    int16_t *sbuf = fft_get_sample_buf();
    if (!sbuf) {                    /* FFT not initialized */
        g_wf_status = WF_ST_NO_FFT;
        return;
    }
    uint16_t nsamp = 0;
    float fs = 0.0f;
    fft_src_t src = fft_prepare_input(sbuf, &nsamp, &fs);
    if (src == FFT_SRC_NONE || nsamp == 0) {
        g_wf_status = WF_ST_WAITING;
        lcd_fill_rect(0, SCOPE_TOP, LCD_WIDTH, SCOPE_H, COLOR_BLACK);
        font_draw_string(4, SCOPE_TOP + 2, "WFALL: waiting for capture",
                         COLOR_GRAY, COLOR_GRAY, &font_small);
        return;
    }
    bool live = (src == FFT_SRC_LIVE);
    fft_process(sbuf, nsamp, &fft_result);

    /* Resolve the history buffer out of the FFT's pool tenancy. Returns NULL
     * if FFT no longer owns the pool, in which case there is nothing valid to
     * draw into — bail rather than scribble over another owner's data. This is
     * a stricter check than the fft_get_sample_buf() guard above, which only
     * proves the FFT was initialised at some point. */
    uint8_t *wf_pool = shared_mem_get(SHMEM_OWNER_FFT);
    if (!wf_pool) {
        g_wf_status = WF_ST_NOT_OWNER;
        return;
    }
    g_wf_status = WF_ST_DRAWN;
    uint8_t (*waterfall_buf)[WATERFALL_COLS] =
        (uint8_t (*)[WATERFALL_COLS])(wf_pool + SHMEM_FFT_WATERFALL_OFFSET);

    /* shared_mem_acquire() zeroes the pool whenever the owner changes, so a
     * round trip through screenshot/persistence wipes the history. Detect that
     * and restart from row 0 instead of scrolling through a zeroed tail.
     *
     * Refill with 0xFF rather than leaving the pool's zeros: intensity 0 maps
     * to the top of the ramp (red = at the display reference), so zeroed rows
     * would render as maximum signal. 0xFF is the other end of the ramp
     * (black = reference - db_range = the noise floor), which is what "no data
     * yet" should look like. Bench 2026-08-13: with the zero fill, entering
     * WFALL painted the whole plot red and the history grew downward
     * through it. */
    uint32_t pool_generation = shared_mem_transition_count();
    if (pool_generation != waterfall_pool_generation) {
        waterfall_pool_generation = pool_generation;
        waterfall_row_idx = 0;
        memset(waterfall_buf, 0xFF, SHMEM_FFT_WATERFALL_SIZE);
    }

    const float *draw_data = (fft_result.avg_db != NULL)
                             ? fft_result.avg_db : fft_result.level_db;

    uint16_t zoom_start = cfg->zoom_start_bin;
    uint16_t zoom_end = cfg->zoom_end_bin;
    uint16_t zoom_span = zoom_end - zoom_start;
    if (zoom_span == 0) zoom_span = 1;

    float ref_db = fft_display_ref(cfg, draw_data, zoom_start, zoom_end);

    uint16_t x;
    for (x = 0; x < WATERFALL_COLS; x++) {
        uint16_t bin = zoom_start + (uint16_t)((uint32_t)x * zoom_span / WATERFALL_COLS);
        if (bin >= FFT_BINS) bin = FFT_BINS - 1;
        float db = draw_data[bin];
        float normalized = (ref_db - db) / cfg->db_range;
        if (normalized < 0.0f) normalized = 0.0f;
        if (normalized > 1.0f) normalized = 1.0f;
        waterfall_buf[waterfall_row_idx][x] = (uint8_t)(normalized * 255.0f);
    }

    uint8_t newest_row = waterfall_row_idx;
    waterfall_row_idx = (uint8_t)((waterfall_row_idx + 1) % WATERFALL_ROWS);

    /* ── Blit ───────────────────────────────────────────────────────
     *
     * This used to clear the whole plot and then draw every pixel column as
     * its own 1 x row_height lcd_fill_rect: 20,481 fill_rects and therefore
     * 20,481 lcd_set_window() calls per frame, each one 3 commands + 8 byte
     * writes with a bus delay after every single one — ~225,000 delayed bus
     * transactions to paint 61,440 pixels. Bench 2026-08-13: the plot
     * visibly rastered left-to-right, top-to-bottom over seconds.
     *
     * Now: one window per history row, and each row is converted once into
     * waterfall_line and streamed row_height times. 65 set_windows and 192
     * lcd_write_pixels() calls per frame, with the same pixels at the same
     * coordinates.
     *
     * One window for the WHOLE block would cost 2 set_windows instead of 65,
     * but the window is global driver state and vInputTask runs at priority 4
     * against the display task's 1: its power-off countdown overlay
     * (input_handler.c) calls lcd_fill_rect() and so re-points the window
     * from under a stream in progress. Per-row windows bound that to one
     * corrupted row for one frame — the same blast radius the per-column
     * version had — for ~700 extra bus transactions out of 66,000.
     *
     * The clear is gone too: the block covers every pixel it draws over, so
     * the only part of the plot that still needs clearing is the residual
     * strip left when SCOPE_H is not a multiple of WATERFALL_ROWS (14 rows
     * at the current 206/64). That halves the pixel writes as well.
     *
     * Why not scroll-and-append, which would be O(1 row)? The history
     * scrolls by exactly one row per frame, so 63 of 64 rows are unchanged
     * in CONTENT — but they all MOVE, and with no framebuffer on this side
     * of the bus there is nothing to shift; the ST7789's own vertical
     * scroll works on panel rows, which MADCTL = 0xA0 (MV = 1) maps to
     * screen COLUMNS, i.e. it would scroll the waterfall sideways. An
     * O(1)-per-frame waterfall needs either a RAM framebuffer (the shared
     * pool is already spoken for by the FFT here) or a different history
     * layout that lets the newest row wrap in place instead of the image
     * scrolling — which changes what is drawn, so it is a design decision,
     * not a speedup. */
    uint16_t row_height = SCOPE_H / WATERFALL_ROWS;
    if (row_height < 1) row_height = 1;

    uint16_t rows_drawn = WATERFALL_ROWS;
    if ((uint32_t)rows_drawn * row_height > SCOPE_H)
        rows_drawn = (uint16_t)(SCOPE_H / row_height);
    const uint16_t block_h = (uint16_t)(rows_drawn * row_height);

    /* The header and the axis labels are composed into the lines they
     * cross (font_text_px) instead of drawn over the streamed rows, so
     * nothing is painted twice and nothing blinks; the residual strip under
     * the block (SCOPE_H - block_h rows) carries the axis labels. */
    ctext_set_t T;
    T.n = 0;
    ctext_add(&T, 4, SCOPE_TOP + 2, live ? "WFALL LIVE CH1" : "WFALL DEMO",
              live ? COLOR_WHITE : COLOR_ORANGE, live ? COLOR_WHITE : COLOR_ORANGE, false);
    {
        char l[12];
        const int ly = SCOPE_TOP + SCOPE_H - 12;
        fft_live_axis_label(fs, FFT_SIZE, zoom_start, l, sizeof l);
        ctext_add(&T, 2, ly, l, COLOR_WHITE, COLOR_BLACK, true);
        fft_live_axis_label(fs, FFT_SIZE, zoom_end, l, sizeof l);
        ctext_add(&T, LCD_WIDTH - 2 - (int)font_string_width(l, &font_small), ly, l,
                  COLOR_WHITE, COLOR_BLACK, true);
    }

    for (uint16_t sy = 0; sy < SCOPE_H; sy++) {
        const uint16_t y = (uint16_t)(SCOPE_TOP + sy);
        const bool in_block = sy < block_h;
        const uint16_t r = (uint16_t)(sy / row_height);
        /* A history row is converted once and reused for its row_height
         * lines unless a label crosses this line. */
        const bool row_first = in_block && (sy % row_height) == 0u;
        bool text_line = false;
        for (uint8_t k = 0; k < T.n; k++)
            if ((int)y >= T.t[k].y && (int)y < T.t[k].y + (int)font_small.height) text_line = true;
        if (row_first || text_line || !in_block) {
            const uint8_t *src = in_block
                ? waterfall_buf[(newest_row + WATERFALL_ROWS - r) % WATERFALL_ROWS] : NULL;
            for (x = 0; x < WATERFALL_COLS; x++) {
                uint16_t c = src ? intensity_to_color(src[x]) : COLOR_BLACK;
                if (text_line) (void)ctext_px(&T, x, y, &c);
                waterfall_line[x] = c;
            }
        }
        lcd_set_window(0, y, WATERFALL_COLS, 1);
        lcd_write_pixels(waterfall_line, WATERFALL_COLS);
        if (text_line && in_block && (sy + 1u) % row_height != 0u) {
            /* the next line of this row has no text: rebuild it plain */
            const uint8_t *src = waterfall_buf[(newest_row + WATERFALL_ROWS - r) % WATERFALL_ROWS];
            for (x = 0; x < WATERFALL_COLS; x++)
                waterfall_line[x] = intensity_to_color(src[x]);
        }
    }
}

#endif /* FEATURE_FFT */
