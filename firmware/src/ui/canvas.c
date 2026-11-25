/*
 * OpenScope 2C53T - canvas: one rectangle of the screen and its draw cycle
 */

#include <stddef.h>
#include "canvas.h"
#include "lcd.h"

/* The strip being painted, before it is sent, and a copy of its base layer
 * (the frame's base function's pixels, before the overlays). */
static uint16_t s_strip[LCD_HEIGHT];
static uint16_t s_base[LCD_HEIGHT];

/* What each strip held when it was last sent: a 32-bit FNV-1a hash of its
 * 16-bit pixels. A strip whose new pixels hash the same is not sent again;
 * with a steady picture that is most of them. Two different strips hashing
 * alike would leave the old one on the glass until it changes again (one
 * chance in 2^32 for each change). */

#define CANVAS_HASH_SEED   2166136261u     /* FNV-1a offset basis */
#define CANVAS_HASH_PRIME  16777619u       /* FNV-1a prime */

/* The rows the overlays cover in a column, kept as at most this many runs:
 * Y-T's two traces, its trigger line and one more (the math trace or a
 * cursor line). More are joined into fewer (overlay_runs). */
#define CANVAS_RUNS_MAX    4u

/* Two runs of rows to send closer than this are sent as one window: a window
 * costs lcd_write_column() 11 bus writes before its first pixel (CASET and
 * its 4 bytes, RASET and its 4, RAMWR; the runs of a column have rows of
 * their own, so RASET is always sent), a pixel one. */
#define CANVAS_RUN_MERGE_GAP 11u

/* Rows [lo, hi) of the screen; lo == hi: no run. A column's runs are in
 * order, top to bottom, the empty ones last. */
typedef struct {
    uint8_t lo, hi;
} canvas_run_t;

_Static_assert(LCD_HEIGHT <= UINT8_MAX, "a screen row fits a run's uint8_t");

/* One canvas's memory: what each strip held when last sent (hash, indexed
 * by screen column) and the rectangle and holes that describes. Not valid
 * before its first frame. When the last frame had a base (based), also each
 * strip's base (its hash) and the rows where the strip differed from it
 * (runs): what the glass holds is that base with those rows over it. */
typedef struct {
    bool          valid;
    bool          based;
    canvas_rect_t rect;
    canvas_rect_t holes[CANVAS_MAX_HOLES];
    uint8_t       nholes;
    uint32_t      hash[LCD_WIDTH];
    uint32_t      base_hash[LCD_WIDTH];
    canvas_run_t  runs[LCD_WIDTH][CANVAS_RUNS_MAX];
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

/* The rows of the strip (h rows from row y0) that differ from its base, as
 * runs into out. Past CANVAS_RUNS_MAX the two runs with the smallest gap
 * between them are joined: a run that also covers rows where nothing
 * differs is still right, those rows are sent as they are. */
static void overlay_runs(uint16_t y0, uint16_t h, canvas_run_t out[CANVAS_RUNS_MAX])
{
    canvas_run_t r[CANVAS_RUNS_MAX + 1u];
    uint8_t n = 0;
    for (uint16_t i = 0; i < h;) {
        if (s_strip[i] == s_base[i]) { i++; continue; }
        const uint16_t lo = i;
        while (i < h && s_strip[i] != s_base[i]) i++;
        r[n++] = (canvas_run_t){ (uint8_t)(y0 + lo), (uint8_t)(y0 + i) };
        if (n > CANVAS_RUNS_MAX) {
            uint8_t k = 0;
            for (uint8_t j = 1; j + 1u < n; j++)
                if (r[j + 1u].lo - r[j].hi < r[k + 1u].lo - r[k].hi) k = j;
            r[k].hi = r[k + 1u].hi;
            for (uint8_t j = (uint8_t)(k + 1u); j + 1u < n; j++) r[j] = r[j + 1u];
            n--;
        }
    }
    for (uint8_t j = 0; j < CANVAS_RUNS_MAX; j++) out[j] = (j < n) ? r[j] : (canvas_run_t){ 0u, 0u };
}

/* Send what changed in column x when only the overlays did: the rows of the
 * old runs a (the overlays leave them, the base shows again) and of the new
 * runs b (the overlays arrive), top to bottom, joined where they are closer
 * than CANVAS_RUN_MERGE_GAP. */
static void send_runs(uint16_t x, const canvas_run_t *a, const canvas_run_t *b, uint16_t y0,
                      const canvas_rect_t *holes, uint8_t nholes)
{
    uint8_t ia = 0, ib = 0;
    uint16_t lo = 0, hi = 0;                /* the window being gathered; empty when equal */
    for (;;) {
        const bool more_a = ia < CANVAS_RUNS_MAX && a[ia].lo < a[ia].hi;
        const bool more_b = ib < CANVAS_RUNS_MAX && b[ib].lo < b[ib].hi;
        if (!more_a && !more_b) break;
        const canvas_run_t next = (more_a && (!more_b || a[ia].lo <= b[ib].lo)) ? a[ia++] : b[ib++];
        if (lo < hi && next.lo < hi + CANVAS_RUN_MERGE_GAP) {
            if (next.hi > hi) hi = next.hi;
            continue;
        }
        if (lo < hi) send_rows(x, lo, hi, y0, holes, nholes);
        lo = next.lo;
        hi = next.hi;
    }
    if (lo < hi) send_rows(x, lo, hi, y0, holes, nholes);
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
    /* ... and its runs: only a frame with a base leaves them. */
    const bool based = f->base != NULL;
    const bool runs_known = same && based && m->based;

    const canvas_strip_t strip = { s_strip, (int16_t)r->y, (int16_t)(r->y + r->h) };
    const uint16_t y_end = (uint16_t)(r->y + r->h);
    uint16_t sent = 0;
    for (uint16_t x = r->x; x < r->x + r->w; x++) {
        uint32_t base_hash = CANVAS_HASH_SEED;
        canvas_run_t runs[CANVAS_RUNS_MAX];
        if (based) {
            f->base(f->ctx, x, &strip);
            for (uint16_t i = 0; i < r->h; i++) {
                base_hash = (base_hash ^ s_strip[i]) * CANVAS_HASH_PRIME;
                s_base[i] = s_strip[i];
            }
        }
        f->paint(f->ctx, x, &strip);

        uint32_t hash = CANVAS_HASH_SEED;
        for (uint16_t i = 0; i < r->h; i++) hash = (hash ^ s_strip[i]) * CANVAS_HASH_PRIME;
        if (based) overlay_runs(r->y, r->h, runs);
        if (!same || hash != m->hash[x]) {
            m->hash[x] = hash;
            sent++;
            if (runs_known && base_hash == m->base_hash[x])
                send_runs(x, m->runs[x], runs, r->y, holes, nholes);
            else
                send_rows(x, r->y, y_end, r->y, holes, nholes);
        }
        /* The glass now holds this strip, sent or not: keep its base and
         * runs together. */
        if (based) {
            m->base_hash[x] = base_hash;
            for (uint8_t j = 0; j < CANVAS_RUNS_MAX; j++) m->runs[x][j] = runs[j];
        }
    }

    /* Discard the watch marks this frame's own writes left, so the next
     * frame sees only other painters'; the memory now describes the glass. */
    (void)lcd_watch_take(f->id);
    m->valid = true;
    m->based = based;
    m->rect = *r;
    m->nholes = nholes;
    for (uint8_t i = 0; i < nholes; i++) m->holes[i] = holes[i];
    return sent;
}
