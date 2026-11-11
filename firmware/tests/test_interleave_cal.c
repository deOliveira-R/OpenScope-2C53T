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
 */
#include "../src/dsp/interleave_cal.h"
#include "../src/dsp/interleave.h"

#include <math.h>
#include <stdio.h>

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
        interleave_corr_t c = { f.gain, f.offset };
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
    p[2].a_p2p = 20;
    CHECK(il_cal_fit(p, 5, &prm, &f) == IL_CAL_NOT_FLAT, "p2p not refused");
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

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("test_interleave_cal: all passed\n");
    return 0;
}
