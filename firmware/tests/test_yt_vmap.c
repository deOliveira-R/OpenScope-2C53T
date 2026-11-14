/*
 * The Y-T vertical transform (src/ui/yt_vmap.h), as scope_ui.c builds it
 * for both renderers (full path and live compositor) and every consumer.
 *
 * [1] true scale: code 128 on the centre row, 256 counts over the full plot,
 *     32 counts within a pixel of one 26 px division, clipping at the edges.
 * [2] true scale == the former draw_channel_fixed() formula, code for code,
 *     and == the roll view's fixed map (scope_roll_y) over the full plot.
 * [3] autofit == the former autofit_prep()/autofit_y(), code for code, for a
 *     range of windows and both band layouts (unchanged when true scale is off).
 * [4] band layout: split halves only when both channels autofit.
 * [5] cursor scale: counts per row of each map.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <stdlib.h>
#include "yt_vmap.h"
#include "scope_roll.h"

/* scope_ui.c's plot: SCOPE_TOP 18, SCOPE_BOT LCD_HEIGHT - 16 = 224. */
#define TOP   18
#define BOT   224
#define H     (BOT - TOP)
#define MID   (TOP + H / 2)

static int fails = 0;
#define CHECK(cond, ...) do { if (cond) { printf("  PASS: "); } else { printf("  FAIL: "); fails++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

/* The pre-change draw_channel_fixed() transform, centre 128. */
static int old_fixed_y(int top, int bot, int v)
{
    int y_mid = (top + bot) / 2;
    int yy = y_mid - (v - 128) * H / 256;
    if (yy < top)  yy = top;
    if (yy >= bot) yy = bot - 1;
    return yy;
}

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
    yt_vmap_t t;
    yt_vmap_true(&t, TOP, BOT, 0);

    printf("[1] true scale\n");
    CHECK(MID == 121 && H == 206, "plot geometry: centre row %d, height %d", MID, H);
    CHECK(y_of(&t, 128) == MID, "code 128 on the centre row (%d)", y_of(&t, 128));
    CHECK(yt_vmap_raw(&t, 0) - yt_vmap_raw(&t, 256) == H,
          "256 counts span the full plot (%d px)", yt_vmap_raw(&t, 0) - yt_vmap_raw(&t, 256));
    {
        int bad = 0;
        for (int k = 1; k <= 4; k++) {
            int up = MID - yt_vmap_raw(&t, 128 + 32 * k);
            int dn = yt_vmap_raw(&t, 128 - 32 * k) - MID;
            /* 32 * 206 / 256 = 25.75 px per division, truncated toward zero */
            if (up != dn || up != (32 * k * H) / 256 || abs(up - 26 * k) > 1) bad++;
        }
        CHECK(bad == 0, "32k counts = k divisions within 1 px of 26k, symmetric (+32: %d px)",
              MID - yt_vmap_raw(&t, 160));
    }
    CHECK(y_of(&t, 0) == BOT - 1 && y_of(&t, 255) == MID - (127 * H) / 256,
          "code 0 -> last row %d, code 255 -> row %d", y_of(&t, 0), y_of(&t, 255));
    CHECK(yt_vmap_clip(&t, TOP - 50) == TOP && yt_vmap_clip(&t, BOT + 50) == BOT - 1,
          "clipped to [%d, %d)", TOP, BOT);
    {
        int out = 0;
        for (int v = 0; v < 256; v++) { int y = y_of(&t, v); if (y < TOP || y >= BOT) out++; }
        CHECK(out == 0, "every code lands on the plot");
    }
    yt_vmap_t t2;
    yt_vmap_true(&t2, TOP, BOT, 1);
    {
        int diff = 0;
        for (int v = 0; v < 256; v++) diff += (y_of(&t, v) != y_of(&t2, v));
        CHECK(diff == 0 && t.fx_ch == 0 && t2.fx_ch == 1,
              "both channels share the full plot and the same rows (overlay)");
    }

    printf("[2] the former fixed renderer and the roll view agree\n");
    {
        int d_old = 0, d_roll = 0;
        scope_roll_vmap_t m = { true, 0, 255, 128, TOP, BOT, H };
        for (int v = 0; v < 256; v++) {
            if (y_of(&t, v) != old_fixed_y(TOP, BOT, v)) d_old++;
            if (y_of(&t, v) != scope_roll_y(&m, (uint8_t)v)) d_roll++;
        }
        CHECK(d_old == 0, "== draw_channel_fixed() for all 256 codes (%d differ)", d_old);
        CHECK(d_roll == 0, "== scope_roll_y(fixed, full plot) for all 256 codes (%d differ)", d_roll);
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

    printf("[5] cursor scale\n");
    CHECK(fabsf(yt_vmap_counts_per_px(&t) - 256.0f / (float)H) < 1e-6f,
          "true scale: 256/SCOPE_H counts per row");
    {
        yt_vmap_t a;
        yt_vmap_autofit(&a, 40, 200, TOP, BOT);
        yt_vmap_t off = { .on = false, .fx_ch = -1, .span = 8 };
        CHECK(fabsf(yt_vmap_counts_per_px(&a) - 160.0f / (float)(H - 3)) < 1e-6f &&
              yt_vmap_counts_per_px(&off) == 0.0f, "autofit: span/h; nothing drawn: 0");
    }

    if (fails) { printf("test_yt_vmap: %d FAILED\n", fails); return 1; }
    printf("test_yt_vmap: all checks passed\n");
    return 0;
}
