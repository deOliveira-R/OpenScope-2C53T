/*
 * Host tests for the fast-timebase sin(x)/x interpolator (parity C5,
 * src/dsp/sinc_interp.h).
 *
 * What is checked, and why each is the property that matters on the glass:
 *   [1] the committed table IS the documented design: regenerated here in
 *       double precision (sinc x Kaiser beta 5, 16 taps, d = k/20) and
 *       quantised the same way, compared to within 1 LSB; every phase sums to exactly 32768 (a DC
 *       level is drawn where it is) and phase 0 is the identity;
 *   [2] phase k and phase 20-k are mirror images (no lead/lag bias between
 *       the left and right halves of a sample interval);
 *   [3] frequency response of every phase: |H| within 0.5 % of 1 up to
 *       0.4 x fs, so a 50 MHz sine on a 125 MS/s record is drawn at its own
 *       amplitude;
 *   [4] end to end on synthetic records: interpolated sines at f/fs up to
 *       0.4 land within a few counts of the true waveform at the output
 *       times, for every zoom stock uses (2, 4, 5, 10, 20);
 *   [5] a sampled step overshoots by no more than the IDEAL (unwindowed,
 *       infinitely long) sin(x)/x reconstruction's own Gibbs overshoot,
 *       computed here; the window may only reduce it;
 *   [6] the output saturates instead of wrapping (a full-scale step's
 *       overshoot is 255, never a small number);
 *   [7] edges repeat the edge sample: a constant record stays constant at
 *       every phase at both ends (and nothing is read out of bounds);
 *   [8] loose sanity against stock's decoded half-sample row (0x080466E0,
 *       docs/re/parity2_static.md s3.6): same family, within 300 LSB a tap;
 *   [9] any phase (sinc_interp_frac, the zoom while paused, C14): bit-exact
 *       sinc_interp_at() on the 20 rows, the blended taps within a hair of
 *       the design evaluated at the true phase, unity DC gain, and sines up
 *       to 0.4 fs at 1/1000-sample times within a count of the double-
 *       precision design.
 */
#include "../src/dsp/sinc_interp.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
            failures++;                                                       \
        }                                                                     \
    } while (0)

#define NT ((int)SINC_INTERP_TAPS)
#define NP ((int)SINC_INTERP_PHASES)

static double i0(double x)
{
    double s = 1.0, t = 1.0;
    for (int k = 1; k < 50; k++) {
        t *= (x / 2.0 / k) * (x / 2.0 / k);
        s += t;
    }
    return s;
}

static double sincd(double x)
{
    return fabs(x) < 1e-12 ? 1.0 : sin(M_PI * x) / (M_PI * x);
}

/* The design, in double: tap j at t = j - 7 - d, normalised to unit sum. */
static void ref_taps(int k, double h[NT])
{
    const double d = (double)k / NP;
    double s = 0.0;
    for (int j = 0; j < NT; j++) {
        const double t = j - 7 - d;
        const double u = 1.0 - (t / 8.0) * (t / 8.0);
        h[j] = sincd(t) * i0(5.0 * sqrt(u > 0.0 ? u : 0.0)) / i0(5.0);
        s += h[j];
    }
    for (int j = 0; j < NT; j++) h[j] /= s;
}

static void test_table_is_the_design(void)
{
    for (int k = 0; k < NP; k++) {
        const int32_t *row = sinc_interp_row((uint8_t)k);
        double h[NT];
        ref_taps(k, h);
        /* Quantise as the generator does: round each tap, then put the
         * rounding residue on the largest tap so the phase sums to 32768. */
        long q[NT], qs = 0;
        int big = 0;
        for (int j = 0; j < NT; j++) {
            q[j] = lround(h[j] * 32768.0);
            qs += q[j];
            if (fabs(h[j]) > fabs(h[big])) big = j;
        }
        q[big] += 32768 - qs;
        long sum = 0;
        int worst = 0;
        for (int j = 0; j < NT; j++) {
            sum += row[j];
            int e = (int)q[j] - (int)row[j];
            if (e < 0) e = -e;
            if (e > worst) worst = e;
        }
        CHECK(sum == 32768, "[1] phase %d sums to %ld, not 32768", k, sum);
        CHECK(worst <= 1, "[1] phase %d differs from the design by %d LSB", k, worst);
    }
    const int32_t *r0 = sinc_interp_row(0);
    for (int j = 0; j < NT; j++)
        CHECK(r0[j] == (j == SINC_INTERP_CENTER ? 32768 : 0), "[1] phase 0 tap %d not identity", j);
    CHECK(sinc_interp_row(NP) == sinc_interp_row(0), "[1] phase 20 must wrap to 0");
}

static void test_mirror(void)
{
    for (int k = 1; k < NP; k++) {
        const int32_t *a = sinc_interp_row((uint8_t)k);
        const int32_t *b = sinc_interp_row((uint8_t)(NP - k));
        for (int j = 0; j < NT; j++)
            CHECK(a[j] == b[NT - 1 - j], "[2] phase %d tap %d is not the mirror of phase %d", k, j, NP - k);
    }
}

static void test_frequency_response(void)
{
    double worst = 0.0;
    int wk = -1;
    double wf = 0.0;
    for (int k = 1; k < NP; k++) {
        const int32_t *h = sinc_interp_row((uint8_t)k);
        const double d = (double)k / NP;
        for (double f = 0.0; f <= 0.4001; f += 0.01) {
            double re = 0.0, im = 0.0;
            for (int j = 0; j < NT; j++) {
                const double ph = -2.0 * M_PI * f * (j - 7 - d);
                re += h[j] / 32768.0 * cos(ph);
                im += h[j] / 32768.0 * sin(ph);
            }
            const double e = fabs(sqrt(re * re + im * im) - 1.0);
            if (e > worst) { worst = e; wk = k; wf = f; }
        }
    }
    CHECK(worst <= 0.005, "[3] |H| off by %.4f at phase %d, f/fs %.2f (limit 0.5 %%)", worst, wk, wf);
    printf("  [3] worst passband error 0..0.4 fs: %.3f %% (phase %d, f/fs %.2f)\n",
           worst * 100.0, wk, wf);
}

static void test_sines_end_to_end(void)
{
    enum { N = 256 };
    static const uint8_t Ls[5] = { 2, 4, 5, 10, 20 };
    static const double fs_[5] = { 0.05, 0.1, 0.2, 0.3, 0.4 };
    /* Bound per band: rounding (0.5) + quantised input (0.5) + response. */
    static const double lim[5] = { 1.5, 1.5, 1.5, 2.0, 2.5 };
    uint8_t rec[N];
    for (int fi = 0; fi < 5; fi++) {
        const double f = fs_[fi];
        double worst = 0.0;
        for (int li = 0; li < 5; li++) {
            const int L = Ls[li];
            for (double ph0 = 0.0; ph0 < 6.28; ph0 += 0.7) {
                for (int i = 0; i < N; i++)
                    rec[i] = (uint8_t)lround(128.0 + 90.0 * sin(2.0 * M_PI * f * i + ph0));
                /* Away from the ends: the edge rule repeats samples there. */
                for (int n = 20; n < N - 20; n++) {
                    for (int p = 0; p < L; p++) {
                        const double t = n + (double)p / L;
                        const double want = 128.0 + 90.0 * sin(2.0 * M_PI * f * t + ph0);
                        const uint8_t got = sinc_interp_at(rec, N, n, (uint8_t)(p * (NP / L)));
                        const double e = fabs((double)got - want);
                        if (e > worst) worst = e;
                    }
                }
            }
        }
        CHECK(worst <= lim[fi], "[4] f/fs %.2f: worst error %.2f counts (limit %.1f)", f, worst, lim[fi]);
        printf("  [4] f/fs %.2f: worst error %.2f counts of a 90-count sine\n", f, worst);
    }
}

/* Ideal sin(x)/x reconstruction of a unit step sampled at n >= 0: the peak
 * of sum_{n>=0} sinc(t - n), with the tail summed in closed form. */
static double ideal_step_peak(void)
{
    double best = 0.0;
    for (int i = 0; i <= 200; i++) {
        const double t = i / 100.0;           /* the peak sits in 0 < t < 2 */
        double v = 0.0;
        for (int n = 0; n < 200000; n++) v += sincd(t - n);
        if (v > best) best = v;
    }
    return best;
}

static void test_step_overshoot(void)
{
    enum { N = 64, LO = 40, HI = 200 };
    uint8_t rec[N];
    for (int i = 0; i < N; i++) rec[i] = (i < N / 2) ? LO : HI;
    int mx = 0, mn = 255;
    for (int n = 4; n < N - 4; n++) {
        for (int p = 0; p < NP; p++) {
            const int v = sinc_interp_at(rec, N, n, (uint8_t)p);
            if (v > mx) mx = v;
            if (v < mn) mn = v;
        }
    }
    const double over = (double)(mx - HI) / (HI - LO);
    const double under = (double)(LO - mn) / (HI - LO);
    const double ideal = ideal_step_peak() - 1.0;
    CHECK(ideal > 0.13 && ideal < 0.15, "[5] ideal sinc overshoot computed as %.4f (expect ~0.141)", ideal);
    CHECK(over <= ideal + 1.0 / (HI - LO),
          "[5] step overshoot %.4f exceeds the ideal sin(x)/x's own %.4f", over, ideal);
    CHECK(under <= ideal + 1.0 / (HI - LO),
          "[5] step undershoot %.4f exceeds the ideal sin(x)/x's own %.4f", under, ideal);
    CHECK(over > 0.0, "[5] a band-limited step must ring a little (got %.4f): is this still sin(x)/x?", over);
    printf("  [5] step overshoot %.2f %% / undershoot %.2f %% (ideal sin(x)/x %.2f %%)\n",
           over * 100.0, under * 100.0, ideal * 100.0);
}

static void test_saturation(void)
{
    enum { N = 48 };
    uint8_t rec[N];
    for (int i = 0; i < N; i++) rec[i] = (i < N / 2) ? 0u : 255u;
    for (int n = 8; n < N - 8; n++) {
        for (int p = 1; p < NP; p++) {
            const int v = sinc_interp_at(rec, N, n, (uint8_t)p);
            if (n >= N / 2 + 1)
                CHECK(v >= 200, "[6] n %d phase %d: high side read %d (wrapped?)", n, p, v);
            if (n <= N / 2 - 2)
                CHECK(v <= 55, "[6] n %d phase %d: low side read %d (wrapped?)", n, p, v);
        }
    }
}

static void test_edges(void)
{
    enum { N = 10 };
    /* Guard bytes either side: an out-of-bounds read would pick them up. */
    uint8_t buf[N + 32];
    memset(buf, 0xEE, sizeof(buf));
    uint8_t *rec = buf + 16;
    for (int i = 0; i < N; i++) rec[i] = 77u;
    for (int n = -3; n < N + 3; n++)
        for (int p = 0; p < NP; p++)
            CHECK(sinc_interp_at(rec, N, n, (uint8_t)p) == 77u,
                  "[7] constant record not constant at n %d phase %d", n, p);
    CHECK(sinc_interp_at(NULL, N, 0, 3) == 0u, "[7] NULL source must read 0");
    for (int i = 0; i < N; i++) rec[i] = (uint8_t)(i * 20);
    CHECK(sinc_interp_at(rec, N, 4, 0) == 80u, "[7] phase 0 must be the sample itself");
    CHECK(sinc_interp_at(rec, N, 4, 20) == 80u, "[7] phase 20 must reduce to 0");
}

static void test_vs_stock_half_sample(void)
{
    /* 0x080466E0 set 1 (ratio 2, phase 1), parity2_static.md s3.6. */
    static const int stock[16] = { -112, 189, -424, 883, -1683, 3105, -6197, 20469,
                                   20861, -6567, 3436, -1957, 1093, -564, 261, -125 };
    const int32_t *ours = sinc_interp_row(NP / 2);
    int worst = 0;
    for (int j = 0; j < NT; j++) {
        int e = (int)ours[j] - stock[j];
        if (e < 0) e = -e;
        if (e > worst) worst = e;
        CHECK((ours[j] < 0) == (stock[j] < 0), "[8] tap %d sign differs from stock", j);
    }
    CHECK(worst <= 300, "[8] half-sample row %d LSB from stock's (limit 300)", worst);
    printf("  [8] half-sample row within %d LSB of stock's\n", worst);
}

/* The design at a CONTINUOUS phase d (0 <= d < 1). */
static void ref_taps_d(double d, double h[NT])
{
    double s = 0.0;
    for (int j = 0; j < NT; j++) {
        const double t = j - 7 - d;
        const double u = 1.0 - (t / 8.0) * (t / 8.0);
        h[j] = sincd(t) * i0(5.0 * sqrt(u > 0.0 ? u : 0.0)) / i0(5.0);
        s += h[j];
    }
    for (int j = 0; j < NT; j++) h[j] /= s;
}

static void test_any_phase(void)
{
    enum { N = 200 };
    static uint8_t rec[N];
    unsigned seed = 12345u;
    for (int i = 0; i < N; i++) {
        seed = seed * 1103515245u + 12345u;
        rec[i] = (uint8_t)(seed >> 16);
    }
    /* (a) on a row: the table path, bit for bit (noise is the hard case). */
    int diff = 0;
    for (int n = -2; n < N + 2; n++)
        for (int p = 0; p < NP; p++)
            if (sinc_interp_frac(rec, N, n, p * (1000 / NP)) != sinc_interp_at(rec, N, n, (uint8_t)p))
                diff++;
    CHECK(diff == 0, "[9] %d row phases differ from sinc_interp_at", diff);

    /* (b) the blended taps against the design at the true phase. */
    /* The L1 norm of the tap error x 255 bounds the output error for ANY
     * 8-bit input (adversarial noise included); band-limited input does far
     * better, see (c). Linear blending of rows 1/20 apart leaves ~1e-3 on
     * the centre taps (the sinc's curvature x (1/40)^2). */
    double worst = 0.0, wsum = 0.0;
    for (int m = 0; m < 1000; m++) {
        float t[NT];
        double h[NT], sum = 0.0, l1 = 0.0;
        sinc_interp_taps((float)m / 1000.0f, t);
        ref_taps_d(m / 1000.0, h);
        for (int j = 0; j < NT; j++) {
            l1 += fabs(t[j] - h[j]);
            sum += t[j];
        }
        if (l1 > worst) worst = l1;
        if (fabs(sum - 1.0) > wsum) wsum = fabs(sum - 1.0);
    }
    CHECK(wsum < 1e-5, "[9] blended taps sum off 1 by %.2e", wsum);
    CHECK(worst * 255.0 <= 2.0, "[9] blended taps' L1 error %.2e = %.2f counts worst case (limit 2)",
          worst, worst * 255.0);
    printf("  [9] blended taps' L1 error %.2e of the design (%.2f counts on any input)\n",
           worst, worst * 255.0);
    {
        float t[NT];
        sinc_interp_taps(0.0f, t);
        int id = 1;
        for (int j = 0; j < NT; j++) id &= (t[j] == (j == SINC_INTERP_CENTER ? 1.0f : 0.0f));
        CHECK(id, "[9] taps at phase 0 are not the identity");
    }

    /* (c) sines to 0.4 fs at 1/1000-sample times against the design. */
    double we = 0.0;
    for (double f = 0.02; f <= 0.4001; f += 0.02) {
        for (int i = 0; i < N; i++)
            rec[i] = (uint8_t)lround(128.0 + 100.0 * sin(2.0 * M_PI * f * i + 0.3));
        for (int n = 40; n < 60; n++)
            for (int m = 0; m < 1000; m += 7) {
                double h[NT], y = 0.0;
                ref_taps_d(m / 1000.0, h);
                for (int j = 0; j < NT; j++) y += h[j] * rec[n - 7 + j];
                const double e = fabs(sinc_interp_frac(rec, N, n, m) - y);
                if (e > we) we = e;
            }
    }
    CHECK(we <= 1.0, "[9] any-phase output %.2f counts from the design (limit 1)", we);
    CHECK(sinc_interp_frac(NULL, N, 0, 300) == 0u, "[9] NULL source must read 0");
}

int main(void)
{
    test_table_is_the_design();
    test_mirror();
    test_frequency_response();
    test_sines_end_to_end();
    test_step_overshoot();
    test_saturation();
    test_edges();
    test_vs_stock_half_sample();
    test_any_phase();
    if (failures == 0) {
        printf("test_sinc_interp: all checks passed\n");
        return 0;
    }
    printf("test_sinc_interp: %d check(s) FAILED\n", failures);
    return 1;
}
