/*
 * OpenScope 2C53T - the waterfall's drawing: history rows to LCD lines.
 *
 * Holds no state and needs only the LCD calls, so
 * tests/test_waterfall_blit.c runs this code on the host against a model LCD.
 */
#ifndef WATERFALL_BLIT_H
#define WATERFALL_BLIT_H

#include <stdbool.h>
#include <stdint.h>

#include "lcd.h"

/* A history row is exactly one screen line wide. */
#define WATERFALL_BLIT_COLS LCD_WIDTH

/* Text composed into the lines it crosses. */
typedef bool (*waterfall_text_line_fn)(const void *text, int y);
typedef bool (*waterfall_text_px_fn)(const void *text, int x, int y, uint16_t *c);

typedef struct {
    const uint8_t (*history)[WATERFALL_BLIT_COLS]; /* intensities: 0 = reference, 255 = floor */
    uint16_t rows;                  /* rows in history */
    uint16_t newest;                /* the row drawn at the top */
    uint16_t top, height;           /* the plot's screen lines */
    uint16_t *line;                 /* one assembled line, WATERFALL_BLIT_COLS pixels */
    const void *text;
    waterfall_text_line_fn text_line;
    waterfall_text_px_fn   text_px;
} waterfall_frame_t;

uint16_t waterfall_color(uint8_t intensity);
void waterfall_blit(const waterfall_frame_t *f);

#endif /* WATERFALL_BLIT_H */
