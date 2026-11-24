/* See scope_roll_meas.h. */
#include "scope_roll_meas.h"
#include <string.h>

bool scope_roll_measure(const uint8_t *codes, uint16_t n,
                        uint8_t step, volts_per_count_t volts_per_count, rec_code_t zero,
                        scope_params_t *out, scope_measure_t *raw)
{
    scope_measure_t m;
    if (out != NULL)
        memset(out, 0, sizeof(*out));
    const uint32_t period_ms = scope_roll_period_ms(step).ms;
    if (codes == NULL || out == NULL || period_ms == 0u || n < 2u)
        return false;
    scope_measure_record(codes, n, &m);
    if (raw != NULL)
        *raw = m;

    const float rate = 1000.0f / (float)period_ms;
    scope_prm_ctx_t ctx;
    ctx.sample_rate.hz = rate;
    ctx.volts_per_count = volts_per_count;
    ctx.zero_count = zero;
    ctx.record_whole = true;           /* MCU-paced points, oldest first */
    ctx.freq_hz = (m.level_valid && m.cycles_valid && m.period_valid && m.period_samples > 0.0f)
                      ? rate / m.period_samples : 0.0f;
    scope_params_compute(&m, &ctx, out);
    return m.valid;
}
