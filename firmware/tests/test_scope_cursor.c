/*
 * Host tests for the time views' cursors (src/ui/scope_cursor.c): lines
 * kept in the units of what they mark, put on the screen at the scale on it.
 *
 *   [1] time: at every timebase code, merged or not, and every trigger
 *       column, a line put on a column comes back to that column (pixel ->
 *       ticks -> pixel), and stays on its time when the code changes.
 *   [2] roll: the same at every roll period, in ms back from the newest.
 *   [3] level: on every channel x range x probe x calibration source, on a
 *       true-scale map at any 0 V and on fitted bands, a line put on a row
 *       comes back to that row, inside the property's range; a range with
 *       no volts has no conversion.
 *   [4] one arrow press is CURSOR_STEP_PX pixels at every scale; the plot's
 *       edges hold it; a line off the plot comes to the edge on its side.
 *   [5] the defaults (a third, two thirds) and the placement of a pair that
 *       comes onto the screen; the start values are off the plot at every
 *       scale, so the first showing places them.
 *   [6] the readout strings, from the stored values.
 *   [7] the ranges: the named constants and what they rest on.
 */

#include "../src/ui/scope_cursor.h"
#include "../src/ui/scope_cal.h"
#include "../src/ui/scope_timebase.h"
#include "../src/ui/scope_roll.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

static int failures = 0;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
            failures++;                                                       \
        }                                                                     \
    } while (0)

/* The full plot (scope_ui.c): rows 18..223, the graticule's division 26 and
 * centre 122; the measurement strip from row 200. */
#define PLOT_TOP     18
#define PLOT_BOT     224
#define PLOT_DIV     26
#define PLOT_GMID    (PLOT_TOP + 4 * PLOT_DIV)
#define STRIP_TOP    200
#define CENTRE_CODE  100                  /* record 0 V at position 0, with the calibration page */
#define FAST_CODE_LAST 3                  /* 0x00-0x03: the only codes the merge runs at */

static scope_cursor_axes_t time_axes(bool roll, uint32_t per_col, int32_t ref)
{
    scope_cursor_axes_t a;
    memset(&a, 0, sizeof a);
    a.roll = roll;
    a.per_col = per_col;
    a.ref_col = ref;
    a.t_conf = roll ? SCOPE_CURSOR_RAW : SCOPE_CURSOR_MEASURED;
    a.cols = (int16_t)SCOPE_GEOM_COLS;
    return a;
}

static scope_cursor_axes_t level_axes(const yt_vmap_t *m, float zero, float vpc, int16_t row_end)
{
    scope_cursor_axes_t a = time_axes(false, 1u, 0);
    a.level_ok = true;
    a.channel = 1u;
    a.map = *m;
    a.zero_code = zero;
    a.volts_per_code = vpc;
    a.v_conf = SCOPE_CURSOR_MEASURED;
    a.row_top = m->y_top;
    a.row_end = row_end;
    return a;
}

/* Every pixel of the plot for line sel: the value there, back to the pixel. */
static int round_trips(const scope_cursor_axes_t *a, cursor_sel_t sel, int lo, int hi, int *clamped)
{
    int bad = 0;
    int32_t vlo, vhi;
    scope_cursor_range(a, sel, &vlo, &vhi);
    for (int px = lo; px < hi; px++) {
        int32_t v, back;
        if (!scope_cursor_at(a, sel, px, &v) || !scope_cursor_px(a, sel, v, &back) || back != px) bad++;
        if (clamped && (v == vlo || v == vhi)) (*clamped)++;
    }
    return bad;
}

static void test_time(void)
{
    printf("[1] Y-T time in ticks\n");
    static const int32_t refs[] = { 0, 1, 150, 299, 300 };
    int bad = 0, n = 0, moved = 0;
    for (uint8_t c = 0; c < SCOPE_TIMEBASE_CODE_COUNT; c++)
        for (int il = 0; il < 2; il++) {
            if (il && c > FAST_CODE_LAST) continue;
            for (size_t r = 0; r < sizeof refs / sizeof refs[0]; r++) {
                const scope_cursor_axes_t a = time_axes(false, scope_timebase_column_ticks(c, il != 0), refs[r]);
                bad += round_trips(&a, CURSOR_SEL_V1, 0, SCOPE_GEOM_COLS, NULL);
                n++;
            }
            /* a line put on column 200 at this code, viewed at the next: it
             * keeps its time, and comes back to 200 */
            if (c + 1u < SCOPE_TIMEBASE_CODE_COUNT) {
                const scope_cursor_axes_t a = time_axes(false, scope_timebase_column_ticks(c, il != 0), 150);
                const scope_cursor_axes_t b = time_axes(false, scope_timebase_column_ticks((uint8_t)(c + 1u), false), 150);
                int32_t t, px, back;
                (void)scope_cursor_at(&a, CURSOR_SEL_V2, 200, &t);
                (void)scope_cursor_px(&b, CURSOR_SEL_V2, t, &px);
                (void)scope_cursor_px(&a, CURSOR_SEL_V2, t, &back);
                if (back != 200 || t != 50 * (int32_t)a.per_col) bad++;
                if (px != 200) moved++;
            }
        }
    CHECK(bad == 0, "time: %d of %d axes lose a column (pixel -> ticks -> pixel)", bad, n);
    CHECK(moved > 0, "a line keeps its time, so its column moves with the timebase");
    /* held views: columns the ratio clamp makes (test_paused_view [7]) */
    static const uint32_t held[] = { 2u, 25u, 50u, 2000u, 500000u };
    bad = 0;
    for (size_t i = 0; i < sizeof held / sizeof held[0]; i++) {
        const scope_cursor_axes_t a = time_axes(false, held[i], 137);
        bad += round_trips(&a, CURSOR_SEL_V2, 0, SCOPE_GEOM_COLS, NULL);
    }
    CHECK(bad == 0, "time: held columns keep their pixels too (%d off)", bad);
    const scope_cursor_axes_t none = time_axes(false, 0u, 150);
    int32_t v, px;
    CHECK(!scope_cursor_at(&none, CURSOR_SEL_V1, 10, &v) && !scope_cursor_px(&none, CURSOR_SEL_V1, 0, &px),
          "no column: no conversion");
}

static void test_roll(void)
{
    printf("[2] roll time in ms\n");
    int bad = 0, outside = 0;
    for (uint8_t s = 0; s < SCOPE_ROLL_STEPS; s++) {
        const scope_cursor_axes_t a = time_axes(true, scope_roll_period_ms(s).ms, (int32_t)SCOPE_GEOM_COLS - 1);
        bad += round_trips(&a, CURSOR_SEL_V1, 0, SCOPE_GEOM_COLS, NULL);
        int32_t v;
        (void)scope_cursor_at(&a, CURSOR_SEL_V1, (int32_t)SCOPE_GEOM_COLS - 1, &v);
        if (v != 0) bad++;
        (void)scope_cursor_at(&a, CURSOR_SEL_V1, 0, &v);
        if (v <= SCOPE_CURSOR_ROLL_MIN_MS) outside++;
    }
    CHECK(bad == 0, "roll: every period keeps every column, the newest is 0 ms (%d off)", bad);
    CHECK(outside == 0, "roll: the oldest column is inside the range at every period");
}

/* A source of probe factors for scope_cal (the channel's probe setting). */
static float g_probe = 1.0f;
static float probe_src(uint8_t ch) { (void)ch; return g_probe; }

static void test_level(void)
{
    printf("[3] levels in microvolts\n");
    static const float gains[] = { 0.0f, 0.8f, 1.2533f };     /* compiled table; calpage m 148 and 216 */
    static const float zeros[] = { 0.0f, 50.0f, 100.0f, 150.0f, 227.0f };
    static const struct { uint8_t mn, mx; } fits[] = { { 0, 255 }, { 40, 200 }, { 120, 121 }, { 0, 3 } };
    int bad = 0, clamped = 0, maps = 0, none_ok = 0, none_n = 0;
    scope_cal_set_probe_source(probe_src);
    for (size_t g = 0; g < sizeof gains / sizeof gains[0]; g++)
        for (uint8_t ch = 1; ch <= 2; ch++)
            for (uint8_t r = 0; r < SCOPE_CAL_RANGE_COUNT; r++)
                for (int pr = 0; pr < 2; pr++) {
                    scope_cal_set_stock_gain(1u, gains[g]);
                    scope_cal_set_stock_gain(2u, gains[g]);
                    g_probe = pr ? (float)SCOPE_PROBE_10X_FACTOR : 1.0f;
                    const float vpc = scope_cal_volts_per_count(ch, r);
                    yt_vmap_t m[1 + sizeof fits / sizeof fits[0] + 1];
                    int nm = 0;
                    if (vpc > 0.0f)
                        yt_vmap_true(&m[nm++], PLOT_TOP, PLOT_BOT, PLOT_GMID, PLOT_DIV, (int8_t)(ch - 1u),
                                     CENTRE_CODE, scope_cal_codes_per_div(ch, r));
                    for (size_t f = 0; f < sizeof fits / sizeof fits[0]; f++)
                        yt_vmap_autofit(&m[nm++], fits[f].mn, fits[f].mx, PLOT_TOP, STRIP_TOP);
                    yt_vmap_autofit(&m[nm++], 30, 90, (int16_t)(PLOT_TOP + (STRIP_TOP - PLOT_TOP) / 2 + 1), STRIP_TOP);
                    for (int i = 0; i < nm; i++)
                        for (size_t z = 0; z < sizeof zeros / sizeof zeros[0]; z++) {
                            const int16_t end = m[i].y_bot < STRIP_TOP ? m[i].y_bot : STRIP_TOP;
                            const scope_cursor_axes_t a = level_axes(&m[i], zeros[z], vpc, end);
                            if (vpc > 0.0f) {
                                bad += round_trips(&a, CURSOR_SEL_H1, a.row_top, a.row_end, &clamped);
                                maps++;
                            } else {
                                int32_t v;
                                none_n++;
                                none_ok += !scope_cursor_at(&a, CURSOR_SEL_H1, a.row_top, &v);
                            }
                        }
                }
    scope_cal_set_stock_gain(1u, 0.0f);
    scope_cal_set_stock_gain(2u, 0.0f);
    g_probe = 1.0f;
    printf("  %d maps with volts, %d without\n", maps, none_n);
    CHECK(bad == 0, "level: %d rows of %d maps lose their row (pixel -> uV -> pixel)", bad, maps);
    CHECK(clamped == 0, "level: every row of every plot is inside +-%ld uV (%d at the end)",
          (long)SCOPE_CURSOR_LEVEL_MAX, clamped);
    CHECK(none_n > 0 && none_ok == none_n, "a range with no volts has no conversion (%d of %d)", none_ok, none_n);

    yt_vmap_t t;
    yt_vmap_true(&t, PLOT_TOP, PLOT_BOT, PLOT_GMID, PLOT_DIV, 0, CENTRE_CODE, 25.0f);
    const scope_cursor_axes_t a = level_axes(&t, CENTRE_CODE, 0.04f, STRIP_TOP);
    int32_t v, px;
    CHECK(scope_cursor_at(&a, CURSOR_SEL_H1, PLOT_GMID, &v) && v == 0, "0 V on the centre line: %ld uV", (long)v);
    CHECK(scope_cursor_at(&a, CURSOR_SEL_H2, PLOT_GMID - PLOT_DIV, &v) && v == 1000000,
          "one division up = 25 codes x 40 mV = 1 V: %ld uV", (long)v);
    CHECK(scope_cursor_px(&a, CURSOR_SEL_H1, -500000, &px) && px == PLOT_GMID + PLOT_DIV / 2,
          "-0.5 V half a division down: row %ld", (long)px);
}

static void test_keys(void)
{
    printf("[4] one arrow press\n");
    int bad = 0;
    for (uint8_t c = 0; c < SCOPE_TIMEBASE_CODE_COUNT; c++) {
        const scope_cursor_axes_t a = time_axes(false, scope_timebase_column_ticks(c, false), 150);
        for (int px = 0; px < (int)SCOPE_GEOM_COLS; px += 7) {
            int32_t v, w, at;
            (void)scope_cursor_at(&a, CURSOR_SEL_V1, px, &v);
            for (int dir = -1; dir <= 1; dir += 2) {
                (void)scope_cursor_key(&a, CURSOR_SEL_V1, v, dir, &w);
                (void)scope_cursor_px(&a, CURSOR_SEL_V1, w, &at);
                int want = px + dir * CURSOR_STEP_PX;
                if (want < 0) want = 0;
                if (want > (int)SCOPE_GEOM_COLS - 1) want = (int)SCOPE_GEOM_COLS - 1;
                if (at != want) bad++;
            }
        }
    }
    CHECK(bad == 0, "time: UP / DOWN move the line %d columns at every code, held at the edges (%d off)",
          CURSOR_STEP_PX, bad);
    yt_vmap_t f;
    yt_vmap_autofit(&f, 120, 121, PLOT_TOP, STRIP_TOP);
    const scope_cursor_axes_t l = level_axes(&f, 100.0f, 0.02f, STRIP_TOP);
    bad = 0;
    for (int row = l.row_top; row < l.row_end; row++) {
        int32_t v, w, at;
        (void)scope_cursor_at(&l, CURSOR_SEL_H2, row, &v);
        (void)scope_cursor_key(&l, CURSOR_SEL_H2, v, -1, &w);
        (void)scope_cursor_px(&l, CURSOR_SEL_H2, w, &at);
        const int want = row - CURSOR_STEP_PX < l.row_top ? l.row_top : row - CURSOR_STEP_PX;
        if (at != want) bad++;
    }
    CHECK(bad == 0, "level: UP moves the line %d rows up on a zoomed fit (%d off)", CURSOR_STEP_PX, bad);
    /* off the plot: to the edge on its side, whatever the direction */
    const scope_cursor_axes_t a = time_axes(false, 1000u, 150);
    int32_t w, at;
    (void)scope_cursor_key(&a, CURSOR_SEL_V1, -1000000, +1, &w);
    (void)scope_cursor_px(&a, CURSOR_SEL_V1, w, &at);
    CHECK(at == 0, "a line off the left comes to column 0 first (%ld)", (long)at);
    (void)scope_cursor_key(&a, CURSOR_SEL_V1, 1000000, -1, &w);
    (void)scope_cursor_px(&a, CURSOR_SEL_V1, w, &at);
    CHECK(at == (int32_t)SCOPE_GEOM_COLS - 1, "a line off the right comes to the last column (%ld)", (long)at);
    (void)scope_cursor_key(&l, CURSOR_SEL_H1, SCOPE_CURSOR_LEVEL_MAX, +1, &w);
    (void)scope_cursor_px(&l, CURSOR_SEL_H1, w, &at);
    CHECK(at == l.row_top, "a level above the band comes to its top row (%ld)", (long)at);
    scope_cursor_axes_t nolevel = l;
    nolevel.level_ok = false;
    CHECK(!scope_cursor_key(&nolevel, CURSOR_SEL_H1, 123, -1, &w) && w == 123, "no conversion: the line stays");
}

static void test_defaults(void)
{
    printf("[5] defaults and placement\n");
    const scope_cursor_axes_t a = time_axes(false, 20u, 150);
    int32_t v[2], px;
    CHECK(scope_cursor_default(&a, CURSOR_SEL_V1, &v[0]) && scope_cursor_px(&a, CURSOR_SEL_V1, v[0], &px) &&
          px == (int32_t)SCOPE_GEOM_COLS / 3, "V1 starts a third across (%ld)", (long)px);
    CHECK(scope_cursor_default(&a, CURSOR_SEL_V2, &v[1]) && scope_cursor_px(&a, CURSOR_SEL_V2, v[1], &px) &&
          px == 2 * (int32_t)SCOPE_GEOM_COLS / 3, "V2 two thirds across (%ld)", (long)px);
    yt_vmap_t t;
    yt_vmap_true(&t, PLOT_TOP, PLOT_BOT, PLOT_GMID, PLOT_DIV, 0, CENTRE_CODE, 25.0f);
    const scope_cursor_axes_t l = level_axes(&t, CENTRE_CODE, 0.04f, STRIP_TOP);
    CHECK(scope_cursor_default(&l, CURSOR_SEL_H1, &v[0]) && scope_cursor_px(&l, CURSOR_SEL_H1, v[0], &px) &&
          px == PLOT_TOP + (STRIP_TOP - PLOT_TOP) / 3, "H1 a third down the band (%ld)", (long)px);
    CHECK(scope_cursor_default(&l, CURSOR_SEL_H2, &v[1]) && scope_cursor_px(&l, CURSOR_SEL_H2, v[1], &px) &&
          px == PLOT_TOP + 2 * (STRIP_TOP - PLOT_TOP) / 3, "H2 two thirds down (%ld)", (long)px);

    /* a pair comes on: a line on the plot keeps its place, one off it moves */
    int32_t pair[2] = { 40 * 20, SCOPE_CURSOR_TIME_MAX };
    scope_cursor_place(&a, CURSOR_SEL_V1, pair);
    CHECK(pair[0] == 40 * 20 && scope_cursor_px(&a, CURSOR_SEL_V2, pair[1], &px) &&
          px == 2 * (int32_t)SCOPE_GEOM_COLS / 3, "placement: V1 stays at its time, V2 comes to its default");
    scope_cursor_axes_t nolevel = l;
    nolevel.level_ok = false;
    int32_t lv[2] = { 7, -SCOPE_CURSOR_LEVEL_MAX };
    scope_cursor_place(&nolevel, CURSOR_SEL_H1, lv);
    CHECK(lv[0] == 7 && lv[1] == -SCOPE_CURSOR_LEVEL_MAX, "no conversion: nothing is placed");

    /* the start values are off the plot at every scale */
    scope_state_t s;
    scope_state_init(&s);
    int on = 0;
    for (uint8_t c = 0; c < SCOPE_TIMEBASE_CODE_COUNT; c++)
        for (int32_t ref = 0; ref < (int32_t)SCOPE_GEOM_COLS; ref += 50) {
            const scope_cursor_axes_t x = time_axes(false, scope_timebase_column_ticks(c, false), ref);
            for (int i = 0; i < 2; i++)
                on += scope_cursor_px(&x, CURSOR_SEL_V1, s.cursor.t_tick[i], &px) && scope_cursor_on_plot(&x, CURSOR_SEL_V1, px);
        }
    for (uint8_t st = 0; st < SCOPE_ROLL_STEPS; st++) {
        const scope_cursor_axes_t x = time_axes(true, scope_roll_period_ms(st).ms, (int32_t)SCOPE_GEOM_COLS - 1);
        for (int i = 0; i < 2; i++)
            on += scope_cursor_px(&x, CURSOR_SEL_V1, s.cursor.roll_ms[i], &px) && scope_cursor_on_plot(&x, CURSOR_SEL_V1, px);
    }
    for (uint8_t r = 0; r < SCOPE_CAL_RANGE_COUNT; r++) {
        const float vpc = scope_cal_volts_per_count(1u, r) * (float)SCOPE_PROBE_10X_FACTOR;
        if (!(vpc > 0.0f)) continue;
        yt_vmap_t m;
        yt_vmap_autofit(&m, 0, 255, PLOT_TOP, STRIP_TOP);
        const scope_cursor_axes_t x = level_axes(&m, 227.0f, vpc, STRIP_TOP);
        for (int i = 0; i < 2; i++)
            on += scope_cursor_px(&x, CURSOR_SEL_H1, s.cursor.level_uv[i], &px) && scope_cursor_on_plot(&x, CURSOR_SEL_H1, px);
    }
    CHECK(on == 0, "the start values are off the plot at every scale (%d on it)", on);
}

static void expect(const scope_cursor_reading_t r, const char *want)
{
    char s[16];
    scope_cursor_format(&r, s, sizeof s);
    CHECK(strcmp(s, want) == 0, "readout \"%s\", want \"%s\"", s, want);
}

static void test_readout(void)
{
    printf("[6] readouts from the stored values\n");
    const scope_cursor_axes_t yt = time_axes(false, 20u, 150);
    expect(scope_cursor_x(&yt, 2500), "1us");
    expect(scope_cursor_x(&yt, -25), "-10ns");
    expect(scope_cursor_dt(&yt, 0, 2500, CUR_SECOND_MINUS_FIRST), "1us");
    expect(scope_cursor_dt(&yt, 0, 2500, CUR_FIRST_MINUS_SECOND), "-1us");
    expect(scope_cursor_inv_dt(&yt, 0, 2500, CUR_SECOND_MINUS_FIRST), "1.00MHz");
    expect(scope_cursor_inv_dt(&yt, 77, 77, CUR_SECOND_MINUS_FIRST), "--");
    /* the time is the stored one, not its column's: a tick between columns reads */
    expect(scope_cursor_dt(&yt, 0, 1, CUR_SECOND_MINUS_FIRST), "0ns");
    expect(scope_cursor_dt(&yt, 0, SCOPE_CURSOR_TIME_MAX, CUR_SECOND_MINUS_FIRST), "602.00ms");
    const scope_cursor_axes_t roll = time_axes(true, 40u, 300);
    expect(scope_cursor_x(&roll, -1000), "-1.00s");
    expect(scope_cursor_dt(&roll, -1000, -500, CUR_SECOND_MINUS_FIRST), "500.00ms");
    expect(scope_cursor_inv_dt(&roll, -1000, -500, CUR_SECOND_MINUS_FIRST), "2.0Hz");
    yt_vmap_t t;
    yt_vmap_true(&t, PLOT_TOP, PLOT_BOT, PLOT_GMID, PLOT_DIV, 0, CENTRE_CODE, 25.0f);
    scope_cursor_axes_t l = level_axes(&t, CENTRE_CODE, 0.04f, STRIP_TOP);
    const microvolts_t a = { 1500000 }, b = { -250000 };
    expect(scope_cursor_y(&l, a), "1.50V");
    expect(scope_cursor_y(&l, b), "-250mV");
    expect(scope_cursor_dv(&l, a, b, CUR_SECOND_MINUS_FIRST), "-1.75V");
    expect(scope_cursor_dv(&l, a, b, CUR_FIRST_MINUS_SECOND), "1.75V");
    l.v_conf = SCOPE_CURSOR_PROVISIONAL;
    expect(scope_cursor_y(&l, a), "~1.50V");
    l.level_ok = false;
    expect(scope_cursor_y(&l, a), "--");
    expect(scope_cursor_dv(&l, a, b, CUR_SECOND_MINUS_FIRST), "--");
    CHECK(fabsf(scope_cursor_col_rate(&yt) - 125e6f) < 1.0f && fabsf(scope_cursor_col_rate(&roll) - 25.0f) < 1e-4f,
          "the column rate: 20 ticks = 125 MHz, 40 ms = 25 Hz");
}

static void test_ranges(void)
{
    printf("[7] ranges\n");
    CHECK(SCOPE_CURSOR_TIME_MAX == 1505000000, "Y-T: 301 columns x 5e6 ticks");
    CHECK(SCOPE_CURSOR_ROLL_MIN_MS == -240800 && SCOPE_ROLL_SLOWEST_PERIOD_MS == scope_roll_period_ms(SCOPE_ROLL_STEPS - 1u).ms,
          "roll: 301 points x the slowest period, 800 ms");
    CHECK(SCOPE_CURSOR_LEVEL_MAX == 2048000000, "level: 2 x 256 codes x 400 mV x 10");
    CHECK(fabsf(scope_cal_nominal_mv(SCOPE_CAL_RANGE_COUNT - 1u) * 1000.0f - (float)SCOPE_CURSOR_TOP_RANGE_UV_PER_CODE) < 0.5f,
          "the top range's nominal gain is stock's 400 mV a code");
}

int main(void)
{
    test_time();
    test_roll();
    test_level();
    test_keys();
    test_defaults();
    test_readout();
    test_ranges();
    if (failures == 0) {
        printf("test_scope_cursor: PASS\n");
        return 0;
    }
    printf("test_scope_cursor: %d FAILURES\n", failures);
    return 1;
}
