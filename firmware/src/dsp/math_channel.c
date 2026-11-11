/*
 * Math channel on the real records (parity C12). See math_channel.h.
 */

#include "math_channel.h"
#include <stdio.h>

int math_op_uses_a(math_op_t op)
{
    return op == MATH_ADD || op == MATH_SUB || op == MATH_MUL || op == MATH_DIV
        || op == MATH_INV_A || op == MATH_ABS_A;
}

int math_op_uses_b(math_op_t op)
{
    return op == MATH_ADD || op == MATH_SUB || op == MATH_MUL || op == MATH_DIV
        || op == MATH_INV_B || op == MATH_ABS_B;
}

static int calibrated_for(math_op_t op, const math_in_t *in)
{
    if (math_op_uses_a(op) && !(in->k_a > 0.0f)) return 0;
    if (math_op_uses_b(op) && !(in->k_b > 0.0f)) return 0;
    return 1;
}

math_unit_t math_unit_for(math_op_t op, const math_in_t *in)
{
    int cal = calibrated_for(op, in);
    if (op == MATH_DIV) return MATH_UNIT_RATIO;
    if (op == MATH_MUL) return cal ? MATH_UNIT_VOLTS2 : MATH_UNIT_COUNTS2;
    return cal ? MATH_UNIT_VOLTS : MATH_UNIT_COUNTS;
}

math_unit_t math_compute(math_op_t op, const uint8_t *a, const uint8_t *b,
                         uint16_t n, const math_in_t *in, float *out)
{
    const int cal = calibrated_for(op, in);
    const float ka = cal ? in->k_a : 1.0f;
    const float kb = cal ? in->k_b : 1.0f;
    const int ua = math_op_uses_a(op), ub = math_op_uses_b(op);

    for (uint16_t i = 0; i < n; i++) {
        /* counts about each channel's own 0 V code */
        const int ca = ua ? (int)a[i] - (int)in->zero_a : 0;
        const int cb = ub ? (int)b[i] - (int)in->zero_b : 0;
        const float va = (float)ca * ka;
        const float vb = (float)cb * kb;
        float r;

        switch (op) {
        case MATH_ADD:   r = va + vb; break;
        case MATH_SUB:   r = va - vb; break;
        case MATH_MUL:   r = va * vb; break;
        case MATH_DIV:
            if (cb > -MATH_DIV_DEAD_COUNTS && cb < MATH_DIV_DEAD_COUNTS)
                r = (ca > 0) ? MATH_SAT : (ca < 0) ? -MATH_SAT : 0.0f;
            else
                r = va / vb;
            break;
        case MATH_INV_A: r = -va; break;
        case MATH_INV_B: r = -vb; break;
        case MATH_ABS_A: r = va < 0.0f ? -va : va; break;
        case MATH_ABS_B: r = vb < 0.0f ? -vb : vb; break;
        default:         r = 0.0f; break;
        }
        out[i] = r;
    }
    return math_unit_for(op, in);
}

float math_peak(const float *v, uint16_t n)
{
    float pk = 0.0f;
    for (uint16_t i = 0; i < n; i++) {
        float m = v[i] < 0.0f ? -v[i] : v[i];
        if (m >= MATH_SAT) continue;
        if (m > pk) pk = m;
    }
    return pk;
}

/* 1,2,5 x 10^e, e = -3..4 (0.001 .. 50000). */
float math_scale_value(int idx)
{
    static const float mant[3] = { 1.0f, 2.0f, 5.0f };
    if (idx < 0) idx = 0;
    if (idx >= MATH_SCALE_COUNT) idx = MATH_SCALE_COUNT - 1;
    float v = mant[idx % 3];
    for (int e = idx / 3 - 3; e > 0; e--) v *= 10.0f;
    for (int e = idx / 3 - 3; e < 0; e++) v /= 10.0f;
    return v;
}

static int fits_x10(float peak, int idx, int limit_x10)
{
    return peak * 10.0f <= (float)limit_x10 * math_scale_value(idx);
}

int math_scale_pick(float peak, int prev)
{
    /* Keep the old step while the peak is inside MATH_KEEP_DIVS_X10 (a little
     * beyond the pick limit) and would not fit the next lower step: a peak
     * that sits on a boundary moves the scale at most once. */
    if (prev >= 0 && prev < MATH_SCALE_COUNT
        && fits_x10(peak, prev, MATH_KEEP_DIVS_X10)
        && (prev == 0 || !fits_x10(peak, prev - 1, MATH_HALF_DIVS_X10)))
        return prev;
    int i = 0;
    while (i < MATH_SCALE_COUNT - 1 && !fits_x10(peak, i, MATH_HALF_DIVS_X10)) i++;
    return i;
}

float math_step_floor(math_op_t op, const math_in_t *in, float counts_per_div)
{
    const int cal = calibrated_for(op, in);
    const float da = (cal ? in->k_a : 1.0f) * counts_per_div;
    const float db = (cal ? in->k_b : 1.0f) * counts_per_div;
    switch (op) {
    case MATH_ADD:
    case MATH_SUB:   return da < db ? da : db;
    case MATH_MUL:   return da * db;
    case MATH_INV_A:
    case MATH_ABS_A: return da;
    case MATH_INV_B:
    case MATH_ABS_B: return db;
    default:         return 0.0f;           /* MATH_DIV: dimensionless */
    }
}

int math_scale_floor_idx(float floor_step)
{
    int i = 0;
    if (!(floor_step > 0.0f)) return 0;
    /* 0.1 % slack so a floor that IS a ladder value (0.5 V) is not pushed one
     * step up by float rounding. */
    while (i < MATH_SCALE_COUNT - 1 && math_scale_value(i) < floor_step * 0.999f) i++;
    return i;
}

void math_scale_label(int idx, math_unit_t unit, char *out, uint32_t n)
{
    static const char *const suffix[] = { "c", "c2", "V", "V2", "" };
    static const unsigned mant[3] = { 1u, 2u, 5u };
    if (idx < 0) idx = 0;
    if (idx >= MATH_SCALE_COUNT) idx = MATH_SCALE_COUNT - 1;
    int e = idx / 3 - 3;                 /* value = mant * 10^e */
    unsigned m = mant[idx % 3];
    char num[16];

    if (unit == MATH_UNIT_VOLTS && e < 0 && e >= -3) {
        /* millivolts: mant * 10^(e+3), always an integer here */
        unsigned v = m;
        for (int k = e + 3; k > 0; k--) v *= 10u;
        snprintf(num, sizeof(num), "%umV", v);
    } else if (e >= 0) {
        unsigned v = m;
        for (int k = e; k > 0; k--) v *= 10u;
        snprintf(num, sizeof(num), "%u%s", v, suffix[unit]);
    } else {
        /* 0.00m : -e digits after the point, the mantissa in the last */
        char dec[8];
        int z = -e - 1, p = 0;
        dec[p++] = '0'; dec[p++] = '.';
        while (z-- > 0 && p < 6) dec[p++] = '0';
        dec[p] = 0;
        snprintf(num, sizeof(num), "%s%u%s", dec, m, suffix[unit]);
    }
    snprintf(out, n, "%s/d", num);
}

void math_to_rows(const float *v, uint16_t n, float step, int px_per_div,
                  int y_mid, int y_top, int y_bot, int16_t *rows)
{
    const float lim = (float)(y_bot - y_top) * 2.0f;   /* far past the band */
    for (uint16_t i = 0; i < n; i++) {
        float d = (step > 0.0f) ? v[i] / step * (float)px_per_div : 0.0f;
        if (d > lim) d = lim;
        if (d < -lim) d = -lim;
        int off = (int)(d >= 0.0f ? d + 0.5f : d - 0.5f);
        int y = y_mid - off;
        if (y < y_top) y = y_top;
        if (y > y_bot - 1) y = y_bot - 1;
        rows[i] = (int16_t)y;
    }
}

const char *math_channel_name(math_op_t op)
{
    static const char *const names[MATH_COUNT] = {
        "A+B", "A-B", "A*B", "A/B", "-A", "-B", "|A|", "|B|"
    };
    return ((unsigned)op < MATH_COUNT) ? names[op] : "???";
}
