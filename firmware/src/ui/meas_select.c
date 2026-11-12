/*
 * OpenScope 2C53T - per-channel measurement selection (see meas_select.h)
 */
#include "meas_select.h"
#include "scope_state.h"

static uint16_t g_ch2_mask = MEAS_SELECT_CH2_DEFAULT;

uint16_t meas_select_get(uint8_t ch)
{
    if (ch == 0u) return (uint16_t)(scope_state_get()->prm_mask & SCOPE_PRM_ALL_MASK);
    if (ch == 1u) return g_ch2_mask;
    return 0u;
}

void meas_select_set(uint8_t ch, uint16_t mask)
{
    mask = (uint16_t)(mask & SCOPE_PRM_ALL_MASK);
    if (ch == 0u)      scope_state_get()->prm_mask = mask;
    else if (ch == 1u) g_ch2_mask = mask;
}

void meas_select_toggle(uint8_t ch, uint8_t prm)
{
    if (prm >= SCOPE_PRM_COUNT) return;
    meas_select_set(ch, (uint16_t)(meas_select_get(ch) ^ (1u << prm)));
}

unsigned meas_select_count(uint8_t ch)
{
    unsigned n = 0;
    for (uint16_t m = meas_select_get(ch); m; m &= (uint16_t)(m - 1u)) n++;
    return n;
}

void meas_select_defaults(void)
{
    meas_select_set(0, MEAS_SELECT_CH1_DEFAULT);
    meas_select_set(1, MEAS_SELECT_CH2_DEFAULT);
}
