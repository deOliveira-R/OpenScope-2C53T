/*
 * OpenScope 2C53T — the time views' cursors: units, screen and readout
 *
 * See scope_cursor.h. Deliberately free of hardware and RTOS dependencies
 * so it can be exercised on the host — see tests/test_scope_cursor.c.
 */

#include "scope_cursor.h"

#include "scope_cal.h"

#include <math.h>
#include <stdio.h>
#include <stddef.h>

/* The string every other part of the scope UI uses for "cannot measure". */
#define CURSOR_NA   "--"

/* A pixel this far off the plot is as off as any: a line's float row is
 * clamped to it before it becomes an integer. */
#define CURSOR_PX_FAR   100000

/* Microvolts in a volt. */
#define CURSOR_UV_PER_V 1e6f

/* Milliseconds in a second: roll's time unit. */
#define CURSOR_MS_PER_S 1000.0f

static scope_cursor_reading_t make(scope_cursor_unit_t u,
                                   scope_cursor_conf_t c,
                                   float v)
{
    scope_cursor_reading_t r;
    r.unit = u;
    r.confidence = c;
    r.value = v;
    return r;
}

/* scope_cal / scope_timebase tiers -> cursor confidence. The mapping is
 * deliberately total: a tier this module does not recognise must not silently
 * become MEASURED. */
static scope_cursor_conf_t conf_from_cal(scope_cal_tier_t t)
{
    /* FACTORY = this unit's own stock calibration (scope_cal.h): the same
     * scale stock's own cursors use, so it reads as a measured quantity. */
    if (t == SCOPE_CAL_MEASURED || t == SCOPE_CAL_FACTORY) return SCOPE_CURSOR_MEASURED;
    if (t == SCOPE_CAL_PROVISIONAL) return SCOPE_CURSOR_PROVISIONAL;
    return SCOPE_CURSOR_RAW;
}

static scope_cursor_conf_t conf_from_tb(scope_tb_tier_t t)
{
    if (t == SCOPE_TB_MEASURED)    return SCOPE_CURSOR_MEASURED;
    if (t == SCOPE_TB_PROVISIONAL) return SCOPE_CURSOR_PROVISIONAL;
    return SCOPE_CURSOR_RAW;
}

scope_cursor_conf_t scope_cursor_tb_conf(tb_code_t tb_code)
{
    return conf_from_tb(scope_timebase_get_tier(tb_code.code));
}

scope_cursor_conf_t scope_cursor_cal_conf(uint8_t channel, uint8_t range_idx)
{
    return conf_from_cal(scope_cal_get_tier(channel, range_idx));
}

/* ── lines and pixels ──────────────────────────────────────────────── */

static bool is_time(cursor_sel_t sel)
{
    return sel == CURSOR_SEL_V1 || sel == CURSOR_SEL_V2;
}

void scope_cursor_range(const scope_cursor_axes_t *ax, cursor_sel_t sel, int32_t *lo, int32_t *hi)
{
    if (!is_time(sel)) {
        *lo = -SCOPE_CURSOR_LEVEL_MAX;
        *hi = SCOPE_CURSOR_LEVEL_MAX;
    } else if (ax->roll) {
        *lo = SCOPE_CURSOR_ROLL_MIN_MS;
        *hi = 0;
    } else {
        *lo = -SCOPE_CURSOR_TIME_MAX;
        *hi = SCOPE_CURSOR_TIME_MAX;
    }
}

static int32_t in_range(const scope_cursor_axes_t *ax, cursor_sel_t sel, int64_t v)
{
    int32_t lo, hi;
    scope_cursor_range(ax, sel, &lo, &hi);
    return (int32_t)(v < lo ? lo : v > hi ? hi : v);
}

/* a / b to the nearest integer, halves up; b > 0. */
static int64_t div_nearest(int64_t a, int64_t b)
{
    int64_t q = a / b, r = a % b;
    if (r < 0) { q--; r += b; }
    return (2 * r >= b) ? q + 1 : q;
}

static int32_t px_of(float x)
{
    if (x > (float)CURSOR_PX_FAR)  return CURSOR_PX_FAR;
    if (x < -(float)CURSOR_PX_FAR) return -CURSOR_PX_FAR;
    return (int32_t)floorf(x + 0.5f);
}

/* The axis of line sel converts at all. */
static bool converts(const scope_cursor_axes_t *ax, cursor_sel_t sel)
{
    return is_time(sel) ? ax->per_col > 0u
                        : ax->level_ok && ax->volts_per_code > 0.0f && ax->row_end > ax->row_top;
}

bool scope_cursor_px(const scope_cursor_axes_t *ax, cursor_sel_t sel, int32_t v, int32_t *px)
{
    if (!converts(ax, sel))
        return false;
    if (is_time(sel)) {
        *px = (int32_t)(ax->ref_col + div_nearest(v, (int64_t)ax->per_col));
        return true;
    }
    const float code = ax->zero_code + (float)v / CURSOR_UV_PER_V / ax->volts_per_code;
    *px = px_of(yt_vmap_row_of(&ax->map, code));
    return true;
}

bool scope_cursor_at(const scope_cursor_axes_t *ax, cursor_sel_t sel, int32_t px, int32_t *v)
{
    if (!converts(ax, sel))
        return false;
    if (is_time(sel)) {
        *v = in_range(ax, sel, ((int64_t)px - ax->ref_col) * (int64_t)ax->per_col);
        return true;
    }
    const float uv = (yt_vmap_code_of(&ax->map, (float)px) - ax->zero_code) * ax->volts_per_code * CURSOR_UV_PER_V;
    const float lim = (float)SCOPE_CURSOR_LEVEL_MAX;
    *v = in_range(ax, sel, uv > lim ? SCOPE_CURSOR_LEVEL_MAX : uv < -lim ? -SCOPE_CURSOR_LEVEL_MAX
                                                                      : (int64_t)floorf(uv + 0.5f));
    return true;
}

/* The first and last pixel of the plot for line sel. */
static void plot_px(const scope_cursor_axes_t *ax, cursor_sel_t sel, int32_t *lo, int32_t *hi)
{
    *lo = is_time(sel) ? 0 : ax->row_top;
    *hi = is_time(sel) ? ax->cols - 1 : ax->row_end - 1;
}

bool scope_cursor_on_plot(const scope_cursor_axes_t *ax, cursor_sel_t sel, int32_t px)
{
    int32_t lo, hi;
    plot_px(ax, sel, &lo, &hi);
    return px >= lo && px <= hi;
}

bool scope_cursor_key(const scope_cursor_axes_t *ax, cursor_sel_t sel, int32_t v, int dir, int32_t *out)
{
    int32_t px, lo, hi;
    *out = v;
    if (!scope_cursor_px(ax, sel, v, &px))
        return false;
    plot_px(ax, sel, &lo, &hi);
    if (px < lo)      px = lo;
    else if (px > hi) px = hi;
    else {
        px += dir * CURSOR_STEP_PX;
        if (px < lo) px = lo;
        if (px > hi) px = hi;
    }
    return scope_cursor_at(ax, sel, px, out);
}

bool scope_cursor_default(const scope_cursor_axes_t *ax, cursor_sel_t sel, int32_t *v)
{
    const int32_t k = (sel == CURSOR_SEL_V2 || sel == CURSOR_SEL_H2) ? 2 : 1;   /* line 1 or 2 */
    const int32_t px = is_time(sel) ? (ax->cols * k) / SCOPE_CURSOR_DEFAULT_PARTS
                                    : ax->row_top + ((ax->row_end - ax->row_top) * k) / SCOPE_CURSOR_DEFAULT_PARTS;
    return scope_cursor_at(ax, sel, px, v);
}

void scope_cursor_place(const scope_cursor_axes_t *ax, cursor_sel_t first, int32_t v[2])
{
    for (int i = 0; i < 2; i++) {
        const cursor_sel_t sel = (cursor_sel_t)(first + i);
        int32_t px, d;
        if (scope_cursor_px(ax, sel, v[i], &px) && scope_cursor_on_plot(ax, sel, px))
            continue;
        if (scope_cursor_default(ax, sel, &d))
            v[i] = d;
    }
}

/* ── readouts ──────────────────────────────────────────────────────── */

/* The time unit's rate: ticks or milliseconds per second. */
static float t_unit_hz(const scope_cursor_axes_t *ax)
{
    return ax->roll ? CURSOR_MS_PER_S : (float)SCOPE_TIMEBASE_TICK_HZ;
}

scope_cursor_reading_t scope_cursor_x(const scope_cursor_axes_t *ax, int32_t t)
{
    return make(SCOPE_CURSOR_UNIT_SECONDS, ax->t_conf, (float)t / t_unit_hz(ax));
}

scope_cursor_reading_t scope_cursor_dt(const scope_cursor_axes_t *ax, int32_t first, int32_t second,
                                       cursor_order_t order)
{
    return make(SCOPE_CURSOR_UNIT_SECONDS, ax->t_conf,
                (float)cursor_delta(first, second, order) / t_unit_hz(ax));
}

scope_cursor_reading_t scope_cursor_inv_dt(const scope_cursor_axes_t *ax, int32_t first, int32_t second,
                                           cursor_order_t order)
{
    const int64_t d = cursor_delta(first, second, order);
    /* Two lines on one time: there is no interval, so there is no frequency. */
    if (d == 0)
        return make(SCOPE_CURSOR_UNIT_NONE, SCOPE_CURSOR_RAW, 0.0f);
    return make(SCOPE_CURSOR_UNIT_HERTZ, ax->t_conf, t_unit_hz(ax) / (float)(d < 0 ? -d : d));
}

scope_cursor_reading_t scope_cursor_y(const scope_cursor_axes_t *ax, microvolts_t level)
{
    if (!converts(ax, CURSOR_SEL_H1))
        return make(SCOPE_CURSOR_UNIT_NONE, SCOPE_CURSOR_RAW, 0.0f);
    return make(SCOPE_CURSOR_UNIT_VOLTS, ax->v_conf, (float)level.uv / CURSOR_UV_PER_V);
}

scope_cursor_reading_t scope_cursor_dv(const scope_cursor_axes_t *ax, microvolts_t first, microvolts_t second,
                                       cursor_order_t order)
{
    if (!converts(ax, CURSOR_SEL_H1))
        return make(SCOPE_CURSOR_UNIT_NONE, SCOPE_CURSOR_RAW, 0.0f);
    return make(SCOPE_CURSOR_UNIT_VOLTS, ax->v_conf,
                (float)cursor_delta(first.uv, second.uv, order) / CURSOR_UV_PER_V);
}

float scope_cursor_col_rate(const scope_cursor_axes_t *ax)
{
    return ax->per_col ? t_unit_hz(ax) / (float)ax->per_col : 0.0f;
}

/* ── formatting ─────────────────────────────────────────────────────────
 *
 * Magnitude only; the caller below prepends the marker and the sign.
 */

static void fmt_seconds_mag(char *b, uint32_t n, float s)
{
    if (s < 1e-6f) {
        snprintf(b, n, "%uns", (unsigned)(s * 1e9f + 0.5f));
    } else if (s < 1e-3f) {
        snprintf(b, n, "%uus", (unsigned)(s * 1e6f + 0.5f));
    } else if (s < 1.0f) {
        const unsigned hundredths_ms = (unsigned)(s * 1e5f + 0.5f);
        snprintf(b, n, "%u.%02ums", hundredths_ms / 100u, hundredths_ms % 100u);
    } else {
        const unsigned hundredths = (unsigned)(s * 100.0f + 0.5f);
        snprintf(b, n, "%u.%02us", hundredths / 100u, hundredths % 100u);
    }
}

static void fmt_volts_mag(char *b, uint32_t n, float v)
{
    const unsigned mv = (unsigned)(v * 1000.0f + 0.5f);
    if (mv < 1000u)
        snprintf(b, n, "%umV", mv);
    else
        snprintf(b, n, "%u.%02uV", mv / 1000u, (mv % 1000u) / 10u);
}

static void fmt_hz_mag(char *b, uint32_t n, float hz)
{
    if (hz < 1000.0f) {
        const unsigned tenths = (unsigned)(hz * 10.0f + 0.5f);
        snprintf(b, n, "%u.%uHz", tenths / 10u, tenths % 10u);
    } else if (hz < 1000000.0f) {
        const unsigned hundredths = (unsigned)(hz / 10.0f + 0.5f);
        snprintf(b, n, "%u.%02ukHz", hundredths / 100u, hundredths % 100u);
    } else {
        const unsigned hundredths = (unsigned)(hz / 10000.0f + 0.5f);
        snprintf(b, n, "%u.%02uMHz", hundredths / 100u, hundredths % 100u);
    }
}

void scope_cursor_format(const scope_cursor_reading_t *r, char *out, uint32_t n)
{
    if (out == NULL || n == 0u)
        return;

    if (r == NULL || r->unit == SCOPE_CURSOR_UNIT_NONE) {
        snprintf(out, n, CURSOR_NA);
        return;
    }

    /* Leading '~' for a value converted through a PROVISIONAL table row —
     * the same one-character warning scope_cal_range_label() puts on the
     * status bar, for the same reason: it is the only place a user can see
     * the difference between "measured" and "roughly". */
    const char *mark = (r->confidence == SCOPE_CURSOR_PROVISIONAL) ? "~" : "";
    const char *sign = (r->value < 0.0f) ? "-" : "";
    const float mag  = (r->value < 0.0f) ? -r->value : r->value;

    char body[16];
    body[0] = '\0';

    switch (r->unit) {
    case SCOPE_CURSOR_UNIT_SECONDS:
        fmt_seconds_mag(body, sizeof(body), mag);
        break;
    case SCOPE_CURSOR_UNIT_VOLTS:
        fmt_volts_mag(body, sizeof(body), mag);
        break;
    case SCOPE_CURSOR_UNIT_HERTZ:
        fmt_hz_mag(body, sizeof(body), mag);
        break;
    case SCOPE_CURSOR_UNIT_SAMPLES:
        snprintf(body, sizeof(body), "%usmp", (unsigned)(mag + 0.5f));
        break;
    case SCOPE_CURSOR_UNIT_COUNTS:
        snprintf(body, sizeof(body), "%ucnt", (unsigned)(mag + 0.5f));
        break;
    default:
        snprintf(out, n, CURSOR_NA);
        return;
    }

    snprintf(out, n, "%s%s%s", mark, sign, body);
}
