/*
 * OpenScope 2C53T — roll mode, pure half. See scope_roll.h for the stock
 * evidence and for why each difference from stock is there.
 */

#include "scope_roll.h"

#include <stdio.h>
#include <string.h>

/* Stock TMR3 auto-reload values, table 0x080465A4 (nine words, then a
 * non-table word), indexed by ms[0x2D] - 0x14 at 0x0800B6E0..0x0800B700. */
static const uint16_t roll_arr[SCOPE_ROLL_STEPS] = {
    19u, 39u, 79u, 199u, 399u, 799u, 1999u, 3999u, 7999u,
};

uint16_t scope_roll_arr(uint8_t step)
{
    return (step < SCOPE_ROLL_STEPS) ? roll_arr[step] : 0u;
}

uint32_t scope_roll_period_ms(uint8_t step)
{
    if (step >= SCOPE_ROLL_STEPS)
        return 0u;
    return ((uint32_t)roll_arr[step] + 1u) * 1000u / SCOPE_ROLL_TICK_HZ;
}

float scope_roll_seconds_per_div(uint8_t step)
{
    return (float)scope_roll_period_ms(step) * (float)SCOPE_ROLL_POINTS_PER_DIV / 1000.0f;
}

void scope_roll_label(uint8_t step, char *out, uint32_t n)
{
    if (out == NULL || n == 0u)
        return;
    uint32_t ms = scope_roll_period_ms(step) * SCOPE_ROLL_POINTS_PER_DIV;
    if (ms == 0u)
        snprintf(out, n, "--");
    else if (ms < 1000u)
        snprintf(out, n, "%lums", (unsigned long)ms);
    else
        snprintf(out, n, "%lus", (unsigned long)(ms / 1000u));
}

uint8_t scope_roll_stock_code(uint8_t step)
{
    if (step >= SCOPE_ROLL_STEPS)
        step = SCOPE_ROLL_STEPS - 1u;
    return (uint8_t)(SCOPE_ROLL_STOCK_CODE_BASE + step);
}

void scope_roll_fifo_reset(scope_roll_fifo_t *f)
{
    if (f == NULL)
        return;
    f->seq++;
    f->head = 0u;
    f->count = 0u;
    f->total = 0u;
    f->seq++;
}

void scope_roll_fifo_push(scope_roll_fifo_t *f, uint8_t ch1, uint8_t ch2)
{
    if (f == NULL)
        return;
    f->seq++;                                   /* odd: writing */
    f->pt[0][f->head] = ch1;
    f->pt[1][f->head] = ch2;
    f->head = (uint16_t)((f->head + 1u) % SCOPE_ROLL_POINTS);
    if (f->count < SCOPE_ROLL_POINTS)
        f->count++;
    f->total++;
    f->seq++;                                   /* even: stable */
}

bool scope_roll_fifo_get(const scope_roll_fifo_t *f, uint8_t ch, uint16_t age,
                         uint8_t *out)
{
    if (f == NULL || ch > 1u || age >= f->count || out == NULL)
        return false;
    /* head is one past the newest; the newest is head - 1. */
    uint32_t slot = ((uint32_t)f->head + SCOPE_ROLL_POINTS - 1u - age) % SCOPE_ROLL_POINTS;
    *out = f->pt[ch][slot];
    return true;
}

bool scope_roll_fifo_snapshot(const scope_roll_fifo_t *src, scope_roll_fifo_t *dst)
{
    if (src == NULL || dst == NULL)
        return false;
    for (int attempt = 0; attempt < 4; attempt++) {
        uint32_t s0 = src->seq;
        if (s0 & 1u)
            continue;
        memcpy(dst->pt, src->pt, sizeof(dst->pt));
        dst->head = src->head;
        dst->count = src->count;
        dst->total = src->total;
        if (src->seq == s0) {
            dst->seq = 0u;
            return true;
        }
    }
    dst->seq = 0u;
    return false;
}

uint8_t scope_roll_record_code(uint8_t raw, int adc_offset)
{
    int v = (int)raw + adc_offset;
    if (v < 0)   v = 0;
    if (v > 255) v = 255;
    return (uint8_t)v;
}

bool scope_roll_tb_step(uint8_t *yt_idx, bool *roll, uint8_t *step, int dir,
                        uint8_t yt_count)
{
    if (yt_idx == NULL || roll == NULL || step == NULL || yt_count == 0u || dir == 0)
        return false;
    if (!*roll) {
        if (dir > 0 && *yt_idx >= yt_count - 1u) {
            *yt_idx = (uint8_t)(yt_count - 1u);
            *roll = true;
            *step = 0u;
            return true;
        }
        int i = (int)*yt_idx + (dir > 0 ? 1 : -1);
        if (i < 0) i = 0;
        if (i > (int)yt_count - 1) i = (int)yt_count - 1;
        *yt_idx = (uint8_t)i;
        return false;
    }
    if (dir < 0 && *step == 0u) {
        *roll = false;
        *yt_idx = (uint8_t)(yt_count - 1u);
        return true;
    }
    int s = (int)*step + (dir > 0 ? 1 : -1);
    if (s > (int)SCOPE_ROLL_STEPS - 1) s = (int)SCOPE_ROLL_STEPS - 1;
    if (s == (int)*step)
        return false;                            /* already the slowest */
    *step = (uint8_t)s;
    return true;
}

scope_disp_mode_t scope_disp_mode_cycle(scope_disp_mode_t m, int dir)
{
    int n = (int)SCOPE_DISP_COUNT;
    int i = ((int)m % n + n) % n;
    i = (i + (dir >= 0 ? 1 : n - 1)) % n;
    return (scope_disp_mode_t)i;
}

const char *scope_disp_mode_label(scope_disp_mode_t m)
{
    switch (m) {
    case SCOPE_DISP_YT:   return "Y-T";
    case SCOPE_DISP_ROLL: return "Roll";
    case SCOPE_DISP_XY:   return "X-Y";
    default:              return "?";
    }
}

uint16_t scope_roll_age_at_column(uint16_t x, uint16_t width)
{
    if (width == 0u || x >= width)
        return 0xFFFFu;
    return (uint16_t)(((uint32_t)(width - 1u - x) * SCOPE_ROLL_POINTS_PER_DIV)
                      / SCOPE_ROLL_PX_PER_DIV);
}

uint16_t scope_roll_visible(const scope_roll_fifo_t *f, uint16_t width)
{
    if (f == NULL || width == 0u)
        return 0u;
    uint16_t n = (uint16_t)(scope_roll_age_at_column(0u, width) + 1u);
    return (f->count < n) ? f->count : n;
}

#define ROLL_FIT_MIN_SPAN 16

bool scope_roll_fit(const scope_roll_fifo_t *f, uint8_t ch, uint16_t n,
                    scope_roll_vmap_t *m)
{
    if (f == NULL || m == NULL || ch > 1u)
        return false;
    uint8_t lo = 255u, hi = 0u, v;
    uint16_t got = 0u;
    for (uint16_t age = 0u; age < n && scope_roll_fifo_get(f, ch, age, &v); age++) {
        if (v < lo) lo = v;
        if (v > hi) hi = v;
        got++;
    }
    if (got == 0u)
        return false;
    if ((int)hi - (int)lo < ROLL_FIT_MIN_SPAN) {
        int mid = ((int)hi + (int)lo) / 2;
        int a = mid - ROLL_FIT_MIN_SPAN / 2;
        if (a < 0) a = 0;
        if (a > 255 - ROLL_FIT_MIN_SPAN) a = 255 - ROLL_FIT_MIN_SPAN;
        lo = (uint8_t)a;
        hi = (uint8_t)(a + ROLL_FIT_MIN_SPAN);
    }
    m->fixed = false;
    m->lo = lo;
    m->hi = hi;
    return true;
}

int16_t scope_roll_y(const scope_roll_vmap_t *m, uint8_t code)
{
    if (m == NULL || m->y_bot <= m->y_top)
        return 0;
    int y;
    if (m->fixed) {
        int mid = ((int)m->y_top + (int)m->y_bot) / 2;
        y = mid - ((int)code - (int)m->center) * (int)m->scope_h / 256;
    } else {
        /* 2 px margin top and bottom, as autofit leaves the band edges free. */
        int top = (int)m->y_top + 2, bot = (int)m->y_bot - 3;
        if (bot <= top) { top = m->y_top; bot = m->y_bot - 1; }
        int span = (int)m->hi - (int)m->lo;
        if (span <= 0) span = 1;
        y = bot - ((int)code - (int)m->lo) * (bot - top) / span;
    }
    if (y < m->y_top)  y = m->y_top;
    if (y >= m->y_bot) y = m->y_bot - 1;
    return (int16_t)y;
}

float scope_roll_point_volts(uint8_t code, uint8_t zero, float volts_per_count)
{
    if (!(volts_per_count > 0.0f))
        return 0.0f;
    return ((float)code - (float)zero) * volts_per_count;
}
