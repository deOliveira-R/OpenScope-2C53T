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
    19u, 39u, 79u, 199u, 399u, 799u, 1999u, 3999u, SCOPE_ROLL_ARR_SLOWEST,
};

uint16_t scope_roll_arr(uint8_t step)
{
    return (step < SCOPE_ROLL_STEPS) ? roll_arr[step] : 0u;
}

ms_t scope_roll_period_ms(uint8_t step)
{
    ms_t p = { 0u };
    if (step >= SCOPE_ROLL_STEPS)
        return p;
    p.ms = ((uint32_t)roll_arr[step] + 1u) * 1000u / SCOPE_ROLL_TICK_HZ;
    return p;
}

float scope_roll_seconds_per_div(uint8_t step)
{
    return (float)scope_roll_period_ms(step).ms * (float)SCOPE_ROLL_POINTS_PER_DIV / 1000.0f;
}

void scope_roll_label(uint8_t step, char *out, uint32_t n)
{
    if (out == NULL || n == 0u)
        return;
    uint32_t ms = scope_roll_period_ms(step).ms * SCOPE_ROLL_POINTS_PER_DIV;
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

void scope_roll_fifo_push(scope_roll_fifo_t *f, rec_code_t ch1, rec_code_t ch2)
{
    if (f == NULL)
        return;
    f->seq++;                                   /* odd: writing */
    f->pt[0][f->head] = ch1.v;
    f->pt[1][f->head] = ch2.v;
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

/* The points must be read between the two reads of seq: no compiler may move
 * them across either. */
#define ROLL_COMPILER_BARRIER()  __asm__ __volatile__("" ::: "memory")

bool scope_roll_fifo_columns(const scope_roll_fifo_t *src, uint8_t cols[2][SCOPE_ROLL_POINTS],
                             uint16_t *n_out, uint32_t *total_out)
{
    uint16_t n = 0u;
    uint32_t total = 0u;
    bool whole = false;
    for (int attempt = 0; src != NULL && cols != NULL && attempt < 4 && !whole; attempt++) {
        const uint32_t s0 = src->seq;
        if (s0 & 1u)
            continue;
        ROLL_COMPILER_BARRIER();
        const uint16_t width = SCOPE_ROLL_POINTS;
        const uint16_t shown = (uint16_t)(scope_roll_age_at_column(0u, width) + 1u);
        n = (src->count < shown) ? src->count : shown;
        total = src->total;
        for (uint16_t x = (uint16_t)(width - n); x < width; x++) {
            const uint16_t age = scope_roll_age_at_column(x, width);
            for (uint8_t ch = 0u; ch < 2u; ch++)
                (void)scope_roll_fifo_get(src, ch, age, &cols[ch][x]);
        }
        ROLL_COMPILER_BARRIER();
        whole = (src->seq == s0);
    }
    if (n_out) *n_out = n;
    if (total_out) *total_out = total;
    return whole;
}

rec_code_t scope_roll_record_code(adc_raw_t raw)
{
    return rec_from_raw(raw);
}

bool scope_roll_tb_step(uint8_t *yt_idx, bool *roll, uint8_t *step, int dir,
                        uint8_t yt_count, bool roll_offered)
{
    if (yt_idx == NULL || roll == NULL || step == NULL || yt_count == 0u || dir == 0)
        return false;
    if (!*roll) {
        if (dir > 0 && *yt_idx >= yt_count - 1u && roll_offered) {
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


uint16_t scope_roll_age_at_column(uint16_t x, uint16_t width)
{
    if (width == 0u || x >= width)
        return 0xFFFFu;
    return (uint16_t)(((uint32_t)(width - 1u - x) * SCOPE_ROLL_POINTS_PER_DIV)
                      / SCOPE_ROLL_PX_PER_DIV);
}

#define ROLL_FIT_MIN_SPAN 16

bool scope_roll_fit_codes(const uint8_t *codes, uint16_t n, uint8_t *lo_out, uint8_t *hi_out)
{
    if (codes == NULL || lo_out == NULL || hi_out == NULL || n == 0u)
        return false;
    uint8_t lo = 255u, hi = 0u;
    for (uint16_t i = 0u; i < n; i++) {
        if (codes[i] < lo) lo = codes[i];
        if (codes[i] > hi) hi = codes[i];
    }
    if ((int)hi - (int)lo < ROLL_FIT_MIN_SPAN) {
        int mid = ((int)hi + (int)lo) / 2;
        int a = mid - ROLL_FIT_MIN_SPAN / 2;
        if (a < 0) a = 0;
        if (a > 255 - ROLL_FIT_MIN_SPAN) a = 255 - ROLL_FIT_MIN_SPAN;
        lo = (uint8_t)a;
        hi = (uint8_t)(a + ROLL_FIT_MIN_SPAN);
    }
    *lo_out = lo;
    *hi_out = hi;
    return true;
}

volts_t scope_roll_point_volts(rec_code_t code, rec_code_t zero,
                               volts_per_count_t volts_per_count)
{
    volts_t v = { 0.0f };
    if (!(volts_per_count.vpc > 0.0f))
        return v;
    v.v = ((float)code.v - (float)zero.v) * volts_per_count.vpc;
    return v;
}

bool scope_roll_trig_hit(const scope_roll_fifo_t *f, uint8_t ch, int col_shift,
                         rec_code_t level, uint8_t edge)
{
    if (f == NULL || ch > 1u || f->count != SCOPE_ROLL_POINTS)
        return false;
    const int i = (int)SCOPE_ROLL_TRIG_IDX + col_shift;
    if (i < 0 || i + 1 >= (int)SCOPE_ROLL_POINTS)
        return false;
    /* oldest-first index i is age (count - 1 - i). */
    uint8_t a, b;
    if (!scope_roll_fifo_get(f, ch, (uint16_t)(SCOPE_ROLL_POINTS - 1u - (unsigned)i), &a) ||
        !scope_roll_fifo_get(f, ch, (uint16_t)(SCOPE_ROLL_POINTS - 2u - (unsigned)i), &b))
        return false;
    /* Stock's compare chain (0x0803EC64..0x0803ECA2): rising skips when
     * a > level and needs b > level; falling skips when a < level and needs
     * b < level. */
    if ((edge == SCOPE_ROLL_EDGE_RISING || edge == SCOPE_ROLL_EDGE_BOTH) &&
        a <= level.v && b > level.v)
        return true;
    if ((edge == SCOPE_ROLL_EDGE_FALLING || edge == SCOPE_ROLL_EDGE_BOTH) &&
        a >= level.v && b < level.v)
        return true;
    return false;
}

scope_roll_trig_act_t scope_roll_trig_decide(uint8_t mode, bool hit)
{
    if (!hit || mode == 0u)
        return SCOPE_ROLL_TRIG_NONE;
    return (mode == 2u) ? SCOPE_ROLL_TRIG_STOP : SCOPE_ROLL_TRIG_HIT;
}

scope_roll_trig_act_t scope_roll_trig_step(scope_roll_fifo_t *f, uint8_t mode,
                                           uint8_t ch, int col_shift,
                                           rec_code_t level, uint8_t edge)
{
    if (f == NULL || mode == 0u)          /* AUTO: stock skips the check (ms[0x17] == 1) */
        return SCOPE_ROLL_TRIG_NONE;
    const scope_roll_trig_act_t act =
        scope_roll_trig_decide(mode, scope_roll_trig_hit(f, ch, col_shift, level, edge));
    if (act == SCOPE_ROLL_TRIG_HIT) {
        /* Stock's word store at 0x08040000: width ms[0xDB6] = 0. */
        f->seq++;
        f->head = 0u;
        f->count = 0u;
        f->seq++;
    }
    return act;
}
