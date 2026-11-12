/*
 * OpenScope 2C53T - two-pane menu frame (see menu_frame.h)
 */
#include "menu_frame.h"
#include "lcd.h"
#include "font.h"
#include "theme.h"

/* Disc spans, centred rows: outer ring 10 px, inner fill 8 px, dot 4 px. */
static const uint8_t k_disc10[10] = { 4, 8, 10, 10, 10, 10, 10, 10, 8, 4 };
static const uint8_t k_disc8[8]   = { 4, 6, 8, 8, 8, 8, 6, 4 };
static const uint8_t k_disc4[4]   = { 2, 4, 4, 2 };

static void disc(uint16_t cx, uint16_t y, const uint8_t *spans, uint8_t n, uint16_t c)
{
    for (uint8_t i = 0; i < n; i++)
        lcd_fill_rect((uint16_t)(cx - spans[i] / 2u), (uint16_t)(y + i), spans[i], 1, c);
}

void menu_frame_box(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                    uint16_t fill, uint16_t border, uint8_t bw)
{
    if (w == 0u || h == 0u) return;
    if (bw == 0u || 2u * bw >= w || 2u * bw >= h) {
        lcd_fill_rect(x, y, w, h, bw ? border : fill);
        return;
    }
    lcd_fill_rect(x, y, w, bw, border);                         /* top    */
    lcd_fill_rect(x, (uint16_t)(y + h - bw), w, bw, border);    /* bottom */
    lcd_fill_rect(x, (uint16_t)(y + bw), bw, (uint16_t)(h - 2u * bw), border);              /* left  */
    lcd_fill_rect((uint16_t)(x + w - bw), (uint16_t)(y + bw), bw, (uint16_t)(h - 2u * bw), border); /* right */
    lcd_fill_rect((uint16_t)(x + bw), (uint16_t)(y + bw), (uint16_t)(w - 2u * bw),
                  (uint16_t)(h - 2u * bw), fill);
}

/* Knock the four corner pixels to the background: reads as r = 1-2 rounding. */
static void round_corners(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t bg)
{
    lcd_set_pixel(x, y, bg);
    lcd_set_pixel((uint16_t)(x + w - 1u), y, bg);
    lcd_set_pixel(x, (uint16_t)(y + h - 1u), bg);
    lcd_set_pixel((uint16_t)(x + w - 1u), (uint16_t)(y + h - 1u), bg);
}

void menu_frame_draw_rail(const menu_frame_t *f, uint8_t sel, bool rail_focus, uint8_t *first)
{
    const theme_t *th = theme_get();
    const uint8_t vis = menu_frame_visible(f);
    uint8_t fr = menu_frame_first(sel, f->n_rows, vis, first ? *first : 0u);
    if (first) *first = fr;

    const bool scroll = f->n_rows > vis;
    const uint16_t text_x = (uint16_t)(f->x + 8u + (f->icon ? MENU_FRAME_ICON_W + 4u : 0u));
    const uint16_t text_w = (uint16_t)(f->x + f->rail_w - (scroll ? 3u : 1u) - text_x);
    const uint16_t nx = (uint16_t)(f->x + f->rail_w);

    for (uint8_t i = 0; i < vis; i++) {
        const uint8_t row = (uint8_t)(fr + i);
        const uint16_t y = (uint16_t)(f->y + i * f->row_h);
        if (row >= f->n_rows) {
            lcd_fill_rect(f->x, y, f->rail_w, f->row_h, th->status_bar_bg);
            lcd_fill_rect(nx, y, MENU_FRAME_NOTCH_W, f->row_h, th->background);
            continue;
        }
        const bool is_sel = (row == sel);
        const uint16_t bg = is_sel ? th->menu_selected_bg : th->status_bar_bg;
        const uint16_t ty = (uint16_t)(y + (f->row_h - font_medium.height) / 2u);

        /* Row: accent bar, fill around the text band, the text box. */
        lcd_fill_rect(f->x, y, 3, f->row_h,
                      is_sel ? (rail_focus ? th->highlight : th->text_secondary) : bg);
        lcd_fill_rect((uint16_t)(f->x + 3u), y, (uint16_t)(f->rail_w - 3u), (uint16_t)(ty - y), bg);
        lcd_fill_rect((uint16_t)(f->x + 3u), (uint16_t)(ty + font_medium.height), (uint16_t)(f->rail_w - 3u),
                      (uint16_t)(y + f->row_h - ty - font_medium.height), bg);
        lcd_fill_rect((uint16_t)(f->x + 3u), ty, (uint16_t)(text_x - f->x - 3u), font_medium.height, bg);
        if (f->icon)
            f->icon(row, (uint16_t)(f->x + 8u), (uint16_t)(y + (f->row_h - 12u) / 2u),
                    th->text_primary, bg);
        font_draw_string_box(text_x, ty, text_w, f->label(row),
                             is_sel ? th->text_primary : th->text_secondary, bg,
                             &font_medium, FONT_ALIGN_LEFT);
        lcd_fill_rect((uint16_t)(text_x + text_w), ty,
                      (uint16_t)(f->x + f->rail_w - text_x - text_w), font_medium.height, bg);

        /* Notch column: a triangle toward the pane on the selected row. */
        lcd_fill_rect(nx, y, MENU_FRAME_NOTCH_W, f->row_h, th->background);
        if (is_sel) {
            const uint16_t cy = (uint16_t)(y + f->row_h / 2u);
            for (uint8_t c = 0; c < MENU_FRAME_NOTCH_W; c++) {
                const uint8_t half = (uint8_t)(MENU_FRAME_NOTCH_W - c);
                lcd_fill_rect((uint16_t)(nx + c), (uint16_t)(cy - half), 1, (uint16_t)(2u * half),
                              th->menu_selected_bg);
            }
        }
    }
    /* Rail below the last whole row. */
    const uint16_t used = (uint16_t)(vis * f->row_h);
    if (used < f->h) {
        lcd_fill_rect(f->x, (uint16_t)(f->y + used), f->rail_w, (uint16_t)(f->h - used), th->status_bar_bg);
        lcd_fill_rect(nx, (uint16_t)(f->y + used), MENU_FRAME_NOTCH_W, (uint16_t)(f->h - used), th->background);
    }
    /* Scroll bar on the rail's right edge, only when the rows do not fit. */
    if (scroll) {
        const uint16_t sx = (uint16_t)(f->x + f->rail_w - 2u);
        const uint16_t th_h = (uint16_t)((uint32_t)f->h * vis / f->n_rows);
        const uint16_t th_y = (uint16_t)((uint32_t)f->h * fr / f->n_rows);
        lcd_fill_rect(sx, f->y, 2, f->h, th->background);
        lcd_fill_rect(sx, (uint16_t)(f->y + th_y), 2, th_h ? th_h : 1u, th->text_secondary);
    }
}

void menu_frame_clear_pane(const menu_frame_t *f)
{
    lcd_fill_rect(menu_frame_pane_x(f), f->y, menu_frame_pane_w(f), f->h, theme_get()->background);
}

void menu_frame_draw_title(const menu_frame_t *f, const char *title)
{
    const theme_t *th = theme_get();
    const uint16_t px = menu_frame_pane_x(f), pw = menu_frame_pane_w(f);
    lcd_fill_rect(px, f->y, pw, 2, th->background);
    font_draw_string_box((uint16_t)(px + 4u), (uint16_t)(f->y + 2u), (uint16_t)(pw - 4u), title,
                         th->highlight, th->background, &font_medium, FONT_ALIGN_LEFT);
    lcd_fill_rect(px, (uint16_t)(f->y + 2u), 4, font_medium.height, th->background);
    lcd_fill_rect(px, (uint16_t)(f->y + 2u + font_medium.height), pw,
                  (uint16_t)(MENU_FRAME_TITLE_H - 2u - font_medium.height), th->background);
}

void menu_frame_draw_footer(const menu_frame_t *f, const char *text)
{
    const theme_t *th = theme_get();
    const uint16_t px = menu_frame_pane_x(f), pw = menu_frame_pane_w(f);
    const uint16_t y = (uint16_t)(f->y + f->h - MENU_FRAME_FOOTER_H);
    lcd_fill_rect(px, y, pw, 1, th->background);
    font_draw_string_box((uint16_t)(px + 4u), (uint16_t)(y + 1u), (uint16_t)(pw - 4u),
                         text ? text : "", th->text_secondary, th->background,
                         &font_small, FONT_ALIGN_LEFT);
    lcd_fill_rect(px, (uint16_t)(y + 1u), 4, font_small.height, th->background);
    lcd_fill_rect(px, (uint16_t)(y + 1u + font_small.height), pw,
                  (uint16_t)(MENU_FRAME_FOOTER_H - 1u - font_small.height), th->background);
}

void menu_frame_radio(const menu_frame_t *f, uint16_t y, uint16_t h, const char *label,
                      bool focused, bool checked, bool enabled)
{
    const theme_t *th = theme_get();
    const uint16_t x = (uint16_t)(menu_frame_pane_x(f) + 4u);
    const uint16_t w = (uint16_t)(menu_frame_pane_w(f) - 8u);
    const uint16_t bg = focused ? th->menu_selected_bg : th->status_bar_bg;
    const uint16_t ring = !enabled ? th->text_secondary : (checked ? th->highlight : th->text_primary);

    menu_frame_box(x, y, w, h, bg, focused ? th->highlight : bg, 1);
    round_corners(x, y, w, h, th->background);
    const uint16_t dy = (uint16_t)(y + (h - 10u) / 2u);
    disc((uint16_t)(x + 13u), dy, k_disc10, 10, ring);
    disc((uint16_t)(x + 13u), (uint16_t)(dy + 1u), k_disc8, 8, bg);
    if (checked) disc((uint16_t)(x + 13u), (uint16_t)(dy + 3u), k_disc4, 4, ring);
    font_draw_string_box((uint16_t)(x + 26u), (uint16_t)(y + (h - font_medium.height) / 2u),
                         (uint16_t)(w - 30u), label,
                         enabled ? th->text_primary : th->text_secondary, bg,
                         &font_medium, FONT_ALIGN_LEFT);
}

void menu_frame_value_row(const menu_frame_t *f, uint16_t y, uint16_t h, const char *label,
                          const char *value, bool focused, bool enabled)
{
    const theme_t *th = theme_get();
    const uint16_t x = (uint16_t)(menu_frame_pane_x(f) + 4u);
    const uint16_t w = (uint16_t)(menu_frame_pane_w(f) - 8u);
    const uint16_t bg = focused ? th->menu_selected_bg : th->status_bar_bg;
    const uint16_t vw = 96u, vh = (uint16_t)(font_medium.height + 4u);
    const uint16_t vx = (uint16_t)(x + w - vw - 4u), vy = (uint16_t)(y + (h - vh) / 2u);
    const uint16_t fg = enabled ? th->text_primary : th->text_secondary;

    lcd_fill_rect(x, y, w, h, bg);
    if (focused) lcd_fill_rect(x, (uint16_t)(y + 2u), 3, (uint16_t)(h - 4u), th->highlight);  /* the cursor */
    round_corners(x, y, w, h, th->background);
    font_draw_string_box((uint16_t)(x + 8u), (uint16_t)(y + (h - font_medium.height) / 2u),
                         (uint16_t)(vx - x - 12u), label, fg, bg, &font_medium, FONT_ALIGN_LEFT);
    menu_frame_box(vx, vy, vw, vh, th->background,
                   focused && enabled ? th->highlight : th->text_secondary, 1);
    font_draw_string_box((uint16_t)(vx + 2u), (uint16_t)(vy + 2u), (uint16_t)(vw - 4u), value,
                         enabled ? (focused ? th->highlight : th->text_primary) : th->text_secondary,
                         th->background, &font_medium, FONT_ALIGN_CENTER);
}

void menu_frame_chip(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const char *label,
                     uint16_t on_color, bool on, bool focused, bool enabled)
{
    const theme_t *th = theme_get();
    /* Focus inverts the chip (filled, dark text), so the cursor reads at a
     * glance whether the chip is on (channel colour) or off (grey). */
    const uint16_t bg = focused ? (on ? on_color : th->text_secondary) : th->status_bar_bg;
    const uint16_t border = focused ? bg : (on ? on_color : th->text_secondary);
    uint16_t fg = focused ? th->background : (on ? on_color : th->text_secondary);
    if (!enabled && !focused) fg = th->text_secondary;

    menu_frame_box(x, y, w, h, bg, border, on ? 2u : 1u);
    round_corners(x, y, w, h, th->background);
    const uint8_t in = on ? 2u : 1u;
    const uint16_t ty = (uint16_t)(y + (h - font_small.height) / 2u);
    font_draw_string_box((uint16_t)(x + in), ty, (uint16_t)(w - 2u * in), label, fg, bg,
                         &font_small, FONT_ALIGN_CENTER);
}
