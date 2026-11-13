/*
 * Host test for util/units.h: conversions, clamps, rounding, cursor sign and
 * fixed-point text. Table driven. The negative half (code that mixes units
 * must not compile) is tests/test_units_negative.c.
 *
 * Build/run:  make -C firmware test-units
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../src/util/units.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static adc_raw_t   R(uint8_t v) { adc_raw_t x = { v }; return x; }
static rec_code_t  C(uint8_t v) { rec_code_t x = { v }; return x; }

static void test_raw_rec(void)
{
    static const struct { uint8_t raw, rec; } t[] = {
        { 0, 0 }, { 27, 0 }, { 28, 0 }, { 29, 1 }, { 128, 100 },
        { 254, 226 }, { 255, 227 },
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
        CHECK(rec_from_raw(R(t[i].raw)).v == t[i].rec);
    CHECK(REC_CODE_MAX == 227);
    CHECK(trig_level_rec(R(100)).v == 72);
    CHECK(trig_level_rec(R(10)).v == 0);

    CHECK(raw_from_rec(C(0)).v == 28);
    CHECK(raw_from_rec(C(227)).v == 255);
    CHECK(raw_from_rec(C(255)).v == 255);          /* saturates */
    for (int r = 28; r <= 255; r++)                /* round trip */
        CHECK(raw_from_rec(rec_from_raw(R((uint8_t)r))).v == r);

    rec_delta_t d = rec_sub(C(10), C(100));
    CHECK(d.v == -90);
    d = rec_sub(C(227), C(0));
    CHECK(d.v == 227);
    volts_per_count_t k = { 0.02f };
    CHECK(fabsf(volts_of(rec_sub(C(110), C(100)), k).v - 0.2f) < 1e-6f);
    CHECK(fabsf(volts_of(rec_sub(C(90), C(100)), k).v + 0.2f) < 1e-6f);
}

static void test_ppm_ratio(void)
{
    ppm_period_t p = { 250000 };
    CHECK(fabsf(period_frac_of(p) - 0.25f) < 1e-7f);
    CHECK(ppm_period_from_frac(0.25f).ppm == 250000);
    CHECK(ppm_period_from_frac(-0.0005f).ppm == -500);
    CHECK(ppm_period_from_frac(0.0f).ppm == 0);
    CHECK(ppm_period_from_frac(1e30f).ppm > 2000000000);     /* saturates */
    CHECK(ppm_period_from_frac(-1e30f).ppm < -2000000000);
    CHECK(ppm_period_from_frac(NAN).ppm == 0);

    CHECK(permille_from_ratio(0.505f).pm == 505);
    CHECK(permille_from_ratio(0.0f).pm == 0);
    CHECK(permille_from_ratio(1.0f).pm == 1000);
    CHECK(permille_from_ratio(1.7f).pm == 1000);
    CHECK(permille_from_ratio(-0.3f).pm == 0);
    CHECK(permille_from_ratio(NAN).pm == 0);

    static const struct { uint16_t pm; uint8_t pct; } t[] = {
        { 0, 0 }, { 4, 0 }, { 5, 1 }, { 504, 50 }, { 505, 51 },
        { 995, 100 }, { 1000, 100 }, { 60000, 100 },
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        permille_t pm = { t[i].pm };
        CHECK(percent_from_permille(pm).pct == t[i].pct);
    }
}

static void test_time(void)
{
    CHECK(ms_from_ticks(0).ms == 0);
    CHECK(ms_from_ticks(1500).ms == 1500 * 1000u / UNITS_TICK_RATE_HZ);
    CHECK(ms_from_ticks(0xFFFFFFFFu).ms ==
          (uint32_t)(((uint64_t)0xFFFFFFFFu * 1000u) / UNITS_TICK_RATE_HZ));
}

static void test_cursor(void)
{
    CHECK(cursor_delta(10, 30, CUR_SECOND_MINUS_FIRST) == 20);
    CHECK(cursor_delta(10, 30, CUR_FIRST_MINUS_SECOND) == -20);
    CHECK(cursor_delta(30, 10, CUR_SECOND_MINUS_FIRST) == -20);
    CHECK(cursor_delta(5, 5, CUR_FIRST_MINUS_SECOND) == 0);
    /* rows grow down: second cursor lower on screen (row 80 vs 20) = lower
     * voltage, so second - first in volts is negative */
    CHECK(cursor_dy_volts_sign(20, 80, CUR_SECOND_MINUS_FIRST) == -60);
    CHECK(cursor_dy_volts_sign(20, 80, CUR_FIRST_MINUS_SECOND) == 60);
    CHECK(cursor_dy_volts_sign(80, 20, CUR_SECOND_MINUS_FIRST) == 60);
    /* the two orders are exact negatives */
    for (int a = 0; a < 200; a += 17)
        for (int b = 0; b < 200; b += 13)
            CHECK(cursor_dy_volts_sign(a, b, CUR_SECOND_MINUS_FIRST) ==
                  -cursor_dy_volts_sign(a, b, CUR_FIRST_MINUS_SECOND));
}

static void test_fmt(void)
{
    static const struct { int32_t v; uint8_t d; const char *s; } t[] = {
        { -60, 2, "-0.60" }, { 60, 2, "0.60" }, { 0, 2, "0.00" },
        { -5, 2, "-0.05" }, { 5, 1, "0.5" }, { -5, 1, "-0.5" },
        { 15, 1, "1.5" }, { 12345, 2, "123.45" }, { -12345, 3, "-12.345" },
        { 7, 0, "7" }, { -7, 0, "-7" }, { 0, 0, "0" },
        { 2147483647, 2, "21474836.47" },
        { (int32_t)0x80000000, 2, "-21474836.48" },
        { (int32_t)0x80000000, 0, "-2147483648" },
        { 1, 9, "0.000000001" },
    };
    char b[40];
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        int n = fmt_fixed(b, sizeof b, t[i].v, t[i].d);
        CHECK(strcmp(b, t[i].s) == 0);
        CHECK(n == (int)strlen(t[i].s));
        if (strcmp(b, t[i].s)) printf("  got '%s' want '%s'\n", b, t[i].s);
    }
    /* truncation: NUL-terminated, full length reported */
    char s4[4];
    CHECK(fmt_fixed(s4, sizeof s4, -12345, 2) == 7);
    CHECK(strcmp(s4, "-12") == 0);
    CHECK(fmt_fixed(b, 1, 5, 1) == 3 && b[0] == '\0');
    CHECK(fmt_fixed(NULL, 0, 5, 1) == 3);
    CHECK(fmt_fixed(b, sizeof b, 5, 10) == -1);
}

static void test_parse(void)
{
    static const struct { const char *s; uint8_t d; int ok; int32_t v; } t[] = {
        { "1.5", 1, 1, 15 }, { "-0.05", 2, 1, -5 }, { "1.55", 1, 0, 0 },
        { "abc", 1, 0, 0 }, { "", 1, 0, 0 }, { "+1", 0, 1, 1 },
        { "1", 2, 1, 100 }, { "-1", 1, 1, -10 }, { "-0.60", 2, 1, -60 },
        { "0.6", 2, 1, 60 }, { "007", 0, 1, 7 }, { "-", 1, 0, 0 },
        { "+", 1, 0, 0 }, { ".5", 1, 0, 0 }, { "1.", 1, 0, 0 },
        { "1..5", 1, 0, 0 }, { "1.5x", 1, 0, 0 }, { " 1", 0, 0, 0 },
        { "1 ", 0, 0, 0 }, { "1.5", 0, 0, 0 }, { "--1", 0, 0, 0 },
        { "2147483647", 0, 1, 2147483647 }, { "2147483648", 0, 0, 0 },
        { "-2147483648", 0, 1, (int32_t)0x80000000 },
        { "-2147483649", 0, 0, 0 },
        { "21474836.47", 2, 1, 2147483647 }, { "21474836.48", 2, 0, 0 },
        { "99999999999999999999", 0, 0, 0 },
        { "1e3", 0, 0, 0 },
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        int32_t v = 12345;
        int r = parse_fixed(t[i].s, t[i].d, &v);
        CHECK((r == 0) == t[i].ok);
        if (t[i].ok) CHECK(v == t[i].v);
        else         CHECK(v == 12345);               /* untouched on error */
        if ((r == 0) != t[i].ok || (t[i].ok && v != t[i].v))
            printf("  case '%s',%u\n", t[i].s, t[i].d);
    }
    int32_t v;
    CHECK(parse_fixed(NULL, 0, &v) == -1);
    CHECK(parse_fixed("1", 0, NULL) == -1);
    CHECK(parse_fixed("1", 10, &v) == -1);
}

/* fmt_fixed and parse_fixed are inverses over a spread of values */
static void test_roundtrip(void)
{
    static const int32_t vals[] = { 0, 1, -1, 5, -5, 60, -60, 999, -1000,
        123456, -123456, 2147483647, (int32_t)0x80000000 };
    for (uint8_t d = 0; d <= 9; d++)
        for (unsigned i = 0; i < sizeof vals / sizeof vals[0]; i++) {
            char b[40];
            int32_t back = 0;
            fmt_fixed(b, sizeof b, vals[i], d);
            CHECK(parse_fixed(b, d, &back) == 0 && back == vals[i]);
        }
}

int main(void)
{
    test_raw_rec();
    test_ppm_ratio();
    test_time();
    test_cursor();
    test_fmt();
    test_parse();
    test_roundtrip();
    printf("units: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
