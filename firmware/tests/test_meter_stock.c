/*
 * Host tests for the stock meter behaviours in meter_stock.c and their hooks
 * in meter_data.c: status text, degF, range index/label, rescale, the beep
 * gate and pulse, and the calibration-wizard word guard.
 *
 * Vectors come from the disassembly of stock V1.2.0 (addresses in
 * meter_stock.c); frames come from the captured unit #2 text in
 * test_meter_data.c. Nothing here is a bench measurement of the SoC beyond
 * those frames.
 */

#include <stdio.h>
#include <string.h>

#include "meter_data.h"
#include "meter_stock.h"
#include "fpga_meter_plan.h"

static int tests_run, tests_passed;

#define TEST(name) do { \
    tests_run++; \
    printf("  %-52s ", #name); \
    if (test_##name()) { tests_passed++; printf("PASS\n"); } \
    else { printf("FAIL\n"); } \
} while (0)

#define ASSERT(cond) do { \
    if (!(cond)) { printf("[line %d: %s] ", __LINE__, #cond); return 0; } \
} while (0)

#define ASSERT_STR(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        printf("[line %d: \"%s\" != \"%s\"] ", __LINE__, (a), (b)); return 0; } \
} while (0)

static int close_to(float a, float b, float tol)
{
    float d = a - b;
    return (d < 0 ? -d : d) <= tol;
}

/* The inverse of meter_data.c's bcd_lookup for the glyphs used here. */
static uint8_t seg(uint8_t code)
{
    switch (code) {
    case 0: return 0xEB;  case 1: return 0x0A;  case 2: return 0xAD;
    case 3: return 0x8F;  case 4: return 0x4E;  case 5: return 0xC7;
    case 6: return 0xE7;  case 7: return 0x8A;  case 8: return 0xEF;
    case 9: return 0xCF;
    case 10: return 0xEE; case 11: return 0x23; case 12: return 0x65;
    case 13: return 0x27; case 14: return 0x61; case 16: return 0x00;
    case 17: return 0xE1; case 18: return 0xEC; case 19: return 0xE5;
    case 20: return 0x24;
    default: return 0x01;
    }
}

static void glyph_frame(uint8_t f[12], const uint8_t c[4], uint8_t f6hi, uint8_t f7)
{
    uint8_t n0 = seg(c[0]), n1 = seg(c[1]), n2 = seg(c[2]), n3 = seg(c[3]);
    memset(f, 0, 12);
    f[0] = 0x5A; f[1] = 0xA5;
    f[2] = n0 & 0xF0;
    f[3] = (n1 & 0xF0) | (n0 & 0x0F);
    f[4] = (n2 & 0xF0) | (n1 & 0x0F);
    f[5] = (n3 & 0xF0) | (n2 & 0x0F);
    f[6] = (f6hi & 0xF0) | (n3 & 0x0F);
    f[7] = f7;
    f[8] = 0x00;
}

static void hex_frame(uint8_t f[12], const char *hex)
{
    for (int i = 0; i < 12; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        f[i] = (uint8_t)v;
    }
}

static void feed(const uint8_t f[12], uint8_t submode)
{
    meter_data_process_frame((const volatile uint8_t *)f, submode);
}

/* ── status text ────────────────────────────────────────────────────── */

static int test_status_vectors(void)
{
    const uint8_t autot[4] = { 10, 11, 12, 13 };
    const uint8_t ol[4]    = { 16, 0, 14, 16 };
    const uint8_t blank[4] = { 16, 16, 16, 16 };
    const uint8_t cal[4]   = { 16, 17, 10, 14 };
    const uint8_t pas[4]   = { 16, 18, 10, 5 };
    const uint8_t er1[4]   = { 16, 19, 20, 1 };
    const uint8_t er2[4]   = { 16, 19, 20, 2 };
    const uint8_t er3[4]   = { 16, 19, 20, 3 };
    const uint8_t erc[4]   = { 16, 19, 20, 12 };
    const uint8_t er0[4]   = { 16, 19, 20, 0 };
    const uint8_t num[4]   = { 1, 2, 3, 4 };
    const uint8_t au[4]    = { 10, 11, 5, 5 };

    ASSERT(meter_stock_status_from_codes(autot) == METER_STATUS_AUTO);
    ASSERT(meter_stock_status_from_codes(ol) == METER_STATUS_OL);
    ASSERT(meter_stock_status_from_codes(blank) == METER_STATUS_BLANK);
    ASSERT(meter_stock_status_from_codes(cal) == METER_STATUS_CAL);
    ASSERT(meter_stock_status_from_codes(pas) == METER_STATUS_PAS);
    ASSERT(meter_stock_status_from_codes(er1) == METER_STATUS_ER1);
    ASSERT(meter_stock_status_from_codes(er2) == METER_STATUS_ER2);
    ASSERT(meter_stock_status_from_codes(er3) == METER_STATUS_ER3);
    ASSERT(meter_stock_status_from_codes(erc) == METER_STATUS_ERC);
    ASSERT(meter_stock_status_from_codes(er0) == METER_STATUS_ERC);   /* (d3-1)<3 fails */
    ASSERT(meter_stock_status_from_codes(num) == METER_STATUS_NONE);
    ASSERT(meter_stock_status_from_codes(au) == METER_STATUS_NONE);   /* "AU" alone is not AUTO */
    ASSERT_STR(meter_stock_status_text(METER_STATUS_ER3), "Er3");
    ASSERT_STR(meter_stock_status_text(METER_STATUS_PAS), "PAS");
    return 1;
}

static int test_status_text_reaches_the_reading(void)
{
    uint8_t f[12];
    static const struct { uint8_t c[4]; const char *txt; uint8_t code; } v[] = {
        { { 16, 17, 10, 14 }, "CAL", METER_STATUS_CAL },
        { { 16, 19, 20, 1 },  "Er1", METER_STATUS_ER1 },
        { { 16, 19, 20, 2 },  "Er2", METER_STATUS_ER2 },
        { { 16, 19, 20, 3 },  "Er3", METER_STATUS_ER3 },
        { { 16, 19, 20, 12 }, "ErC", METER_STATUS_ERC },
        { { 10, 11, 12, 13 }, "AUTO", METER_STATUS_AUTO },
    };

    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
        meter_data_init();
        glyph_frame(f, v[i].c, 0, 0);
        feed(f, 6);
        ASSERT(meter_reading.valid);
        ASSERT_STR(meter_reading.display_str, v[i].txt);
        ASSERT(meter_reading.status_code == v[i].code);
        ASSERT(meter_reading.result_class != METER_RESULT_NORMAL);
    }
    /* PAS is the continuity pass glyph run: labelled, but still decoded as
     * the continuity marker, which the beep path depends on. */
    meter_data_init();
    {
        const uint8_t c[4] = { 16, 18, 10, 5 };
        glyph_frame(f, c, 0, 0);
        feed(f, 7);
        ASSERT(meter_reading.status_code == METER_STATUS_PAS);
        ASSERT(meter_reading.result_class == METER_RESULT_CONTINUITY);
        ASSERT(meter_reading.continuity_beep);
    }
    /* A number clears it again. */
    {
        const uint8_t n[4] = { 1, 2, 3, 4 };
        glyph_frame(f, n, 0, 0);
        feed(f, 6);
        ASSERT(meter_reading.status_code == 0);
    }
    return 1;
}

/* ── degF ───────────────────────────────────────────────────────────── */

static int test_degf_conversion(void)
{
    ASSERT(close_to(meter_stock_degc_to_degf(0.0f), 32.0f, 1e-4f));
    ASSERT(close_to(meter_stock_degc_to_degf(100.0f), 212.0f, 1e-3f));
    ASSERT(close_to(meter_stock_degc_to_degf(-40.0f), -40.0f, 1e-3f));
    ASSERT(close_to(meter_stock_degc_to_degf(24.8f), 76.64f, 1e-2f));
    return 1;
}

static int test_degf_toggle_in_parser(void)
{
    uint8_t f[12];

    /* unit #2's internal-sensor frame: "  28" C */
    hex_frame(f, "5AA50000A0ED0F002000011F");
    meter_data_init();
    meter_data_set_fahrenheit(false);
    feed(f, 10);
    ASSERT_STR(meter_reading.display_str, "28");
    ASSERT_STR(meter_reading.unit_suffix, "C");

    meter_data_set_fahrenheit(true);
    feed(f, 10);
    ASSERT_STR(meter_reading.display_str, "82.4");
    ASSERT_STR(meter_reading.unit_suffix, "F");
    ASSERT(close_to(meter_reading.value, 82.4f, 0.01f));
    ASSERT(meter_reading.range_index == 4);        /* stock: unit index 9 + 4*F39 */
    ASSERT_STR(meter_reading.range_label, "F");

    /* the toggle only touches temperature */
    hex_frame(f, "5AA50000A0ED0F002000011F");
    feed(f, 6);
    ASSERT(strcmp(meter_reading.unit_suffix, "F") != 0);

    meter_data_set_fahrenheit(false);
    hex_frame(f, "5AA50000A0ED0F002000011F");
    feed(f, 10);
    ASSERT_STR(meter_reading.unit_suffix, "C");
    ASSERT(meter_reading.range_index == 0);
    return 1;
}

/* ── range index ────────────────────────────────────────────────────── */

static int test_range_index_vectors(void)
{
    /* selectors 1..7, rx[6] bits 4/5/6, rx[7] bits 0/2/3 (0x0803E1D0..0x0803E348) */
    ASSERT(meter_stock_range_index(1, 1, false, 0x00, 0x01, 9) == 0);   /* V: f7.0 -> mV */
    ASSERT(meter_stock_range_index(1, 2, false, 0x00, 0x00, 9) == 1);   /* V */
    ASSERT(meter_stock_range_index(3, 1, false, 0x00, 0x00, 9) == 0);   /* Ohm */
    ASSERT(meter_stock_range_index(3, 1, false, 0x40, 0x00, 9) == 1);   /* kOhm: f6.6 */
    ASSERT(meter_stock_range_index(3, 1, false, 0x40, 0x04, 9) == 2);   /* f7.2 wins */
    ASSERT(meter_stock_range_index(3, 1, false, 0x00, 0x24, 9) == 2);   /* EXP-206 300 kOhm frame */
    ASSERT(meter_stock_range_index(4, 1, false, 0x20, 0x00, 9) == 0);   /* cap: f6.5 */
    ASSERT(meter_stock_range_index(4, 1, false, 0x10, 0x00, 9) == 1);   /* f6.4 */
    ASSERT(meter_stock_range_index(4, 1, false, 0x00, 0x01, 9) == 2);
    ASSERT(meter_stock_range_index(4, 1, false, 0x00, 0x00, 9) == 3);
    ASSERT(meter_stock_range_index(5, 1, false, 0x00, 0x00, 9) == 0);   /* temp */
    ASSERT(meter_stock_range_index(5, 1, true,  0x00, 0x00, 9) == 4);
    ASSERT(meter_stock_range_index(6, 1, false, 0x10, 0x00, 9) == 0);   /* currents */
    ASSERT(meter_stock_range_index(6, 1, false, 0x00, 0x01, 9) == 1);
    ASSERT(meter_stock_range_index(7, 1, false, 0x00, 0x00, 9) == 2);
    /* diode / continuity */
    ASSERT(meter_stock_range_index(2, 1, false, 0x00, 0x01, 9) == 0);
    ASSERT(meter_stock_range_index(2, 1, false, 0x00, 0x00, 9) == 1);
    ASSERT(meter_stock_range_index(2, 2, false, 0x00, 0x00, 7) == 7);   /* f7.3 clear: untouched */
    ASSERT(meter_stock_range_index(2, 2, false, 0x40, 0x08, 7) == 1);
    ASSERT(meter_stock_range_index(2, 2, false, 0x00, 0x08, 7) == 0);
    ASSERT(meter_stock_range_index(2, 2, false, 0x00, 0x0C, 7) == 2);
    return 1;
}

static int test_range_label_and_reading(void)
{
    char b[8];
    uint8_t f[12];

    meter_stock_range_label(3, 1, false, 1, b, sizeof b);
    ASSERT_STR(b, "kOhm");
    meter_stock_range_label(3, 1, false, 2, b, sizeof b);
    ASSERT_STR(b, "MOhm");
    meter_stock_range_label(1, 1, false, 0, b, sizeof b);
    ASSERT_STR(b, "mV");
    meter_stock_range_label(4, 1, false, 3, b, sizeof b);
    ASSERT_STR(b, "R3");                          /* unit not settled: index, not a guess */
    {
        /* a short buffer truncates and never writes past n */
        char s[4] = { 'x', 'x', 'x', 'x' };
        meter_stock_range_label(4, 1, false, 3, s, 2);
        ASSERT(s[0] == 'R' && s[1] == '\0' && s[2] == 'x' && s[3] == 'x');
        meter_stock_range_label(3, 1, false, 1, s, 2);
        ASSERT(s[0] == 'k' && s[1] == '\0' && s[2] == 'x');
    }

    /* a resistance frame in the kOhm regime (f6 upper nibble 4) */
    meter_data_init();
    {
        const uint8_t c[4] = { 9, 9, 2, 4 };
        glyph_frame(f, c, 0x40, 0x00);
        f[3] |= 0x10;                              /* DP before digit 1: 9.924 */
        feed(f, 6);
    }
    ASSERT(meter_reading.range_index == 1);
    ASSERT_STR(meter_reading.range_label, "kOhm");
    return 1;
}

static int test_selector_for_submode(void)
{
    uint8_t sel, f36;
    ASSERT(meter_stock_selector_for_submode(7, &sel, &f36) && sel == 2 && f36 == 2);  /* Cont */
    ASSERT(meter_stock_selector_for_submode(8, &sel, &f36) && sel == 2 && f36 == 1);  /* Diode */
    ASSERT(meter_stock_selector_for_submode(1, &sel, &f36) && sel == 1 && f36 == 2);  /* ACV */
    ASSERT(meter_stock_selector_for_submode(10, &sel, &f36) && sel == 5);
    ASSERT(meter_stock_selector_for_submode(11, &sel, &f36) && sel == 0 && f36 == 0);  /* Auto */
    ASSERT(!meter_stock_selector_for_submode(12, &sel, &f36));
    return 1;
}

/* ── rescale ────────────────────────────────────────────────────────── */

static int test_rescale(void)
{
    ASSERT(close_to(meter_stock_range_scale(0), 1.0f, 0));
    ASSERT(close_to(meter_stock_range_scale(2), 1e6f, 1.0f));
    /* 9.924 kOhm (idx 1) shown on an Ohm range (idx 0) */
    ASSERT(close_to(meter_stock_rescale(9.924f, 1, 0), 9924.0f, 0.01f));
    /* 500 Ohm seen on the kOhm range */
    ASSERT(close_to(meter_stock_rescale(500.0f, 0, 1), 0.5f, 1e-5f));
    ASSERT(close_to(meter_stock_rescale(3.0f, 1, 1), 3.0f, 0));
    /* min of a kOhm reading and an Ohm reading is the Ohm one */
    {
        float a = meter_stock_rescale(9.924f, 1, 0);
        float b = meter_stock_rescale(500.0f, 0, 0);
        ASSERT((b < a ? b : a) == 500.0f);
    }
    ASSERT(close_to(meter_unit_factor("kOhm"), 1000.0f, 0));
    ASSERT(close_to(meter_unit_factor("mV"), 0.001f, 1e-9f));
    ASSERT(close_to(meter_unit_factor("uF"), 1e-6f, 1e-12f));
    ASSERT(meter_unit_factor("bogus") == 0.0f);
    ASSERT(meter_unit_factor(NULL) == 0.0f);
    return 1;
}

static int test_format_fixed(void)
{
    char b[16];
    meter_stock_format_fixed(b, sizeof b, 82.44f, 1);  ASSERT_STR(b, "82.4");
    meter_stock_format_fixed(b, sizeof b, -12.3f, 2);  ASSERT_STR(b, "-12.30");
    meter_stock_format_fixed(b, sizeof b, 0.0f, 1);    ASSERT_STR(b, "0.0");
    meter_stock_format_fixed(b, sizeof b, 9.96f, 1);   ASSERT_STR(b, "10.0");
    meter_stock_format_fixed(b, sizeof b, -0.04f, 1);  ASSERT_STR(b, "0.0");  /* no "-0.0" */
    meter_stock_format_fixed(b, sizeof b, 5.0f, 0);    ASSERT_STR(b, "5");
    return 1;
}

/* ── beep ───────────────────────────────────────────────────────────── */

static int test_beep_gate_only_auto_and_continuity(void)
{
    /* stock selectors: only Auto (0), and 2 with F36 == 2 */
    for (uint8_t sel = 0; sel < 8; sel++)
        for (uint8_t f36 = 0; f36 < 3; f36++) {
            bool want = (sel == 0) || (sel == 2 && f36 == 2);
            ASSERT(meter_stock_beep_gate(sel, f36) == want);
        }
    /* OpenScope submodes: continuity only (there is no Auto submode) */
    for (uint8_t m = 0; m < 11; m++)
        ASSERT(meter_stock_beep_gate_for_submode(m) == (m == 7));
    ASSERT(!meter_stock_beep_gate_for_submode(8));      /* diode: the checker's correction */
    ASSERT(!meter_stock_beep_gate_for_submode(6));      /* ohms */
    ASSERT(!meter_stock_beep_gate_for_submode(0));      /* DCV */
    ASSERT(!meter_stock_beep_gate_for_submode(200));
    return 1;
}

static int test_beep_pulse_is_fixed_length(void)
{
    meter_beep_pulse_t p = { 0, false };
    int on = 0;

    ASSERT(!meter_beep_pulse_tick(&p));                 /* idle stays idle */
    meter_beep_pulse_edge(&p);
    ASSERT(p.active);
    while (meter_beep_pulse_tick(&p)) {
        on++;
        ASSERT(on < 100);
    }
    ASSERT(on == (int)METER_BEEP_PULSE_TICKS - 1);      /* 10 sounding ticks, off on the 11th */
    ASSERT(!p.active);
    return 1;
}

static int test_beep_edge_retriggers(void)
{
    meter_beep_pulse_t p = { 0, false };
    meter_beep_pulse_edge(&p);
    for (int i = 0; i < 8; i++) ASSERT(meter_beep_pulse_tick(&p));
    meter_beep_pulse_edge(&p);                          /* edge near the end restarts it */
    for (int i = 0; i < 10; i++) ASSERT(meter_beep_pulse_tick(&p));
    ASSERT(!meter_beep_pulse_tick(&p));
    return 1;
}

/* ── calibration-wizard guard ───────────────────────────────────────── */

static int test_cal_wizard_words_are_refused(void)
{
    ASSERT(meter_word_is_cal_wizard(0x02A0));
    ASSERT(meter_word_is_cal_wizard(0x0503));
    ASSERT(meter_word_is_cal_wizard(0x0501));
    /* every real function word, Auto and the stock polling words pass */
    static const uint16_t ok[] = { 0x0514, 0x050C, 0x050D, 0x0517, 0x050E, 0x050B,
                                   0x050A, 0x0512, 0x0511, 0x0516, 0x0510, 0x0515,
                                   0x0508, 0x0509, 0x0000, 0x0502, 0x0500 };
    for (unsigned i = 0; i < sizeof ok / sizeof ok[0]; i++)
        ASSERT(!meter_word_is_cal_wizard(ok[i]));
    return 1;
}


/* ── Auto (M2): stock's selector-0 state machine, 0x0803E1F2 ─────────── */

static meter_auto_fsm_t fsm_new(void)
{
    meter_auto_fsm_t st = { 0, 0, 0 };
    meter_auto_fsm_reset(&st);
    return st;
}

static int test_auto_fsm_entry_and_undecided(void)
{
    meter_auto_fsm_t st = fsm_new();
    ASSERT(st.f2f == 0 && st.f36 == 1);                 /* 0x0800E3C2, 0x0800CBD6 */
    /* F2F == 0: the case does nothing, whatever the bits say (0x0803E1F6) */
    ASSERT(meter_auto_fsm_step(&st, false, 0x02, 0x00, 0x20) == METER_AUTO_FN_NONE);
    ASSERT(st.f2f == 0 && st.f36 == 1);
    /* the AUTO glyph run: F2F = 0, F36 = 0 */
    st.f2f = 1; st.f36 = 2;
    meter_auto_fsm_auto_text(&st);
    ASSERT(st.f2f == 0 && st.f36 == 0);
    ASSERT(meter_auto_fsm_function(&st) == METER_AUTO_FN_NONE);
    /* a numeric frame presets F2F = 0xFF and decides */
    ASSERT(meter_auto_fsm_step(&st, true, 0x02, 0x00, 0x00) == METER_AUTO_FN_DCV);
    return 1;
}

static int test_auto_fsm_decisions(void)
{
    meter_auto_fsm_t st;

    st = fsm_new();      /* DC voltage: rx[2].1 (0x0803E328) */
    ASSERT(meter_auto_fsm_step(&st, true, 0x02, 0x00, 0x00) == METER_AUTO_FN_DCV);
    ASSERT(st.f2f == 1 && st.f36 == 1 && st.f37 == 1);
    ASSERT(meter_auto_fsm_step(&st, true, 0x02, 0x00, 0x01) == METER_AUTO_FN_DCV);
    ASSERT(st.f37 == 0);                                 /* rx[7].0: the mV range */
    ASSERT(!meter_auto_fsm_hz_valid(&st));

    st = fsm_new();      /* AC voltage: rx[2].0 (0x0803E338) */
    ASSERT(meter_auto_fsm_step(&st, true, 0x01, 0x00, 0x00) == METER_AUTO_FN_ACV);
    ASSERT(st.f2f == 1 && st.f36 == 2 && st.f37 == 1);
    ASSERT(meter_auto_fsm_hz_valid(&st));                /* Hz only here */

    st = fsm_new();      /* resistance: rx[7].5 (0x0803E2E0) */
    ASSERT(meter_auto_fsm_step(&st, true, 0x00, 0x00, 0x20) == METER_AUTO_FN_OHM);
    ASSERT(st.f2f == 3 && st.f36 == 0 && st.f37 == 0);
    ASSERT(meter_auto_fsm_step(&st, true, 0x00, 0x40, 0x20) == METER_AUTO_FN_OHM);
    ASSERT(st.f37 == 1);                                 /* rx[6].6: kOhm */
    ASSERT(meter_auto_fsm_step(&st, true, 0x00, 0x40, 0x24) == METER_AUTO_FN_OHM);
    ASSERT(st.f37 == 2);                                 /* rx[7].2 beats rx[6].6 */
    ASSERT(!meter_auto_fsm_hz_valid(&st));

    st = fsm_new();      /* continuity: rx[7].3 and no earlier bit (0x0803E3A8) */
    ASSERT(meter_auto_fsm_step(&st, true, 0x00, 0x00, 0x08) == METER_AUTO_FN_CONT);
    ASSERT(st.f2f == 2 && st.f36 == 2 && st.f37 == 0);
    ASSERT(meter_auto_fsm_step(&st, true, 0x00, 0x40, 0x08) == METER_AUTO_FN_CONT);
    ASSERT(st.f37 == 1);
    ASSERT(!meter_auto_fsm_hz_valid(&st));               /* F36 == 2 but F2F == 2 */

    st = fsm_new();      /* nothing recognised: F36 = 0, no function */
    ASSERT(meter_auto_fsm_step(&st, true, 0x00, 0x00, 0x00) == METER_AUTO_FN_NONE);
    ASSERT(st.f36 == 0 && st.f2f == 0xFF);
    return 1;
}

static int test_auto_fsm_priority_order(void)
{
    meter_auto_fsm_t st;
    /* resistance beats DC beats AC beats continuity, as the branch order */
    st = fsm_new();
    ASSERT(meter_auto_fsm_step(&st, true, 0x03, 0x00, 0x28) == METER_AUTO_FN_OHM);
    st = fsm_new();
    ASSERT(meter_auto_fsm_step(&st, true, 0x03, 0x00, 0x08) == METER_AUTO_FN_DCV);
    st = fsm_new();
    ASSERT(meter_auto_fsm_step(&st, true, 0x01, 0x00, 0x08) == METER_AUTO_FN_ACV);
    return 1;
}

static int test_auto_fsm_status_frames_keep_the_decision(void)
{
    /* Status text paths (OL, blank, PAS) jump past the F2F = 0xFF preset
     * (0x0803DCCC..), so a decided state is decided again from the same
     * bits; an undecided one (F2F == 0 after "AUTO") stays undecided. */
    meter_auto_fsm_t st = fsm_new();
    ASSERT(meter_auto_fsm_step(&st, true, 0x00, 0x00, 0x20) == METER_AUTO_FN_OHM);
    ASSERT(meter_auto_fsm_step(&st, false, 0x00, 0x00, 0x20) == METER_AUTO_FN_OHM);
    ASSERT(st.f2f == 3);
    /* the bits go away: F36 = 0 but F2F is not touched */
    ASSERT(meter_auto_fsm_step(&st, false, 0x00, 0x00, 0x00) == METER_AUTO_FN_OHM);
    meter_auto_fsm_auto_text(&st);
    ASSERT(meter_auto_fsm_step(&st, false, 0x00, 0x00, 0x20) == METER_AUTO_FN_NONE);
    ASSERT(meter_auto_fn_submode(METER_AUTO_FN_DCV) == 0);
    ASSERT(meter_auto_fn_submode(METER_AUTO_FN_ACV) == 1);
    ASSERT(meter_auto_fn_submode(METER_AUTO_FN_OHM) == 6);
    ASSERT(meter_auto_fn_submode(METER_AUTO_FN_CONT) == 7);
    ASSERT(meter_auto_fn_submode(METER_AUTO_FN_NONE) == 0xFF);
    return 1;
}

/* Real frames (unit #2, test_meter_data.c) fed to submode 11. */
static const uint8_t AUTO_DCV[12] = { 0x5A, 0xA5, 0xC6, 0xF7, 0xEB, 0xEB, 0x0F, 0x00, 0x02, 0x00, 0x01, 0x4E };
static const uint8_t AUTO_ACV[12] = { 0x5A, 0xA5, 0xA5, 0xAD, 0xED, 0xF7, 0x07, 0x00, 0x02, 0x00, 0x00, 0x32 };
static const uint8_t AUTO_OHM[12] = { 0x5A, 0xA5, 0x04, 0xE0, 0x9B, 0xEA, 0x07, 0x28, 0x00, 0x00, 0x01, 0x4B };

static int test_auto_reading_dcv(void)
{
    meter_data_init();
    feed(AUTO_DCV, 11);
    ASSERT(meter_reading.valid);
    ASSERT(meter_reading.submode == 11);                 /* the UI's submode, not the function */
    ASSERT(meter_reading.auto_fn == METER_AUTO_FN_DCV);
    ASSERT(meter_reading.result_class == METER_RESULT_NORMAL);
    ASSERT_STR(meter_reading.display_str, "5.008");
    ASSERT_STR(meter_reading.unit_suffix, "V");
    ASSERT(close_to(meter_reading.value, 5.008f, 0.001f));
    ASSERT(meter_reading.expected_frame_family == FPGA_METER_FRAME_FAMILY_VOLTAGE);
    ASSERT(meter_reading.range_index == 1);
    ASSERT_STR(meter_reading.range_label, "V");
    return 1;
}

static int test_auto_reading_acv_needs_no_mains_window(void)
{
    uint8_t quiet[12];

    meter_data_init();
    feed(AUTO_ACV, 11);
    ASSERT(meter_reading.valid);
    ASSERT(meter_reading.auto_fn == METER_AUTO_FN_ACV);
    ASSERT_STR(meter_reading.unit_suffix, "V");
    ASSERT(meter_reading.value > 200.0f && meter_reading.value < 260.0f);
    ASSERT(close_to(meter_reading.aux_freq_hz, 50.0f, 0.1f));

    /* 400 Hz: outside the mains window the fixed ACV function insists on.
     * Auto decided AC from the frame's own bit and prints rx[10..11] as Hz. */
    memcpy(quiet, AUTO_ACV, 12);
    quiet[10] = 0x01; quiet[11] = 0x90;
    meter_data_init();
    feed(quiet, 1);
    ASSERT(!meter_reading.valid);                        /* fixed ACV rejects it */
    meter_data_init();
    feed(quiet, 11);
    ASSERT(meter_reading.valid);
    ASSERT(meter_reading.auto_fn == METER_AUTO_FN_ACV);
    ASSERT(close_to(meter_reading.aux_freq_hz, 400.0f, 0.1f));
    return 1;
}

static int test_auto_reading_ohm_and_the_family_gate(void)
{
    uint8_t marked[12];

    meter_data_init();
    feed(AUTO_OHM, 11);
    ASSERT(meter_reading.valid);
    ASSERT(meter_reading.submode == 11 && meter_reading.auto_fn == METER_AUTO_FN_OHM);
    ASSERT_STR(meter_reading.unit_suffix, "Ohm");
    ASSERT(meter_reading.expected_frame_family == FPGA_METER_FRAME_FAMILY_RESISTANCE);

    /* The family is the function the frame decided on: a resistance frame
     * that carries the voltage payload marker is still a foreign frame. */
    memcpy(marked, AUTO_OHM, 12);
    marked[8] = 0x02;
    meter_data_init();
    feed(marked, 11);
    ASSERT(!meter_reading.valid);
    ASSERT(meter_reading.reject_reason == METER_REJECT_WRONG_FRAME_FAMILY);
    return 1;
}

static int test_auto_reading_continuity_shows_pas_and_beeps(void)
{
    uint8_t f[12];
    const uint8_t pas[4] = { 16, 18, 10, 5 };

    /* PAS is a status string: stock decides nothing from it while F2F is 0
     * (right after "AUTO"), so the first numeric frame decides continuity. */
    meter_data_init();
    glyph_frame(f, pas, 0, 0x08);
    feed(f, 11);
    ASSERT(meter_reading.valid && meter_reading.auto_fn == METER_AUTO_FN_NONE);
    ASSERT_STR(meter_reading.display_str, "PAS");        /* still the text */
    {
        const uint8_t n[4] = { 0, 0, 1, 7 };
        glyph_frame(f, n, 0, 0x08);
        feed(f, 11);
        ASSERT(meter_reading.auto_fn == METER_AUTO_FN_CONT);
    }
    glyph_frame(f, pas, 0, 0x08);
    feed(f, 11);
    ASSERT(meter_reading.valid);
    ASSERT(meter_reading.auto_fn == METER_AUTO_FN_CONT);
    ASSERT(meter_reading.result_class == METER_RESULT_CONTINUITY);
    ASSERT(meter_reading.continuity_beep);
    ASSERT(meter_reading.status_code == METER_STATUS_PAS);
    ASSERT_STR(meter_reading.display_str, "PAS");        /* M5: stock's own text */
    /* and in the fixed continuity function */
    meter_data_init();
    glyph_frame(f, pas, 0, 0x08);
    feed(f, 7);
    ASSERT_STR(meter_reading.display_str, "PAS");
    ASSERT(meter_reading.continuity_beep);
    return 1;
}

static int test_auto_reading_undecided_is_not_a_number(void)
{
    uint8_t f[12];
    const uint8_t autot[4] = { 10, 11, 12, 13 };
    const uint8_t num[4]   = { 1, 2, 3, 4 };

    meter_data_init();
    glyph_frame(f, autot, 0, 0);
    feed(f, 11);
    ASSERT(meter_reading.valid);
    ASSERT_STR(meter_reading.display_str, "AUTO");
    ASSERT(meter_reading.auto_fn == METER_AUTO_FN_NONE);

    /* a number whose bits name no function: shown as nothing */
    glyph_frame(f, num, 0, 0);
    feed(f, 11);
    ASSERT(!meter_reading.valid);
    ASSERT(meter_reading.reject_reason == METER_REJECT_WRONG_FRAME_FAMILY);
    ASSERT_STR(meter_reading.display_str, "---");
    ASSERT(meter_reading.range_label[0] == '\0');

    /* the same frame in a fixed function is decided by the MCU, not the SoC */
    meter_data_init();
    feed(AUTO_DCV, 11);
    ASSERT(meter_reading.valid);
    /* Auto -> a new function word resets the decision */
    meter_data_invalidate(11);
    ASSERT(!meter_reading.valid && meter_reading.auto_fn == 0);
    glyph_frame(f, autot, 0, 0);
    feed(f, 11);
    feed(AUTO_OHM, 11);                                  /* decides again after "AUTO" */
    ASSERT(meter_reading.valid && meter_reading.auto_fn == METER_AUTO_FN_OHM);
    return 1;
}

static int test_auto_word_and_beep_gate(void)
{
    ASSERT(fpga_meter_stock_cmd_word_for_submode(11) == 0x0514);
    ASSERT(meter_stock_beep_gate_for_submode(11));       /* beeps in Auto (checker) */
    ASSERT(!meter_word_is_cal_wizard(0x0514));
    return 1;
}

int main(void)
{
    printf("meter_stock tests\n");
    TEST(status_vectors);
    TEST(status_text_reaches_the_reading);
    TEST(degf_conversion);
    TEST(degf_toggle_in_parser);
    TEST(range_index_vectors);
    TEST(range_label_and_reading);
    TEST(selector_for_submode);
    TEST(rescale);
    TEST(format_fixed);
    TEST(beep_gate_only_auto_and_continuity);
    TEST(beep_pulse_is_fixed_length);
    TEST(beep_edge_retriggers);
    TEST(cal_wizard_words_are_refused);
    TEST(auto_fsm_entry_and_undecided);
    TEST(auto_fsm_decisions);
    TEST(auto_fsm_priority_order);
    TEST(auto_fsm_status_frames_keep_the_decision);
    TEST(auto_reading_dcv);
    TEST(auto_reading_acv_needs_no_mains_window);
    TEST(auto_reading_ohm_and_the_family_gate);
    TEST(auto_reading_continuity_shows_pas_and_beeps);
    TEST(auto_reading_undecided_is_not_a_number);
    TEST(auto_word_and_beep_gate);
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
