/*
 * OpenScope 2C53T - oscilloscope screen: live Y-T frame
 *
 * The column-by-column update of the Y-T plot: compose a column, skip it
 * when unchanged, send it.
 */

#include "scope_ui_parts.h"
#include "canvas.h"

/* Columns sent and skipped by the last live frame (`ui dump`). */
volatile uint16_t ui_scope_live_cols_sent, ui_scope_live_cols_skipped;

volatile uint32_t ui_scope_live_send_us;     /* of the frame's time, the part spent sending columns */

#if FEATURE_BENCHSHELL
/* `fpga scope livecheck <frames>`: while live_check_frames is not zero each
 * live frame also evaluates every pixel with a per-pixel rule (one chain of
 * tests in z-order) and counts the pixels where the painted column differs
 * from it; the first difference is kept. */
volatile uint32_t live_check_frames, live_check_pixels, live_check_diffs;

volatile uint16_t live_check_first_x, live_check_first_y, live_check_first_want, live_check_first_got;

#endif
/* Time view_build() took in the last live frame, microseconds (`ui dump`). */
volatile uint32_t ui_scope_view_us_last;

/* One live frame's inputs, built once (yt_frame_prepare) and read by the
 * paint function for every column. p1, p2 and mp carry the previous column's
 * rows, for the connecting spans. */
typedef struct {
    const theme_t          *th;
    const scope_state_t    *ss;
    const volatile uint8_t *b1, *b2;        /* the window's columns (yt_cols) */
    uint16_t     band_bot;                  /* the band's end: the measurement strip's top */
    autofit_t    a1, a2;                    /* the two maps (yt_fits) */
    pm_set_t     pm;                        /* the edge tags */
    cur_layer_t  cl;                        /* the cursor layer */
    bool         curs;                      /* it has something to draw */
    math_frame_t mf;
    bool         mth;                       /* math is on */
    int16_t      mlab_w;                    /* width of its label, pixels */
    const uint8_t *pbuf;                    /* persistence intensities, or NULL */
    uint16_t     pbase_top, pbase_bot;      /* ghost colours above and below SCOPE_MID_Y */
    int16_t      trig_y;                    /* the trigger line's row */
    uint16_t     trig_step;                 /* its dot mask */
    int16_t      p1, p2, mp;
} yt_frame_t;
/* Paint column x of the band into s: the canvas's paint function. */
static void yt_paint_column(void *ctx, uint16_t x, const canvas_strip_t *s)
{
    yt_frame_t *const f = ctx;
    const theme_t *const th = f->th;
    const volatile uint8_t *const b1 = f->b1, *const b2 = f->b2;
    const uint16_t band_bot = f->band_bot;
    const bool curs = f->curs;
    const bool mth = f->mth;
    const int16_t mlab_w = f->mlab_w;
    const uint8_t *const pbuf = f->pbuf;
    const uint16_t pbase_top = f->pbase_top, pbase_bot = f->pbase_bot;
    const int16_t trig_y = f->trig_y;
    const uint16_t trig_step = f->trig_step;
    int16_t p1 = f->p1, p2 = f->p2, mp = f->mp;
    const uint16_t rows = (uint16_t)(band_bot - SCOPE_TOP);
    uint16_t *const col = s->px;
    {
        /* 3a. The trace lives in the 301-column window (scope_geom.h); right
         * of it the column carries only the trigger line, the tags, the
         * cursors and background. Inside it, a re-sampled held record may not
         * reach every column: [g_view_first, g_view_end) is what view_build
         * filled. */
        const bool inwin = (x < SCOPE_GEOM_COLS);
        const bool trace = (x >= g_view_first && x < g_view_end);
        const bool on1 = f->a1.on && trace, on2 = f->a2.on && trace;
        /* One transform, shared with the full redraw (yt_fits). */
        int16_t y1 = on1 ? autofit_y(&f->a1, b1[x]) : (int16_t)SCOPE_MID_Y;
        int16_t y2 = on2 ? autofit_y(&f->a2, b2[x]) : (int16_t)SCOPE_MID_Y;

        /* Rows y and y+1, plus the span to the previous column's row, so
         * steep edges draw as connected verticals. draw_channel (scope_yt.c)
         * draws only prev..y, without the extra row. */
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
        if (mth && f->mf.ok && trace) {
            const int16_t r = math_row(&f->mf, x);
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
         * own map: the one transform, as everywhere else. -1 = none. */
        int16_t bl1 = -1, bh1 = -1, bl2 = -1, bh2 = -1;
        if (g_mask_bounds_on && MASK_BOUND_DOT(x)) {
            uint8_t lo, hi;
            if (on1 && scope_mask_bound_at(0, (int)x, g_place.trig_x, &lo, &hi)) {
                bl1 = autofit_y(&f->a1, lo); bh1 = autofit_y(&f->a1, hi);
            }
            if (on2 && scope_mask_bound_at(1, (int)x, g_place.trig_x, &lo, &hi)) {
                bl2 = autofit_y(&f->a2, lo); bh2 = autofit_y(&f->a2, hi);
            }
        }

        /* Only the edge columns can carry a tag, and only the PM_TPOS_W
         * columns round the trigger point the pointer: gate both per column,
         * so the per-pixel shape calls (plot_markers.c) run inside their
         * boxes only. */
        const bool edge = (x < PM_TAG_W) || (x >= LCD_WIDTH - PM_TAG_W);
        const bool tcol = g_place.trig_x >= 0 &&
                          (int)x + PM_TPOS_W / 2 >= (int)g_place.trig_x &&
                          (int)x <= (int)g_place.trig_x + PM_TPOS_W / 2;

        /* 3b. Paint the column into s_live_col, lowest layer first, so that
         * each later layer overwrites the ones under it. The order is the
         * z-order of the full redraw read from the bottom: background and
         * persistence ghosts, grid, centre lines, trigger line, mask bounds,
         * CH1, CH2 (painted after CH1 there, so it wins here), mask strip,
         * math zero tick, trace and label, the edge tags, the
         * trigger-position pointer, cursors over everything. Each layer
         * touches only the rows it can occupy, which is what makes the frame
         * cheap compared with a chain of tests at every pixel (the livecheck
         * rule below). Index i of s_live_col is screen row SCOPE_TOP + i. */
#define LIVE_ROW_OK(yy)   ((int)(yy) >= (int)SCOPE_TOP && (int)(yy) < (int)band_bot)
#define LIVE_PUT(yy, cc)  do { if (LIVE_ROW_OK(yy)) col[(int)(yy) - (int)SCOPE_TOP] = (cc); } while (0)
#define LIVE_SPAN(lo, hi, cc) do { \
            int s_ = (int)(lo), e_ = (int)(hi); \
            if (s_ < (int)SCOPE_TOP) s_ = (int)SCOPE_TOP; \
            if (e_ > (int)band_bot - 1) e_ = (int)band_bot - 1; \
            for (int y_ = s_; y_ <= e_; y_++) col[y_ - (int)SCOPE_TOP] = (cc); \
        } while (0)
        if (!pbuf) {
            for (uint16_t i = 0; i < rows; i++) col[i] = th->background;
        } else {
            for (uint16_t i = 0; i < rows; i++) {
                uint16_t c = th->background;
                const uint8_t in = pbuf[i * PERSIST_WIDTH + x];
                if (in) {
                    const uint16_t g = persist_scale_rgb565(
                        ((uint16_t)(SCOPE_TOP + i) < SCOPE_MID_Y) ? pbase_top : pbase_bot, in);
                    if (g != 0x0000) c = g;
                }
                col[i] = c;
            }
        }
        if (inwin) {
            if (scope_geom_vgrid((int)x))                       /* dotted vertical line */
                for (uint16_t y = SCOPE_TOP; y < band_bot; y++)
                    if ((y & 1u) == 0) col[y - SCOPE_TOP] = th->grid;
            if ((x & 1u) == 0)                                  /* dotted horizontal lines */
                for (uint16_t i = 0; i < rows; i += SCOPE_DIV_PX) col[i] = th->grid;
            if (x == SCOPE_GEOM_CENTER_COL)
                for (uint16_t i = 0; i < rows; i++) col[i] = th->grid_center;
            else
                LIVE_PUT(SCOPE_GRID_MID_Y, th->grid_center);
        }
        if ((x & trig_step) == 0 && x < LCD_WIDTH - 6) LIVE_PUT(trig_y, th->trigger);
        if (bl1 >= 0) LIVE_PUT(bl1, th->text_secondary);
        if (bh1 >= 0) LIVE_PUT(bh1, th->text_secondary);
        if (bl2 >= 0) LIVE_PUT(bl2, th->text_secondary);
        if (bh2 >= 0) LIVE_PUT(bh2, th->text_secondary);
        if (on1) LIVE_SPAN(lo1, hi1, tc1);
        if (on2) LIVE_SPAN(lo2, hi2, tc2);
        if (mstrip)
            for (int y = (int)band_bot - (int)MASK_STRIP_H; y < (int)band_bot; y++)
                if (LIVE_ROW_OK(y) && (!mdot || ((x + (unsigned)y) & 1u) == 0u))
                    col[y - (int)SCOPE_TOP] = mstrip;
        if (mth && f->mf.ok && x < 6u) LIVE_PUT(SCOPE_GRID_MID_Y, th->warning);   /* math zero tick */
        if (mhi >= mlo) LIVE_SPAN(mlo, mhi, th->warning);                       /* math trace */
        if (mcol)                                                               /* math label */
            for (uint16_t y = SCOPE_TOP; y < SCOPE_TOP + font_small.height && y < band_bot; y++)
                if (font_text_px(f->mf.label, &font_small, (int)x - (int)MATH_LABEL_X, (int)y - SCOPE_TOP))
                    col[y - SCOPE_TOP] = th->warning;
        if (edge)                                                               /* ground / trigger tags */
            for (uint16_t y = SCOPE_TOP; y < band_bot; y++) {
                uint16_t c;
                if (markers_px(&f->pm, (int)x, (int)y, th->background, &c)) col[y - SCOPE_TOP] = c;
            }
        if (tcol)                                                               /* trigger-position pointer */
            for (uint16_t y = SCOPE_TOP; y < SCOPE_TOP + PM_TPOS_H && y < band_bot; y++)
                if (tpos_px(g_place.trig_x, (int)x, (int)y)) col[y - SCOPE_TOP] = th->trigger;
        if (curs)                                                               /* cursor lines and labels */
            for (uint16_t y = SCOPE_TOP; y < band_bot; y++) {
                uint16_t c;
                if (cursor_layer_px(&f->cl, (int)x, (int)y, &c)) col[y - SCOPE_TOP] = c;
            }
#undef LIVE_SPAN
#undef LIVE_PUT
#undef LIVE_ROW_OK

#if FEATURE_BENCHSHELL
        /* 3c. `fpga scope livecheck`: every pixel of the column against the
         * per-pixel rule, one chain of tests from the top layer down, in the
         * painter's z-order. It checks the painter against this rule, not
         * against draw_scope_screen. Counts the pixels that differ; draws
         * nothing. */
        if (live_check_frames) {
            for (uint16_t y = SCOPE_TOP; y < band_bot; y++) {
                uint16_t c;
                /* Top layer first: cursors over everything, CH2 over CH1,
                 * traces over mask bounds over the trigger line over the
                 * grid. */
                if (curs && cursor_layer_px(&f->cl, (int)x, (int)y, &c))
                    ;                                      /* cursor lines and labels */
                else if (tcol && y < SCOPE_TOP + PM_TPOS_H &&
                    tpos_px(g_place.trig_x, (int)x, (int)y))  /* trigger-position pointer */
                    c = th->trigger;
                else if (edge && markers_px(&f->pm, (int)x, (int)y, th->background, &c))
                    ;                                      /* ground / trigger tags */
                else if (mcol && y < SCOPE_TOP + font_small.height &&
                         font_text_px(f->mf.label, &font_small, (int)x - (int)MATH_LABEL_X,
                                      (int)y - SCOPE_TOP))
                    c = th->warning;                       /* math label */
                else if ((int16_t)y >= mlo && (int16_t)y <= mhi)
                    c = th->warning;                       /* math trace */
                else if (mth && f->mf.ok && x < 6u && y == SCOPE_GRID_MID_Y)
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
                          (((y - SCOPE_TOP) % SCOPE_DIV_PX) == 0 && (x & 1u) == 0)))
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
                live_check_pixels++;
                if (c != col[y - SCOPE_TOP]) {
                    if (live_check_diffs++ == 0) {
                        live_check_first_x = x; live_check_first_y = y;
                        live_check_first_want = c; live_check_first_got = col[y - SCOPE_TOP];
                    }
                }
            }
        }
#endif

        /* 3e. Feed persistence, after this column's ghosts were read. */
        if (pbuf && trace) {
            if (on1) persist_note(x, pp1, y1);
            if (on2) persist_note(x, pp2, y2);
        }
    }
    f->p1 = p1; f->p2 = p2; f->mp = mp;
}
/* The live Y-T frame: one canvas frame in column strips over the plot band
 * (SCOPE_TOP down to the measurement strip's top), for every one of the
 * LCD_WIDTH columns. Each column's final pixels are composed by
 * yt_paint_column and sent by the canvas only when they changed, so the
 * glass never shows a blank band (draw_scope_screen, scope_ui.c, clears it
 * and repaints). Called by main.c's scope_render for an incremental redraw
 * of the Y-T view with a record; the measurement strip and the popup box are
 * not drawn here. Expects the channel buffers to exist (returns otherwise).
 * Changes: yt_cols and the view globals, the persistence buffer, the cursor
 * readout's bands (vband), popup_hole_last and the `ui dump` counters.
 *
 * Outline:
 *   1. Build the window (view_build) and arm the mask overlay and math.
 *   2. Per frame: band bottom, the two maps, the edge tags, the vbands, the
 *      cursor layer, the math frame, persistence decay and colour bases,
 *      the trigger row, the popup hole.
 *   3. The canvas cycle; per column x (yt_paint_column):
 *      a. derive: trace rows and spans to the previous column, math span,
 *         mask colours and bounds, tag and pointer columns;
 *      b. paint the strip, lowest layer first;
 *      c. (bench shell) livecheck against the per-pixel rule;
 *      e. feed this column's traces to persistence.
 *   4. Publish the counters. */
void draw_scope_live_frame(void)
{
    yt_frame_t f;
    f.th = theme_get();
    f.ss = scope_state_get();
    const theme_t *const th = f.th;
    const scope_state_t *const ss = f.ss;
    const volatile uint8_t *b1 = fpga_get_ch1_buf();
    const volatile uint8_t *b2 = fpga_get_ch2_buf();

    if (!b1 || !b2) return;

    /* 1. The window: placed on the trigger source's crossing and filled into
     * yt_cols for both channels by the same view_build() the full redraw
     * uses (draw_yt_traces, scope_yt.c), so the two renderers stay in
     * phase. From here b1/b2 are the window's columns. */
    const uint32_t vb0 = DWT->CYCCNT;
    view_build(ss, b1, b2);
    ui_scope_view_us_last = (DWT->CYCCNT - vb0) / (system_core_clock / 1000000u);
    f.b1 = yt_cols[0];
    f.b2 = yt_cols[1];
    mask_overlay_begin();          /* per-column mask colours, see draw_mask_strip */
    g_math_live = true;            /* yt_cols is this frame's window */

    /* 2. Per frame. The band stops at the measurement strip's top, which
     * moves with the selection (strip_top, scope_furniture.c). The strip
     * repaints itself; streaming over it would fight it at frame rate. */
    f.band_bot = strip_top(ss);

    /* The maps exactly as the full redraw builds them (yt_fits: true scale
     * or autofit, the band split), so a change of renderer never moves the
     * trace. They run against SCOPE_BOT even though this function only
     * paints down to band_bot: the full redraw draws the trace under the
     * strip and paints the badges on top, so matching its geometry means
     * matching its y_bot, not its visible extent. */
    yt_fits(ss, &f.a1, &f.a2);

    /* The tags, on the same maps. */
    markers_build(ss, th, &f.a1, &f.a2, f.band_bot, &f.pm);

    /* What this frame drew, for the cursor readout (as draw_yt_traces
     * records it). */
    vband_clear();
    {
        uint8_t slot = 0;
        if (f.a1.on) vband_note(slot++, 1u, ss->ch1.vdiv_idx, &f.a1, yt_vmap_counts_per_px(&f.a1));
        if (f.a2.on) vband_note(slot, 2u, ss->ch2.vdiv_idx, &f.a2, yt_vmap_counts_per_px(&f.a2));
    }

    /* Cursors: one more layer of the column composition. */
    cursor_layer_build(ss, th, &f.cl);
    f.curs = f.cl.v || f.cl.h;

    /* Math trace: the same frame helpers as the full redraw, one row per
     * column, its label as per-pixel text. */
    f.mth = math_enabled;
    if (f.mth) math_prepare(ss, &f.mf);
    f.mlab_w = f.mth ? (int16_t)font_string_width(f.mf.label, &font_small) : 0;
    f.mp = 0;

    /* Persistence: the ghosts are read from the shared-pool buffer
     * (PERSIST_WIDTH x PERSIST_HEIGHT intensities, not .bss) under the grid,
     * decayed once per frame, and this frame's traces are added to it column
     * by column after the column is painted, as draw_channel() (scope_yt.c)
     * feeds it in the full redraw. */
    f.pbuf = (persist_enabled && persist_is_initialized()) ? persist_get_buffer() : NULL;
    if (f.pbuf) persist_frame_decay();
    f.pbase_top = persist_base(ss, th, SCOPE_TOP);
    f.pbase_bot = persist_base(ss, th, (uint16_t)(SCOPE_BOT - 1));

    /* Trigger dotted line, placed and clamped as draw_trigger_line
     * (scope_yt.c) places it: on the trigger source's map, at the record code
     * where the hardware fires; every 2nd column while the level has focus,
     * every 4th otherwise. */
    f.trig_y = trigger_marker_y(ss, (ss->trigger.source == TRIG_SRC_CH2) ? &f.a2 : &f.a1);
    if (f.trig_y < SCOPE_TOP + 2) f.trig_y = SCOPE_TOP + 2;
    if (f.trig_y > SCOPE_BOT - 3) f.trig_y = SCOPE_BOT - 3;
    f.trig_step = scope_trig_level_focus ? 1u : 3u;   /* dot mask */

    f.p1 = 0;
    f.p2 = 0;

    /* The popup is a HOLE in the band, not a layer: its glyphs are the one
     * thing this compositor cannot recompute per pixel, so the canvas steps
     * around the box. When the popup ends the hole closes and the next live
     * frame paints the band back over it, with no full repaint at either
     * end. popup_hole_last: the box has rows inside the band. */
    const canvas_rect_t popup = { POPUP_X, POPUP_Y, POPUP_W, POPUP_H };
    const bool hole = popup_live() && POPUP_Y < f.band_bot && POPUP_Y + POPUP_H > SCOPE_TOP;
    popup_hole_last = hole;

    /* 3. The cycle, over the whole LCD width. */
    const canvas_frame_t frame = {
        .rect = { 0, SCOPE_TOP, LCD_WIDTH, (uint16_t)(f.band_bot - SCOPE_TOP) },
        .holes = &popup, .nholes = hole ? 1u : 0u,
        .all = false,
        .paint = yt_paint_column, .ctx = &f,
    };
    const uint32_t send0 = lcd_perf_column_cycles;
    const uint16_t cols_sent = canvas_draw_columns(&frame);

    /* 4. */
    ui_scope_live_cols_sent = cols_sent;
    ui_scope_live_send_us = (lcd_perf_column_cycles - send0) / (system_core_clock / 1000000u);
    ui_scope_live_cols_skipped = (uint16_t)(LCD_WIDTH - cols_sent);

#if FEATURE_BENCHSHELL
    if (live_check_frames) live_check_frames--;
#endif

    /* The measurement strip is not painted here: scope_ui_strip_refresh()
     * (scope_furniture.c) repaints it in place between frames, its passes
     * tear-checked against the acquisition's frame generation. */
}
