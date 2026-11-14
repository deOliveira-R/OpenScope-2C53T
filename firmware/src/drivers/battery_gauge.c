/* OpenScope 2C53T - battery gauge logic (pure). See battery_gauge.h. */
#include "battery_gauge.h"

uint8_t batt_gauge_percent(uint16_t mv)
{
    if (mv >= 4120u) return 100u;
    if (mv >= 4000u) return (uint8_t)(80u + (mv - 4000u) * 20u / 120u);
    if (mv >= 3900u) return (uint8_t)(60u + (mv - 3900u) * 20u / 100u);
    if (mv >= 3800u) return (uint8_t)(40u + (mv - 3800u) * 20u / 100u);
    if (mv >= 3700u) return (uint8_t)(20u + (mv - 3700u) * 20u / 100u);
    if (mv >= 3600u) return (uint8_t)(10u + (mv - 3600u) * 10u / 100u);
    if (mv >= 3300u) return (uint8_t)((mv - 3300u) * 10u / 300u);
    return 0u;
}

void batt_gauge_step(batt_gauge_t *g, uint16_t raw_mv, bool ext_now)
{
    g->raw_mv = raw_mv;

    /* The symbol follows PC7 after a short debounce; the very first reading
     * takes PC7 as it is. */
    if (g->avg_count == 0u) {
        g->external = ext_now;
        g->ext_count = 0u;
    } else if (ext_now != g->external) {
        if (++g->ext_count >= BATT_EXT_DEBOUNCE) {
            g->external = ext_now;
            g->ext_count = 0u;
        }
    } else {
        g->ext_count = 0u;
    }

    /* The voltage step arrives with the cable, so it is removed by the pin
     * as read this second, not by the debounced state. */
    const uint16_t est = (ext_now && raw_mv > BATT_EXT_STEP_MV)
                       ? (uint16_t)(raw_mv - BATT_EXT_STEP_MV) : raw_mv;
    g->avg_buf[g->avg_idx] = est;
    g->avg_idx = (uint8_t)((g->avg_idx + 1u) % BATT_AVG_SIZE);
    if (g->avg_count < BATT_AVG_SIZE) g->avg_count++;

    uint32_t sum = 0u;
    for (uint8_t i = 0; i < g->avg_count; i++) sum += g->avg_buf[i];
    g->rest_mv = (uint16_t)(sum / g->avg_count);
    g->percent = batt_gauge_percent(g->rest_mv);

    /* Critical: only on battery, 10 readings in a row. */
    if (raw_mv < BATT_CRITICAL_MV && !ext_now) {
        if (g->critical_count < BATT_CRITICAL_COUNT) g->critical_count++;
        if (g->critical_count >= BATT_CRITICAL_COUNT) g->critical = true;
    } else {
        g->critical_count = 0u;
        g->critical = false;
    }
}
