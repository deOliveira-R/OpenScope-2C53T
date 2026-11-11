/*
 * OpenScope 2C53T — horizontal (time) calibration
 *
 * See scope_timebase.h for provenance, the tier rules, and why an R^2 alone is
 * not sufficient evidence for a sample rate. Free of hardware and RTOS
 * dependencies so it can be exercised on the host — see
 * tests/test_scope_timebase.c.
 */

#include "scope_timebase.h"

#include <stdio.h>

/*
 * Capture sample rate in S/s, indexed by reg-0x01 code, bench unit #1.
 * Source: docs/experiments/2026-08-18-10-time-axis.md and -12-.
 *
 * 0.0f means "no trustworthy rate", and the three reasons for that are
 * genuinely different — see the tier table below. Do not collapse them.
 *
 * RESCALED 2026-08-19 (EXP-14). Every rate here was previously 1.21x too high,
 * because the fits used the frequency COMMANDED from the ESP32 source while the
 * source delivered 0.8250x that. Its DDS loop reschedules from `now` after the
 * work is done, so its real rate is whatever two dacWrite() calls cost — 32,999
 * Hz against an assumed 40,000, measured against the ESP32's own crystal. The
 * generator now measures and reports its loop rate, and the fits divide by the
 * measured value. Consequences worth keeping in mind:
 *
 *   - the error was purely multiplicative, so the 1-2-5 SHAPE of this ladder,
 *     every R^2, and every fold check were untouched. Only the scale moved.
 *   - the corrected 0x10 lands 1.1% from Stlkv's independent figure (12,437),
 *     measured on a different unit with a different generator and a completely
 *     different method. That is this project's first cross-rig agreement on an
 *     ABSOLUTE quantity.
 *   - the round 1-2-5 ladder 12.5k / 25k / 50k / 125k sits inside our
 *     run-to-run spread on all four codes and is very likely the truth, but it
 *     is NOT written here. These are the measurements; fitting them to the
 *     round numbers we suspect would be inventing data.
 *
 * The AMPLITUDE calibration in scope_cal.c is untouched by this — the loop-rate
 * error moves frequency only. Absolute volts remain unverified.
 *
 * WITHDRAWN 2026-08-18 (EXP-12): code 0x08 was published at 1.07 kS/s, then at
 * 1,660 S/s (EXP-10), then fitted at 1,414 and 1,526 S/s in two passes minutes
 * apart. All are artifacts. The record at 0x08 does not reproduce between
 * reads — one unchanged tone gave peak bins [171, 132, 104] — and the fitted
 * rate missed its own fold predictions by up to 227 bins. It is entered here
 * as 0.0f, and the reason is INCOHERENT rather than UNMEASURED.
 */
static const float sample_rate[SCOPE_TIMEBASE_CODE_COUNT] = {
    /* 0x00 */  124830000.0f,   /* 2026-11-10 fold test = 0x04 (mask 3)    */
    /* 0x01 */  124830000.0f,   /* 2026-11-10 fold test = 0x04 (mask 3)    */
    /* 0x02 */  124830000.0f,   /* 2026-11-10 fold test = 0x04 (mask 3)    */
    /* 0x03 */  124830000.0f,   /* 2026-11-10 fold test = 0x04 (mask 3)    */
    /* 0x04 */  124830000.0f,   /* 2026-11-10 10 MHz tone, CAN bits agree  */
    /* 0x05 */          0.0f,   /* CONFLICT: tone 100M (AUTO), CAN 50M (NORMAL) */
    /* 0x06 */   24981200.0f,   /* 2026-11-10 CAN bit fit, NORMAL trigger  */
    /* 0x07 */   12494100.0f,   /* 2026-11-10 CAN bit fit, NORMAL trigger  */
    /* 0x08 */    4997500.0f,   /* 2026-11-10 CAN bit fit, NORMAL trigger  */
    /* 0x09 */    2498800.0f,   /* 2026-11-10 CAN bit fit, NORMAL trigger  */
    /* 0x0A */    1249600.0f,   /* 2026-11-10 CAN bit fit, NORMAL trigger  */
    /* 0x0B */      0.0f,        /* source too slow to measure (R2 -0.05)   */
    /* 0x0C */      0.0f,        /* source too slow to measure (R2  0.60)   */
    /* 0x0D */     123662.7f,   /* EXP-18 R2 0.9997 — provisional, bins 4-25 */
    /* 0x0E */       49930.1f,   /* EXP-17 slope fit, n=37, SE 31 S/s       */
    /* 0x0F */       24979.1f,   /* EXP-17 slope fit, n=29, SE 10 S/s       */
    /* 0x10 */       12490.0f,   /* EXP-17 slope fit, n=49, SE  2 S/s       */
    /* 0x11 */        4990.8f,   /* EXP-18 R2 1.0000                        */
    /* 0x12 */        2494.9f,   /* EXP-18 R2 1.0000, fold-tested           */
    /* 0x13 */        1250.4f,   /* EXP-18 R2 1.0000                        */
    /* 0x14 */         500.2f,   /* EXP-18 R2 1.0000                        */
};

/*
 * REVISED 2026-08-19 (EXP-17). The three measured rates moved by +1.78% /
 * -2.94% / -0.68%. The revision came out of validating the frequency
 * estimator, not out of looking for it: applied to a KNOWN drive, the
 * estimator's error was not scattered, it was constant within each timebase
 * code and different BETWEEN codes.
 *
 * That pattern is what assigns blame. A source-scale error -- the EXP-14 bug,
 * where the generator delivered 0.825x what it was told -- multiplies every
 * commanded frequency by one factor, so it moves every code the same
 * direction by the same amount. Here 0x0E needed +1.8% and 0x0F needed -2.9%,
 * opposite signs, which no single source factor can produce. And within one
 * code the error held constant from bin 6.8 to bin 246, a 36:1 span, so it is
 * not an interpolation artifact either (that varies with fractional bin).
 *
 * Method: 66 records (two ranges, five drive frequencies, two reps), peak
 * located by the shipped estimator, then fs from a least-squares fit of bin
 * against drive frequency. The fit is deliberately free-intercept rather than
 * through the origin, because the intercept turns out to be real: -0.045 bins,
 * the SAME on all three codes, a small fixed bias in parabolic interpolation
 * over a Hanning magnitude spectrum. It is worth only 0.03-0.11% here, but
 * fitting through the origin would have folded it into the slope.
 *
 * Four checks, all independent of the fit itself:
 *   - two DISJOINT drive sets (100/250/500/1000/2000 and 150/330/700/1500/
 *     3000 Hz) agree to 0.00%, 0.03% and 0.24%;
 *   - all three land within 0.14% of the round 1-2-5 ladder 12.5k/25k/50k;
 *   - averaging per-point ratios instead of fitting agrees to 0.11%;
 *   - 0x10 against Stlkv's independent rig (12,437 S/s, different unit,
 *     generator, firmware and method) improves from +1.11% to +0.43%.
 *
 * The last one is the one that matters: it is out-of-sample. Nothing in this
 * fit knows about his number, and the correction moved toward it.
 *
 * The round ladder is STILL NOT WRITTEN HERE, for the reason it was not
 * written in EXP-14: 12,490 is what was measured and 12,500 is what was
 * expected, and a table that records expectations is not evidence. The
 * agreement is now close enough that the ladder is almost certainly the
 * design intent -- but "almost certainly" belongs in a comment, not in a
 * float that later work will treat as data.
 *
 * Why the first ladder was wrong is NOT established. The leading candidate is
 * that it was fitted partly on torn records: it swept through `spi3 opread`,
 * whose ~17.5 ms /256 burst lets the engine lap the buffer, and which still
 * tears 6 reads in 12 (EXP-16). This fit used `spi3 read`, which after the
 * 2026-08-19 snapshot fix tears 0 in 12. That is a plausible mechanism and
 * nothing more; it has not been tested by re-fitting the old data.
 *
 * 0x0D (119,678 S/s) was NOT re-measured -- the bench source cannot reach far
 * enough above it to place a peak usefully. It very likely carries a similar
 * error and stays PROVISIONAL.
 */

/*
 * EXTENDED 2026-08-19 (EXP-18). The calibrated ladder went from 4 codes to 8.
 *
 * Codes 0x11-0x14 had never been swept. Every earlier session worked downward
 * from 0x10 and treated the unmeasured codes as a block, but the ones ABOVE
 * 0x10 are SLOWER, which makes them EASIER to measure than anything already
 * done -- more cycles per record, higher bins, better resolved. They were not
 * hard. Nobody had looked.
 *
 *   0x11  4,990.8   R2 1.0000
 *   0x12  2,494.9   R2 1.0000, fold-tested
 *   0x13  1,250.4   R2 1.0000
 *   0x14    500.2   R2 1.0000
 *
 * THE LADDER IS 1-2.5-5, NOT UNIFORM x2. Predicted 6,250 for 0x11 by assuming
 * each step doubles; measured 4,990.8. The real sequence is
 * 500 / 1250 / 2500 / 5000 / 12500 / 25000 / 50000 / 125000 -- ratios
 * 2.5, 2, 2, 2.5, 2, 2, 2.5. Every measured code lands within 0.2% of it,
 * except 0x0D (see below). Prediction beaten by measurement, again, and in
 * the direction that matters: the wrong prediction was the round-looking one.
 *
 * FOLD TEST at 0x12 (Nyquist 1,247 Hz): five tones from 1.4 to 3.0 kHz land
 * where 2,494.9 S/s says they alias to, worst miss 2.4 bins of 512. Aliasing
 * depends only on f/fs, so this is scale-invariant -- it tests the rate, not
 * the arithmetic that produced it.
 *
 * 0x0D RE-MEASURED, and it is the first evidence for EXP-17's untested
 * mechanism. The old 119,678 came from a sweep through `spi3 opread`, whose
 * slow /256 burst lets the engine lap the buffer (6 reads in 12 torn). Read
 * instead through `spi3 read` -- 0 in 12 torn since the snapshot fix -- the
 * same code gives 123,662.7 with R2 going 0.947 -> 0.9997, and its distance
 * from the round 125,000 drops from -4.26% to -1.07%. EXP-17 could only offer
 * torn records as a plausible story; this is a controlled-ish test of it (same
 * code, same rig, different read path) and it went the predicted way.
 *
 * It stays PROVISIONAL regardless: at ~124 kS/s the bench source only reaches
 * bin 25, so this is the least-resolved code in the table and -1.07% is a
 * bigger miss than every other code by a factor of five.
 *
 * NOT CHASED, worth recording: the INCOHERENT band 0x06-0x09 fitted to
 * 1,231 / 1,595 / 1,350 / 1,550 S/s -- all close to 0x13's 1,250. If the rate
 * field is narrower than the codes we write, or those codes select roll mode
 * rather than a rate, that would explain both the clustering and why the
 * numbers do not reproduce. Untested.
 */

/*
 * EXTENDED 2026-11-10 (unit #2, deOliveira-R bench). Codes 0x00-0x04 and
 * 0x06-0x0A now have rates. Two methods, two independent references.
 *
 * WHY 0x06-0x09 WERE "INCOHERENT". In AUTO with a quiet or untriggered input
 * the task commits free-running roll reads. op04 and op05 are read ~140 us
 * apart while the FPGA keeps writing, so the two channels of one committed
 * record are different moments, and successive reads do not reproduce.
 * Same-wire test: CH1 and CH2 clipped to ONE CAN_L node, sparse traffic
 * (1 frame / 100 ms): AUTO records show the frame on one channel and idle on
 * the other; NORMAL trigger (held, handed-over records only) agrees 99.7-100 %
 * sample by sample at 0x08 and 99.8 % at 0x04. The rates below were all taken
 * in NORMAL. The EXP-15 cluster near 1.25 kS/s for 0x06-0x09 is consistent
 * with fitting the roll cadence, as Stlkv predicted.
 *
 * Method A, 0x00-0x04: G431 TIM1 on PA8 = 170 MHz / 17 = 10.000 MHz (HSI,
 * +-1 %), square, into CH1, range 6. FFT peaks at 0x04: 0.0797 0.1604 0.2402
 * 0.4006 plus folded 80->45 / 70->55 MHz, i.e. every harmonic of 10 MHz at
 * 125 MS/s and none of them at 100 (which would put all on multiples of 0.1).
 * Refined fundamental over 6 records: f/fs = 0.08011 +- 0.00001 -> 124.83 MS/s
 * against the 10.000 MHz nominal. 0x00-0x03 give identical peak lists with
 * both channels on (mask 3), so per converter they run the same clock; the
 * single-channel interleave (mask 1, tb <= 3) doubles it and is not modelled
 * by this table.
 *
 * Method B, 0x04 and 0x06-0x0A: a 500 kbit/s CAN bus (G0B1 + G431, both on
 * HSI) as the time reference. Edge-to-edge gaps of the differential signal
 * (sub-sample threshold interpolation), each assigned its integer bit count,
 * least-squares samples/bit, fs = spb / 2.000 us. 12 records per code:
 *   0x06  49.962 +- 0.012 spb (51 gaps)  -> 24.981 MS/s
 *   0x07  24.988 +- 0.008     (113)      -> 12.494 MS/s
 *   0x08   9.995 +- 0.003     (252)      ->  4.9975 MS/s
 *   0x09   4.998 +- 0.001     (518)      ->  2.4988 MS/s
 *   0x0A   2.4993, rms 0.56  (1244 edges, up to 404 bits from the first
 *          edge of each record)    ->  1.2496 MS/s
 * At 2.5 samples/bit a one-bit gap is sampled as 2 or 3 and a three-bit gap
 * as 7 or 8, so the smallest-gap cluster (2.0) is NOT the bit: fitting it
 * gives 1.0 MS/s, which is wrong. Fitting cumulative edge positions against
 * integer bit counts resolves it; the CAN analyser (CRC-valid decodes at
 * 2.50 samples/bit) agrees.
 * The gap populations sit on clean 1/2/3-bit multiples (0x06: 49.9 / 99.8 /
 * 149.7), which is the fold check for this method. 0x04: 498 samples per two
 * bits = 124.5 MS/s, agreeing with method A to 0.3 % (the two HSI references).
 * Cross-method at 0x06: the method-A tone record there peaks at 0.4006 =
 * 10/25, consistent with 25 MS/s.
 *
 * Every row lands within 0.15 % of a 125M / 25M / 12.5M / 5M / 2.5M / 1.25M
 * ladder. The measured values are what is written, per this file's rule.
 *
 * 0x05 is left at 0: method A (in AUTO) puts the tone at f/fs = 0.1004 with
 * peaks at 0.1/0.3 (100 MS/s), method B (in NORMAL) gives gaps of 100.4 /
 * 199.7 / 299.7 / 499.5 samples = 1/2/3/5 bits at 50 MS/s. Unresolved.
 * 0x0B/0x0C are below one sample per CAN bit and stay unmeasured.
 */

/*
 * Why each code is where it is. Three distinct kinds of "no":
 *
 *   0x06-0x09     (HISTORICAL, superseded 2026-11-10 -- see the block above:
 *                 measured in NORMAL they are coherent.) INCOHERENT. Measured,
 *                 and the records do not reproduce.
 *                 EXP-15 (2026-08-19) widened this from 0x08 alone: fitting all
 *                 four codes gives 1,231 / 1,595 / 1,350 / 1,550 S/s — a 30%
 *                 band — where a real ladder step is 2-2.5x, and 40 Hz lands on
 *                 bin 28 in ALL FOUR. Read-to-read spread is 144-309 bins on a
 *                 512-bin spectrum. A number that does not change when the code
 *                 changes is not the device's rate; it is ours. Stlkv predicted
 *                 exactly this on issue #18 before it was run.
 *                 (His proposed MECHANISM — the fit returns our read cadence —
 *                 is NOT established: a round-trip opread takes ~800 ms, but
 *                 that is dominated by the hex dump over serial, and the SPI
 *                 burst at /256 is ~17.5 ms, which would predict ~58 kS/s.
 *                 Prediction confirmed, mechanism open.)
 *   0x0A-0x0C     UNMEASURABLE WITH OUR SOURCE. The ESP32 tops out near
 *                 4.5 kHz, which at these rates puts the tone in bins 1-15,
 *                 so the fit measures quantisation rather than rate. A
 *                 statement about our instrument — the device is probably
 *                 fine. A faster generator settles these.
 *   everything
 *   else at 0.0f  NEVER MEASURED.
 *
 * Conflating the three would be the same error as treating a floating pin and
 * a pin driven low as one thing, which cost this project weeks.
 */
static const scope_tb_tier_t tier_by_code[SCOPE_TIMEBASE_CODE_COUNT] = {
    SCOPE_TB_MEASURED, SCOPE_TB_MEASURED, SCOPE_TB_MEASURED, SCOPE_TB_MEASURED,
    SCOPE_TB_MEASURED,      /* 0x04 tone + CAN   */
    SCOPE_TB_NONE,          /* 0x05 conflict     */
    SCOPE_TB_MEASURED,      /* 0x06 CAN          */
    SCOPE_TB_MEASURED,      /* 0x07 CAN          */
    SCOPE_TB_MEASURED,      /* 0x08 CAN          */
    SCOPE_TB_MEASURED,      /* 0x09 CAN          */
    SCOPE_TB_MEASURED,      /* 0x0A CAN          */
    SCOPE_TB_NONE,          /* 0x0B unmeasurable */
    SCOPE_TB_NONE,          /* 0x0C unmeasurable */
    SCOPE_TB_PROVISIONAL,   /* 0x0D              */
    SCOPE_TB_MEASURED,      /* 0x0E              */
    SCOPE_TB_MEASURED,      /* 0x0F              */
    SCOPE_TB_MEASURED,      /* 0x10              */
    SCOPE_TB_MEASURED,      /* 0x11 EXP-18       */
    SCOPE_TB_MEASURED,      /* 0x12 EXP-18       */
    SCOPE_TB_MEASURED,      /* 0x13 EXP-18       */
    SCOPE_TB_MEASURED,      /* 0x14 EXP-18       */
};

float scope_timebase_sample_rate(uint8_t code)
{
    if (code >= SCOPE_TIMEBASE_CODE_COUNT)
        return 0.0f;
    return sample_rate[code];
}

scope_tb_tier_t scope_timebase_get_tier(uint8_t code)
{
    if (code >= SCOPE_TIMEBASE_CODE_COUNT)
        return SCOPE_TB_NONE;

    /* A code with no rate cannot be anything but NONE, whatever the tier
     * table says. Derive the guard rather than trusting the two to agree. */
    if (sample_rate[code] <= 0.0f)
        return SCOPE_TB_NONE;

    return tier_by_code[code];
}

float scope_timebase_seconds_per_sample(uint8_t code)
{
    const float fs = scope_timebase_sample_rate(code);
    return (fs > 0.0f) ? (1.0f / fs) : 0.0f;
}

float scope_timebase_seconds_per_div(uint8_t code)
{
    return scope_timebase_seconds_per_sample(code) *
           SCOPE_TIMEBASE_SAMPLES_PER_DIV;
}

float scope_timebase_hz_from_period(uint8_t code, float period_samples)
{
    const float fs = scope_timebase_sample_rate(code);

    if (fs <= 0.0f || period_samples <= 0.0f)
        return 0.0f;

    return fs / period_samples;
}

void scope_timebase_label(uint8_t code, char *out, uint32_t n)
{
    if (out == NULL || n == 0u)
        return;

    const float s = scope_timebase_seconds_per_div(code);

    if (s <= 0.0f) {
        /* No rate. The grid has no time meaning on this code, and saying so is
         * the point — a nominal label here would be the defect this module
         * exists to remove. */
        snprintf(out, n, "--");
        return;
    }

    const char *mark =
        (scope_timebase_get_tier(code) == SCOPE_TB_PROVISIONAL) ? "~" : "";

    /* Integer formatting throughout: the shell and the LCD both build against
     * nano.specs, which has no float printf. */
    if (s < 1e-6f) {
        snprintf(out, n, "%s%uns", mark, (unsigned)(s * 1e9f + 0.5f));
    } else if (s < 1e-5f) {
        const unsigned tenths_us = (unsigned)(s * 1e7f + 0.5f);
        snprintf(out, n, "%s%u.%uus", mark, tenths_us / 10u, tenths_us % 10u);
    } else if (s < 1e-3f) {
        snprintf(out, n, "%s%uus", mark, (unsigned)(s * 1e6f + 0.5f));
    } else if (s < 1.0f) {
        const unsigned hundredths_ms = (unsigned)(s * 1e5f + 0.5f);
        snprintf(out, n, "%s%u.%02ums", mark,
                 hundredths_ms / 100u, hundredths_ms % 100u);
    } else {
        const unsigned hundredths_s = (unsigned)(s * 100.0f + 0.5f);
        snprintf(out, n, "%s%u.%02us", mark,
                 hundredths_s / 100u, hundredths_s % 100u);
    }
}
