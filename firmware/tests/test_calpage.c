/*
 * Host tests for the stock calibration page decoder (src/util/calpage.c) and
 * its hand-off to the vertical scale (src/ui/scope_cal.c). Parity item C4.
 *
 * The numbers asserted here are not invented: every "expect" code below was
 * read off a live stock V1.2.0 over SWD on bench unit #2 (2026-11-11 session,
 * RESULTS.md S2/S3) for the same page bytes as tests/fixtures/calpage_unit2.h,
 * or is the bank-1 value the same formula gives where stock used a corrupt
 * entry. Unit #1's page (src/util/cal_reference.c, already in the tree) is the
 * clean control, and synthetic corruptions of it prove the rules fire on a
 * page other than the one they were written against.
 */
#include "../src/util/calpage.h"
#include "../src/ui/scope_cal.h"
#include "fixtures/calpage_unit2.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

extern const uint8_t cal_reference_page[4096];   /* unit #1, src/util/cal_reference.c */

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

static uint16_t code_of(const calpage_t *p, uint8_t ch, uint8_t bank, uint8_t r, int pos)
{
    uint16_t c = 0xFFFFu;
    CHECK(calpage_dac_code(p, ch, bank, r, pos, &c),
          "ch%u b%u r%u pos %d: expected a code", ch + 1u, bank, r, pos);
    return c;
}

/* Write one endpoint into a page copy (ch 0/1, ep 0 base/1 upper). */
static void poke(uint8_t *page, uint8_t ch, uint8_t ep, uint8_t bank, uint8_t r, uint16_t v)
{
    const unsigned k = ep * 30u + bank * 10u + r;
    uint8_t *w = page + 0x38u + 4u * k + (ch ? 2u : 0u);
    w[0] = (uint8_t)v;
    w[1] = (uint8_t)(v >> 8);
}

/* ── unit #2: the corrupt page, against stock's live outputs ─────────── */

static void test_unit2_decode(void)
{
    calpage_t p;
    calpage_parse(calpage_unit2, sizeof(calpage_unit2), &p);
    CHECK(p.status == CALPAGE_ST_OK, "unit2 status %d", (int)p.status);
    CHECK(p.signature == 0x55u && p.sentinel == 0x0C61u, "sig %02X sentinel %04X",
          p.signature, p.sentinel);
    CHECK(p.m[0] == 183u && p.m[1] == 181u, "m %u/%u (page 0x12C/0x12D)", p.m[0], p.m[1]);
    CHECK(p.trim[0] == -1 && p.trim[1] == 0, "trims %d/%d", p.trim[0], p.trim[1]);
    CHECK(p.hdr_pos[0] == -49 && p.hdr_pos[1] == -50 && p.hdr_tb == 5u && p.hdr_mask == 3u,
          "header pos %d/%d tb %u mask %u", p.hdr_pos[0], p.hdr_pos[1], p.hdr_tb, p.hdr_mask);
    CHECK(p.raw[0][1][0][0] == 0xDC5Bu && p.raw[0][1][0][1] == 0x3C59u,
          "CH1 upper b0 r0/r1 decode: %04X %04X", p.raw[0][1][0][0], p.raw[0][1][0][1]);
    CHECK(p.raw[1][0][0][0] == 1736u && p.raw[0][0][0][2] == 1762u, "CH2 base b0 r0 / CH1 base b0 r2");

    /* BAD: exactly the two > 4095 entries; never "trusted" via the 12-bit cut. */
    CHECK(p.flags[0][1][0][0] & CALPAGE_F_RANGE, "0xDC5B must be BAD");
    CHECK(p.flags[0][1][0][1] & CALPAGE_F_RANGE, "0x3C59 must be BAD");
    /* The CH2 bank-0 anomalies have in-family spans; only the cross-bank rule sees them. */
    CHECK(p.flags[1][0][0][0] & CALPAGE_F_XBANK, "CH2 base b0 r0 1736 cross-bank");
    CHECK(p.flags[1][0][0][1] & CALPAGE_F_XBANK, "CH2 base b0 r1 1539 cross-bank");
    CHECK(p.flags[1][0][0][2] & CALPAGE_F_XBANK, "CH2 base b0 r2 1627 cross-bank");
    CHECK(p.flags[1][1][0][0] & CALPAGE_F_XBANK, "CH2 upper b0 r0 3278 cross-bank");
    CHECK(p.flags[0][0][0][2] & (CALPAGE_F_XBANK | CALPAGE_F_SPAN), "CH1 base b0 r2 1762");

    /* Exactly the six bank-0 pairs r0..r2 x 2 channels are substituted, all
     * from bank 1; everything else is served by its own bank. */
    CHECK(p.n_substituted == 6u, "substituted %u, want 6", p.n_substituted);
    CHECK(p.n_unusable == 0u, "unusable %u", p.n_unusable);
    CHECK(p.n_clean_pairs == 54u, "clean pairs %u, want 54", p.n_clean_pairs);
    for (uint8_t c = 0; c < 2u; c++)
        for (uint8_t b = 0; b < 3u; b++)
            for (uint8_t r = 0; r < 10u; r++) {
                const int want = (b == 0u && r <= 2u) ? 1 : (int)b;
                CHECK(p.src_bank[c][b][r] == want, "src_bank ch%u b%u r%u = %d, want %d",
                      c + 1u, b, r, p.src_bank[c][b][r], want);
            }
}

static void test_unit2_live_codes(void)
{
    calpage_t p;
    calpage_parse(calpage_unit2, sizeof(calpage_unit2), &p);

    /* Clean entries: ours == stock's live readback (bank 0 = tb 0x05). */
    CHECK(code_of(&p, 0, 0, 9, -48) == 1980u, "CH1 r9 pos -48 (live 1980)");
    CHECK(code_of(&p, 0, 0, 9, -58) == 1901u, "CH1 r9 pos -58 (live 1901)");
    CHECK(code_of(&p, 1, 0, 6, -44) == 2033u, "CH2 r6 pos -44 (live 2033)");
    /* S2: tb 0x03, mask 1 -> bank 2; CH1 r6 pos -49 read 1964 live. */
    CHECK(calpage_bank_select(3u, 1u) == 2u, "tb3 mask1 -> bank 2");
    CHECK(code_of(&p, 0, 2, 6, -49) == 1964u, "CH1 b2 r6 pos -49 (live 1964)");
    /* Truncation, not rounding: 1980.8 -> 1980 (S3). */
    CHECK(calpage_stock_formula(1570u, 3150u, -48) == 1980u, "stock truncates");

    /* The corrupt entries: our formula reproduces stock's live outputs on the
     * stored values... */
    const uint32_t st = calpage_stock_formula(p.raw[0][0][0][0], p.raw[0][1][0][0], -62);
    CHECK((st & 0x0FFFu) == 3812u, "stock CH1 r0 pos -62 = 12004 & 0xFFF = 3812, got %lu",
          (unsigned long)(st & 0x0FFFu));
    CHECK(calpage_stock_formula(p.raw[1][0][0][0], p.raw[1][1][0][0], -62) == 2028u,
          "stock CH2 r0 pos -62 (live 2028)");
    CHECK(calpage_stock_formula(p.raw[1][0][0][0], p.raw[1][1][0][0], -70) == 1967u,
          "stock CH2 r0 pos -70 (live 1967)");
    /* ...and we do not copy them: bank 1's clean pair is used instead. */
    const uint16_t ch1r0 = code_of(&p, 0, 0, 0, -62);
    CHECK(ch1r0 != 3812u, "CH1 r0 bank 0 must NOT produce 3812");
    CHECK(ch1r0 == calpage_stock_formula(1561u, 3148u, -62) && ch1r0 == 1862u,
          "CH1 r0 b0 pos -62 -> bank-1 pair -> 1862, got %u", ch1r0);
    CHECK(code_of(&p, 1, 0, 0, -62) == 1919u, "CH2 r0 b0 pos -62 -> bank 1 -> 1919");
    CHECK(code_of(&p, 0, 0, 1, -49) == calpage_stock_formula(1568u, 3149u, -49),
          "CH1 r1 b0 -> bank 1 (stock: 1014)");
    CHECK(code_of(&p, 0, 0, 2, -49) == calpage_stock_formula(1571u, 3153u, -49),
          "CH1 r2 b0 -> bank 1 (stock: 2118)");

    /* Never > 4095 anywhere, at any position, through either entry point. */
    for (uint8_t c = 0; c < 2u; c++)
        for (uint8_t b = 0; b < 3u; b++)
            for (uint8_t r = 0; r < 10u; r++)
                for (int pos = -120; pos <= 120; pos += 5) {
                    uint16_t a = 0, d = 0;
                    const bool oka = calpage_dac_code(&p, c, b, r, pos, &a);
                    const bool okd = calpage_dac_code_page(calpage_unit2,
                                         (const int8_t (*)[CALPAGE_BANKS][CALPAGE_RANGES])p.src_bank,
                                         c, b, r, pos, &d);
                    CHECK(oka && okd && a == d && a <= 4095u && a > 0u,
                          "ch%u b%u r%u pos %d: %u vs page path %u", c + 1u, b, r, pos, a, d);
                }
}

static void test_bank_rule_and_pos(void)
{
    /* Stock 0x080089C2: tb >= 5 -> 0; tb == 4 or mask 3 -> 1; else 2. */
    CHECK(calpage_bank_select(5u, 1u) == 0u && calpage_bank_select(0x14u, 3u) == 0u, ">=5");
    CHECK(calpage_bank_select(4u, 1u) == 1u && calpage_bank_select(4u, 2u) == 1u, "==4");
    CHECK(calpage_bank_select(3u, 3u) == 1u && calpage_bank_select(0u, 3u) == 1u, "dual fast");
    CHECK(calpage_bank_select(3u, 2u) == 2u && calpage_bank_select(0u, 1u) == 2u, "single fast");
    CHECK(calpage_clamp_pos(150) == 100 && calpage_clamp_pos(-101) == -100 &&
          calpage_clamp_pos(-48) == -48, "clamp +-100 (0x08008F32)");
    /* The endpoints are the positions +-100 exactly. */
    CHECK(calpage_stock_formula(1561u, 3148u, -100) == 1561u, "pos -100 = base");
    CHECK(calpage_stock_formula(1561u, 3148u, 100) == 3148u, "pos +100 = upper");
    CHECK(calpage_stock_formula(1561u, 3148u, 999) == 3148u, "pos clamps");
}

/* ── unit #1: clean control, and synthetic corruptions of it ─────────── */

static void test_unit1_clean(void)
{
    calpage_t p;
    calpage_parse(cal_reference_page, 4096u, &p);
    CHECK(p.status == CALPAGE_ST_OK, "unit1 status %d", (int)p.status);
    CHECK(p.sentinel == 0x0CD9u && p.m[0] == 184u && p.m[1] == 182u, "unit1 sentinel/m");
    CHECK(p.n_clean_pairs == 60u && p.n_substituted == 0u && p.n_unusable == 0u,
          "unit1 must be entirely clean: clean %u subst %u", p.n_clean_pairs, p.n_substituted);
    CHECK(fabsf(calpage_gain(&p, 0) - 156.0f / 150.0f) < 1e-6f, "unit1 g1 1.04");
}

static void test_synthetic_corruption(void)
{
    uint8_t pg[CALPAGE_LEN];
    calpage_t p;

    /* > 4095 on a clean page: substituted from bank 1, not cut to 12 bits. */
    memcpy(pg, cal_reference_page, sizeof(pg));
    poke(pg, 0, 1, 0, 0, 0xDC5Bu);
    calpage_parse(pg, sizeof(pg), &p);
    CHECK(p.status == CALPAGE_ST_OK && p.src_bank[0][0][0] == 1, "bad b0 -> b1");
    CHECK(code_of(&p, 0, 0, 0, 0) == calpage_stock_formula(p.raw[0][0][1][0], p.raw[0][1][1][0], 0),
          "uses bank 1's pair");
    CHECK(code_of(&p, 0, 0, 0, 0) != (calpage_stock_formula(p.raw[0][0][0][0], 0xDC5Bu, 0) & 0xFFFu),
          "not stock's cut value");

    /* An in-family-span outlier (+117, the CH2 b0 r0 shape) is caught by the
     * cross-bank rule alone. Shift both endpoints so the span stays right. */
    memcpy(pg, cal_reference_page, sizeof(pg));
    poke(pg, 1, 0, 0, 3, (uint16_t)(calpage_entry(pg, 1, 0, 0, 3) + 117u));
    poke(pg, 1, 1, 0, 3, (uint16_t)(calpage_entry(pg, 1, 1, 0, 3) + 117u));
    calpage_parse(pg, sizeof(pg), &p);
    CHECK(!(p.flags[1][0][0][3] & CALPAGE_F_SPAN), "span still in family");
    CHECK((p.flags[1][0][0][3] & CALPAGE_F_XBANK) && p.src_bank[1][0][3] == 1, "cross-bank -> b1");

    /* Bad bank 1: substitute 2 first (the other fast bank), then 0. */
    memcpy(pg, cal_reference_page, sizeof(pg));
    poke(pg, 0, 0, 1, 5, 0u);
    calpage_parse(pg, sizeof(pg), &p);
    CHECK(p.src_bank[0][1][5] == 2 && p.src_bank[0][0][5] == 0 && p.src_bank[0][2][5] == 2,
          "b1 -> b2; neighbours unaffected");

    /* No clean bank at all for one range: no code, the caller keeps its own. */
    memcpy(pg, cal_reference_page, sizeof(pg));
    for (uint8_t b = 0; b < 3u; b++) poke(pg, 1, 1, b, 7, 0xFFFFu);
    calpage_parse(pg, sizeof(pg), &p);
    uint16_t c = 1234u;
    CHECK(p.status == CALPAGE_ST_OK && p.n_unusable == 3u, "3 unusable pairs");
    CHECK(!calpage_dac_code(&p, 1, 0, 7, 0, &c) && c == 1234u, "no code, output untouched");
    CHECK(calpage_dac_code(&p, 0, 0, 7, 0, &c), "CH1 r7 unaffected");
}

static void test_whole_page_rejections(void)
{
    uint8_t pg[CALPAGE_LEN];
    calpage_t p;
    uint16_t c;

    memset(pg, 0xFF, sizeof(pg));
    calpage_parse(pg, sizeof(pg), &p);
    CHECK(p.status == CALPAGE_ST_SIGNATURE, "erased page");
    CHECK(!calpage_dac_code(&p, 0, 0, 6, 0, &c) && calpage_gain(&p, 0) == 0.0f, "erased: nothing");

    calpage_parse(cal_reference_page, CALPAGE_LEN - 1u, &p);
    CHECK(p.status == CALPAGE_ST_SHORT, "short");
    calpage_parse(NULL, 4096u, &p);
    CHECK(p.status == CALPAGE_ST_NONE, "null");

    memcpy(pg, cal_reference_page, sizeof(pg));
    pg[0] = 0xAAu;   /* "settings reset, calibration kept": still used */
    calpage_parse(pg, sizeof(pg), &p);
    CHECK(p.status == CALPAGE_ST_OK, "0xAA is valid");
    pg[0] = 0x00u;
    calpage_parse(pg, sizeof(pg), &p);
    CHECK(p.status == CALPAGE_ST_SIGNATURE, "bad signature");

    memcpy(pg, cal_reference_page, sizeof(pg));
    poke(pg, 1, 1, 2, 9, 0xFFFFu);   /* sentinel ms[0x34E] */
    calpage_parse(pg, sizeof(pg), &p);
    CHECK(p.status == CALPAGE_ST_SENTINEL, "sentinel FFFF");

    /* V1.0.3-style content: the same words hold small signed trims. */
    memcpy(pg, cal_reference_page, sizeof(pg));
    for (uint8_t ch = 0; ch < 2u; ch++)
        for (uint8_t e = 0; e < 2u; e++)
            for (uint8_t b = 0; b < 3u; b++)
                for (uint8_t r = 0; r < 10u; r++)
                    poke(pg, ch, e, b, r, (uint16_t)(int16_t)(r * 7 - 30));
    poke(pg, 1, 1, 2, 9, 0x0123u);   /* keep a non-blank sentinel */
    calpage_parse(pg, sizeof(pg), &p);
    CHECK(p.status == CALPAGE_ST_IMPLAUSIBLE, "trim-shaped page rejected (%d)", (int)p.status);
    CHECK(calpage_gain(&p, 0) == 0.0f && !calpage_dac_code(&p, 0, 0, 6, 0, &c),
          "rejected page: no gain, no codes");
}

static void test_gain(void)
{
    uint8_t pg[CALPAGE_LEN];
    calpage_t p;
    calpage_parse(calpage_unit2, sizeof(calpage_unit2), &p);
    CHECK(fabsf(calpage_gain(&p, 0) - 155.0f / 150.0f) < 1e-6f, "g1 = (183-28)/150");
    CHECK(fabsf(calpage_gain(&p, 1) - 153.0f / 150.0f) < 1e-6f, "g2 = (181-28)/150");
    CHECK(calpage_gain(&p, 2) == 0.0f, "bad channel");

    memcpy(pg, calpage_unit2, sizeof(pg));
    const uint8_t bad[] = { 0x00u, 0xFFu, 28u, 147u, 217u };
    for (unsigned i = 0; i < sizeof(bad); i++) {
        pg[0x12C] = bad[i];
        calpage_parse(pg, sizeof(pg), &p);
        CHECK(calpage_gain(&p, 0) == 0.0f, "m=%u must give no gain", bad[i]);
        CHECK(calpage_gain(&p, 1) > 0.0f, "CH2 unaffected by CH1's m");
    }
}

/* ── hand-off to the vertical scale ────────────────────────────────────── */

static void test_scope_cal_handoff(void)
{
    /* Negative control first: with no page gain the compiled table is
     * untouched (exactly table x 0.92, ranges 0-3 exactly 0). */
    const float before_r6 = scope_cal_mv_per_count(1u, 6u);
    CHECK(scope_cal_mv_per_count(1u, 0u) == 0.0f && scope_cal_get_tier(1u, 0u) == SCOPE_CAL_NONE,
          "no page: r0 NONE");
    CHECK(fabsf(before_r6 - 42.95f * SCOPE_CAL_SOURCE_SCALE) < 1e-4f, "no page: r6 compiled");

    calpage_t p;
    calpage_parse(calpage_unit2, sizeof(calpage_unit2), &p);
    scope_cal_set_stock_gain(1u, calpage_gain(&p, 0));
    static const float nominal[10] = { 0.4f, 0.8f, 2, 4, 8, 20, 40, 80, 200, 400 };
    for (uint8_t r = 0; r < 10u; r++) {
        const float want = nominal[r] / (155.0f / 150.0f);
        CHECK(fabsf(scope_cal_mv_per_count(1u, r) - want) < want * 1e-5f,
              "CH1 r%u: %f mV/count, want nominal/g %f (no source scale)", r,
              (double)scope_cal_mv_per_count(1u, r), (double)want);
        CHECK(scope_cal_get_tier(1u, r) == SCOPE_CAL_FACTORY, "CH1 r%u tier FACTORY", r);
        CHECK(scope_cal_true_scale_ok(1u, r), "CH1 r%u has a volts/div now", r);
    }
    CHECK(fabsf(scope_cal_mv_per_count(2u, 6u) - 41.71f * SCOPE_CAL_SOURCE_SCALE) < 1e-4f,
          "CH2 stays on the compiled table until it gets its own g");
    CHECK(scope_cal_get_tier(2u, 0u) == SCOPE_CAL_NONE, "CH2 r0 still NONE");

    scope_cal_set_stock_gain(1u, 0.0f);
    CHECK(scope_cal_mv_per_count(1u, 6u) == before_r6 && scope_cal_get_tier(1u, 0u) == SCOPE_CAL_NONE,
          "clearing g restores the compiled table bit-for-bit");
    scope_cal_set_stock_gain(1u, -1.0f);
    CHECK(scope_cal_stock_gain(1u) == 0.0f, "negative g refused");
}

int main(void)
{
    test_unit2_decode();
    test_unit2_live_codes();
    test_bank_rule_and_pos();
    test_unit1_clean();
    test_synthetic_corruption();
    test_whole_page_rejections();
    test_gain();
    test_scope_cal_handoff();

    if (failures == 0) {
        printf("test_calpage: all checks passed\n");
        return 0;
    }
    printf("test_calpage: %d check(s) FAILED\n", failures);
    return 1;
}
