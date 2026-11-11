/*
 * Host tests for dsp/interleave_cal (250 MS/s converter-B self-calibration).
 *
 *   1. A known B = g A + o across five levels is recovered; the fitted pair,
 *      fed to interleave_correct_b(), maps B back onto A.
 *   2. Identity data gives identity; an inverted sweep order does not matter.
 *   3. Refusals: not flat (p2p and drift), too few usable points (clipped
 *      points are dropped), span too small, gain out of window, poor fit
 *      (a bent B). A refusal never reports OK.
 *   4. The DAC sweep points are the B4 set, direction-aware, and refused for
 *      a short or out-of-range span.
 *   5. Unit #2's bench levels (2026-11-11) fit B = 0.9809 A + 3.89 with ~1
 *      count residuals and give five ascending knots; a level that reads lower
 *      in B than the one below it is refused (not monotonic).
 *   6. Skew estimate from merged tone records: sine and band-limited square,
 *      5..49 MHz, B at 0.451 / 0.5 / 0.6 of a period, 1.5 count noise, the
 *      mean of 32 records (random trigger phase, as the shell takes them)
 *      within 0.003 period at >= 10 MHz (noise bound ~0.0008; 0.003 is a
 *      -62 dBc image at 10 MHz), the same image (0.003 x 10/f) below; refusals (no tone, out of band, A/B mismatch,
 *      noise, range); round trip: merging with the measured skew reads ~0.5.
 */
#include "../src/dsp/interleave_cal.h"
#include "../src/dsp/interleave.h"

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

static void line(il_cal_point_t *p, unsigned n, float g, float o, float a0, float step)
{
    for (unsigned i = 0; i < n; i++) {
        p[i].a_mean = a0 + step * (float)i;
        p[i].b_mean = g * p[i].a_mean + o;
        p[i].a_p2p = p[i].b_p2p = 2;
        p[i].a_drift = p[i].b_drift = 0.3f;
        p[i].clipped = false;
    }
}

static uint32_t rng = 777u;
static double urand(void) { rng = rng * 1664525u + 1013904223u; return ((rng >> 8) + 0.5) / 16777216.0; }
static double grand(void) { return sqrt(-2.0 * log(urand())) * cos(2.0 * M_PI * urand()); }
static uint8_t q8(double v) { return v <= 0 ? 0 : v >= 255 ? 255 : (uint8_t)(v + 0.5); }

/* Merged record (even = A, odd = B) of a tone of f MHz; `square` sums odd
 * harmonics 1..7 (a G431 square through a ~50 MHz front end). */
static double tone_phase = 0.7;
static void tone_merged(uint8_t *m, double f_mhz, double tau, double amp, double b_amp,
                        bool square, double noise)
{
    const double fc = f_mhz / 125.0;
    for (unsigned k = 0; k < 512; k++) {
        for (unsigned ph = 0; ph < 2; ph++) {
            const double t = k + (ph ? tau : 0.0);
            double v = 0;
            for (int h = 1; h <= (square ? 7 : 1); h += 2)
                if (h * f_mhz < 70.0) v += sin(h * (2 * M_PI * fc * t + tone_phase)) / h;
            m[2 * k + ph] = q8(110.0 + (ph ? b_amp : amp) * v + noise * grand());
        }
    }
}

static void test_bench_levels(void)
{
    static const float A[5] = { 9.77f, 49.61f, 98.98f, 148.60f, 199.02f };
    static const float B[5] = { 13.96f, 51.63f, 100.96f, 150.59f, 198.62f };
    il_cal_params_t prm;
    il_cal_default_params(&prm);
    il_cal_point_t p[5];
    for (int i = 0; i < 5; i++) {
        p[4 - i].a_mean = A[i]; p[4 - i].b_mean = B[i];      /* sweep order reversed on purpose */
        p[4 - i].a_p2p = p[4 - i].b_p2p = 12;
        p[4 - i].a_drift = p[4 - i].b_drift = 0.4f;
        p[4 - i].clipped = false;
    }
    il_cal_fit_t f;
    CHECK(il_cal_fit(p, 5, &prm, &f) == IL_CAL_OK, "bench levels refused");
    CHECK(fabsf(f.gain - 0.9809f) < 2e-4f && fabsf(f.offset - 3.89f) < 0.02f,
          "bench fit %.4f %.3f, want 0.9809 3.89", f.gain, f.offset);
    CHECK(f.resid_max > 0.9f && f.resid_max < 1.1f, "bench residual %.2f, want ~1.02", f.resid_max);
    CHECK(f.n_knots == 5, "knots %u", f.n_knots);
    for (int i = 0; i < 5 && f.n_knots == 5; i++)
        CHECK(f.knot_a[i] == (int16_t)lroundf(A[i] * 100) && f.knot_b[i] == (int16_t)lroundf(B[i] * 100),
              "knot %d = (%d, %d)", i, f.knot_a[i], f.knot_b[i]);
    interleave_corr_t c;
    interleave_corr_identity(&c);
    CHECK(interleave_corr_set_knots(&c, f.n_knots, f.knot_a, f.knot_b), "fit knots refused by the map");
    for (int i = 0; i < 5; i++)
        CHECK(fabsf(interleave_map_b(B[i], &c) - A[i]) < 0.01f, "level %d maps to %.3f", i,
              interleave_map_b(B[i], &c));

    /* Not monotonic: two close levels where B reads backwards; the line
     * still fits within its residual gate, the map must not. */
    p[2].a_mean = 100.0f; p[2].b_mean = 102.0f;
    p[1].a_mean = 101.0f; p[1].b_mean = 101.5f;
    CHECK(il_cal_fit(p, 5, &prm, &f) == IL_CAL_NOT_MONOTONIC && f.n_knots == 0,
          "backwards level not refused (%s)", il_cal_status_str(il_cal_fit(p, 5, &prm, &f)));
}

static void test_skew_estimate(void)
{
    uint8_t m[1024];
    il_skew_est_t e;
    static const double fs[] = { 5.5, 10.0, 25.0, 49.0 };
    static const double taus[] = { 0.451, 0.5, 0.6 };
    double worst = 0;
    for (unsigned i = 0; i < 4; i++)
        for (unsigned j = 0; j < 3; j++)
            for (int sq = 0; sq < 2; sq++) {
                double sum = 0;
                for (int r = 0; r < 32; r++) {
                    tone_phase = 2 * M_PI * urand();
                    tone_merged(m, fs[i], taus[j], 70.0, 70.0, sq, 1.5);
                    const il_skew_status_t st = il_skew_estimate(m, &e);
                    CHECK(st == IL_SKEW_OK, "%.1f MHz tau %.3f sq %d: %s", fs[i], taus[j], sq, il_skew_status_str(st));
                    CHECK(fabs(e.cycles * 125.0 - fs[i]) < 0.05, "frequency %.3f MHz, want %.1f", e.cycles * 125.0, fs[i]);
                    sum += e.tau;
                }
                const double err = fabs(sum / 32 - taus[j]);
                if (err > worst) worst = err;
                /* The image an error leaves is ~w * err: equal spur allows err ~ 1/f. */
                const double tol = 0.003 * (fs[i] < 10.0 ? 10.0 / fs[i] : 1.0);
                CHECK(err < tol, "%.1f MHz tau %.3f sq %d: estimated %.4f", fs[i], taus[j], sq, sum / 32);
            }
    printf("  skew estimate: worst error %.5f period over 5.5..49 MHz, sine and square, 32 records, 1.5 count noise\n", worst);

    /* Refusals */
    memset(m, 110, sizeof(m));
    CHECK(il_skew_estimate(m, &e) == IL_SKEW_LOW_AMP || il_skew_estimate(m, &e) == IL_SKEW_FREQ, "DC accepted");
    tone_merged(m, 10.0, 0.451, 5.0, 5.0, false, 0.3);
    CHECK(il_skew_estimate(m, &e) == IL_SKEW_LOW_AMP, "5-count tone accepted");
    tone_merged(m, 2.0, 0.451, 70.0, 70.0, false, 0.5);
    CHECK(il_skew_estimate(m, &e) == IL_SKEW_FREQ, "2 MHz accepted");
    tone_merged(m, 55.0, 0.451, 70.0, 70.0, false, 0.5);
    CHECK(il_skew_estimate(m, &e) == IL_SKEW_FREQ, "55 MHz accepted");
    tone_merged(m, 10.0, 0.451, 70.0, 35.0, false, 0.5);
    CHECK(il_skew_estimate(m, &e) == IL_SKEW_MISMATCH, "half-amplitude B accepted");
    tone_merged(m, 10.0, 0.451, 20.0, 20.0, false, 25.0);
    CHECK(il_skew_estimate(m, &e) == IL_SKEW_NOISY || il_skew_estimate(m, &e) == IL_SKEW_MISMATCH,
          "noise accepted (%s)", il_skew_status_str(il_skew_estimate(m, &e)));
    tone_merged(m, 10.0, 0.8, 70.0, 70.0, false, 0.5);
    CHECK(il_skew_estimate(m, &e) == IL_SKEW_RANGE && fabsf(e.tau - 0.8f) < 0.01f, "tau 0.8 accepted");
    CHECK(il_skew_estimate(NULL, &e) == IL_SKEW_ARGS, "NULL");

    /* Round trip on raw converter records: measure (32 records), correct,
     * measure again. */
    uint8_t a[1024], b[1024];
    double sum = 0, sum2 = 0;
    interleave_corr_t c;
    interleave_corr_identity(&c);
    for (int pass = 0; pass < 2; pass++) {
        double s_tau = 0;
        for (int r = 0; r < 32; r++) {
            const double ph = 2 * M_PI * urand();
            for (unsigned k = 0; k < 1024; k++) {
                a[k] = q8(110 + 70 * sin(2 * M_PI * 0.08 * k + ph) + 1.5 * grand());
                b[k] = q8(110 + 70 * sin(2 * M_PI * 0.08 * (k + 0.451) + ph) + 1.5 * grand());
            }
            interleave_merge(a, b, pass ? &c : NULL);
            CHECK(il_skew_estimate(a, &e) == IL_SKEW_OK, "record refused (pass %d)", pass);
            s_tau += e.tau;
        }
        if (pass == 0) sum = s_tau / 32; else sum2 = s_tau / 32;
        if (pass == 0) CHECK(interleave_skew_set(&c, (int32_t)lround((sum - 0.5) * 1e6)), "measured skew refused");
    }
    const int32_t ppm = c.skew_ppm;
    CHECK(ppm > -52000 && ppm < -46000, "raw skew %ld ppm, want ~-49000", (long)ppm);
    CHECK(fabs(sum2 - 0.5) < 0.003, "corrected records read tau %.4f, want 0.500", sum2);
    e.tau = (float)sum2;
    printf("  round trip: measured %ld ppm, corrected record reads tau %.4f\n", (long)ppm, e.tau);
}

int main(void)
{
    il_cal_params_t prm;
    il_cal_default_params(&prm);
    il_cal_point_t p[5];
    il_cal_fit_t f;

    /* 1: recovery. Numbers are test data, not a unit's. */
    line(p, 5, 0.97f, 4.0f, 40.0f, 40.0f);
    CHECK(il_cal_fit(p, 5, &prm, &f) == IL_CAL_OK, "clean line refused");
    CHECK(fabsf(f.gain - 0.97f) < 1e-4f && fabsf(f.offset - 4.0f) < 1e-2f,
          "fit %f %f", f.gain, f.offset);
    CHECK(f.resid_max < 1e-2f && f.used == 5, "resid %f used %u", f.resid_max, f.used);
    {
        interleave_corr_t c = { .gain = f.gain, .offset = f.offset };
        for (unsigned i = 0; i < 5; i++) {
            const uint8_t b = (uint8_t)(p[i].b_mean + 0.5f);
            const int a = (int)(p[i].a_mean + 0.5f);
            const int d = (int)interleave_correct_b(b, &c) - a;
            CHECK(d >= -1 && d <= 1, "corrected B off A by %d at %u", d, i);
        }
    }
    /* noise inside the tolerance still fits */
    line(p, 5, 1.02f, -3.0f, 50.0f, 35.0f);
    p[1].b_mean += 0.6f; p[3].b_mean -= 0.7f;
    CHECK(il_cal_fit(p, 5, &prm, &f) == IL_CAL_OK, "mild noise refused");
    CHECK(f.resid_max > 0.3f, "residual not reported (%f)", f.resid_max);

    /* 2: identity, reversed order */
    line(p, 5, 1.0f, 0.0f, 200.0f, -35.0f);
    CHECK(il_cal_fit(p, 5, &prm, &f) == IL_CAL_OK && fabsf(f.gain - 1.0f) < 1e-4f &&
          fabsf(f.offset) < 1e-2f, "identity not recovered");

    /* 3: refusals */
    line(p, 5, 0.98f, 4.0f, 40.0f, 40.0f);
    p[2].a_p2p = 60;          /* a real signal: well above the 32-count noise allowance */
    CHECK(il_cal_fit(p, 5, &prm, &f) == IL_CAL_NOT_FLAT, "p2p not refused");
    /* Bench noise (+-5 codes, spread ~16 after glitch trimming) must pass. */
    p[2].a_p2p = 16;
    CHECK(il_cal_fit(p, 5, &prm, &f) != IL_CAL_NOT_FLAT, "honest noise refused as not flat");
    p[2].a_p2p = 60;
    line(p, 5, 0.98f, 4.0f, 40.0f, 40.0f);
    p[4].b_drift = 5.0f;
    CHECK(il_cal_fit(p, 5, &prm, &f) == IL_CAL_NOT_FLAT, "drift not refused");

    line(p, 5, 0.98f, 4.0f, 40.0f, 40.0f);
    p[0].clipped = p[1].clipped = p[4].clipped = true;
    CHECK(il_cal_fit(p, 5, &prm, &f) == IL_CAL_FEW_POINTS && f.used == 2, "few points");
    p[1].clipped = false;                /* three usable (1, 2, 3) */
    CHECK(il_cal_fit(p, 5, &prm, &f) == IL_CAL_OK && f.used == 3, "three usable refused");
    /* a clipped point must not poison the fit */
    p[0].clipped = true; p[0].b_mean = 255.0f; p[0].a_mean = 40.0f;
    CHECK(il_cal_fit(p, 5, &prm, &f) == IL_CAL_OK && fabsf(f.gain - 0.98f) < 1e-3f,
          "clipped point leaked into the fit");

    line(p, 5, 0.98f, 4.0f, 100.0f, 5.0f);   /* span 20 */
    CHECK(il_cal_fit(p, 5, &prm, &f) == IL_CAL_SPAN, "small span not refused");
    line(p, 5, 0.98f, 4.0f, 100.0f, 0.0f);   /* all equal */
    CHECK(il_cal_fit(p, 5, &prm, &f) == IL_CAL_SPAN, "zero span not refused");

    line(p, 5, 2.5f, 0.0f, 20.0f, 20.0f);
    CHECK(il_cal_fit(p, 5, &prm, &f) == IL_CAL_GAIN_RANGE, "gain 2.5 not refused");
    line(p, 5, 0.3f, 0.0f, 40.0f, 40.0f);
    CHECK(il_cal_fit(p, 5, &prm, &f) == IL_CAL_GAIN_RANGE, "gain 0.3 not refused");

    line(p, 5, 0.98f, 4.0f, 40.0f, 40.0f);
    p[2].b_mean += 6.0f;                     /* bent */
    CHECK(il_cal_fit(p, 5, &prm, &f) == IL_CAL_POOR_FIT, "bent B not refused");
    CHECK(f.resid_max > prm.max_resid, "poor-fit residual not reported");

    CHECK(il_cal_fit(NULL, 5, &prm, &f) == IL_CAL_ARGS, "NULL points");
    CHECK(il_cal_fit(p, IL_CAL_MAX_POINTS + 1, &prm, &f) == IL_CAL_ARGS, "too many points");

    /* 4: DAC points */
    uint16_t d[5];
    CHECK(il_cal_dac_points(1000, 3000, d) && d[0] == 1080 && d[1] == 1400 &&
          d[2] == 2000 && d[3] == 2600 && d[4] == 3000, "B4 point set");
    CHECK(il_cal_dac_points(3000, 1000, d) && d[0] == 2920 && d[1] == 2600 &&
          d[2] == 2000 && d[3] == 1400 && d[4] == 1000, "inverted sweep");
    CHECK(!il_cal_dac_points(1000, 1500, d), "short span accepted");
    CHECK(il_cal_dac_points(0, 4095, d) && d[4] == 4095, "full span refused");
    CHECK(!il_cal_dac_points(100, 0x7000, d), "out of range accepted");

    test_bench_levels();
    test_skew_estimate();

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("test_interleave_cal: all passed\n");
    return 0;
}
