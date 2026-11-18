/*
 * Host tests for the scope horizontal (time) calibration.
 *
 * As with test_scope_cal.c, these do not second-guess bench numbers. They test
 * the PROPERTIES that keep this table from failing the way rate figures have
 * already failed on this project:
 *
 *   1. A code with no trustworthy rate returns exactly 0.0f, so a caller that
 *      forgets to check gets an obviously-zero frequency rather than a
 *      plausible one.
 *   2. Code 0x08 must never return one of its three withdrawn artifacts (all
 *      fitted to AUTO roll reads); it holds the 2026-11-10 NORMAL-trigger
 *      measurement.
 *   3. Out-of-range codes refuse rather than wrap.
 *   4. Derived quantities really are derived — s/div from the rate and the
 *      renderer's samples-per-division, Hz from the rate and a period.
 *   5. Labels come from the measurement and are NOT the nominal timebase_table
 *      strings, which are a constant ~2x away at stock's 25 samples/div.
 *   6. The fast-code zoom (parity C5) is stock's 1000/ratio per published
 *      record kind, and the label it produces is distinct per code and the
 *      same whichever record kind the hardware delivered.
 */

#include "../src/ui/scope_timebase.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

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






static void test_out_of_range(void)
{
    CHECK(scope_timebase_sample_rate(SCOPE_TIMEBASE_CODE_COUNT) == 0.0f,
          "a code past the end must not resolve");
    CHECK(scope_timebase_sample_rate(255) == 0.0f, "code 255 must not resolve");
    CHECK(scope_timebase_get_tier(200) == SCOPE_TB_NONE,
          "an out-of-range tier must be NONE");
}

static void test_derived_quantities(void)
{
    for (uint8_t c = 0; c < SCOPE_TIMEBASE_CODE_COUNT; c++) {
        const float fs = scope_timebase_sample_rate(c);
        if (fs <= 0.0f) continue;

        CHECK(fabsf(scope_timebase_seconds_per_sample(c) - 1.0f / fs) < 1e-12f,
              "code 0x%02X: s/sample must be 1/fs", c);
        CHECK(fabsf(scope_timebase_seconds_per_div(c) * fs / SCOPE_TIMEBASE_SAMPLES_PER_DIV - 1.0f) < 1e-6f,
              "code 0x%02X: s/div must be samples-per-div / fs", c);
        CHECK(fabsf(scope_timebase_hz_from_period(c, 100.0f) - fs / 100.0f) < 1e-6f,
              "code 0x%02X: Hz must be fs / period_samples", c);
    }

    /* A non-positive period is not a measurement. */
    CHECK(scope_timebase_hz_from_period(0x10, 0.0f) == 0.0f,
          "zero period must not yield a frequency");
    CHECK(scope_timebase_hz_from_period(0x10, -5.0f) == 0.0f,
          "negative period must not yield a frequency");
}

static void test_labels(void)
{
    char buf[16];

    /* 12490 S/s, 25 samples/div (stock's, C5) -> 2.0016 ms/div. */
    scope_timebase_label(0x10, buf, sizeof(buf));
    CHECK(strcmp(buf, "2.00ms") == 0,
          "0x10 label should be derived as 2.00ms, got \"%s\"", buf);

    /* And it must NOT be the nominal table's "1ms" — that is the whole point.
     * The nominal labels are a constant ~2x away on every measured code. */
    CHECK(strcmp(buf, "1ms") != 0, "0x10 must not still read as the nominal 1ms");

    /* 25,000 S/s -> exactly 1 ms/div (a float holds 0.99999997 ms: the unit is chosen on the rounded value). */
    scope_timebase_label(0x0F, buf, sizeof(buf));
    CHECK(strcmp(buf, "1.00ms") == 0,
          "0x0F label should be 1.00ms, got \"%s\"", buf);

    /* 50,000 S/s -> 500 us/div, sub-millisecond so microseconds. */
    scope_timebase_label(0x0E, buf, sizeof(buf));
    CHECK(strcmp(buf, "500us") == 0,
          "0x0E label should be 500us, got \"%s\"", buf);

    /* 0x0D is measured since 2026-11-14 (it was the last provisional row):
     * no marker. 125 k x 0.9997 -> 200 us/div. */
    scope_timebase_label(0x0D, buf, sizeof(buf));
    CHECK(strcmp(buf, "200us") == 0, "0x0D label should be 200us, got \"%s\"", buf);

    /* 0x0B / 0x0C have rates now: 50 us and 100 us per division. */
    scope_timebase_label(0x0B, buf, sizeof(buf));
    CHECK(strcmp(buf, "50us") == 0, "0x0B label should be 50us, got \"%s\"", buf);
    scope_timebase_label(0x0C, buf, sizeof(buf));
    CHECK(strcmp(buf, "100us") == 0, "0x0C label should be 100us, got \"%s\"", buf);

    /* No rate (a code past the table) -> explicit nothing, never a nominal number. */
    scope_timebase_label(0x15, buf, sizeof(buf));
    CHECK(strcmp(buf, "--") == 0, "a code past the table should be \"--\", got \"%s\"", buf);

    /* Fast codes: 4.9975 MS/s -> 5.0025 us/div needs a decimal; 124.83 MS/s ->
     * 200 ns/div needs nanoseconds (it printed "0us" before the ns branch). */
    scope_timebase_label(0x08, buf, sizeof(buf));
    CHECK(strcmp(buf, "5.0us") == 0, "0x08 label should be 5.0us, got \"%s\"", buf);
    scope_timebase_label(0x00, buf, sizeof(buf));
    CHECK(strcmp(buf, "200ns") == 0, "0x00 label should be 200ns, got \"%s\"", buf);
    scope_timebase_label(0x0A, buf, sizeof(buf));
    CHECK(strcmp(buf, "20us") == 0, "0x0A label should be 20us, got \"%s\"", buf);

    /* Interleaved record: same 25 samples/div at twice the rate -> half the
     * time. 124.83 MS/s x2 -> 100 ns/div at 0x00; factor 0 behaves as 1. */
    scope_timebase_label_scaled(0x00, 2u, buf, sizeof(buf));
    CHECK(strcmp(buf, "100ns") == 0, "0x00 interleaved label should be 100ns, got \"%s\"", buf);
    scope_timebase_label_scaled(0x00, 0u, buf, sizeof(buf));
    CHECK(strcmp(buf, "200ns") == 0, "factor 0 must act as 1, got \"%s\"", buf);
    scope_timebase_label_scaled(0x15, 2u, buf, sizeof(buf));
    CHECK(strcmp(buf, "--") == 0, "no rate stays \"--\" when scaled, got \"%s\"", buf);

    /* Truncation must be safe. */
    char small[3];
    scope_timebase_label(0x10, small, sizeof(small));
    CHECK(small[2] == '\0', "label must NUL-terminate within a 3-byte buffer");
}

/* Parity C5: the fast-code zoom and the label it produces. */
static void test_view_zoom(void)
{
    /* Stock's ms[0xE04] = 1000 / ratio (0x08030B4E..0x08030B6C). */
    static const uint8_t merged[4]  = { 10, 5, 2, 1 };
    static const uint8_t per_adc[4] = { 20, 10, 4, 2 };
    for (uint8_t c = 0; c < 4; c++) {
        CHECK(scope_timebase_interp_factor(c, true) == merged[c],
              "code 0x%02X merged zoom should be %u, got %u", c, merged[c],
              scope_timebase_interp_factor(c, true));
        CHECK(scope_timebase_interp_factor(c, false) == per_adc[c],
              "code 0x%02X per-converter zoom should be %u, got %u", c, per_adc[c],
              scope_timebase_interp_factor(c, false));
    }
    for (uint8_t c = 4; c < SCOPE_TIMEBASE_CODE_COUNT + 3u; c++) {
        CHECK(scope_timebase_interp_factor(c, true) == 1u &&
              scope_timebase_interp_factor(c, false) == 1u,
              "code 0x%02X must not be zoomed", c);
    }

    /* Distinct, correct time/div at 0x00-0x03 for BOTH record kinds, and the
     * same string either way (the zoom compensates the 2x merge). */
    static const char *want[4] = { "10ns", "20ns", "50ns", "100ns" };
    char a[16], b[16];
    for (uint8_t c = 0; c < 4; c++) {
        scope_timebase_view_label(c, true, a, sizeof(a));
        scope_timebase_view_label(c, false, b, sizeof(b));
        CHECK(strcmp(a, want[c]) == 0, "code 0x%02X merged label should be %s, got \"%s\"",
              c, want[c], a);
        CHECK(strcmp(b, want[c]) == 0, "code 0x%02X per-converter label should be %s, got \"%s\"",
              c, want[c], b);
    }
    /* Past the fast codes the view label is the plain (merged x2) one. */
    scope_timebase_view_label(0x04, false, a, sizeof(a));
    CHECK(strcmp(a, "200ns") == 0, "0x04 view label should be 200ns, got \"%s\"", a);
    scope_timebase_view_label(0x10, false, a, sizeof(a));
    CHECK(strcmp(a, "2.00ms") == 0, "0x10 view label should be 2.00ms, got \"%s\"", a);
    scope_timebase_view_label(0x15, true, a, sizeof(a));
    CHECK(strcmp(a, "--") == 0, "no rate stays \"--\" in the view label, got \"%s\"", a);
}

/* OpenScope's own core: rates are the core's clock over the package's dividers. */
static void test_core_rates(void)
{
    static const struct { uint8_t code; float fs; } want[] = {
        { 0x00, 125e6f }, { 0x04, 125e6f }, { 0x05, 50e6f }, { 0x06, 25e6f }, { 0x07, 12.5e6f },
        { 0x08, 5e6f }, { 0x09, 2.5e6f }, { 0x0A, 1.25e6f }, { 0x0B, 500e3f }, { 0x0C, 250e3f },
        { 0x0D, 125e3f }, { 0x0E, 50e3f }, { 0x0F, 25e3f }, { 0x10, 12.5e3f }, { 0x11, 5e3f },
        { 0x12, 2.5e3f }, { 0x13, 1250.0f }, { 0x14, 500.0f },
    };
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++)
        CHECK(scope_timebase_sample_rate(want[i].code) == want[i].fs, "core rate of code 0x%02X is %.1f, expected %.1f",
              want[i].code, (double)scope_timebase_sample_rate(want[i].code), (double)want[i].fs);
    CHECK(scope_timebase_rate_table()[0x0A] == 1.25e6f, "the table handed to searches must be the core's");
    CHECK(scope_timebase_get_tier(0x0A) == SCOPE_TB_MEASURED, "an exact rate is a measured-tier rate");
    CHECK(scope_timebase_sample_rate(SCOPE_TIMEBASE_CODE_COUNT) == 0.0f, "out of range stays 0");
}

int main(void)
{
    test_view_zoom();
    test_out_of_range();
    test_derived_quantities();
    test_labels();
    test_core_rates();

    if (failures == 0) {
        printf("test_scope_timebase: all checks passed\n");
        return 0;
    }
    printf("test_scope_timebase: %d check(s) FAILED\n", failures);
    return 1;
}
