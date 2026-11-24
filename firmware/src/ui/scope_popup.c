/*
 * OpenScope 2C53T - oscilloscope screen: popup
 *
 * The message box over the plot and its timed overlay.
 */

#include "scope_ui_parts.h"

/* ═══ Quick-change popup state (popup_on is shared with the other scope files, see scope_ui_parts.h) ═══ */

static char popup_text[32] = "";

/* A scope popup lives POPUP_MS of wall clock, running or stopped; popup_on
 * stays set until a frame has been drawn without the box (the erase). */
bool     popup_on;

static uint32_t popup_until;

bool     popup_hole_last;  /* the last frame of the plot kept a hole for the box */

/* Has the box been painted for THIS popup? A live frame paints it once and
 * then leaves it alone (the plot keeps a hole for it, scope_yt_draw); a full
 * redraw repaints it. Without this flag every live frame would repaint the
 * box and the text would strobe at the capture rate. */
static bool popup_painted = false;

#define POPUP_MS 500u

/* True while the scope popup is set and its POPUP_MS has not run out. */
bool popup_live(void)
{
    return popup_on && (int32_t)(xTaskGetTickCount() - popup_until) < 0;
}

/* Popups outside the scope. The scope's popup rides on the scope renderer,
 * which is the only place that draws it. Anywhere else (meter, siggen,
 * settings) the popup is a tick-timed overlay that the display task
 * services (ui_popup_overlay_service), sized to its text. */
#define POPUP_OVERLAY_MS 1200u

static bool     ov_pending;

static bool     ov_drawn;

static uint32_t ov_until;

/* Show a popup with `text` (truncated to fit popup_text). In the scope it
 * becomes the plot popup (popup_on, POPUP_MS); on any other screen, and over
 * the Oscilloscope Settings page, it becomes the overlay (POPUP_OVERLAY_MS). */
void scope_show_popup(const char *text)
{
    int i = 0;
    while (text[i] && i < (int)sizeof(popup_text) - 1) {
        popup_text[i] = text[i];
        i++;
    }
    popup_text[i] = '\0';
    /* The scope's own popup rides on the plot. Over the Oscilloscope Settings
     * page no plot is drawn, so it takes the overlay path the other screens
     * use (main.c services it while the page is up). */
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

/* Paint the overlay box centred on the glass, sized to the text (large font,
 * medium if the text would not fit), with a one-pixel border. */
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

/* Called by the display task each pass. Returns 0 for nothing to do, 1 when
 * the box was (re)painted (first time, or the screen was repainted under it),
 * 2 when the overlay has expired and the caller must repaint the screen. */
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

/* Drop a pending overlay without repainting. */
void ui_popup_overlay_cancel(void)
{
    ov_pending = false;
    ov_drawn = false;
}

/* True while the plot popup is set, including the frame that erases it. */
bool scope_popup_active(void)
{
    return popup_on;
}

/* ═══ Plot popup ═══ */

/* Paint the 200 px popup box with its text and mark it painted. */
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

/* After a full frame of the plot (Y-T or roll): paint the box if the frame
 * kept its hole (popup_hole_last, the decision the plot was painted with);
 * otherwise the plot was drawn without it and the popup ends. */
void draw_popup(const theme_t *th)
{
    if (!popup_on) return;
    if (popup_hole_last) draw_popup_box(th);
    else                 popup_on = false;
}

/* Popup upkeep for the waterfall view. Their frames
 * compose the plot with no hole for the box, so the box is put back over
 * each frame while it lives. The popup ends when it expires. */
void scope_popup_view_tick(void)
{
    if (!popup_on) return;
    if (popup_live()) draw_popup_box(theme_get());
    else popup_on = false;
}

/*
 * Popup upkeep after a live Y-T frame. The plot treats the popup rectangle
 * as a hole (the one thing it cannot recompute per pixel, because of the
 * glyphs). The box is painted exactly once, and the band underneath is
 * repainted on the first frame after the countdown expires. No full redraw is
 * needed at either end, so the popup does not make the trace flicker.
 * popup_hole_last tells whether the last frame kept the hole.
 */
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
