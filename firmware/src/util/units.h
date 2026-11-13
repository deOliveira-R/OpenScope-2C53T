/*
 * units.h - typed units for values that cross module boundaries.
 *
 * Every unit is a single-member struct: zero cost under the AAPCS (passed and
 * returned in a register) and no implicit conversion, so mixing a raw ADC code
 * with a record code, ppm with a gain, or per-mille with percent is a compile
 * error. Conversions are explicit functions below. Header-only; no
 * dependencies beyond the C library.
 *
 * Boundaries with a bug history that these types close:
 *   - ADC raw <-> record code (the 28-count offset)
 *   - cursor sign (screen: second - first; shell: first - second; pixel rows
 *     grow downwards while volts grow upwards)
 *   - ppm of a period vs a gain x 1e6
 *   - per-mille vs percent
 *   - x100 fixed point printed by hand ("-0.60" printed as "0.60")
 *   - milliseconds vs RTOS ticks
 */
#ifndef UNITS_H
#define UNITS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADC_REC_OFFSET 28
#define REC_CODE_MAX   (255 - ADC_REC_OFFSET)     /* 227 */

typedef struct { uint8_t  v;    } adc_raw_t;          /* code as the ADC delivers it */
typedef struct { uint8_t  v;    } rec_code_t;         /* code in the record: raw - 28, 0..227 */
typedef struct { int16_t  v;    } rec_delta_t;        /* difference of two record codes */
typedef struct { float    vpc;  } volts_per_count_t;
typedef struct { float    v;    } volts_t;
typedef struct { int32_t  uv;   } microvolts_t;
typedef struct { float    hz;   } sample_rate_t;
typedef struct { uint8_t  code; } tb_code_t;          /* timebase code */
typedef struct { uint32_t ms;   } ms_t;
typedef struct { int32_t  ppm;  } ppm_period_t;       /* parts per million of one period */
typedef struct { int32_t  e6;   } gain_e6_t;          /* a gain multiplied by 1e6 */
typedef struct { uint16_t pm;   } permille_t;         /* 0..1000 */
typedef struct { uint8_t  pct;  } percent_t;          /* 0..100 */

typedef enum {
    CUR_SECOND_MINUS_FIRST = 0,   /* the screen readout */
    CUR_FIRST_MINUS_SECOND = 1    /* the shell readout */
} cursor_order_t;

/* ---- ADC raw <-> record code ------------------------------------------- */

/* Clamps at 0 (raw below the offset) and REC_CODE_MAX. */
static inline rec_code_t rec_from_raw(adc_raw_t r)
{
    rec_code_t c;
    int32_t x = (int32_t)r.v - ADC_REC_OFFSET;
    if (x < 0) x = 0;
    if (x > REC_CODE_MAX) x = REC_CODE_MAX;
    c.v = (uint8_t)x;
    return c;
}

/* Inverse; a record code above REC_CODE_MAX saturates at raw 255. */
static inline adc_raw_t raw_from_rec(rec_code_t c)
{
    adc_raw_t r;
    uint32_t x = (uint32_t)c.v + ADC_REC_OFFSET;
    r.v = (uint8_t)(x > 255u ? 255u : x);
    return r;
}

/* The trigger comparator code (raw) as a record level. */
static inline rec_code_t trig_level_rec(adc_raw_t code)
{
    return rec_from_raw(code);
}

static inline rec_delta_t rec_sub(rec_code_t a, rec_code_t zero)
{
    rec_delta_t d;
    d.v = (int16_t)((int16_t)a.v - (int16_t)zero.v);
    return d;
}

static inline volts_t volts_of(rec_delta_t d, volts_per_count_t k)
{
    volts_t v;
    v.v = (float)d.v * k.vpc;
    return v;
}

/* ---- ppm / gain / ratios ----------------------------------------------- */

static inline float period_frac_of(ppm_period_t p)
{
    return (float)p.ppm / 1000000.0f;
}

static inline ppm_period_t ppm_period_from_frac(float f)
{
    ppm_period_t p;
    float x = f * 1000000.0f;
    if (x != x)            x = 0.0f;                 /* NaN */
    if (x > 2147483520.0f) x = 2147483520.0f;
    if (x < -2147483520.0f) x = -2147483520.0f;
    p.ppm = (int32_t)(x >= 0.0f ? x + 0.5f : x - 0.5f);
    return p;
}

/* A ratio 0..1 as per-mille, rounded to nearest, clamped to 0..1000. */
static inline permille_t permille_from_ratio(float r)
{
    permille_t p;
    float x = r * 1000.0f;
    if (!(x > 0.0f)) x = 0.0f;                       /* also NaN */
    if (x > 1000.0f) x = 1000.0f;
    p.pm = (uint16_t)(x + 0.5f);
    return p;
}

/* Rounded to nearest, halves up: 505 -> 51, 1000 -> 100. */
static inline percent_t percent_from_permille(permille_t p)
{
    percent_t c;
    uint32_t x = ((uint32_t)p.pm + 5u) / 10u;
    c.pct = (uint8_t)(x > 100u ? 100u : x);
    return c;
}

/* ---- time -------------------------------------------------------------- */

#ifdef configTICK_RATE_HZ
#define UNITS_TICK_RATE_HZ ((uint32_t)configTICK_RATE_HZ)
#else
#define UNITS_TICK_RATE_HZ 1000u
#endif

static inline ms_t ms_from_ticks(uint32_t ticks)
{
    ms_t m;
    m.ms = (uint32_t)(((uint64_t)ticks * 1000u) / UNITS_TICK_RATE_HZ);
    return m;
}

/* ---- cursors ------------------------------------------------------------ */

static inline int32_t cursor_delta(int32_t first, int32_t second, cursor_order_t o)
{
    return o == CUR_FIRST_MINUS_SECOND ? first - second : second - first;
}

/*
 * Rows grow downwards, volts grow upwards: the result is the distance in rows
 * in the direction of rising voltage, ordered by `o`. Multiply by volts per
 * row for the voltage delta. Second below first on screen (greater row) under
 * SECOND_MINUS_FIRST is negative.
 */
static inline int32_t cursor_dy_volts_sign(int32_t first_row, int32_t second_row,
                                           cursor_order_t o)
{
    return cursor_delta(-first_row, -second_row, o);
}

/* ---- fixed-point text --------------------------------------------------- */

/*
 * Prints `scaled` / 10^decimals ("-60", 2 -> "-0.60"; the sign survives a zero
 * integer part). decimals 0..9. Returns the length (snprintf style, excluding
 * the NUL), or -1 on bad arguments. Output is truncated and NUL-terminated if
 * n is too small.
 */
static inline int fmt_fixed(char *out, size_t n, int32_t scaled, uint8_t decimals)
{
    char tmp[24];
    int len = 0, pos = 0;
    uint32_t mag, intp, frac = 0, div = 1;
    uint8_t i;
    if (decimals > 9 || (out == NULL && n != 0)) return -1;
    mag = scaled < 0 ? 0u - (uint32_t)scaled : (uint32_t)scaled;
    for (i = 0; i < decimals; i++) div *= 10u;
    intp = mag / div;
    frac = mag % div;
    if (scaled < 0) tmp[len++] = '-';
    {
        char d[12];
        int k = 0;
        do { d[k++] = (char)('0' + intp % 10u); intp /= 10u; } while (intp);
        while (k) tmp[len++] = d[--k];
    }
    if (decimals) {
        tmp[len++] = '.';
        for (i = decimals; i > 0; i--) {
            div /= 10u;
            tmp[len++] = (char)('0' + (frac / div) % 10u);
        }
    }
    if (n) {
        for (pos = 0; pos < len && (size_t)pos + 1 < n; pos++) out[pos] = tmp[pos];
        out[pos] = '\0';
    }
    return len;
}

/*
 * Parses [+-]digits[.digits] into value * 10^decimals. Returns 0 on success,
 * -1 on error: empty or non-numeric text, trailing characters, no digit before
 * the point, more fractional digits than `decimals` (no silent rounding),
 * overflow of int32. "1.5",1 -> 15; "-0.05",2 -> -5; "1.55",1 -> error.
 * *out is written only on success.
 */
static inline int parse_fixed(const char *s, uint8_t decimals, int32_t *out)
{
    uint64_t acc = 0;
    int neg = 0, ndig = 0, nfrac = 0, seen_dot = 0;
    uint8_t i;
    if (s == NULL || out == NULL || decimals > 9) return -1;
    if (*s == '+' || *s == '-') { neg = (*s == '-'); s++; }
    for (; *s; s++) {
        if (*s == '.') {
            if (seen_dot || ndig == 0) return -1;
            seen_dot = 1;
            continue;
        }
        if (*s < '0' || *s > '9') return -1;
        if (seen_dot && ++nfrac > decimals) return -1;
        acc = acc * 10u + (uint64_t)(*s - '0');
        if (acc > 0xFFFFFFFFull * 10u) return -1;      /* keep acc bounded */
        ndig++;
    }
    if (ndig == 0 || (seen_dot && nfrac == 0)) return -1;
    for (i = (uint8_t)nfrac; i < decimals; i++) {
        acc *= 10u;
        if (acc > 0xFFFFFFFFull * 10u) return -1;
    }
    if (neg) {
        if (acc > 2147483648ull) return -1;
        *out = (int32_t)(0 - acc);
    } else {
        if (acc > 2147483647ull) return -1;
        *out = (int32_t)acc;
    }
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* UNITS_H */
