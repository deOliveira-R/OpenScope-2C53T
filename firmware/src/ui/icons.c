/*
 * OpenScope 2C53T - small glyphs (see icons.h). Pure: no LCD.
 */
#include "icons.h"
#include "siggen_wave.h"
#include <stddef.h>

/* Row literals: bit 15 is x = 0, so the binary reads as the picture. */
#define R(b)  ((uint16_t)(b))

/* 9x12. Low run x0..4 on row 10, the step at x4, high run x4..8 on row 1, and
 * an arrowhead on the step pointing the way the edge goes. */
static const uint16_t edge_rise[12] = {
    R(0x0000),
    R(0x0F80),   /* ....##### */
    R(0x0800),   /* ....#.... */
    R(0x0800),
    R(0x1C00),   /* ...###... */
    R(0x3E00),   /* ..#####.. */
    R(0x0800),
    R(0x0800),
    R(0x0800),
    R(0x0800),
    R(0xF800),   /* #####.... */
    R(0x0000),
};
static const uint16_t edge_fall[12] = {
    R(0x0000),
    R(0xF800),   /* #####.... */
    R(0x0800),
    R(0x0800),
    R(0x0800),
    R(0x0800),
    R(0x3E00),   /* ..#####.. */
    R(0x1C00),   /* ...###... */
    R(0x0800),
    R(0x0800),
    R(0x0F80),   /* ....##### */
    R(0x0000),
};
/* 7x7 triangles. */
static const uint16_t arrow_left[7] = {
    R(0x0200),   /* ......# */
    R(0x0E00),   /* ....### */
    R(0x3E00),   /* ..##### */
    R(0xFE00),   /* ####### */
    R(0x3E00),
    R(0x0E00),
    R(0x0200),
};
static const uint16_t arrow_right[7] = {
    R(0x8000),   /* #...... */
    R(0xE000),   /* ###.... */
    R(0xF800),   /* #####.. */
    R(0xFE00),   /* ####### */
    R(0xF800),
    R(0xE000),
    R(0x8000),
};
/* 9x5 */
static const uint16_t arrow_down[5] = {
    R(0xFF80),   /* ######### */
    R(0x7F00),   /* .#######. */
    R(0x3E00),   /* ..#####.. */
    R(0x1C00),   /* ...###... */
    R(0x0800),   /* ....#.... */
};
static const uint16_t arrow_up[5] = {
    R(0x0800),
    R(0x1C00),
    R(0x3E00),
    R(0x7F00),
    R(0xFF80),
};
/* 11x11 disc, and the "T" that goes over it. */
static const uint16_t trig_disc[11] = {
    R(0x1F00),   /* ...#####... */
    R(0x7FC0),   /* .#########. */
    R(0x7FC0),
    R(0xFFE0),   /* ########### */
    R(0xFFE0),
    R(0xFFE0),
    R(0xFFE0),
    R(0xFFE0),
    R(0x7FC0),
    R(0x7FC0),
    R(0x1F00),
};
static const uint16_t trig_t[11] = {
    R(0x0000),
    R(0x0000),
    R(0x3F80),   /* ..#######.. */
    R(0x3F80),
    R(0x0E00),   /* ....###.... */
    R(0x0E00),
    R(0x0E00),
    R(0x0E00),
    R(0x0E00),
    R(0x0000),
    R(0x0000),
};
/* 7x7 */
static const uint16_t dot[7] = {
    R(0x3800),   /* ..###.. */
    R(0x7C00),   /* .#####. */
    R(0xFE00),   /* ####### */
    R(0xFE00),
    R(0xFE00),
    R(0x7C00),
    R(0x3800),
};
/* 8x6, fits the battery's 16x6 interior. */
static const uint16_t bolt[6] = {
    R(0x0600),   /* .....##. */
    R(0x0C00),   /* ....##.. */
    R(0x1F00),   /* ...##### */
    R(0xF800),   /* #####... */
    R(0x3000),   /* ..##.... */
    R(0x6000),   /* .##..... */
};

static const icon_bitmap_t icon_table[ICON_COUNT] = {
    [ICON_EDGE_RISE]   = {  9, 12, edge_rise },
    [ICON_EDGE_FALL]   = {  9, 12, edge_fall },
    [ICON_ARROW_LEFT]  = {  7,  7, arrow_left },
    [ICON_ARROW_RIGHT] = {  7,  7, arrow_right },
    [ICON_ARROW_DOWN]  = {  9,  5, arrow_down },
    [ICON_ARROW_UP]    = {  9,  5, arrow_up },
    [ICON_TRIG_DISC]   = { 11, 11, trig_disc },
    [ICON_TRIG_T]      = { 11, 11, trig_t },
    [ICON_DOT]         = {  7,  7, dot },
    [ICON_BOLT]        = {  8,  6, bolt },
};

const icon_bitmap_t *icon_bitmap(icon_id_t id)
{
    return ((unsigned)id < (unsigned)ICON_COUNT) ? &icon_table[id] : NULL;
}

bool icon_px(const icon_bitmap_t *ic, uint8_t x, uint8_t y)
{
    if (ic == NULL || x >= ic->w || y >= ic->h || x >= 16u)
        return false;
    return (ic->rows[y] & (uint16_t)(0x8000u >> x)) != 0u;
}

void icon_row(const icon_bitmap_t *ic, uint8_t row, uint16_t fg, uint16_t bg,
              uint16_t *line)
{
    if (ic == NULL || line == NULL || row >= ic->h)
        return;
    for (uint8_t x = 0; x < ic->w; x++)
        line[x] = icon_px(ic, x, row) ? fg : bg;
}

void icon_row_over(const icon_bitmap_t *ic, uint8_t row, uint8_t dx, uint16_t fg,
                   uint16_t *line, uint8_t line_w)
{
    if (ic == NULL || line == NULL || row >= ic->h)
        return;
    for (uint8_t x = 0; x < ic->w; x++) {
        const unsigned px = (unsigned)dx + x;
        if (px >= line_w)
            break;
        if (icon_px(ic, x, row))
            line[px] = fg;
    }
}

/* ── Battery ──────────────────────────────────────────────────────── */
/* Body x0..19 (1 px outline), nub x20..21 on rows 3..6, interior x2..17 on
 * rows 2..7 (a 1 px gap inside the outline), 4 bars of 3 px with 1 px gaps
 * at x2, 6, 10, 14. */
#define BATT_BODY_W   20u
#define BATT_NUB_Y0   3u
#define BATT_NUB_Y1   6u
#define BATT_IN_X0    2u
#define BATT_IN_Y0    2u
#define BATT_IN_Y1    7u
#define BATT_BAR_W    3u
#define BATT_BAR_PITCH 4u
#define BATT_BOLT_DX  6u     /* bolt at x6..13, centred in x2..17 */

uint8_t icon_battery_bars(uint8_t pct)
{
    if (pct == 0u)  return 0u;
    if (pct > 100u) pct = 100u;
    return (uint8_t)((pct + 24u) / 25u);
}

void icon_battery_row(uint8_t row, uint8_t bars, bool charging,
                      uint16_t fg, uint16_t fill, uint16_t bg, uint16_t *line)
{
    if (line == NULL || row >= ICON_BATT_H)
        return;
    if (bars > ICON_BATT_BARS) bars = ICON_BATT_BARS;
    for (uint8_t x = 0; x < ICON_BATT_W; x++) {
        uint16_t c = bg;
        if (x < BATT_BODY_W) {
            if (row == 0u || row == ICON_BATT_H - 1u || x == 0u || x == BATT_BODY_W - 1u)
                c = fg;
            else if (!charging && row >= BATT_IN_Y0 && row <= BATT_IN_Y1 && x >= BATT_IN_X0) {
                const uint8_t k = (uint8_t)((x - BATT_IN_X0) / BATT_BAR_PITCH);
                const uint8_t o = (uint8_t)((x - BATT_IN_X0) % BATT_BAR_PITCH);
                if (k < bars && o < BATT_BAR_W)
                    c = fill;
            }
        } else if (row >= BATT_NUB_Y0 && row <= BATT_NUB_Y1) {
            c = fg;
        }
        line[x] = c;
    }
    if (charging && row >= BATT_IN_Y0)
        icon_row_over(&icon_table[ICON_BOLT], (uint8_t)(row - BATT_IN_Y0), BATT_BOLT_DX,
                      fill, line, (uint8_t)ICON_BATT_W);
}

/* ── Generator waveform ───────────────────────────────────────────── */
void icon_wave_points(uint8_t wave, uint8_t duty_pct, uint8_t y[ICON_WAVE_W])
{
    uint16_t cyc[SIGGEN_CYCLE_SAMPLES];
    /* Full amplitude: the icon is the shape, not the level. */
    siggen_wave_build(cyc, wave, 30u, duty_pct);
    /* Column c shows sample c * 100 / 16 (0, 6, 12, ... 93), scaled to the
     * min..max of those 16 points so every shape fills the icon. */
    uint16_t v[ICON_WAVE_W];
    uint16_t mn = 0xFFFFu, mx = 0u;
    for (unsigned c = 0; c < ICON_WAVE_W; c++) {
        v[c] = cyc[(c * SIGGEN_CYCLE_SAMPLES) / ICON_WAVE_W];
        if (v[c] < mn) mn = v[c];
        if (v[c] > mx) mx = v[c];
    }
    for (unsigned c = 0; c < ICON_WAVE_W; c++) {
        if (mx <= mn) {
            y[c] = (uint8_t)(ICON_WAVE_H / 2u);
        } else {
            const uint32_t s = ((uint32_t)(v[c] - mn) * (ICON_WAVE_H - 1u) + (mx - mn) / 2u)
                             / (uint32_t)(mx - mn);
            y[c] = (uint8_t)((ICON_WAVE_H - 1u) - s);
        }
    }
}

void icon_wave_row(const uint8_t y[ICON_WAVE_W], uint8_t row, uint16_t fg,
                   uint16_t bg, uint16_t *line)
{
    if (y == NULL || line == NULL || row >= ICON_WAVE_H)
        return;
    for (unsigned c = 0; c < ICON_WAVE_W; c++) {
        uint8_t a = y[c], b = (c > 0u) ? y[c - 1u] : y[c];
        if (a >= ICON_WAVE_H) a = ICON_WAVE_H - 1u;
        if (b >= ICON_WAVE_H) b = ICON_WAVE_H - 1u;
        const uint8_t lo = (a < b) ? a : b, hi = (a < b) ? b : a;
        line[c] = (row >= lo && row <= hi) ? fg : bg;
    }
}

/* ── Tab ──────────────────────────────────────────────────────────── */
uint8_t icon_tab_inset(uint8_t row)
{
    return (uint8_t)(row / ICON_TAB_SHEAR);
}
