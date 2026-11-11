/*
 * Host tests for roll mode (parity C11): src/ui/scope_roll.c.
 *
 *  - the step table is stock's (ARR 0x080465A4, 10 kHz tick, 25 points/div),
 *  - the FIFO behaves like stock's 301-point FIFO (newest right, count
 *    saturating at 301, the oldest dropped),
 *  - the timebase ladder continues past the last FPGA code into roll and back,
 *  - one 32 px grid division holds 25 points, so the stock label holds,
 *  - a roll point's volts carry g = (m - 28)/150 on every unit, i.e. NOT
 *    stock V1.1.2's /192 with the m >= 221 gate (ADDENDUM_B S16), and agree
 *    with the Y-T rule (zero = 100 + pos with the cal page).
 *
 * Build/run: make test-scope-roll
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "scope_roll.h"
#include "scope_cal.h"
#include "scope_measure.h"

static int run, passed;
#define CHECK(name, cond) do { run++; if (cond) { passed++; printf("  %-62s[PASS]\n", name); } \
                               else { printf("  %-62s[FAIL]\n", name); } } while (0)

static void test_table(void)
{
    static const uint16_t arr[9] = { 19, 39, 79, 199, 399, 799, 1999, 3999, 7999 };
    static const uint32_t ms[9]  = { 2, 4, 8, 20, 40, 80, 200, 400, 800 };
    static const char *lbl[9] = { "50ms", "100ms", "200ms", "500ms", "1s", "2s", "5s", "10s", "20s" };
    int ok_arr = 1, ok_ms = 1, ok_lbl = 1, ok_div = 1, ok_code = 1;
    for (uint8_t i = 0; i < 9; i++) {
        char b[12];
        ok_arr &= scope_roll_arr(i) == arr[i];
        ok_ms  &= scope_roll_period_ms(i) == ms[i];
        scope_roll_label(i, b, sizeof b);
        ok_lbl &= strcmp(b, lbl[i]) == 0;
        ok_div &= fabsf(scope_roll_seconds_per_div(i) - (float)ms[i] * 0.025f) < 1e-6f;
        ok_code &= scope_roll_stock_code(i) == 0x14 + i;
    }
    CHECK("ARR table = stock 0x080465A4", ok_arr);
    CHECK("period = (ARR+1)/10 kHz = 2..800 ms", ok_ms);
    CHECK("labels 50ms..20s (25 points per division)", ok_lbl);
    CHECK("seconds/div = 25 x period", ok_div);
    CHECK("reg-0x01 byte = stock ms[0x2D] 0x14..0x1C", ok_code);
    char b[12];
    scope_roll_label(9, b, sizeof b);
    CHECK("out of range: ARR 0, period 0, label --",
          scope_roll_arr(9) == 0 && scope_roll_period_ms(9) == 0 && strcmp(b, "--") == 0);
}

static void test_fifo(void)
{
    static scope_roll_fifo_t f, snap;
    memset(&f, 0x5A, sizeof f);
    f.seq = 0;
    scope_roll_fifo_reset(&f);
    uint8_t v = 0;
    CHECK("reset: empty, get refuses", f.count == 0 && !scope_roll_fifo_get(&f, 0, 0, &v));

    for (int i = 0; i < 10; i++)
        scope_roll_fifo_push(&f, (uint8_t)i, (uint8_t)(200 + i));
    uint8_t a = 0, b2 = 0, c = 0;
    bool g1 = scope_roll_fifo_get(&f, 0, 0, &a);
    bool g2 = scope_roll_fifo_get(&f, 1, 9, &b2);
    CHECK("10 points: count 10", f.count == 10 && f.total == 10);
    CHECK("age 0 = newest (right edge), age 9 = oldest", g1 && a == 9 && g2 && b2 == 200);
    CHECK("age >= count refused", !scope_roll_fifo_get(&f, 0, 10, &c));

    for (int i = 10; i < 1000; i++)
        scope_roll_fifo_push(&f, (uint8_t)(i & 0xFF), (uint8_t)((i * 3) & 0xFF));
    CHECK("count saturates at 301 (stock cmp #300; bhi)", f.count == SCOPE_ROLL_POINTS && f.total == 1000);
    int ok = 1;
    for (uint16_t age = 0; age < SCOPE_ROLL_POINTS; age++) {
        int i = 999 - age;
        ok &= scope_roll_fifo_get(&f, 0, age, &a) && a == (uint8_t)(i & 0xFF);
        ok &= scope_roll_fifo_get(&f, 1, age, &b2) && b2 == (uint8_t)((i * 3) & 0xFF);
    }
    CHECK("full FIFO holds exactly the newest 301, in order, both channels", ok);
    CHECK("the 302nd-newest point is gone", !scope_roll_fifo_get(&f, 0, 301, &a));
    CHECK("bad channel refused", !scope_roll_fifo_get(&f, 2, 0, &a));
    CHECK("seq even after pushes (no writer in flight)", (f.seq & 1u) == 0);

    bool s = scope_roll_fifo_snapshot(&f, &snap);
    ok = s && snap.count == f.count;
    for (uint16_t age = 0; age < SCOPE_ROLL_POINTS; age++) {
        uint8_t x = 0, y = 1;
        scope_roll_fifo_get(&f, 0, age, &x);
        scope_roll_fifo_get(&snap, 0, age, &y);
        ok &= x == y;
    }
    CHECK("snapshot = source", ok);
    f.seq |= 1u;                       /* a writer stuck mid-push */
    CHECK("snapshot refuses a torn source", !scope_roll_fifo_snapshot(&f, &snap));
    f.seq++;

    CHECK("record code = raw - 28, clamped (Y-T read domain)",
          scope_roll_record_code(128, -28) == 100 && scope_roll_record_code(10, -28) == 0 &&
          scope_roll_record_code(255, 10) == 255);
}

static void test_tb_step(void)
{
    uint8_t tb = 0x13, step = 5;
    bool roll = false;
    bool ch = scope_roll_tb_step(&tb, &roll, &step, +1, 21);
    CHECK("Y-T 0x13 slower -> 0x14, still Y-T", !ch && tb == 0x14 && !roll);
    ch = scope_roll_tb_step(&tb, &roll, &step, +1, 21);
    CHECK("Y-T 0x14 slower -> roll step 0 (stock 0x14, 50ms)", ch && roll && step == 0 && tb == 0x14);
    for (int i = 0; i < 8; i++)
        scope_roll_tb_step(&tb, &roll, &step, +1, 21);
    CHECK("eight more -> step 8 (20 s/div)", roll && step == 8);
    ch = scope_roll_tb_step(&tb, &roll, &step, +1, 21);
    CHECK("slowest holds, reports no change", !ch && roll && step == 8);
    ch = scope_roll_tb_step(&tb, &roll, &step, -1, 21);
    CHECK("faster in roll -> step 7", ch && step == 7);
    step = 0;
    ch = scope_roll_tb_step(&tb, &roll, &step, -1, 21);
    CHECK("faster from step 0 -> Y-T at 0x14", ch && !roll && tb == 0x14);
    tb = 0;
    ch = scope_roll_tb_step(&tb, &roll, &step, -1, 21);
    CHECK("fastest Y-T holds at 0", !ch && tb == 0 && !roll);
    CHECK("NULL / dir 0 refused", !scope_roll_tb_step(NULL, &roll, &step, 1, 21) &&
                                  !scope_roll_tb_step(&tb, &roll, &step, 0, 21));
}

static void test_disp_mode(void)
{
    CHECK("Y-T -> Roll -> X-Y -> Y-T",
          scope_disp_mode_cycle(SCOPE_DISP_YT, 1) == SCOPE_DISP_ROLL &&
          scope_disp_mode_cycle(SCOPE_DISP_ROLL, 1) == SCOPE_DISP_XY &&
          scope_disp_mode_cycle(SCOPE_DISP_XY, 1) == SCOPE_DISP_YT);
    CHECK("reverse: Y-T -> X-Y", scope_disp_mode_cycle(SCOPE_DISP_YT, -1) == SCOPE_DISP_XY);
    CHECK("labels", strcmp(scope_disp_mode_label(SCOPE_DISP_ROLL), "Roll") == 0 &&
                    strcmp(scope_disp_mode_label(SCOPE_DISP_XY), "X-Y") == 0);
}

static void test_geometry(void)
{
    CHECK("newest point at the right edge", scope_roll_age_at_column(319, 320) == 0);
    CHECK("one 32 px division back = 25 points (stock label holds)",
          scope_roll_age_at_column(319 - 32, 320) == 25 &&
          scope_roll_age_at_column(319 - 64, 320) == 50);
    int mono = 1;
    for (uint16_t x = 1; x < 320; x++)
        mono &= scope_roll_age_at_column(x, 320) <= scope_roll_age_at_column(x - 1, 320);
    CHECK("age grows right to left (stock scrolls R-to-L)", mono);
    CHECK("left edge = age 249 (250 of 301 visible)", scope_roll_age_at_column(0, 320) == 249);
    CHECK("off-trace column refused", scope_roll_age_at_column(320, 320) == 0xFFFF);

    static scope_roll_fifo_t f;
    scope_roll_fifo_reset(&f);
    for (int i = 0; i < 100; i++) scope_roll_fifo_push(&f, 1, 2);
    CHECK("visible = count while filling", scope_roll_visible(&f, 320) == 100);
    for (int i = 0; i < 400; i++) scope_roll_fifo_push(&f, 1, 2);
    CHECK("visible capped at 250 when full", scope_roll_visible(&f, 320) == 250);

    scope_roll_vmap_t m = { true, 0, 255, 128, 18, 224, 206 };
    CHECK("fixed: centre code at band middle", scope_roll_y(&m, 128) == (18 + 224) / 2);
    CHECK("fixed: higher code is higher on screen", scope_roll_y(&m, 160) < scope_roll_y(&m, 128));
    CHECK("fixed: clamped into the band", scope_roll_y(&m, 255) >= 18 && scope_roll_y(&m, 0) < 224);

    scope_roll_fifo_reset(&f);
    for (int i = 0; i < 50; i++) scope_roll_fifo_push(&f, (uint8_t)(60 + i), 0);
    scope_roll_vmap_t fm = { false, 0, 0, 0, 18, 224, 206 };
    bool fit = scope_roll_fit(&f, 0, scope_roll_visible(&f, 320), &fm);
    CHECK("fit: min..max of the visible points", fit && fm.lo == 60 && fm.hi == 109);
    CHECK("fit: min at the bottom, max at the top",
          scope_roll_y(&fm, 60) == 224 - 3 && scope_roll_y(&fm, 109) == 18 + 2);
    scope_roll_fifo_reset(&f);
    for (int i = 0; i < 5; i++) scope_roll_fifo_push(&f, 0, 100);
    fit = scope_roll_fit(&f, 1, 5, &fm);
    CHECK("fit: a flat input is spread over >= 16 counts", fit && fm.hi - fm.lo == 16);
    scope_roll_fifo_reset(&f);
    CHECK("fit: empty FIFO refused", !scope_roll_fit(&f, 0, 250, &fm));
}

/* The amplitude correction. Range 7, nominal 80 mV/count (stock 0x0804BFB8);
 * the unit's g = (m - 28)/150 enters through scope_cal. */
static void test_volts(void)
{
    const uint8_t zero = scope_measure_zero_for(1, -10, -28);    /* 100 + pos = 90 */
    CHECK("zero code with the cal page = 100 + pos", zero == 90);
    int ok150 = 1, not192 = 1;
    const uint8_t ms[] = { 183, 186, 220, 221, 230 };            /* both sides of stock's gate */
    for (unsigned i = 0; i < sizeof ms; i++) {
        const float g = ((float)ms[i] - 28.0f) / 150.0f;
        scope_cal_set_stock_gain(1, g);
        const float vpc = scope_cal_volts_per_count(1, 7);
        const float v = scope_roll_point_volts((uint8_t)(zero + 10), zero, vpc);
        const float want = 10.0f * 0.080f * 150.0f / ((float)ms[i] - 28.0f);
        const float v192 = 10.0f * 0.080f * 192.0f / ((float)ms[i] - 28.0f);
        ok150 &= fabsf(v - want) < 1e-5f;
        not192 &= fabsf(v - v192) > 1e-3f;
        /* Y-T: (code - zero) x the same volts_per_count, by construction. */
        ok150 &= fabsf(v - ((float)(zero + 10) - (float)zero) * vpc) < 1e-6f;
    }
    CHECK("roll volts = counts x k_r x 150/(m-28), m 183..230", ok150);
    CHECK("no /192, and no m >= 221 gate (V1.1.2 regression)", not192);
    scope_cal_set_stock_gain(1, 0.0f);
    CHECK("negative counts give negative volts",
          scope_roll_point_volts(80, 90, 0.01f) < 0.0f);
    CHECK("uncalibrated (vpc 0) -> 0, caller prints counts",
          scope_roll_point_volts(200, 90, 0.0f) == 0.0f);
}

int main(void)
{
    printf("scope_roll (parity C11)\n");
    test_table();
    test_fifo();
    test_tb_step();
    test_disp_mode();
    test_geometry();
    test_volts();
    printf("%d/%d passed\n", passed, run);
    return passed == run ? 0 : 1;
}
