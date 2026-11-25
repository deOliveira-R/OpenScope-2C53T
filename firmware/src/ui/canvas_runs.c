/*
 * OpenScope 2C53T - canvas runs: send only the rows of a column that changed
 * (FEATURE_CANVASRUNS). See canvas_runs.h.
 */

#include "canvas_runs.h"
#include "canvas.h"
#include "lcd.h"

/* The rows the overlays cover in a column, kept as at most this many runs:
 * Y-T's two traces, its trigger line and one more (the math trace or a
 * cursor line). More are joined into fewer (overlay_runs). */
#define CANVAS_RUNS_MAX    4u

/* Two runs of rows to send closer than this are sent as one window: a window
 * costs lcd_write_column() 11 bus writes before its first pixel (CASET and
 * its 4 bytes, RASET and its 4, RAMWR; the runs of a column have rows of
 * their own, so RASET is always sent), a pixel one. */
#define CANVAS_RUN_MERGE_GAP 11u

/* The windows of one column at most: its old runs and its new ones, none
 * joined. */
#define CANVAS_WINDOWS_MAX (2u * CANVAS_RUNS_MAX)

/* Rows [lo, hi) of the screen; lo == hi: no run. A column's runs are in
 * order, top to bottom, the empty ones last. */
typedef struct {
    uint8_t lo, hi;
} canvas_run_t;

_Static_assert(LCD_HEIGHT <= UINT8_MAX, "a screen row fits a run's uint8_t");

/* One canvas's memory, valid when its last frame had a base (based): each
 * strip's base (its hash) and the rows where the strip differed from it
 * (runs). What the glass holds is that base with those rows over it. */
typedef struct {
    bool         based;
    uint32_t     base_hash[LCD_WIDTH];
    canvas_run_t runs[LCD_WIDTH][CANVAS_RUNS_MAX];
} canvas_runs_mem_t;

static canvas_runs_mem_t s_runs[CANVAS_COUNT];

/* The frame being drawn: whose memory, whether it has a base, and whether
 * the runs on the glass are known (trusted, and this frame and the last one
 * both have a base). Then the current column's base: a copy and its hash. */
static canvas_runs_mem_t *s_mem;
static bool     s_based;
static bool     s_known;
static uint16_t s_base[LCD_HEIGHT];
static uint32_t s_base_hash;
static canvas_window_t s_windows[CANVAS_WINDOWS_MAX];

void canvas_runs_begin(uint8_t id, bool based, bool trusted)
{
    s_mem = &s_runs[id];
    s_based = based;
    s_known = trusted && based && s_mem->based;
    /* This frame leaves runs for every column it draws, or none. */
    s_mem->based = based;
}

void canvas_runs_take_base(const uint16_t *strip, uint16_t h)
{
    for (uint16_t i = 0; i < h; i++) s_base[i] = strip[i];
    s_base_hash = canvas_hash(strip, h);
}

/* The rows of the strip (h rows from row y0) that differ from its base, as
 * runs into out. Past CANVAS_RUNS_MAX the two runs with the smallest gap
 * between them are joined: a run that also covers rows where nothing
 * differs is still right, those rows are sent as they are. */
static void overlay_runs(const uint16_t *strip, uint16_t y0, uint16_t h, canvas_run_t out[CANVAS_RUNS_MAX])
{
    canvas_run_t r[CANVAS_RUNS_MAX + 1u];
    uint8_t n = 0;
    for (uint16_t i = 0; i < h;) {
        if (strip[i] == s_base[i]) { i++; continue; }
        const uint16_t lo = i;
        while (i < h && strip[i] != s_base[i]) i++;
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

/* What changed in a column when only the overlays did: the rows of the old
 * runs a (the overlays leave them, the base shows again) and of the new runs
 * b (the overlays arrive), top to bottom, joined where they are closer than
 * CANVAS_RUN_MERGE_GAP. Returns the number of windows in s_windows. */
static uint8_t run_windows(const canvas_run_t *a, const canvas_run_t *b)
{
    uint8_t ia = 0, ib = 0, n = 0;
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
        if (lo < hi) s_windows[n++] = (canvas_window_t){ lo, hi };
        lo = next.lo;
        hi = next.hi;
    }
    if (lo < hi) s_windows[n++] = (canvas_window_t){ lo, hi };
    return n;
}

uint8_t canvas_runs_windows(uint16_t x, const uint16_t *strip, uint16_t y0, uint16_t h,
                            const canvas_window_t **windows)
{
    *windows = s_windows;
    if (!s_based) return 0;
    canvas_run_t runs[CANVAS_RUNS_MAX];
    overlay_runs(strip, y0, h, runs);
    /* The windows come from the runs the glass holds now, before they are
     * replaced by this column's. */
    const uint8_t n = (s_known && s_base_hash == s_mem->base_hash[x])
                    ? run_windows(s_mem->runs[x], runs) : 0u;
    s_mem->base_hash[x] = s_base_hash;
    for (uint8_t j = 0; j < CANVAS_RUNS_MAX; j++) s_mem->runs[x][j] = runs[j];
    return n;
}
