/*
 * OpenScope 2C53T - the waterfall's drawing (waterfall_blit.h)
 */
#include "waterfall_blit.h"

#include <stddef.h>

/*
 * Intensity -> colour, with a BLACK floor.
 *
 * intensity 0 = at/above the display reference, 255 = at/below the floor
 * (reference - db_range). The ramp runs black -> blue -> cyan -> yellow ->
 * red in four equal 64-step segments, the conventional spectrogram ordering,
 * and puts "nothing here" (the noise floor) at black.
 */
uint16_t waterfall_color(uint8_t intensity)
{
    uint16_t level = (uint16_t)(255u - intensity);   /* 0 = floor, 255 = hot */
    uint8_t  t = (uint8_t)((level & 63u) * 4u);      /* position in segment  */

    if (level < 64)  return RGB565(0, 0, t);                        /* black->blue  */
    if (level < 128) return RGB565(0, t, 255);                      /* blue ->cyan  */
    if (level < 192) return RGB565(t, 255, (uint8_t)(255 - t));     /* cyan ->yellow*/
    return RGB565(255, (uint8_t)(255 - t), 0);                      /* yellow->red  */
}

/*
 * The plot is not cleared first. Every screen line of the plot is written
 * once: a history row is converted once into waterfall_line and streamed
 * row_height times, one window per screen line (lcd_rect_begin: set up with
 * no bus delays) and its pixels (lcd_rect_row: the screenshot shadow kept a
 * line at a time). The residual lines under the block (the plot height is not a multiple
 * of the rows: 14 lines at 206/64) are written black, or with the axis
 * labels.
 *
 * A window per line costs lcd_rect_begin's eleven bus writes (about 1 us)
 * against 320 pixels, so the block is not sent as one window: a line is the
 * unit the history and the labels are composed in anyway.
 *
 * Why not scroll-and-append, which would be O(1 row)? The history scrolls
 * by exactly one row per frame, so 63 of 64 rows are unchanged in content,
 * but they all MOVE, and with no framebuffer on this side of the bus there
 * is nothing to shift. The ST7789's own vertical scroll works on panel
 * rows, which MADCTL = 0xA0 (MV = 1) maps to screen COLUMNS, i.e. it would
 * scroll the waterfall sideways. An O(1)-per-frame waterfall needs either
 * a RAM framebuffer (the shared pool is already used by the FFT) or a
 * different history layout that lets the newest row wrap in place instead
 * of the image scrolling. That changes what is drawn. */
void waterfall_blit(const waterfall_frame_t *f)
{
    uint16_t row_height = (uint16_t)(f->height / f->rows);
    if (row_height < 1) row_height = 1;

    uint16_t rows_drawn = f->rows;
    if ((uint32_t)rows_drawn * row_height > f->height)
        rows_drawn = (uint16_t)(f->height / row_height);
    const uint16_t block_h = (uint16_t)(rows_drawn * row_height);

    for (uint16_t sy = 0; sy < f->height; sy++) {
        const uint16_t y = (uint16_t)(f->top + sy);
        const bool in_block = sy < block_h;
        const uint16_t r = (uint16_t)(sy / row_height);
        const uint8_t *src = in_block ? f->history[(f->newest + f->rows - r) % f->rows] : NULL;
        /* A history row is converted once and reused for its row_height
         * lines unless a label crosses this line. */
        const bool row_first = in_block && (sy % row_height) == 0u;
        const bool text_line = f->text_line(f->text, y);
        if (row_first || text_line || !in_block) {
            for (uint16_t x = 0; x < WATERFALL_BLIT_COLS; x++)
                f->line[x] = src ? waterfall_color(src[x]) : COLOR_BLACK;
            if (text_line) f->text_row(f->text, y, f->line);
        }
        lcd_rect_begin(0, y, WATERFALL_BLIT_COLS, 1);
        lcd_rect_row(f->line);
        if (text_line && in_block && (sy + 1u) % row_height != 0u) {
            /* the next line of this row has no text: rebuild it plain */
            for (uint16_t x = 0; x < WATERFALL_BLIT_COLS; x++)
                f->line[x] = waterfall_color(src[x]);
        }
    }
}
