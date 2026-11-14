/*
 * Zoom while paused (parity C14): the pure arithmetic in src/dsp/paused_view.c.
 *
 * [1] ratio: stock's formula against the live dumps (1000 at tb 5/mask 3,
 *     tb 3/mask 1, tb 4/mask 3) and the computed ones (100, 50, 500); the
 *     rate form (OpenScope's one-sample-per-column geometry) agrees.
 * [2] y map: identity, the k = V_cap/V_now orientation, clamp.
 * [3] taps: DC gain, phase-0 identity, symmetry of the half-sample set, and the
 *     comparison with stock's half-sample table.
 * [4] sampler: ratio 1000 is a pure index map; interpolation reproduces a
 *     slow sine; out-of-record columns are refused (stock leaves them blank).
 * [5] the one window plan (C5 x C14 merge): running keeps the fast-code zoom,
 *     a held record whose ratio is 1000/L stays on the C5 integer window,
 *     anything else takes the general path; the column rate it implies.
 * [6] the two fills agree: pv_fill at ratio 1000/L over a window inside the
 *     record is scope_geom_fill byte for byte, for every zoom stock uses, and
 *     the blank span of a zoomed-out / edge window is reported exactly.
 */
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "paused_view.h"
#include "scope_geom.h"
#include <stdlib.h>

static int fails = 0;
#define CHECK(cond, ...) do { if (cond) { printf("  PASS: "); } else { printf("  FAIL: "); fails++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

int main(void)
{
    printf("[1] ratio\n");
    /* record periods (0.4 ns) at tb 0..5: 10,10,10,10,20,50 (0x080465E0) */
    CHECK(pv_stock_ratio_x1000(5, 50, false) == 1000, "tb 5 mask 3 (no x2 at tb >= 4) -> 1000 (dump)");
    CHECK(pv_stock_ratio_x1000(3, 10, false) == 1000, "tb 3 one channel -> 1000 (dump)");
    CHECK(pv_stock_ratio_x1000(4, 20, false) == 1000, "tb 4 mask 3 (no x2 at tb >= 4) -> 1000 (dump)");
    CHECK(pv_stock_ratio_x1000(0, 10, false) == 100,  "tb 0 one channel -> 100 (factor 10)");
    CHECK(pv_stock_ratio_x1000(0, 10, true)  == 50,   "tb 0 mask 3 -> 50 (factor 20)");
    CHECK(pv_stock_ratio_x1000(3, 10, true)  == 500,  "tb 3 mask 3 -> 500 (factor 2)");
    CHECK(pv_stock_ratio_x1000(1, 10, false) == 200,  "tb 1 one channel -> 200 (factor 5)");
    CHECK(pv_stock_ratio_x1000(2, 10, false) == 500,  "tb 2 one channel -> 500 (factor 2)");
    /* rate form: same code -> 1000; 2x the rate now -> zoom in x2 (500) */
    CHECK(pv_ratio_x1000(125000.0f, 125000.0f) == 1000, "same rate -> 1000");
    CHECK(pv_ratio_x1000(125000.0f, 250000.0f) == 500,  "now twice as fast -> 500 (zoom in)");
    CHECK(pv_ratio_x1000(125000.0f, 25000.0f)  == 5000, "now 5x slower -> 5000 (zoom out)");
    CHECK(pv_ratio_x1000(0.0f, 25000.0f) == 1000 && pv_ratio_x1000(25000.0f, 0.0f) == 1000,
          "unknown rate -> no zoom");
    CHECK(pv_ratio_x1000(1.0f, 1e9f) == PV_RATIO_MIN, "clamped at 100x in");

    printf("[2] y map\n");
    CHECK(pv_y_map(150, 128, 0, 0, 1.0f, 1.0f, 0, 255) == 150, "same range/pos -> identity");
    CHECK(pv_y_map(150, 128, 10, 10, 2.0f, 2.0f, 0, 255) == 150, "same range, any pos -> identity");
    /* range 2x larger now (V_now = 2 V_cap): trace halves about the centre */
    CHECK(pv_y_map(168, 128, 0, 0, 1.0f, 2.0f, 0, 255) == 148, "range up 2x: offset 40 -> 20");
    CHECK(pv_y_map(168, 128, 0, 0, 2.0f, 1.0f, 0, 255) == 208, "range down 2x: offset 40 -> 80");
    CHECK(pv_y_map(168, 128, 0, 5, 1.0f, 1.0f, 0, 255) == 173, "position moves it by pos_now - pos_cap");
    CHECK(pv_y_map(250, 128, 0, 0, 4.0f, 1.0f, 28, 228) == 228, "clamp high");
    CHECK(pv_y_map(10, 128, 0, 0, 4.0f, 1.0f, 28, 228) == 28, "clamp low");
    CHECK(pv_y_map(170, 128, 0, 0, 0.0f, 1.0f, 0, 255) == 170, "uncalibrated range -> k = 1");
    /* scope_ui's held-record re-draw: centre 0, the 0 V codes as positions,
     * k = volts/count at capture over now. Every code keeps its volts at the
     * new range and position (Vp-p 3.94 V at 2 V/div must stay 3.94 V at
     * 5 V/div), to the rounding of one count. */
    {
        const float kc = 0.0775f, kn = 0.194f;       /* r7 -> r8, unit #1 */
        const int zc = 60, zn = 100;                 /* position -40 -> 0 */
        int bad = 0;
        for (int v = 0; v < 228; v++) {
            const int w = pv_y_map(v, 0, zc, zn, kc, kn, 0, 255);
            const float vc = (float)(v - zc) * kc, vn = (float)(w - zn) * kn;
            if (fabsf(vc - vn) > 0.5f * kn + 1e-4f) bad++;
        }
        CHECK(bad == 0, "held re-draw keeps every code's volts (range and position changed)");
        CHECK(pv_y_map(zc, 0, zc, zn, kc, kn, 0, 255) == zn, "the capture's 0 V lands on today's 0 V");
    }

    printf("[3] taps\n");
    {
        float t[PV_TAPS]; double mx = 0;
        for (int p = 0; p < 64; p++) {
            pv_taps(p / 64.0f, t);
            double s = 0; for (int k = 0; k < 16; k++) s += t[k];
            if (fabs(s - 1.0) > mx) mx = fabs(s - 1.0);
        }
        CHECK(mx < 1e-5, "DC gain 1 at every phase (worst %.2e)", mx);
        pv_taps(0.0f, t);
        CHECK(t[7] == 1.0f && t[8] == 0.0f && t[6] == 0.0f, "phase 0 is the identity");
        pv_taps(0.5f, t);
        double asym = 0; for (int k = 0; k < 8; k++) { double d = fabs(t[7 - k] - t[8 + k]); if (d > asym) asym = d; }
        CHECK(asym < 1e-5, "half-sample set is symmetric (worst %.2e)", asym);
        /* stock's half-sample set (Q15), 0x080466E0 set 1 */
        static const int stock[16] = { -112, 189, -424, 883, -1683, 3105, -6197, 20469,
                                       20861, -6567, 3436, -1957, 1093, -564, 261, -125 };
        double worst = 0;
        for (int k = 0; k < 16; k++) { double d = fabs(t[k] * 32768.0 - stock[k]); if (d > worst) worst = d; }
        CHECK(worst < 500.0, "within %.0f LSB of stock's skewed half-sample taps (<500 = 1.5%%)", worst);
        CHECK(t[7] > 0.6f && t[8] > 0.6f, "centre taps carry the weight");
        /* impulse response: sampling a unit impulse reproduces the taps */
        uint8_t rec[40] = {0}; rec[20] = 200;
        uint8_t out; int ok = 1; double e = 0;
        for (int k = 0; k < 16; k++) {
            /* position between in[n+7] and in[n+8] with n = 20-7+(.. ) */
            int n = 27 - k;                                   /* impulse sits at tap k */
            ok &= pv_sample(rec, 0, 40, n * 1000 + 500, 500, &out);
            double want = 200.0 * t[k];
            if (want < 0) want = 0;
            e = fmax(e, fabs(out - want));
        }
        CHECK(ok && e <= 1.0, "impulse response == taps (worst %.2f counts)", e);
    }

    printf("[4] sampler\n");
    {
        uint8_t rec[1024]; volatile uint8_t *r = rec;
        for (int i = 0; i < 1024; i++) rec[i] = (uint8_t)(i * 7);
        uint8_t o;
        int32_t pos = pv_src_pos_x1000(10, 160, 500, 1000);
        CHECK(pos == (500 - 150) * 1000, "pivot map: col 10 of pivot 160@500 -> index 350");
        CHECK(pv_sample(r, 0, 1024, pos, 1000, &o) && o == rec[350], "ratio 1000 is the plain index");
        CHECK(pv_sample(r, 0, 1024, 350400, 1000, &o) && o == rec[350], "rounds to nearest (350.4)");
        CHECK(pv_sample(r, 0, 1024, 350600, 1000, &o) && o == rec[351], "rounds to nearest (350.6)");
        CHECK(!pv_sample(r, 0, 1024, -1000, 1000, &o) && o == 0, "before the record is refused");
        CHECK(!pv_sample(r, 0, 1024, 1024000, 1000, &o), "past the record is refused");
        CHECK(pv_sample(r + 100, -100, 924, -50000, 1000, &o) && o == rec[50], "offset pointer, negative lo");
        /* a slow sine (30 samples/period) interpolated 5x */
        uint8_t s[1024];
        for (int i = 0; i < 1024; i++) s[i] = (uint8_t)lrint(128 + 100 * sin(2 * 3.14159265358979 * i / 30.0));
        double err = 0; int cnt = 0;
        for (int i = 100; i < 140; i++)
            for (int ph = 0; ph < 5; ph++) {
                double t = i + ph / 5.0;
                uint8_t v;
                if (!pv_sample(s, 0, 1024, (int32_t)lrint(t * 1000), 200, &v)) { err = 999; continue; }
                err = fmax(err, fabs(v - (128 + 100 * sin(2 * 3.14159265358979 * t / 30.0))));
                cnt++;
            }
        CHECK(cnt == 200 && err < 2.5, "5x interpolation of a slow sine, worst error %.2f counts", err);
        CHECK(pv_sample(s, 0, 1024, 0, 200, &o), "first sample accepted at ratio < 1000 (taps clamp)");
        CHECK(!pv_sample(s, 0, 1024, -1, 200, &o), "position before sample 0 refused");
    }
    {   /* The right-hand taps clamp to the LAST sample, never past it: a flat
         * record followed by a hostile guard byte must interpolate flat. */
        uint8_t g[64];
        for (int i = 0; i < 64; i++) g[i] = (i < 32) ? 90u : 255u;
        uint8_t v = 0;
        bool ok = pv_sample(g, 0, 32, 31 * 1000 + 500, 200, &v);
        CHECK(ok && v == 90u, "taps past the record end clamp to rec[hi-1] (got %u)", (unsigned)v);
    }

    printf("[5] window plan\n");
    {
        const float adc = 124.8e6f, mrg = 249.6e6f;        /* top rates, scope_timebase */
        uint32_t r = 0;
        CHECK(pv_view_plan(false, mrg, 0.0f, 10, &r) == 10 && r == 100,
              "running, 0x00 merged: the C5 zoom 10, R 100");
        CHECK(pv_view_plan(false, mrg, mrg, 1, &r) == 1 && r == 1000, "running, slow code: L 1");
        CHECK(pv_view_plan(true, mrg, mrg * 10.0f, 10, &r) == 10 && r == 100,
              "STOP at 0x00 merged, untouched: still the C5 window (L 10)");
        CHECK(pv_view_plan(true, mrg, mrg * 2.0f, 2, &r) == 2 && r == 500,
              "held 0x00 record (merged), now 0x02: L 2 on the integer window");
        CHECK(pv_view_plan(true, adc, mrg * 10.0f, 10, &r) == 20 && r == 50,
              "held per-converter record, now merged 0x00: L 20 (the record's own zoom)");
        CHECK(pv_view_plan(true, mrg, adc * 1.0f, 1, &r) == 0 && r == 2000,
              "held 0x03 merged, now 0x04 (half the column rate): general, R 2000");
        CHECK(pv_view_plan(true, 50e6f, 125e6f, 1, &r) == 0 && r == 400,
              "held 0x05, now 0x04: R 400 is no divisor zoom -> general");
        CHECK(pv_view_plan(true, 1e6f, 1e9f, 1, &r) == 0 && r == PV_RATIO_MIN,
              "a 1000x zoom clamps at 100x (R 10): general (100 does not divide 20)");
        CHECK(pv_view_plan(true, 125e6f, 125e6f, 1, &r) == 1 && r == 1000, "held, same rate: L 1");
        CHECK(pv_view_plan(true, 0.0f, 125e6f, 4, &r) == 4 && r == 250,
              "held but the record rate unknown: the code's own zoom");
        CHECK(pv_view_plan(false, 1.0f, 1.0f, 3, &r) == 1 && r == 1000,
              "a zoom the table cannot phase (3) draws at L 1");
        /* the cursors' column rate is 1000 fs_rec / R: running 0x00 = fs x 10 */
        pv_view_plan(false, mrg, 0.0f, 10, &r);
        CHECK(fabsf(1000.0f * mrg / (float)r - mrg * 10.0f) < 1.0f, "column rate = record rate x L");
    }

    printf("[6] fills agree\n");
    {
        enum { N = 1024 };
        static uint8_t rec[N];
        uint8_t a[SCOPE_GEOM_COLS], b[SCOPE_GEOM_COLS];
        srand(7);
        for (int i = 0; i < N; i++) rec[i] = (uint8_t)(rand() & 0xFF);
        static const uint8_t Ls[6] = { 1, 2, 4, 5, 10, 20 };
        int bad = 0, spans = 0;
        for (int li = 0; li < 6; li++) {
            const int L = Ls[li];
            const int32_t umax = (N - 1) * L - (int32_t)(SCOPE_GEOM_COLS - 1);
            for (int32_t u0 = 0; u0 <= umax; u0 += (umax / 37) + 1) {
                scope_window_t v = { u0, (uint8_t)L };
                scope_geom_fill(rec, N, &v, a, SCOPE_GEOM_COLS);
                uint16_t f = 9, e = 9;
                pv_fill(rec, 0, N, u0 * (1000 / L), (uint32_t)(1000 / L), b,
                        SCOPE_GEOM_COLS, &f, &e);
                if (memcmp(a, b, sizeof a) != 0) bad++;
                if (f != 0 || e != SCOPE_GEOM_COLS) spans++;
            }
        }
        CHECK(bad == 0, "pv_fill(1000/L) == scope_geom_fill, every L, %d windows differ", bad);
        CHECK(spans == 0, "an in-record window has no blank column (%d wrong spans)", spans);
        /* zoomed out 4x about sample 100 at column 150: columns left of
         * 150 - 100/4 = 125 fall before the record */
        uint16_t f = 0, e = 0;
        pv_fill(rec, 0, N, 100 * 1000 - 150 * 4000, 4000, b, SCOPE_GEOM_COLS, &f, &e);
        CHECK(f == 125 && e == SCOPE_GEOM_COLS, "zoom out: blank prefix to column %u (want 125)", f);
        CHECK(b[0] == 0 && b[125] == rec[0] && b[150] == rec[100], "blank reads 0, column 150 = sample 100");
        /* zoomed in 10x near the end: the record ends at column 150 + 10 x 3 */
        pv_fill(rec, 0, N, 1020 * 1000 - 150 * 100, 100, b, SCOPE_GEOM_COLS, &f, &e);
        CHECK(f == 0 && e == 150 + 40, "zoom in: last shown column %u (want 190: n = 1023 up to frac .9)", e);
        pv_fill(rec, 0, N, 5000000, 1000, b, SCOPE_GEOM_COLS, &f, &e);
        CHECK(f == 0 && e == 0, "nothing in view -> empty span");
    }

    printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
