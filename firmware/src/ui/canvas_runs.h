/*
 * OpenScope 2C53T - canvas runs: send only the rows of a column that changed
 * (FEATURE_CANVASRUNS, firmware/features.mk)
 *
 * The canvas (canvas.c) sends a changed column whole. With this feature it
 * remembers, per canvas and column, the hash of the base the glass holds and
 * the rows where the overlays differed from it (runs); when a changed
 * column's base is the one on the glass, only the rows the overlays left
 * and the rows they arrived on need sending. That memory is about 8 KB of
 * RAM (CANVAS_COUNT x LCD_WIDTH x (a base hash and CANVAS_RUNS_MAX runs),
 * and a copy of one base strip).
 *
 * THIS HEADER IS THE FEATURE'S WHOLE CONTRACT WITH THE CANVAS: canvas.c calls
 * it with no #if. With FEATURE_CANVASRUNS 0 the calls are inline no-ops,
 * canvas_runs_windows() always answers 0 (send the whole column) and
 * canvas_runs.c is not compiled: every changed column goes whole, the method
 * before the feature. The send itself (send_rows, holes) stays in canvas.c.
 *
 * A frame of canvas id, in this order (display task only):
 *   canvas_runs_begin(id, based, trusted)        once, before the first column;
 *   for each column x, increasing:
 *     canvas_runs_take_base(strip, h)            after the base painted the strip
 *                                                (based frames only);
 *     canvas_runs_windows(x, strip, y0, h, &w)   after the overlays painted it,
 *                                                for every column, sent or not.
 */
#ifndef CANVAS_RUNS_H
#define CANVAS_RUNS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Rows [lo, hi) of the screen to send in one write. */
typedef struct {
    uint16_t lo, hi;
} canvas_window_t;

/* The strip hash: a 32-bit FNV-1a hash of the 16-bit pixels. canvas.c
 * keeps it for each strip it sent, this feature for each strip's base. */
#define CANVAS_HASH_SEED   2166136261u     /* FNV-1a offset basis */
#define CANVAS_HASH_PRIME  16777619u       /* FNV-1a prime */

static inline uint32_t canvas_hash(const uint16_t *px, uint16_t n)
{
    uint32_t hash = CANVAS_HASH_SEED;
    for (uint16_t i = 0; i < n; i++) hash = (hash ^ px[i]) * CANVAS_HASH_PRIME;
    return hash;
}

#if FEATURE_CANVASRUNS

/* Start a frame of canvas id (canvas_id_t). based: the frame has a base
 * layer; trusted: the canvas's memory of the glass may be trusted (same
 * rectangle and holes, nobody else drew on the rows, not `all`). */
void canvas_runs_begin(uint8_t id, bool based, bool trusted);

/* The base of the current column: h pixels of strip, as the frame's base
 * function left them. */
void canvas_runs_take_base(const uint16_t *strip, uint16_t h);

/* Column x of the frame, painted (h pixels of strip from screen row y0):
 * records its base and runs, which the glass holds from now on whether the
 * column is sent or not, and returns the number of windows to send when it
 * changed, in *windows; 0 means the whole column. */
uint8_t canvas_runs_windows(uint16_t x, const uint16_t *strip, uint16_t y0, uint16_t h,
                            const canvas_window_t **windows);

#else  /* !FEATURE_CANVASRUNS: every changed column is sent whole */

static inline void canvas_runs_begin(uint8_t id, bool based, bool trusted)
{
    (void)id; (void)based; (void)trusted;
}
static inline void canvas_runs_take_base(const uint16_t *strip, uint16_t h)
{
    (void)strip; (void)h;
}
static inline uint8_t canvas_runs_windows(uint16_t x, const uint16_t *strip, uint16_t y0, uint16_t h,
                                          const canvas_window_t **windows)
{
    (void)x; (void)strip; (void)y0; (void)h;
    *windows = NULL;
    return 0;
}

#endif /* FEATURE_CANVASRUNS */
#endif /* CANVAS_RUNS_H */
