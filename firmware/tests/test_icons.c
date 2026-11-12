/*
 * Host test: src/ui/icons.c (glyph bounds, no overflow, shapes) and the bar
 * geometry in src/ui/status_bar.h (slots inside 320 px, no overlap, and the
 * widest strings each slot prints fit it in font_small).
 *
 *   make test-icons
 */
#include "icons.h"
#include "status_bar.h"
#include "font.h"
#include "siggen_wave.h"
#include <stdio.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define CANARY 0xA5A5u
#define FG     0x1111u
#define BG     0x2222u
#define FILL   0x3333u

/* A line with canaries on both sides of [0, n). */
typedef struct { uint16_t pre[4]; uint16_t px[ICON_MAX_W]; uint16_t post[4]; } guarded_t;

static void guard_init(guarded_t *g)
{
    for (int i = 0; i < 4; i++) g->pre[i] = g->post[i] = CANARY;
    for (unsigned i = 0; i < ICON_MAX_W; i++) g->px[i] = CANARY;
}
static bool guard_ok(const guarded_t *g, unsigned n)
{
    for (int i = 0; i < 4; i++)
        if (g->pre[i] != CANARY || g->post[i] != CANARY) return false;
    for (unsigned i = n; i < ICON_MAX_W; i++)
        if (g->px[i] != CANARY) return false;
    return true;
}

static unsigned set_count(const icon_bitmap_t *ic)
{
    unsigned n = 0;
    for (uint8_t y = 0; y < ic->h; y++)
        for (uint8_t x = 0; x < ic->w; x++)
            n += icon_px(ic, x, y);
    return n;
}

static void test_bitmaps(void)
{
    for (int id = 0; id < ICON_COUNT; id++) {
        const icon_bitmap_t *ic = icon_bitmap((icon_id_t)id);
        CHECK(ic != NULL, "icon %d is NULL", id);
        if (!ic) continue;
        CHECK(ic->w >= 1 && ic->w <= 16, "icon %d width %u", id, ic->w);
        CHECK(ic->h >= 1 && ic->h <= ICON_MAX_H, "icon %d height %u", id, ic->h);
        CHECK(ic->h <= SB_H - 2u, "icon %d (h %u) does not fit a bar with a 1 px margin", id, ic->h);
        /* No bits past the declared width: a table drawn wider than its box
         * would paint over the next slot. */
        const uint16_t spill = (uint16_t)(0xFFFFu >> ic->w);
        for (uint8_t y = 0; y < ic->h; y++)
            CHECK((ic->rows[y] & spill) == 0u, "icon %d row %u has bits past w=%u", id, y, ic->w);
        CHECK(set_count(ic) > 0u, "icon %d is empty", id);
        CHECK(!icon_px(ic, ic->w, 0) && !icon_px(ic, 0, ic->h), "icon %d answers outside its box", id);
        /* icon_row writes exactly w pixels; past the last row, nothing. */
        for (uint8_t y = 0; y <= ic->h; y++) {
            guarded_t g; guard_init(&g);
            icon_row(ic, y, FG, BG, g.px);
            if (y == ic->h) {
                CHECK(g.px[0] == CANARY, "icon %d row %u (past h) was written", id, y);
            } else {
                CHECK(guard_ok(&g, ic->w), "icon %d row %u overflows its %u px", id, y, ic->w);
                for (uint8_t x = 0; x < ic->w; x++)
                    CHECK(g.px[x] == (icon_px(ic, x, y) ? FG : BG), "icon %d (%u,%u)", id, x, y);
            }
        }
        /* The overlay clips at the line's width. */
        guarded_t g; guard_init(&g);
        for (unsigned i = 0; i < 5; i++) g.px[i] = BG;
        for (uint8_t y = 0; y < ic->h; y++)
            icon_row_over(ic, y, 3, FG, g.px, 5);
        CHECK(guard_ok(&g, 5), "icon %d overlay wrote past line_w", id);
    }
    CHECK(icon_bitmap(ICON_COUNT) == NULL, "id past the table is not NULL");
    CHECK(icon_bitmap((icon_id_t)-1) == NULL, "negative id is not NULL");
    CHECK(!icon_px(NULL, 0, 0), "NULL icon answers");
    guarded_t g; guard_init(&g);
    icon_row(NULL, 0, FG, BG, g.px);
    CHECK(g.px[0] == CANARY, "NULL icon wrote a row");
}

static void test_shapes(void)
{
    const icon_bitmap_t *r = icon_bitmap(ICON_EDGE_RISE), *f = icon_bitmap(ICON_EDGE_FALL);
    /* Rising: low on the left, high on the right; falling the other way. */
    CHECK(icon_px(r, 0, 10) && !icon_px(r, 0, 1) && icon_px(r, 8, 1) && !icon_px(r, 8, 10),
          "rising edge is not low-left / high-right");
    CHECK(icon_px(f, 0, 1) && !icon_px(f, 0, 10) && icon_px(f, 8, 10) && !icon_px(f, 8, 1),
          "falling edge is not high-left / low-right");
    unsigned run = 0;
    for (uint8_t y = 1; y <= 10; y++) run += icon_px(r, 4, y);
    CHECK(run == 10, "rising edge step is not a full vertical run (%u)", run);
    /* Arrowheads point the way: the narrow row is nearer the far level. */
    unsigned rw4 = 0, rw5 = 0, fw6 = 0, fw7 = 0;
    for (uint8_t x = 0; x < 9; x++) { rw4 += icon_px(r, x, 4); rw5 += icon_px(r, x, 5);
                                      fw6 += icon_px(f, x, 6); fw7 += icon_px(f, x, 7); }
    CHECK(rw4 < rw5, "rising arrowhead does not point up");
    CHECK(fw7 < fw6, "falling arrowhead does not point down");
    /* Mirror pairs. */
    const icon_bitmap_t *al = icon_bitmap(ICON_ARROW_LEFT), *ar = icon_bitmap(ICON_ARROW_RIGHT);
    const icon_bitmap_t *ad = icon_bitmap(ICON_ARROW_DOWN), *au = icon_bitmap(ICON_ARROW_UP);
    bool mir = true;
    for (uint8_t y = 0; y < al->h; y++)
        for (uint8_t x = 0; x < al->w; x++)
            mir &= icon_px(al, x, y) == icon_px(ar, (uint8_t)(al->w - 1u - x), y);
    CHECK(mir, "left/right arrows are not mirror images");
    mir = true;
    for (uint8_t y = 0; y < ad->h; y++)
        for (uint8_t x = 0; x < ad->w; x++)
            mir &= icon_px(ad, x, y) == icon_px(au, x, (uint8_t)(ad->h - 1u - y));
    CHECK(mir, "down/up arrows are not mirror images");
    CHECK(icon_px(al, 0, 3) && icon_px(ar, 6, 3), "arrow tips are not at the pointing side");
    /* The T sits inside the disc. */
    const icon_bitmap_t *d = icon_bitmap(ICON_TRIG_DISC), *t = icon_bitmap(ICON_TRIG_T);
    bool inside = t->w == d->w && t->h == d->h;
    for (uint8_t y = 0; y < t->h; y++)
        for (uint8_t x = 0; x < t->w; x++)
            if (icon_px(t, x, y) && !icon_px(d, x, y)) inside = false;
    CHECK(inside, "the T leaves the disc");
}

static void test_battery(void)
{
    static const struct { uint8_t pct, bars; } tab[] = {
        {0,0},{1,1},{25,1},{26,2},{50,2},{51,3},{75,3},{76,4},{100,4},{255,4} };
    for (unsigned i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
        CHECK(icon_battery_bars(tab[i].pct) == tab[i].bars, "battery %u%% -> %u bars, want %u",
              tab[i].pct, icon_battery_bars(tab[i].pct), tab[i].bars);
    CHECK(ICON_BATT_W <= ICON_MAX_W && ICON_BATT_H <= SB_H - 2u, "battery glyph too big");

    for (uint8_t bars = 0; bars <= ICON_BATT_BARS + 1u; bars++) {
        for (int chg = 0; chg <= 1; chg++) {
            unsigned fills = 0, outline = 0;
            for (uint8_t row = 0; row <= ICON_BATT_H; row++) {
                guarded_t g; guard_init(&g);
                icon_battery_row(row, bars, chg, FG, FILL, BG, g.px);
                if (row == ICON_BATT_H) {
                    CHECK(g.px[0] == CANARY, "battery row past h written");
                    continue;
                }
                CHECK(guard_ok(&g, ICON_BATT_W), "battery row %u overflows", row);
                for (unsigned x = 0; x < ICON_BATT_W; x++) {
                    fills   += g.px[x] == FILL;
                    outline += g.px[x] == FG;
                }
                CHECK(g.px[0] == FG, "battery row %u has no left outline", row);
            }
            const uint8_t b = bars > ICON_BATT_BARS ? ICON_BATT_BARS : bars;
            const unsigned want = chg ? set_count(icon_bitmap(ICON_BOLT)) : (unsigned)b * 3u * 6u;
            CHECK(fills == want, "battery bars=%u chg=%d: %u fill px, want %u", bars, chg, fills, want);
            CHECK(outline == 2u * 20u + 2u * 8u + 2u * 4u, "battery outline %u px", outline);
        }
    }
}

static void test_wave(void)
{
    for (unsigned w = 0; w <= SIGGEN_WAVEFORM_COUNT; w++) {
        uint8_t y[ICON_WAVE_W + 1];
        y[ICON_WAVE_W] = 0xEE;
        icon_wave_points((uint8_t)w, 50u, y);
        CHECK(y[ICON_WAVE_W] == 0xEE, "wave %u wrote past 16 points", w);
        uint8_t lo = 0xFF, hi = 0;
        for (unsigned c = 0; c < ICON_WAVE_W; c++) {
            CHECK(y[c] < ICON_WAVE_H, "wave %u col %u y=%u off the icon", w, c, y[c]);
            if (y[c] < lo) lo = y[c];
            if (y[c] > hi) hi = y[c];
        }
        if (w == SIGGEN_DC || w == SIGGEN_WAVEFORM_COUNT) {
            CHECK(lo == hi && lo == ICON_WAVE_H / 2u, "flat wave %u not on the middle row", w);
        } else {
            CHECK(lo == 0u && hi == ICON_WAVE_H - 1u, "wave %u does not span the icon (%u..%u)", w, lo, hi);
        }
        for (uint8_t row = 0; row <= ICON_WAVE_H; row++) {
            guarded_t g; guard_init(&g);
            icon_wave_row(y, row, FG, BG, g.px);
            if (row == ICON_WAVE_H) { CHECK(g.px[0] == CANARY, "wave row past h"); continue; }
            CHECK(guard_ok(&g, ICON_WAVE_W), "wave %u row %u overflows", w, row);
        }
        /* Connected: every column has at least one pixel, and the trace is
         * continuous (each column's span touches the previous point). */
        for (unsigned c = 0; c < ICON_WAVE_W; c++) {
            unsigned n = 0;
            for (uint8_t row = 0; row < ICON_WAVE_H; row++) {
                guarded_t g; guard_init(&g);
                icon_wave_row(y, row, FG, BG, g.px);
                n += g.px[c] == FG;
            }
            const unsigned span = (unsigned)(c ? (y[c] > y[c-1] ? y[c] - y[c-1] : y[c-1] - y[c]) : 0) + 1u;
            CHECK(n == span, "wave %u col %u: %u px, want %u", w, c, n, span);
        }
    }
    /* Square at 50 %: high first half, low second half. */
    uint8_t y[ICON_WAVE_W];
    icon_wave_points(SIGGEN_SQUARE, 50u, y);
    CHECK(y[0] == 0u && y[ICON_WAVE_W - 1] == ICON_WAVE_H - 1u, "square 50%% shape (%u .. %u)",
          y[0], y[ICON_WAVE_W - 1]);
}

/* ── Bar geometry ──────────────────────────────────────────────────── */
static unsigned sw(const char *s)
{
    unsigned t = 0;
    for (; *s; s++) {
        const unsigned c = (unsigned char)*s;
        if (c < font_small.first_char || c > font_small.last_char) continue;
        t += font_small.advances[font_small.charmap[c - font_small.first_char]];
    }
    return t;
}

typedef struct { const char *name; unsigned x, w; } slot_t;

static void test_geometry(void)
{
    const slot_t top[] = {
        { "gen", SB_GEN_X, SB_GEN_W }, { "tb", SB_TB_X, SB_TB_W }, { "run", SB_RUN_X, SB_RUN_W },
        { "focus", SB_FOCUS_X, SB_FOCUS_W }, { "dot", SB_DOT_X, SB_DOT_W },
        { "view", SB_VIEW_X, SB_VIEW_W }, { "batt", SB_BATT_X, SB_BATT_W } };
    const slot_t bot[] = {
        { "ch1", IB_CH1_X, IB_CH_W }, { "trig", IB_TRIG_X, IB_TRIG_W }, { "ch2", IB_CH2_X, IB_CH_W } };
    const unsigned nt = sizeof(top) / sizeof(top[0]), nb = sizeof(bot) / sizeof(bot[0]);
    for (unsigned i = 0; i < nt; i++) {
        CHECK(top[i].x + top[i].w <= 320u, "top slot %s runs off the screen", top[i].name);
        if (i) CHECK(top[i - 1].x + top[i - 1].w <= top[i].x, "top slots %s/%s overlap",
                     top[i - 1].name, top[i].name);
    }
    for (unsigned i = 0; i < nb; i++) {
        CHECK(bot[i].x + bot[i].w <= 320u, "bottom slot %s runs off the screen", bot[i].name);
        if (i) CHECK(bot[i - 1].x + bot[i - 1].w <= bot[i].x, "bottom slots %s/%s overlap",
                     bot[i - 1].name, bot[i].name);
    }
    CHECK(SB_TITLE_X >= SB_GEN_X + SB_GEN_W && SB_TITLE_X + SB_TITLE_W <= SB_BATT_X,
          "title slot overlaps the gen icon or the battery");
    CHECK(IB_Y + SB_H == 240u, "info bar does not end at the bottom row");

    /* Widest contents, measured with the real font. */
    CHECK(ICON_WAVE_W + 4u <= SB_GEN_W, "gen icon + tab margins");
    const char *tb[] = { "H=~2.13ms x2.5", "H=~213us /2.5", "H=500ms", "H=20.0us /2.5", "H=100ns x10" };
    for (unsigned i = 0; i < sizeof(tb) / sizeof(tb[0]); i++)
        CHECK(2u + sw(tb[i]) <= SB_TB_W, "\"%s\" (%u px) overflows the timebase slot", tb[i], sw(tb[i]));
    /* RUN tab: the text must sit inside the tab's narrowest text row (row
     * 13 of the 12 px font at y 2), or its corners paint outside the shear. */
    const unsigned inner = SB_RUN_W - 2u * icon_tab_inset(SB_TEXT_Y + 11u);
    const char *run[] = { "RUN", "STOP", "ARM" };
    for (unsigned i = 0; i < 3; i++)
        CHECK(sw(run[i]) + 2u <= inner, "\"%s\" does not fit the sheared tab (%u > %u)", run[i], sw(run[i]), inner);
    CHECK(sw("X1 Y1") + 2u <= SB_FOCUS_W && sw("Mask") + 2u <= SB_FOCUS_W, "focus words");
    CHECK(7u + 22u + 9u <= SB_FOCUS_W, "focus arrows (x+7 .. x+22+9)");
    CHECK(1u + icon_bitmap(ICON_DOT)->w <= SB_DOT_W, "dot");
    const char *views[] = { "Y-T", "ROLL", "X-Y", "FFT", "SPLIT", "WFALL" };
    for (unsigned i = 0; i < 6; i++)
        CHECK(sw(views[i]) + 2u <= SB_VIEW_W, "view word %s", views[i]);
    CHECK(ICON_BATT_W + 2u + sw("100%") + 2u <= SB_BATT_W, "battery glyph + 100%% (%u)", sw("100%"));
    CHECK(sw("Signal Generator") <= SB_TITLE_W, "default title");
    /* A title at the copy limit in wide characters still fits. */
    char wide[SB_TITLE_MAX];
    memset(wide, 'M', sizeof(wide) - 1u); wide[sizeof(wide) - 1u] = '\0';
    CHECK(sw(wide) <= SB_TITLE_W, "a %u-char title (%u px) overflows %u px",
          (unsigned)(SB_TITLE_MAX - 1u), sw(wide), (unsigned)SB_TITLE_W);

    /* CH badge: chip + gap + "<vdiv>" "X10" "<coupling>" 3 px apart. Every
     * state the keys reach (DC/AC) fits with the chip at the widest label
     * (provisional, 6 chars); GND fits once the chip is dropped. */
    const char *vd[] = { "~888mV", "~13.7V", "1.37V", "--" };
    for (unsigned i = 0; i < sizeof(vd) / sizeof(vd[0]); i++) {
        const unsigned t = sw(vd[i]) + IB_FIELD_GAP + sw("X10") + IB_FIELD_GAP;
        CHECK(IB_CHIP_W + IB_CHIP_GAP + t + sw("AC") <= IB_CH_W, "\"%s X10 AC\" (%u px) overflows the CH badge",
              vd[i], IB_CHIP_W + IB_CHIP_GAP + t + sw("AC"));
        CHECK(IB_CHIP_W + IB_CHIP_GAP + t + sw("DC") <= IB_CH_W, "\"%s X10 DC\" overflows", vd[i]);
        CHECK(t + sw("GND") <= IB_CH_W, "\"%s X10 GND\" overflows even without the chip", vd[i]);
    }
    /* Trigger badge: disc, mode, edge, source, level, 3 px gaps. */
    const unsigned tw = icon_bitmap(ICON_TRIG_DISC)->w + 3u + sw("Single") + 3u +
                        icon_bitmap(ICON_EDGE_RISE)->w + 3u + sw("CH2") + 3u + sw("-100");
    CHECK(tw <= IB_TRIG_W, "widest trigger badge %u px > %u", tw, (unsigned)IB_TRIG_W);
}

int main(void)
{
    test_bitmaps();
    test_shapes();
    test_battery();
    test_wave();
    test_geometry();
    printf("test_icons: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
