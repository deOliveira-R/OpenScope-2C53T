/* Zoom while paused: pure arithmetic, see paused_view.h. */
#include "paused_view.h"
#include <math.h>

#define PV_PI 3.14159265358979f

uint32_t pv_ratio_x1000(float fs_cap, float fs_now)
{
    if (!(fs_cap > 0.0f) || !(fs_now > 0.0f))
        return PV_RATIO_ONE;
    float r = 1000.0f * fs_cap / fs_now;
    if (r < (float)PV_RATIO_MIN) r = (float)PV_RATIO_MIN;
    if (r > (float)PV_RATIO_MAX) r = (float)PV_RATIO_MAX;
    return (uint32_t)(r + 0.5f);
}

uint32_t pv_stock_ratio_x1000(uint8_t tb_now, uint32_t record_period_0p4ns,
                              bool merged)
{
    static const uint8_t M[3] = { 1, 2, 5 };           /* table 0x080466C8 */
    uint32_t disp = M[tb_now % 3u];                    /* x 10^(tb/3), 0.4 ns */
    for (uint8_t i = 0; i < tb_now / 3u; i++)
        disp *= 10u;
    uint32_t rec = record_period_0p4ns * (merged ? 2u : 1u);
    if (rec == 0u)
        return PV_RATIO_ONE;
    return (uint32_t)(((uint64_t)disp * 1000u + rec / 2u) / rec);
}

/* Hamming-windowed sinc, half-width 8 (the 16 taps span d = -7-f .. 8-f).
 * sin(pi d) for d = k - 7 - f is -(-1)^(k-7) sin(pi f) ... one sinf per call;
 * the window cosine runs by the Chebyshev recurrence, so a column costs two
 * trig calls, not 32. */
void pv_taps(float frac, float taps[PV_TAPS])
{
    if (frac < 0.0f) frac = 0.0f;
    if (frac >= 1.0f) frac = 0.999999f;
    if (frac < 1e-6f) {                       /* exact: phase 0 is identity */
        for (uint32_t k = 0; k < PV_TAPS; k++)
            taps[k] = (k == 7u) ? 1.0f : 0.0f;
        return;
    }
    const float sf = sinf(PV_PI * frac);
    const float step = PV_PI / 8.0f;
    const float c1 = 2.0f * cosf(step);
    /* window argument u = d / 8 -> cos(pi d / 8); d(k) = k - 7 - frac */
    float cm2 = cosf(step * (-7.0f - frac - 1.0f));   /* k = -1 */
    float cm1 = cosf(step * (-7.0f - frac));          /* k = 0  */
    float sum = 0.0f;
    for (uint32_t k = 0; k < PV_TAPS; k++) {
        const float d = (float)(int32_t)k - 7.0f - frac;
        const float c = (k == 0u) ? cm1 : (c1 * cm1 - cm2);
        if (k != 0u) { cm2 = cm1; cm1 = c; }
        const float w = 0.54f + 0.46f * c;
        const float sgn = (((int32_t)k - 7) & 1) ? 1.0f : -1.0f; /* -(-1)^(k-7) */
        const float s = sgn * sf / (PV_PI * d);       /* sinc(d) */
        taps[k] = s * w;
        sum += taps[k];
    }
    const float inv = 1.0f / sum;
    for (uint32_t k = 0; k < PV_TAPS; k++)
        taps[k] *= inv;
}

int32_t pv_src_pos_x1000(int32_t col, int32_t pivot_col, int32_t pivot_idx,
                         uint32_t ratio_x1000)
{
    return pivot_idx * 1000 + (col - pivot_col) * (int32_t)ratio_x1000;
}

static inline int32_t floordiv1000(int32_t v)
{
    return (v >= 0) ? v / 1000 : -((-v + 999) / 1000);
}

bool pv_sample(const volatile uint8_t *rec, int32_t lo, int32_t hi,
               int32_t pos_x1000, uint32_t ratio_x1000, uint8_t *out)
{
    *out = 0;
    if (rec == 0 || hi <= lo)
        return false;
    if (ratio_x1000 >= PV_RATIO_ONE) {
        int32_t i = floordiv1000(pos_x1000 + 500);
        if (i < lo || i >= hi)
            return false;
        *out = rec[i];
        return true;
    }
    int32_t n = floordiv1000(pos_x1000);                /* in[n] <= pos */
    if (n < lo || n >= hi)
        return false;
    const float frac = (float)(pos_x1000 - n * 1000) * 0.001f;
    float taps[PV_TAPS];
    pv_taps(frac, taps);
    float acc = 0.0f;
    for (int32_t k = 0; k < (int32_t)PV_TAPS; k++) {
        int32_t i = n - 7 + k;
        if (i < lo) i = lo;
        if (i >= hi) i = hi - 1;
        acc += taps[k] * (float)rec[i];
    }
    int v = (int)(acc + 0.5f);
    if (acc < 0.0f) v = 0;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    *out = (uint8_t)v;
    return true;
}

int pv_y_map(int raw, int centre, int pos_cap, int pos_now,
             float v_cap, float v_now, int lo, int hi)
{
    float k = (v_cap > 0.0f && v_now > 0.0f) ? v_cap / v_now : 1.0f;
    float y = (float)(centre + pos_now) + k * (float)(raw - centre - pos_cap);
    int yi = (int)(y + (y >= 0.0f ? 0.5f : -0.5f));
    if (yi < lo) yi = lo;
    if (yi > hi) yi = hi;
    return yi;
}
