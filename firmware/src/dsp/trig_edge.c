/* See trig_edge.h. Integer-only. */
#include "trig_edge.h"

#define TRIG_POS   ((int)TRIG_EDGE_TRIG_POS)

int trig_edge_anchor_known(const volatile uint8_t *rec, rec_code_t crossing, int rising)
{
    if (rec == 0)
        return -1;
    const int c = (int)crossing.v;
    for (int i = TRIG_POS; i >= TRIG_POS - 1; i--) {
        const int p = (int)rec[i - 1], s = (int)rec[i];
        if (rising ? (p < c && s >= c) : (p >= c && s < c))
            return i;
    }
    return -1;
}

int trig_edge_anchor(const volatile uint8_t *rec, rec_code_t crossing, int rising)
{
    const int c = (int)crossing.v;
    if (rec == 0 || c < 4 || c > 251)
        return -1;
    const int hyst = 3;
    int best = -1;
    int armed = 0;
    for (int i = TRIG_POS - 64; i <= TRIG_POS + 48; i++) {
        int s = (int)rec[i];
        if (!armed) {
            if (rising ? (s <= c - hyst) : (s >= c + hyst)) armed = 1;
        } else if (rising ? (s >= c) : (s <= c)) {
            if (i >= TRIG_POS - 48) {
                int d = i - TRIG_POS; if (d < 0) d = -d;
                int db = best - TRIG_POS; if (db < 0) db = -db;
                if (best < 0 || d < db) best = i;
            }
            armed = 0;                            /* re-arm for the next one */
        }
    }
    return best;
}

/* See trig_edge.h (parity C3). One pass; the arm flag is the only state.
 * "Past the level" is s >= T rising, s <= T falling: the hardware's own
 * compare (the committed records fire ON the code, tests [9]) and the old
 * soft trigger's. */
int trig_edge_search(const volatile uint8_t *rec, int n, int lo, int hi,
                     int level, int rising, unsigned hyst, unsigned confirm)
{
    if (rec == 0 || n < 3)
        return -1;
    if (lo < 1) lo = 1;
    if (hi > n - (int)confirm) hi = n - (int)confirm;
    const int T = level;
    const int arm_at = rising ? T - (int)hyst : T + (int)hyst;
    /* Armed by a sample on the far side before the window too, so a window
     * that opens just under the level can still take its first crossing --
     * stock's test looks back one sample, and so does this. */
    int armed = rising ? ((int)rec[lo - 1] <= arm_at) : ((int)rec[lo - 1] >= arm_at);
    for (int i = lo; i < hi; i++) {
        const int s = (int)rec[i];
        const int p = (int)rec[i - 1];
        if (armed && (rising ? (p < T && s >= T) : (p > T && s <= T))) {
            unsigned k = 1;
            for (; k <= confirm; k++) {
                int q = (int)rec[i + (int)k];
                if (rising ? (q < T) : (q > T)) break;
            }
            if (k > confirm)
                return i;
            /* A spike: stay armed. Disarming here (needing the far side
             * again) loses the edge itself on a slow noisy ramp -- the
             * chatter right after the first crossing fails the confirm and
             * the ramp never comes back down: [8] found half the edges
             * missing that way. */
        }
        if (rising ? (s <= arm_at) : (s >= arm_at))
            armed = 1;
    }
    return -1;
}

/* Literal S3: rising s[i-1] <= L < s[i] (&& s[i+1] > L), falling mirrored. */
int trig_edge_search_stock(const volatile uint8_t *rec, int n, int lo, int hi,
                           int level, int rising, unsigned confirm)
{
    if (rec == 0 || n < 3)
        return -1;
    if (lo < 1) lo = 1;
    if (hi > n - (int)confirm) hi = n - (int)confirm;
    const int L = level;
    for (int i = lo; i < hi; i++) {
        const int a = (int)rec[i - 1], b = (int)rec[i];
        if (rising) {
            if (a <= L && L < b && (confirm == 0 || (int)rec[i + 1] > L)) return i;
        } else {
            if (a >= L && L > b && (confirm == 0 || (int)rec[i + 1] < L)) return i;
        }
    }
    return -1;
}

static void rev_bytes(volatile uint8_t *a, uint32_t lo, uint32_t hi)
{
    /* reverse a[lo..hi) */
    while (hi > lo + 1u) {
        hi--;
        const uint8_t t = a[lo];
        a[lo] = a[hi];
        a[hi] = t;
        lo++;
    }
}

void trig_edge_rotate_left(volatile uint8_t *rec, uint32_t n, uint32_t k)
{
    if (n == 0u) return;
    k %= n;
    if (k == 0u) return;
    rev_bytes(rec, 0u, k);
    rev_bytes(rec, k, n);
    rev_bytes(rec, 0u, n);
}
