/*
 * OpenScope 2C53T - home screen drawing (stock's mode-selection page).
 *
 * The model is home_model.c; this file only paints it. Geometry follows
 * stock V1.2.0's display commands 0x0B..0x11 (0x0800FFB8..0x080102B8, all
 * gated on ms[0xF68] == 0), which place JPEGs from the W25Q volume
 * "3:/System file/":
 *     (105, 13)  39+sel.jpg   per-selection top strip   -> our page dots [H]
 *     ( 89, 39)  36.jpg       static, behind the icon   -> our frame [H]
 *     (  0, 60)  37.jpg       static, left side         -> our left chevron [H]
 *     (250, 60)  38.jpg       static, right side        -> our right chevron [H]
 *     (106, 75)  51+sel.jpg   centre icon, 108 x 70 (52.JPG measured) [V]
 *     (  0, 95)  43+sel.jpg   left neighbour preview [H]
 *     (266, 95)  47+sel.jpg   right neighbour preview [H]
 *     ( 75,190)  170 x 25 box, item name in 0x055F, centred [V]
 * OpenScope has no JPEG decoder and the images' contents are unknown without
 * a W25Q dump, so the pictures are drawn as simple vector icons at stock's
 * positions. No buffers: everything goes straight to the LCD.
 *
 * Ours, not stock: the top status bar and bottom hint bar stay (stock's page
 * starts at y = 13 with no bars), so the dots sit just under the status bar.
 */
#include "ui.h"
#include "lcd.h"
#include "font.h"
#include "theme.h"
#include "home_model.h"

static home_t s_home = HOME_INIT;

home_t *home_ui_model(void)
{
    return &s_home;
}

#define HOME_SEL_COLOR    0x055Fu   /* stock's highlight (label colour, table 0x0804C180) */
#define HOME_DIM_COLOR    0x8C73u   /* stock's unselected grey (0x0804C180[0]) */

#define ICON_W   108
#define ICON_H    70
#define ICON_X   106
#define ICON_Y    75
#define NB_W      54
#define NB_H      35
#define NB_Y      (ICON_Y + (ICON_H - NB_H) / 2)
#define NB_LX      6
#define NB_RX     (LCD_WIDTH - 6 - NB_W)
#define FRAME_X   89
#define FRAME_Y   39
#define FRAME_W  142
#define FRAME_H  142
#define DOTS_Y    28
#define LABEL_X   75
#define LABEL_Y  190
#define LABEL_W  170
#define LABEL_H   25

/* One cycle, 32 points, amplitude 100. */
static const int8_t k_sine32[32] = {
    0, 20, 38, 56, 71, 83, 92, 98, 100, 98, 92, 83, 71, 56, 38, 20,
    0, -20, -38, -56, -71, -83, -92, -98, -100, -98, -92, -83, -71, -56, -38, -20
};

static void rect_outline(int x, int y, int w, int h, uint16_t c)
{
    lcd_fill_rect((uint16_t)x, (uint16_t)y, (uint16_t)w, 1, c);
    lcd_fill_rect((uint16_t)x, (uint16_t)(y + h - 1), (uint16_t)w, 1, c);
    lcd_fill_rect((uint16_t)x, (uint16_t)y, 1, (uint16_t)h, c);
    lcd_fill_rect((uint16_t)(x + w - 1), (uint16_t)y, 1, (uint16_t)h, c);
}

static void circle(int cx, int cy, int r, uint16_t c, bool fill)
{
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        if (fill) {
            lcd_fill_rect((uint16_t)(cx - x), (uint16_t)(cy + y), (uint16_t)(2 * x + 1), 1, c);
            lcd_fill_rect((uint16_t)(cx - x), (uint16_t)(cy - y), (uint16_t)(2 * x + 1), 1, c);
            lcd_fill_rect((uint16_t)(cx - y), (uint16_t)(cy + x), (uint16_t)(2 * y + 1), 1, c);
            lcd_fill_rect((uint16_t)(cx - y), (uint16_t)(cy - x), (uint16_t)(2 * y + 1), 1, c);
        } else {
            lcd_set_pixel((uint16_t)(cx + x), (uint16_t)(cy + y), c);
            lcd_set_pixel((uint16_t)(cx - x), (uint16_t)(cy + y), c);
            lcd_set_pixel((uint16_t)(cx + x), (uint16_t)(cy - y), c);
            lcd_set_pixel((uint16_t)(cx - x), (uint16_t)(cy - y), c);
            lcd_set_pixel((uint16_t)(cx + y), (uint16_t)(cy + x), c);
            lcd_set_pixel((uint16_t)(cx - y), (uint16_t)(cy + x), c);
            lcd_set_pixel((uint16_t)(cx + y), (uint16_t)(cy - x), c);
            lcd_set_pixel((uint16_t)(cx - y), (uint16_t)(cy - x), c);
        }
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

/* A sine across [x0, x0+w) centred on cy, 2 px thick, `cycles` periods. */
static void sine(int x0, int w, int cy, int amp, int cycles, uint16_t c)
{
    for (int i = 0; i < w; i++) {
        const int ph = (i * 32 * cycles / w) & 31;
        const int y = cy - (k_sine32[ph] * amp) / 100;
        lcd_fill_rect((uint16_t)(x0 + i), (uint16_t)y, 1, 2, c);
    }
}

/* A square wave across [x0, x0+w), `cycles` periods. */
static void square(int x0, int w, int cy, int amp, int cycles, uint16_t c)
{
    const int half = w / (2 * cycles);
    int x = x0;
    bool hi = true;
    while (x < x0 + w) {
        const int seg = (x + half <= x0 + w) ? half : (x0 + w - x);
        lcd_fill_rect((uint16_t)x, (uint16_t)(hi ? cy - amp : cy + amp), (uint16_t)seg, 2, c);
        x += seg;
        if (x < x0 + w)
            lcd_fill_rect((uint16_t)x, (uint16_t)(cy - amp), 2, (uint16_t)(2 * amp + 2), c);
        hi = !hi;
    }
}

/* The item's picture in a w x h box (the centre icon is 108 x 70, the
 * neighbour previews half that). `c` is the line colour, `a` the accent. */
static void draw_icon(home_item_t it, int x, int y, int w, int h, uint16_t c, uint16_t a)
{
    const uint16_t bg = theme_get()->background;
    lcd_fill_rect((uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h, bg);

    switch (it) {
    case HOME_ITEM_METER: {
        /* Meter body: a display window over a selector dial. */
        const int bw = w * 5 / 8, bx = x + (w - bw) / 2;
        rect_outline(bx, y, bw, h, c);
        rect_outline(bx + bw / 8, y + h / 8, bw - bw / 4, h / 3, a);
        circle(bx + bw / 2, y + h * 2 / 3 + 1, h / 6, c, false);
        lcd_fill_rect((uint16_t)(bx + bw / 2), (uint16_t)(y + h / 2 + 2), 2,
                      (uint16_t)(h / 6), a);
        break;
    }
    case HOME_ITEM_SCOPE: {
        /* A screen with a grid and a trace. */
        rect_outline(x, y, w, h, c);
        for (int i = 1; i < 4; i++)
            lcd_fill_rect((uint16_t)(x + w * i / 4), (uint16_t)(y + 1), 1, (uint16_t)(h - 2), COLOR_GRID);
        lcd_fill_rect((uint16_t)(x + 1), (uint16_t)(y + h / 2), (uint16_t)(w - 2), 1, COLOR_GRID);
        sine(x + 2, w - 4, y + h / 2, h / 3, 2, a);
        break;
    }
    case HOME_ITEM_SIGGEN:
        /* A square over a sine: what comes out of the generator. */
        rect_outline(x, y, w, h, c);
        square(x + w / 8, w * 3 / 4, y + h / 3, h / 8, 2, a);
        sine(x + w / 8, w * 3 / 4, y + h * 2 / 3, h / 8, 2, c);
        break;
    case HOME_ITEM_SETTINGS:
    default: {
        /* A gear. */
        const int cx = x + w / 2, cy = y + h / 2, r = h * 3 / 10;
        const int t = (h >= 50) ? 8 : 4;              /* tooth size */
        static const int8_t dx[8] = { 0, 7, 10, 7, 0, -7, -10, -7 };
        static const int8_t dy[8] = { -10, -7, 0, 7, 10, 7, 0, -7 };
        for (int i = 0; i < 8; i++) {
            const int tx = cx + dx[i] * (r + t / 2) / 10 - t / 2;
            const int ty = cy + dy[i] * (r + t / 2) / 10 - t / 2;
            lcd_fill_rect((uint16_t)tx, (uint16_t)ty, (uint16_t)t, (uint16_t)t, c);
        }
        circle(cx, cy, r, c, true);
        circle(cx, cy, r / 2, bg, true);
        circle(cx, cy, r / 2, a, false);
        break;
    }
    }
}

static void draw_dots(uint8_t sel)
{
    const theme_t *th = theme_get();
    const int n = (int)HOME_ITEM_COUNT, pitch = 16;
    const int x0 = LCD_WIDTH / 2 - (n - 1) * pitch / 2;
    lcd_fill_rect((uint16_t)(x0 - 8), DOTS_Y - 5, (uint16_t)((n - 1) * pitch + 16), 11, th->background);
    for (int i = 0; i < n; i++)
        circle(x0 + i * pitch, DOTS_Y, (i == sel) ? 4 : 3,
               (i == sel) ? HOME_SEL_COLOR : HOME_DIM_COLOR, true);
}

static void chevron(int x, int cy, int dir, uint16_t c)
{
    /* 6 rows each side of the tip, 2 px strokes. */
    for (int i = 0; i <= 6; i++) {
        const int off = (dir < 0) ? i : (6 - i);
        lcd_fill_rect((uint16_t)(x + off), (uint16_t)(cy - 6 + i), 2, 1, c);
        lcd_fill_rect((uint16_t)(x + off), (uint16_t)(cy + 6 - i), 2, 1, c);
    }
}

void draw_home_selection(void)
{
    const theme_t *th = theme_get();
    const uint8_t sel = s_home.sel < (uint8_t)HOME_ITEM_COUNT ? s_home.sel : (uint8_t)HOME_ITEM_SCOPE;

    draw_dots(sel);
    draw_icon((home_item_t)sel, ICON_X, ICON_Y, ICON_W, ICON_H, th->text_primary, HOME_SEL_COLOR);
    draw_icon(home_neighbour(sel, -1), NB_LX, NB_Y, NB_W, NB_H, HOME_DIM_COLOR, HOME_DIM_COLOR);
    draw_icon(home_neighbour(sel, +1), NB_RX, NB_Y, NB_W, NB_H, HOME_DIM_COLOR, HOME_DIM_COLOR);

    /* Stock clears the 170 x 25 box, then centres the name in it. The box
     * writer paints every pixel once, so a change does not flash. */
    const int fh = font_medium.height;
    const int ty = LABEL_Y + (LABEL_H - fh) / 2;
    lcd_fill_rect(LABEL_X, LABEL_Y, LABEL_W, (uint16_t)(ty - LABEL_Y), th->background);
    lcd_fill_rect(LABEL_X, (uint16_t)(ty + fh), LABEL_W, (uint16_t)(LABEL_Y + LABEL_H - ty - fh), th->background);
    font_draw_string_box(LABEL_X, (uint16_t)ty, LABEL_W,
                         home_item_label((home_item_t)sel), HOME_SEL_COLOR, th->background,
                         &font_medium, FONT_ALIGN_CENTER);
}

void draw_home_screen(void)
{
    const theme_t *th = theme_get();
    lcd_fill_rect(0, 16, LCD_WIDTH, LCD_HEIGHT - 32, th->background);
    rect_outline(FRAME_X, FRAME_Y, FRAME_W, FRAME_H, HOME_DIM_COLOR);
    chevron(NB_LX + NB_W + 8, ICON_Y + ICON_H / 2, -1, th->text_secondary);
    chevron(NB_RX - 8 - 8, ICON_Y + ICON_H / 2, +1, th->text_secondary);
    draw_home_selection();
}
