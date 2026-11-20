/*
 * OpenScope 2C53T - oscilloscope screen: live Y-T frame
 *
 * The column-by-column update of the Y-T plot: compose a column, skip it
 * when unchanged, send it.
 */

#include "scope_ui_parts.h"

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
/* One column of the live band, painted before it is sent (draw_scope_live_frame). */
static uint16_t s_live_col[LCD_HEIGHT];

/* What each column of the band held when it was last sent: a 32-bit FNV-1a
 * hash of its pixels. A column whose new pixels hash the same is not sent
 * again; with a steady trace that is most of them. The hashes are trusted
 * only while nobody else has drawn on the band's rows (lcd_watch_take) and
 * the band and its popup hole are where they were. Two different columns
 * hashing alike would leave the old one on the glass until it changes again
 * (one chance in 2^32 for each change). */
static uint32_t s_live_hash[LCD_WIDTH];

static bool     s_live_hash_valid;

static bool     s_live_hole_was;

#define LIVE_HASH_SEED   2166136261u     /* FNV-1a offset basis */

#define LIVE_HASH_PRIME  16777619u       /* FNV-1a prime */

/* Columns sent and skipped by the last live frame (`ui dump`). */
volatile uint16_t ui_scope_live_cols_sent, ui_scope_live_cols_skipped;

volatile uint32_t ui_scope_live_send_us;     /* of the frame's time, the part spent sending columns */

/* Rows between two dotted horizontal grid lines: one division of the graticule. */
#define SCOPE_GRID_ROW_PITCH 26u

#if FEATURE_BENCHSHELL
/* `fpga scope livecheck <frames>`: while live_check_frames is not zero each live frame
 * also evaluates the per-pixel rule and counts the pixels where the painted
 * column differs from it. */
volatile uint32_t live_check_frames, live_check_pixels, live_check_diffs;

volatile uint16_t live_check_first_x, live_check_first_y, live_check_first_want, live_check_first_got;

#endif
/* Time view_build() took in the last live frame, microseconds (`ui dump`). */
volatile uint32_t ui_scope_view_us_last;

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
    const uint32_t vb0 = DWT->CYCCNT;
    view_build(ss, b1, b2);
    ui_scope_view_us_last = (DWT->CYCCNT - vb0) / (system_core_clock / 1000000u);
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

    /* May the columns of the last frame be trusted? Not after anybody else
     * drew on these rows, nor when the band or its hole moved. */
    lcd_watch_rows(SCOPE_TOP, band_bot);
    if (lcd_watch_take() || hole != s_live_hole_was) s_live_hash_valid = false;
    s_live_hole_was = hole;
    uint16_t cols_sent = 0;
    uint32_t send_cycles = 0;        /* time inside the panel writes, for `ui dump` */

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

        /* Paint the column into s_live_col, lowest layer first, so that each
         * later layer overwrites the ones under it. The order is the z-order
         * of the full path read from the bottom: background and persistence
         * ghosts, grid, centre lines, trigger line, mask bounds, CH1, CH2
         * (painted after CH1 there, so it wins here), mask strip, math, the
         * edge tags, the trigger-position pointer, cursors over everything.
         * Each layer touches only the rows it can occupy; until 2026-11-20
         * every pixel walked the whole chain of tests (about 330 CPU cycles
         * a pixel, 81-96 ms a frame measured on the unit). */
        const uint16_t rows = (uint16_t)(band_bot - SCOPE_TOP);
        uint16_t *const col = s_live_col;
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
                for (uint16_t i = 0; i < rows; i += SCOPE_GRID_ROW_PITCH) col[i] = th->grid;
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
        if (mth && mf.ok && x < 6u) LIVE_PUT(SCOPE_GRID_MID_Y, th->warning);   /* math zero tick */
        if (mhi >= mlo) LIVE_SPAN(mlo, mhi, th->warning);                       /* math trace */
        if (mcol)                                                               /* math label */
            for (uint16_t y = SCOPE_TOP; y < SCOPE_TOP + font_small.height && y < band_bot; y++)
                if (font_text_px(mf.label, &font_small, (int)x - (int)MATH_LABEL_X, (int)y - SCOPE_TOP))
                    col[y - SCOPE_TOP] = th->warning;
        if (edge)                                                               /* ground / trigger tags */
            for (uint16_t y = SCOPE_TOP; y < band_bot; y++) {
                uint16_t c;
                if (markers_px(&pm, (int)x, (int)y, th->background, &c)) col[y - SCOPE_TOP] = c;
            }
        if (tcol)                                                               /* trigger-position pointer */
            for (uint16_t y = SCOPE_TOP; y < SCOPE_TOP + PM_TPOS_H && y < band_bot; y++)
                if (tpos_px(g_trig_x_actual, (int)x, (int)y)) col[y - SCOPE_TOP] = th->trigger;
        if (curs)                                                               /* cursor lines and labels */
            for (uint16_t y = SCOPE_TOP; y < band_bot; y++) {
                uint16_t c;
                if (cursor_layer_px(&cl, (int)x, (int)y, &c)) col[y - SCOPE_TOP] = c;
            }
#undef LIVE_SPAN
#undef LIVE_PUT
#undef LIVE_ROW_OK

#if FEATURE_BENCHSHELL
        /* `fpga scope livecheck`: every pixel of the column against the rule the
         * column painter replaced, written as one chain of tests in z-order.
         * Counts the pixels that differ; draws nothing. */
        if (live_check_frames) {
            for (uint16_t y = SCOPE_TOP; y < band_bot; y++) {
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

        uint32_t hash = LIVE_HASH_SEED;
        for (uint16_t i = 0; i < rows; i++) hash = (hash ^ col[i]) * LIVE_HASH_PRIME;
        if (!s_live_hash_valid || hash != s_live_hash[x]) {
            s_live_hash[x] = hash;
            cols_sent++;
            const uint32_t s0 = DWT->CYCCNT;
            for (uint8_t sg = 0; sg < nseg; sg++) {
                if (seg_bot[sg] <= seg_top[sg]) continue;
                lcd_write_column(x, seg_top[sg], &col[seg_top[sg] - SCOPE_TOP],
                                 (uint16_t)(seg_bot[sg] - seg_top[sg]));
            }
            send_cycles += DWT->CYCCNT - s0;
        }
        if (pbuf && trace) {
            if (on1) persist_note(x, pp1, y1);
            if (on2) persist_note(x, pp2, y2);
        }
    }

    (void)lcd_watch_take();          /* the marks of this frame's own windows */
    s_live_hash_valid = true;
    ui_scope_live_cols_sent = cols_sent;
    ui_scope_live_send_us = send_cycles / (system_core_clock / 1000000u);
    ui_scope_live_cols_skipped = (uint16_t)(LCD_WIDTH - cols_sent);

#if FEATURE_BENCHSHELL
    if (live_check_frames) live_check_frames--;
#endif

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
}
