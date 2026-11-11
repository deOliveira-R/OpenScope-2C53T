/*
 * interleave — see interleave.h.
 *
 * In place without a scratch buffer. Output index i reads A[256 + i/2]:
 *   - i in [0, 512) reads A[256..511]: the read index is ahead of i, so walk
 *     upward;
 *   - i in [512, 1024) reads A[512..767]: the read index is at or behind i, so
 *     walk downward.
 * Each half writes only indices the other half never reads ([0,512) vs reads
 * of [512,768); [512,1024) vs reads of [256,512)), so the two passes do not
 * interfere and each one only overwrites A samples it has already consumed.
 */
#include "interleave.h"

#include <stddef.h>

uint8_t interleave_correct_b(uint8_t b, const interleave_corr_t *c)
{
    if (c == NULL || (c->gain == 1.0f && c->offset == 0.0f) || c->gain <= 0.0f)
        return b;
    float v = ((float)b - c->offset) / c->gain;
    if (v <= 0.0f)   return 0u;
    if (v >= 255.0f) return 255u;
    return (uint8_t)(v + 0.5f);
}

void interleave_merge(uint8_t *a, const uint8_t *b, const interleave_corr_t *c)
{
    const uint32_t half = INTERLEAVE_N / 2u;

    /* Upper half first, walking down: out[i] = A or B at 256 + i/2 >= 512. */
    for (uint32_t i = INTERLEAVE_N; i-- > half; ) {
        const uint32_t k = INTERLEAVE_WIN_LO + i / 2u;
        a[i] = (i & 1u) ? interleave_correct_b(b[k], c) : a[k];
    }
    /* Lower half, walking up: out[i] = A or B at 256 + i/2 < 512. */
    for (uint32_t i = 0; i < half; i++) {
        const uint32_t k = INTERLEAVE_WIN_LO + i / 2u;
        a[i] = (i & 1u) ? interleave_correct_b(b[k], c) : a[k];
    }
}
