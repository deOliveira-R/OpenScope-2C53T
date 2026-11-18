/* dsp/acq_timing.h: the acquisition's waits. The expected values are the ones the task computed before the
 * numbers got their names (2026-11-17), so naming them changed nothing. */
#include <stdio.h>
#include "acq_timing.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static unsigned old_auto(float fs) { if (fs <= 0.0f) return 25u; float ms = 3.0f * 1024.0f / fs * 1000.0f + 230.0f; if (ms < 25.0f) ms = 25.0f; if (ms > 1000.0f) ms = 1000.0f; return (unsigned)(unsigned short)ms; }
static unsigned old_post(float fs, int core, unsigned gap)
{
    if (fs <= 0.0f) return 600u;
    float ms = 1024.0f / fs * 1000.0f + 100.0f;
    if (core) { ms = 1024.0f / fs * 1000.0f + 1.0f; if (ms < (float)gap) ms = (float)gap; }
    if (ms > 5000.0f) ms = 5000.0f;
    return (unsigned)(unsigned short)ms;
}
static unsigned old_force(float fs) { unsigned ms = (fs > 0.0f) ? (unsigned)(1000.0f / fs) + 1u : 3u; return ms > 5u ? 5u : ms; }

int main(void)
{
    static const float rates[] = { 0.0f, 125e6f, 50e6f, 25e6f, 12.5e6f, 5e6f, 2.5e6f, 1.25e6f, 500e3f, 250e3f, 125e3f, 50e3f, 25e3f,
                                   12.5e3f, 5e3f, 2.5e3f, 1250.0f, 500.0f, 124830000.0f, 499847.0f, 500.2f };
    CHECK(ACQ_REC_SAMPLES == 1024u);
    for (unsigned i = 0; i < sizeof rates / sizeof rates[0]; i++) {
        const float fs = rates[i];
        CHECK(acq_time_auto_wait_ms(fs, ACQ_AUTO_MIN_MS) == old_auto(fs));
        for (int core = 0; core < 2; core++) CHECK(acq_time_post_record_ms(fs, core, ACQ_POLL_GAP_MS) == old_post(fs, core, 30u));
        CHECK(acq_time_force_settle_ms(fs) == old_force(fs));
    }
    /* the figures quoted to the owner */
    CHECK(acq_time_auto_wait_ms(125e6f, ACQ_AUTO_MIN_MS) == 230u);          /* fast timebases: 0.23 s */
    CHECK(acq_time_auto_wait_ms(500.0f, ACQ_AUTO_MIN_MS) == ACQ_AUTO_MAX_MS);
    CHECK(acq_time_post_record_ms(125e6f, true, ACQ_POLL_GAP_MS) == 30u);   /* 33 records/s */
    CHECK(acq_time_post_record_ms(125e6f, false, ACQ_POLL_GAP_MS) == 100u); /* the factory design: 10 records/s */
    CHECK(acq_time_post_record_ms(500.0f, true, ACQ_POLL_GAP_MS) == 2049u);
    CHECK(acq_time_force_settle_ms(500.0f) == 3u && acq_time_force_settle_ms(125e6f) == 1u);
    CHECK(acq_time_trigd_stale_ms(30u, 230u) == 2u * 230u + 250u);
    CHECK(acq_time_trigd_stale_ms(2049u, 1000u) == 2u * 2049u + 250u);
    if (fails) { printf("test_acq_timing: %d FAILED\n", fails); return 1; }
    printf("test_acq_timing: all passed\n");
    return 0;
}
