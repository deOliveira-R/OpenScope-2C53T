/*
 * OpenScope 2C53T - oscilloscope screen: roll view
 *
 * The scrolling plot of slow timebases and its badges.
 */

#include "scope_ui_parts.h"
#include "canvas.h"
#include "plot_elements.h"

/* ═══ Roll: a display type (docs/design/display_architecture.md) ═══
 *
 * The 301-point roll FIFO (scope_roll.h) drawn right to left, newest at the
 * right edge, as the factory firmware does. One frame is built from a
 * snapshot of the FIFO (roll_prepare) and painted column by column through
 * the canvas, so a point arriving every 2 ms never blanks the band and only
 * the columns that changed are sent:
 *  - column x shows the point of age scope_roll_age_at_column(x, 301): one
 *    point per column, 25 points per 25 px division, the whole 301-point
 *    FIFO on the glass, so the label (50 ms..20 s) is what one grid
 *    division means;
 *  - the vertical maps are the Y-T ones (yt_vmap.h): true volts/div where
 *    true scale is on and the range is calibrated, else fitted to the
 *    visible points' own min..max, in split bands when both channels are
 *    fitted. Codes are drawn raw, as the Y-T record is; the unit's
 *    g = (m-28)/150 sits in the volts/div label;
 *  - the elements are the ground, the graticule, the traces (with the clip
 *    colours, plot_elements.h) and the fill counter; there are no cursors
 *    and no trigger line (they describe the record). The roll trigger runs
 *    in the acquisition task (fpga_acq.c, scope_roll_trig_step);
 *  - the 12 measurements are the badges (draw_roll_badges), on the roll
 *    point rate.
 */
/* ui_draw_scratch (declared in scope_ui_parts.h, defined in scope_ui.c) is
 * storage shared between the views; its roll member holds the FIFO copy. */
#define roll_snap      (ui_draw_scratch.roll)

/* The fill counter's box, top left: "ROLL 301/301" in font_small, and where
 * its text starts in it. */
#define ROLL_LABEL_W       96u
#define ROLL_LABEL_H       16u
#define ROLL_LABEL_TEXT_X  4
#define ROLL_LABEL_TEXT_Y  (SCOPE_TOP + 2)

/* Repaint the 12 measurements over the roll FIFO (scope_roll_meas.h): the same
 * badge slots, order and units as the Y-T badges (draw_measurement_badges),
 * the time base being the roll point rate. Zero and scale are per channel,
 * exactly the Y-T inputs: scope_measure_zero_for(position) and scope_cal
 * gain. `roll_snap` is linearised here, after the trace has been drawn from
 * it. Repainted when `full` or every ROLL_BADGE_EVERY new points, so a 2 ms
 * roll does not repaint the badges at frame rate. nvis = visible points. */
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
                           (volts_per_count_t){ scope_cal_volts_per_count(1u, ss->ch1.vdiv_idx) },
                           (rec_code_t){ z1 }, &p1, NULL);
    const bool have2 = ss->ch2.enabled &&
        scope_roll_measure(&roll_snap, 1u, nvis, ss->roll_step,
                           (volts_per_count_t){ scope_cal_volts_per_count(2u, ss->ch2.vdiv_idx) },
                           (rec_code_t){ z2 }, &p2, NULL);

    mstrip_layout_t L;
    strip_layout(ss, &L);
    draw_strip_cells(&L, have1 ? &p1 : NULL, have2 ? &p2 : NULL, th, !full);
}

/* One frame's inputs, built once (roll_prepare). */
typedef struct {
    const theme_t       *th;
    const scope_state_t *ss;
    uint16_t  band_bot;                         /* the band's end: the measurement strip's top */
    uint16_t  nvis;                             /* points on the glass */
    uint16_t  first;                            /* the first column that has a point; SCOPE_GEOM_COLS: none */
    bool      on[2];                            /* the channel is drawn */
    yt_vmap_t map[2];
    uint8_t   rail[2][PLOT_RAIL_BYTES];         /* columns at the converter's limit */
    int16_t   prev[2];                          /* the previous column's row */
    uint8_t   off_plot;                         /* bit per channel: a sample was off the plot this frame */
    char      label[24];                        /* the fill counter */
} roll_frame_t;

/* What one column shows. */
typedef struct {
    plot_trace_col_t tr[2];
    bool             in_label;                  /* the column crosses the fill counter's box */
} roll_col_t;

/* The code column x shows for channel ch, or false: one point per column,
 * the newest at the right, so the columns without a point (a FIFO still
 * filling) are the leftmost. */
static inline bool roll_code(uint8_t ch, uint16_t x, uint8_t *v)
{
    return x < SCOPE_GEOM_COLS &&
           scope_roll_fifo_get(&roll_snap, ch, scope_roll_age_at_column(x, SCOPE_GEOM_COLS), v);
}

/* Find the first column with a point and mark channel ch's columns at the
 * converter's limit. The columns' codes are laid out in a local array for
 * plot_rail_columns; kept out of line so that array is off the stack again
 * before the frame is painted. */
static __attribute__((noinline)) void roll_rails(roll_frame_t *f, uint8_t ch)
{
    uint8_t code[SCOPE_GEOM_COLS];
    f->first = SCOPE_GEOM_COLS;
    for (uint16_t x = 0; x < SCOPE_GEOM_COLS; x++) {
        if (!roll_code(ch, x, &code[x])) continue;
        if (f->first == SCOPE_GEOM_COLS) f->first = x;
    }
    plot_rail_columns(code, f->first, SCOPE_GEOM_COLS, f->rail[ch]);
}

/* Build the frame: snapshot the FIFO, choose each channel's map (true scale
 * as Y-T's, so a switch between the views does not move the trace; else
 * fitted to the visible points), lay the points out by column. Changes
 * roll_snap. */
static void roll_prepare(roll_frame_t *f)
{
    f->th = theme_get();
    const scope_state_t *const ss = f->ss = scope_state_get();
    (void)scope_roll_fifo_snapshot(fpga_roll_fifo(), &roll_snap);
    f->nvis = scope_roll_visible(&roll_snap, SCOPE_GEOM_COLS);
    f->band_bot = strip_top(ss);

    const bool en[2] = { ss->ch1.enabled, ss->ch2.enabled };
    const bool fx[2] = { ss->true_scale && scope_cal_true_scale_ok(1u, ss->ch1.vdiv_idx),
                         ss->true_scale && scope_cal_true_scale_ok(2u, ss->ch2.vdiv_idx) };
    const bool split = yt_vmap_split(en[0], en[1], fx[0], fx[1]);     /* the Y-T band rule */
    for (uint8_t ch = 0; ch < 2u; ch++) {
        f->on[ch] = en[ch];
        if (fx[ch]) {
            yt_vmap_true(&f->map[ch], SCOPE_TOP, SCOPE_BOT, SCOPE_GRID_MID_Y, (int8_t)ch, yt_centre_code(),
                         scope_cal_codes_per_div((uint8_t)(ch + 1u), ch ? ss->ch2.vdiv_idx : ss->ch1.vdiv_idx));
        } else {
            uint8_t lo = 0, hi = 0;
            int16_t top, bot;
            yt_vmap_band(SCOPE_TOP, (int16_t)f->band_bot, split, ch, &top, &bot);
            f->on[ch] = en[ch] && scope_roll_fit(&roll_snap, ch, f->nvis, &lo, &hi);
            yt_vmap_autofit(&f->map[ch], lo, hi, top, bot);
        }
    }

    for (uint8_t ch = 0; ch < 2u; ch++) roll_rails(f, ch);
    f->prev[0] = f->prev[1] = 0;
    f->off_plot = 0;

    /* Fill counter: how much of the 301-point history is in. */
    snprintf(f->label, sizeof(f->label), "ROLL %3u/%u", (unsigned)roll_snap.count, (unsigned)SCOPE_ROLL_POINTS);
}

/* Work out column x into c, and advance the frame's previous rows. */
static void roll_column(roll_frame_t *f, uint16_t x, roll_col_t *c)
{
    const theme_t *const th = f->th;
    const uint16_t ch_color[2] = { th->ch1, th->ch2 };
    for (uint8_t ch = 0; ch < 2u; ch++) {
        plot_trace_col_t *const t = &c->tr[ch];
        uint8_t code = 0;
        t->on = f->on[ch] && roll_code(ch, x, &code);
        if (!t->on) continue;
        plot_trace_col(t, &f->map[ch], yt_vmap_raw(&f->map[ch], code), x > f->first, f->prev[ch],
                       (int)f->band_bot, plot_rail_at(f->rail[ch], x), ch_color[ch], th);
        f->prev[ch] = t->y;
        if (t->off_plot) f->off_plot |= (uint8_t)(1u << ch);
    }
    c->in_label = (x < ROLL_LABEL_W);
}

/* The fill counter at pixel (x, y) of its box: the text's colour or the
 * box's background. */
static inline uint16_t roll_label_px(const roll_frame_t *f, uint16_t x, int y)
{
    return font_text_px(f->label, &font_small, (int)x - ROLL_LABEL_TEXT_X, y - ROLL_LABEL_TEXT_Y)
           ? f->th->highlight : f->th->background;
}

#if FEATURE_BENCHSHELL
/* `fpga scope livecheck` in roll: every pixel of column x against the
 * per-pixel rule, top layer first (the fill counter's box, CH2, CH1, the
 * graticule, the background), as yt_check_column does for Y-T. */
static void roll_check_column(const roll_frame_t *f, const roll_col_t *c, uint16_t x, const canvas_strip_t *s)
{
    for (int y = s->lo; y < s->hi; y++) {
        uint16_t want;
        if (c->in_label && y < (int)(SCOPE_TOP + ROLL_LABEL_H))
            want = roll_label_px(f, x, y);
        else if (plot_trace_px(&c->tr[1], y, &want))
            ;
        else if (plot_trace_px(&c->tr[0], y, &want))
            ;
        else if (plot_grid_px(f->th, x, y, &want))
            ;
        else
            want = f->th->background;
        const uint16_t got = s->px[y - s->lo];
        live_check_pixels++;
        if (want != got && live_check_diffs++ == 0) {
            live_check_first_x = x; live_check_first_y = (uint16_t)y;
            live_check_first_want = want; live_check_first_got = got;
        }
    }
}
#endif

/* Paint column x of the band into s: the canvas's paint function. The
 * elements, lowest first: the ground, the graticule, CH1, CH2 over it, the
 * fill counter's box over everything. */
static void roll_paint_column(void *ctx, uint16_t x, const canvas_strip_t *s)
{
    roll_frame_t *const f = ctx;
    roll_col_t c;
    roll_column(f, x, &c);

    const uint16_t bg = f->th->background;
    for (int i = 0; i < s->hi - s->lo; i++) s->px[i] = bg;
    plot_grid_paint(f->th, x, s);
    plot_trace_paint(&c.tr[0], s);
    plot_trace_paint(&c.tr[1], s);
    if (c.in_label)
        for (int y = SCOPE_TOP; y < (int)(SCOPE_TOP + ROLL_LABEL_H); y++)
            strip_put(s, y, roll_label_px(f, x, y));
#if FEATURE_BENCHSHELL
    if (live_check_frames) roll_check_column(f, &c, x, s);
#endif
}

/* Draw the roll view: one canvas frame over the band, then the badges.
 * full: send every column and repaint what surrounds the plot (the strip's
 * rows, the popup box); otherwise only the columns that changed, and the
 * badges when they are due. The popup box is a hole of the frame, as in Y-T.
 * Changes roll_snap, popup_hole_last and, when the popup has ended,
 * popup_on. */
void draw_roll_screen(bool full)
{
    roll_frame_t f;
    roll_prepare(&f);

    const canvas_rect_t popup = { POPUP_X, POPUP_Y, POPUP_W, POPUP_H };
    const bool hole = popup_live() && POPUP_Y < f.band_bot && POPUP_Y + POPUP_H > SCOPE_TOP;
    popup_hole_last = hole;

    const canvas_frame_t cf = {
        .rect = { 0, SCOPE_TOP, LCD_WIDTH, (uint16_t)(f.band_bot - SCOPE_TOP) },
        .holes = &popup, .nholes = hole ? 1u : 0u,
        .all = full,
        .paint = roll_paint_column, .ctx = &f,
    };
    (void)canvas_draw_columns(&cf);
    uint8_t at_limit = 0;
    for (uint8_t ch = 0; ch < 2u; ch++)
        for (unsigned i = 0; f.on[ch] && i < PLOT_RAIL_BYTES; i++)
            if (f.rail[ch][i]) at_limit |= (uint8_t)(1u << ch);
    scope_ui_plot_clip_set(at_limit, f.off_plot);
#if FEATURE_BENCHSHELL
    if (live_check_frames) live_check_frames--;
#endif

    if (full && f.band_bot < SCOPE_BOT)
        lcd_fill_rect(0, f.band_bot, LCD_WIDTH, (uint16_t)(SCOPE_BOT - f.band_bot), f.th->background);
    draw_roll_badges(f.ss, f.th, f.nvis, full);
    if (full) draw_popup(f.th);
}
