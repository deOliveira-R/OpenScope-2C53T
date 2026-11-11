/* See scope_roll_meas.h. */
#include "scope_roll_meas.h"
#include <string.h>

bool scope_roll_measure(const scope_roll_fifo_t *lin, uint8_t ch, uint16_t n,
                        uint8_t step, float volts_per_count, uint8_t zero,
                        scope_params_t *out, scope_measure_t *raw)
{
    scope_measure_t m;
    if (out != NULL)
        memset(out, 0, sizeof(*out));
    const uint32_t period_ms = scope_roll_period_ms(step);
    if (lin == NULL || out == NULL || ch > 1u || period_ms == 0u || lin->count < 2u)
        return false;
    if (n > lin->count || n == 0u)
        n = lin->count;
    /* Linearised: oldest at 0, newest at count - 1. */
    scope_measure_record(&lin->pt[ch][lin->count - n], n, &m);
    if (raw != NULL)
        *raw = m;

    const float rate = 1000.0f / (float)period_ms;
    scope_prm_ctx_t ctx;
    ctx.sample_rate = rate;
    ctx.volts_per_count = volts_per_count;
    ctx.zero_count = zero;
    ctx.freq_hz = (m.level_valid && m.cycles_valid && m.period_valid && m.period_samples > 0.0f)
                      ? rate / m.period_samples : 0.0f;
    scope_params_compute(&m, &ctx, out);
    return m.valid;
}
