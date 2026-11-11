/*
 * interleave_cal — self-calibration of converter B against converter A for
 * the single-channel 250 MS/s record (parity item C2). Pure functions; the
 * device half (DAC sweep, collecting op04/op05 means) is in fpga.c and the
 * `fpga interleave cal` shell command.
 *
 * Method (bench B4, 2026-11-10): with CH2 off both converters sample CH1, so
 * a DC level that is moved with the CH1 offset DAC reads as (A, B) pairs on
 * one line, B = gain * A + offset, in record units (the units the merge
 * works in, interleave.h). A least-squares fit through several levels gives
 * the pair for fpga_interleave_corr_set(). Nothing unit-specific is compiled
 * in: the numbers come from the unit being calibrated, at run time.
 *
 * The line is the plausibility gate. What is applied is the piecewise map
 * through the levels themselves (knots, interleave.h): unit #2's B is not a
 * line of A (bench 2026-11-11: the line left -1.02 / +0.09 / +0.94 count
 * residuals), and a map through the measured levels leaves none at them.
 *
 * The fit refuses instead of guessing: a point that is not flat (noise, a
 * signal on the input), a point at a rail, too few usable points, too small
 * a span of A, a gain outside the plausible window, or a poor fit all return
 * a status and leave the correction alone.
 */
#ifndef INTERLEAVE_CAL_H
#define INTERLEAVE_CAL_H

#include <stdint.h>
#include <stdbool.h>

#include "interleave.h"   /* IL_KNOTS_MAX */

#define IL_CAL_MAX_POINTS  8u

/* One DAC level: the means of the 512-sample window of both converters,
 * averaged over several records, and how much they moved. */
typedef struct {
    float   a_mean, b_mean;   /* record units */
    uint8_t a_p2p, b_p2p;     /* largest peak-to-peak inside one record */
    float   a_drift, b_drift; /* spread of the per-record means */
    bool    clipped;          /* a sample hit 0 or 255 in either converter */
} il_cal_point_t;

typedef struct {
    uint8_t max_p2p;          /* flat: peak-to-peak per record at most this   */
    float   max_drift;        /* flat: record means within this               */
    uint8_t min_points;       /* usable points required                       */
    float   min_span;         /* A must cover at least this many codes        */
    float   gain_lo, gain_hi; /* plausible B/A slope                          */
    float   max_resid;       /* worst residual allowed, record units         */
} il_cal_params_t;

typedef enum {
    IL_CAL_OK = 0,
    IL_CAL_ARGS,          /* NULL or too many points */
    IL_CAL_NOT_FLAT,      /* a point failed the flatness test: input not quiet */
    IL_CAL_FEW_POINTS,    /* fewer than min_points usable (clipped points dropped) */
    IL_CAL_SPAN,          /* A spread too small to fit a slope */
    IL_CAL_GAIN_RANGE,    /* slope outside the plausible window */
    IL_CAL_POOR_FIT,      /* worst residual above max_resid */
    IL_CAL_NOT_MONOTONIC, /* the levels do not make a usable piecewise map */
} il_cal_status_t;

typedef struct {
    float gain, offset;       /* B = gain * A + offset */
    float resid_max, resid_rms; /* in B record units */
    float a_span;
    uint8_t used;
    /* Piecewise map through the usable levels, ascending in A, 1/100 record
     * unit (interleave_corr_set_knots). n_knots is 0 when more levels were
     * usable than IL_KNOTS_MAX (then the line alone is applied). */
    uint8_t n_knots;
    int16_t knot_a[IL_KNOTS_MAX], knot_b[IL_KNOTS_MAX];
} il_cal_fit_t;

void il_cal_default_params(il_cal_params_t *p);

/* Fit. `out` is filled as far as computed even on a refusal (used, a_span);
 * gain/offset and the knots are valid only for IL_CAL_OK. */
il_cal_status_t il_cal_fit(const il_cal_point_t *pts, unsigned n,
                           const il_cal_params_t *prm, il_cal_fit_t *out);

/* The DAC codes of the sweep: base + 80, base + 400, mid, upper - 400, upper
 * (direction-aware: `upper` may be below `base`). Returns false when the
 * endpoints are closer than 1000 codes or a code leaves 0..4095. */
bool il_cal_dac_points(uint16_t base, uint16_t upper, uint16_t out[5]);

const char *il_cal_status_str(il_cal_status_t s);

/* ── Timing skew (`fpga interleave skew measure`) ─────────────────────
 * From one merged record (even = A, odd = B; INTERLEAVE_N samples) of a tone
 * on CH1: the frequency from A's spectral peak (integer-bin scan, then a
 * golden-section refinement of |DFT|), then a least-squares sine (DC, cos,
 * sin) at that frequency fitted to A and to B on the same index grid. B is
 * the same tone seen tau converter periods later, so tau = (phase_A -
 * phase_B) / w. A frequency error moves both phases alike and cancels in the
 * difference. Works on a square wave too (the fit takes the fundamental; the
 * G431's 10 MHz has no even harmonics and its odd ones fall elsewhere).
 * With a skew correction already applied the record reads the RESIDUAL (tau
 * ~ 0.5), which is how `measure` refines the correction in place. */
typedef struct {
    float tau;            /* B after A, converter periods (0.5 = nominal) */
    float cycles;         /* tone, cycles per converter sample (x 125 MHz) */
    float amp_a, amp_b;   /* fitted fundamental amplitudes, record units */
    float resid_a;        /* rms residual of A's fit / amp_a */
} il_skew_est_t;

typedef enum {
    IL_SKEW_OK = 0,
    IL_SKEW_ARGS,
    IL_SKEW_LOW_AMP,      /* fundamental below 8 counts: no usable tone */
    IL_SKEW_FREQ,         /* peak outside ~5..50 MHz */
    IL_SKEW_MISMATCH,     /* A and B disagree on amplitude: not the same signal */
    IL_SKEW_NOISY,        /* A is far from a periodic signal at that tone */
    IL_SKEW_RANGE,        /* |tau - 0.5| beyond the correction's range */
} il_skew_status_t;

il_skew_status_t il_skew_estimate(const uint8_t *merged, il_skew_est_t *out);
const char *il_skew_status_str(il_skew_status_t s);

#endif
