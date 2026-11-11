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
    }
    return "?";
}
