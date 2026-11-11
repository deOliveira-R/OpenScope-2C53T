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

static void test_measured_codes(void)
{
    const struct { uint8_t code; float fs; } want[] = {
        { 0x0E, 49930.1f }, { 0x0F, 24979.1f }, { 0x10, 12490.0f },
    };

    for (unsigned i = 0; i < 3; i++) {
        CHECK(fabsf(scope_timebase_sample_rate(want[i].code) - want[i].fs) < 1.0f,
              "code 0x%02X: expected %.1f S/s, got %.1f", want[i].code,
              (double)want[i].fs,
              (double)scope_timebase_sample_rate(want[i].code));
        CHECK(scope_timebase_get_tier(want[i].code) == SCOPE_TB_MEASURED,
              "code 0x%02X should be MEASURED", want[i].code);
    }

    /*
     * EXP-18: the four slow codes, which had never been swept because every
     * session worked downward from 0x10. They are the best-resolved rows in
     * the table (R2 1.0000 each).
     */
    const struct { uint8_t code; float fs; } slow[] = {
        { 0x11, 4990.8f }, { 0x12, 2494.9f },
        { 0x13, 1250.4f }, { 0x14, 500.2f },
    };
    for (unsigned i = 0; i < 4; i++) {
        CHECK(fabsf(scope_timebase_sample_rate(slow[i].code) - slow[i].fs) < 1.0f,
              "code 0x%02X: expected %.1f S/s, got %.1f", slow[i].code,
              (double)slow[i].fs,
              (double)scope_timebase_sample_rate(slow[i].code));
        CHECK(scope_timebase_get_tier(slow[i].code) == SCOPE_TB_MEASURED,
              "code 0x%02X should be MEASURED", slow[i].code);
    }

    /*
     * The ladder is 1-2.5-5, NOT uniform x2 -- predicting 6,250 for 0x11 by
     * assuming each step doubles was wrong by 25%. Encode the real shape so a
     * future "correction" toward a uniform ladder fails here.
     */
    const struct { uint8_t lo, hi; float ratio; } steps[] = {
        { 0x14, 0x13, 2.5f }, { 0x13, 0x12, 2.0f }, { 0x12, 0x11, 2.0f },
        { 0x11, 0x10, 2.5f }, { 0x10, 0x0F, 2.0f }, { 0x0F, 0x0E, 2.0f },
    };
    for (unsigned i = 0; i < 6; i++) {
        const float got = scope_timebase_sample_rate(steps[i].hi) /
                          scope_timebase_sample_rate(steps[i].lo);
        CHECK(fabsf(got - steps[i].ratio) < 0.05f,
              "0x%02X/0x%02X should step by %.1fx, got %.3fx",
              steps[i].hi, steps[i].lo, (double)steps[i].ratio, (double)got);
    }

    /* The ladder roughly doubles downward; a transcription slip that swapped
     * two rows would break this. */
    CHECK(scope_timebase_sample_rate(0x0F) > 1.8f * scope_timebase_sample_rate(0x10),
          "0x0F should be about double 0x10");
    CHECK(scope_timebase_sample_rate(0x0E) > 1.8f * scope_timebase_sample_rate(0x0F),
          "0x0E should be about double 0x0F");
}

static void test_code_08_artifacts_stay_out(void)
{
    /*
     * 0x08 has had three published rates -- 1.07 kS/s, 1,660 S/s, and a
     * two-pass 1,414/1,526 -- all fitted to AUTO roll reads that do not
     * reproduce. It now holds the 2026-11-10 NORMAL-trigger CAN-bit fit
     * (4.9975 MS/s). Re-entering any artifact fails the build, and so does
     * dropping back to a kS/s-scale number.
     */
    const float artifacts[] = { 1070.0f, 1660.0f, 1414.0f, 1526.0f };
    const float got = scope_timebase_sample_rate(0x08);
    for (unsigned i = 0; i < 4; i++)
        CHECK(fabsf(got - artifacts[i]) > 1.0f,
              "code 0x08 is back at the withdrawn artifact %.0f S/s", (double)artifacts[i]);
    CHECK(got > 4.9e6f && got < 5.1e6f,
          "code 0x08 should hold the CAN-bit measurement (~5.0 MS/s), got %.1f", (double)got);
    CHECK(scope_timebase_get_tier(0x08) == SCOPE_TB_MEASURED, "code 0x08 must be MEASURED");
}

static void test_fast_codes_2026_11(void)
{
    /*
     * 2026-11-10: method A (10.000 MHz tone, fold-checked) for 0x00-0x04,
     * method B (500 kbit/s CAN bits, NORMAL trigger) for 0x06-0x0A. Values are
     * what was measured, not the round ladder they sit within 0.15 % of.
     */
    const struct { uint8_t code; float fs, round_val; } want[] = {
        { 0x00, 124830000.0f, 125e6f }, { 0x03, 124830000.0f, 125e6f },
        { 0x04, 124830000.0f, 125e6f }, { 0x05,  49900000.0f,  50e6f },
        { 0x06,  24981200.0f,  25e6f },
        { 0x07,  12494100.0f, 12.5e6f }, { 0x08,   4997500.0f,   5e6f },
        { 0x09,   2498800.0f,  2.5e6f }, { 0x0A,   1249600.0f, 1.25e6f },
    };
    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        const float got = scope_timebase_sample_rate(want[i].code);
        CHECK(fabsf(got / want[i].fs - 1.0f) < 1e-6f, "code 0x%02X: expected %.0f, got %.0f",
              want[i].code, (double)want[i].fs, (double)got);
        CHECK(got != want[i].round_val, "code 0x%02X was rounded to %.0f", want[i].code,
              (double)want[i].round_val);
        CHECK(fabsf(got / want[i].round_val - 1.0f) < 0.005f,
              "code 0x%02X drifted >0.5%% from its ladder value", want[i].code);
        CHECK(scope_timebase_get_tier(want[i].code) == SCOPE_TB_MEASURED,
              "code 0x%02X should be MEASURED", want[i].code);
    }
    /* 0x05 was briefly read as 100 MS/s from an AUTO roll read; stock's own
     * record settles it at ~50. */
    CHECK(scope_timebase_sample_rate(0x05) < 60e6f, "code 0x05 must not be the 100 MS/s roll-read artifact");
}

static void test_precorrection_rates_stay_out(void)
{
    /*
     * EXP-14: the first published ladder (150706 / 62958 / 30235 / 14853.6)
     * was fitted against frequencies COMMANDED from a source that delivered
     * 0.8250x them. The numbers were internally consistent, linear, and
     * fold-checked — and every one was 1.21x too high. If a merge or a revert
     * puts them back, fail here rather than on someone's bench.
     */
    const struct { uint8_t code; float bad; } gone[] = {
        { 0x0D, 150706.0f }, { 0x0E, 62958.0f },
        { 0x0F, 30235.0f },  { 0x10, 14853.6f },
    };

    for (unsigned i = 0; i < 4; i++) {
        CHECK(fabsf(scope_timebase_sample_rate(gone[i].code) - gone[i].bad) > 1.0f,
              "code 0x%02X is back at its pre-correction rate %.1f — the source "
              "scale error has been reintroduced", gone[i].code,
              (double)gone[i].bad);
    }

    /* And the corrected ladder must stay near the round 1-2-5 values that an
     * independent rig reports, without being equal to them. */
    CHECK(scope_timebase_sample_rate(0x10) > 11800.0f &&
          scope_timebase_sample_rate(0x10) < 13200.0f,
          "0x10 should sit near 12.5 kS/s, got %.1f",
          (double)scope_timebase_sample_rate(0x10));

    /*
     * EXP-17: the pre-EXP-17 ladder is ALSO out, for the same reason as the
     * pre-EXP-14 one. It was internally consistent and fold-checked, and it
     * was still 0.7-2.9% off per code -- an error that could not be seen
     * until the estimator was pointed at a known drive. Superseded values do
     * not get to come back on a merge.
     */
    const struct { uint8_t code; float bad; } superseded[] = {
        { 0x0E, 49056.0f }, { 0x0F, 25736.0f }, { 0x10, 12575.0f },
    };
    for (unsigned i = 0; i < 3; i++) {
        CHECK(fabsf(scope_timebase_sample_rate(superseded[i].code) -
                    superseded[i].bad) > 1.0f,
              "code 0x%02X is back at its pre-EXP-17 rate %.1f",
              superseded[i].code, (double)superseded[i].bad);
    }

    /*
     * And the ladder must stay MEASURED, not rounded. If someone "tidies"
     * these to 12500/25000/50000 the table stops being evidence -- the
     * agreement with the round ladder is the finding, so writing the round
     * ladder in would make the finding unfalsifiable.
     */
    const struct { uint8_t code; float round_val; } tidy[] = {
        { 0x0E, 50000.0f }, { 0x0F, 25000.0f }, { 0x10, 12500.0f },
    };
    for (unsigned i = 0; i < 3; i++) {
        const float got = scope_timebase_sample_rate(tidy[i].code);
        CHECK(got != tidy[i].round_val,
              "code 0x%02X was rounded to the expected %.0f — these rows must "
              "hold what was measured", tidy[i].code, (double)tidy[i].round_val);
        CHECK(fabsf(got / tidy[i].round_val - 1.0f) < 0.01f,
              "code 0x%02X drifted >1%% from the round ladder it should be "
              "converging on (%.1f vs %.0f)", tidy[i].code,
              (double)got, (double)tidy[i].round_val);
    }
}

static void test_unmeasured_codes_return_zero(void)
{
    const uint8_t none[] = { 0x0B, 0x0C };

    for (unsigned i = 0; i < sizeof(none) / sizeof(none[0]); i++) {
        CHECK(scope_timebase_sample_rate(none[i]) == 0.0f,
              "code 0x%02X has no measured rate and must return 0.0f", none[i]);
        CHECK(scope_timebase_get_tier(none[i]) == SCOPE_TB_NONE,
              "code 0x%02X must be tier NONE", none[i]);
    }
}

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
        CHECK(fabsf(scope_timebase_seconds_per_div(c) -
                    (SCOPE_TIMEBASE_SAMPLES_PER_DIV / fs)) < 1e-12f,
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

    /* 24979 S/s -> 1.0008 ms/div. */
    scope_timebase_label(0x0F, buf, sizeof(buf));
    CHECK(strcmp(buf, "1.00ms") == 0,
          "0x0F label should be 1.00ms, got \"%s\"", buf);

    /* 49930 S/s -> 501 us/div, sub-millisecond so microseconds. */
    scope_timebase_label(0x0E, buf, sizeof(buf));
    CHECK(strcmp(buf, "501us") == 0,
          "0x0E label should be 501us, got \"%s\"", buf);

    /* Provisional carries the marker. 0x0D was re-measured in EXP-18 (R2
     * 0.9997, up from 0.947) but stays provisional: at ~124 kS/s the bench
     * source only reaches bin 25, so it is the least-resolved row and misses
     * the round ladder by 1.07% where every other code is inside 0.2%. */
    scope_timebase_label(0x0D, buf, sizeof(buf));
    CHECK(buf[0] == '~', "0x0D is PROVISIONAL and must be marked, got \"%s\"", buf);

    /* No rate -> explicit nothing, never a nominal number. */
    scope_timebase_label(0x0B, buf, sizeof(buf));
    CHECK(strcmp(buf, "--") == 0, "0x0B label should be \"--\", got \"%s\"", buf);

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
    scope_timebase_label_scaled(0x0B, 2u, buf, sizeof(buf));
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
    scope_timebase_view_label(0x0B, true, a, sizeof(a));
    CHECK(strcmp(a, "--") == 0, "no rate stays \"--\" in the view label, got \"%s\"", a);
}

int main(void)
{
    test_view_zoom();
    test_measured_codes();
    test_code_08_artifacts_stay_out();
    test_fast_codes_2026_11();
    test_precorrection_rates_stay_out();
    test_unmeasured_codes_return_zero();
    test_out_of_range();
    test_derived_quantities();
    test_labels();

    if (failures == 0) {
        printf("test_scope_timebase: all checks passed\n");
        return 0;
    }
    printf("test_scope_timebase: %d check(s) FAILED\n", failures);
    return 1;
}
