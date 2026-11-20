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

bool scope_ui_take_live_request(void)
{
    const bool r = g_live_req;
    g_live_req = false;
    return r;
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
 * other (one scope view at a time). Sharing them keeps the coldtrace build inside
 * RAM (it overflowed by 56 B when the round-2 parity branches were merged;
 * the Y-T window is smaller than the waterfall line, so it costs nothing). */
ui_draw_scratch_t ui_draw_scratch;

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
}
