/*
 * OpenScope 2C53T - oscilloscope screen
 *
 * The full redraw of the screen in its current view, and the request for a
 * live update.
 */

#include "scope_ui_parts.h"

_Static_assert(MSTRIP_PRM_COUNT == SCOPE_PRM_COUNT,
               "measure_strip's mask width must be stock's 12 measurements");

/* A change the live frame can show by itself (cursor moved or reselected):
 * the display task runs one live frame instead of a clear-and-repaint. */
static volatile bool g_live_req;

void scope_ui_request_live(void) { g_live_req = true; }

/* Read and clear the live request (the display task, once per pass). */
bool scope_ui_take_live_request(void)
{
    const bool r = g_live_req;
    g_live_req = false;
    return r;
}

/* Display-task scratch shared by draw paths that never run at once: the roll
 * view's FIFO copy (~610 B), the FFT waterfall's one-row pixel line (640 B)
 * and the Y-T window (2 x 301 B: what each column of each channel shows,
 * interpolated when zoomed). Each is filled and consumed on the display task
 * inside one view's draw calls, and only one scope view is drawn at a time.
 * Sharing them keeps the coldtrace build inside its static RAM; the Y-T
 * window is smaller than the waterfall line, so it adds nothing. */
ui_draw_scratch_t ui_draw_scratch;

/* Full redraw of the Y-T screen: clear the plot, then paint every layer in
 * order, bottom first. frame drives the demo waveform's phase. The live
 * frame (draw_scope_live_frame, scope_live.c) must give the same pixels. */
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

    /* Mask strip at the bottom of the live band (the live frame paints the
     * same rows), only when draw_demo_waveform() armed the overlay. */
    draw_mask_strip(th, band_bot);

    /* Layer 6: Math channel overlay */
    draw_math_waveform();

    /* Layer 6b: plot-edge markers over the traces, as stock: ground tags,
     * trigger tag, and the trigger-position pointer over the column where the
     * trigger point actually landed (g_place.trig_x), not where it was asked
     * to land, which the soft-trigger fallback may not honour. Same builder
     * and shapes as the live frame. */
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
            for (int x = g_place.trig_x - PM_TPOS_W / 2; x <= g_place.trig_x + PM_TPOS_W / 2; x++)
                if (x >= 0 && x < LCD_WIDTH && tpos_px(g_place.trig_x, x, SCOPE_TOP + dy))
                    lcd_set_pixel((uint16_t)x, (uint16_t)(SCOPE_TOP + dy), th->trigger);
    }

    /* Layer 7: Cursor lines and labels (the readout is in the AUX row) */
    draw_cursors(band_bot);

    /* No trigger status badge on the plot: the status bar's trigger dot and
     * RUN/STOP/ARM tab show it. */

    /* Layer 9: Run/Stop */
    draw_run_stop(ss, th);

    /* Layer 10: Measurement badges */
    draw_measurement_badges(ss, th, false);

    /* Layer 11: Quick-change popup (on top of everything) */
    draw_popup(th);

    /* No channel name on the plot: the status bar's focus glyph shows the
     * active channel in its colour. */
}
