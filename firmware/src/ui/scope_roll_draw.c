/*
 * OpenScope 2C53T - oscilloscope screen: roll view
 *
 * The scrolling plot of slow timebases and its badges.
 */

#include "scope_ui_parts.h"

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
