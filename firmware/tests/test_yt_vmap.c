/*
 * The Y-T vertical transform (src/ui/yt_vmap.h), as scope_yt_frame.c builds
 * it for the plot and every other consumer.
 *
 * [1] true scale (stock geometry): 0 V at position 0 on the centre grid line,
 *     one 26 px division = the range's nominal volts/div (codes/div from the
 *     unit's gain), where the ADC rails land, clipping, overlay.
 * [2] true scale == its closed formula, code for code.
 * [3] autofit == the former autofit_prep()/autofit_y(), code for code, for a
 *     range of windows and both band layouts (unchanged when true scale is off).
 * [4] band layout: split halves only when both channels autofit.
 * [5] fractional codes: the row of a code between codes and back.
 * [6] the draw spans stay on the plot.
 * [7] panes (plot_pane.h): the full plot's numbers, a half pane's
 *     graticule and true scale on it.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <stdlib.h>
#include "yt_vmap.h"
#include "plot_pane.h"
#include "scope_cal.h"

/* scope_ui.c's plot: SCOPE_TOP 18, SCOPE_BOT LCD_HEIGHT - 16 = 224. */
#define TOP   18
#define BOT   224
#define H     (BOT - TOP)
#define MID   (TOP + H / 2)
#define DIV   26                  /* the full plot's division (plot_pane.h) */
#define GMID  (TOP + 4 * DIV)     /* SCOPE_GRID_MID_Y */
#define ZERO  100                 /* record 0 V at position 0, page in use */

static int fails = 0;
#define CHECK(cond, ...) do { if (cond) { printf("  PASS: "); } else { printf("  FAIL: "); fails++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

/* The pre-change autofit_prep() + autofit_y(), given the window's min/max. */
static int old_autofit_y(int top, int bot, int mn, int mx, int v)
{
    int span = mx - mn;
    if (span < 8) span = 8;
    int h = (bot - top) - 3;
    if (h < 4) h = 4;
    int yy = (bot - 2) - ((v - mn) * h) / span;
    if (yy < top)  yy = top;
    if (yy >= bot) yy = bot - 1;
    return yy;
}

static int y_of(const yt_vmap_t *a, int v) { return yt_vmap_clip(a, yt_vmap_raw(a, v)); }

int main(void)
{
    /* Unit #1's page: g = 1.032 on both channels. Range 6 is 1 V/div
     * nominal, 40/1.032 mV per count, so one division holds 25.8 codes. */
    scope_cal_set_stock_gain(1, 1.032f);
    scope_cal_set_stock_gain(2, 1.032f);
    const float cpd = scope_cal_codes_per_div(1, 6);
    yt_vmap_t t;
    yt_vmap_true(&t, TOP, BOT, GMID, DIV, 0, ZERO, cpd);

    printf("[1] true scale: stock geometry, 0 V on the centre line\n");
    CHECK(GMID == 122 && (GMID - TOP) % 26 == 0, "centre line on the 4th grid line (row %d)", GMID);
    CHECK(fabsf(scope_cal_volts_per_div(1, 6) - 1.0f) < 1e-6f && fabsf(cpd - 25.8f) < 1e-3f,
          "range 6: 1 V/div nominal, %.3f codes/div", cpd);
    CHECK(y_of(&t, ZERO) == GMID, "code %d (0 V at position 0) on the centre line", ZERO);
    {
        int bad = 0;
        for (int k = -3; k <= 4; k++) {
            const float code = ZERO + cpd * (float)k;            /* k volts */
            const int c = (int)lroundf(code);
            if (c < 0 || c > 255) continue;
            if (abs(y_of(&t, c) - (GMID - 26 * k)) > 1) bad++;
        }
        CHECK(bad == 0, "k volts at 1 V/div lands on grid line k (within the 1-code rounding)");
    }
    {
        yt_vmap_t n;
        yt_vmap_true(&n, TOP, BOT, GMID, DIV, 0, ZERO, 25.0f);       /* nominal gain */
        int bad = 0;
        for (int k = -4; k <= 4; k++) {
            const int c = ZERO + 25 * k;
            if (c >= 0 && c <= 255 && yt_vmap_raw(&n, c) != GMID - 26 * k) bad++;
        }
        CHECK(bad == 0, "nominal gain: 25 codes = one 26 px division exactly");
    }
    CHECK(y_of(&t, 0) == GMID + (100 * 2600) / 2580 && y_of(&t, 0) < BOT,
          "ADC floor (code 0) at row %d: %.2f div below centre, on the plot",
          y_of(&t, 0), (float)ZERO / cpd);
    CHECK(y_of(&t, 255) == TOP && yt_vmap_raw(&t, 227) < TOP,
          "upper rail (record 227, %.2f div above centre) clips at the plot top",
          (227.0f - ZERO) / cpd);
    CHECK(yt_vmap_clip(&t, TOP - 50) == TOP && yt_vmap_clip(&t, BOT + 50) == BOT - 1,
          "clipped to [%d, %d)", TOP, BOT);
    yt_vmap_t t2;
    yt_vmap_true(&t2, TOP, BOT, GMID, DIV, 1, ZERO, scope_cal_codes_per_div(2, 6));
    {
        int diff = 0;
        for (int v = 0; v < 256; v++) diff += (y_of(&t, v) != y_of(&t2, v));
        CHECK(diff == 0 && t.fx_ch == 0 && t2.fx_ch == 1,
              "both channels share the full plot and the same rows (overlay)");
    }
    {
        /* Every calibrated range: one division is that range's nominal volts. */
        int bad = 0;
        for (uint8_t r = 0; r < 10u; r++) {
            const float vpd = scope_cal_volts_per_div(1, r);
            const float vpc = scope_cal_volts_per_count(1, r);
            if (!(vpd > 0.0f) || fabsf(scope_cal_codes_per_div(1, r) * vpc - vpd) > 1e-5f * vpd) bad++;
        }
        CHECK(bad == 0, "every range: codes/div x volts/count == nominal volts/div");
    }

    /* One zero for trace, tag and measurements (scope_measure_zero_for):
     * on every calibrated range and position, the 0 V code 100 + pos is
     * drawn pos / codes-per-div divisions above the centre line, and a
     * record sitting on it is drawn on the ground tag's row (same map). */
    {
        int bad = 0;
        for (uint8_t r = 0; r < 10u; r++)
            for (int pos = -100; pos <= 100; pos += 5) {
                yt_vmap_t m;
                const float c = scope_cal_codes_per_div(2, r);
                yt_vmap_true(&m, TOP, BOT, GMID, DIV, 1, ZERO, c);
                const int want = GMID - (int)lroundf((float)pos * 26.0f / c);
                const int got = yt_vmap_raw(&m, ZERO + pos);
                if (abs(got - want) > 1) bad++;
            }
        CHECK(bad == 0, "0 V at position p: p / codes-per-div divisions above centre, every range");
    }

    printf("[2] the map formula\n");
    {
        int bad = 0;
        for (int v = 0; v < 256; v++) {
            int ref = GMID - ((v - ZERO) * 2600) / 2580;
            if (ref < TOP) ref = TOP;
            if (ref >= BOT) ref = BOT - 1;
            if (y_of(&t, v) != ref) bad++;
        }
        CHECK(bad == 0, "row = centre - (code - zero) x 26 / codes-per-div, clipped (%d differ)", bad);
    }

    printf("[3] autofit unchanged\n");
    {
        static const int win[][2] = { {126, 130}, {0, 255}, {40, 200}, {100, 101},
                                      {128, 128}, {250, 255}, {0, 3}, {60, 190} };
        static const int band[][2] = { {TOP, BOT}, {TOP, MID - 1}, {MID + 1, BOT} };
        int diff = 0, n = 0;
        for (unsigned w = 0; w < sizeof(win) / sizeof(win[0]); w++)
            for (unsigned b = 0; b < 3; b++) {
                yt_vmap_t a;
                yt_vmap_autofit(&a, (uint8_t)win[w][0], (uint8_t)win[w][1],
                                (int16_t)band[b][0], (int16_t)band[b][1]);
                for (int v = 0; v < 256; v++, n++)
                    if (y_of(&a, v) != old_autofit_y(band[b][0], band[b][1], win[w][0], win[w][1], v))
                        diff++;
                if (a.fx_ch != -1) diff++;
            }
        CHECK(diff == 0, "== autofit_prep/autofit_y on %d (window, band, code) cases", n);
        yt_vmap_t a;
        yt_vmap_autofit(&a, 126, 130, TOP, BOT);
        CHECK(y_of(&a, 126) - y_of(&a, 130) == ((4 * (H - 3)) / 8),
              "autofit: 4 counts of noise still stretch to half the band (min span 8)");
        CHECK(y_of(&t, 126) - y_of(&t, 130) <= 4,
              "true scale: the same 4 counts are %d px", y_of(&t, 126) - y_of(&t, 130));
    }

    printf("[4] band layout\n");
    CHECK(yt_vmap_split(true, true, false, false), "both on, both autofit: split");
    CHECK(!yt_vmap_split(true, true, true, true), "both on, both true scale: overlaid");
    CHECK(!yt_vmap_split(true, true, true, false) && !yt_vmap_split(true, true, false, true),
          "one true scale: no split (the autofit one uses the full plot)");
    CHECK(!yt_vmap_split(true, false, false, false) && !yt_vmap_split(false, true, false, false),
          "one channel: no split");
    {
        int16_t a, b, c, d, e, f;
        yt_vmap_band(TOP, BOT, true, 0, &a, &b);
        yt_vmap_band(TOP, BOT, true, 1, &c, &d);
        yt_vmap_band(TOP, BOT, false, 1, &e, &f);
        CHECK(a == TOP && b == MID - 1 && c == MID + 1 && d == BOT,
              "split: CH1 [%d,%d) CH2 [%d,%d), as before", a, b, c, d);
        CHECK(e == TOP && f == BOT, "unsplit: the full plot");
    }

    printf("[5] fractional codes: row of a code and code of a row\n");
    {
        yt_vmap_t a, z;
        yt_vmap_autofit(&a, 40, 200, TOP, BOT);
        yt_vmap_autofit(&z, 120, 121, TOP, BOT);           /* a fit zoomed to its 8-count floor */
        const yt_vmap_t *m[3] = { &t, &a, &z };
        int bad = 0, off = 0;
        for (int i = 0; i < 3; i++) {
            for (int y = TOP; y < BOT; y++) {
                const float c = yt_vmap_code_of(m[i], (float)y);
                if (fabsf(yt_vmap_row_of(m[i], c) - (float)y) > 1e-3f) bad++;
            }
            for (int c = 0; c <= 255; c++)          /* on whole codes, the integer map to a row */
                if (fabsf(yt_vmap_row_of(m[i], (float)c) - (float)yt_vmap_raw(m[i], c)) >= 1.0f) off++;
        }
        CHECK(bad == 0, "row -> code -> row is the identity on every map (%d rows off)", bad);
        CHECK(off == 0, "a whole code's row is within a row of yt_vmap_raw's (%d off)", off);
    }

    printf("[6] every map kind, every code pair: the draw spans stay on the plot\n");
    {
        /* A trace column: rows lo..hi between the previous and this code
         * (the plot adds the row under hi, clipped by its strip). Saturated
         * codes (0, 255), noise windows and empty-looking bands included. */
        yt_vmap_t maps[8];
        int nm = 0;
        yt_vmap_true(&maps[nm++], TOP, BOT, GMID, DIV, 0, ZERO, cpd);
        yt_vmap_true(&maps[nm++], TOP, BOT, GMID, DIV, 1, 128, 25.0f);
        yt_vmap_autofit(&maps[nm++], 0, 255, TOP, BOT);
        yt_vmap_autofit(&maps[nm++], 0, 255, TOP, MID - 1);
        yt_vmap_autofit(&maps[nm++], 255, 255, MID + 1, BOT);
        yt_vmap_autofit(&maps[nm++], 0, 0, TOP, BOT);
        yt_vmap_autofit(&maps[nm++], 126, 130, TOP, MID - 1);
        yt_vmap_autofit(&maps[nm++], 0, 255, 100, 102);       /* band shorter than the margin */
        int bad = 0;
        long steps = 0;
        for (int m = 0; m < nm; m++)
            for (int p = 0; p < 256; p++)
                for (int v = 0; v < 256; v++) {
                    const int y0 = y_of(&maps[m], p), y1 = y_of(&maps[m], v);
                    int lo = y0 < y1 ? y0 : y1, hi = y0 < y1 ? y1 : y0;
                    if (lo < maps[m].y_top || hi >= maps[m].y_bot) bad++;
                    for (int16_t r = (int16_t)lo; r <= (int16_t)hi; r++) steps++;
                }
        CHECK(bad == 0 && steps > 0, "%d maps x 65536 code pairs: %d spans off the band, %ld rows", nm, bad, steps);
    }

    printf("[7] panes\n");
    {
        const plot_pane_t full = plot_pane_make(CANVAS_PLOT, TOP, BOT, 200);
        CHECK(full.div_px == DIV && full.grid_mid == GMID && full.mid == MID && full.band_bot == 200,
              "full plot: %d rows a division, centre line %d, middle row %d (the fixed layout)",
              full.div_px, full.grid_mid, full.mid);
        /* The split view's upper half: rows 18..120, the divider on 121. */
        const plot_pane_t half = plot_pane_make(CANVAS_SPLIT_TIME, TOP, TOP + H / 2, TOP + H / 2);
        CHECK(half.div_px == 13 && half.grid_mid == TOP + 4 * 13,
              "half pane: %d rows a division, centre line %d", half.div_px, half.grid_mid);
        yt_vmap_t m;
        yt_vmap_true(&m, half.top, half.bot, half.grid_mid, half.div_px, 0, ZERO, 25.0f);
        int bad = 0;
        for (int k = -4; k <= 3; k++) {
            const int c = ZERO + 25 * k;
            if (c >= 0 && c <= 255 && yt_vmap_raw(&m, c) != half.grid_mid - half.div_px * k) bad++;
        }
        CHECK(bad == 0, "half pane, nominal gain: 25 codes = one 13 px division exactly");
    }

    if (fails) { printf("test_yt_vmap: %d FAILED\n", fails); return 1; }
    printf("test_yt_vmap: all checks passed\n");
    return 0;
}
