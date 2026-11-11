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
 * Converter B can be corrected for gain and offset against A before the merge:
 *
 *     B' = (B - offset) / gain      (record units, rounded, clamped 0..255)
 *
 * The identity (gain 1, offset 0) leaves B untouched. Unit #2 measured
 * B = 0.9795 A + 4.6 in raw codes; those numbers belong to that unit and are
 * set at run time, never compiled in. Timing skew (0.05 sample on unit #2) is
 * not corrected here: B alone is undersampled for content above 62.5 MHz, so
 * a fractional delay on B's own samples would be wrong for exactly the signals
 * that need it.
 */
#ifndef INTERLEAVE_H
#define INTERLEAVE_H

#include <stdint.h>

#define INTERLEAVE_N        1024u   /* record length in and out */
#define INTERLEAVE_WIN_LO    256u   /* first converter sample used */

typedef struct {
    float gain;     /* B / A slope; 1.0 = no correction */
    float offset;   /* B - gain * A at A = 0, record units */
} interleave_corr_t;

/* Correct one converter-B sample. */
uint8_t interleave_correct_b(uint8_t b, const interleave_corr_t *c);

/* Merge in place: `a` holds converter A's 1024 samples on entry and the
 * 1024-sample merged record on return. `b` is read only. `c` may be NULL
 * (no correction). */
void interleave_merge(uint8_t *a, const uint8_t *b, const interleave_corr_t *c);

#endif
