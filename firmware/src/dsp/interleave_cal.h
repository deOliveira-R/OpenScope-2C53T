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
 * The fit refuses instead of guessing: a point that is not flat (noise, a
 * signal on the input), a point at a rail, too few usable points, too small
 * a span of A, a gain outside the plausible window, or a poor fit all return
 * a status and leave the correction alone.
 */
#ifndef INTERLEAVE_CAL_H
#define INTERLEAVE_CAL_H

#include <stdint.h>
#include <stdbool.h>

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
} il_cal_status_t;

typedef struct {
    float gain, offset;       /* B = gain * A + offset */
    float resid_max, resid_rms; /* in B record units */
    float a_span;
    uint8_t used;
} il_cal_fit_t;

void il_cal_default_params(il_cal_params_t *p);

/* Fit. `out` is filled as far as computed even on a refusal (used, a_span);
 * gain/offset are valid only for IL_CAL_OK. */
il_cal_status_t il_cal_fit(const il_cal_point_t *pts, unsigned n,
                           const il_cal_params_t *prm, il_cal_fit_t *out);

/* The DAC codes of the sweep: base + 80, base + 400, mid, upper - 400, upper
 * (direction-aware: `upper` may be below `base`). Returns false when the
 * endpoints are closer than 1000 codes or a code leaves 0..4095. */
bool il_cal_dac_points(uint16_t base, uint16_t upper, uint16_t out[5]);

const char *il_cal_status_str(il_cal_status_t s);

#endif
