/*
 * Math channel host test (parity C12): stock's eight ops on real records.
 *
 * Build and run:  make -C firmware test-math-channel
 *
 * What is pinned: the arithmetic in volts (calibrated) and counts (not), with
 * each channel's own 0 V code; A/B near zero; saturation; the 1-2-5 scale and
 * its hysteresis; the row mapping and its clipping; labels.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "math_channel.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)
static int near(float a, float b, float tol) { return fabsf(a - b) <= tol; }

#define N 8

/* 0 V at 100 (a), 90 (b): different zeros on purpose, as with two positions. */
static math_in_t cal_in(void)
{
    math_in_t in = { 100, 90, 0.05f, 0.1f };      /* 50 mV and 100 mV per count */
    return in;
}

static void test_add_sub_volts(void)
{
    math_in_t in = cal_in();
    uint8_t a[N] = { 100, 120, 80, 100, 140, 60, 100, 100 };   /* 0,+1,-1,0,+2,-2 V */
    uint8_t b[N] = {  90, 100,  90,  80,  90,  70,  90, 130 };   /* 0,+1, 0,-1, 0,-2, 0,+4 V */
    float o[N];

    math_unit_t u = math_compute(MATH_ADD, a, b, N, &in, o);
    CHECK(u == MATH_UNIT_VOLTS, "add unit");
    CHECK(near(o[1], 2.0f, 1e-4f) && near(o[2], -1.0f, 1e-4f) && near(o[3], -1.0f, 1e-4f)
          && near(o[5], -4.0f, 1e-4f) && near(o[7], 4.0f, 1e-4f), "A+B: %g %g %g %g %g", o[1], o[2], o[3], o[5], o[7]);
    math_compute(MATH_SUB, a, b, N, &in, o);
    CHECK(near(o[1], 0.0f, 1e-4f) && near(o[2], -1.0f, 1e-4f) && near(o[3], 1.0f, 1e-4f)
          && near(o[5], 0.0f, 1e-4f) && near(o[7], -4.0f, 1e-4f), "A-B: %g %g %g", o[1], o[3], o[7]);
}

static void test_mul_div_unary(void)
{
    math_in_t in = cal_in();
    uint8_t a[N] = { 120, 80, 140, 100, 120, 100, 100, 100 };    /* +1 -1 +2 0 +1 */
    uint8_t b[N] = { 110, 110, 100,  100, 90, 90, 90, 90 };      /* +2 +2 +1 +1 0 */
    float o[N];

    math_unit_t u = math_compute(MATH_MUL, a, b, N, &in, o);
    CHECK(u == MATH_UNIT_VOLTS2, "mul unit");
    CHECK(near(o[0], 2.0f, 1e-4f) && near(o[1], -2.0f, 1e-4f) && near(o[2], 2.0f, 1e-4f)
          && near(o[3], 0.0f, 1e-4f), "A*B: %g %g %g %g", o[0], o[1], o[2], o[3]);

    u = math_compute(MATH_DIV, a, b, N, &in, o);
    CHECK(u == MATH_UNIT_RATIO, "div unit");
    CHECK(near(o[0], 0.5f, 1e-4f) && near(o[1], -0.5f, 1e-4f) && near(o[2], 2.0f, 1e-4f),
          "A/B: %g %g %g", o[0], o[1], o[2]);

    math_compute(MATH_INV_A, a, NULL, N, &in, o);
    CHECK(near(o[0], -1.0f, 1e-4f) && near(o[1], 1.0f, 1e-4f) && near(o[3], 0.0f, 1e-4f), "-A");
    math_compute(MATH_INV_B, NULL, b, N, &in, o);
    CHECK(near(o[0], -2.0f, 1e-4f) && near(o[3], -1.0f, 1e-4f), "-B: %g", o[0]);
    math_compute(MATH_ABS_A, a, NULL, N, &in, o);
    CHECK(near(o[0], 1.0f, 1e-4f) && near(o[1], 1.0f, 1e-4f) && near(o[2], 2.0f, 1e-4f), "|A|");
    uint8_t b2[N] = { 70, 90, 110, 90, 90, 90, 90, 90 };         /* -2 0 +2 V */
    math_compute(MATH_ABS_B, NULL, b2, N, &in, o);
    CHECK(near(o[0], 2.0f, 1e-4f) && near(o[1], 0.0f, 1e-4f) && near(o[2], 2.0f, 1e-4f), "|B|");
}

static void test_counts_when_uncalibrated(void)
{
    math_in_t in = { 128, 128, 0.0f, 0.0f };
    uint8_t a[N] = { 138, 118, 128, 200, 0, 128, 128, 128 };
    uint8_t b[N] = { 148, 128, 100, 128, 255, 128, 128, 128 };
    float o[N];
    CHECK(math_compute(MATH_ADD, a, b, N, &in, o) == MATH_UNIT_COUNTS, "counts unit");
    CHECK(o[0] == 30.0f && o[1] == -10.0f && o[2] == -28.0f && o[4] == -1.0f, "A+B counts");
    CHECK(math_compute(MATH_MUL, a, b, N, &in, o) == MATH_UNIT_COUNTS2, "counts2 unit");
    CHECK(o[0] == 200.0f, "A*B counts");

    /* one calibrated channel is not enough for a two-channel op ... */
    in.k_a = 0.05f;
    CHECK(math_compute(MATH_SUB, a, b, N, &in, o) == MATH_UNIT_COUNTS, "mixed -> counts");
    CHECK(o[0] == -10.0f, "mixed stays in counts for both");
    /* ... but is enough for an op that reads only that channel */
    CHECK(math_compute(MATH_INV_A, a, b, N, &in, o) == MATH_UNIT_VOLTS, "-A uses A's gain only");
    CHECK(near(o[0], -0.5f, 1e-5f), "-A in volts");
}

static void test_div_by_zero(void)
{
    math_in_t in = cal_in();
    /* B at its zero (90), +-1 count (inside the dead zone), and +-2 (out) */
    uint8_t b[N] = {  90,  91,  89,  92,  88,  90,  90,  90 };
    uint8_t a[N] = { 120,  80, 100, 120,  80, 100, 140, 100 };
    float o[N];
    math_compute(MATH_DIV, a, b, N, &in, o);
    CHECK(o[0] >= MATH_SAT, "+A / 0 -> +sat (%g)", o[0]);
    CHECK(o[1] <= -MATH_SAT, "-A / +1 count -> -sat");
    CHECK(o[2] == 0.0f, "0/0 -> 0, not NaN");
    CHECK(o[3] < MATH_SAT && near(o[3], 1.0f / 0.2f, 1e-3f), "B at +2 counts is a quotient: %g", o[3]);
    CHECK(near(o[4], -1.0f / -0.2f, 1e-3f), "B at -2 counts: %g", o[4]);
    for (int i = 0; i < N; i++) CHECK(o[i] == o[i], "no NaN at %d", i);
    /* the peak ignores the saturated samples */
    CHECK(near(math_peak(o, N), 5.0f, 1e-3f), "peak skips sat: %g", math_peak(o, N));
}

static void test_clip_and_extremes(void)
{
    math_in_t in = { 128, 128, 1.0f, 1.0f };      /* 1 V per count: large numbers */
    uint8_t a[N] = { 0, 255, 128, 255, 0, 128, 128, 128 };
    uint8_t b[N] = { 255, 255, 0, 0, 0, 128, 128, 128 };
    float o[N];
    math_compute(MATH_ADD, a, b, N, &in, o);
    CHECK(o[1] == 254.0f && o[4] == -256.0f, "full-range sum is exact in float: %g %g", o[1], o[4]);
    math_compute(MATH_MUL, a, b, N, &in, o);
    CHECK(o[4] == 16384.0f && o[0] == -128.0f * 127.0f, "full-range product");

    int16_t r[N];
    float v[N] = { 0.0f, 1.0f, -1.0f, 1000.0f, -1000.0f, MATH_SAT, -MATH_SAT, 0.04f };
    math_to_rows(v, N, 1.0f, 26, 120, 18, 224, r);
    CHECK(r[0] == 120, "zero on the centre row");
    CHECK(r[1] == 94 && r[2] == 146, "+-1 step = 26 rows: %d %d", r[1], r[2]);
    CHECK(r[3] == 18 && r[4] == 223, "huge values clip to the band: %d %d", r[3], r[4]);
    CHECK(r[5] == 18 && r[6] == 223, "saturated values clip to the edge of their sign");
    CHECK(r[7] == 119, "rounds to nearest row (0.04*26 = 1.04): %d", r[7]);
    float z[2] = { 5.0f, -5.0f };
    int16_t rz[2];
    math_to_rows(z, 2, 0.0f, 26, 120, 18, 224, rz);       /* step 0 must not divide */
    CHECK(rz[0] == 120 && rz[1] == 120, "step 0 draws the centre");
}

static void test_scale(void)
{
    CHECK(near(math_scale_value(0), 0.001f, 1e-9f), "first rung");
    CHECK(near(math_scale_value(1), 0.002f, 1e-9f) && near(math_scale_value(2), 0.005f, 1e-9f), "1-2-5");
    CHECK(near(math_scale_value(3), 0.01f, 1e-9f), "next decade");
    CHECK(near(math_scale_value(9), 1.0f, 1e-6f) && near(math_scale_value(10), 2.0f, 1e-6f), "1 V, 2 V");
    CHECK(near(math_scale_value(MATH_SCALE_COUNT - 1), 50000.0f, 1.0f), "last rung");

    /* peak 3 V: first step with 3.5*step >= 3 is 1 V/div */
    CHECK(math_scale_value(math_scale_pick(3.0f, -1)) == 1.0f, "3 V -> 1 V/d");
    CHECK(math_scale_value(math_scale_pick(3.6f, -1)) == 2.0f, "3.6 V -> 2 V/d (3.5 div limit)");
    CHECK(near(math_scale_value(math_scale_pick(0.0f, -1)), 0.001f, 1e-9f), "no signal -> finest");
    CHECK(math_scale_pick(1e9f, -1) == MATH_SCALE_COUNT - 1, "off the ladder -> coarsest");

    /* hysteresis: a peak flickering around the 3.5-division limit moves the
     * scale at most once, then holds (no frame-to-frame flip). */
    int idx = math_scale_pick(3.6f, -1);            /* 2 V/d */
    int changes = 0;
    for (int i = 0; i < 8; i++) {
        int n2 = math_scale_pick((i & 1) ? 3.4f : 3.6f, idx);
        if (n2 != idx) changes++;
        idx = n2;
    }
    CHECK(changes <= 1, "scale flipped %d times on a boundary", changes);
    idx = math_scale_pick(3.6f, math_scale_pick(3.4f, -1));
    CHECK(math_scale_value(idx) == 1.0f, "3.6 V stays on 1 V/d once there (<= 4 div)");
    /* ... but a real drop does move it down, and growth moves it up */
    idx = math_scale_pick(1.0f, idx);
    CHECK(math_scale_value(idx) == 0.5f, "peak 1 V drops to 500 mV/d (%g)", math_scale_value(idx));
    idx = math_scale_pick(30.0f, idx);
    CHECK(math_scale_value(idx) == 10.0f, "peak 30 V rises to 10 V/d");
    /* every pick actually fits */
    for (float pk = 0.0005f; pk < 20000.0f; pk *= 1.37f) {
        int i = math_scale_pick(pk, -1);
        CHECK(pk * 10.0f <= 35.0f * math_scale_value(i) || i == MATH_SCALE_COUNT - 1, "fits %g", pk);
        if (i > 0) CHECK(pk * 10.0f > 35.0f * math_scale_value(i - 1), "smallest fit %g", pk);
    }
}

/* Scale floor: a quiet result must not be autoscaled into the ADC noise. */
static void test_scale_floor(void)
{
    /* 14 mV/count (range 4 class), 32 counts/div -> 0.448 V/div per channel */
    math_in_t in = { 100, 100, 0.014f, 0.028f };
    CHECK(near(math_step_floor(MATH_SUB, &in, 32.0f), 0.448f, 1e-4f), "A-B floor = finer channel div");
    CHECK(near(math_step_floor(MATH_ABS_B, &in, 32.0f), 0.896f, 1e-4f), "|B| floor = B's div");
    CHECK(near(math_step_floor(MATH_MUL, &in, 32.0f), 0.448f * 0.896f, 1e-4f), "A*B floor = product");
    CHECK(math_step_floor(MATH_DIV, &in, 32.0f) == 0.0f, "A/B has no floor");
    CHECK(math_scale_floor_idx(0.0f) == 0, "no floor -> 0");
    CHECK(math_scale_value(math_scale_floor_idx(0.448f)) == 0.5f, "0.448 -> 0.5/d");
    CHECK(math_scale_value(math_scale_floor_idx(0.5f)) == 0.5f, "a ladder value is its own floor");
    /* A-B of one signal with 2 counts of noise: the pick alone would draw it at
     * 10 mV/d (2.8 div of noise); with the floor it is under 0.1 div. */
    const float noise = 2.0f * 0.014f;
    int idx = math_scale_pick(noise, -1);
    CHECK(math_scale_value(idx) < 0.05f, "pick alone autoscales the noise: %g", math_scale_value(idx));
    int f = math_scale_floor_idx(math_step_floor(MATH_SUB, &in, 32.0f));
    if (idx < f) idx = f;
    CHECK(noise / math_scale_value(idx) < 0.1f, "floored: noise %g div", noise / math_scale_value(idx));
    /* uncalibrated: one channel division in counts */
    math_in_t raw = { 128, 128, 0.0f, 0.0f };
    CHECK(math_step_floor(MATH_ADD, &raw, 32.0f) == 32.0f, "counts floor = 32 c");
}

static void test_labels(void)
{
    char s[24];
    math_scale_label(math_scale_pick(0.9f, -1), MATH_UNIT_VOLTS, s, sizeof s);
    CHECK(!strcmp(s, "500mV/d") || !strcmp(s, "200mV/d"), "volts mV: %s", s);
    math_scale_label(9, MATH_UNIT_VOLTS, s, sizeof s);   CHECK(!strcmp(s, "1V/d"), "%s", s);
    math_scale_label(10, MATH_UNIT_VOLTS, s, sizeof s);  CHECK(!strcmp(s, "2V/d"), "%s", s);
    math_scale_label(5, MATH_UNIT_VOLTS, s, sizeof s);   CHECK(!strcmp(s, "50mV/d"), "%s", s);
    math_scale_label(2, MATH_UNIT_VOLTS, s, sizeof s);   CHECK(!strcmp(s, "5mV/d"), "%s", s);
    math_scale_label(8, MATH_UNIT_VOLTS2, s, sizeof s);  CHECK(!strcmp(s, "0.5V2/d"), "%s", s);
    math_scale_label(7, MATH_UNIT_VOLTS2, s, sizeof s);  CHECK(!strcmp(s, "0.2V2/d"), "%s", s);
    math_scale_label(1, MATH_UNIT_RATIO, s, sizeof s);   CHECK(!strcmp(s, "0.002/d"), "%s", s);
    math_scale_label(13, MATH_UNIT_COUNTS, s, sizeof s); CHECK(!strcmp(s, "20c/d"), "%s", s);
    math_scale_label(19, MATH_UNIT_COUNTS2, s, sizeof s);CHECK(!strcmp(s, "2000c2/d"), "%s", s);

    static const char *want[MATH_COUNT] = { "A+B", "A-B", "A*B", "A/B", "-A", "-B", "|A|", "|B|" };
    for (int i = 0; i < MATH_COUNT; i++)
        CHECK(!strcmp(math_channel_name((math_op_t)i), want[i]), "name %d", i);
    CHECK(!strcmp(math_channel_name((math_op_t)MATH_COUNT), "???"), "out of range");
    CHECK(MATH_COUNT == 8, "stock has eight ops");
}

int main(void)
{
    test_add_sub_volts();
    test_mul_div_unary();
    test_counts_when_uncalibrated();
    test_div_by_zero();
    test_clip_and_extremes();
    test_scale();
    test_scale_floor();
    test_labels();
    printf("math_channel: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
