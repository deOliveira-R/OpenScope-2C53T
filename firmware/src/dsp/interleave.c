/*
 * interleave — see interleave.h.
 *
 * The merge is in place without a scratch buffer. Output index i reads
 * A[256 + i/2]:
 *   - i in [0, 512) reads A[256..511]: the read index is ahead of i, so walk
 *     upward;
 *   - i in [512, 1024) reads A[512..767]: the read index is at or behind i, so
 *     walk downward.
 * Each half writes only indices the other half never reads ([0,512) vs reads
 * of [512,768); [512,1024) vs reads of [256,512)), so the two passes do not
 * interfere and each one only overwrites A samples it has already consumed.
 *
 * B is corrected first, in its own buffer, while A is still intact: the skew
 * FIR reads A neighbours, which the merge would otherwise have moved.
 */
#include "interleave.h"
#include "interleave_skew_taps.h"

#include <stddef.h>

typedef char il_skew_taps_check[(IL_SKEW_TAPS == IL_SKEW_TAPS_N) ? 1 : -1];

void interleave_corr_identity(interleave_corr_t *c)
{
    *c = (interleave_corr_t){ .gain = 1.0f, .offset = 0.0f };
}

bool interleave_knots_valid(uint8_t n, const int16_t *a, const int16_t *b)
{
    if (n < 2u || n > IL_KNOTS_MAX || !a || !b) return false;
    for (uint8_t i = 0; i < n; i++) {
        if (a[i] < 0 || a[i] > IL_KNOT_MAX_C || b[i] < 0 || b[i] > IL_KNOT_MAX_C) return false;
        if (i == 0) continue;
        const int32_t da = a[i] - a[i - 1], db = b[i] - b[i - 1];
        /* 0.5 <= db/da <= 2, integer form; also rules out da, db <= 0. */
        if (da <= 0 || db <= 0 || 2 * db < da || db > 2 * da) return false;
    }
    return true;
}

bool interleave_corr_set_knots(interleave_corr_t *c, uint8_t n,
                               const int16_t *a, const int16_t *b)
{
    if (!c) return false;
    if (n == 0u) { c->n_knots = 0; return true; }
    if (!interleave_knots_valid(n, a, b)) return false;
    c->n_knots = 0;                          /* the acq task falls back to the line meanwhile */
    __asm volatile ("" ::: "memory");
    for (uint8_t i = 0; i < n; i++) { c->knot_a[i] = a[i]; c->knot_b[i] = b[i]; }
    __asm volatile ("" ::: "memory");
    c->n_knots = n;
    return true;
}

bool interleave_skew_set(interleave_corr_t *c, int32_t ppm)
{
    if (!c || ppm > IL_SKEW_PPM_MAX || ppm < -IL_SKEW_PPM_MAX) return false;
    c->skew_ppm = 0;                         /* the merge skips the FIR while taps change */
    __asm volatile ("" ::: "memory");
    if (ppm == 0) return true;
    const float d = 2.0f * (float)ppm * 1e-6f;   /* merged samples */
    float sum = 0.0f;
    for (unsigned j = 0; j < IL_SKEW_TAPS; j++) {
        float g = 0.0f, dp = d;
        for (unsigned p = 0; p < IL_SKEW_P; p++, dp *= d) g += IL_SKEW_FARROW[p][j] * dp;
        if (j == IL_SKEW_M) g += 1.0f;
        c->skew_taps[j] = g;
        sum += g;
    }
    /* Unit DC gain exactly: the level map has already put B on A's scale, and
     * a fitted sum of 1 - 1e-3 would move a level of 200 by 0.2 count. */
    c->skew_taps[IL_SKEW_M] += 1.0f - sum;
    __asm volatile ("" ::: "memory");
    c->skew_ppm = ppm;
    return true;
}

static bool corr_is_identity(const interleave_corr_t *c)
{
    return c == NULL ||
           (c->n_knots < 2u && c->skew_ppm == 0 &&
            ((c->gain == 1.0f && c->offset == 0.0f) || c->gain <= 0.0f));
}

float interleave_map_b(float b, const interleave_corr_t *c)
{
    if (c == NULL) return b;
    const uint8_t n = c->n_knots;
    if (n >= 2u && n <= IL_KNOTS_MAX) {
        const float bc = b * (float)IL_KNOT_SCALE;
        uint8_t i = 0;                       /* segment [i, i+1]; the end ones extrapolate */
        while (i + 2u < n && bc > (float)c->knot_b[i + 1]) i++;
        const float b0 = c->knot_b[i], b1 = c->knot_b[i + 1];
        const float a0 = c->knot_a[i], a1 = c->knot_a[i + 1];
        if (b1 <= b0) return b;              /* a torn update seen mid-write: leave B */
        return (a0 + (bc - b0) * (a1 - a0) / (b1 - b0)) / (float)IL_KNOT_SCALE;
    }
    if ((c->gain == 1.0f && c->offset == 0.0f) || c->gain <= 0.0f) return b;
    return (b - c->offset) / c->gain;
}

static uint8_t round_clamp(float v)
{
    if (v <= 0.0f)   return 0u;
    if (v >= 255.0f) return 255u;
    return (uint8_t)(v + 0.5f);
}

uint8_t interleave_correct_b(uint8_t b, const interleave_corr_t *c)
{
    return round_clamp(interleave_map_b((float)b, c));
}

/* B[k] for k in the merge window, from the 17 merged neighbours of output
 * 2k+1: tap j (offset j - 8) even reads B[k + (j - 8)/2] (level-mapped), odd
 * reads A[k + (j - 7)/2]. Walking up, B[k-4..k-1] are already overwritten, so
 * a 16-entry ring keeps the mapped values of B[k-4..k+4]; B[k+4] is still
 * raw when it enters. */
static void skew_correct_b(const uint8_t *a, uint8_t *b, const interleave_corr_t *c)
{
    float ring[16];
    const unsigned lo = INTERLEAVE_WIN_LO, hi = INTERLEAVE_WIN_LO + INTERLEAVE_N / 2u;
    const unsigned h = IL_SKEW_M / 2u;       /* 4: B reach either side */
    const float *g = c->skew_taps;
    for (unsigned k = lo - h; k < lo + h; k++) ring[k & 15u] = interleave_map_b((float)b[k], c);
    for (unsigned k = lo; k < hi; k++) {
        ring[(k + h) & 15u] = interleave_map_b((float)b[k + h], c);
        float acc = 0.0f;
        for (unsigned j = 0; j < IL_SKEW_TAPS; j += 2u)          /* B taps */
            acc += g[j] * ring[(k + j / 2u - h) & 15u];
        for (unsigned j = 1; j < IL_SKEW_TAPS; j += 2u)          /* A taps */
            acc += g[j] * (float)a[k + (j + 1u) / 2u - h];
        b[k] = round_clamp(acc);
    }
}

void interleave_merge(uint8_t *a, uint8_t *b, const interleave_corr_t *c)
{
    const uint32_t half = INTERLEAVE_N / 2u;
    const unsigned lo = INTERLEAVE_WIN_LO, hi = INTERLEAVE_WIN_LO + half;

    if (!corr_is_identity(c)) {
        if (c->skew_ppm != 0) skew_correct_b(a, b, c);
        else for (unsigned k = lo; k < hi; k++) b[k] = interleave_correct_b(b[k], c);
    }
    /* Upper half first, walking down: out[i] = A or B at 256 + i/2 >= 512. */
    for (uint32_t i = INTERLEAVE_N; i-- > half; ) {
        const uint32_t k = INTERLEAVE_WIN_LO + i / 2u;
        a[i] = (i & 1u) ? b[k] : a[k];
    }
    /* Lower half, walking up: out[i] = A or B at 256 + i/2 < 512. */
    for (uint32_t i = 0; i < half; i++) {
        const uint32_t k = INTERLEAVE_WIN_LO + i / 2u;
        a[i] = (i & 1u) ? b[k] : a[k];
    }
}
