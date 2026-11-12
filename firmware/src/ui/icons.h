/*
 * OpenScope 2C53T - small glyphs for the bars (status, info) and later screens
 *
 * Pure: no LCD, no FreeRTOS, so tests/test_icons.c runs this very code. Every
 * glyph is const data (flash, no RAM) or computed from a const description,
 * and is recolourable: the caller picks fg/bg per draw.
 *
 * Output is one ROW at a time into a caller's uint16_t line[w] (RGB565), which
 * the caller blits with lcd_blit_rect(x, y + row, w, 1, line). A row costs at
 * most 2 * ICON_MAX_W bytes of stack, and the bus sees one window per row
 * rather than one per pixel.
 *
 * Families (UI_STUDY.md section 4.5):
 *   bitmaps   1-bit rows, MSB = leftmost pixel, width <= 16: the edge glyphs
 *             (9x12), the arrow-function arrows, the trigger disc and its "T",
 *             the trigger-state dot, the charging bolt;
 *   battery   22x10 outline + nub + 4 bars (B1), computed;
 *   wave      16x12 generator waveform icon (S8): 16 points sampled from the
 *             very cycle the DAC plays (siggen_wave_build), not a drawing;
 *   tab       a sheared tab (stock's trapezoid bar tabs), any width.
 */
#ifndef ICONS_H
#define ICONS_H

#include <stdint.h>
#include <stdbool.h>

#define ICON_MAX_W   32u     /* widest row any icon_* function writes */
#define ICON_MAX_H   16u     /* tallest glyph: one bar height */

typedef enum {
    ICON_EDGE_RISE = 0,      /* 9x12: low-left, step up, high-right, arrow up */
    ICON_EDGE_FALL,          /* 9x12: mirror image, arrow down */
    ICON_ARROW_LEFT,         /* 7x7  filled triangle pointing left (trigger level marker) */
    ICON_ARROW_RIGHT,        /* 7x7  pointing right (channel ground marker) */
    ICON_ARROW_DOWN,         /* 9x5  pointing down (trigger position marker) */
    ICON_ARROW_UP,           /* 9x5  pointing up */
    ICON_TRIG_DISC,          /* 11x11 filled disc */
    ICON_TRIG_T,             /* 11x11 the "T" drawn over the disc */
    ICON_DOT,                /* 7x7  filled dot (trigger state) */
    ICON_BOLT,               /* 8x6  charging bolt, sits in the battery's interior */
    ICON_COUNT
} icon_id_t;

typedef struct {
    uint8_t         w, h;    /* w <= 16, h <= ICON_MAX_H */
    const uint16_t *rows;    /* h rows, bit 15 = x 0 */
} icon_bitmap_t;

/* NULL for an out-of-range id. */
const icon_bitmap_t *icon_bitmap(icon_id_t id);

/* Is pixel (x, y) set? false outside the icon. */
bool icon_px(const icon_bitmap_t *ic, uint8_t x, uint8_t y);

/* Row `row` of the icon into line[0..w-1]: fg where set, bg elsewhere.
 * A row >= h, or a NULL icon, writes nothing. */
void icon_row(const icon_bitmap_t *ic, uint8_t row, uint16_t fg, uint16_t bg,
              uint16_t *line);
/* The same, but only the SET pixels are written (an overlay on a row already
 * built, e.g. the "T" over the disc, the bolt over the battery). `dx` shifts
 * the overlay right inside the line; `line_w` is the line's width (pixels
 * that would land at or past it are dropped). */
void icon_row_over(const icon_bitmap_t *ic, uint8_t row, uint8_t dx, uint16_t fg,
                   uint16_t *line, uint8_t line_w);

/* ── Battery (B1) ─────────────────────────────────────────────────── */
#define ICON_BATT_W     22u
#define ICON_BATT_H     10u
#define ICON_BATT_BARS  4u
/* Bars lit for a percentage: 0 -> 0, 1..25 -> 1, ... 76..100 -> 4. */
uint8_t icon_battery_bars(uint8_t pct);
/* One row: outline/nub in `fg`, lit bars in `fill`, the rest `bg`. With
 * `charging` the bars are left empty and the bolt (ICON_BOLT) is drawn in
 * `fill` (a voltage-based % is meaningless on the charger). */
void icon_battery_row(uint8_t row, uint8_t bars, bool charging,
                      uint16_t fg, uint16_t fill, uint16_t bg, uint16_t *line);

/* ── Generator waveform icon (S8) ─────────────────────────────────── */
#define ICON_WAVE_W  16u
#define ICON_WAVE_H  12u
/* y (0 = top .. ICON_WAVE_H-1) of each of the 16 columns, from one cycle of
 * the DAC table for `wave` at `duty_pct`, scaled to the min..max of those 16
 * points (a flat wave, DC, sits on the middle row). An unknown wave gives DC. */
void icon_wave_points(uint8_t wave, uint8_t duty_pct, uint8_t y[ICON_WAVE_W]);
/* One row of the polyline through those points: each column is filled from
 * its own y to the previous column's, so the steps stay connected. */
void icon_wave_row(const uint8_t y[ICON_WAVE_W], uint8_t row, uint16_t fg,
                   uint16_t bg, uint16_t *line);

/* ── Sheared tab ──────────────────────────────────────────────────── */
/* Stock's bar tabs are wider at the top: each side steps in by one pixel every
 * ICON_TAB_SHEAR rows. This gives the inset of a row; the caller fills
 * [x + inset, x + w - inset) per row (two fill_rects per row would be wasted:
 * one run per row is enough), so a tab can be wider than a line buffer. */
#define ICON_TAB_SHEAR  3u
uint8_t icon_tab_inset(uint8_t row);

#endif /* ICONS_H */
