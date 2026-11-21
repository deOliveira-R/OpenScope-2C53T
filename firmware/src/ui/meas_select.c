/*
 * OpenScope 2C53T - measurement selection (see meas_select.h)
 */
#include "meas_select.h"
#include "scope_state.h"

static uint16_t g_ch2_mask = MEAS_SELECT_CH2_DEFAULT;
static uint16_t g_fft_mask = MEAS_SELECT_FFT_DEFAULT;

unsigned meas_select_size(uint8_t set)
{
    if (set == MEAS_SET_CH1 || set == MEAS_SET_CH2) return SCOPE_PRM_COUNT;
    if (set == MEAS_SET_FFT)                        return MEAS_SELECT_FFT_COUNT;
    return 0u;
}

/* The spectrum's measurements, in fft_meas_id_t order (scripts/model_config.py
 * reads this table for fft.meas). */
static const char *const fft_meas_labels[MEAS_SELECT_FFT_COUNT] = { "Peak", "Level", "H2", "H3" };

const char *meas_select_label(uint8_t set, unsigned id)
{
    if (id >= meas_select_size(set)) return "?";
    return (set == MEAS_SET_FFT) ? fft_meas_labels[id] : scope_prm_label(id);
}

uint16_t meas_select_all(uint8_t set)
{
    return (uint16_t)((1u << meas_select_size(set)) - 1u);
}

uint16_t meas_select_get(uint8_t set)
{
    switch (set) {
    case MEAS_SET_CH1: return (uint16_t)(scope_state_get()->prm_mask & meas_select_all(set));
    case MEAS_SET_CH2: return g_ch2_mask;
    case MEAS_SET_FFT: return g_fft_mask;
    default:           return 0u;
    }
}

void meas_select_set(uint8_t set, uint16_t mask)
{
    mask = (uint16_t)(mask & meas_select_all(set));
    switch (set) {
    case MEAS_SET_CH1: scope_state_get()->prm_mask = mask; break;
    case MEAS_SET_CH2: g_ch2_mask = mask; break;
    case MEAS_SET_FFT: g_fft_mask = mask; break;
    default: break;
    }
}

void meas_select_toggle(uint8_t set, uint8_t id)
{
    if (id >= meas_select_size(set)) return;
    meas_select_set(set, (uint16_t)(meas_select_get(set) ^ (1u << id)));
}

unsigned meas_select_count(uint8_t set)
{
    unsigned n = 0;
    for (uint16_t m = meas_select_get(set); m; m &= (uint16_t)(m - 1u)) n++;
    return n;
}

void meas_select_defaults(void)
{
    meas_select_set(MEAS_SET_CH1, MEAS_SELECT_CH1_DEFAULT);
    meas_select_set(MEAS_SET_CH2, MEAS_SELECT_CH2_DEFAULT);
    meas_select_set(MEAS_SET_FFT, MEAS_SELECT_FFT_DEFAULT);
}
