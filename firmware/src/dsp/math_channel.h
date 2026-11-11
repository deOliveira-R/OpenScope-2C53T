/*
 * Math channel (parity C12) -- stock's eight operations on the REAL records.
 *
 * Stock's Mathematics menu has CH1+CH2, CH1-CH2, CH1*CH2, CH1/CH2, -CH1,
 * -CH2, |CH1|, |CH2| (label table 0x08015714..0x08015730, V1.2.0 in this
 * order) plus FFT, which has its own view here. Until C12 the overlay ran a
 * five-op int16 routine on a synthetic sine and square.
 *
 * Everything is computed in the instrument's honest vertical unit:
 *   - calibrated channel(s): VOLTS, from (record - zero) * volts_per_count,
 *     where `zero` is the record code of 0 V (scope_measure_zero_for(): 100 +
 *     pos with the stock calibration page, 128 without) and volts_per_count
 *     is scope_cal's number for the channel's range;
 *   - an uncalibrated range (k <= 0): COUNTS about the zero code. Never mixed:
 *     a two-channel op is in volts only when BOTH channels have a gain.
 * The math trace is drawn with its own scale (a 1-2-5 step per division picked
 * from the data), because A*B and A/B do not share a graticule with A and B.
 *
 * Pure functions, no hardware: tests/test_math_channel.c.
 */
#ifndef MATH_CHANNEL_H
#define MATH_CHANNEL_H
#include <stdint.h>

/* Stock's order. Persisted as config.math_op (clamped on load). */
typedef enum {
    MATH_ADD = 0,      /* CH1 + CH2 */
    MATH_SUB,          /* CH1 - CH2 */
    MATH_MUL,          /* CH1 * CH2 */
    MATH_DIV,          /* CH1 / CH2 */
    MATH_INV_A,        /* -CH1 */
    MATH_INV_B,        /* -CH2 */
    MATH_ABS_A,        /* |CH1| */
    MATH_ABS_B,        /* |CH2| */
    MATH_COUNT
} math_op_t;

typedef enum {
    MATH_UNIT_COUNTS = 0,
    MATH_UNIT_COUNTS2,   /* counts * counts */
    MATH_UNIT_VOLTS,
    MATH_UNIT_VOLTS2,
    MATH_UNIT_RATIO      /* dimensionless (A / B) */
} math_unit_t;

typedef struct {
    uint8_t zero_a, zero_b;    /* record code of 0 V, per channel */
    float   k_a, k_b;          /* volts per count; <= 0 = not calibrated */
} math_in_t;

/* Marker for "undefined / saturated" outputs (division by ~0): finite so the
 * code stays correct under -ffast-math, huge so it clips to the band edge. */
#define MATH_SAT  1.0e30f

/* A divisor within this many counts of its zero is "about 0": the 8-bit ADC
 * has ~1-2 counts of noise, so the quotient there is noise over noise. */
#define MATH_DIV_DEAD_COUNTS  2

int  math_op_uses_a(math_op_t op);
int  math_op_uses_b(math_op_t op);

/* Unit the result of `op` is in for these inputs (volts only when every
 * channel the op reads has a gain). */
math_unit_t math_unit_for(math_op_t op, const math_in_t *in);

/* out[i] for i < n. Channels the op does not read may be NULL.
 * Division: |B - zero| < MATH_DIV_DEAD_COUNTS gives +-MATH_SAT (sign of A,
 * 0 when A is exactly at its zero). Returns the unit. */
math_unit_t math_compute(math_op_t op, const uint8_t *a, const uint8_t *b,
                         uint16_t n, const math_in_t *in, float *out);

/* Largest |v| over the finite (non-MATH_SAT) samples; 0 when there are none. */
float math_peak(const float *v, uint16_t n);

/* 1-2-5 per-division ladder: MATH_SCALE_COUNT entries from 0.001 to 50000. */
#define MATH_SCALE_COUNT  24
float math_scale_value(int idx);

/* Pick the ladder index so `peak` fits in +-MATH_HALF_DIVS divisions.
 * prev < 0 means none; otherwise the previous choice is kept while it still
 * fits and the next lower step would not (hysteresis: a peak that sits on a
 * step boundary must not flip the scale every frame). */
#define MATH_HALF_DIVS_X10  35      /* 3.5 divisions: the pick limit */
#define MATH_KEEP_DIVS_X10  40      /* 4.0: the previous step is kept up to here */
int math_scale_pick(float peak, int prev);

/* Finest step the result may be drawn at: one division of the channel(s) the
 * op reads, carried through the op (min of the two for A+B / A-B, the product
 * for A*B; none for A/B, a ratio). Without it a quiet result (A-B of the same
 * signal, |A| of a grounded input) is autoscaled until 1-2 counts of ADC noise
 * fill the screen. `counts_per_div` is the channel graticule in record codes
 * (SCOPE_CAL_COUNTS_PER_DIV); the gains follow math_compute()'s volts/counts
 * rule. Returns 0 when there is no floor. */
float math_step_floor(math_op_t op, const math_in_t *in, float counts_per_div);

/* Smallest ladder index whose step is >= floor_step (0 for no floor). */
int math_scale_floor_idx(float floor_step);

/* "500mV/d", "2V/d", "0.5V2/d", "20c/d", "0.2/d" ... */
void math_scale_label(int idx, math_unit_t unit, char *out, uint32_t n);

/* Screen rows for the trace: y_mid - v/step*px_per_div, rounded, clipped to
 * [y_top, y_bot-1]; MATH_SAT clips to the edge of its sign. */
void math_to_rows(const float *v, uint16_t n, float step, int px_per_div,
                  int y_mid, int y_top, int y_bot, int16_t *rows);

const char *math_channel_name(math_op_t op);   /* "A+B" ... "|B|" */

#endif
