/*
 * OpenScope 2C53T - canvas: one rectangle of the screen and its draw cycle
 *
 * A canvas is the only painter of its rectangle (docs/design/
 * display_architecture.md). A frame goes strip by strip: the caller's paint
 * function fills the strip buffer with the strip's final pixels, the canvas
 * compares them with what the strip held when it was last sent, and sends
 * the strip only when it changed. A full redraw is the same cycle with every
 * strip sent. Nothing is ever blanked on the glass: a strip goes from its old
 * pixels to its new ones in one write.
 *
 * Strips are columns (canvas_draw_columns); row strips come with the first
 * display type that needs them (the waterfall).
 *
 * One canvas is drawn at a time (the scope's plot), so the strip buffer and
 * the memory of what was sent are one static set, not one per canvas.
 * Display task only.
 */

#ifndef CANVAS_H
#define CANVAS_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint16_t x, y, w, h;
} canvas_rect_t;

/* Holes a frame may have: rectangles inside the canvas that belong to
 * somebody else (a popup box) and are stepped around. */
#define CANVAS_MAX_HOLES 3u

/* One strip handed to the paint function: px[i] is the pixel at coordinate
 * lo + i along the strip, for lo <= coordinate < hi (rows, for a column
 * strip). Elements write through strip_put and strip_span, which drop what
 * falls outside. */
typedef struct canvas_strip {
    uint16_t *px;
    int16_t   lo, hi;
} canvas_strip_t;

static inline void strip_put(const canvas_strip_t *s, int at, uint16_t color)
{
    if (at >= s->lo && at < s->hi) s->px[at - s->lo] = color;
}

/* Coordinates first..last, both included. */
static inline void strip_span(const canvas_strip_t *s, int first, int last, uint16_t color)
{
    if (first < s->lo) first = s->lo;
    if (last > s->hi - 1) last = s->hi - 1;
    for (int at = first; at <= last; at++) s->px[at - s->lo] = color;
}

/* Paint strip `index` (a column strip: screen column index) into s. Called
 * once per strip, in increasing index order; it must write every pixel of
 * the strip. */
typedef void (*canvas_paint_fn)(void *ctx, uint16_t index, const canvas_strip_t *s);

typedef struct {
    canvas_rect_t        rect;      /* the canvas, in screen coordinates */
    const canvas_rect_t *holes;     /* nholes rectangles, or NULL */
    uint8_t              nholes;    /* at most CANVAS_MAX_HOLES */
    bool                 all;       /* send every strip (a full redraw) */
    canvas_paint_fn      paint;
    void                *ctx;       /* handed to paint */
} canvas_frame_t;

/* Draw one frame in column strips. Returns the number of columns sent; the
 * others were unchanged. Strips are trusted as unchanged only while the
 * rectangle and the holes are the ones of the last frame and nobody else has
 * drawn on the rectangle's rows since (lcd_watch_rows); otherwise every
 * strip is sent. A hole clipped to nothing by the rectangle is ignored. */
uint16_t canvas_draw_columns(const canvas_frame_t *f);

#endif /* CANVAS_H */
