/*
 * OpenScope 2C53T - signal generator state.
 *
 * Holds stock's five parameters (ms+0xE58..0xE61 in V1.2.0) and pushes
 * every change to the hardware the way stock does: rebuild the 100-sample
 * cycle (0x08029E14), then re-apply the timer (0x08029D40). The math is in
 * siggen_wave.c, the registers in drivers/dac_output.c.
 *
 * Output runs in every mode; nothing outside this module stops it.
 */

#ifndef SIGNAL_GEN_H
#define SIGNAL_GEN_H

#include <stdint.h>
#include <stdbool.h>

#include "siggen_wave.h"

typedef struct {
    uint8_t  waveform;          /* siggen_waveform_t, 0..12 (E59) */
    uint32_t frequency_hz;      /* 0..50000, 0 = output off (E5C) */
    uint8_t  amplitude_dv;      /* 0..30, 0.1 V units (E61) */
    uint8_t  duty_pct;          /* 0..100, Square/Sawtooth only (E60) */
    bool     output_enabled;    /* (E58) */
    bool     start_on;          /* boot policy (ours): restore the enable at
                                 * power-on like stock, instead of OFF */
} siggen_config_t;

/* Stock factory values (0x080294A0..0x0802954C) except the enable, which
 * stock sets ON; the caller decides. */
#define SIGGEN_DEFAULT_WAVE    SIGGEN_SINE
#define SIGGEN_DEFAULT_FREQ    1000u
#define SIGGEN_DEFAULT_AMP_DV  30u
#define SIGGEN_DEFAULT_DUTY    50u

/* Load a config (clamped) and apply it. NULL = stock defaults, off. */
void siggen_init(const siggen_config_t *cfg);

void siggen_set_frequency(uint32_t freq_hz);   /* clamped to 50 kHz */
void siggen_set_waveform(uint8_t wave);        /* ignored if >= 13 */
void siggen_set_amplitude_dv(uint8_t amp_dv);  /* clamped to 30 */
void siggen_set_duty(uint8_t pct);             /* clamped to 100 */
void siggen_enable(bool enable);

siggen_waveform_t siggen_cycle_waveform(void);
uint8_t siggen_amplitude_up(void);     /* +0.1 V, returns new dV */
uint8_t siggen_amplitude_down(void);   /* -0.1 V */
uint8_t siggen_duty_cycle_up(void);    /* +5 %, returns new % */
uint8_t siggen_duty_cycle_down(void);  /* -5 % */

const siggen_config_t *siggen_get_config(void);

/* The cycle currently built (also what the DMA outputs). */
const uint16_t *siggen_get_cycle(void);

/* The timer setting of the last apply; false when output is off. */
bool siggen_get_timing(siggen_timing_t *t);

/* The timer setting the current frequency would produce, whether or not
 * the output is on, so the screen can show the real frequency while the
 * generator is stopped. False for 0 Hz. */
bool siggen_preview_timing(siggen_timing_t *t);

void siggen_set_start_on(bool on);

#endif
