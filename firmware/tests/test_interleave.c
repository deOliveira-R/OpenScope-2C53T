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
 */
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
    const interleave_corr_t c = { 0.98f, 4.0f };
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
    const interleave_corr_t c = { 0.9795f, 4.07f };
    int worst = 0;
    for (int v = 10; v <= 245; v++) {
        uint8_t b = clamp8(0.9795 * v + 4.07);
        int got = interleave_correct_b(b, &c);
        int err = got - v; if (err < 0) err = -err;
        if (err > worst) worst = err;
    }
    CHECK(worst <= 1, "corrected B should match A within 1 count, worst %d", worst);

    const interleave_corr_t id = { 1.0f, 0.0f };
    for (int v = 0; v < 256; v++) {
        CHECK(interleave_correct_b((uint8_t)v, &id) == v, "identity must leave %d unchanged", v);
        CHECK(interleave_correct_b((uint8_t)v, NULL) == v, "NULL must leave %d unchanged", v);
    }
    const interleave_corr_t big = { 0.5f, -10.0f };
    CHECK(interleave_correct_b(200, &big) == 255, "must clamp high");
    const interleave_corr_t low = { 1.0f, 50.0f };
    CHECK(interleave_correct_b(20, &low) == 0, "must clamp low");
    const interleave_corr_t bad = { 0.0f, 3.0f };
    CHECK(interleave_correct_b(77, &bad) == 77, "a non-positive gain must be ignored, not divide by zero");
}

int main(void)
{
    test_in_place_matches_reference();
    test_order_and_window();
    test_trigger_index_preserved();
    test_tone_doubles_rate();
    test_gain_offset_correction();
    if (failures == 0) { printf("test_interleave: all checks passed\n"); return 0; }
    printf("test_interleave: %d check(s) FAILED\n", failures);
    return 1;
}
