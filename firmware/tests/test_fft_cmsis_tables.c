/*
 * Host test: the firmware's CMSIS FFT path with the trimmed table set.
 *
 * The image builds CMSIS-DSP with ARM_DSP_CONFIG_TABLES and only the three
 * tables a 4096-point real FFT needs (Makefile, FFT_TABLE_DEFS). This test
 * compiles src/dsp/fft.c (USE_CMSIS_DSP) and the same CMSIS sources with the
 * same defines and checks:
 *   - arm_rfft_fast_init_f32() accepts 4096 and refuses every other length
 *     (their tables are not built), so a wrong FFT_SIZE cannot pass silently;
 *   - fft_init() reports the engine as usable, and a pure tone lands in the
 *     bin it must, for several bins across the span, with the image far
 *     below it;
 *   - the raw transform agrees with a direct DFT at a few bins (the tables
 *     are the right ones, not merely self-consistent).
 *
 * Built a second time WITHOUT the real FFT's twiddle table
 * (-DEXPECT_ENGINE_FAIL): the init must then refuse 4096 too, and fft.c must
 * say so and transform nothing: no sample buffer, fft_process() leaves the
 * result alone. That is what a wrong table selection looks like in the
 * image, so it has to be this visible.
 *
 * The one piece not taken from the tree is arm_bitreversal_32: the firmware
 * links the Cortex-M assembly (arm_bitreversal2.S), which a host cannot
 * assemble, so the same swap loop is written out below in C.
 *
 * Build/run:  make -C firmware test-fft-cmsis-tables
 */
#include "fft.h"
#include "arm_math.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails, checks;
#define CHECKF(c, ...) do { checks++; if (!(c)) { fails++; \
    printf("FAIL %s:%d  %s: ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define CHECK(c) CHECKF(c, "%s", "")

/* CMSIS arm_bitreversal2 in C: swap the pairs the table lists (the table
 * holds byte offsets of 32-bit words for the assembly, hence >> 2). */
void arm_bitreversal_32(uint32_t *pSrc, const uint16_t bitRevLen, const uint16_t *pBitRevTab)
{
    for (uint32_t i = 0; i < bitRevLen; i += 2u) {
        const uint32_t a = pBitRevTab[i] >> 2, b = pBitRevTab[i + 1u] >> 2;
        uint32_t t = pSrc[a];     pSrc[a] = pSrc[b];         pSrc[b] = t;
        t = pSrc[a + 1u];         pSrc[a + 1u] = pSrc[b + 1u]; pSrc[b + 1u] = t;
    }
}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void test_init_lengths(void)
{
    static const uint16_t lens[] = { 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 1000 };
    for (unsigned i = 0; i < sizeof lens / sizeof lens[0]; i++) {
        arm_rfft_fast_instance_f32 S;
        memset(&S, 0, sizeof S);
        const arm_status st = arm_rfft_fast_init_f32(&S, lens[i]);
#ifdef EXPECT_ENGINE_FAIL
        CHECKF(st != ARM_MATH_SUCCESS, "length %u accepted without its tables", (unsigned)lens[i]);
        continue;
#endif
        if (lens[i] == FFT_SIZE) CHECKF(st == ARM_MATH_SUCCESS, "length %u refused", (unsigned)lens[i]);
        else CHECKF(st != ARM_MATH_SUCCESS, "length %u accepted: its tables are linked", (unsigned)lens[i]);
    }
    CHECK(FFT_SIZE == 4096);
}

/* The raw transform against a direct DFT of the same input, at a few bins. */
static void test_raw_against_dft(void)
{
    static float32_t in[FFT_SIZE], ref[FFT_SIZE], out[FFT_SIZE];
    arm_rfft_fast_instance_f32 S;
    const arm_status st = arm_rfft_fast_init_f32(&S, FFT_SIZE);
    CHECK(st == ARM_MATH_SUCCESS);
    if (st != ARM_MATH_SUCCESS) return;         /* never transform through an instance that was refused */
    for (int n = 0; n < FFT_SIZE; n++) {
        in[n] = (float32_t)(0.7 * cos(2.0 * M_PI * 37.0 * n / FFT_SIZE + 0.3) +
                            0.2 * sin(2.0 * M_PI * 1500.0 * n / FFT_SIZE) +
                            0.05 * ((n * 2654435761u >> 16) & 0xFF) / 255.0 - 0.1);
        ref[n] = in[n];
    }
    arm_rfft_fast_f32(&S, in, out, 0);          /* modifies in[] */
    static const int bins[] = { 1, 2, 36, 37, 38, 511, 1024, 1500, 2046, 2047 };
    for (unsigned k = 0; k < sizeof bins / sizeof bins[0]; k++) {
        double re = 0.0, im = 0.0;
        for (int n = 0; n < FFT_SIZE; n++) {
            const double a = -2.0 * M_PI * (double)bins[k] * n / FFT_SIZE;
            re += ref[n] * cos(a);
            im += ref[n] * sin(a);
        }
        const double gr = out[2 * bins[k]], gi = out[2 * bins[k] + 1];
        const double err = hypot(gr - re, gi - im), mag = hypot(re, im);
        CHECKF(err <= 1e-3 * mag + 0.02, "bin %d: dft %.4f%+.4fj, cmsis %.4f%+.4fj", bins[k], re, im, gr, gi);
    }
    double dc = 0.0;
    for (int n = 0; n < FFT_SIZE; n++) dc += ref[n];
    CHECKF(fabs(out[0] - dc) <= 1e-3 * fabs(dc) + 0.02, "DC %.4f vs %.4f", (double)out[0], dc);
}

/* fft.c end to end: a tone exactly on bin k peaks at bin k. */
static void test_tone_bins(void)
{
    static int16_t samples[FFT_SIZE];
    static fft_result_t r;
    fft_config_t c;
    memset(&c, 0, sizeof c);
    c.window = FFT_WINDOW_HANNING;
    c.sample_rate_hz = 4096.0f;                 /* 1 Hz per bin: freq == bin */
    c.db_range = 80.0f;
    c.peak_count = 4;
    fft_init(&c);
    CHECK(fft_is_initialized());
    CHECK(fft_engine_ok());
    CHECK(fft_get_sample_buf() != NULL);
    if (!fft_engine_ok()) { fft_deinit(); return; }

    static const int tones[] = { 3, 10, 100, 333, 1000, 1024, 1777, 2000, 2040 };
    for (unsigned t = 0; t < sizeof tones / sizeof tones[0]; t++) {
        const int k = tones[t];
        for (int n = 0; n < FFT_SIZE; n++)
            samples[n] = (int16_t)lrint(12000.0 * sin(2.0 * M_PI * (double)k * n / FFT_SIZE));
        memset(&r, 0, sizeof r);
        fft_process(samples, FFT_SIZE, &r);
        CHECK(r.level_db != NULL && r.num_bins == FFT_BINS);
        if (!r.level_db) continue;
        int best = 1;
        for (int i = 2; i < FFT_BINS; i++)
            if (r.level_db[i] > r.level_db[best]) best = i;
        CHECKF(best == k, "tone on bin %d peaks at bin %d", k, best);
        /* Hann, coherent: amplitude 12000 * 0.5 -> 20 log10(6000) = 75.56 dB */
        CHECKF(fabsf(r.level_db[k] - 75.56f) < 0.1f, "bin %d level %.2f dB", k, (double)r.level_db[k]);
        /* nothing else within 60 dB once clear of the Hann main lobe */
        float other = -300.0f;
        for (int i = 1; i < FFT_BINS; i++)
            if ((i < k - 2 || i > k + 2) && r.level_db[i] > other) other = r.level_db[i];
        CHECKF(other < r.level_db[k] - 60.0f, "tone %d: another bin at %.1f dB (peak %.1f)", k,
               (double)other, (double)r.level_db[k]);
        CHECKF(r.num_peaks >= 1 && r.peaks[0].bin == (uint16_t)k, "tone %d: peak list says bin %u", k,
               (unsigned)(r.num_peaks ? r.peaks[0].bin : 0));
    }
    fft_deinit();
    CHECK(!fft_engine_ok() && fft_get_sample_buf() == NULL);
}

#ifdef EXPECT_ENGINE_FAIL
/* The table selection is wrong: fft.c must say so and do nothing. */
static void test_engine_failure_is_loud(void)
{
    static int16_t samples[FFT_SIZE];
    fft_result_t r;
    fft_config_t c;
    memset(&c, 0, sizeof c);
    c.window = FFT_WINDOW_HANNING;
    c.sample_rate_hz = 4096.0f;
    c.peak_count = 4;
    fft_init(&c);
    CHECK(fft_is_initialized());                /* it holds the pool ...            */
    CHECK(!fft_engine_ok());                    /* ... and says the engine is not usable */
    CHECK(fft_get_sample_buf() == NULL);        /* so every view takes its "no FFT" path */
    for (int n = 0; n < FFT_SIZE; n++) samples[n] = (int16_t)(n * 7);
    memset(&r, 0x5A, sizeof r);
    const fft_result_t before = r;
    fft_process(samples, FFT_SIZE, &r);         /* a caller that ignores the NULL: nothing happens */
    CHECK(memcmp(&r, &before, sizeof r) == 0);
    fft_deinit();
    CHECK(!fft_is_initialized() && !fft_engine_ok());
}
#endif

int main(void)
{
    test_init_lengths();
#ifdef EXPECT_ENGINE_FAIL
    test_engine_failure_is_loud();
    printf("test_fft_cmsis_tables (table missing): %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
#endif
    test_raw_against_dft();
    test_tone_bins();
    printf("test_fft_cmsis_tables: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
