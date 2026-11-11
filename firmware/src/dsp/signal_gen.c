/*
 * OpenScope 2C53T - signal generator state and apply (see signal_gen.h).
 *
 * Replaces the earlier 8-waveform DDS (256 samples, DAC1/PA4, bipolar
 * around 1.65 V) with stock V1.2.0's model: 13 waveforms, exactly 100
 * samples per cycle at every frequency, unipolar codes 20 + ..., DAC2/PA5.
 */

#include "signal_gen.h"
#ifndef EMULATOR_BUILD
#include "dac_output.h"
#endif

#include <string.h>

static siggen_config_t g_config = {
    SIGGEN_DEFAULT_WAVE, SIGGEN_DEFAULT_FREQ, SIGGEN_DEFAULT_AMP_DV,
    SIGGEN_DEFAULT_DUTY, false
};
static uint16_t g_cycle[SIGGEN_CYCLE_SAMPLES];
static bool g_cycle_built = false;
static siggen_timing_t g_timing;
static bool g_timing_valid = false;

static uint32_t timer_clock(void)
{
#ifndef EMULATOR_BUILD
    return dac_output_timer_clock_hz();
#else
    return 240000000u;
#endif
}

/* Stock 0x08029E14: rebuild the cycle into the live DMA buffer. Stock
 * builds in place while the DMA runs; building aside and copying keeps
 * the mixed-cycle window to one memcpy. */
static void rebuild(void)
{
    siggen_wave_build(g_cycle, g_config.waveform, g_config.amplitude_dv,
                      g_config.duty_pct);
    g_cycle_built = true;
#ifndef EMULATOR_BUILD
    memcpy(dac_output_get_buffer(), g_cycle, sizeof g_cycle);
#endif
}

/* Stock 0x08029D40: off when the enable is clear or the frequency is 0,
 * otherwise program TMR7 and run. */
static void apply(void)
{
    g_timing_valid = g_config.output_enabled &&
                     siggen_timing_compute(g_config.frequency_hz,
                                           timer_clock(), &g_timing);
#ifndef EMULATOR_BUILD
    if (g_timing_valid)
        dac_output_apply(&g_timing);
    else
        dac_output_stop();
#endif
}

void siggen_init(const siggen_config_t *cfg)
{
    if (cfg != NULL) {
        g_config = *cfg;
        if (g_config.waveform >= SIGGEN_WAVEFORM_COUNT) g_config.waveform = SIGGEN_SINE;
        if (g_config.frequency_hz > SIGGEN_FREQ_MAX_HZ) g_config.frequency_hz = SIGGEN_FREQ_MAX_HZ;
        if (g_config.amplitude_dv > SIGGEN_AMP_MAX_DV) g_config.amplitude_dv = SIGGEN_AMP_MAX_DV;
        if (g_config.duty_pct > SIGGEN_DUTY_MAX) g_config.duty_pct = SIGGEN_DUTY_MAX;
    } else {
        g_config.waveform = SIGGEN_DEFAULT_WAVE;
        g_config.frequency_hz = SIGGEN_DEFAULT_FREQ;
        g_config.amplitude_dv = SIGGEN_DEFAULT_AMP_DV;
        g_config.duty_pct = SIGGEN_DEFAULT_DUTY;
        g_config.output_enabled = false;
    }
    rebuild();
    apply();
}

void siggen_set_frequency(uint32_t freq_hz)
{
    if (freq_hz > SIGGEN_FREQ_MAX_HZ) freq_hz = SIGGEN_FREQ_MAX_HZ;
    g_config.frequency_hz = freq_hz;
    apply();            /* the cycle does not depend on frequency */
}

void siggen_set_waveform(uint8_t wave)
{
    if (wave >= SIGGEN_WAVEFORM_COUNT) return;
    g_config.waveform = wave;
    rebuild();
}

void siggen_set_amplitude_dv(uint8_t amp_dv)
{
    if (amp_dv > SIGGEN_AMP_MAX_DV) amp_dv = SIGGEN_AMP_MAX_DV;
    g_config.amplitude_dv = amp_dv;
    rebuild();
}

void siggen_set_duty(uint8_t pct)
{
    if (pct > SIGGEN_DUTY_MAX) pct = SIGGEN_DUTY_MAX;
    g_config.duty_pct = pct;
    rebuild();
}

void siggen_enable(bool enable)
{
    g_config.output_enabled = enable;
    apply();
}

siggen_waveform_t siggen_cycle_waveform(void)
{
    siggen_set_waveform((uint8_t)((g_config.waveform + 1u) % SIGGEN_WAVEFORM_COUNT));
    return (siggen_waveform_t)g_config.waveform;
}

uint8_t siggen_amplitude_up(void)
{
    if (g_config.amplitude_dv < SIGGEN_AMP_MAX_DV)
        siggen_set_amplitude_dv((uint8_t)(g_config.amplitude_dv + 1u));
    return g_config.amplitude_dv;
}

uint8_t siggen_amplitude_down(void)
{
    if (g_config.amplitude_dv > 0)
        siggen_set_amplitude_dv((uint8_t)(g_config.amplitude_dv - 1u));
    return g_config.amplitude_dv;
}

uint8_t siggen_duty_cycle_up(void)
{
    uint32_t d = g_config.duty_pct + 5u;
    siggen_set_duty((uint8_t)(d > SIGGEN_DUTY_MAX ? SIGGEN_DUTY_MAX : d));
    return g_config.duty_pct;
}

uint8_t siggen_duty_cycle_down(void)
{
    siggen_set_duty((uint8_t)(g_config.duty_pct > 5u ? g_config.duty_pct - 5u : 0u));
    return g_config.duty_pct;
}

const siggen_config_t *siggen_get_config(void)
{
    return &g_config;
}

const uint16_t *siggen_get_cycle(void)
{
    /* The emulator build never calls siggen_init(); build on first look
     * so the preview is not a flat line. */
    if (!g_cycle_built) rebuild();
    return g_cycle;
}

bool siggen_get_timing(siggen_timing_t *t)
{
    if (g_timing_valid && t != NULL) *t = g_timing;
    return g_timing_valid;
}
