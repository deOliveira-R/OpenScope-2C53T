/*
 * Measurement strip: where the scope's automatic measurements sit on the plot.
 *
 * Pure layout (no LCD, no FPGA, no scope state), host-tested in
 * tests/test_measure_strip.c. scope_ui.c draws what this places.
 *
 * Stock V1.2.0 (photos scope/IMG_0324.jpeg, lines.jpeg): three columns over
 * the bottom of the plot, CH1's selected measurements first and then CH2's,
 * flowing left to right and top to bottom, each in its channel's colour,
 * "Label:value". Ours keeps the flow and the colours, and separates label and
 * value instead of running them together ("Freq10.00MHz" before round 1): the
 * label in a dimmed channel colour, the value in the bright one, each in its
 * own column of the cell (label MSTRIP_LABEL_W, then MSTRIP_GAP, then the
 * value left-aligned), so the values of a column line up and a value never
 * runs into the next cell's label.
 *
 * Geometry (see measure_strip_layout):
 *   - up to MSTRIP_COLS (3) cells per row, each MSTRIP_CELL_W wide;
 *   - rows are MSTRIP_ROW_H tall and the block is anchored at `bottom` (the
 *     top of the info bar), so it grows upwards as more is selected;
 *   - an optional full-width AUX row (mask pass/fail readout, CAN line) sits
 *     directly ABOVE the measurement rows, so it can come and go without
 *     moving them;
 *   - `top` is the highest row of strip furniture: the live trace band stops
 *     there.
 *
 * Bit i of a channel mask is scope_prm_id_t i (stock's 12, scope_measure.h),
 * the same convention as meas_select_get().
 */
#ifndef MEASURE_STRIP_H
#define MEASURE_STRIP_H

#include <stdbool.h>
#include <stdint.h>

#define MSTRIP_COLS        3u
#define MSTRIP_CELL_W      106u   /* 3 x 106 = 318: the glass less a 1 px margin each side */
#define MSTRIP_X0          1u
#define MSTRIP_ROW_H       13u    /* font_small (12 px) + 1 */
#define MSTRIP_PAD         2u     /* inside each cell, left and right */
#define MSTRIP_GAP         4u     /* label column to value column */
#define MSTRIP_LABEL_W     33u    /* label column: stock's widest label, "Duty+" in font_small */
#define MSTRIP_VALUE_X     (MSTRIP_PAD + MSTRIP_LABEL_W + MSTRIP_GAP)   /* value column, in the cell */
#define MSTRIP_VALUE_W     (MSTRIP_CELL_W - MSTRIP_VALUE_X - MSTRIP_PAD) /* 65 px */
#define MSTRIP_MAX_PER_CH  6u     /* UI_STUDY S9: up to 6 per channel */
#define MSTRIP_MAX_ROWS    4u     /* 2 x 6 cells in 3 columns */
#define MSTRIP_MAX_CELLS   (MSTRIP_MAX_ROWS * MSTRIP_COLS)
#define MSTRIP_PRM_COUNT   12u    /* == SCOPE_PRM_COUNT (asserted in scope_ui.c) */

typedef struct {
    uint8_t  ch;          /* 0 = CH1, 1 = CH2 */
    uint8_t  prm;         /* scope_prm_id_t */
    uint16_t x, y;        /* top-left of the cell */
} mstrip_cell_t;

typedef struct {
    uint8_t       n;              /* cells placed */
    uint8_t       rows;           /* measurement rows */
    uint8_t       dropped;        /* selected but not shown (per-channel cap) */
    bool          aux;            /* the AUX row is reserved */
    uint16_t      aux_y;          /* its top, when aux */
    uint16_t      top;            /* top of all strip furniture (== bottom if empty) */
    uint16_t      bottom;         /* as passed */
    mstrip_cell_t cell[MSTRIP_MAX_CELLS];
} mstrip_layout_t;

/*
 * Place the cells. mask1/mask2: the selection per channel; a disabled channel
 * contributes nothing. hidden: stock's "hide measurement parameters" (long PRM)
 * - no cells, but the AUX row is still placed when `aux`. Each channel shows at
 * most MSTRIP_MAX_PER_CH, in stock order; the rest are counted in `dropped`.
 */
void measure_strip_layout(uint16_t mask1, bool en1, uint16_t mask2, bool en2,
                          bool hidden, bool aux, uint16_t bottom,
                          mstrip_layout_t *out);

/* The `top` measure_strip_layout() would return, without filling cells: what
 * the live compositor calls every frame. */
uint16_t measure_strip_top(uint16_t mask1, bool en1, uint16_t mask2, bool en2,
                           bool hidden, bool aux, uint16_t bottom);

/*
 * Text placement inside a cell of width MSTRIP_CELL_W at x: label at
 * x + MSTRIP_PAD, value at x + MSTRIP_VALUE_X. Returns true when the label
 * fits its column and the value its own (MSTRIP_VALUE_W); false means the
 * caller draws the VALUE only, right-aligned inside the cell: a value is never
 * overlapped or cut.
 */
bool measure_strip_place(uint16_t x, uint16_t label_w, uint16_t value_w,
                         uint16_t *label_x, uint16_t *value_x);

/* The label colour: the channel colour blended halfway to the background
 * (RGB565), so it reads as the channel's but steps back from the value. */
uint16_t measure_strip_dim(uint16_t fg, uint16_t bg);

/* R1-A's interface (src/ui/meas_select.h). measure_strip.c carries a weak
 * stub returning today's displayed set until meas_select.c is linked. */
uint16_t meas_select_get(uint8_t ch);

#endif /* MEASURE_STRIP_H */
