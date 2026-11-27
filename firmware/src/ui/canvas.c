/*
 * OpenScope 2C53T - canvas: one rectangle of the screen and its draw cycle
 */

#include <stddef.h>
#include "canvas.h"
#include "canvas_runs.h"
#include "lcd.h"

/* The strip being painted, before it is sent. */
static uint16_t s_strip[LCD_HEIGHT] __attribute__((aligned(CANVAS_STRIP_ALIGN)));

/* One canvas's memory: what each strip held when it was last sent (its
 * hash, canvas_runs_windows: canvas_hash, or for a layered column with the
 * feature CANVASRUNS its base's hash with its runs and their pixels; indexed
 * by screen column) and the rectangle and holes
 * that describes. Not valid before its first frame. A strip whose new pixels
 * hash the same is not sent again; with a steady picture that is most of
 * them. Two different strips hashing alike would leave the old one on the
 * glass until it changes again (one chance in 2^32 for each change). */
typedef struct {
    bool          valid;
    canvas_rect_t rect;
    canvas_rect_t holes[CANVAS_MAX_HOLES];
    uint8_t       nholes;
    uint32_t      hash[LCD_WIDTH];
} canvas_mem_t;

static canvas_mem_t s_mem[CANVAS_COUNT];

_Static_assert(CANVAS_COUNT <= LCD_WATCH_SLOTS, "a watch slot per canvas");

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

/* Send rows [y, y_end) of column x from the strip (which starts at row y0),
 * stepping over each hole that covers this column. */
static void send_rows(uint16_t x, uint16_t y, uint16_t y_end, uint16_t y0,
                      const canvas_rect_t *holes, uint8_t nholes)
{
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
        lcd_write_column(x, y, &s_strip[y - y0], (uint16_t)(run_end - y));
        y = run_end;
    }
}

uint16_t canvas_draw_columns(const canvas_frame_t *f)
{
    const canvas_rect_t *r = &f->rect;
    if (f->id >= CANVAS_COUNT) return 0;
    canvas_mem_t *const m = &s_mem[f->id];
    if (r->w == 0u || r->h == 0u || r->h > LCD_HEIGHT || r->x + r->w > LCD_WIDTH)
        return 0;

    canvas_rect_t holes[CANVAS_MAX_HOLES];
    uint8_t nholes = 0;
    for (uint8_t i = 0; i < f->nholes && nholes < CANVAS_MAX_HOLES; i++)
        if (hole_clip(r, &f->holes[i], &holes[nholes])) nholes++;

    /* May the memory of the last frame be trusted? */
    bool same = m->valid && !f->all && rect_equal(r, &m->rect) && nholes == m->nholes;
    for (uint8_t i = 0; same && i < nholes; i++)
        same = rect_equal(&holes[i], &m->holes[i]);
    lcd_watch_rows(f->id, r->y, (uint16_t)(r->y + r->h));
    if (lcd_watch_take(f->id)) same = false;
    const bool based = f->base != NULL;
    canvas_runs_begin(f->id, based, same);

    const canvas_strip_t strip = { s_strip, (int16_t)r->y, (int16_t)(r->y + r->h) };
    const uint16_t y_end = (uint16_t)(r->y + r->h);
    uint16_t sent = 0;
    for (uint16_t x = r->x; x < r->x + r->w; x++) {
        if (based) {
            f->base(f->ctx, x, &strip);
            canvas_runs_take_base(s_strip, r->h);
        }
        f->paint(f->ctx, x, &strip);

        const canvas_window_t *w;
        uint32_t hash;
        const uint8_t nw = canvas_runs_windows(x, s_strip, r->y, r->h, &w, &hash);
        if (!same || hash != m->hash[x]) {
            m->hash[x] = hash;
            sent++;
            if (nw == 0u) send_rows(x, r->y, y_end, r->y, holes, nholes);
            for (uint8_t i = 0; i < nw; i++) send_rows(x, w[i].lo, w[i].hi, r->y, holes, nholes);
        }
    }

    /* Discard the watch marks this frame's own writes left, so the next
     * frame sees only other painters'; the memory now describes the glass. */
    (void)lcd_watch_take(f->id);
    m->valid = true;
    m->rect = *r;
    m->nholes = nholes;
    for (uint8_t i = 0; i < nholes; i++) m->holes[i] = holes[i];
    return sent;
}
