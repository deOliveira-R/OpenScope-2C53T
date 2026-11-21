/*
 * fft_live.c — the live acquisition record as spectrum input. See fft_live.h.
 */
#include "fft_live.h"
#include <stddef.h>

uint16_t fft_live_prepare(const uint8_t *rec, uint16_t n,
                          int16_t *out, uint16_t out_cap)
{
    if (rec == 0 || out == 0 || n == 0 || out_cap == 0)
        return 0;

    uint16_t count = n;
    if (count > out_cap)
        count = out_cap;

    /* Remove the record's OWN mean, not a nominal 128: at range 5 the input
     * sits around code 67 (bottom-clipped, EXP-53), and a 60-count residual
     * DC put a spike in bins 0..2 taller than the fundamental on the bench
     * (FFT-live S2, 2026-09-22). The peak search starts at bin 2 so the
     * header was right; the screen was not. */
    uint32_t sum = 0;
    for (uint16_t i = 0; i < count; i++)
        sum += rec[i];
    int16_t mean = (int16_t)((sum + count / 2) / count);
    for (uint16_t i = 0; i < count; i++)
        out[i] = (int16_t)(((int16_t)rec[i] - mean)
                           * FFT_LIVE_GAIN);
    return count;
}

bool fft_live_axis_known(float sample_rate_hz)
{
    return sample_rate_hz > 0.0f;
}

float fft_live_bin_hz(float sample_rate_hz, uint16_t fft_size, uint16_t bin)
{
    if (!fft_live_axis_known(sample_rate_hz) || fft_size == 0)
        return 0.0f;
    return (float)bin * sample_rate_hz / (float)fft_size;
}

void fft_live_format_hz(float hz, char *buf, int bufsize)
{
    const char *unit;
    float val;

    if (bufsize <= 0)
        return;
    if (hz < 0.0f)
        hz = 0.0f;

    /* Round to the tenth that is printed, then pick the unit on the rounded
     * value so 999.96 Hz reads "1.0kHz" and not "1000.0Hz". Until 2026-09-22
     * the tenth was truncated: the 8 kHz fold at 0x10 (bin 1475 = 4497 Hz)
     * printed "4.4kHz" on the bench, and Nyquist at 0x0E (24,965 Hz) read
     * "24.9kHz". Truncation is a bias, always down, so it cannot be told
     * from a rate error by reading the screen. */
    long tenths;
    if (hz >= 999950.0f) {
        val = hz / 1000000.0f;
        unit = "MHz";
    } else if (hz >= 999.95f) {
        val = hz / 1000.0f;
        unit = "kHz";
    } else {
        val = hz;
        unit = "Hz";
    }
    tenths = (long)(val * 10.0f + 0.5f);

    int integer = (int)(tenths / 10);
    int frac = (int)(tenths % 10);

    int pos = 0;
    if (integer >= 100 && pos < bufsize - 1) buf[pos++] = (char)('0' + integer / 100);
    if (integer >= 10  && pos < bufsize - 1) buf[pos++] = (char)('0' + (integer / 10) % 10);
    if (pos < bufsize - 1) buf[pos++] = (char)('0' + integer % 10);
    if (pos < bufsize - 1) buf[pos++] = '.';
    if (pos < bufsize - 1) buf[pos++] = (char)('0' + frac);
    while (*unit && pos < bufsize - 1)
        buf[pos++] = *unit++;
    buf[pos] = '\0';
}

void fft_live_axis_label(float sample_rate_hz, uint16_t fft_size,
                         uint16_t bin, char *buf, int bufsize)
{
    if (bufsize <= 0)
        return;
    if (!fft_live_axis_known(sample_rate_hz) || fft_size == 0) {
        int pos = 0;
        if (pos < bufsize - 1) buf[pos++] = '-';
        if (pos < bufsize - 1) buf[pos++] = '-';
        buf[pos] = '\0';
        return;
    }
    fft_live_format_hz(fft_live_bin_hz(sample_rate_hz, fft_size, bin),
                       buf, bufsize);
}

/* ── Measurements on a spectrum ────────────────────────────────────── */

/* A harmonic is looked for in its bin and one bin either side: the record
 * is shorter than the transform, so a line is a few bins wide and its top
 * need not sit in the bin the integer multiple names. */
#define FFT_MEAS_HARMONIC_REACH  1u

static fft_meas_t harmonic_dbc(const float *level_db, uint16_t bins, uint16_t peak_bin, unsigned n)
{
    fft_meas_t m = { FFT_MEAS_UNIT_NONE, 0.0f };
    const uint32_t at = (uint32_t)peak_bin * n;
    if (at + FFT_MEAS_HARMONIC_REACH >= bins) return m;
    float top = level_db[at - FFT_MEAS_HARMONIC_REACH];
    for (uint32_t b = at - FFT_MEAS_HARMONIC_REACH + 1u; b <= at + FFT_MEAS_HARMONIC_REACH; b++)
        if (level_db[b] > top) top = level_db[b];
    m.unit = FFT_MEAS_UNIT_DBC;
    m.value = top - level_db[peak_bin];
    return m;
}

void fft_live_measure(const float *level_db, uint16_t bins, uint16_t peak_bin,
                      float sample_rate_hz, uint16_t fft_size,
                      fft_meas_t out[FFT_MEAS_COUNT])
{
    for (unsigned i = 0; i < FFT_MEAS_COUNT; i++) {
        out[i].unit = FFT_MEAS_UNIT_NONE;
        out[i].value = 0.0f;
    }
    if (level_db == NULL || peak_bin == 0u || peak_bin >= bins) return;

    if (fft_live_axis_known(sample_rate_hz) && fft_size != 0u) {
        out[FFT_MEAS_PEAK].unit = FFT_MEAS_UNIT_HZ;
        out[FFT_MEAS_PEAK].value = fft_live_bin_hz(sample_rate_hz, fft_size, peak_bin);
    } else {
        out[FFT_MEAS_PEAK].unit = FFT_MEAS_UNIT_BIN;
        out[FFT_MEAS_PEAK].value = (float)peak_bin;
    }
    out[FFT_MEAS_LEVEL].unit = FFT_MEAS_UNIT_DB;
    out[FFT_MEAS_LEVEL].value = level_db[peak_bin];
    out[FFT_MEAS_H2] = harmonic_dbc(level_db, bins, peak_bin, 2u);
    out[FFT_MEAS_H3] = harmonic_dbc(level_db, bins, peak_bin, 3u);
}

static int put(char *buf, int bufsize, int pos, const char *s)
{
    while (*s && pos < bufsize - 1) buf[pos++] = *s++;
    return pos;
}

static int put_uint(char *buf, int bufsize, int pos, unsigned long v)
{
    char d[12];
    int n = 0;
    do { d[n++] = (char)('0' + v % 10u); v /= 10u; } while (v && n < (int)sizeof d);
    while (n > 0 && pos < bufsize - 1) buf[pos++] = d[--n];
    return pos;
}

void fft_live_format_db(float db, const char *unit, char *buf, int bufsize)
{
    if (bufsize <= 0) return;
    const bool neg = db < 0.0f;
    const unsigned long tenths = (unsigned long)((neg ? -db : db) * 10.0f + 0.5f);
    int pos = 0;
    if (neg && tenths != 0u) pos = put(buf, bufsize, pos, "-");
    pos = put_uint(buf, bufsize, pos, tenths / 10u);
    pos = put(buf, bufsize, pos, ".");
    pos = put_uint(buf, bufsize, pos, tenths % 10u);
    pos = put(buf, bufsize, pos, unit);
    buf[pos] = '\0';
}

void fft_meas_format(const fft_meas_t *m, char *buf, int bufsize)
{
    if (bufsize <= 0) return;
    int pos = 0;
    switch (m ? m->unit : FFT_MEAS_UNIT_NONE) {
    case FFT_MEAS_UNIT_HZ:  fft_live_format_hz(m->value, buf, bufsize); return;
    case FFT_MEAS_UNIT_DB:  fft_live_format_db(m->value, "dB", buf, bufsize); return;
    case FFT_MEAS_UNIT_DBC: fft_live_format_db(m->value, "dBc", buf, bufsize); return;
    case FFT_MEAS_UNIT_BIN:
        pos = put(buf, bufsize, pos, "bin ");
        pos = put_uint(buf, bufsize, pos, (unsigned long)(m->value + 0.5f));
        break;
    default:
        pos = put(buf, bufsize, pos, "--");
        break;
    }
    buf[pos] = '\0';
}
