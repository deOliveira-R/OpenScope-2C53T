/*
 * The waterfall's real drawing code (src/ui/waterfall_blit.c) on a model LCD:
 * lcd_rect_begin() latches a rectangle, lcd_rect_row() streams a row into it.
 *
 * Build and run: make test-waterfall-blit
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "waterfall_blit.h"

#define UNWRITTEN   0xDEADu
#define TEXT_COLOR  0xF81Fu
#define TEXT_COLS   10
#define MAX_ROWS    250
#define SEED        12345u

static uint16_t fb[LCD_HEIGHT][LCD_WIDTH];
static uint16_t win_x, win_y, win_w, win_h;
static uint32_t win_cursor, windows, streams, overruns;

void lcd_rect_begin(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    win_x = x; win_y = y; win_w = w; win_h = h; win_cursor = 0;
    windows++;
}

void lcd_rect_row(const uint16_t *pixels)
{
    streams++;
    for (uint32_t i = 0; i < win_w; i++, win_cursor++) {
        if (win_cursor >= (uint32_t)win_w * win_h) { overruns++; return; }
        const uint32_t px = win_x + win_cursor % win_w, py = win_y + win_cursor / win_w;
        if (px < LCD_WIDTH && py < LCD_HEIGHT) fb[py][px] = pixels[i];
    }
}

/* One label: the first TEXT_COLS columns of lines [text_y, text_y + text_h). */
static int text_y, text_h;
static bool text_line(const void *t, int y) { (void)t; return y >= text_y && y < text_y + text_h; }
static bool text_px(const void *t, int x, int y, uint16_t *c)
{
    if (!text_line(t, y) || x >= TEXT_COLS) return false;
    *c = TEXT_COLOR;
    return true;
}
static void text_row(const void *t, int y, uint16_t *ln)
{
    for (int x = 0; x < WATERFALL_BLIT_COLS; x++) (void)text_px(t, x, y, &ln[x]);
}

static uint8_t history[MAX_ROWS][WATERFALL_BLIT_COLS];
static uint16_t line[WATERFALL_BLIT_COLS];
static int run, failed;

#define CHECK(cond, ...) do { run++; if (!(cond)) { failed++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void fill_history(uint16_t rows)
{
    srand(SEED);
    for (uint16_t r = 0; r < rows; r++)
        for (uint16_t x = 0; x < WATERFALL_BLIT_COLS; x++)
            history[r][x] = (uint8_t)(rand() & 0xFF);
}

static void draw(uint16_t rows, uint16_t newest, uint16_t top, uint16_t height)
{
    for (int y = 0; y < LCD_HEIGHT; y++)
        for (int x = 0; x < LCD_WIDTH; x++) fb[y][x] = UNWRITTEN;
    windows = streams = overruns = 0;
    const waterfall_frame_t f = {
        .history = history, .rows = rows, .newest = newest, .top = top, .height = height,
        .line = line, .text = NULL, .text_line = text_line, .text_row = text_row,
    };
    waterfall_blit(&f);
}

/* Pixels that differ from what the history, newest on top, says. */
static uint32_t wrong_pixels(uint16_t rows, uint16_t newest, uint16_t top, uint16_t height)
{
    uint16_t rh = (uint16_t)(height / rows);
    if (rh < 1) rh = 1;
    const uint16_t drawn = (uint32_t)rows * rh > height ? (uint16_t)(height / rh) : rows;
    const uint16_t block = (uint16_t)(drawn * rh);
    uint32_t wrong = 0;
    for (int y = 0; y < LCD_HEIGHT; y++) {
        for (int x = 0; x < LCD_WIDTH; x++) {
            uint16_t want = UNWRITTEN;
            if (y >= top && y < top + height) {
                const int sy = y - top;
                want = sy < block
                    ? waterfall_color(history[(newest + rows - sy / rh) % rows][x]) : COLOR_BLACK;
                (void)text_px(NULL, x, y, &want);
            }
            if (fb[y][x] != want) wrong++;
        }
    }
    return wrong;
}

static void check_case(const char *name, uint16_t rows, uint16_t newest, uint16_t top, uint16_t height,
                       int ty, int th)
{
    text_y = ty; text_h = th;
    fill_history(rows);
    draw(rows, newest, top, height);
    const uint32_t wrong = wrong_pixels(rows, newest, top, height);
    CHECK(wrong == 0, "%s: %u wrong pixels", name, (unsigned)wrong);
    CHECK(windows == height, "%s: %u windows for %u lines", name, (unsigned)windows, height);
    CHECK(streams == height, "%s: %u streams for %u lines", name, (unsigned)streams, height);
    CHECK(overruns == 0, "%s: a stream overran its window", name);
    printf("  %-48s %s\n", name, wrong == 0 ? "ok" : "WRONG");
}

int main(void)
{
    enum { TOP = 18, PLOT = 206, ROWS = 64, NEWEST = 7 };

    CHECK(waterfall_color(255) == COLOR_BLACK, "the floor is black");
    CHECK(waterfall_color(0) == RGB565(255, 0, 0), "the reference is red");

    check_case("shipping geometry", ROWS, NEWEST, TOP, PLOT, -1, 0);
    check_case("newest row is the last", ROWS, ROWS - 1, TOP, PLOT, -1, 0);
    check_case("exact multiple, no residual strip", ROWS, NEWEST, TOP, 192, -1, 0);
    check_case("16 rows", 16, NEWEST, TOP, PLOT, -1, 0);
    check_case("more rows than lines", MAX_ROWS, NEWEST, TOP, PLOT, -1, 0);
    check_case("label on one line inside a row", ROWS, NEWEST, TOP, PLOT, TOP + 1, 1);
    check_case("label across rows", ROWS, NEWEST, TOP, PLOT, TOP + 2, 9);
    check_case("label in the residual strip", ROWS, NEWEST, TOP, PLOT, TOP + PLOT - 12, 9);

    /* The comparison can fail: one history byte changed after the draw. */
    text_y = -1; text_h = 0;
    fill_history(ROWS);
    draw(ROWS, NEWEST, TOP, PLOT);
    history[NEWEST][100] ^= 0xFF;
    CHECK(wrong_pixels(ROWS, NEWEST, TOP, PLOT) > 0, "negative control saw no difference");

    printf("Results: %d/%d passed\n", run - failed, run);
    return failed ? 1 : 0;
}
