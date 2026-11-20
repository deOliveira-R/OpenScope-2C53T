/*
 * OpenScope 2C53T - oscilloscope screen: roll view
 *
 * The scrolling plot of slow timebases and its badges.
 */

#include "scope_ui_parts.h"

/* ═══ Roll view (state and FIFO in scope_roll.h) ═══
 *
 * The 301-point roll FIFO is drawn right to left, newest at the right edge
 * (as the factory firmware does). Each pixel's final colour is streamed once
 * per column, so a point arriving every 2 ms never blanks the band (its own
 * loop, not yet the canvas the Y-T plot uses), fed from a snapshot of the
 * FIFO instead of the 1024-sample record:
 *  - column x shows the point of age scope_roll_age_at_column(x, 301): one
 *    point per column, 25 points per 25 px division, the whole 301-point
 *    FIFO on the glass, so the label (50 ms..20 s) is what one grid
 *    division means;
 *  - the vertical transform is the Y-T one: true volts/div about mid-scale
 *    when true scale is on (ss->true_scale) and the range is calibrated, else
 *    the visible points' own min..max. Codes are drawn raw, as the Y-T
 *    record is; the unit's g = (m-28)/150 sits in the volts/div label;
 *  - the 12 measurements are the badges (draw_roll_badges), on the roll point
 *    rate; there are no cursors or trigger line (they describe the record).
 *    The roll trigger runs in the acquisition task (fpga_acq.c,
 *    scope_roll_trig_step in scope_roll.h).
 * `full` adds the decorations the compositor cannot recompute per pixel
 * (labels, popup); the band itself is identical either way.
 */
/* ui_draw_scratch (declared in scope_ui_parts.h, defined in scope_ui.c) is
 * storage shared between the views; its roll member holds the FIFO copy. */
#define roll_snap      (ui_draw_scratch.roll)

#define ROLL_LABEL_W 96u                /* "ROLL 301/301" in font_small */

#define ROLL_LABEL_H 16u

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

/*
 * Draw the roll view: the scrolling traces, grid, fill counter, badges and
 * (when `full`) the popup. Reads the roll FIFO snapshot; changes
 * roll_snap, popup_hole_last and, when the popup has ended, popup_on.
 *
 * Steps: snapshot the FIFO, build the vertical map per channel (true scale or
 * fitted to the visible points), then stream the plot column by column with
 * holes for the popup and the fill label, then the label,
 * badges and furniture.
 */
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
    /* True scale uses the Y-T map itself (yt_vmap.h), so a switch between the
     * views does not move the trace. */
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

    /* Popup hole: as in the Y-T plot, the box is never painted over. */
    bool     hole     = popup_live();
    uint16_t hole_top = POPUP_Y;
    uint16_t hole_bot = POPUP_Y + POPUP_H;
    if (hole_top < SCOPE_TOP) hole_top = SCOPE_TOP;
    if (hole_bot > SCOPE_BOT) hole_bot = SCOPE_BOT;
    if (hole_top >= hole_bot) hole = false;
    popup_hole_last = hole;

    int16_t p1 = -1, p2 = -1;
    for (uint16_t x = 0; x < LCD_WIDTH; x++) {
        /* The 301-column window, one point per column (25 per 25 px
         * division); right of it only the furniture and background. */
        const bool inwin = (x < SCOPE_GEOM_COLS);
        const uint16_t age = scope_roll_age_at_column(x, SCOPE_GEOM_COLS);
        uint8_t v;
        int16_t lo1 = -1, hi1 = -2, lo2 = -1, hi2 = -2;
        if (on1 && inwin && scope_roll_fifo_get(&roll_snap, 0u, age, &v)) {
            int16_t y = fx1 ? yt_vmap_clip(&t1, yt_vmap_raw(&t1, v)) : scope_roll_y(&m1, v);
            lo1 = y; hi1 = (int16_t)(y + SCOPE_TRACE_ROWS - 1);
            if (p1 >= 0) { if (p1 < lo1) lo1 = p1; if (p1 > hi1) hi1 = p1; }
            p1 = y;
        } else {
            p1 = -1;
        }
        if (on2 && inwin && scope_roll_fifo_get(&roll_snap, 1u, age, &v)) {
            int16_t y = fx2 ? yt_vmap_clip(&t2, yt_vmap_raw(&t2, v)) : scope_roll_y(&m2, v);
            lo2 = y; hi2 = (int16_t)(y + SCOPE_TRACE_ROWS - 1);
            if (p2 >= 0) { if (p2 < lo2) lo2 = p2; if (p2 > hi2) hi2 = p2; }
            p2 = y;
        } else {
            p2 = -1;
        }

        uint16_t seg_top[2], seg_bot[2];
        uint8_t  nseg = 1;
        /* The fill label (top left) is a hole too, repainted opaque below,
         * so it does not strobe. */
        const bool in_label = (x < ROLL_LABEL_W);
        seg_top[0] = in_label ? (uint16_t)(SCOPE_TOP + ROLL_LABEL_H) : (uint16_t)SCOPE_TOP;
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
                                   (((y - SCOPE_TOP) % SCOPE_DIV_PX) == 0 && (x & 1u) == 0)))
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
     * between glyphs would keep the previous count's pixels. */
    font_draw_string_box(4, SCOPE_TOP + 2, (uint16_t)(ROLL_LABEL_W - 4), buf,
                         th->highlight, th->background, &font_small, FONT_ALIGN_LEFT);
    draw_roll_badges(ss, th, nvis, full);
    if (!full)
        return;
    draw_popup(th);
}
