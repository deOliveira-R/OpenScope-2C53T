/*
 * Host tests for the frequency estimator, driven by REAL BENCH RECORDS.
 *
 * This is the acceptance test EXP-13 asked for. It does not check the
 * estimator against a synthetic sine — a synthetic sine is exactly the case
 * that always worked. It runs the shipped code over 72 captures taken from
 * bench unit #1 on 2026-08-19, across two vertical ranges, three timebase
 * codes, five frequencies and seven amplitudes, with the signal generator
 * corrected (EXP-14) so the recorded drive frequency is DELIVERED rather than
 * merely commanded.
 *
 * The contract being tested is not "always answers". It is:
 *
 *   - when it answers, it is within 5% of the true drive;
 *   - it NEVER answers wrongly, which is what got the previous badge reverted;
 *   - it refuses on torn records rather than guessing.
 *
 * A refusal rate is reported, not asserted, so that improving coverage is
 * visible without loosening correctness.
 */

#include "../src/ui/scope_freq.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <unistd.h>

#define MAXN 1024

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

/* ── synthetic sanity, before touching real data ──────────────────────── */

static void test_synthetic(void)
{
    uint8_t s[MAXN];
    scope_freq_t r;

    /* A clean 500 Hz sine at 12,575 S/s: bin 40.7, unambiguous. */
    for (int i = 0; i < MAXN; i++)
        s[i] = (uint8_t)(128.0 + 90.0 * sin(2.0 * M_PI * 500.0 * i / 12575.0));

    CHECK(scope_freq_estimate(s, MAXN, 12575.0f, &r),
          "a clean synthetic sine must be accepted");
    CHECK(fabsf(r.hz - 500.0f) < 5.0f,
          "clean 500 Hz sine read as %.1f Hz", (double)r.hz);
    CHECK(r.sharpness > 0.95f, "a clean sine should be very sharp, got %.3f",
          (double)r.sharpness);
    CHECK(r.window == MAXN, "a clean record must not need the half-window");

    /* EXP-67 (unit #3, 2026-10-01): a clean 44-count square at 800 Hz,
     * 24,979 S/s (fundamental at bin 32.8, BETWEEN bins) refused 5 of 6 real
     * records at sharpness 0.73-0.90. With the harmonic windows at integer
     * multiples of the integer peak bin (33h) the comb drifts off the
     * square's harmonics by 0.2 bin per order and the >= 7th harmonics are
     * counted as noise: 0.90-0.91 on a perfect record, so quantisation alone
     * tips it. Windows on the interpolated comb (32.8h) read 0.93-0.97 on the
     * same records. Deterministic +/-0.5-count quantisation noise below (an
     * LCG, so the record is the same every run). Revert the comb placement
     * and the sharpness check fails. */
    {
        uint32_t lcg = 0x2C53u;
        for (int i = 0; i < MAXN; i++) {
            double ph = fmod(800.0 * i / 24979.0, 1.0);
            lcg = lcg * 1664525u + 1013904223u;
            int q = (int)((lcg >> 24) % 2u);          /* 0 or 1 count of noise */
            s[i] = (uint8_t)((ph < 0.5 ? 150 : 106) + q);
        }
        /* The bench records also carry one sample of edge overshoot (probe,
         * ESD clamp): +/-6 counts right after each edge. */
        for (int i = 1; i < MAXN; i++) {
            if (s[i] > s[i - 1] + 20) s[i] = (uint8_t)(s[i] + 6);
            else if (s[i] + 20 < s[i - 1]) s[i] = (uint8_t)(s[i] - 6);
        }
        CHECK(scope_freq_estimate(s, MAXN, 24979.0f, &r),
              "a clean 44-count square between bins must be accepted (EXP-67)");
        CHECK(fabsf(r.hz - 800.0f) < 8.0f,
              "44-count 800 Hz square read as %.1f Hz", (double)r.hz);
        CHECK(r.sharpness >= 0.95f,
              "harmonic windows must follow the fractional comb: sharpness %.3f "
              "(windows at integer multiples of the integer peak give ~0.92 here "
              "and 0.73-0.90 on the bench records)", (double)r.sharpness);
    }

    /* A clean 50% square at 500 Hz, 12,575 S/s: same fundamental bin as the
     * sine above (40.7). An ideal square's fundamental carries only 8/pi^2 =
     * 0.811 of the total power, so the old mainlobe-only sharpness capped here
     * at ~0.81 and refused every square, however clean (issue #27). The
     * harmonic-aware metric counts the odd-harmonic mainlobes as signal, so a
     * clean square now clears the 0.90 gate. These checks have teeth: revert
     * the harmonic logic and sharpness drops back to ~0.81, failing both. */
    for (int i = 0; i < MAXN; i++) {
        double ph = fmod(500.0 * i / 12575.0, 1.0);
        s[i] = (ph < 0.5) ? 210 : 46;
    }
    CHECK(scope_freq_estimate(s, MAXN, 12575.0f, &r),
          "a clean square must be accepted (issue #27)");
    CHECK(fabsf(r.hz - 500.0f) < 5.0f,
          "clean 500 Hz square read as %.1f Hz", (double)r.hz);
    CHECK(r.sharpness >= 0.90f,
          "harmonic-aware sharpness must clear the gate for a square, got %.3f "
          "(mainlobe-only would be ~0.81)", (double)r.sharpness);

    /* DC: no frequency exists, and inventing one is the failure mode this
     * whole module is a response to. */
    memset(s, 128, sizeof(s));
    CHECK(!scope_freq_estimate(s, MAXN, 12575.0f, &r), "flat DC must be refused");
    CHECK(r.hz == 0.0f, "a refusal must leave hz at exactly 0");

    /* No sample rate -> no frequency. */
    for (int i = 0; i < MAXN; i++)
        s[i] = (uint8_t)(128.0 + 90.0 * sin(2.0 * M_PI * 500.0 * i / 12575.0));
    CHECK(!scope_freq_estimate(s, MAXN, 0.0f, &r),
          "a timebase code with no rate must yield no frequency");

    /* Bad geometry is refused rather than clamped. */
    CHECK(!scope_freq_estimate(s, 1000, 12575.0f, &r), "non-power-of-two refused");
    CHECK(!scope_freq_estimate(s, 32, 12575.0f, &r),   "too-short refused");
    CHECK(!scope_freq_estimate(NULL, MAXN, 12575.0f, &r), "NULL refused");
    CHECK(!scope_freq_estimate(s, MAXN, 12575.0f, NULL), "NULL out refused");
}

/* ── the real records ─────────────────────────────────────────────────── */

static void test_bench_records(const char *path)
{
    FILE *fh = fopen(path, "r");
    if (fh == NULL) {
        printf("FAIL: cannot open %s\n", path);
        failures++;
        return;
    }

    int c;
    while ((c = fgetc(fh)) == '#') {          /* skip comment lines */
        while ((c = fgetc(fh)) != '\n' && c != EOF) { }
    }
    ungetc(c, fh);

    int count = 0;
    if (fscanf(fh, "%d", &count) != 1 || count <= 0) {
        printf("FAIL: %s has no record count\n", path);
        failures++;
        fclose(fh);
        return;
    }

    uint8_t s[MAXN];
    int answered = 0, refused = 0, wrong = 0;
    int hi_bin = 0, hi_bin_bad = 0;
    float worst = 0.0f, worst_hi = 0.0f;

    for (int rec = 0; rec < count; rec++) {
        int range, code, drive, span;
        float fs;
        if (fscanf(fh, "%d %d %f %d %d", &range, &code, &fs, &drive, &span) != 5) {
            printf("FAIL: %s truncated at record %d\n", path, rec);
            failures++;
            break;
        }
        for (int i = 0; i < MAXN; i++) {
            int v = 0;
            if (fscanf(fh, "%d", &v) != 1) v = 0;
            s[i] = (uint8_t)v;
        }

        scope_freq_t r;
        if (!scope_freq_estimate(s, MAXN, fs, &r)) {
            refused++;
            CHECK(r.hz == 0.0f,
                  "record %d refused but hz = %.1f — a refusal must be silent, "
                  "not quiet", rec, (double)r.hz);
            continue;
        }

        answered++;
        const float err = fabsf(r.hz - (float)drive) / (float)drive * 100.0f;
        if (err > worst) worst = err;

        /*
         * Stratify by bin. Relative precision of a peak search scales with
         * bin number -- one bin of slop at bin 6 is 16%, at bin 60 it is 1.6%
         * -- so a single global bound is dominated by the few records nearest
         * the MIN_BIN floor and says almost nothing about the rest. Measured
         * 2026-08-19 on held-out records: every error above 1% sat at bin ~6,
         * and every record at bin >= 13 came in under 0.31%.
         */
        if (r.bin >= 13.0f) {
            hi_bin++;
            if (err > worst_hi) worst_hi = err;
            if (err > 1.0f) {
                hi_bin_bad++;
                printf("  HI-BIN MISS: code 0x%02X bin %.1f, %d Hz -> %.1f Hz "
                       "(%+.2f%%)\n", code, (double)r.bin, drive,
                       (double)r.hz, (double)err);
            }
        }
        if (err > 5.0f) {
            wrong++;
            printf("  WRONG: range %d code 0x%02X span %d, %d Hz -> %.1f Hz "
                   "(%+.1f%%, sharpness %.2f, window %u)\n",
                   range, code, span, drive, (double)r.hz, (double)err,
                   (double)r.sharpness, r.window);
        }
    }
    fclose(fh);

    printf("  bench records: %d answered, %d refused, %d wrong, worst %.1f%%\n",
           answered, refused, wrong, (double)worst);
    printf("  of those, %d at bin >= 13: worst %.2f%%\n", hi_bin, (double)worst_hi);

    /* THE contract. The previous badge was reverted for producing confident
     * wrong numbers; producing none is acceptable, producing wrong ones is
     * not. */
    CHECK(wrong == 0, "%d record(s) answered with more than 5%% error", wrong);
    CHECK(worst <= 5.0f, "worst error %.1f%% exceeds the 5%% contract",
          (double)worst);

    /*
     * The sharper contract, and the one that would catch a rate-table
     * regression. EXP-17 corrected the sample rates by 0.7-2.9%; before that
     * correction this bound was 3.1% and the errors were CONSTANT within each
     * timebase code -- the signature of a wrong rate rather than a weak
     * estimator. If a future edit puts a wrong rate back, the global 5% bound
     * would still pass and this one would not.
     */
    CHECK(hi_bin_bad == 0,
          "%d record(s) at bin >= 13 missed by more than 1%% — at those bins "
          "the estimator resolves to ~0.3%%, so this points at the sample "
          "rate, not the peak search", hi_bin_bad);
    CHECK(hi_bin >= 30,
          "only %d records landed at bin >= 13 — the fixture set no longer "
          "exercises the well-resolved regime", hi_bin);

    /* Coverage is a quality target, not a correctness one — but a collapse to
     * near-zero answers would mean a regression that the `wrong == 0` check
     * alone would happily pass. Measured coverage was 39/72; allow slack. */
    CHECK(answered * 100 >= count * 80,
          "only %d of %d records answered — coverage regressed (both fixture "
          "sets measured 87-88%%)",
          answered, count);
}

/*
 * Records holding a single edge (a period longer than the record, e.g. a
 * 100 kHz square at 125 MS/s, saturated at 0/255): the peak sits at bin 1,
 * whose left neighbour is bin 0. That neighbour was the raw DC real part,
 * a tiny residual whose sign depends on rounding; negative, it made the
 * interpolation NaN and the harmonic loop never ended -- the display task
 * hung on the unit. Every record must return, refused or with a finite bin.
 */
static void on_hang(int sig)
{
    (void)sig;
    static const char msg[] = "FAIL: scope_freq_estimate did not return (single-edge record)\n";
    (void)!write(1, msg, sizeof(msg) - 1);
    _exit(1);
}

static void test_single_edge_terminates(void)
{
    static const int lv[][2] = { {0, 255}, {255, 0}, {100, 227}, {98, 227},
                                 {127, 128}, {0, 1}, {128, 128} };
    static uint8_t rec[MAXN];
    unsigned n = 0, answered = 0;
    signal(SIGALRM, on_hang);
    alarm(20);
    for (unsigned l = 0; l < sizeof(lv) / sizeof(lv[0]); l++)
        for (unsigned e = 0; e <= MAXN; e += 7) {
            for (unsigned i = 0; i < MAXN; i++)
                rec[i] = (uint8_t)(i < e ? lv[l][0] : lv[l][1]);
            for (unsigned w = 0; w < 2; w++, n++) {
                const uint16_t len = w ? MAXN / 2 : MAXN;
                scope_freq_t f;
                const bool ok = scope_freq_estimate(rec, len, 125e6f, &f);
                CHECK(isfinite(f.hz) && isfinite(f.bin) && isfinite(f.sharpness),
                      "edge %u levels %d/%d n %u: non-finite result", e, lv[l][0], lv[l][1], len);
                if (ok) answered++;
            }
        }
    alarm(0);
    printf("single-edge records: %u estimated, %u answered (all returned)\n", n, answered);
}

/* An open input: a few counts of noise, even with a periodic component
 * (pickup), is not a signal and gets no frequency. Just above the floor a
 * clean tone is still measured. */
static void test_no_signal_no_frequency(void)
{
    static uint8_t rec[MAXN];
    scope_freq_t f;
    srand(7);
    for (unsigned i = 0; i < MAXN; i++)
        rec[i] = (uint8_t)(100 + lroundf(2.0f * sinf(6.2831853f * (float)i / 17.0f)) + rand() % 2);
    CHECK(!scope_freq_estimate(rec, MAXN, 1.25e6f, &f) && f.hz == 0.0f,
          "open input (5 counts p-p, a clean 17-sample line in it): no frequency, got %.0f Hz", f.hz);
    for (unsigned i = 0; i < MAXN; i++)
        rec[i] = (uint8_t)(100 + lroundf(6.0f * sinf(6.2831853f * (float)i / 17.0f)));
    CHECK(scope_freq_estimate(rec, MAXN, 1.25e6f, &f) && fabsf(f.hz - 1.25e6f / 17.0f) < 0.02f * 1.25e6f / 17.0f,
          "12-count tone at 17 samples: measured (%.0f Hz)", f.hz);
}

int main(int argc, char **argv)
{
    test_single_edge_terminates();
    test_no_signal_no_frequency();
    const char *path = (argc > 1) ? argv[1]
                                  : "tests/fixtures/scope_records.txt";
    test_synthetic();
    test_bench_records(path);

    /*
     * The held-out set. MIN_BIN and the sharpness floor were swept on
     * scope_records.txt, which makes that file training data; passing on it
     * shows only that the tuning took. These records were captured
     * afterwards at drive frequencies chosen to be disjoint from it.
     */
    if (argc <= 1)
        test_bench_records("tests/fixtures/scope_records_heldout.txt");

    if (failures == 0) {
        printf("test_scope_freq: all checks passed\n");
        return 0;
    }
    printf("test_scope_freq: %d check(s) FAILED\n", failures);
    return 1;
}
