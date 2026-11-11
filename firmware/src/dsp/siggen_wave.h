/*
 * OpenScope 2C53T - signal generator: waveform, timer and DAC_CTRL math.
 *
 * Pure functions, no hardware access, so the host tests can pin them
 * against the stock V1.2.0 builder run in an emulator
 * (tests/fixtures/siggen_stock_waves.h). The hardware side lives in
 * drivers/dac_output.c, the state and UI glue in dsp/signal_gen.c.
 *
 * Stock model (V1.2.0, addresses are flash addresses):
 *   - one cycle = 100 DAC codes, rebuilt by 0x08029E14 on every edit;
 *   - DAC channel 2 (PA5), paced by TMR7 TRGO, fed by DMA2 channel 4;
 *   - parameters: waveform 0..12, frequency 0..50000 Hz (0 = off),
 *     amplitude 0..30 in 0.1 V units, duty 0..100 % (Square, Sawtooth).
 */

#ifndef SIGGEN_WAVE_H
#define SIGGEN_WAVE_H

#include <stdint.h>
#include <stdbool.h>

#define SIGGEN_CYCLE_SAMPLES  100u     /* stock DMA CNT, 0x0802CABC */
#define SIGGEN_TABLE_COUNT    8u       /* flash tables at 0x0804D848 */
#define SIGGEN_CODE_FLOOR     20u      /* stock `ip = 20`, 0x08029EA0 */
#define SIGGEN_AMP_MAX_DV     30u      /* clamp 0x0800A592: 3.0 V */
#define SIGGEN_DUTY_MAX       100u     /* clamp 0x0800A5D0 */
#define SIGGEN_FREQ_MAX_HZ    50000u   /* clamp 0x0800A45A */

/* Stock waveform index (ms+0xE59) and order of the tbh at 0x08029E2C.
 * The manual's names in brackets where ours differ. */
typedef enum {
    SIGGEN_SINE = 0,
    SIGGEN_SQUARE,          /* duty = high fraction */
    SIGGEN_SAWTOOTH,        /* duty = rise fraction; 50 % = triangle */
    SIGGEN_HALF_WAVE,       /* sits on a mid-scale (2047) floor */
    SIGGEN_FULL_WAVE,       /* sits on a mid-scale (2047) floor */
    SIGGEN_STEP,            /* 8-level staircase up */
    SIGGEN_REV_STEP,        /* 8-level staircase down */
    SIGGEN_EXP_RISE,        /* [Index Up] */
    SIGGEN_EXP_FALL,        /* [Index Decrease] */
    SIGGEN_DC,              /* [Direct Current]: level = amplitude */
    SIGGEN_MULTI_TONE,      /* [Multi-audio] */
    SIGGEN_SINC_PULSE,      /* [Sink Pulse] */
    SIGGEN_LORENTZ,
    SIGGEN_WAVEFORM_COUNT
} siggen_waveform_t;

/* Short display name, "?" for an out-of-range index. */
const char *siggen_wave_name(uint8_t wave);

/* Stock lets duty be edited only for Square and Sawtooth (0x0800A478,
 * `idx - 1 < 2`); the other builders never read it. */
bool siggen_wave_uses_duty(uint8_t wave);

/*
 * Build one 100-sample cycle exactly as stock 0x08029E14 does:
 *   table types:    20 + table[i] * amp / 33
 *   computed types: high level H = amp * 4095 / 33 (amp = 30 -> 3722)
 * Integer floor division throughout, as stock (the /33 is its
 * 0x3E0F83E1 multiply + lsr #3). amp is in 0.1 V units and duty in %;
 * both are clamped to the stock ranges. An unknown wave builds DC 20.
 */
void siggen_wave_build(uint16_t out[SIGGEN_CYCLE_SAMPLES],
                       uint8_t wave, uint8_t amp_dv, uint8_t duty_pct);

/*
 * TMR7 programming for 100 DAC updates per output cycle.
 *
 * update rate = clk / ((div + 1) * (pr + 1)) = 100 * f, so the integer
 * divider N = (div + 1) * (pr + 1) = round(clk / (100 f)).
 *
 * Stock (0x08029D40) always uses pr = 1 (pr = 19 below 20 Hz) and
 * truncates div; we pick div = 0 and pr = N - 1 while N fits the 16-bit
 * PR (f >= 37 Hz at 240 MHz), which halves the frequency step, and round
 * N to nearest. Below that, div grows to keep PR in range, so 1 Hz is
 * exact where stock overflows its 16-bit DIV and outputs ~2.2 Hz.
 *
 * Quantisation at clk = 240 MHz: N = 2.4e6 / f, so the output step is
 * about f^2 / 2.4e6 (1 kHz: 0.42 Hz, 10 kHz: 42 Hz, 50 kHz: 1 kHz, where
 * N = 48 and 49999 Hz comes out as 50000 Hz). Stock's step is twice that.
 *
 * Returns false (and leaves *t zeroed) for f = 0, which is off in stock
 * (0x08029D5C), or for clk = 0. pr >= 1 always: a basic timer with
 * PR = 0 does not count.
 */
typedef struct {
    uint16_t div;           /* TMR7 DIV (prescaler - 1) */
    uint16_t pr;            /* TMR7 PR  (period - 1)   */
    uint32_t actual_mhz;    /* output frequency in milli-hertz, rounded */
} siggen_timing_t;

bool siggen_timing_compute(uint32_t freq_hz, uint32_t tmr_clk_hz,
                           siggen_timing_t *t);

/* Stock's own formula (0x08029D64..0x08029DD2), kept for the tests and
 * the shell to compare against. div keeps only the 16 bits the
 * register holds, as the hardware does. */
bool siggen_timing_stock(uint32_t freq_hz, uint32_t tmr_clk_hz,
                         siggen_timing_t *t);

/*
 * DAC_CTRL composition. The lower half (bits 15:0) belongs to channel 1,
 * the scope's CH1 offset / trigger reference DAC on PA4
 * (scope_trigger.c), and is passed through untouched. The upper half is
 * replaced with stock's channel-2 setup (0x0802CA28..0x0802CAF6):
 *   D2TRGSEL = tsel, D2TRGEN = 1, D2NM = 0 (no noise/triangle),
 *   D2OBDIS = 0 (output buffer ON), D2DMAEN = 1, D2EN = en.
 */
#define SIGGEN_DAC_CH1_MASK     0x0000FFFFu
#define SIGGEN_DAC_D2EN         (1u << 16)
#define SIGGEN_DAC_D2OBDIS      (1u << 17)
#define SIGGEN_DAC_D2TRGEN      (1u << 18)
#define SIGGEN_DAC_D2TRGSEL_POS 19u
#define SIGGEN_DAC_D2DMAEN      (1u << 28)
#define SIGGEN_DAC_TSEL_TMR7    2u      /* DAC_TMR7_TRGOUT_EVENT */
#define SIGGEN_DAC_TSEL_SW      7u      /* DAC_SOFTWARE_TRIGGER  */

uint32_t siggen_dac_ctrl_ch2(uint32_t ctrl, uint8_t tsel, bool en);

#endif /* SIGGEN_WAVE_H */
