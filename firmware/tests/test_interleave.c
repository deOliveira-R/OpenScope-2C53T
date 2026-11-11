/*
 * Host tests for dsp/interleave (single-channel 250 MS/s merge).
 *
 *   1. The in-place merge equals a straightforward out-of-place reference,
 *      for every index (the two-pass walk is the risky part).
 *   2. Order is even = A, odd = B, from the centre half of each converter.
 *   3. A trigger at converter index 512 lands at merged index 512.
 *   4. A tone sampled by two converters half a sample apart merges into one
 *      record at twice the rate: no zig-zag, spectral peak at f/2fs.
 *   5. Gain/offset correction inverts a known B mismatch; identity and NULL
 *      leave B untouched; results round and clamp to 0..255.
 *   6. Piecewise map (parity ilplus) with unit #2's five self-cal levels
 *      (bench 2026-11-11, record units, CH1 shorted): every level's corrected
 *      B mean is within 0.3 count of A, where the linear fit left ~1 count.
 *      Knot validation refuses non-monotonic or implausible maps.
 *   7. Skew FIR: the in-place ring equals an out-of-place reference; DC goes
 *      through unchanged; taps sum to 1; skew 0 is the identity.
 *   8. Image spur on a synthetic tone, B at 0.451 of a converter period with
 *      the bench's nonlinear B: before (stock-like merge) ~-38 dBc at
 *      10 MHz, after (knots + skew) well below; the tone keeps its amplitude.
 */
#include "../src/dsp/interleave.h"

#include <math.h>
#include <stdio.h>
#include <stdbool.h>
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

#define N INTERLEAVE_N

static uint8_t clamp8(double v)
{
    if (v <= 0.0) return 0;
    if (v >= 255.0) return 255;
    return (uint8_t)(v + 0.5);
}

static void reference(const uint8_t *a, const uint8_t *b, const interleave_corr_t *c, uint8_t *out)
{
    for (uint32_t i = 0; i < N; i++) {
        uint32_t k = INTERLEAVE_WIN_LO + i / 2u;
        out[i] = (i & 1u) ? interleave_correct_b(b[k], c) : a[k];
    }
}

static void test_in_place_matches_reference(void)
{
    uint8_t a[N], b[N], ref[N];
    for (uint32_t i = 0; i < N; i++) {           /* every byte distinct-ish */
        a[i] = (uint8_t)(i * 7u + 3u);
        b[i] = (uint8_t)(i * 13u + 101u);
    }
    const interleave_corr_t c = { .gain = 0.98f, .offset = 4.0f };
    reference(a, b, &c, ref);
    interleave_merge(a, b, &c);
    int bad = 0;
    for (uint32_t i = 0; i < N; i++) bad += (a[i] != ref[i]);
    CHECK(bad == 0, "in-place merge differs from the reference at %d indices", bad);
}

static void test_order_and_window(void)
{
    uint8_t a[N], b[N];
    for (uint32_t i = 0; i < N; i++) { a[i] = (uint8_t)(i & 0xFF); b[i] = (uint8_t)(255u - (i & 0xFF)); }
    uint8_t a0[N]; memcpy(a0, a, N);
    interleave_merge(a, b, NULL);
    CHECK(a[0] == a0[256], "merged[0] must be A[256], got %u", a[0]);
    CHECK(a[1] == b[256], "merged[1] must be B[256], got %u", a[1]);
    CHECK(a[1022] == a0[767], "merged[1022] must be A[767]");
    CHECK(a[1023] == b[767], "merged[1023] must be B[767]");
}

static void test_trigger_index_preserved(void)
{
    /* A step at converter index 512 (the un-rotated trigger) in both. */
    uint8_t a[N], b[N];
    for (uint32_t i = 0; i < N; i++) { a[i] = (i < 512) ? 60 : 200; b[i] = (i < 512) ? 60 : 200; }
    interleave_merge(a, b, NULL);
    uint32_t first_high = N;
    for (uint32_t i = 0; i < N; i++) if (a[i] == 200) { first_high = i; break; }
    CHECK(first_high == 512, "trigger step should sit at merged index 512, found %u", first_high);
}

/* Goertzel-ish single-bin magnitude at f cycles/sample. */
static double tone_mag(const uint8_t *x, double f)
{
    double re = 0, im = 0, mean = 0;
    for (uint32_t i = 0; i < N; i++) mean += x[i];
    mean /= N;
    for (uint32_t i = 0; i < N; i++) {
        double w = 0.5 - 0.5 * cos(2 * M_PI * i / (N - 1));
        re += w * (x[i] - mean) * cos(2 * M_PI * f * i);
        im -= w * (x[i] - mean) * sin(2 * M_PI * f * i);
    }
    return sqrt(re * re + im * im);
}

static void test_tone_doubles_rate(void)
{
    /* 10 MHz into converters at 125 MS/s, B half a sample late: the merged
     * record is 250 MS/s, so the tone sits at 10/250 = 0.04 cycles/sample.
     * A wrong order would put a strong image at 0.5 - 0.04 = 0.46. */
    const double f_conv = 10.0 / 125.0;
    uint8_t a[N], b[N];
    for (uint32_t k = 0; k < N; k++) {
        a[k] = clamp8(128.0 + 90.0 * sin(2 * M_PI * f_conv * k));
        b[k] = clamp8(128.0 + 90.0 * sin(2 * M_PI * f_conv * (k + 0.5)));
    }
    uint8_t a2[N], b2[N];
    memcpy(a2, b, N); memcpy(b2, a, N);          /* the wrong order: B even */
    interleave_merge(a, b, NULL);
    interleave_merge(a2, b2, NULL);
    double sig = tone_mag(a, 0.04), img = tone_mag(a, 0.46);
    CHECK(sig > 1000.0 * img, "merged tone should dominate its interleave image (sig %.1f, image %.3f)", sig, img);
    double zig = 0, zig_wrong = 0;
    for (uint32_t i = 1; i + 1 < N; i++) {
        zig       += fabs((double)a[i - 1]  - 2.0 * a[i]  + a[i + 1]);
        zig_wrong += fabs((double)a2[i - 1] - 2.0 * a2[i] + a2[i + 1]);
    }
    CHECK(zig_wrong > 5.0 * zig, "even = A must be far smoother than the reversed order (%.0f vs %.0f)", zig, zig_wrong);
    CHECK(tone_mag(a2, 0.46) > 100.0 * img, "the reversed order must show the image the right order avoids");
}

static void test_gain_offset_correction(void)
{
    /* Unit #2-like mismatch in record units: B = 0.9795 A + 4.07. */
    const interleave_corr_t c = { .gain = 0.9795f, .offset = 4.07f };
    int worst = 0;
    for (int v = 10; v <= 245; v++) {
        uint8_t b = clamp8(0.9795 * v + 4.07);
        int got = interleave_correct_b(b, &c);
        int err = got - v; if (err < 0) err = -err;
        if (err > worst) worst = err;
    }
    CHECK(worst <= 1, "corrected B should match A within 1 count, worst %d", worst);

    const interleave_corr_t id = { .gain = 1.0f, .offset = 0.0f };
    for (int v = 0; v < 256; v++) {
        CHECK(interleave_correct_b((uint8_t)v, &id) == v, "identity must leave %d unchanged", v);
        CHECK(interleave_correct_b((uint8_t)v, NULL) == v, "NULL must leave %d unchanged", v);
    }
    const interleave_corr_t big = { .gain = 0.5f, .offset = -10.0f };
    CHECK(interleave_correct_b(200, &big) == 255, "must clamp high");
    const interleave_corr_t low = { .gain = 1.0f, .offset = 50.0f };
    CHECK(interleave_correct_b(20, &low) == 0, "must clamp low");
    const interleave_corr_t bad = { .gain = 0.0f, .offset = 3.0f };
    CHECK(interleave_correct_b(77, &bad) == 77, "a non-positive gain must be ignored, not divide by zero");
}

/* ── 6. piecewise level map with the bench levels ─────────────────────── */

/* Unit #2, bench 2026-11-11 (DAC 1641/1961/2352/2744/3144). Test data. */
static const double BA[5] = { 9.77, 49.61, 98.98, 148.60, 199.02 };
static const double BB[5] = { 13.96, 51.63, 100.96, 150.59, 198.62 };

static uint32_t rng = 12345u;
static double urand(void) { rng = rng * 1664525u + 1013904223u; return ((rng >> 8) + 0.5) / 16777216.0; }
static double grand(void) { return sqrt(-2.0 * log(urand())) * cos(2.0 * M_PI * urand()); }

static void bench_knots(interleave_corr_t *c)
{
    int16_t ka[5], kb[5];
    for (int i = 0; i < 5; i++) { ka[i] = (int16_t)lround(BA[i] * 100); kb[i] = (int16_t)lround(BB[i] * 100); }
    interleave_corr_identity(c);
    CHECK(interleave_corr_set_knots(c, 5, ka, kb), "bench knots refused");
}

/* Forward model of unit #2's converter B: piecewise through the bench
 * levels (the only truth there is), end segments extended. */
static double bench_b_of_a(double a)
{
    int i = 0;
    while (i < 3 && a > BA[i + 1]) i++;
    return BB[i] + (a - BA[i]) * (BB[i + 1] - BB[i]) / (BA[i + 1] - BA[i]);
}

/* Mean of corrected B over noisy samples of a level whose raw mean is bm. */
static double corrected_mean(double bm, const interleave_corr_t *c)
{
    double s = 0;
    const int n = 4096;
    for (int i = 0; i < n; i++) s += interleave_correct_b(clamp8(bm + 2.0 * grand()), c);
    return s / n;
}

static void test_piecewise_bench_levels(void)
{
    interleave_corr_t pw, lin;
    bench_knots(&pw);
    interleave_corr_identity(&lin);
    lin.gain = 0.9809f; lin.offset = 3.89f;     /* the linear fit of the same levels */
    double worst_pw = 0, worst_lin = 0;
    for (int i = 0; i < 5; i++) {
        const double e_pw = corrected_mean(BB[i], &pw) - BA[i];
        const double e_lin = corrected_mean(BB[i], &lin) - BA[i];
        printf("  level %d: A %6.2f  B %6.2f  linear %+5.2f  piecewise %+5.2f\n", i, BA[i], BB[i], e_lin, e_pw);
        if (fabs(e_pw) > worst_pw) worst_pw = fabs(e_pw);
        if (fabs(e_lin) > worst_lin) worst_lin = fabs(e_lin);
    }
    CHECK(worst_pw <= 0.3, "piecewise map leaves %.2f count at a bench level (want <= 0.3)", worst_pw);
    CHECK(worst_lin > 0.8, "the linear fit should leave ~1 count (got %.2f): test data wrong?", worst_lin);
    /* Exact float map at the knots, monotonic in between, extrapolates. */
    for (int i = 0; i < 5; i++)
        CHECK(fabs(interleave_map_b((float)BB[i], &pw) - BA[i]) < 0.01, "knot %d not mapped exactly", i);
    float prev = -1e9f;
    for (int v = 0; v < 256; v++) {
        const float m = interleave_map_b((float)v, &pw);
        CHECK(m > prev, "map not increasing at %d", v);
        prev = m;
    }
    CHECK(interleave_correct_b(0, &pw) == 0 && interleave_correct_b(255, &pw) == 255,
          "ends must extrapolate and clamp");

    /* Validation: a dip, a flat segment, an implausible slope, too few. */
    int16_t a[5] = { 1000, 5000, 10000, 15000, 20000 }, b[5] = { 1400, 5200, 10100, 15000, 19900 };
    interleave_corr_t c = pw;
    b[2] = 5100;
    CHECK(!interleave_corr_set_knots(&c, 5, a, b), "dip accepted");
    CHECK(c.n_knots == 5 && c.knot_b[2] == pw.knot_b[2], "refused knots changed the map");
    b[2] = 5200;
    CHECK(!interleave_corr_set_knots(&c, 5, a, b), "flat segment accepted");
    b[2] = 30000;
    CHECK(!interleave_corr_set_knots(&c, 5, a, b), "out-of-range knot accepted");
    b[2] = 18000; b[3] = 18500;
    CHECK(!interleave_corr_set_knots(&c, 5, a, b), "slope 0.1 accepted");
    CHECK(!interleave_corr_set_knots(&c, 1, a, b), "one knot accepted");
    CHECK(interleave_corr_set_knots(&c, 0, a, b) && c.n_knots == 0, "clear refused");
}

/* ── 7. skew FIR mechanics ────────────────────────────────────────────── */

static void test_skew_mechanics(void)
{
    interleave_corr_t c;
    interleave_corr_identity(&c);
    CHECK(!interleave_skew_set(&c, IL_SKEW_PPM_MAX + 1), "out-of-range skew accepted");
    CHECK(!interleave_skew_set(&c, -IL_SKEW_PPM_MAX - 1), "out-of-range skew accepted");
    CHECK(c.skew_ppm == 0, "refused skew changed the state");
    CHECK(interleave_skew_set(&c, -49000) && c.skew_ppm == -49000, "skew refused");
    double sum = 0;
    for (unsigned j = 0; j < IL_SKEW_TAPS_N; j++) sum += c.skew_taps[j];
    CHECK(fabs(sum - 1.0) < 1e-6, "taps sum %.9f, want 1", sum);

    /* In place vs out of place: same FIR from the untouched inputs. */
    uint8_t a[N], b[N], a0[N], b0[N];
    for (uint32_t i = 0; i < N; i++) {
        a[i] = clamp8(120 + 80 * sin(0.37 * i) + 10 * grand());
        b[i] = clamp8(118 + 78 * sin(0.37 * i + 0.2) + 10 * grand());
    }
    memcpy(a0, a, N); memcpy(b0, b, N);
    {   /* knots keep the skew */
        interleave_corr_t k; bench_knots(&k);
        CHECK(interleave_corr_set_knots(&c, k.n_knots, k.knot_a, k.knot_b), "knots refused");
        CHECK(c.skew_ppm == -49000, "setting knots dropped the skew");
    }
    interleave_merge(a, b, &c);
    int bad = 0;
    for (uint32_t k = INTERLEAVE_WIN_LO; k < INTERLEAVE_WIN_LO + N / 2; k++) {
        double acc = 0;
        for (int j = -8; j <= 8; j++) {
            const double v = (j & 1) ? a0[(int)k + (j + 1) / 2] : interleave_map_b(b0[(int)k + j / 2], &c);
            acc += c.skew_taps[j + 8] * v;
        }
        const uint8_t want = clamp8(acc);
        const uint32_t i = 2u * (k - INTERLEAVE_WIN_LO) + 1u;
        if (a[i] != want) bad++;
        if (a[i - 1] != a0[k]) bad++;
    }
    CHECK(bad == 0, "skew path differs from the out-of-place reference at %d samples", bad);

    /* DC: a level stays where the map put it. */
    for (int lvl = 20; lvl <= 220; lvl += 50) {
        memset(a, lvl, N); memset(b, lvl, N);
        interleave_corr_t s; interleave_corr_identity(&s); interleave_skew_set(&s, -49000);
        interleave_merge(a, b, &s);
        int moved = 0;
        for (uint32_t i = 0; i < N; i++) moved += (a[i] != lvl);
        CHECK(moved == 0, "DC %d moved at %d samples through the skew FIR", lvl, moved);
    }
    /* skew 0 = identity */
    CHECK(interleave_skew_set(&c, 0) && c.skew_ppm == 0, "skew off refused");
}

/* ── 8. image spur ────────────────────────────────────────────────────── */

/* Hann DFT magnitude at f cycles/merged sample; peak within +-3 bins. */
static double peak_near(const uint8_t *x, double f)
{
    double best = 0;
    for (double d = -3.0; d <= 3.0; d += 0.25) best = fmax(best, tone_mag(x, f + d / N));
    return best;
}

/* Two converters at 125 MS/s, tone f MHz, B at tau of a period after A,
 * through the bench's nonlinear B, then 8-bit quantised with 0.5 count noise. */
static void tone_record(uint8_t *a, uint8_t *b, double f_mhz, double tau, bool nonlinear_b)
{
    const double fc = f_mhz / 125.0;
    for (uint32_t k = 0; k < N; k++) {
        const double xa = 105.0 + 80.0 * sin(2 * M_PI * fc * k + 0.3);
        const double xb = 105.0 + 80.0 * sin(2 * M_PI * fc * (k + tau) + 0.3);
        a[k] = clamp8(xa + 0.5 * grand());
        b[k] = clamp8((nonlinear_b ? bench_b_of_a(xb) : xb) + 0.5 * grand());
    }
}

static double image_dbc(const uint8_t *m, double f_mhz)
{
    const double f = f_mhz / 250.0;
    return 20.0 * log10(peak_near(m, 0.5 - f) / peak_near(m, f));
}

static void test_image_spur(void)
{
    static const double fs[] = { 1.0, 10.0, 30.0, 50.0 };
    interleave_corr_t full;
    bench_knots(&full);
    interleave_skew_set(&full, -49000);
    interleave_corr_t lin;
    interleave_corr_identity(&lin);
    lin.gain = 0.9809f; lin.offset = 3.89f;
    printf("  image spur, B at 0.451 of a period, unit #2's B levels:\n");
    printf("    f MHz   none    line   knots   knots+skew   (ideal converters: 8-bit floor)\n");
    for (unsigned t = 0; t < sizeof(fs) / sizeof(fs[0]); t++) {
        double r[4];
        for (int mode = 0; mode < 4; mode++) {
            uint8_t a[N], b[N];
            tone_record(a, b, fs[t], 0.451, true);
            interleave_corr_t k; bench_knots(&k);
            interleave_merge(a, b, mode == 0 ? NULL : mode == 1 ? &lin : mode == 2 ? &k : &full);
            r[mode] = image_dbc(a, fs[t]);
        }
        uint8_t ai[N], bi[N];
        tone_record(ai, bi, fs[t], 0.5, false);
        interleave_merge(ai, bi, NULL);
        printf("    %5.1f  %6.1f  %6.1f  %6.1f  %6.1f       %6.1f\n", fs[t], r[0], r[1], r[2], r[3],
               image_dbc(ai, fs[t]));
        if (fs[t] == 10.0) {
            CHECK(r[0] > -41.0 && r[0] < -35.0, "uncorrected 10 MHz image %.1f dBc, stock reads -37.9", r[0]);
            CHECK(r[2] > -41.0, "knots alone should leave the skew image (%.1f dBc)", r[2]);
        }
        if (fs[t] >= 10.0)   /* the 8-bit floor here is ~-61..-65; 50 MHz gets 3 dB more room */
            CHECK(r[3] < (fs[t] >= 50.0 ? -52.0 : -55.0), "corrected image at %.0f MHz is %.1f dBc (want < -55)", fs[t], r[3]);
        CHECK(r[3] < r[0] - 15.0, "correction gains only %.1f dB at %.0f MHz", r[0] - r[3], fs[t]);
    }

    /* Amplitude: the corrected tone equals the ideal merge (B at 0.5, no
     * mismatch) within 0.1 dB. */
    for (unsigned t = 1; t < sizeof(fs) / sizeof(fs[0]); t++) {
        uint8_t a[N], b[N], ai[N], bi[N];
        tone_record(a, b, fs[t], 0.451, true);
        tone_record(ai, bi, fs[t], 0.5, false);
        interleave_merge(a, b, &full);
        interleave_merge(ai, bi, NULL);
        const double f = fs[t] / 250.0;
        const double db = 20.0 * log10(peak_near(a, f) / peak_near(ai, f));
        CHECK(fabs(db) < 0.1, "tone amplitude changed by %.3f dB at %.0f MHz", db, fs[t]);
    }
}

int main(void)
{
    test_in_place_matches_reference();
    test_order_and_window();
    test_trigger_index_preserved();
    test_tone_doubles_rate();
    test_gain_offset_correction();
    test_piecewise_bench_levels();
    test_skew_mechanics();
    test_image_spur();
    if (failures == 0) { printf("test_interleave: all checks passed\n"); return 0; }
    printf("test_interleave: %d check(s) FAILED\n", failures);
    return 1;
}
