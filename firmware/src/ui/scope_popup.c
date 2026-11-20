/*
 * OpenScope 2C53T - oscilloscope screen: popup
 *
 * The message box over the plot and its timed overlay.
 */

#include "scope_ui_parts.h"

/* ═══════════════════════════════════════════════════════════════════
 * Quick-change popup state (shared with main.c via extern)
 * ═══════════════════════════════════════════════════════════════════ */

static char popup_text[32] = "";

/* A scope popup lives POPUP_MS of wall clock, running or stopped; popup_on
 * stays set until a frame has been drawn without the box (the erase). */
bool     popup_on;

static uint32_t popup_until;

bool     popup_hole_last;  /* the last compositor frame kept the box */

/* Has the box been painted for THIS popup? The incremental path paints it
 * once and then leaves it alone (see draw_scope_live_frame's hole); the full
 * path repaints it every frame anyway. Without this, every live frame would
 * repaint the box and the text would strobe at the capture rate. */
static bool popup_painted = false;

#define POPUP_MS 500u

bool popup_live(void)
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
    if (current_mode == MODE_OSCILLOSCOPE && !screen_is_open(SCR_OSC_MENU)) {
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

bool scope_popup_active(void)
{
    return popup_on;
}

/* ═══════════════════════════════════════════════════════════════════
 * Quick-change popup overlay
 * ═══════════════════════════════════════════════════════════════════ */

void draw_popup_box(const theme_t *th)
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

void draw_popup(const theme_t *th)
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
