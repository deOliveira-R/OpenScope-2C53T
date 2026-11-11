/*
 * OpenScope 2C53T — the 12 measurements over the roll FIFO (parity C11/C6)
 *
 * Stock's parameter set is computed from the record on screen. In roll the
 * record on screen is the FIFO: 301 points, one every scope_roll_period_ms()
 * milliseconds, so the TIME BASE of every horizontal quantity is the roll
 * point rate (1000 / period_ms points per second), not a timebase-table rate.
 *
 * Differences from the Y-T badges, each for a reason:
 *  - Freq/Peri: Y-T takes them from a spectral estimate because its records
 *    are torn and edge counting measured 5-50 % out (EXP-13/16). A roll FIFO is
 *    one coherent stream (no torn records, MCU-timed points), so the crossing
 *    period is used here: Freq = rate / period_samples, from at least two
 *    rising mid-level crossings, and refused (--) otherwise. A signal above
 *    half the point rate aliases, exactly as on stock; the slowest rolls
 *    (1.25 Hz at 800 ms) cannot see anything faster than 0.6 Hz.
 *  - The window is the points on the glass (the newest `n`), so the numbers
 *    describe what is drawn.
 *  - Vertical: zero and volts-per-count are the caller's, the same per-channel
 *    inputs as the Y-T badges (scope_measure_zero_for(), scope_cal).
 */
#ifndef SCOPE_ROLL_MEAS_H
#define SCOPE_ROLL_MEAS_H

#include "scope_roll.h"
#include "scope_measure.h"

/* Measure channel `ch` (0/1) of a LINEARISED FIFO (scope_roll_fifo_linearize)
 * over its newest `n` points (clamped to the count). `step` is the roll step
 * (its period gives the sample rate), `volts_per_count` <= 0 keeps counts,
 * `zero` is the channel's 0 V record code. Returns false, all refused, when
 * the FIFO holds fewer than 2 points or the step is out of range. */
bool scope_roll_measure(const scope_roll_fifo_t *lin, uint8_t ch, uint16_t n,
                        uint8_t step, float volts_per_count, uint8_t zero,
                        scope_params_t *out, scope_measure_t *raw);

#endif
