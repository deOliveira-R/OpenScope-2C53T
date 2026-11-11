/*
 * OpenScope 2C53T - stock meter behaviours that are pure logic.
 * See meter_stock.h. No hardware, no RTOS: host-testable.
 */

#include "meter_stock.h"

#include <string.h>

/* ── Status text ────────────────────────────────────────────────────── */

meter_stock_status_t meter_stock_status_from_codes(const uint8_t c[4])
{
    /* Stock's order, parser 0x0803DB64..0x0803DBA8 (d0..d3 = c[0]..c[3]). */
    if (c[0] == 10 && c[1] == 11 && c[2] == 12 && c[3] == 13)
        return METER_STATUS_AUTO;                          /* 0x0803DC98 */
    if (c[1] == 0 && c[2] == 14)
        return METER_STATUS_OL;                            /* 0x0803DCCE */
    if (c[0] == 16 && c[1] == 16 && c[2] == 16 && c[3] == 16)
        return METER_STATUS_BLANK;                         /* 0x0803DCE8 */
    if (c[0] == 16 && c[1] == 17 && c[2] == 10 && c[3] == 14)
        return METER_STATUS_CAL;                           /* 0x0803DD06 */
    if (c[1] == 18 && c[2] == 10 && c[3] == 5)
        return METER_STATUS_PAS;                           /* 0x0803DD30 */
    if (c[1] == 19 && c[2] == 20) {                        /* 0x0803DE4C */
        /* (d3 - 1) < 3 ? d3 + 5 : 9 */
        if (c[3] >= 1 && c[3] <= 3)
            return (meter_stock_status_t)(c[3] + 5);
        return METER_STATUS_ERC;
    }
    return METER_STATUS_NONE;
}

const char *meter_stock_status_text(meter_stock_status_t st)
{
    switch (st) {
    case METER_STATUS_AUTO:  return "AUTO";
    case METER_STATUS_OL:    return "OL";
    case METER_STATUS_BLANK: return "---";
    case METER_STATUS_CAL:   return "CAL";
    case METER_STATUS_PAS:   return "PAS";
    case METER_STATUS_ER1:   return "Er1";
    case METER_STATUS_ER2:   return "Er2";
    case METER_STATUS_ER3:   return "Er3";
    case METER_STATUS_ERC:   return "ErC";
    default:                 return "";
    }
}

/* ── degF ───────────────────────────────────────────────────────────── */

float meter_stock_degc_to_degf(float c)
{
    return c * 9.0f / 5.0f + 32.0f;
}

/* ── Selector / range ───────────────────────────────────────────────── */

bool meter_stock_selector_for_submode(uint8_t submode, uint8_t *sel, uint8_t *f36)
{
    /* The function words of fpga_meter_plan.c, by their stock meaning:
     * 0C DCV, 0D ACV, 11 small DC I, 10 large DC I, 16 small AC I, 15 large
     * AC I, 0B Ohm, 17 Continuity, 0E Diode, 0A Cap, 12 Temp. */
    static const uint8_t map[12][2] = {
        { METER_STOCK_SEL_VOLT,   1 },   /* 0 DCV   */
        { METER_STOCK_SEL_VOLT,   2 },   /* 1 ACV   */
        { METER_STOCK_SEL_ISMALL, 1 },   /* 2 DC mA */
        { METER_STOCK_SEL_ILARGE, 1 },   /* 3 DC A  */
        { METER_STOCK_SEL_ISMALL, 2 },   /* 4 AC mA */
        { METER_STOCK_SEL_ILARGE, 2 },   /* 5 AC A  */
        { METER_STOCK_SEL_OHM,    1 },   /* 6 Ohm   */
        { METER_STOCK_SEL_DIODE,  2 },   /* 7 Cont  */
        { METER_STOCK_SEL_DIODE,  1 },   /* 8 Diode */
        { METER_STOCK_SEL_CAP,    1 },   /* 9 Cap   */
        { METER_STOCK_SEL_TEMP,   1 },   /* 10 Temp */
        { METER_STOCK_SEL_AUTO,   0 },   /* 11 Auto: F36 is the SoC's to decide */
    };
    if (submode >= 12) return false;
    if (sel) *sel = map[submode][0];
    if (f36) *f36 = map[submode][1];
    return true;
}

/* ── Auto (selector 0, 0x0803E1F2) ──────────────────────────────────── */

void meter_auto_fsm_reset(meter_auto_fsm_t *st)
{
    st->f2f = 0;
    st->f36 = 1;
}

void meter_auto_fsm_auto_text(meter_auto_fsm_t *st)
{
    st->f2f = 0;
    st->f36 = 0;
}

meter_auto_fn_t meter_auto_fsm_step(meter_auto_fsm_t *st, bool numeric,
                                    uint8_t rx2, uint8_t rx6, uint8_t rx7)
{
    const bool f6_6 = (rx6 & 0x40u) != 0;
    const bool f7_0 = (rx7 & 0x01u) != 0;
    const bool f7_2 = (rx7 & 0x04u) != 0;

    if (numeric) st->f2f = 0xFF;                    /* 0x0803E1AA */
    if (st->f2f == 0)                               /* 0x0803E1F6: undecided */
        return meter_auto_fsm_function(st);

    if (rx7 & 0x20u) {                              /* 0x0803E2E0: resistance */
        st->f36 = 0;
        st->f37 = f7_2 ? 2 : (f6_6 ? 1 : 0);
        st->f2f = 3;
    } else if (rx2 & 0x02u) {                       /* 0x0803E328: DC voltage */
        st->f36 = 1;
        st->f37 = f7_0 ? 0 : 1;
        st->f2f = 1;
    } else if (rx2 & 0x01u) {                       /* 0x0803E338: AC voltage */
        st->f36 = 2;
        st->f37 = f7_0 ? 0 : 1;
        st->f2f = 1;
    } else if (rx7 & 0x08u) {                       /* 0x0803E3A8: continuity */
        st->f36 = 2;
        st->f37 = f7_2 ? 2 : (f6_6 ? 1 : 0);
        st->f2f = 2;
    } else {                                        /* 0x0803E222 */
        st->f36 = 0;
    }
    return meter_auto_fsm_function(st);
}

meter_auto_fn_t meter_auto_fsm_function(const meter_auto_fsm_t *st)
{
    if (st->f2f == 1 && st->f36 == 1) return METER_AUTO_FN_DCV;
    if (st->f2f == 1 && st->f36 == 2) return METER_AUTO_FN_ACV;
    if (st->f2f == 3 && st->f36 == 0) return METER_AUTO_FN_OHM;
    if (st->f2f == 2 && st->f36 == 2) return METER_AUTO_FN_CONT;
    return METER_AUTO_FN_NONE;
}

uint8_t meter_auto_fn_submode(meter_auto_fn_t fn)
{
    switch (fn) {
    case METER_AUTO_FN_DCV:  return 0;
    case METER_AUTO_FN_ACV:  return 1;
    case METER_AUTO_FN_OHM:  return 6;
    case METER_AUTO_FN_CONT: return 7;
    default:                 return 0xFF;
    }
}

bool meter_auto_fsm_hz_valid(const meter_auto_fsm_t *st)
{
    return st->f36 == 2 && st->f2f == 1;
}

uint8_t meter_stock_range_index(uint8_t sel, uint8_t f36, bool degf,
                                uint8_t rx6, uint8_t rx7, uint8_t prev)
{
    const bool f6_4 = (rx6 & 0x10u) != 0;
    const bool f6_5 = (rx6 & 0x20u) != 0;
    const bool f6_6 = (rx6 & 0x40u) != 0;
    const bool f7_0 = (rx7 & 0x01u) != 0;
    const bool f7_2 = (rx7 & 0x04u) != 0;
    const bool f7_3 = (rx7 & 0x08u) != 0;

    switch (sel) {
    case METER_STOCK_SEL_VOLT:                 /* 0x0803E228 */
        return f7_0 ? 0 : 1;
    case METER_STOCK_SEL_DIODE:                /* 0x0803E23E */
        if (f36 == 1)                          /* diode: 0x0803E248 */
            return f7_0 ? 0 : 1;
        if (!f7_3) return prev;                /* continuity, 0x0803E2C8: untouched */
        if (f7_2) return 2;                    /* 0x0803E330 */
        return f6_6 ? 1 : 0;                   /* 0x0803E31A */
    case METER_STOCK_SEL_OHM:                  /* 0x0803E25E */
        if (f7_2) return 2;
        return f6_6 ? 1 : 0;
    case METER_STOCK_SEL_CAP:                  /* 0x0803E27A */
        if (f6_5) return 0;
        if (f6_4) return 1;
        return f7_0 ? 2 : 3;
    case METER_STOCK_SEL_TEMP:                 /* 0x0803E298 */
        return degf ? 4 : 0;
    case METER_STOCK_SEL_ISMALL:               /* 0x0803E1D0 (selectors 6 and 7) */
    case METER_STOCK_SEL_ILARGE:
        if (f6_4) return 0;
        return f7_0 ? 1 : 2;
    default:                                   /* Auto: not ported */
        return prev;
    }
}

void meter_stock_range_label(uint8_t sel, uint8_t f36, bool degf, uint8_t idx,
                             char *buf, unsigned n)
{
    const char *t = NULL;

    (void)degf;
    if (sel == METER_STOCK_SEL_VOLT) {
        t = idx == 0 ? "mV" : idx == 1 ? "V" : NULL;
    } else if (sel == METER_STOCK_SEL_OHM) {
        t = idx == 0 ? "Ohm" : idx == 1 ? "kOhm" : idx == 2 ? "MOhm" : NULL;
    } else if (sel == METER_STOCK_SEL_TEMP) {
        t = idx == 0 ? "C" : idx == 4 ? "F" : NULL;
    }
    (void)f36;
    if (n == 0) return;
    {
        /* "R<n>" where the unit is not settled. Both paths truncate to n-1
         * characters: the old fallback wrote buf[1..2] before checking n. */
        char r[3] = { 'R', (char)('0' + (idx % 10)), '\0' };
        strncpy(buf, t ? t : r, n - 1);
        buf[n - 1] = '\0';
    }
}

float meter_stock_range_scale(uint8_t idx)
{
    float s = 1.0f;
    if (idx > 4) idx = 4;
    while (idx--) s *= 1000.0f;
    return s;
}

float meter_stock_rescale(float v, uint8_t from, uint8_t to)
{
    if (from == to) return v;
    return v * meter_stock_range_scale(from) / meter_stock_range_scale(to);
}

float meter_unit_factor(const char *u)
{
    static const struct { const char *u; float f; } t[] = {
        { "V", 1.0f },     { "mV", 1e-3f },
        { "A", 1.0f },     { "mA", 1e-3f },   { "uA", 1e-6f },
        { "Ohm", 1.0f },   { "kOhm", 1e3f },  { "MOhm", 1e6f },
        { "nF", 1e-9f },   { "uF", 1e-6f },   { "mF", 1e-3f }, { "F", 1.0f },
        { "C", 1.0f },
    };
    if (!u) return 0.0f;
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
        if (strcmp(u, t[i].u) == 0) return t[i].f;
    return 0.0f;
}

/* ── Beep ───────────────────────────────────────────────────────────── */

bool meter_stock_beep_gate(uint8_t sel, uint8_t f36)
{
    if (sel == METER_STOCK_SEL_AUTO) return true;
    return sel == METER_STOCK_SEL_DIODE && f36 == 2;
}

bool meter_stock_beep_gate_for_submode(uint8_t submode)
{
    uint8_t sel, f36;
    if (!meter_stock_selector_for_submode(submode, &sel, &f36)) return false;
    return meter_stock_beep_gate(sel, f36);
}

void meter_beep_pulse_edge(meter_beep_pulse_t *p)
{
    p->active = true;
    p->ticks = 0;
}

bool meter_beep_pulse_tick(meter_beep_pulse_t *p)
{
    if (!p->active) {
        p->ticks = 0;
        return false;
    }
    p->ticks++;
    if (p->ticks >= METER_BEEP_PULSE_TICKS) {
        p->active = false;
        p->ticks = 0;
    }
    return p->active;
}

/* ── Calibration-wizard guard ───────────────────────────────────────── */

bool meter_word_is_cal_wizard(uint16_t word)
{
    return word == 0x02A0u || word == 0x0503u || word == 0x0501u;
}

/* ── Formatting ─────────────────────────────────────────────────────── */

void meter_stock_format_fixed(char *buf, unsigned n, float v, unsigned decimals)
{
    char tmp[24];
    unsigned t = 0, i = 0;
    uint32_t scale = 1;
    uint32_t whole, frac;
    float a;

    if (n == 0) return;
    if (decimals > 4) decimals = 4;
    for (unsigned d = 0; d < decimals; d++) scale *= 10u;

    a = v < 0.0f ? -v : v;
    if (a > 4.0e8f) a = 4.0e8f;
    {
        uint32_t scaled = (uint32_t)(a * (float)scale + 0.5f);
        whole = scaled / scale;
        frac = scaled % scale;
    }
    if (v < 0.0f && (whole != 0 || frac != 0)) {
        if (i + 1 < n) buf[i++] = '-';
    }
    if (whole == 0) tmp[t++] = '0';
    while (whole > 0 && t < sizeof tmp) { tmp[t++] = (char)('0' + whole % 10u); whole /= 10u; }
    while (t > 0 && i + 1 < n) buf[i++] = tmp[--t];
    if (decimals > 0 && i + 1 < n) {
        buf[i++] = '.';
        for (unsigned d = decimals; d > 0 && i + 1 < n; d--) {
            uint32_t p = 1;
            for (unsigned k = 1; k < d; k++) p *= 10u;
            buf[i++] = (char)('0' + (frac / p) % 10u);
        }
    }
    buf[i] = '\0';
}
