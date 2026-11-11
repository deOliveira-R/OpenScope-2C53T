/* Zoom while paused: pure arithmetic, see paused_view.h. */
#include "paused_view.h"
#include "sinc_interp.h"

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

/* One kernel for both zooms (C5 x C14): the fast-code table of sinc_interp.c
 * (sinc x Kaiser beta 5), blended between its 20 rows for an arbitrary phase.
 * Until the C5 merge this file generated its own Hamming-windowed sinc; the
 * two agreed to ~1 % of a tap, and keeping one means the paused view and the
 * running fast-code zoom cannot draw the same record two ways. */
void pv_taps(float frac, float taps[PV_TAPS])
{
    sinc_interp_taps(frac, taps);
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
    /* The 16 taps' samples, clamped to the record's own ends (never a guard
     * byte past it), copied once so the kernel reads plain memory. */
    uint8_t w[PV_TAPS];
    for (int32_t k = 0; k < (int32_t)PV_TAPS; k++) {
        int32_t i = n - SINC_INTERP_CENTER + k;
        if (i < lo) i = lo;
        if (i >= hi) i = hi - 1;
        w[k] = rec[i];
    }
    *out = sinc_interp_frac(w, (int32_t)PV_TAPS, SINC_INTERP_CENTER,
                            pos_x1000 - n * 1000);
    return true;
}

void pv_fill(const volatile uint8_t *rec, int32_t lo, int32_t hi,
             int32_t pos0_x1000, uint32_t ratio_x1000,
             uint8_t *out, uint16_t ncols, uint16_t *first, uint16_t *end)
{
    uint16_t f = ncols, e = 0;
    for (uint16_t x = 0; x < ncols; x++) {
        const int32_t pos = pos0_x1000 + (int32_t)x * (int32_t)ratio_x1000;
        if (pv_sample(rec, lo, hi, pos, ratio_x1000, &out[x])) {
            if (f == ncols) f = x;
            e = (uint16_t)(x + 1u);
        }
    }
    if (f == ncols) f = e = 0;               /* nothing of the record in view */
    if (first) *first = f;
    if (end)   *end = e;
}

uint8_t pv_view_plan(bool held, float fs_rec, float fs_col, uint8_t L_code,
                     uint32_t *ratio_x1000)
{
    if (L_code == 0u || L_code > PV_PLAN_MAX_L || (PV_PLAN_MAX_L % L_code) != 0u)
        L_code = 1u;
    uint32_t r = (uint32_t)PV_RATIO_ONE / L_code;
    uint8_t L = L_code;
    if (held && fs_rec > 0.0f && fs_col > 0.0f) {
        r = pv_ratio_x1000(fs_rec, fs_col);
        L = 0u;
        if (r <= PV_RATIO_ONE && (PV_RATIO_ONE % r) == 0u) {
            const uint32_t l = PV_RATIO_ONE / r;
            if (l <= PV_PLAN_MAX_L && (PV_PLAN_MAX_L % l) == 0u)
                L = (uint8_t)l;
        }
    }
    if (ratio_x1000) *ratio_x1000 = r;
    return L;
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
