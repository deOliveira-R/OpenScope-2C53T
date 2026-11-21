/*
 * fft_live.h — the live acquisition record as spectrum input.
 *
 * Everything here is pure (no LCD, no RTOS, no FPGA driver) so the same code
 * that feeds the spectrum on the device runs under tests/test_fft_live.c on
 * the host. The UI supplies the record and the sample rate; this module
 * decides which samples are analysed and what the frequency axis may claim.
 *
 * Refusal discipline (same as scope_freq / the Freq badge): a bin index is
 * always exact; a figure in hertz is only produced when the sample rate is
 * a measured one (> 0). fft_live_axis_known() is the single test for that.
 */
#ifndef FFT_LIVE_H
#define FFT_LIVE_H

#include <stdint.h>
#include <stdbool.h>

/* Numeric gain applied when widening 8-bit samples to int16: a constant,
 * so it moves every bin by the same dB and cannot change the spectrum's
 * shape. 128 = <<7, leaving one bit of headroom in int16. */
#define FFT_LIVE_GAIN       128

/* Copy an unsigned 8-bit record into a signed int16 buffer, its own mean
 * removed: out[i] = (rec[i] - mean) * FFT_LIVE_GAIN. Every sample of a
 * published record is valid (the capture core hands over whole records in
 * time order), so the whole record is analysed. Returns the number of
 * samples written (0 for an empty record or out_cap 0); never more than
 * out_cap. */
uint16_t fft_live_prepare(const uint8_t *rec, uint16_t n,
                          int16_t *out, uint16_t out_cap);

/* True when a frequency axis may be labelled in hertz. */
bool fft_live_axis_known(float sample_rate_hz);

/* Centre frequency of `bin` for a transform of `fft_size` points, or 0.0f
 * when the axis is not known. Callers must not print 0 as a frequency;
 * check fft_live_axis_known() first. */
float fft_live_bin_hz(float sample_rate_hz, uint16_t fft_size, uint16_t bin);

/* "123.4Hz" / "12.3kHz" / "1.2MHz" — one decimal, no float printf needed
 * (the firmware links newlib-nano without float formatting). */
void fft_live_format_hz(float hz, char *buf, int bufsize);

/* Axis label for `bin`: hertz when the axis is known, "--" when it is not.
 * Exactly the string the screen shows, so the test pins it. */
void fft_live_axis_label(float sample_rate_hz, uint16_t fft_size,
                         uint16_t bin, char *buf, int bufsize);

/* ── The axes of a spectrum on the screen ────────────────────────────
 * What a column and a row of the plot mean, and back. The bars, the peak
 * carets and the cursors all go through these, so they cannot disagree
 * about where a frequency or a level is. on false: no spectrum is drawn. */
typedef struct {
    bool     on;
    uint16_t x0, w;             /* the frequency axis: w columns from x0 */
    uint16_t bin0, bins;        /* ... show bins bin0 .. bin0 + bins */
    uint16_t y_top, height;     /* the level axis: rows y_top .. y_top + height */
    float    ref_db, range_db;  /* the level on row y_top and the span down the height */
    float    sample_rate_hz;    /* <= 0: frequencies are bin numbers */
    uint16_t fft_size;
} fft_axes_t;

/* The bin column x shows. x must be on the axis (x0 <= x < x0 + w). */
static inline uint16_t fft_axes_bin(const fft_axes_t *a, uint16_t x)
{
    return (uint16_t)(a->bin0 + (uint16_t)((uint32_t)(x - a->x0) * a->bins / a->w));
}

/* The first column that shows `bin`, or -1 when the bin is not on the axis.
 * Zoomed in, a bin has several columns and this is the first of them; zoomed
 * out, a column has several bins and this is the one that holds it. */
static inline int fft_axes_x(const fft_axes_t *a, uint16_t bin)
{
    if (bin < a->bin0 || bin >= a->bin0 + a->bins) return -1;
    uint16_t x = (uint16_t)(a->x0 + ((uint32_t)(bin - a->bin0) * a->w + a->bins - 1u) / a->bins);
    if (x >= a->x0 + a->w || fft_axes_bin(a, x) > bin) x--;
    return (int)x;
}

/* How far down the level axis a level is, as a fraction: 0 on the top row,
 * 1 at the bottom, outside 0..1 when it is off the plot. */
static inline float fft_axes_depth(const fft_axes_t *a, float db)
{
    return (a->ref_db - db) / a->range_db;
}

/* The row of a level, or -1 when it is off the plot. */
static inline int fft_axes_y(const fft_axes_t *a, float db)
{
    const float n = fft_axes_depth(a, db);
    if (n < 0.0f || n >= 1.0f) return -1;
    return (int)a->y_top + (int)(n * (float)a->height);
}

/* The level a number of rows is worth, and the bins a number of columns is
 * (at least one): what a cursor moves by per step of the arrows. */
static inline float fft_axes_db_per_rows(const fft_axes_t *a, uint16_t rows)
{
    return a->range_db * (float)rows / (float)a->height;
}
static inline uint16_t fft_axes_bins_per_cols(const fft_axes_t *a, uint16_t cols)
{
    const uint32_t b = (uint32_t)cols * a->bins / a->w;
    return (uint16_t)(b ? b : 1u);
}

/* ── What is measured on a spectrum ──────────────────────────────────
 * The frequency domain's measurements, as the time record has its twelve
 * (scope_measure.h). All read the spectrum the screen shows:
 *   Peak   frequency of the strongest bin: hertz when the axis is known,
 *          else the bin's number. It is the bin's centre, so it is exact to
 *          half a bin width (fs / fft_size), no better.
 *   Level  that bin's level, dB re one count of the transform's input.
 *   H2 H3  the level at twice and three times the peak's bin (the strongest
 *          of that bin and its two neighbours), relative to the peak: dBc.
 *          Refused when that frequency is beyond the spectrum.
 * Everything is refused when there is no peak. */
typedef enum { FFT_MEAS_PEAK = 0, FFT_MEAS_LEVEL, FFT_MEAS_H2, FFT_MEAS_H3, FFT_MEAS_COUNT } fft_meas_id_t;

#define FFT_MEAS_ALL_MASK      ((1u << FFT_MEAS_COUNT) - 1u)
/* What a spectrum view shows until the user chooses: the peak and its level. */
#define FFT_MEAS_DEFAULT_MASK  ((1u << FFT_MEAS_PEAK) | (1u << FFT_MEAS_LEVEL))

typedef enum {
    FFT_MEAS_UNIT_NONE = 0,    /* refused: "--" */
    FFT_MEAS_UNIT_HZ,
    FFT_MEAS_UNIT_BIN,         /* a frequency with no known rate */
    FFT_MEAS_UNIT_DB,
    FFT_MEAS_UNIT_DBC,
} fft_meas_unit_t;

typedef struct {
    fft_meas_unit_t unit;
    float           value;
} fft_meas_t;

/* Measure a spectrum of `bins` levels (dB) whose strongest peak is at
 * peak_bin (0: there is none). */
void fft_live_measure(const float *level_db, uint16_t bins, uint16_t peak_bin,
                      float sample_rate_hz, uint16_t fft_size,
                      fft_meas_t out[FFT_MEAS_COUNT]);

/* "976.6Hz", "bin 16", "44.2dB", "-9.5dBc", "--". 16 bytes is enough. */
void fft_meas_format(const fft_meas_t *m, char *buf, int bufsize);

/* A level with one decimal and its unit, without float printf: "-9.5dBc". */
void fft_live_format_db(float db, const char *unit, char *buf, int bufsize);

#endif /* FFT_LIVE_H */
