/*
 * OpenScope 2C53T — horizontal (time) calibration
 *
 * The rates are the capture core's (see below). Free of hardware and RTOS
 * dependencies so it can be exercised on the host — see
 * tests/test_scope_timebase.c.
 */

#include "scope_timebase.h"
#include "../drivers/fpga_core_regs.h"   /* generated from the capture core's package */

#include <stdio.h>

/* The sample rate of every timebase code: the capture core's clock over the
 * integer its package states (drivers/fpga_core_regs.h, generated from
 * fpga/core/rtl/core_pkg.sv), and the converters' own 50 MHz clock at the
 * 50 MS/s code. Nothing here is measured. What the numbers rest on is the
 * unit's 25 MHz crystal: its error moves every rate by the same factor. */
_Static_assert(SCOPE_TIMEBASE_CODE_COUNT == FPGA_CORE_TB_CODES, "timebase code count differs from the core's");
static const uint32_t core_divider[SCOPE_TIMEBASE_CODE_COUNT] = FPGA_CORE_TB_DIVIDERS;

float scope_timebase_sample_rate(uint8_t code)
{
    if (code >= SCOPE_TIMEBASE_CODE_COUNT)
        return 0.0f;
    if (code == FPGA_CORE_TB_50MS)
        return (float)FPGA_CORE_RATE50_HZ;
    return (float)FPGA_CORE_CLK_HZ / (float)core_divider[code];
}

const float *scope_timebase_rate_table(void)
{
    /* Filled on first use; every writer stores the same values. */
    static float table[SCOPE_TIMEBASE_CODE_COUNT];
    if (table[0] == 0.0f)
        for (unsigned c = SCOPE_TIMEBASE_CODE_COUNT; c-- > 0u; )
            table[c] = scope_timebase_sample_rate((uint8_t)c);
    return table;
}

scope_tb_tier_t scope_timebase_get_tier(uint8_t code)
{
    return (code < SCOPE_TIMEBASE_CODE_COUNT) ? SCOPE_TB_MEASURED : SCOPE_TB_NONE;
}

float scope_timebase_seconds_per_sample(uint8_t code)
{
    const float fs = scope_timebase_sample_rate(code);
    return (fs > 0.0f) ? (1.0f / fs) : 0.0f;
}

float scope_timebase_seconds_per_div(uint8_t code)
{
    return scope_timebase_seconds_per_sample(code) *
           SCOPE_TIMEBASE_SAMPLES_PER_DIV;
}

float scope_timebase_hz_from_period(uint8_t code, float period_samples)
{
    const float fs = scope_timebase_sample_rate(code);

    if (fs <= 0.0f || period_samples <= 0.0f)
        return 0.0f;

    return fs / period_samples;
}

uint8_t scope_timebase_interp_factor(uint8_t code, bool interleaved)
{
    /* Stock's display ratio is 1000 x (div_ns / 25) / record_ns, with
     * div_ns = 10/20/50/100 and record_ns = 4 (merged) or 8 (0x08030A70);
     * these rows are 1000 / ratio. */
    static const uint8_t merged[4]  = { 10u, 5u, 2u, 1u };
    static const uint8_t per_adc[4] = { SCOPE_TIMEBASE_INTERP_MAX, 10u, 4u, 2u };
    if (code >= 4u)
        return 1u;
    return interleaved ? merged[code] : per_adc[code];
}

_Static_assert(SCOPE_TIMEBASE_TICK_HZ % FPGA_CORE_RATE50_HZ == 0u, "the 50 MS/s sample must be whole ticks");

uint32_t scope_timebase_sample_ticks(uint8_t code, bool interleaved)
{
    if (code >= SCOPE_TIMEBASE_CODE_COUNT)
        return 0u;
    const uint32_t own = (code == FPGA_CORE_TB_50MS)
                       ? SCOPE_TIMEBASE_TICK_HZ / FPGA_CORE_RATE50_HZ
                       : core_divider[code] * SCOPE_TIMEBASE_INTERP_MAX;
    return interleaved ? own / 2u : own;
}

uint32_t scope_timebase_column_ticks(uint8_t code, bool interleaved)
{
    return scope_timebase_sample_ticks(code, interleaved) / scope_timebase_interp_factor(code, interleaved);
}

void scope_timebase_view_label(uint8_t code, bool interleaved, char *out, uint32_t n)
{
    const uint8_t f = (uint8_t)((interleaved ? 2u : 1u) *
                                scope_timebase_interp_factor(code, interleaved));
    scope_timebase_label_scaled(code, f, out, n);
}

void scope_timebase_label(uint8_t code, char *out, uint32_t n)
{
    scope_timebase_label_scaled(code, 1u, out, n);
}

void scope_timebase_label_scaled(uint8_t code, uint8_t rate_factor, char *out, uint32_t n)
{
    if (out == NULL || n == 0u)
        return;

    const float s = scope_timebase_seconds_per_div(code) /
                    (float)(rate_factor ? rate_factor : 1u);

    if (s <= 0.0f) {
        /* No rate. The grid has no time meaning on this code, and saying so is
         * the point — a nominal label here would be the defect this module
         * exists to remove. */
        snprintf(out, n, "--");
        return;
    }

    const char *mark =
        (scope_timebase_get_tier(code) == SCOPE_TB_PROVISIONAL) ? "~" : "";

    /* Integer formatting throughout: the shell and the LCD both build against
     * nano.specs, which has no float printf. The unit is chosen on the value
     * rounded to whole nanoseconds: the rates are exact, so s/div lands on
     * round figures, and 1 ms held in a float is 0.99999997 ms ("1000us"). */
    const uint32_t ns = (s < 4.0f) ? (uint32_t)(s * 1e9f + 0.5f) : 0xFFFFFFFFu;
    if (ns < 1000u) {
        snprintf(out, n, "%s%uns", mark, (unsigned)ns);
    } else if (ns < 10000u) {
        const unsigned tenths_us = (unsigned)((ns + 50u) / 100u);
        snprintf(out, n, "%s%u.%uus", mark, tenths_us / 10u, tenths_us % 10u);
    } else if (ns < 1000000u) {
        snprintf(out, n, "%s%uus", mark, (unsigned)((ns + 500u) / 1000u));
    } else if (ns < 1000000000u) {
        const unsigned hundredths_ms = (unsigned)((ns + 5000u) / 10000u);
        snprintf(out, n, "%s%u.%02ums", mark,
                 hundredths_ms / 100u, hundredths_ms % 100u);
    } else {
        const unsigned hundredths_s = (unsigned)(s * 100.0f + 0.5f);
        snprintf(out, n, "%s%u.%02us", mark,
                 hundredths_s / 100u, hundredths_s % 100u);
    }
}
