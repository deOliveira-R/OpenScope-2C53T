/*
 * interleave — the single-channel 250 MS/s record from the two converters.
 *
 * With channel mask 1 (PC2 H, PC1 H) both converters sample CH1. Converter B
 * (op05) samples about half a converter period after A (op04): bench
 * 2026-11-10 on unit #2, 0.45 of a 125 MS/s sample. So
 *
 *     merged[2k] = A[k],  merged[2k + 1] = B[k]
 *
 * is one record at twice the rate. Stock does the same for mask != 3 and
 * timebase <= 3, with even = A and odd = B for every mask (decoded at
 * 0x08025970-0x080259D4).
 *
 * The record keeps the 1024-sample size every consumer assumes: the merge
 * takes A[256..767] and B[256..767], the centre half of each converter's
 * record. After un-rotation the trigger is at A[512], which lands at merged
 * index 2 * (512 - 256) = 512 — the same place as in a normal record. The cost
 * is the time span: 4.1 us at 250 MS/s instead of 8.2 us.
 *
 * CONVERTER B CORRECTION (parity ilplus), applied to B before the merge, in
 * this order:
 *
 * 1. Level map, B into A's units. Either the line B' = (B - offset) / gain
 *    (`fpga interleave corr`, a v1 user-cal record), or — when knots are set
 *    — the piecewise-linear inverse through the self-cal's levels (`fpga
 *    interleave cal`): knot i says "B reads knot_b[i] where A reads
 *    knot_a[i]". Unit #2's B is not a line of A (the linear fit left -1.02 /
 *    +0.09 / +0.94 count odd-even residuals at three of five levels, bench
 *    2026-11-11), so the line leaves a level-dependent odd-even step that the
 *    knots remove. End segments extrapolate; results round and clamp 0..255.
 *
 * 2. Timing skew. B samples 0.451 of a converter period after A, not 0.5
 *    (bench B4 on unit #2: 0.450; stock on the same unit: 0.451, runs
 *    2026-11-11). Uncorrected, that is the image spur at fs/2 - f, about
 *    w * delta / 2: -38 dBc at 10 MHz, stock's -37.9. In the merged record
 *    odd sample n is x(n + delta) with delta = 2 * (tau - 0.5) merged
 *    samples. The correction replaces it with a 17-tap time-varying FIR over
 *    its MERGED neighbourhood — B samples at j + delta, A samples at j, j =
 *    -8..8 — designed by least squares to reproduce x(n) for every input up
 *    to 0.8 pi of the merged rate, i.e. 100 MHz (standard two-channel TIADC
 *    skew compensation, Johansson & Lowenborg 2002; scripts/
 *    gen_interleave_skew.py derives it). Because it uses both converters at
 *    the full 250 MS/s rate it is valid above 62.5 MHz, where a fractional
 *    delay on B's own 125 MS/s samples would alias. The front end's
 *    bandwidth (50 MHz) sits well inside the design band; above 100 MHz the
 *    correction degrades and above 125 MHz the merged record aliases anyway.
 *    The taps for the unit's delta come from a Farrow table in flash
 *    (interleave_skew_taps.h, generic, nothing unit specific), are
 *    renormalised to unit DC gain so a level stays where step 1 put it, and
 *    delta = 0 is exactly the identity.
 *
 *    No record-edge case: the merge uses B[256..767], and the taps of B[k]
 *    read B[k-4..k+4] and A[k-3..k+4], all inside [252, 771] of the 1024.
 *
 * The identity (no knots, gain 1, offset 0, skew 0) leaves B untouched. All
 * numbers are the unit's own, set at run time, never compiled in.
 */
#ifndef INTERLEAVE_H
#define INTERLEAVE_H

#include <stdint.h>
#include <stdbool.h>

#define INTERLEAVE_N        1024u   /* record length in and out */
#define INTERLEAVE_WIN_LO    256u   /* first converter sample used */

#define IL_KNOTS_MAX           5u     /* the self-cal's five DAC levels */
#define IL_KNOT_SCALE        100      /* knots in 1/100 record unit */
#define IL_KNOT_MAX_C      25500      /* 255.00 */
#define IL_SKEW_TAPS_N        17u     /* = IL_SKEW_TAPS (interleave_skew_taps.h) */
#define IL_SKEW_PPM_MAX   150000      /* |tau - 0.5| <= 0.15 period: the Farrow fit range */

typedef struct {
    float gain;     /* B / A slope; 1.0 = no correction (used without knots) */
    float offset;   /* B - gain * A at A = 0, record units */
    /* Piecewise map: n_knots >= 2 overrides gain/offset. Strictly ascending
     * in both, 1/100 record unit. */
    uint8_t n_knots;
    int16_t knot_a[IL_KNOTS_MAX];
    int16_t knot_b[IL_KNOTS_MAX];
    /* B's deviation from half a converter period, ppm of a period
     * (unit #2: 0.451 -> -49000). 0 = no skew correction. */
    int32_t skew_ppm;
    float   skew_taps[IL_SKEW_TAPS_N];   /* from interleave_skew_set() */
} interleave_corr_t;

/* No correction at all. */
void interleave_corr_identity(interleave_corr_t *c);

/* Knots a unit may carry: 2..IL_KNOTS_MAX, each in 0..IL_KNOT_MAX_C, both
 * coordinates strictly ascending, every segment slope dB/dA in 0.5..2 (the
 * linear fit's gain window). */
bool interleave_knots_valid(uint8_t n, const int16_t *a, const int16_t *b);

/* Set (n >= 2) or clear (n = 0) the piecewise map. Refuses invalid knots and
 * leaves the map as it was. */
bool interleave_corr_set_knots(interleave_corr_t *c, uint8_t n,
                               const int16_t *a, const int16_t *b);

/* Set the skew correction (|ppm| <= IL_SKEW_PPM_MAX; 0 = off) and compute its
 * taps. Refuses out of range and leaves the correction as it was. */
bool interleave_skew_set(interleave_corr_t *c, int32_t ppm);

/* Level map of one B value into A's units, unrounded. */
float interleave_map_b(float b, const interleave_corr_t *c);

/* Correct one converter-B sample (level map only, rounded, clamped). */
uint8_t interleave_correct_b(uint8_t b, const interleave_corr_t *c);

/* Merge in place: `a` holds converter A's 1024 samples on entry and the
 * 1024-sample merged record on return. With a correction, the window
 * b[256..767] is rewritten in place with corrected B (the rest of `b` keeps
 * its raw values); without one (`c` NULL or identity) `b` is only read. */
void interleave_merge(uint8_t *a, uint8_t *b, const interleave_corr_t *c);

#endif
