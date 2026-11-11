#include "interleave_cal.h"

#include <math.h>
#include <stddef.h>

void il_cal_default_params(il_cal_params_t *p)
{
    p->max_p2p    = 32;     /* glitch-trimmed spread: a quiet input reads +-5 codes at 125 MS/s from a noisy source (bench 2026-11-11); a real signal swings far more */
    p->max_drift  = 2.0f;
    p->min_points = 3;
    p->min_span   = 60.0f;
    p->gain_lo    = 0.5f;   /* the shell's own corr window, 5000..20000 /1e4 */
    p->gain_hi    = 2.0f;
    p->max_resid  = 1.5f;
}

il_cal_status_t il_cal_fit(const il_cal_point_t *pts, unsigned n,
                           const il_cal_params_t *prm, il_cal_fit_t *out)
{
    if (!pts || !prm || !out || n > IL_CAL_MAX_POINTS) return IL_CAL_ARGS;
    out->gain = 1.0f; out->offset = 0.0f;
    out->resid_max = out->resid_rms = out->a_span = 0.0f;
    out->used = 0;
    out->n_knots = 0;

    /* Flatness is judged on every point, clipped or not: a noisy input is
     * refused outright. */
    for (unsigned i = 0; i < n; i++) {
        if (pts[i].a_p2p > prm->max_p2p || pts[i].b_p2p > prm->max_p2p ||
            pts[i].a_drift > prm->max_drift || pts[i].b_drift > prm->max_drift)
            return IL_CAL_NOT_FLAT;
    }

    double sa = 0, sb = 0, saa = 0, sab = 0, amin = 1e9, amax = -1e9;
    unsigned m = 0;
    for (unsigned i = 0; i < n; i++) {
        if (pts[i].clipped) continue;       /* a rail is not a level */
        const double a = pts[i].a_mean, b = pts[i].b_mean;
        sa += a; sb += b; saa += a * a; sab += a * b;
        if (a < amin) amin = a;
        if (a > amax) amax = a;
        m++;
    }
    out->used = (uint8_t)m;
    if (m < prm->min_points) return IL_CAL_FEW_POINTS;
    out->a_span = (float)(amax - amin);
    if (out->a_span < prm->min_span) return IL_CAL_SPAN;

    const double den = (double)m * saa - sa * sa;
    if (den <= 0.0) return IL_CAL_SPAN;
    const double g = ((double)m * sab - sa * sb) / den;
    const double o = (sb - g * sa) / (double)m;
    if (g < prm->gain_lo || g > prm->gain_hi) {
        out->gain = (float)g; out->offset = (float)o;
        return IL_CAL_GAIN_RANGE;
    }
    double worst = 0, ss = 0;
    for (unsigned i = 0; i < n; i++) {
        if (pts[i].clipped) continue;
        const double r = (double)pts[i].b_mean - (g * pts[i].a_mean + o);
        const double ar = fabs(r);
        if (ar > worst) worst = ar;
        ss += r * r;
    }
    out->gain = (float)g; out->offset = (float)o;
    out->resid_max = (float)worst;
    out->resid_rms = (float)sqrt(ss / (double)m);
    if (worst > prm->max_resid) return IL_CAL_POOR_FIT;

    /* Knots: the usable levels, ascending in A. Beyond IL_KNOTS_MAX levels
     * the line alone is applied (the shell sweeps five). */
    if (m > IL_KNOTS_MAX) return IL_CAL_OK;
    uint8_t k = 0;
    for (unsigned i = 0; i < n; i++) {
        if (pts[i].clipped) continue;
        const float a = pts[i].a_mean, b = pts[i].b_mean;
        if (a < 0.0f || b < 0.0f || a > 255.0f || b > 255.0f) return IL_CAL_NOT_MONOTONIC;
        const int16_t ac = (int16_t)(a * (float)IL_KNOT_SCALE + 0.5f);
        const int16_t bc = (int16_t)(b * (float)IL_KNOT_SCALE + 0.5f);
        uint8_t j = k;
        while (j > 0 && out->knot_a[j - 1] > ac) {          /* insertion sort */
            out->knot_a[j] = out->knot_a[j - 1];
            out->knot_b[j] = out->knot_b[j - 1];
            j--;
        }
        out->knot_a[j] = ac; out->knot_b[j] = bc;
        k++;
    }
    /* B must rise with A, with every segment's slope in the plausible window
     * (interleave_knots_valid): a level that reads lower in B than the level
     * below it is noise or a moving input, not a converter. */
    if (!interleave_knots_valid(k, out->knot_a, out->knot_b)) return IL_CAL_NOT_MONOTONIC;
    out->n_knots = k;
    return IL_CAL_OK;
}

bool il_cal_dac_points(uint16_t base, uint16_t upper, uint16_t out[5])
{
    const int b = base, u = upper;
    const int d = (u >= b) ? 1 : -1;
    if ((u - b) * d < 1000) return false;
    const int v[5] = { b + d * 80, b + d * 400, (b + u) / 2, u - d * 400, u };
    for (int i = 0; i < 5; i++) {
        if (v[i] < 0 || v[i] > 4095) return false;
        out[i] = (uint16_t)v[i];
    }
    return true;
}

const char *il_cal_status_str(il_cal_status_t s)
{
    switch (s) {
    case IL_CAL_OK:         return "ok";
    case IL_CAL_ARGS:       return "bad arguments";
    case IL_CAL_NOT_FLAT:   return "input not flat (noise or a signal on CH1)";
    case IL_CAL_FEW_POINTS: return "too few usable points (levels at a rail)";
    case IL_CAL_SPAN:       return "level span too small to fit";
    case IL_CAL_GAIN_RANGE: return "gain outside 0.5..2.0";
    case IL_CAL_POOR_FIT:   return "poor fit (B is not a line of A)";
    case IL_CAL_NOT_MONOTONIC: return "levels not monotonic (B must rise with A)";
    }
    return "?";
}

/* ── Timing skew ────────────────────────────────────────────────────── */

#define SK_N      (INTERLEAVE_N / 2u)   /* samples per converter in the record */
#define SK_BIN_LO 20u                   /* 4.9 MHz: below it the phase w * tau is so small that
                                         * the 8-bit record's quantisation error (coherent with a
                                         * crystal tone) is a large part of it */
#define SK_BIN_HI 205u                  /* 50.0 MHz: the front end's band */
#define SK_PI     3.14159265358979f

/* |DFT|^2 of one converter's samples (phase 0 = A, 1 = B) at w rad/sample. */
static float sk_power(const uint8_t *m, unsigned ph, float mean, float w)
{
    const float cw = 2.0f * cosf(w);
    float s1 = 0.0f, s2 = 0.0f;
    for (unsigned k = 0; k < SK_N; k++) {
        const float s = ((float)m[2u * k + ph] - mean) + cw * s1 - s2;
        s2 = s1; s1 = s;
    }
    return s1 * s1 + s2 * s2 - cw * s1 * s2;
}

/* Hann-weighted least-squares sine at w: y ~ d + i cos(w c) + q sin(w c),
 * c = k - (N-1)/2 on both converters' common index grid. The window keeps the
 * square wave's harmonics from leaking into the fundamental's phase. Returns
 * amplitude, phase and the weighted rms residual. */
static void sk_fit(const uint8_t *m, unsigned ph, float w, float *amp, float *phase, float *rms)
{
    float sw = 0, sc = 0, ss = 0, scc = 0, sss = 0, scs = 0;
    float sy = 0, syc = 0, sys = 0, syy = 0;
    const float c0 = -0.5f * (float)(SK_N - 1u);
    float cs = cosf(w * c0), sn = sinf(w * c0);
    const float rc = cosf(w), rs = sinf(w);
    float hc = 1.0f, hs = 0.0f;                         /* Hann phase 2 pi k / N */
    const float hrc = cosf(2.0f * SK_PI / (float)SK_N), hrs = sinf(2.0f * SK_PI / (float)SK_N);
    for (unsigned k = 0; k < SK_N; k++) {
        const float wt = 0.5f - 0.5f * hc;
        const float y = (float)m[2u * k + ph];
        sw += wt; sc += wt * cs; ss += wt * sn;
        scc += wt * cs * cs; sss += wt * sn * sn; scs += wt * cs * sn;
        sy += wt * y; syc += wt * y * cs; sys += wt * y * sn; syy += wt * y * y;
        const float nc = cs * rc - sn * rs; sn = sn * rc + cs * rs; cs = nc;
        const float nh = hc * hrc - hs * hrs; hs = hs * hrc + hc * hrs; hc = nh;
    }
    /* Cramer on the symmetric 3x3 normal equations. */
    const float m00 = sw, m01 = sc, m02 = ss, m11 = scc, m12 = scs, m22 = sss;
    const float c00 = m11 * m22 - m12 * m12, c01 = m02 * m12 - m01 * m22, c02 = m01 * m12 - m02 * m11;
    const float det = m00 * c00 + m01 * c01 + m02 * c02;
    if (!(det > 0.0f) && !(det < 0.0f)) { *amp = 0.0f; *phase = 0.0f; *rms = 0.0f; return; }
    const float c11 = m00 * m22 - m02 * m02, c12 = m01 * m02 - m00 * m12, c22 = m00 * m11 - m01 * m01;
    const float d = (c00 * sy + c01 * syc + c02 * sys) / det;
    const float i = (c01 * sy + c11 * syc + c12 * sys) / det;
    const float q = (c02 * sy + c12 * syc + c22 * sys) / det;
    *amp = sqrtf(i * i + q * q);
    *phase = atan2f(q, i);
    const float e = syy - (d * sy + i * syc + q * sys);   /* LS identity */
    *rms = sqrtf((e > 0.0f ? e : 0.0f) / sw);
}

il_skew_status_t il_skew_estimate(const uint8_t *merged, il_skew_est_t *out)
{
    if (!merged || !out) return IL_SKEW_ARGS;
    out->tau = 0.5f; out->cycles = 0.0f;
    out->amp_a = out->amp_b = out->resid_a = 0.0f;

    uint32_t s = 0;
    for (unsigned k = 0; k < SK_N; k++) s += merged[2u * k];
    const float mean = (float)s / (float)SK_N;

    /* Coarse: the strongest integer bin of A's whole spectrum (1..N/2 - 1),
     * so a tone outside the range is found where it is and refused, rather
     * than its leakage at the range's edge taken for a peak. */
    const float dw = 2.0f * SK_PI / (float)SK_N;
    unsigned best = 0; float bp = -1.0f;
    for (unsigned b = 1u; b < SK_N / 2u; b++) {
        const float p = sk_power(merged, 0, mean, dw * (float)b);
        if (p > bp) { bp = p; best = b; }
    }
    out->cycles = (float)best / (float)SK_N;
    if (best < SK_BIN_LO || best > SK_BIN_HI) return IL_SKEW_FREQ;

    /* Fine: golden section on the main lobe (unimodal within +-1 bin). */
    float lo = (float)best - 1.0f, hi = (float)best + 1.0f;
    const float gr = 0.618034f;
    float x1 = hi - gr * (hi - lo), x2 = lo + gr * (hi - lo);
    float f1 = sk_power(merged, 0, mean, dw * x1), f2 = sk_power(merged, 0, mean, dw * x2);
    for (unsigned it = 0; it < 28u; it++) {
        if (f1 > f2) { hi = x2; x2 = x1; f2 = f1; x1 = hi - gr * (hi - lo); f1 = sk_power(merged, 0, mean, dw * x1); }
        else         { lo = x1; x1 = x2; f1 = f2; x2 = lo + gr * (hi - lo); f2 = sk_power(merged, 0, mean, dw * x2); }
    }
    const float bin = 0.5f * (lo + hi);
    const float w = dw * bin;
    out->cycles = bin / (float)SK_N;

    float pa, pb, ra, rb;
    sk_fit(merged, 0, w, &out->amp_a, &pa, &ra);
    sk_fit(merged, 1, w, &out->amp_b, &pb, &rb);
    out->resid_a = out->amp_a > 0.0f ? ra / out->amp_a : 1e9f;
    if (out->amp_a < 8.0f) return IL_SKEW_LOW_AMP;
    /* The level map has put B on A's scale: the same tone reads the same
     * amplitude. A torn record or a signal on one converter only does not. */
    const float ratio = out->amp_b / out->amp_a;
    if (ratio < 0.8f || ratio > 1.25f) return IL_SKEW_MISMATCH;
    /* A square wave leaves ~0.3 of its fundamental outside the fit; noise and
     * tearing more. */
    if (out->resid_a > 0.6f) return IL_SKEW_NOISY;

    float dp = pa - pb;                       /* B later: its phase is behind */
    while (dp >  SK_PI) dp -= 2.0f * SK_PI;
    while (dp < -SK_PI) dp += 2.0f * SK_PI;
    out->tau = dp / w;
    const float dev = out->tau - 0.5f;
    if (dev > (float)IL_SKEW_PPM_MAX * 1e-6f || dev < -(float)IL_SKEW_PPM_MAX * 1e-6f)
        return IL_SKEW_RANGE;
    return IL_SKEW_OK;
}

const char *il_skew_status_str(il_skew_status_t s)
{
    switch (s) {
    case IL_SKEW_OK:       return "ok";
    case IL_SKEW_ARGS:     return "bad arguments";
    case IL_SKEW_LOW_AMP:  return "no tone (fundamental below 8 counts)";
    case IL_SKEW_FREQ:     return "tone outside 5..50 MHz";
    case IL_SKEW_MISMATCH: return "A and B disagree (torn record or CH2 on)";
    case IL_SKEW_NOISY:    return "not a steady tone";
    case IL_SKEW_RANGE:    return "B outside 0.35..0.65 of a period";
    }
    return "?";
}
