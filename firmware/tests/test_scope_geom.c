/*
 * Host tests for the Y-T horizontal geometry (parity C5, src/ui/scope_geom.h):
 * which record sample, at which phase, each screen column shows.
 *
 *   [1] the graticule: 13 vertical lines at 0, 25, .., 300 and none outside;
 *   [2] at zoom 1 the mapping is EXACTLY the pre-C5 buf[toff + x]: the anchor
 *       sample sits on column tx whatever the sub-sample fraction (the mask
 *       overlay was taught on that alignment);
 *   [3] at zoom 10, columns tx..tx+9 are the anchor sample at phases 0..9 and
 *       column tx-1 is the previous sample at phase 9;
 *   [4] the sub-sample crossing: half a sample back at zoom 10 moves the
 *       window 5 columns, and the anchor still reports column tx;
 *   [5] stock's window: hardware anchor at 512, hpos -150..+150, every zoom
 *       -> the window stays inside [HEAD_SKIP, 1024) and is never clamped;
 *   [6] clamping at the record end keeps every column inside the record and
 *       reports where the anchor really landed;
 *   [7] span (the samples a window needs either side of the trigger);
 *   [8] the crossing fraction, both directions;
 *   [9] fill == the interpolator on the whole record, column by column, for
 *       every zoom and windows touching both record ends (the stack copy of
 *       the source window must not shift or truncate anything);
 *  [10] a zoom that does not divide 20 falls back to one column per sample.
 */
#include "../src/ui/scope_geom.h"
#include "../src/dsp/sinc_interp.h"
#include "../src/ui/scope_record.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

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

#define N 1024
static const uint8_t Ls[6] = { 1, 2, 4, 5, 10, 20 };

static void test_grid(void)
{
    int lines = 0;
    for (int x = -5; x < 330; x++) {
        if (scope_geom_vgrid(x)) {
            lines++;
            CHECK(x % 25 == 0 && x >= 0 && x <= 300, "[1] grid line at column %d", x);
        }
    }
    CHECK(lines == 13, "[1] %d vertical lines, stock has 13 (12 divisions)", lines);
    CHECK(SCOPE_GEOM_COLS == SCOPE_GEOM_DIVS * SCOPE_GEOM_DIV_PX + 1u, "[1] 301 columns");
    CHECK(SCOPE_GEOM_TRIG_COL_MIN == 0 && SCOPE_GEOM_TRIG_COL_MAX == 300,
          "[1] trigger column 150 +- 150");
}

static void test_zoom1_is_the_old_mapping(void)
{
    for (int tx = 0; tx <= 300; tx += 37) {
        for (int32_t back = 0; back <= 256; back += 64) {
            scope_window_t v;
            const int at = scope_geom_fit(&v, 1, 500, back, tx, 0, N);
            CHECK(at == tx, "[2] tx %d back %d: anchor landed on %d", tx, (int)back, at);
            CHECK(scope_geom_first_sample(&v) == 500 - tx, "[2] toff should be %d", 500 - tx);
            for (int x = 0; x < (int)SCOPE_GEOM_COLS; x += 7) {
                int32_t n; uint8_t p;
                scope_geom_col_src(&v, x, &n, &p);
                CHECK(n == 500 - tx + x && p == 0, "[2] column %d -> %d/%u, want %d/0",
                      x, (int)n, p, 500 - tx + x);
            }
        }
    }
}

static void test_zoom10_columns(void)
{
    scope_window_t v;
    const int tx = 150;
    const int at = scope_geom_fit(&v, 10, 512, 0, tx, 0, N);
    CHECK(at == tx, "[3] anchor landed on %d", at);
    for (int k = 0; k < 10; k++) {
        int32_t n; uint8_t p;
        scope_geom_col_src(&v, tx + k, &n, &p);
        CHECK(n == 512 && p == k, "[3] column %d -> %d/%u, want 512/%d", tx + k, (int)n, p, k);
    }
    int32_t n; uint8_t p;
    scope_geom_col_src(&v, tx - 1, &n, &p);
    CHECK(n == 511 && p == 9, "[3] column %d -> %d/%u, want 511/9", tx - 1, (int)n, p);
    scope_geom_col_src(&v, tx + 10, &n, &p);
    CHECK(n == 513 && p == 0, "[3] column %d -> %d/%u, want 513/0", tx + 10, (int)n, p);
}

static void test_subsample_crossing(void)
{
    scope_window_t v0, v1;
    scope_geom_fit(&v0, 10, 512, 0, 150, 0, N);
    const int at = scope_geom_fit(&v1, 10, 512, 128, 150, 0, N);
    CHECK(v0.u0 - v1.u0 == 5, "[4] half a sample back must move the window 5 columns (moved %d)",
          (int)(v0.u0 - v1.u0));
    CHECK(at == 150, "[4] the crossing must still be reported on column 150 (got %d)", at);
    int32_t n; uint8_t p;
    scope_geom_col_src(&v1, 150, &n, &p);
    CHECK(n == 511 && p == 5, "[4] column 150 should show 511 + 5/10, got %d/%u", (int)n, p);
}

static void test_stock_window_fits(void)
{
    for (int li = 0; li < 6; li++) {
        for (int hpos = -150; hpos <= 150; hpos += 10) {
            scope_window_t v;
            const int tx = 150 + hpos;
            const int at = scope_geom_fit(&v, Ls[li], 512, 128, tx, 0, N);
            CHECK(at == tx, "[5] L %u hpos %d: clamped (anchor on %d)", Ls[li], hpos, at);
            int32_t n0, n1; uint8_t p;
            scope_geom_col_src(&v, 0, &n0, &p);
            scope_geom_col_src(&v, (int)SCOPE_GEOM_COLS - 1, &n1, &p);
            CHECK(n0 >= (int32_t)SCOPE_RECORD_HEAD_SKIP && n1 < N,
                  "[5] L %u hpos %d: window %d..%d leaves [HEAD_SKIP, 1024)", Ls[li], hpos,
                  (int)n0, (int)n1);
        }
    }
}

static void test_clamp_at_record_end(void)
{
    for (int li = 0; li < 6; li++) {
        scope_window_t v;
        const int at = scope_geom_fit(&v, Ls[li], 1020, 0, 10, 0, N);
        int32_t n1; uint8_t p;
        scope_geom_col_src(&v, (int)SCOPE_GEOM_COLS - 1, &n1, &p);
        CHECK(n1 <= N - 1, "[6] L %u: last column reads sample %d", Ls[li], (int)n1);
        int32_t na; uint8_t pa;
        scope_geom_col_src(&v, at, &na, &pa);
        CHECK(na == 1020 && pa == 0, "[6] L %u: reported column %d shows %d/%u, not the anchor",
              Ls[li], at, (int)na, pa);
        CHECK(at > 10, "[6] L %u: a clamped window must report the moved column", Ls[li]);
    }
    scope_window_t v;
    const int at = scope_geom_fit(&v, 1, 3, 0, 150, 0, N);
    CHECK(scope_geom_first_sample(&v) == 0 && at == 3, "[6] start clamps at 0 (at %d)", at);
}

static void test_span(void)
{
    int32_t b, a;
    scope_geom_span(1, 150, &b, &a);
    CHECK(b == 150 && a == 150, "[7] L1 tx150 span %d/%d", (int)b, (int)a);
    scope_geom_span(1, 0, &b, &a);
    CHECK(b == 0 && a == 300, "[7] L1 tx0 span %d/%d", (int)b, (int)a);
    scope_geom_span(20, 150, &b, &a);
    CHECK(b == 8 && a == 8, "[7] L20 tx150 span %d/%d", (int)b, (int)a);
    scope_geom_span(2, 400, &b, &a);
    CHECK(b == 150 && a == 0, "[7] tx past 300 clamps (span %d/%d)", (int)b, (int)a);
}

static void test_back_fraction(void)
{
    CHECK(scope_geom_back_q8(10, 20, 15) == 128, "[8] rising half way");
    CHECK(scope_geom_back_q8(10, 20, 20) == 0, "[8] rising on cur");
    CHECK(scope_geom_back_q8(10, 20, 10) == 256, "[8] rising on prev");
    CHECK(scope_geom_back_q8(20, 10, 15) == 128, "[8] falling half way");
    CHECK(scope_geom_back_q8(20, 10, 12) == 51, "[8] falling 2/10 back -> 51");
    CHECK(scope_geom_back_q8(15, 15, 15) == 0, "[8] flat pair");
    CHECK(scope_geom_back_q8(10, 20, 40) == 0 && scope_geom_back_q8(10, 20, 0) == 256,
          "[8] clamps outside the pair");
}

static void test_fill_matches_interpolator(void)
{
    static uint8_t rec[N];
    for (int i = 0; i < N; i++)
        rec[i] = (uint8_t)lround(128.0 + 80.0 * sin(0.37 * i) + 30.0 * sin(1.9 * i + 0.4));
    uint8_t out[SCOPE_GEOM_COLS];
    static const int32_t anchors[5] = { 0, 9, 512, 1015, 1023 };
    static const int txs[3] = { 0, 150, 300 };
    for (int li = 0; li < 6; li++) {
        for (int ai = 0; ai < 5; ai++) {
            for (int ti = 0; ti < 3; ti++) {
                scope_window_t v;
                scope_geom_fit(&v, Ls[li], anchors[ai], 77, txs[ti], 0, N);
                memset(out, 0xA5, sizeof(out));
                scope_geom_fill(rec, N, &v, out, SCOPE_GEOM_COLS);
                int bad = 0;
                for (int x = 0; x < (int)SCOPE_GEOM_COLS; x++) {
                    int32_t n; uint8_t p;
                    scope_geom_col_src(&v, x, &n, &p);
                    const uint8_t want = sinc_interp_at(rec, N, n, (uint8_t)(p * (20 / Ls[li])));
                    if (out[x] != want) bad++;
                    if (p == 0 && out[x] != rec[n]) bad++;
                }
                CHECK(bad == 0, "[9] L %u anchor %d tx %d: %d columns differ from the interpolator",
                      Ls[li], (int)anchors[ai], txs[ti], bad);
            }
        }
    }
    /* NULL record: zeros, no crash. */
    scope_window_t v = { 0, 10 };
    memset(out, 0xA5, sizeof(out));
    scope_geom_fill(NULL, N, &v, out, SCOPE_GEOM_COLS);
    CHECK(out[0] == 0 && out[300] == 0, "[9] NULL record must fill zeros");
}

static void test_bad_zoom(void)
{
    scope_window_t v;
    const int at = scope_geom_fit(&v, 3, 500, 0, 150, 0, N);
    CHECK(v.L == 1 && at == 150 && scope_geom_first_sample(&v) == 350,
          "[10] zoom 3 must fall back to 1 (L %u at %d)", v.L, at);
    scope_geom_fit(&v, 0, 500, 0, 150, 0, N);
    CHECK(v.L == 1, "[10] zoom 0 must fall back to 1");
}

int main(void)
{
    test_grid();
    test_zoom1_is_the_old_mapping();
    test_zoom10_columns();
    test_subsample_crossing();
    test_stock_window_fits();
    test_clamp_at_record_end();
    test_span();
    test_back_fraction();
    test_fill_matches_interpolator();
    test_bad_zoom();
    if (failures == 0) {
        printf("test_scope_geom: all checks passed\n");
        return 0;
    }
    printf("test_scope_geom: %d check(s) FAILED\n", failures);
    return 1;
}
