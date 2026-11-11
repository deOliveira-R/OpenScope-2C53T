/* See scope_geom.h. */
#include "scope_geom.h"
#include "sinc_interp.h"

static inline int32_t floor_div(int32_t a, int32_t b)
{
    int32_t q = a / b;
    if ((a % b) != 0 && ((a < 0) != (b < 0))) q--;
    return q;
}

/* A zoom the table cannot phase (0, or one that does not divide 20) is
 * drawn at one column per sample rather than with wrong phases. */
static inline uint8_t valid_L(uint8_t L)
{
    return (L == 0u || L > SCOPE_GEOM_MAX_L || (SINC_INTERP_PHASES % L) != 0u) ? 1u : L;
}

static inline uint8_t view_L(const scope_window_t *v)
{
    return valid_L(v->L);
}

void scope_geom_col_src(const scope_window_t *v, int x, int32_t *n, uint8_t *p)
{
    const int32_t L = view_L(v);
    const int32_t u = v->u0 + x;
    const int32_t q = floor_div(u, L);
    *n = q;
    *p = (uint8_t)(u - q * L);
}

int32_t scope_geom_first_sample(const scope_window_t *v)
{
    return floor_div(v->u0, view_L(v));
}

static int clamp_tx(int tx)
{
    if (tx < SCOPE_GEOM_TRIG_COL_MIN) tx = SCOPE_GEOM_TRIG_COL_MIN;
    if (tx > SCOPE_GEOM_TRIG_COL_MAX) tx = SCOPE_GEOM_TRIG_COL_MAX;
    return tx;
}

void scope_geom_span(uint8_t L, int tx, int32_t *before, int32_t *after)
{
    L = valid_L(L);
    tx = clamp_tx(tx);
    /* Column 0 is tx/L samples (rounded up) before the anchor, the last
     * column (300 - tx)/L after it (rounded up). */
    *before = (tx + L - 1) / L;
    *after  = ((int)SCOPE_GEOM_COLS - 1 - tx + L - 1) / L;
}

int scope_geom_fit(scope_window_t *v, uint8_t L, int32_t anchor, int32_t back_q8,
                   int tx, int32_t lo, int32_t n_rec)
{
    L = valid_L(L);
    tx = clamp_tx(tx);
    if (back_q8 < 0)   back_q8 = 0;
    if (back_q8 > 256) back_q8 = 256;

    /* The anchor's position in 1/L samples. */
    int32_t ua = anchor * (int32_t)L;
    if (L > 1u)
        ua -= (back_q8 * (int32_t)L + 128) >> 8;

    int32_t u0 = ua - tx;
    const int32_t u_min = lo * (int32_t)L;
    const int32_t u_max = (n_rec - 1) * (int32_t)L - ((int32_t)SCOPE_GEOM_COLS - 1);
    if (u0 > u_max) u0 = u_max;
    if (u0 < u_min) u0 = u_min;      /* a record shorter than the window: left edge wins */

    v->L  = L;
    v->u0 = u0;
    return (int)(ua - u0);
}

int32_t scope_geom_back_q8(int prev, int cur, int level)
{
    const int32_t den = (int32_t)cur - (int32_t)prev;
    if (den == 0)
        return 0;
    int32_t num = ((int32_t)cur - (int32_t)level) * 256;
    if (den < 0) { num = -num; }
    const int32_t d = (den < 0) ? -den : den;
    int32_t q = (num + d / 2) / d;
    if (q < 0)   q = 0;
    if (q > 256) q = 256;
    return q;
}

void scope_geom_fill(const volatile uint8_t *rec, int32_t n_rec,
                     const scope_window_t *v, uint8_t *out, uint16_t ncols)
{
    if (out == 0 || ncols == 0u)
        return;
    if (rec == 0 || n_rec <= 0) {
        for (uint16_t x = 0; x < ncols; x++) out[x] = 0u;
        return;
    }
    const uint8_t L = view_L(v);

    if (L == 1u) {
        for (uint16_t x = 0; x < ncols; x++) {
            int32_t i = v->u0 + x;
            if (i < 0) i = 0;
            if (i >= n_rec) i = n_rec - 1;
            out[x] = rec[i];
        }
        return;
    }

    /* Source window: the samples of the first and last column plus the
     * taps either side (7 before, 8 after). L >= 2 here, so this is at most
     * 300/2 + 1 + 16 = 167 bytes. */
    enum { SRC_MAX = (SCOPE_GEOM_COLS - 1u) / 2u + 1u + SINC_INTERP_TAPS + 1u };
    uint8_t src[SRC_MAX];
    int32_t n_first, n_last;
    uint8_t p;
    scope_geom_col_src(v, 0, &n_first, &p);
    scope_geom_col_src(v, (int)ncols - 1, &n_last, &p);
    const int32_t s0 = n_first - SINC_INTERP_CENTER;
    int32_t ns = (n_last + (int32_t)SINC_INTERP_TAPS - SINC_INTERP_CENTER) - s0;
    if (ns > (int32_t)SRC_MAX) ns = (int32_t)SRC_MAX;   /* ncols > 301: tail clamps */
    for (int32_t k = 0; k < ns; k++) {
        int32_t i = s0 + k;
        if (i < 0) i = 0;
        if (i >= n_rec) i = n_rec - 1;
        src[k] = rec[i];
    }

    const uint8_t step = (uint8_t)(SINC_INTERP_PHASES / L);   /* L divides 20 */
    for (uint16_t x = 0; x < ncols; x++) {
        int32_t n;
        scope_geom_col_src(v, x, &n, &p);
        out[x] = sinc_interp_at(src, ns, n - s0, (uint8_t)(p * step));
    }
}
