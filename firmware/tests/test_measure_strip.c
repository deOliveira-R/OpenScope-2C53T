/*
 * Measurement strip + plot markers host test (UI round 1, R1-C).
 *
 *   make -C firmware test-measure-strip
 *
 * Links the SHIPPED measure_strip.c, plot_markers.c, the real value formatter
 * (scope_prm_format -> scope_cursor_format) and the real font_small metrics,
 * so "fits" means: the string the firmware would print, in the font it would
 * print it in, fits the box the firmware would draw it in.
 *
 *  1. No cell overflows: every one of stock's 12 labels with the widest value
 *     its formatter can produce over the instrument's domain (swept, both
 *     signs, every unit) fits label + MSTRIP_GAP + value inside a cell.
 *  2. Layout: cells on the glass, inside [top, bottom), never overlapping each
 *     other or the AUX row; CH1 first then CH2, stock order; per-channel cap;
 *     top == measure_strip_top(); hidden/disabled place nothing.
 *  3. Markers: shapes inside their box, knock-outs only inside the fill, the
 *     three labels distinct, placement always inside the band.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* LCD stubs: font.c is linked for font_string_width only. */
void lcd_set_pixel(uint16_t x, uint16_t y, uint16_t c) { (void)x; (void)y; (void)c; }
void lcd_set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h) { (void)x; (void)y; (void)w; (void)h; }
void lcd_write_data(uint16_t d) { (void)d; }
void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t c) { (void)x; (void)y; (void)w; (void)h; (void)c; }

#include "font.h"
#include "scope_measure.h"
#include "measure_strip.h"
#include "plot_markers.h"

static int run, failed;
#define CHECK(cond, ...) do { run++; if (!(cond)) { failed++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define LCD_W 320u
#define BOTTOM 224u    /* SCOPE_BOT: top of the info bar */

/* ── 1. fit ──────────────────────────────────────────────────────────── */

/* The instrument's domain per unit, generously: */
typedef struct { scope_prm_unit_t u; float lo, hi; const char *name; } dom_t;
static const dom_t k_dom[] = {
    { SCOPE_PRM_UNIT_HZ,      0.0f,     999.99e6f, "Hz"  },  /* above 2x the 250 MS/s merge */
    { SCOPE_PRM_UNIT_SECONDS, -999.99f, 999.99f,   "s"   },  /* Per of a 1 mHz signal */
    { SCOPE_PRM_UNIT_SAMPLES, 0.0f,     99999.0f,  "smp" },  /* records are 1024 */
    { SCOPE_PRM_UNIT_PERCENT, 0.0f,     100.0f,    "%"   },
    { SCOPE_PRM_UNIT_VOLTS,   -999.99f, 999.99f,   "V"   },  /* 400 V CAT with an X10 probe */
    { SCOPE_PRM_UNIT_COUNTS,  -999.0f,  999.0f,    "cnt" },  /* codes 0..255, spans +-255 */
};

static unsigned value_w(scope_prm_unit_t u, float v, char *s, size_t n)
{
    scope_prm_t p; p.unit = u; p.value = v;
    scope_prm_format(&p, s, (uint32_t)n);
    return font_string_width(s, &font_small);
}

/* Widest formatted value over [lo, hi]: a log sweep of magnitudes (with the
 * rounding edges 9.995, 99.995, ... that carry into an extra digit) on both
 * sides of zero, clipped to the domain. */
static unsigned widest(const dom_t *d, char *worst, size_t n)
{
    unsigned best = 0;
    char s[24];
    for (int dec = -12; dec <= 9; dec++) {
        static const float mant[] = { 1.0f, 1.2345f, 4.4444f, 8.8888f, 9.9949f,
                                      9.995f, 9.9999f };
        for (unsigned k = 0; k < sizeof(mant) / sizeof(mant[0]); k++) {
            for (int sg = -1; sg <= 1; sg += 2) {
                float v = (float)sg * mant[k] * powf(10.0f, (float)dec);
                if (v < d->lo || v > d->hi) continue;
                unsigned w = value_w(d->u, v, s, sizeof(s));
                if (w > best) { best = w; snprintf(worst, n, "%s", s); }
            }
        }
    }
    float ends[2] = { d->lo, d->hi };
    for (int e = 0; e < 2; e++) {
        unsigned w = value_w(d->u, ends[e], s, sizeof(s));
        if (w > best) { best = w; snprintf(worst, n, "%s", s); }
    }
    return best;
}

static void test_fit(void)
{
    const unsigned inner = MSTRIP_CELL_W - 2u * MSTRIP_PAD;
    unsigned lw_max = 0, vw_max = 0;
    char vworst[24] = "", lworst[16] = "";
    for (unsigned i = 0; i < SCOPE_PRM_COUNT; i++) {
        unsigned lw = font_string_width(scope_prm_label(i), &font_small);
        if (lw > lw_max) { lw_max = lw; snprintf(lworst, sizeof(lworst), "%s", scope_prm_label(i)); }
    }
    for (unsigned d = 0; d < sizeof(k_dom) / sizeof(k_dom[0]); d++) {
        char w[24];
        unsigned vw = widest(&k_dom[d], w, sizeof(w));
        printf("  widest %-4s value: %-12s %3u px\n", k_dom[d].name, w, vw);
        if (vw > vw_max) { vw_max = vw; snprintf(vworst, sizeof(vworst), "%s", w); }
    }
    char na[8];
    scope_prm_t none = { SCOPE_PRM_UNIT_NONE, 0.0f };
    scope_prm_format(&none, na, sizeof(na));
    unsigned naw = font_string_width(na, &font_small);
    if (naw > vw_max) vw_max = naw;

    printf("  widest label %s %u px (column %u), widest value %s %u px (column %u)\n",
           lworst, lw_max, MSTRIP_LABEL_W, vworst, vw_max, MSTRIP_VALUE_W);
    CHECK(lw_max <= MSTRIP_LABEL_W, "label %s wider than the label column", lworst);
    CHECK(vw_max <= MSTRIP_VALUE_W, "value %s wider than the value column", vworst);
    CHECK(MSTRIP_PAD + MSTRIP_LABEL_W + MSTRIP_GAP + MSTRIP_VALUE_W + MSTRIP_PAD <= MSTRIP_CELL_W, "columns overflow the cell");

    /* Every label with the widest value, through the placement the firmware
     * uses: it must report "both fit" and keep both inside the cell. */
    for (unsigned c = 0; c < MSTRIP_COLS; c++) {
        const uint16_t x = (uint16_t)(MSTRIP_X0 + c * MSTRIP_CELL_W);
        for (unsigned i = 0; i < SCOPE_PRM_COUNT; i++) {
            unsigned lw = font_string_width(scope_prm_label(i), &font_small);
            uint16_t lx, vx;
            bool ok = measure_strip_place(x, (uint16_t)lw, (uint16_t)vw_max, &lx, &vx);
            CHECK(ok, "%s + %s does not fit a %u px cell", scope_prm_label(i), vworst, MSTRIP_CELL_W);
            CHECK(lx >= x && lx + lw + MSTRIP_GAP <= vx,
                  "label %s overlaps the value (lx %u w %u vx %u)", scope_prm_label(i), lx, lw, vx);
            CHECK(vx + vw_max <= x + MSTRIP_CELL_W - MSTRIP_PAD, "value leaves the cell");
            CHECK(vx + vw_max <= LCD_W, "value leaves the glass");
        }
    }
    /* The boundary: a full label column and a full value column fit, one
     * pixel more in either does not; the gap is between them. */
    {
        uint16_t lx, vx;
        CHECK(measure_strip_place(MSTRIP_X0, MSTRIP_LABEL_W, MSTRIP_VALUE_W, &lx, &vx), "full columns rejected");
        CHECK(lx + MSTRIP_LABEL_W + MSTRIP_GAP == vx, "label-to-value gap %d", (int)vx - (int)(lx + MSTRIP_LABEL_W));
        CHECK(!measure_strip_place(MSTRIP_X0, MSTRIP_LABEL_W, MSTRIP_VALUE_W + 1u, NULL, NULL), "value column overflow accepted");
        CHECK(!measure_strip_place(MSTRIP_X0, MSTRIP_LABEL_W + 1u, 10, NULL, NULL), "label column overflow accepted");
    }
    (void)inner;
    /* A value too wide for its cell is still placed inside it (value only). */
    {
        uint16_t lx, vx;
        bool ok = measure_strip_place(MSTRIP_X0, 33, 90, &lx, &vx);
        CHECK(!ok, "33 + 90 px reported as fitting");
        CHECK(vx >= MSTRIP_X0 && vx + 90 <= MSTRIP_X0 + MSTRIP_CELL_W, "fallback value outside the cell");
    }
}

/* ── 2. layout ───────────────────────────────────────────────────────── */

static unsigned pop(uint16_t m) { unsigned c = 0; for (unsigned i = 0; i < 12; i++) c += (m >> i) & 1u; return c; }

static void check_layout(uint16_t m1, bool e1, uint16_t m2, bool e2, bool hid, bool aux)
{
    mstrip_layout_t L;
    memset(&L, 0xA5, sizeof(L));
    measure_strip_layout(m1, e1, m2, e2, hid, aux, BOTTOM, &L);

    unsigned n1 = (e1 && !hid) ? (pop(m1) > MSTRIP_MAX_PER_CH ? MSTRIP_MAX_PER_CH : pop(m1)) : 0;
    unsigned n2 = (e2 && !hid) ? (pop(m2) > MSTRIP_MAX_PER_CH ? MSTRIP_MAX_PER_CH : pop(m2)) : 0;
    unsigned want_drop = (e1 && !hid ? pop(m1) - n1 : 0) + (e2 && !hid ? pop(m2) - n2 : 0);
    CHECK(L.n == n1 + n2, "n %u != %u (m1 %03x m2 %03x)", L.n, n1 + n2, m1, m2);
    CHECK(L.dropped == want_drop, "dropped %u != %u", L.dropped, want_drop);
    CHECK(L.rows <= MSTRIP_MAX_ROWS, "rows %u", L.rows);
    CHECK(L.rows == (n1 + 2u) / 3u + (n2 + 2u) / 3u, "rows %u for %u + %u cells", L.rows, n1, n2);
    /* One block per channel: each starts at the first column of a new row,
     * and inside a block the cells flow left to right, top to bottom. */
    {
        const unsigned r1 = (n1 + 2u) / 3u, first = L.top + (aux ? MSTRIP_ROW_H : 0u);
        unsigned k[2] = { 0, 0 };
        for (unsigned i = 0; i < L.n; i++) {
            const unsigned ch = L.cell[i].ch, j = k[ch]++;
            const unsigned row = (ch ? r1 : 0u) + j / 3u;
            CHECK(L.cell[i].x == MSTRIP_X0 + (j % 3u) * MSTRIP_CELL_W, "cell %u x", i);
            CHECK(L.cell[i].y == first + row * MSTRIP_ROW_H, "cell %u y", i);
        }
        CHECK(k[0] == n1 && k[1] == n2, "cells per channel");
    }
    CHECK(L.top == measure_strip_top(m1, e1, m2, e2, hid, aux, BOTTOM), "top disagrees");
    CHECK(L.top == BOTTOM - (L.rows + (aux ? 1u : 0u)) * MSTRIP_ROW_H, "top %u", L.top);
    CHECK(L.aux == aux, "aux flag");
    if (aux)
        CHECK(L.aux_y == L.top && L.aux_y + MSTRIP_ROW_H <= BOTTOM, "aux row placement");

    /* Which ones show: the priority pick, per channel, independent of the
     * code's table walk -- no dropped one outranks a shown one. */
    for (unsigned ch = 0; ch < 2u; ch++) {
        const uint16_t m = ch ? m2 : m1;
        if (!(ch ? e2 : e1) || hid) continue;
        uint16_t got = 0;
        for (unsigned k = 0; k < L.n; k++)
            if (L.cell[k].ch == ch) got |= (uint16_t)(1u << L.cell[k].prm);
        int worst_shown = -1, best_dropped = 99;
        for (unsigned r = 0; r < 12u; r++) {
            const unsigned p = mstrip_show_priority[r];
            if (got & (1u << p)) worst_shown = (int)r;
            else if ((m & (1u << p)) && best_dropped == 99) best_dropped = (int)r;
        }
        CHECK(worst_shown < best_dropped, "ch%u mask %03x: a dropped prm outranks a shown one", ch, m);
        CHECK(got == measure_strip_shown(m), "ch%u mask %03x: cells %03x != shown %03x", ch, m, got,
              measure_strip_shown(m));
    }

    int prev_ch = -1, prev_prm = -1;
    for (unsigned k = 0; k < L.n; k++) {
        const mstrip_cell_t *c = &L.cell[k];
        CHECK(c->x + MSTRIP_CELL_W <= LCD_W, "cell %u off the glass right", k);
        CHECK(c->y >= L.top && c->y + MSTRIP_ROW_H <= BOTTOM, "cell %u y %u outside [%u,%u)", k, c->y, L.top, BOTTOM);
        if (aux)
            CHECK(c->y >= L.aux_y + MSTRIP_ROW_H, "cell %u overlaps the AUX row", k);
        CHECK(c->ch == 0 || c->ch == 1, "ch");
        CHECK(((c->ch ? m2 : m1) >> c->prm) & 1u, "cell %u shows an unselected prm %u", k, c->prm);
        /* order: CH1 then CH2, each in stock order */
        CHECK((int)c->ch > prev_ch || ((int)c->ch == prev_ch && (int)c->prm > prev_prm),
              "cell %u out of order", k);
        prev_ch = c->ch; prev_prm = c->prm;
        for (unsigned j = 0; j < k; j++) {
            const mstrip_cell_t *o = &L.cell[j];
            bool sep = o->x + MSTRIP_CELL_W <= c->x || c->x + MSTRIP_CELL_W <= o->x ||
                       o->y + MSTRIP_ROW_H <= c->y || c->y + MSTRIP_ROW_H <= o->y;
            CHECK(sep, "cells %u and %u overlap", j, k);
        }
    }
}

static void test_layout(void)
{
    static const uint16_t m2s[] = { 0x000, 0x001, 0x100, 0x500, 0x03F, 0xFC0, 0xFFF, 0x555, 0xAAA, 0x111 };
    for (unsigned m1 = 0; m1 < 4096u; m1++)
        for (unsigned j = 0; j < sizeof(m2s) / sizeof(m2s[0]); j++)
            for (unsigned f = 0; f < 16u; f++)
                check_layout((uint16_t)m1, f & 1u, m2s[j], (f >> 1) & 1u, (f >> 2) & 1u, (f >> 3) & 1u);

    /* CH1's default (Freq Peri Duty+ Vp-p Vrms) with CH2 on Vp-p alone: CH1
     * takes two rows, CH2's Vp-p starts its own block at the first column of
     * row 3 (stock runs it on after CH1's last cell; ours starts a block). */
    mstrip_layout_t L;
    measure_strip_layout(SCOPE_PRM_DEFAULT_MASK, true, 1u << SCOPE_PRM_VPP, true, false, false, BOTTOM, &L);
    CHECK(L.n == 6 && L.rows == 3, "default: %u cells %u rows", L.n, L.rows);
    CHECK(L.cell[5].ch == 1 && L.cell[5].prm == SCOPE_PRM_VPP &&
          L.cell[5].x == MSTRIP_X0 && L.cell[5].y == BOTTOM - MSTRIP_ROW_H,
          "default: CH2 Vp-p not at the start of its block");
    CHECK(L.cell[0].prm == SCOPE_PRM_FREQ && L.cell[0].x == MSTRIP_X0 && L.cell[0].y == BOTTOM - 3u * MSTRIP_ROW_H,
          "default: Freq not first");
    /* The same selection on both channels: the same measurement in the same
     * column, CH2's block right under CH1's. */
    measure_strip_layout(SCOPE_PRM_DEFAULT_MASK, true, SCOPE_PRM_DEFAULT_MASK, true, false, false, BOTTOM, &L);
    CHECK(L.n == 10 && L.rows == 4, "both default: %u cells %u rows", L.n, L.rows);
    for (unsigned i = 0; i < 5u; i++)
        CHECK(L.cell[i].prm == L.cell[i + 5u].prm && L.cell[i].x == L.cell[i + 5u].x &&
              L.cell[i + 5u].y == L.cell[i].y + 2u * MSTRIP_ROW_H, "both default: cell %u not aligned", i);

    /* Hidden: nothing placed, the band runs to the bottom (or the AUX row). */
    CHECK(measure_strip_top(0xFFF, true, 0xFFF, true, true, false, BOTTOM) == BOTTOM, "hidden top");
    CHECK(measure_strip_top(0xFFF, true, 0xFFF, true, true, true, BOTTOM) == BOTTOM - MSTRIP_ROW_H, "hidden aux top");
    /* Everything on: capped at 6 + 6 = 4 rows. */
    measure_strip_layout(0xFFF, true, 0xFFF, true, false, true, BOTTOM, &L);
    CHECK(L.rows == 4 && L.n == 12 && L.dropped == 12, "all-on: rows %u n %u dropped %u", L.rows, L.n, L.dropped);
    /* ... and the six are the owner's priority (levels first), drawn in
     * stock order: Freq Duty+ Vmax Vmin Vp-p Vrms. */
    {
        static const uint8_t want[6] = { SCOPE_PRM_FREQ, SCOPE_PRM_DUTY_POS, SCOPE_PRM_VMAX,
                                         SCOPE_PRM_VMIN, SCOPE_PRM_VPP, SCOPE_PRM_VRMS };
        int ok = 1;
        for (unsigned k = 0; k < 6; k++)
            ok &= L.cell[k].ch == 0 && L.cell[k].prm == want[k] && L.cell[k + 6].ch == 1 &&
                  L.cell[k + 6].prm == want[k];
        CHECK(ok, "all-on: not Freq Duty+ Vmax Vmin Vp-p Vrms on both channels");
    }
    /* 7 on, Vp-p not among them: the 7th in priority (Peri, stock order
     * after the six) fills the slot and Avg (last) is dropped. */
    {
        const uint16_t m = (uint16_t)((1u << SCOPE_PRM_VMAX) | (1u << SCOPE_PRM_VMIN) | (1u << SCOPE_PRM_FREQ) |
                                      (1u << SCOPE_PRM_DUTY_POS) | (1u << SCOPE_PRM_VRMS) |
                                      (1u << SCOPE_PRM_PERI) | (1u << SCOPE_PRM_AVG));
        CHECK(measure_strip_shown(m) == (uint16_t)(m & ~(1u << SCOPE_PRM_AVG)), "7 on: shown %03x",
              measure_strip_shown(m));
        CHECK(measure_strip_shown(0x0FFu) == 0x0FFu - (1u << SCOPE_PRM_WID_POS) - (1u << SCOPE_PRM_WID_NEG),
              "8 on (ids 0-7): shown %03x", measure_strip_shown(0x0FFu));
        CHECK(measure_strip_shown(SCOPE_PRM_DEFAULT_MASK) == SCOPE_PRM_DEFAULT_MASK, "5 on: not all shown");
    }
    /* A channel that is off contributes nothing, even selected. */
    measure_strip_layout(0x001, false, 0x001, true, false, false, BOTTOM, &L);
    CHECK(L.n == 1 && L.cell[0].ch == 1, "disabled CH1 still placed");
}

/* ── 3. dim colour ───────────────────────────────────────────────────── */

static void test_dim(void)
{
    CHECK(measure_strip_dim(0xFFE0, 0x0000) == 0x7BE0, "dim yellow on black = %04x", measure_strip_dim(0xFFE0, 0));
    CHECK(measure_strip_dim(0x07FF, 0x07FF) == 0x07FF, "dim(x, x) != x");
    CHECK(measure_strip_dim(0x0000, 0xFFFF) == 0x7BEF, "dim black on white = %04x", measure_strip_dim(0, 0xFFFF));
}

/* ── 4. markers ──────────────────────────────────────────────────────── */

static void test_markers(void)
{
    static const pm_dir_t dirs[] = { PM_POINT_RIGHT, PM_POINT_LEFT, PM_POINT_UP, PM_POINT_DOWN };
    for (unsigned d = 0; d < 4; d++) {
        for (unsigned lab = 0; lab < 4; lab++) {
            unsigned fill = 0, knock = 0;
            for (int y = -2; y < PM_TAG_H + 2; y++)
                for (int x = -2; x < PM_TAG_W + 2; x++) {
                    pm_px_t p = plot_marker_px(dirs[d], (pm_label_t)lab, x, y);
                    bool in = x >= 0 && y >= 0 && x < PM_TAG_W && y < PM_TAG_H;
                    CHECK(in || p == PM_PX_NONE, "dir %u px outside the box at %d,%d", d, x, y);
                    if (p == PM_PX_FILL) fill++;
                    if (p == PM_PX_KNOCK) knock++;
                    /* a knock-out is surrounded by the tag: its 4 neighbours are tag pixels */
                    if (p == PM_PX_KNOCK) {
                        CHECK(plot_marker_px(dirs[d], (pm_label_t)lab, x - 1, y) != PM_PX_NONE &&
                              plot_marker_px(dirs[d], (pm_label_t)lab, x + 1, y) != PM_PX_NONE &&
                              plot_marker_px(dirs[d], (pm_label_t)lab, x, y - 1) != PM_PX_NONE &&
                              plot_marker_px(dirs[d], (pm_label_t)lab, x, y + 1) != PM_PX_NONE,
                              "dir %u label %u knock-out at the edge %d,%d", d, lab, x, y);
                    }
                }
            /* body 5x9 + point 4+... = 45 + 16 (rows 1..7 of the point) */
            CHECK(fill + knock == 61, "dir %u label %u: %u tag pixels, want 61", d, lab, fill + knock);
            if (d < 2) CHECK((lab == 0) == (knock == 0), "dir %u label %u: knock %u", d, lab, knock);
            else       CHECK(knock == 0, "vertical tags carry no label");
        }
        /* the point: the tip pixel only, at the middle of the far edge */
        unsigned tip = 0;
        for (int i = 0; i < 9; i++) {
            int x = (dirs[d] == PM_POINT_RIGHT) ? 8 : (dirs[d] == PM_POINT_LEFT) ? 0 : i;
            int y = (dirs[d] == PM_POINT_UP) ? 0 : (dirs[d] == PM_POINT_DOWN) ? 8 : i;
            if (plot_marker_px(dirs[d], PM_LABEL_NONE, x, y) != PM_PX_NONE) { tip++;
                CHECK(i == 4, "dir %u tip at %d", d, i); }
        }
        CHECK(tip == 1, "dir %u: %u tip pixels", d, tip);
    }
    /* LEFT is the mirror of RIGHT, DOWN of UP */
    for (int y = 0; y < 9; y++)
        for (int x = 0; x < 9; x++) {
            /* shapes only: the labels are deliberately NOT mirrored */
            CHECK((plot_marker_px(PM_POINT_LEFT, PM_LABEL_NONE, x, y) != PM_PX_NONE) ==
                  (plot_marker_px(PM_POINT_RIGHT, PM_LABEL_NONE, 8 - x, y) != PM_PX_NONE), "LEFT not a mirror");
            CHECK((plot_marker_px(PM_POINT_DOWN, PM_LABEL_NONE, x, y) != PM_PX_NONE) ==
                  (plot_marker_px(PM_POINT_UP, PM_LABEL_NONE, x, 8 - y) != PM_PX_NONE), "DOWN not a mirror");
        }
    /* the three labels differ, and "1"/"T" read the right way round on a LEFT tag too */
    unsigned diff12 = 0, diff1t = 0, diff2t = 0;
    for (int y = 0; y < 9; y++)
        for (int x = 0; x < 9; x++) {
            pm_px_t a = plot_marker_px(PM_POINT_RIGHT, PM_LABEL_1, x, y);
            pm_px_t b = plot_marker_px(PM_POINT_RIGHT, PM_LABEL_2, x, y);
            pm_px_t t = plot_marker_px(PM_POINT_RIGHT, PM_LABEL_T, x, y);
            diff12 += a != b; diff1t += a != t; diff2t += b != t;
        }
    CHECK(diff12 && diff1t && diff2t, "labels not distinct");
    /* "T" on a LEFT tag: the top bar is 3 knock-outs in row 2, the stem in col 2 of the glyph */
    {
        unsigned bar = 0;
        for (int x = 0; x < 9; x++) bar += plot_marker_px(PM_POINT_LEFT, PM_LABEL_T, x, 2) == PM_PX_KNOCK;
        CHECK(bar == 3, "T bar %u", bar);
        CHECK(plot_marker_px(PM_POINT_LEFT, PM_LABEL_T, 6, 4) == PM_PX_KNOCK, "T stem on LEFT tag (glyph mirrored?)");
        /* '1' row 1 is "##.": its flag points left on either tag */
        CHECK(plot_marker_px(PM_POINT_LEFT, PM_LABEL_1, 5, 3) == PM_PX_KNOCK &&
              plot_marker_px(PM_POINT_LEFT, PM_LABEL_1, 7, 3) == PM_PX_FILL, "'1' mirrored on a LEFT tag");
        CHECK(plot_marker_px(PM_POINT_RIGHT, PM_LABEL_1, 1, 3) == PM_PX_KNOCK &&
              plot_marker_px(PM_POINT_RIGHT, PM_LABEL_1, 3, 3) == PM_PX_FILL, "'1' wrong on a RIGHT tag");
    }

    /* trigger-position pointer: 9+7+5+3+1 */
    unsigned tp = 0;
    for (int y = -1; y <= PM_TPOS_H; y++)
        for (int x = -1; x <= PM_TPOS_W; x++)
            tp += plot_marker_tpos_px(x, y) == PM_PX_FILL;
    CHECK(tp == 25, "tpos pixels %u", tp);
    CHECK(plot_marker_tpos_px(4, 4) == PM_PX_FILL && plot_marker_tpos_px(3, 4) == PM_PX_NONE, "tpos tip");

    /* placement: always inside the band; direction says where the level is */
    for (int top = 18; top < 40; top += 7)
        for (int bot = top + 9; bot < 230; bot += 13)
            for (int row = -50; row < 300; row++) {
                int y0 = -999;
                pm_dir_t d = plot_marker_place(row, top, bot, PM_POINT_RIGHT, &y0);
                CHECK(y0 >= top && y0 + PM_TAG_H <= bot, "tag outside band row %d [%d,%d) y0 %d", row, top, bot, y0);
                pm_dir_t want = row < top ? PM_POINT_UP : row >= bot ? PM_POINT_DOWN : PM_POINT_RIGHT;
                CHECK(d == want, "dir for row %d in [%d,%d)", row, top, bot);
                if (want == PM_POINT_RIGHT && row - 4 >= top && row + 4 < bot)
                    CHECK(y0 + 4 == row, "tag not centred on row %d", row);
            }
}

int main(void)
{
    printf("test_measure_strip\n");
    test_fit();
    test_layout();
    test_dim();
    test_markers();
    printf("%d checks, %d failed\n", run, failed);
    return failed ? 1 : 0;
}
