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
 * Split puts two panes in the band over the measurement strip, one above the
 * other with a divider row between them (plot_pane_split); the half the keys
 * are on is marked (the focus mark, view_table.h).
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

#include <stdbool.h>
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
    bool    marked;         /* framed by the focus mark: split's half the keys are on */
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

/* Rows between split's two panes: the divider line. */
#define PLOT_PANE_DIVIDER_ROWS  1

/* Split's two panes in the band [top, bot): the upper one from top, the
 * lower one down to bot, the divider row between them; the upper pane takes
 * the odd row. Each pane's traces end at its own bottom; marked: the half
 * (view_half_t order: 0 upper, 1 lower) the keys are on, -1 none. */
static inline void plot_pane_split(int16_t top, int16_t bot, canvas_id_t upper, canvas_id_t lower,
                                   int marked, plot_pane_t out[2])
{
    const int16_t up_bot = (int16_t)(top + (bot - top - PLOT_PANE_DIVIDER_ROWS + 1) / 2);
    const int16_t lo_top = (int16_t)(up_bot + PLOT_PANE_DIVIDER_ROWS);
    out[0] = plot_pane_make(upper, top, up_bot, up_bot);
    out[1] = plot_pane_make(lower, lo_top, bot, bot);
    out[0].marked = (marked == 0);
    out[1].marked = (marked == 1);
}

#endif /* PLOT_PANE_H */
