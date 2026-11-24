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
 *   - an incremental frame leaves the same glass as a full one.
 */
#include <stdio.h>
#include <string.h>
#include "canvas.h"
#include "lcd.h"

static int run, passed;
#define CHECK(name, cond) do { run++; if (cond) { passed++; printf("  %-66s[PASS]\n", name); } \
                               else { printf("  %-66s[FAIL]\n", name); } } while (0)

/* ── the fake LCD ── */
#define UNTOUCHED 0xDEADu
static uint16_t glass[LCD_HEIGHT][LCD_WIDTH];
static unsigned column_writes;
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
    paints = 0; order_ok = true; column_writes = 0;
    return canvas_draw_columns(f);
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
    canvas_frame_t top = { .id = CANVAS_SPLIT_TIME, .rect = { 0, 18, 320, 104 }, .paint = paint };
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

    printf("\n%d/%d passed\n", passed, run);
    return passed == run ? 0 : 1;
}
