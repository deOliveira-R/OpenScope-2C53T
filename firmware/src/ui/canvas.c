/*
 * OpenScope 2C53T - canvas: one rectangle of the screen and its draw cycle
 */

#include "canvas.h"
#include "lcd.h"

/* The strip being painted, before it is sent. */
static uint16_t s_strip[LCD_HEIGHT];

/* What each strip held when it was last sent: a 32-bit FNV-1a hash of its
 * 16-bit pixels. A strip whose new pixels hash the same is not sent again;
 * with a steady picture that is most of them. Two different strips hashing
 * alike would leave the old one on the glass until it changes again (one
 * chance in 2^32 for each change). */
static uint32_t s_hash[LCD_WIDTH];

#define CANVAS_HASH_SEED   2166136261u     /* FNV-1a offset basis */
#define CANVAS_HASH_PRIME  16777619u       /* FNV-1a prime */

/* What s_hash describes: the rectangle and holes of the last frame. Not
 * valid before the first frame. */
static bool          s_valid;
static canvas_rect_t s_rect;
static canvas_rect_t s_holes[CANVAS_MAX_HOLES];
static uint8_t       s_nholes;

static bool rect_equal(const canvas_rect_t *a, const canvas_rect_t *b)
{
    return a->x == b->x && a->y == b->y && a->w == b->w && a->h == b->h;
}

/* Clip hole to rect. False when nothing of it is left. */
static bool hole_clip(const canvas_rect_t *rect, const canvas_rect_t *hole, canvas_rect_t *out)
{
    uint16_t x0 = hole->x, x1 = (uint16_t)(hole->x + hole->w);
    uint16_t y0 = hole->y, y1 = (uint16_t)(hole->y + hole->h);
    if (x0 < rect->x) x0 = rect->x;
    if (y0 < rect->y) y0 = rect->y;
    if (x1 > rect->x + rect->w) x1 = (uint16_t)(rect->x + rect->w);
    if (y1 > rect->y + rect->h) y1 = (uint16_t)(rect->y + rect->h);
    if (x0 >= x1 || y0 >= y1) return false;
    *out = (canvas_rect_t){ x0, y0, (uint16_t)(x1 - x0), (uint16_t)(y1 - y0) };
    return true;
}

uint16_t canvas_draw_columns(const canvas_frame_t *f)
{
    const canvas_rect_t *r = &f->rect;
    if (r->w == 0u || r->h == 0u || r->h > LCD_HEIGHT || r->x + r->w > LCD_WIDTH)
        return 0;

    canvas_rect_t holes[CANVAS_MAX_HOLES];
    uint8_t nholes = 0;
    for (uint8_t i = 0; i < f->nholes && nholes < CANVAS_MAX_HOLES; i++)
        if (hole_clip(r, &f->holes[i], &holes[nholes])) nholes++;

    /* May the memory of the last frame be trusted? */
    bool same = s_valid && !f->all && rect_equal(r, &s_rect) && nholes == s_nholes;
    for (uint8_t i = 0; same && i < nholes; i++)
        same = rect_equal(&holes[i], &s_holes[i]);
    lcd_watch_rows(r->y, (uint16_t)(r->y + r->h));
    if (lcd_watch_take()) same = false;

    const canvas_strip_t strip = { s_strip, (int16_t)r->y, (int16_t)(r->y + r->h) };
    uint16_t sent = 0;
    for (uint16_t x = r->x; x < r->x + r->w; x++) {
        f->paint(f->ctx, x, &strip);

        uint32_t hash = CANVAS_HASH_SEED;
        for (uint16_t i = 0; i < r->h; i++) hash = (hash ^ s_strip[i]) * CANVAS_HASH_PRIME;
        if (same && hash == s_hash[x]) continue;
        s_hash[x] = hash;
        sent++;

        /* The column's rows in runs, top to bottom, stepping over each hole
         * that covers this column. */
        uint16_t y = r->y;
        const uint16_t y_end = (uint16_t)(r->y + r->h);
        while (y < y_end) {
            uint16_t run_end = y_end;       /* the next hole's top, if one is below y */
            uint16_t skip_to = y;           /* past the hole y is inside, if any */
            for (uint8_t i = 0; i < nholes; i++) {
                const canvas_rect_t *h = &holes[i];
                if (x < h->x || x >= h->x + h->w) continue;
                if (y >= h->y && y < h->y + h->h) {
                    if (h->y + h->h > skip_to) skip_to = (uint16_t)(h->y + h->h);
                } else if (h->y > y && h->y < run_end) {
                    run_end = h->y;
                }
            }
            if (skip_to > y) { y = skip_to; continue; }
            lcd_write_column(x, y, &s_strip[y - r->y], (uint16_t)(run_end - y));
            y = run_end;
        }
    }

    /* Discard the watch marks this frame's own writes left, so the next
     * frame sees only other painters'; the memory now describes the glass. */
    (void)lcd_watch_take();
    s_valid = true;
    s_rect = *r;
    s_nholes = nholes;
    for (uint8_t i = 0; i < nholes; i++) s_holes[i] = holes[i];
    return sent;
}
