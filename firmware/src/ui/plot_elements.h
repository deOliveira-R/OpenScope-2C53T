/*
 * OpenScope 2C53T - plot elements shared by the display types
 *
 * What more than one display type draws the same way (docs/design/
 * display_architecture.md, "Element library"): the graticule and a trace of
 * record codes against a vertical map. An element paints what it owns of one
 * column into the canvas's strip.
 */

#ifndef PLOT_ELEMENTS_H
#define PLOT_ELEMENTS_H

#include <stdint.h>
#include <stdbool.h>
#include "canvas.h"
#include "theme.h"
#include "yt_vmap.h"
#include "plot_pane.h"

/* The graticule of the time views in pane p (scope_geom.h, plot_pane.h):
 * dotted lines every division, a solid centre cross, in the 301 columns from
 * 0. Columns right of them carry none. */
void plot_grid_paint(const theme_t *th, const plot_pane_t *p, uint16_t x, const canvas_strip_t *s);
/* The same as a rule for one pixel: its colour into *c, or false. */
bool plot_grid_px(const theme_t *th, const plot_pane_t *p, uint16_t x, int y, uint16_t *c);

/* What one column of one trace shows. */
typedef struct {
    bool     on;                /* the trace has a sample in this column */
    int16_t  y;                 /* its row, kept on the map's band */
    int16_t  from;              /* the previous column's row, -1 when there is none */
    int16_t  lo, hi;            /* rows drawn in `color`: the sample's and the span to the previous column's */
    int16_t  tip_lo, tip_hi;    /* the rows of the sample alone ... */
    uint16_t color;
    uint16_t tip;               /* ... and their colour: a clip colour when the sample is cut off, else `color` */
    bool     off_plot;          /* the sample is valid but off the plot (and not at the converter's limit) */
} plot_trace_col_t;

/* Work out one column of a trace.
 *   map       the band the trace is drawn in (yt_vmap.h);
 *   landed    the row the column's sample lands on through the caller's
 *             own code-to-row transform, on or off that band;
 *   joined    there is a previous column to join to, whose row is prev;
 *   seen_end  the first row that cannot be seen (the measurement strip's
 *             top): a sample that lands there or beyond the map's band is
 *             off the plot and is drawn on the last rows that can be seen;
 *   at_rail   the trace sits at the converter's limit here
 *             (plot_rail_columns);
 *   color     the trace's colour in this column.
 * A trace is SCOPE_TRACE_ROWS thick. A cut-off sample colours its own rows
 * only, the converter's limit before off the plot; the span to the previous
 * column keeps `color`. */
void plot_trace_col(plot_trace_col_t *t, const yt_vmap_t *map, int landed, bool joined, int16_t prev,
                    int seen_end, bool at_rail, uint16_t color, const theme_t *th);

/* Paint the column; nothing when !on. */
void plot_trace_paint(const plot_trace_col_t *t, const canvas_strip_t *s);
/* The same as a rule for one pixel. */
bool plot_trace_px(const plot_trace_col_t *t, int y, uint16_t *c);

/* One bit per column of [0, PLOT_RAIL_COLS): the trace sits on a converter
 * rail there. */
#define PLOT_RAIL_COLS   301u       /* SCOPE_GEOM_COLS (asserted in plot_elements.c) */
#define PLOT_RAIL_BYTES  ((PLOT_RAIL_COLS + 7u) / 8u)

/* Mark in bits the columns of [first, end) where cols sits on a converter
 * rail: record code 0 or REC_CODE_MAX (units.h) for at least
 * REC_RAIL_RUN_MIN columns in a row. A run, as rec_rail_run asks of a
 * record: fast edges carry single samples that touch a rail, and one sample
 * is not a clipped input. */
void plot_rail_columns(const volatile uint8_t *cols, uint16_t first, uint16_t end, uint8_t *bits);

static inline bool plot_rail_at(const uint8_t *bits, uint16_t x)
{
    return x < PLOT_RAIL_COLS && (bits[x >> 3] & (1u << (x & 7u))) != 0u;
}

#endif /* PLOT_ELEMENTS_H */
