/*
 * fft_source.c - which channel the spectrum views analyse. See fft_source.h.
 */
#include "fft_source.h"

static void put(char *dst, const char *a, const char *b)
{
    while (*a) *dst++ = *a++;
    while (*b) *dst++ = *b++;
    *dst = '\0';
}

void fft_source_select(const fft_source_in_t *in, fft_source_t *out)
{
    const uint8_t ch = in->active_ch ? 1u : 0u;

    out->ch      = ch;
    out->buf     = ch;
    out->merged  = false;
    out->rate_hz = 0.0f;
    put(out->label, ch ? "CH2" : "CH1", "");
    out->note[0] = '\0';

    if (!in->enabled[ch]) {
        out->state = FFT_SEL_OFF;
        put(out->note, out->label, " is off");
        return;
    }
    if (!in->rec_has[ch]) {
        out->state = FFT_SEL_WAIT;
        put(out->note, out->label, ": wait");
        return;
    }
    out->state = FFT_SEL_OK;
    if (in->rec_rate_hz > 0.0f) {
        if (ch == 0u) {
            out->rate_hz = in->rec_rate_hz;
        } else {
            out->rate_hz = in->rec_merged ? in->rec_rate_hz * 0.5f : in->rec_rate_hz;
        }
    }
    out->merged = (ch == 0u) && in->rec_merged;
}

uint8_t fft_source_key(const fft_source_t *s)
{
    return (s->state == FFT_SEL_OK) ? (uint8_t)(1u + s->ch) : 0u;
}
