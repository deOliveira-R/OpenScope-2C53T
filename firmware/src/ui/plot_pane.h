/*
 * OpenScope 2C53T - a pane: the band of the screen one view draws into
 *
 * A pane is a full-width band of rows with its own drawing memory (a canvas,
 * canvas.h). The time view draws into whatever pane it is given: the
 * graticule, the maps, the tags, the cursors and persistence all take their
 * rows from it, so the same view can fill the plot or one half of the split
 * view. Panes are full width: the columns are the time axis's 301 (scope_geom.h)
 * in every pane.
 *
 * The graticule has PLOT_PANE_DIVS_V divisions down the pane, each the pane's
 * height over that count rounded up: 26 rows on the 206-row plot (stock's
 * pitch), 13 on a half of it. The centre line is the middle one of those
 * lines; true scale puts 0 V at position 0 on it.
 *
 * Pure C: host-built by `make test-yt-vmap`.
 */
#ifndef PLOT_PANE_H
#define PLOT_PANE_H

#include <stdint.h>
#include "canvas.h"

/* Divisions of the graticule down a pane (stock: 8). */
#define PLOT_PANE_DIVS_V  8

/* Rows per division of a pane rows high. */
#define PLOT_PANE_DIV_PX(rows)  (((rows) + PLOT_PANE_DIVS_V - 1) / PLOT_PANE_DIVS_V)

typedef struct {
    canvas_id_t canvas;     /* its drawing memory */
    int16_t top;            /* its first row */
    int16_t bot;            /* the first row under its plot area */
    int16_t band_bot;       /* the first row the traces cannot use: bot, or the measurement strip's top */
    int16_t div_px;         /* rows per graticule division */
    int16_t grid_mid;       /* the graticule's centre line */
    int16_t mid;            /* the middle row: the pixel convention's 0 (no record) */
} plot_pane_t;

/* The pane of rows [top, bot), its traces ending at band_bot (<= bot). */
static inline plot_pane_t plot_pane_make(canvas_id_t canvas, int16_t top, int16_t bot, int16_t band_bot)
{
    const int16_t div = (int16_t)PLOT_PANE_DIV_PX(bot - top);
    return (plot_pane_t){
        .canvas = canvas, .top = top, .bot = bot, .band_bot = band_bot, .div_px = div,
        .grid_mid = (int16_t)(top + (PLOT_PANE_DIVS_V / 2) * div),
        .mid = (int16_t)(top + (bot - top) / 2),
    };
}

/* Row `row` of pane `from` at the same place on pane `to`'s graticule (in
 * divisions from the top). The identity when the two have the same top and
 * division. */
static inline int16_t plot_pane_row_to(const plot_pane_t *from, const plot_pane_t *to, int row)
{
    return (int16_t)(to->top + ((row - from->top) * to->div_px) / from->div_px);
}

#endif /* PLOT_PANE_H */
