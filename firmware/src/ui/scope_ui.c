/*
 * OpenScope 2C53T - oscilloscope screen
 *
 * The Y-T screen's full redraw and live update, and the request for a live
 * update.
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

/* What the last frame of a time view (Y-T, roll) drew cut off, bit 0 CH1,
 * bit 1 CH2: columns at the converter's limit, and samples that are valid
 * but off the plot (and not at the limit). The display type sets both after
 * each frame; the info bar's channel badges show them. */
static volatile uint8_t s_at_limit, s_off_plot;

void scope_ui_plot_clip_set(uint8_t at_limit, uint8_t off_plot)
{
    s_at_limit = at_limit;
    s_off_plot = off_plot;
}
uint8_t scope_ui_plot_at_limit(void) { return s_at_limit; }
uint8_t scope_ui_off_plot(void)      { return s_off_plot; }

/* The cursor state keeps its horizontal lines as rows of this pane. */
_Static_assert(CURSOR_SCOPE_TOP == SCOPE_TOP && CURSOR_SCOPE_BOT == SCOPE_BOT,
               "the cursor rows (view.curh) are rows of the full plot");

plot_pane_t scope_pane_full(const scope_state_t *ss)
{
    return plot_pane_make(CANVAS_PLOT, SCOPE_TOP, SCOPE_BOT, (int16_t)strip_top(ss));
}

/* The rows between the status bar and the plot belong to the scope screen:
 * a full paint of any of its views starts with them. */
void scope_ui_paint_margin(void)
{
    lcd_fill_rect(0, SB_H, LCD_WIDTH, SCOPE_TOP - SB_H, theme_get()->background);
}

/* Display-task scratch shared by draw paths that never run at once: the roll
 * view's FIFO copy (~610 B), the FFT waterfall's one-row pixel line (640 B)
 * and the Y-T window (2 x 301 B: what each column of each channel shows,
 * interpolated when zoomed). Each is filled and consumed on the display task
 * inside one view's draw calls, and only one scope view is drawn at a time.
 * Sharing them keeps the coldtrace build inside its static RAM; the Y-T
 * window is smaller than the waterfall line, so it adds nothing. */
ui_draw_scratch_t ui_draw_scratch;

/* Full redraw of the Y-T screen: every column of the plot (scope_yt_draw,
 * the same frame a live update draws), then what surrounds it. Nothing is
 * assumed about what the glass holds. frame: the display's frame counter. */
void draw_scope_screen(uint32_t frame)
{
    const theme_t *th = theme_get();
    const scope_state_t *ss = scope_state_get();

    const plot_pane_t pane = scope_pane_full(ss);
    scope_yt_draw(&pane, true, frame);

    /* The measurement strip's rows, between the band and the info bar: its
     * cells and its AUX row (the CAN line, the cursor readout or the mask
     * readout) paint themselves over a cleared background. */
    const uint16_t band_bot = strip_top(ss);
    if (band_bot < SCOPE_BOT)
        lcd_fill_rect(0, band_bot, LCD_WIDTH, (uint16_t)(SCOPE_BOT - band_bot), th->background);
    draw_measurement_badges(ss, th, false);

    /* The popup box, in the hole the plot left for it. */
    draw_popup(th);
}

/* Live update of the Y-T plot: only the columns that changed. The
 * measurement strip refreshes itself (scope_ui_strip_refresh) and the popup
 * box is kept up by scope_popup_overlay_tick. */
void draw_scope_live_frame(uint32_t frame)
{
    const plot_pane_t pane = scope_pane_full(scope_state_get());
    scope_yt_draw(&pane, false, frame);
}
