/*
 * test_fft_source.c - which channel the spectrum views analyse
 * (src/dsp/fft_source.c). The code the display task runs: active channel,
 * enables, what the published record holds, the 250 MS/s merge -> buffer,
 * sample rate, label, and the "is off" answer.
 *
 * Build: make test-fft-source
 */
#include <stdio.h>
#include <string.h>
#include "fft_source.h"

static int run, passed;
#define CHECK(name, cond) do { run++; if (cond) { passed++; printf("  %-70s[PASS]\n", name); } \
                               else { printf("  %-70s[FAIL]\n", name); } } while (0)

#define FS   124800000.0f      /* one converter at the fast codes */

static fft_source_t sel(uint8_t ach, bool en1, bool en2, bool has1, bool has2, bool merged, float rate)
{
    const fft_source_in_t in = { ach, { en1, en2 }, { has1, has2 }, merged, rate };
    fft_source_t s;
    memset(&s, 0xA5, sizeof s);          /* every field must be written */
    fft_source_select(&in, &s);
    return s;
}

int main(void)
{
    printf("test_fft_source\n");
    fft_source_t s;

    printf("both channels on\n");
    s = sel(0, true, true, true, true, false, FS);
    CHECK("active CH1: CH1's buffer, the record's rate, label CH1",
          s.state == FFT_SEL_OK && s.ch == 0 && s.buf == 0 && !s.merged && s.rate_hz == FS &&
          strcmp(s.label, "CH1") == 0 && s.note[0] == '\0');
    s = sel(1, true, true, true, true, false, FS);
    CHECK("active CH2: CH2's buffer, the same rate, label CH2",
          s.state == FFT_SEL_OK && s.ch == 1 && s.buf == 1 && !s.merged && s.rate_hz == FS &&
          strcmp(s.label, "CH2") == 0 && s.note[0] == '\0');
    CHECK("the two never share a history key", fft_source_key(&s) == 2 &&
          (s = sel(0, true, true, true, true, false, FS), fft_source_key(&s)) == 1);

    printf("CH1 alone at a fast timebase: the 250 MS/s merge (as before this change)\n");
    s = sel(0, true, false, true, false, true, 2.0f * FS);
    CHECK("active CH1: CH1's buffer is the merge, at twice the converter rate",
          s.state == FFT_SEL_OK && s.buf == 0 && s.merged && s.rate_hz == 2.0f * FS);
    s = sel(1, true, false, true, false, true, 2.0f * FS);
    CHECK("active CH2 (short CH2 key) while CH2 is off: \"CH2 is off\", no buffer rate",
          s.state == FFT_SEL_OFF && s.ch == 1 && strcmp(s.note, "CH2 is off") == 0 &&
          strcmp(s.label, "CH2") == 0 && s.rate_hz == 0.0f && fft_source_key(&s) == 0);

    printf("CH2 alone\n");
    s = sel(1, false, true, false, true, false, FS);
    CHECK("active CH2: its own 125 MS/s record, never the merge",
          s.state == FFT_SEL_OK && s.buf == 1 && !s.merged && s.rate_hz == FS);
    s = sel(0, false, true, false, true, false, FS);
    CHECK("active CH1 while CH1 is off: \"CH1 is off\" (not CH2's data)",
          s.state == FFT_SEL_OFF && s.ch == 0 && strcmp(s.note, "CH1 is off") == 0);

    printf("a channel just switched on: the published record is still the old routing's\n");
    s = sel(1, true, true, true, false, true, 2.0f * FS);
    CHECK("active CH2, record without CH2's samples: wait, not converter B's copy of CH1",
          s.state == FFT_SEL_WAIT && s.ch == 1 && fft_source_key(&s) == 0 && s.note[0] != '\0');
    s = sel(0, true, true, false, true, false, FS);
    CHECK("active CH1, record without CH1's samples: wait", s.state == FFT_SEL_WAIT && s.ch == 0);
    s = sel(1, true, true, true, true, true, 2.0f * FS);
    CHECK("CH2 never takes the doubled rate (a merged flag halves it back)",
          s.state == FFT_SEL_OK && s.buf == 1 && !s.merged && s.rate_hz == FS);

    printf("rate not known (display and hardware timebase disagree)\n");
    s = sel(0, true, true, true, true, false, 0.0f);
    CHECK("CH1: rate 0 stays 0 (the axis is labelled in bins)", s.state == FFT_SEL_OK && s.rate_hz == 0.0f);
    s = sel(1, true, true, true, true, true, 0.0f);
    CHECK("CH2: rate 0 stays 0", s.state == FFT_SEL_OK && s.rate_hz == 0.0f);
    s = sel(1, true, true, true, true, false, -5.0f);
    CHECK("a negative rate is not a rate", s.rate_hz == 0.0f);

    printf("robustness\n");
    s = sel(7, true, true, true, true, false, FS);
    CHECK("an active-channel value above 1 counts as CH2", s.ch == 1 && s.buf == 1 && strcmp(s.label, "CH2") == 0);
    s = sel(0, false, false, true, true, false, FS);
    CHECK("off wins over a record that still holds the channel", s.state == FFT_SEL_OFF);
    CHECK("label and note are terminated inside their arrays",
          memchr(s.label, 0, sizeof s.label) != NULL && memchr(s.note, 0, sizeof s.note) != NULL);

    /* exhaustive: the answer never names or reads the other channel */
    int ok = 1;
    for (unsigned m = 0; m < 64; m++) {
        const uint8_t a = m & 1u;
        s = sel(a, m & 2u, m & 4u, m & 8u, m & 16u, m & 32u, FS);
        const bool en = a ? (m & 4u) : (m & 2u), has = a ? (m & 16u) : (m & 8u);
        ok &= s.ch == a && s.buf == a && s.label[2] == (a ? '2' : '1');
        ok &= s.state == (!en ? FFT_SEL_OFF : !has ? FFT_SEL_WAIT : FFT_SEL_OK);
        ok &= (s.state == FFT_SEL_OK) == (fft_source_key(&s) == 1u + a);
        ok &= s.merged == (s.state == FFT_SEL_OK && a == 0 && (m & 32u));
    }
    CHECK("all 64 combinations: own channel only; off > wait > ok", ok);

    printf("\n%d/%d passed\n", passed, run);
    return passed == run ? 0 : 1;
}
