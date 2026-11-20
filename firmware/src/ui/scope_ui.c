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

    scope_yt_draw(true, frame);

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
    scope_yt_draw(false, frame);
}
