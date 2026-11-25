/*
 * The canvas draw cycle (src/ui/canvas.c) against a model of the glass.
 *
 * The LCD is faked: lcd_write_column() writes into a 320 x 240 array and the
 * watched rows work as the driver's do. A picture is a function of (x, y)
 * and a version number; the paint function fills each strip from it.
 *
 * What must hold:
 *   - after any frame the glass shows the picture everywhere inside the
 *     rectangle and outside the holes, and nothing outside it was written;
 *   - an unchanged picture sends no strip; a change in some columns sends
 *     those columns only;
 *   - `all` sends every strip;
 *   - somebody else drawing on the rows, another rectangle, or a hole
 *     opening, closing or moving makes the next frame send every strip;
 *   - an incremental frame leaves the same glass as a full one;
 *   - with a base layer: the glass stays right through random frames that
 *     move the overlays, change the base, open and close holes, draw `all`,
 *     draw over the rows from elsewhere, overflow the runs and change the
 *     rectangle; with the feature CANVASRUNS (canvas_runs.h) a column whose
 *     base held still sends only the rows its overlays left and arrived on
 *     (joined across short gaps), without it every changed column goes whole.
 *
 * Built twice by the Makefile (test-canvas), with FEATURE_CANVASRUNS 0 and 1.
 */
#include <stdio.h>
#include <string.h>
#include "canvas.h"
#include "lcd.h"

/* The build under test: with the canvas runs or without (features.mk). */
#define RUNS (FEATURE_CANVASRUNS != 0)

static int run, passed;
#define CHECK(name, cond) do { run++; if (cond) { passed++; printf("  %-66s[PASS]\n", name); } \
                               else { printf("  %-66s[FAIL]\n", name); } } while (0)

/* ── the fake LCD ── */
#define UNTOUCHED 0xDEADu
static uint16_t glass[LCD_HEIGHT][LCD_WIDTH];
static unsigned column_writes, pixel_writes;
static const canvas_rect_t *write_rect;     /* writes must stay inside it, when set */
static bool     wrote_outside;
static uint16_t watch_y0[LCD_WATCH_SLOTS], watch_y1[LCD_WATCH_SLOTS];
static bool     watch_dirty[LCD_WATCH_SLOTS] = { true, true };

void lcd_watch_rows(uint8_t slot, uint16_t y0, uint16_t y1)
{
    if (y0 != watch_y0[slot] || y1 != watch_y1[slot]) watch_dirty[slot] = true;
    watch_y0[slot] = y0; watch_y1[slot] = y1;
}
bool lcd_watch_take(uint8_t slot) { const bool d = watch_dirty[slot]; watch_dirty[slot] = false; return d; }
static void watch_mark(uint16_t y, uint32_t h)
{
    for (unsigned i = 0; i < LCD_WATCH_SLOTS; i++)
        if (y < watch_y1[i] && (uint32_t)y + h > watch_y0[i]) watch_dirty[i] = true;
}
void lcd_write_column(uint16_t x, uint16_t y, const uint16_t *pixels, uint16_t count)
{
    column_writes++;
    pixel_writes += count;
    if (write_rect && (x < write_rect->x || x >= write_rect->x + write_rect->w ||
                       y < write_rect->y || y + count > write_rect->y + write_rect->h))
        wrote_outside = true;
    watch_mark(y, count);
    for (uint16_t i = 0; i < count; i++) glass[y + i][x] = pixels[i];
}
/* Another painter: one pixel, as lcd_set_pixel would. */
static void foreign_pixel(uint16_t x, uint16_t y, uint16_t c)
{
    watch_mark(y, 1u);
    glass[y][x] = c;
}

/* ── the picture ── */
static unsigned version;          /* changes the picture in columns [ver_x0, ver_x1) */
static uint16_t ver_x0, ver_x1;
static unsigned paints;
static uint16_t last_index;
static bool     order_ok;

static uint16_t picture(uint16_t x, uint16_t y)
{
    uint16_t c = (uint16_t)(x * 131u + y * 7u);
    if (c == UNTOUCHED) c = 1u;
    if (x >= ver_x0 && x < ver_x1) c = (uint16_t)(c + version * 3u);
    return c;
}
static void paint(void *ctx, uint16_t index, const canvas_strip_t *s)
{
    (void)ctx;
    if (paints && index != last_index + 1u) order_ok = false;
    last_index = index; paints++;
    for (int y = s->lo; y < s->hi; y++) strip_put(s, y, picture(index, (uint16_t)y));
    strip_put(s, s->lo - 1, 0x1234u);       /* outside: must be dropped */
    strip_put(s, s->hi, 0x1234u);
    strip_span(s, s->hi, s->hi + 5, 0x1234u);
}

static bool in_hole(const canvas_frame_t *f, uint16_t x, uint16_t y)
{
    for (uint8_t i = 0; i < f->nholes; i++) {
        const canvas_rect_t *h = &f->holes[i];
        if (x >= h->x && x < h->x + h->w && y >= h->y && y < h->y + h->h) return true;
    }
    return false;
}
/* The glass shows the picture inside the rectangle outside the holes;
 * `rest` is what every other pixel must hold. */
static bool glass_ok(const canvas_frame_t *f, uint16_t rest)
{
    for (uint16_t y = 0; y < LCD_HEIGHT; y++)
        for (uint16_t x = 0; x < LCD_WIDTH; x++) {
            const bool in = x >= f->rect.x && x < f->rect.x + f->rect.w &&
                            y >= f->rect.y && y < f->rect.y + f->rect.h && !in_hole(f, x, y);
            if (glass[y][x] != (in ? picture(x, y) : rest)) return false;
        }
    return true;
}
static void glass_fill(uint16_t c)
{
    for (uint16_t y = 0; y < LCD_HEIGHT; y++)
        for (uint16_t x = 0; x < LCD_WIDTH; x++) glass[y][x] = c;
}
static uint16_t draw(const canvas_frame_t *f)
{
    paints = 0; order_ok = true; column_writes = 0; pixel_writes = 0;
    return canvas_draw_columns(f);
}

/* ── a picture in layers: a base under overlays ── */
#define RAND_FRAMES     3000u
#define OV_MAX          12u       /* overlay runs a column may have: more than the canvas keeps */
#define BASE_COLORS     4u
static const uint16_t base_color[BASE_COLORS] = { 0x0000u, 0x1082u, 0x39E7u, 0xFFFFu };
/* Overlay colours: two of the base's, so an overlay pixel sometimes equals
 * the base under it. */
static const uint16_t ov_color[] = { 0xF800u, 0x07E0u, 0x1082u, 0x39E7u };
#define OV_COLORS (sizeof ov_color / sizeof ov_color[0])

typedef struct { int16_t lo, hi; uint16_t color; } ov_run_t;    /* rows [lo, hi) */
static uint8_t  base_ver[LCD_WIDTH];
static ov_run_t ov[LCD_WIDTH][OV_MAX];
static uint8_t  nov[LCD_WIDTH];
static unsigned base_calls, ov_paints;
static int      based_index;          /* the strip base painted last, -1: none pending */
static bool     layer_order_ok;

static uint16_t base_px(uint16_t x, uint16_t y)
{
    return base_color[(x * 3u + y / 13u + base_ver[x]) % BASE_COLORS];
}
/* What the glass must show at (x, y): the last overlay run over it, else the base. */
static uint16_t layered_px(uint16_t x, uint16_t y)
{
    uint16_t c = base_px(x, y);
    for (uint8_t i = 0; i < nov[x]; i++)
        if ((int)y >= ov[x][i].lo && (int)y < ov[x][i].hi) c = ov[x][i].color;
    return c;
}
static void paint_base(void *ctx, uint16_t index, const canvas_strip_t *s)
{
    (void)ctx;
    if (based_index >= 0) layer_order_ok = false;        /* the last base had no paint */
    based_index = index; base_calls++;
    for (int y = s->lo; y < s->hi; y++) strip_put(s, y, base_px(index, (uint16_t)y));
}
static void paint_overlays(void *ctx, uint16_t index, const canvas_strip_t *s)
{
    (void)ctx;
    if (based_index != (int)index) layer_order_ok = false;
    based_index = -1; ov_paints++;
    for (uint8_t i = 0; i < nov[index]; i++)
        strip_span(s, ov[index][i].lo, ov[index][i].hi - 1, ov[index][i].color);
}
static uint16_t draw_layered(const canvas_frame_t *f)
{
    base_calls = ov_paints = 0; based_index = -1; layer_order_ok = true;
    write_rect = &f->rect; wrote_outside = false;
    const uint16_t n = draw(f);
    write_rect = NULL;
    if (based_index >= 0 || base_calls != f->rect.w || ov_paints != f->rect.w) layer_order_ok = false;
    return n;
}
/* The rectangle shows the layered picture outside the holes and the box
 * colour inside them. */
static bool layered_ok(const canvas_frame_t *f, uint16_t box)
{
    for (uint16_t y = f->rect.y; y < f->rect.y + f->rect.h; y++)
        for (uint16_t x = f->rect.x; x < f->rect.x + f->rect.w; x++)
            if (glass[y][x] != (in_hole(f, x, y) ? box : layered_px(x, y))) return false;
    return true;
}
static void ov_set(uint16_t x, uint8_t n, const ov_run_t *runs)
{
    nov[x] = n;
    for (uint8_t i = 0; i < n; i++) ov[x][i] = runs[i];
}
static void ov_all(int16_t lo, int16_t hi, uint16_t color)
{
    for (uint16_t x = 0; x < LCD_WIDTH; x++) ov_set(x, 1u, &(ov_run_t){ lo, hi, color });
}

static uint32_t rng = 2463534242u;
static uint32_t rnd(uint32_t n)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng % n;
}

/* A hole: the box painted by its owner (another painter), then the watch
 * mark of that taken, as the popup does before the frame that steps around it. */
static void box_paint(const canvas_rect_t *h, uint8_t slot, uint16_t box)
{
    for (uint16_t y = h->y; y < h->y + h->h && y < LCD_HEIGHT; y++)
        for (uint16_t x = h->x; x < h->x + h->w && x < LCD_WIDTH; x++) glass[y][x] = box;
    watch_mark(h->y, h->h);
    (void)lcd_watch_take(slot);
}

static void layered_tests(void)
{
    printf("canvas draw cycle with a base layer\n");
    const uint16_t BOX = 0x0BB0u;
    canvas_frame_t f = { .id = CANVAS_SPLIT_UPPER, .rect = { 0, 18, 320, 206 },
                         .base = paint_base, .paint = paint_overlays };
    const uint16_t h = f.rect.h;
    memset(base_ver, 0, sizeof base_ver);
    ov_all(100, 103, ov_color[0]);
    glass_fill(UNTOUCHED);
    uint16_t n = draw_layered(&f);
    CHECK("the first frame sends whole columns", n == 320 && pixel_writes == 320u * h);
    CHECK("... base then paint, once each, for every column", layer_order_ok);
    CHECK("... and the glass shows base and overlays, nothing outside", layered_ok(&f, BOX) && !wrote_outside);

    n = draw_layered(&f);
    CHECK("an unchanged picture sends nothing", n == 0 && column_writes == 0);

    /* The overlay moves far: the rows it left and the rows it arrived on, two windows. */
    ov_all(150, 153, ov_color[0]);
    n = draw_layered(&f);
    CHECK(RUNS ? "an overlay moves far: old rows and new rows, 2 windows of 3"
               : "an overlay moves far: whole columns",
          n == 320 && layered_ok(&f, BOX) &&
          (RUNS ? column_writes == 640 && pixel_writes == 320u * 6u
                : column_writes == 320 && pixel_writes == 320u * h));

    /* Runs closer than the cost of a window are sent as one. */
    ov_all(163, 166, ov_color[0]);              /* gap from [150,153): 10 rows */
    n = draw_layered(&f);
    CHECK(RUNS ? "runs 10 rows apart go as one window" : "an overlay moves 13 rows: whole columns",
          column_writes == 320 && pixel_writes == 320u * (RUNS ? 16u : h));
    ov_all(177, 180, ov_color[0]);              /* gap from [163,166): 11 rows */
    n = draw_layered(&f);
    CHECK(RUNS ? "runs 11 rows apart go as two windows" : "an overlay moves 14 rows: whole columns",
          RUNS ? column_writes == 640 && pixel_writes == 320u * 6u
               : column_writes == 320 && pixel_writes == 320u * h);
    ov_all(178, 181, ov_color[0]);              /* overlapping */
    n = draw_layered(&f);
    CHECK(RUNS ? "overlapping runs go as one window" : "an overlay moves 1 row: whole columns",
          column_writes == 320 && pixel_writes == 320u * (RUNS ? 4u : h) && layered_ok(&f, BOX));

    /* The base changes in some columns: those go whole. */
    for (uint16_t x = 40; x < 45; x++) base_ver[x]++;
    n = draw_layered(&f);
    CHECK("the base changes in 5 columns: those 5 go whole", n == 5 && pixel_writes == 5u * h &&
          layered_ok(&f, BOX));

    /* An overlay over the whole strip in a colour the base also has; the base
     * changes under it (the strip does not: nothing is sent, but the base and
     * the overlay rows kept for the column change), then the overlay goes. */
    ov_all((int16_t)f.rect.y, (int16_t)(f.rect.y + h), base_color[1]);
    (void)draw_layered(&f);
    for (uint16_t x = 0; x < LCD_WIDTH; x++) base_ver[x] = (uint8_t)(base_ver[x] + 1u);
    n = draw_layered(&f);
    CHECK("the base changes under an overlay that covers it: nothing is sent", n == 0 && column_writes == 0);
    memset(nov, 0, sizeof nov);
    n = draw_layered(&f);
    CHECK("... then the overlay goes: the new base shows", n == 320 && layered_ok(&f, BOX));

    /* Many overlay runs in a column: more than the canvas keeps. */
    for (uint16_t x = 0; x < LCD_WIDTH; x++) {
        ov_run_t r[OV_MAX];
        for (uint8_t i = 0; i < OV_MAX; i++)
            r[i] = (ov_run_t){ (int16_t)(20 + i * 15 + x % 3), (int16_t)(21 + i * 15 + x % 3), ov_color[i % 2] };
        ov_set(x, OV_MAX, r);
    }
    (void)draw_layered(&f);
    CHECK("12 overlay runs a column: the glass is right", layered_ok(&f, BOX));
    for (uint16_t x = 0; x < LCD_WIDTH; x++)
        for (uint8_t i = 0; i < OV_MAX; i++) { ov[x][i].lo += 4; ov[x][i].hi += 4; }
    (void)draw_layered(&f);
    CHECK("... and after they all move", layered_ok(&f, BOX));

    /* A frame without a base on the same canvas and rectangle, then the
     * layered picture again: the runs of before the plain frame are stale. */
    ov_all(100, 103, ov_color[0]);
    (void)draw_layered(&f);
    canvas_frame_t plain = f;
    plain.base = NULL; plain.paint = paint;
    (void)draw(&plain);
    n = draw_layered(&f);
    CHECK("a frame without a base between: the next layered one is right",
          n == 320 && layered_ok(&f, BOX));

    /* Random frames. */
    canvas_rect_t holes[CANVAS_MAX_HOLES];
    f.holes = holes; f.nholes = 0;
    int16_t trace[2][LCD_WIDTH];
    for (uint8_t ch = 0; ch < 2u; ch++)
        for (uint16_t x = 0; x < LCD_WIDTH; x++) trace[ch][x] = (int16_t)(60 + ch * 80);
    unsigned bad = 0, outside = 0, order = 0, partial = 0, partial_holes = 0, whole = 0, cut = 0;
    uint32_t px_steady = 0, cols_steady = 0;
    for (unsigned fr = 0; fr < RAND_FRAMES; fr++) {
        bool steady = true;
        /* The traces drift; a few columns get many runs. */
        for (uint16_t x = 0; x < LCD_WIDTH; x++) {
            ov_run_t r[OV_MAX];
            uint8_t k = 0;
            for (uint8_t ch = 0; ch < 2u; ch++) {
                int16_t y = (int16_t)(trace[ch][x] + (int16_t)rnd(9) - 4);
                if (y < 0) y = 0;
                if (y > 230) y = 230;
                trace[ch][x] = y;
                r[k++] = (ov_run_t){ y, (int16_t)(y + 1 + (int16_t)rnd(6)), ov_color[ch] };
            }
            if (rnd(40) == 0)
                while (k < OV_MAX) {
                    const int16_t y = (int16_t)rnd(LCD_HEIGHT);
                    r[k++] = (ov_run_t){ y, (int16_t)(y + 1 + (int16_t)rnd(3)), ov_color[rnd(OV_COLORS)] };
                }
            ov_set(x, k, r);
        }
        if (rnd(10) == 0) {                 /* the base changes in some columns */
            const uint16_t x0 = (uint16_t)rnd(LCD_WIDTH), w = (uint16_t)(1u + rnd(40));
            for (uint16_t x = x0; x < x0 + w && x < LCD_WIDTH; x++) base_ver[x]++;
            steady = false;
        }
        if (rnd(15) == 0) {                 /* a hole opens, moves or closes */
            f.nholes = (uint8_t)rnd(CANVAS_MAX_HOLES + 1u);
            for (uint8_t i = 0; i < f.nholes; i++) {
                holes[i] = (canvas_rect_t){ (uint16_t)rnd(300), (uint16_t)rnd(230),
                                            (uint16_t)(1u + rnd(120)), (uint16_t)(1u + rnd(60)) };
                box_paint(&holes[i], f.id, BOX);
            }
            steady = false;
        }
        f.all = rnd(25) == 0;
        if (f.all) steady = false;
        if (rnd(20) == 0) {                 /* somebody else draws on the rows (not in a hole) */
            const uint16_t x = (uint16_t)rnd(LCD_WIDTH), y = (uint16_t)(f.rect.y + rnd(f.rect.h));
            foreign_pixel(x, y, in_hole(&f, x, y) ? BOX : 0x0001u);
            steady = false;
        }
        if (rnd(30) == 0) {                 /* another rectangle */
            f.rect.y = (uint16_t)(rnd(2) ? 18u : 30u);
            f.rect.h = (uint16_t)(rnd(2) ? 206u : 150u);
            steady = false;
        }
        n = draw_layered(&f);
        if (!layered_ok(&f, BOX)) bad++;
        if (wrote_outside) outside++;
        if (!layer_order_ok) order++;
        if (column_writes > n) partial++;
        if (column_writes > n && f.nholes) partial_holes++;
        if (!f.nholes && pixel_writes != (uint32_t)n * f.rect.h) cut++;     /* a column not sent whole */
        if (steady) { px_steady += pixel_writes; cols_steady += n; }
        else whole++;
    }
    char name[96];
    snprintf(name, sizeof name, "%u random frames: the glass is right after every one", RAND_FRAMES);
    CHECK(name, bad == 0);
    CHECK("... nothing written outside the rectangle, layers in order", outside == 0 && order == 0);
    if (RUNS) {
        CHECK("... runs were sent around holes too", partial_holes > 0);
        snprintf(name, sizeof name, "... a steady base sends %lu pixels a column, not %u",
                 (unsigned long)(cols_steady ? px_steady / cols_steady : 0u), (unsigned)f.rect.h);
        CHECK(name, cols_steady > 0 && px_steady * 4u < cols_steady * 150u && whole > 0 && partial > 0);
    } else {
        CHECK("... every changed column went whole", cut == 0 && cols_steady > 0 && whole > 0);
    }
}

int main(void)
{
    printf("canvas draw cycle\n");
    glass_fill(UNTOUCHED);
    canvas_frame_t f = { .rect = { 0, 18, 320, 180 }, .paint = paint };

    uint16_t n = draw(&f);
    CHECK("the first frame sends every column", n == 320 && paints == 320 && order_ok);
    CHECK("... and the glass shows the picture, nothing outside", glass_ok(&f, UNTOUCHED));

    n = draw(&f);
    CHECK("an unchanged picture sends nothing", n == 0 && column_writes == 0 && paints == 320);

    version = 1; ver_x0 = 40; ver_x1 = 57;
    n = draw(&f);
    CHECK("a change in 17 columns sends those 17", n == 17 && column_writes == 17);
    CHECK("... and the glass shows the new picture", glass_ok(&f, UNTOUCHED));

    f.all = true;
    n = draw(&f);
    CHECK("`all` sends every column", n == 320 && column_writes == 320);
    f.all = false;

    foreign_pixel(5, 100, 0x0001u);
    n = draw(&f);
    CHECK("another painter on the rows: every column is sent again", n == 320 && glass_ok(&f, UNTOUCHED));
    foreign_pixel(5, 10, UNTOUCHED);        /* above the rectangle */
    n = draw(&f);
    CHECK("another painter outside the rows: nothing is sent", n == 0);

    /* A hole: the columns under it go out in two runs, the box is not touched. */
    const canvas_rect_t box = { 60, 102, 200, 36 };
    for (uint16_t y = box.y; y < box.y + box.h; y++)
        for (uint16_t x = box.x; x < box.x + box.w; x++) foreign_pixel(x, y, 0x0BB0u);
    (void)lcd_watch_take(CANVAS_PLOT);
    f.holes = &box; f.nholes = 1;
    n = draw(&f);
    bool box_kept = true;
    for (uint16_t y = box.y; y < box.y + box.h; y++)
        for (uint16_t x = box.x; x < box.x + box.w; x++) box_kept = box_kept && glass[y][x] == 0x0BB0u;
    CHECK("a hole opens: every column is sent, two runs under the box", n == 320 && column_writes == 320 + 200);
    CHECK("... and the box is left alone", box_kept);
    n = draw(&f);
    CHECK("the same hole again: nothing is sent", n == 0);
    f.nholes = 0;
    n = draw(&f);
    CHECK("the hole closes: every column is sent, the box is painted over", n == 320 && glass_ok(&f, UNTOUCHED));

    /* Three holes, one clipped by the rectangle, one outside it. */
    const canvas_rect_t three[3] = { { 0, 18, 96, 16 }, { 140, 10, 40, 24 }, { 100, 200, 50, 20 } };
    f.holes = three; f.nholes = 3;
    glass_fill(UNTOUCHED); foreign_pixel(0, 18, UNTOUCHED);
    n = draw(&f);
    CHECK("three holes (one clipped, one outside): the glass is right", n == 320 && glass_ok(&f, UNTOUCHED));
    f.nholes = 0;

    /* Another rectangle (the band's bottom moved). */
    f.rect.h = 206;
    n = draw(&f);
    CHECK("a taller rectangle: every column is sent", n == 320);
    glass_fill(UNTOUCHED); foreign_pixel(0, 18, UNTOUCHED);
    (void)draw(&f);
    CHECK("... and the glass shows it", glass_ok(&f, UNTOUCHED));

    /* Incremental equals full: change the picture, draw incrementally, keep
     * the glass; then the same picture drawn with `all` on a wiped glass. */
    version = 2; ver_x0 = 0; ver_x1 = 320;
    (void)draw(&f);
    static uint16_t inc[LCD_HEIGHT][LCD_WIDTH];
    memcpy(inc, glass, sizeof(inc));
    glass_fill(UNTOUCHED);
    f.all = true; (void)draw(&f); f.all = false;
    CHECK("an incremental frame leaves the glass a full one does", memcmp(inc, glass, sizeof(inc)) == 0);

    /* A narrower rectangle not at column 0. */
    canvas_frame_t g = { .rect = { 100, 50, 64, 32 }, .paint = paint };
    glass_fill(UNTOUCHED); foreign_pixel(100, 50, UNTOUCHED);
    n = draw(&g);
    CHECK("a rectangle inside the screen: 64 columns, nothing outside", n == 64 && glass_ok(&g, UNTOUCHED));

    /* Two canvases drawn in turn, as the split view's halves: each keeps its
     * own memory, so neither resends what did not change. */
    version = 0; ver_x0 = ver_x1 = 0;
    canvas_frame_t top = { .id = CANVAS_SPLIT_UPPER, .rect = { 0, 18, 320, 104 }, .paint = paint };
    canvas_frame_t bot = { .id = CANVAS_PLOT, .rect = { 0, 122, 320, 101 }, .paint = paint };
    glass_fill(UNTOUCHED);
    (void)draw(&top); (void)draw(&bot);
    n = draw(&top);
    const uint16_t nb = draw(&bot);
    CHECK("two canvases in turn: neither resends an unchanged picture", n == 0 && nb == 0);
    version = 3; ver_x0 = 10; ver_x1 = 13;
    n = draw(&top);
    bool top_ok = true;
    for (uint16_t y = top.rect.y; y < top.rect.y + top.rect.h; y++)
        for (uint16_t x = 0; x < LCD_WIDTH; x++) top_ok = top_ok && glass[y][x] == picture(x, y);
    CHECK("... a change sends only its columns of that canvas", n == 3 && top_ok);
    (void)draw(&bot);

    foreign_pixel(7, 50, 0x0002u);          /* on the upper half's rows only */
    n = draw(&bot);
    CHECK("another painter on one canvas's rows: the other is not disturbed", n == 0);
    n = draw(&top);
    CHECK("... and that canvas resends everything", n == 320);

    /* The full plot drawn over the upper half's rows (a view change and back). */
    canvas_frame_t plot = { .id = CANVAS_PLOT, .rect = { 0, 18, 320, 206 }, .paint = paint };
    (void)draw(&plot);
    n = draw(&top);
    CHECK("another canvas painted the same rows: this one resends everything", n == 320);
    n = draw(&bot);
    CHECK("... and so does the canvas whose rectangle changed", n == 320);

    canvas_frame_t badid = { .id = CANVAS_COUNT, .rect = { 0, 18, 320, 10 }, .paint = paint };
    CHECK("an unknown canvas draws nothing", draw(&badid) == 0 && paints == 0);

    canvas_frame_t bad = { .rect = { 300, 0, 40, 10 }, .paint = paint };
    CHECK("a rectangle off the screen draws nothing", draw(&bad) == 0 && paints == 0);

    layered_tests();

    printf("\n%d/%d passed\n", passed, run);
    return passed == run ? 0 : 1;
}
