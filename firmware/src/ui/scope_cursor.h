/*
 * OpenScope 2C53T — the time views' cursors: units, screen and readout
 *
 * WHAT THIS IS
 * ------------
 * The arithmetic between a cursor line, kept in the unit of what it marks,
 * and the screen: where the line falls on the plot as drawn now, which value
 * a pixel is, what one arrow press does, where a line starts, and the
 * readout strings. It owns no scale of its own: seconds come from
 * scope_timebase.c (the core's exact rates, as whole ticks), volts from
 * scope_cal.c, and the vertical map is the one the renderer drew the active
 * channel with (yt_vmap.h), handed in as scope_cursor_axes_t.
 *
 * THE UNITS (cursor_state_t, the view.cur* properties)
 * -----------------------------------------------------
 *   Y-T time   ticks of the finest column (SCOPE_TIMEBASE_TICK_HZ, 0.4 ns)
 *              from the trigger point. Every column of every code, held or
 *              running, is a whole number of ticks, so a line put on a
 *              column comes back to it at every timebase.
 *   roll time  milliseconds back from the newest point: the roll periods
 *              are whole milliseconds.
 *   level      microvolts at the probe tip of the active channel.
 *
 * THE RULES
 * ---------
 *   - A line whose pixel is off the plot, or has no conversion (no band for
 *     the active channel, or a range with no volts), is not drawn.
 *   - The readout reads the stored values, never the pixels: exact, with a
 *     leading '~' when the table row it rests on is PROVISIONAL. A level
 *     with no volts to convert through reads "--", as does 1/dt with the
 *     two lines on one time.
 *
 * Host-testable: no hardware, no RTOS. See tests/test_scope_cursor.c.
 */

#ifndef SCOPE_CURSOR_H
#define SCOPE_CURSOR_H

#include <stdbool.h>
#include <stdint.h>

#include "../util/units.h"   /* tb_code_t, cursor_order_t, microvolts_t */
#include "scope_state.h"     /* cursor_sel_t, CURSOR_STEP_PX, SCOPE_PROBE_10X_FACTOR */
#include "scope_geom.h"      /* SCOPE_GEOM_COLS */
#include "scope_roll.h"      /* SCOPE_ROLL_POINTS, SCOPE_ROLL_SLOWEST_PERIOD_MS */
#include "scope_timebase.h"  /* SCOPE_TIMEBASE_TICK_HZ, SCOPE_TIMEBASE_SLOWEST_COLUMN_TICKS */
#include "yt_vmap.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── The ranges of the lines (model_table.h holds them as literals) ──── */

/* Y-T: +- the plot's columns at the slowest column, 1.505e9 ticks (0.6 s). */
#define SCOPE_CURSOR_TIME_MAX \
    ((int32_t)((uint32_t)SCOPE_GEOM_COLS * SCOPE_TIMEBASE_SLOWEST_COLUMN_TICKS))
_Static_assert((uint64_t)SCOPE_GEOM_COLS * SCOPE_TIMEBASE_SLOWEST_COLUMN_TICKS <= (uint64_t)INT32_MAX,
               "the Y-T cursor time range must fit an int32 property");

/* Roll: 0 (the newest point) back to the FIFO's points at the slowest
 * period, -240800 ms. */
#define SCOPE_CURSOR_ROLL_MIN_MS \
    (-(int32_t)(SCOPE_ROLL_POINTS * SCOPE_ROLL_SLOWEST_PERIOD_MS))

/* Level: the highest range's full scale (the converter's 256 codes at
 * stock's 400 mV a count, scope_cal_nominal_mv(9)) at the 10X probe is
 * 1024 V. A row of the plot reads up to a whole code span from the 0 V code
 * (a position at a rail), and a unit's own gain reads up to 1 / 0.8 of the
 * nominal ladder (calpage.h CALPAGE_M_MIN), so the range is twice that:
 * +-2048 V, which holds every level the plot can show (test_scope_cursor). */
#define SCOPE_CURSOR_ADC_CODES          256
#define SCOPE_CURSOR_TOP_RANGE_UV_PER_CODE  400000
#define SCOPE_CURSOR_LEVEL_HEADROOM     2
#define SCOPE_CURSOR_LEVEL_MAX \
    ((int32_t)(SCOPE_CURSOR_LEVEL_HEADROOM * SCOPE_CURSOR_ADC_CODES * \
               SCOPE_CURSOR_TOP_RANGE_UV_PER_CODE * SCOPE_PROBE_10X_FACTOR))
_Static_assert((int64_t)SCOPE_CURSOR_LEVEL_HEADROOM * SCOPE_CURSOR_ADC_CODES *
               SCOPE_CURSOR_TOP_RANGE_UV_PER_CODE * SCOPE_PROBE_10X_FACTOR <= (int64_t)INT32_MAX,
               "the level range must fit an int32 property");

/* What a reading is expressed in. */
typedef enum {
    SCOPE_CURSOR_UNIT_NONE = 0,   /* nothing honest to print — renders "--" */
    SCOPE_CURSOR_UNIT_SAMPLES,    /* raw time (the measurement badges) */
    SCOPE_CURSOR_UNIT_COUNTS,     /* raw level (the measurement badges) */
    SCOPE_CURSOR_UNIT_SECONDS,
    SCOPE_CURSOR_UNIT_VOLTS,
    SCOPE_CURSOR_UNIT_HERTZ,
} scope_cursor_unit_t;

/* How much the reading is worth. RAW is not a lesser tier — a raw count is
 * exact; it just is not volts. */
typedef enum {
    SCOPE_CURSOR_RAW = 0,       /* no calibration used, quantity is exact  */
    SCOPE_CURSOR_PROVISIONAL,   /* converted through a PROVISIONAL row     */
    SCOPE_CURSOR_MEASURED,      /* converted through a MEASURED row        */
} scope_cursor_conf_t;

typedef struct {
    scope_cursor_unit_t unit;
    scope_cursor_conf_t confidence;
    float               value;   /* signed, in `unit`; 0 when unit is NONE */
} scope_cursor_reading_t;

/* Confidence of a timebase code's rate, and of a (channel, range) gain. */
scope_cursor_conf_t scope_cursor_tb_conf(tb_code_t tb_code);
scope_cursor_conf_t scope_cursor_cal_conf(uint8_t channel, uint8_t range_idx);

/*
 * The plot as drawn now, for the lines: the time axis of the source on the
 * screen and the active channel's vertical map. The display fills it
 * (scope_ui_cursor_axes, scope_cursor_draw.c); everything below is pure.
 */
typedef struct scope_cursor_axes {
    /* time: column ref_col is time 0, one column is per_col of the unit */
    bool                roll;        /* roll's milliseconds, else Y-T's ticks */
    uint32_t            per_col;     /* ticks or ms per column */
    int32_t             ref_col;     /* the trigger point's column; roll: the newest point's */
    scope_cursor_conf_t t_conf;
    int16_t             cols;        /* the plot's columns [0, cols) */
    /* level: the active channel's map; level_ok false = no conversion */
    bool                level_ok;
    uint8_t             channel;     /* 1 or 2 */
    yt_vmap_t           map;         /* the map its band was drawn with */
    float               zero_code;   /* its 0 V, in the map's codes */
    float               volts_per_code;   /* at the probe tip, in the map's codes */
    scope_cursor_conf_t v_conf;
    int16_t             row_top, row_end; /* the band's visible rows [row_top, row_end) */
} scope_cursor_axes_t;

/* The range of the property behind line sel (roll's or Y-T's time pair, the
 * level pair). */
void scope_cursor_range(const scope_cursor_axes_t *ax, cursor_sel_t sel, int32_t *lo, int32_t *hi);

/* The pixel (column for V1/V2, row for H1/H2) of value v, on or off the
 * plot; false when there is no conversion. */
bool scope_cursor_px(const scope_cursor_axes_t *ax, cursor_sel_t sel, int32_t v, int32_t *px);

/* The value at pixel px, inside the property's range; false when there is
 * no conversion. scope_cursor_px of the result is px again. */
bool scope_cursor_at(const scope_cursor_axes_t *ax, cursor_sel_t sel, int32_t px, int32_t *v);

/* Pixel px is on the plot for line sel. */
bool scope_cursor_on_plot(const scope_cursor_axes_t *ax, cursor_sel_t sel, int32_t px);

/* One arrow press (dir -1 = UP: left / up, +1 = DOWN: right / down): the
 * line CURSOR_STEP_PX pixels on, kept on the plot; a line that is off the
 * plot comes to the edge on its side first. False (and *out = v) when there
 * is no conversion. */
bool scope_cursor_key(const scope_cursor_axes_t *ax, cursor_sel_t sel, int32_t v, int dir, int32_t *out);

/* Where line sel starts: a third (V1, H1) and two thirds (V2, H2) across
 * the plot's columns or down the band's rows. False with no conversion. */
#define SCOPE_CURSOR_DEFAULT_PARTS  3   /* the plot in thirds: line 1 at one, line 2 at two */
bool scope_cursor_default(const scope_cursor_axes_t *ax, cursor_sel_t sel, int32_t *v);

/* A pair coming onto the screen (first = V1 or H1): each of its two lines
 * that is off the plot, or not convertible, goes to its default where there
 * is one. */
void scope_cursor_place(const scope_cursor_axes_t *ax, cursor_sel_t first, int32_t v[2]);

/* Readouts from stored values. X: a time (seconds from the trigger point,
 * or back from the newest point); dt and 1/dt of two times in `order`
 * (1/dt NONE on one time); Y: a level; dV of two levels (NONE without a
 * conversion for the level pair). */
scope_cursor_reading_t scope_cursor_x(const scope_cursor_axes_t *ax, int32_t t);
scope_cursor_reading_t scope_cursor_dt(const scope_cursor_axes_t *ax, int32_t first, int32_t second,
                                       cursor_order_t order);
scope_cursor_reading_t scope_cursor_inv_dt(const scope_cursor_axes_t *ax, int32_t first, int32_t second,
                                           cursor_order_t order);
scope_cursor_reading_t scope_cursor_y(const scope_cursor_axes_t *ax, microvolts_t level);
scope_cursor_reading_t scope_cursor_dv(const scope_cursor_axes_t *ax, microvolts_t first, microvolts_t second,
                                       cursor_order_t order);

/* The column rate of the time axis, Hz (the shell's fs). */
float scope_cursor_col_rate(const scope_cursor_axes_t *ax);

/*
 * Render a reading: "1.23ms", "~213us" (provisional), "-45smp", "37cnt",
 * "--". Always NUL-terminated; 16 bytes is enough for every case.
 * The '~' leads, as it does in scope_cal_range_label().
 */
void scope_cursor_format(const scope_cursor_reading_t *r, char *out, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif /* SCOPE_CURSOR_H */
