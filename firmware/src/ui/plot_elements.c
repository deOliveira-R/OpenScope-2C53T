/*
 * OpenScope 2C53T - plot elements shared by the display types
 */

#include "plot_elements.h"
#include "scope_ui_parts.h"

_Static_assert(PLOT_RAIL_COLS == SCOPE_GEOM_COLS, "one rail bit per graticule column");

void plot_grid_paint(const theme_t *th, uint16_t x, const canvas_strip_t *s)
{
    if (x >= SCOPE_GEOM_COLS) return;
    if (scope_geom_vgrid((int)x))                           /* dotted vertical line: every other row */
        for (int y = SCOPE_TOP; y < s->hi; y++)
            if ((y & 1) == 0) strip_put(s, y, th->grid);
    if ((x & 1u) == 0)                                      /* dotted horizontal lines: every other column */
        for (int y = SCOPE_TOP; y < s->hi; y += SCOPE_DIV_PX) strip_put(s, y, th->grid);
    if (x == SCOPE_GEOM_CENTER_COL)
        strip_span(s, s->lo, s->hi - 1, th->grid_center);
    else
        strip_put(s, SCOPE_GRID_MID_Y, th->grid_center);
}

bool plot_grid_px(const theme_t *th, uint16_t x, int y, uint16_t *c)
{
    if (x >= SCOPE_GEOM_COLS) return false;
    if (y == SCOPE_GRID_MID_Y || x == SCOPE_GEOM_CENTER_COL) { *c = th->grid_center; return true; }
    if ((scope_geom_vgrid((int)x) && (y & 1) == 0) ||
        (((y - SCOPE_TOP) % SCOPE_DIV_PX) == 0 && (x & 1u) == 0)) { *c = th->grid; return true; }
    return false;
}

void plot_trace_col(plot_trace_col_t *t, const yt_vmap_t *map, int landed, bool joined, int16_t prev,
                    int seen_end, bool at_rail, uint16_t color, const theme_t *th)
{
    /* The row kept on the band. What can be seen of the band ends at
     * seen_end. */
    const int seen_bot = (map->y_bot < seen_end) ? map->y_bot : seen_end;
    const bool off = landed < map->y_top || landed >= seen_bot;
    const int16_t y = yt_vmap_clip(map, landed);

    t->on = true;
    t->y = y;
    t->from = joined ? prev : -1;
    t->lo = y;
    t->hi = (int16_t)(y + SCOPE_TRACE_ROWS - 1);
    if (joined) {
        if (prev < t->lo) t->lo = prev;
        if (prev > t->hi) t->hi = prev;
    }
    /* The sample's own rows. One at or under seen_end would paint nothing:
     * it is drawn on the last rows that can be seen. */
    t->tip_lo = y;
    if (landed >= seen_bot) {
        t->tip_lo = (int16_t)(seen_bot - SCOPE_TRACE_ROWS);
        if (t->lo > t->tip_lo) t->lo = t->tip_lo;
    }
    t->tip_hi = (int16_t)(t->tip_lo + SCOPE_TRACE_ROWS - 1);
    t->color = color;
    t->tip = at_rail ? th->clip_converter : off ? th->clip_screen : color;
    t->off_plot = off && !at_rail;
}

void plot_trace_paint(const plot_trace_col_t *t, const canvas_strip_t *s)
{
    if (!t->on) return;
    strip_span(s, t->lo, t->hi, t->color);
    if (t->tip != t->color) strip_span(s, t->tip_lo, t->tip_hi, t->tip);
}

bool plot_trace_px(const plot_trace_col_t *t, int y, uint16_t *c)
{
    if (!t->on) return false;
    if (y >= t->tip_lo && y <= t->tip_hi) { *c = t->tip; return true; }
    if (y >= t->lo && y <= t->hi) { *c = t->color; return true; }
    return false;
}

void plot_rail_columns(const volatile uint8_t *cols, uint16_t first, uint16_t end, uint8_t *bits)
{
    memset(bits, 0, PLOT_RAIL_BYTES);
    uint16_t run = 0;
    int was = 0;                            /* the rail of the run so far: -1 low, +1 high, 0 none */
    for (uint16_t x = first; x <= end; x++) {
        int rail = 0;
        if (x < end) {
            const uint8_t v = cols[x];
            rail = (v == 0u) ? -1 : (v >= REC_CODE_MAX) ? 1 : 0;
        }
        if (rail != 0 && rail == was) { run++; continue; }
        if (was != 0 && run >= REC_RAIL_RUN_MIN)
            for (uint16_t k = (uint16_t)(x - run); k < x; k++) bits[k >> 3] |= (uint8_t)(1u << (k & 7u));
        was = rail;
        run = (rail != 0) ? 1u : 0u;
    }
}
