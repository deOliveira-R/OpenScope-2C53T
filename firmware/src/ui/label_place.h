/*
 * OpenScope 2C53T - where a label may go on a plot
 *
 * A plot's labels (the spectrum's header, dB scale, axis ends and peak tags;
 * the cursor lines' V1/V2/H1/H2) are placed one after another, the most
 * important first. Each label has a list of candidate boxes, the position it
 * prefers first; it takes the first candidate that lies inside the plot and
 * meets no box placed before it, or it is left out. So two labels never
 * overprint, and where nothing collides every label is where it always was.
 * Pure: boxes in, a choice out (host test: make test-label-place).
 */
#ifndef LABEL_PLACE_H
#define LABEL_PLACE_H

#include <stdbool.h>
#include <stdint.h>

/* A label's box on the screen: its top-left pixel, width and height. */
typedef struct {
    int16_t x, y, w, h;
} label_box_t;

/* a and b share at least one pixel. */
static inline bool label_boxes_meet(const label_box_t *a, const label_box_t *b)
{
    return a->x < b->x + b->w && b->x < a->x + a->w &&
           a->y < b->y + b->h && b->y < a->y + a->h;
}

/* b lies wholly inside area. */
static inline bool label_box_inside(const label_box_t *b, const label_box_t *area)
{
    return b->x >= area->x && b->y >= area->y &&
           b->x + b->w <= area->x + area->w && b->y + b->h <= area->y + area->h;
}

/* The column a label w wide starting at x keeps on area: x, moved in from
 * the edge it would cross. */
static inline int16_t label_x_within(int16_t x, int16_t w, const label_box_t *area)
{
    if (x + w > area->x + area->w) x = (int16_t)(area->x + area->w - w);
    return x < area->x ? area->x : x;
}

/* The index of the first of the candidates cand[0..nc) that lies inside area
 * and meets none of the boxes placed[0..np); -1 when none does. */
static inline int label_place(const label_box_t *area, const label_box_t *cand, uint8_t nc,
                              const label_box_t *placed, uint8_t np)
{
    for (uint8_t i = 0; i < nc; i++) {
        if (!label_box_inside(&cand[i], area)) continue;
        bool free = true;
        for (uint8_t k = 0; k < np && free; k++) free = !label_boxes_meet(&cand[i], &placed[k]);
        if (free) return (int)i;
    }
    return -1;
}

#endif /* LABEL_PLACE_H */
