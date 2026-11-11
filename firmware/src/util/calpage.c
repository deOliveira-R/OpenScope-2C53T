/*
 * calpage.c — stock calibration page decode, validation, bank substitution
 * and the offset-DAC formula. See calpage.h for the layout, the stock
 * addresses and why entries are validated rather than used as stored.
 *
 * No HAL, no RTOS, no flash pointer: the caller copies the page into RAM
 * first, so this builds and runs on the host (tests/test_calpage.c).
 */
#include "calpage.h"

#include <string.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static bool in_dac_range(uint16_t v) { return v != 0u && v != 0xFFFFu && v <= 4095u; }

/* Median of up to three values (statistics.median semantics: the mean of the
 * middle two for an even count), as the research importer computes it. */
static float median_of(const uint16_t *v, unsigned n)
{
    uint16_t s[3];
    for (unsigned i = 0; i < n; i++) s[i] = v[i];
    for (unsigned i = 1; i < n; i++)
        for (unsigned j = i; j > 0 && s[j - 1] > s[j]; j--) {
            uint16_t t = s[j]; s[j] = s[j - 1]; s[j - 1] = t;
        }
    if (n == 3u) return (float)s[1];
    return ((float)s[0] + (float)s[1]) / 2.0f;
}

static void validate(calpage_t *p)
{
    /* 1. Range: 0 and 0xFFFF are blank/sentinel values; > 4095 cannot be a
     *    12-bit DAC code. Stock silently keeps CH1's low 12 bits and drives
     *    CH2's TMR13 with the whole value; neither is trusted here. */
    for (unsigned c = 0; c < CALPAGE_CHANNELS; c++)
        for (unsigned e = 0; e < CALPAGE_ENDPOINTS; e++)
            for (unsigned b = 0; b < CALPAGE_BANKS; b++)
                for (unsigned r = 0; r < CALPAGE_RANGES; r++)
                    if (!in_dac_range(p->raw[c][e][b][r]))
                        p->flags[c][e][b][r] |= CALPAGE_F_RANGE;

    /* 2. Span: one position step is (upper - base)/200 DAC codes, set by the
     *    front end's resistor ratios, so the span is nearly constant across
     *    ranges and banks (~1580-1600 on both known units). A pair far off
     *    that is not a calibration. Skipped where an endpoint is blank
     *    (already BAD), as in the importer. */
    for (unsigned c = 0; c < CALPAGE_CHANNELS; c++)
        for (unsigned b = 0; b < CALPAGE_BANKS; b++)
            for (unsigned r = 0; r < CALPAGE_RANGES; r++) {
                const uint16_t lo = p->raw[c][0][b][r], hi = p->raw[c][1][b][r];
                if (lo == 0u || lo == 0xFFFFu || hi == 0u || hi == 0xFFFFu)
                    continue;
                int span = (int)hi - (int)lo - CALPAGE_SPAN_NOM;
                if (span < 0) span = -span;
                if (span > CALPAGE_SPAN_TOL) {
                    p->flags[c][0][b][r] |= CALPAGE_F_SPAN;
                    p->flags[c][1][b][r] |= CALPAGE_F_SPAN;
                }
            }

    /* 3. Cross-bank: the three banks hold the same (ch, endpoint, range)
     *    measured at different timebases and agree within ~20 codes on clean
     *    entries. An entry > 40 from the median of its in-range siblings is an
     *    outlier even when its span looks right. */
    for (unsigned c = 0; c < CALPAGE_CHANNELS; c++)
        for (unsigned e = 0; e < CALPAGE_ENDPOINTS; e++)
            for (unsigned r = 0; r < CALPAGE_RANGES; r++) {
                uint16_t v[3];
                unsigned n = 0;
                for (unsigned b = 0; b < CALPAGE_BANKS; b++)
                    if (in_dac_range(p->raw[c][e][b][r]))
                        v[n++] = p->raw[c][e][b][r];
                if (n < 2u) continue;
                const float med = median_of(v, n);
                for (unsigned b = 0; b < CALPAGE_BANKS; b++) {
                    const uint16_t x = p->raw[c][e][b][r];
                    if (!in_dac_range(x)) continue;
                    float d = (float)x - med;
                    if (d < 0.0f) d = -d;
                    if (d > (float)CALPAGE_XBANK_TOL)
                        p->flags[c][e][b][r] |= CALPAGE_F_XBANK;
                }
            }
}

static bool pair_clean(const calpage_t *p, unsigned c, unsigned b, unsigned r)
{
    return p->flags[c][0][b][r] == 0u && p->flags[c][1][b][r] == 0u;
}

/* Nearest-bank fallback order. Banks 1 and 2 are both fast-timebase banks
 * and track each other closely, so each is the other's first substitute;
 * bank 0 (slow timebases) takes bank 1 first, which is what the research
 * plan (B2) and the live S3 comparison call for. */
static const uint8_t subst_order[CALPAGE_BANKS][CALPAGE_BANKS] = {
    { 0u, 1u, 2u },
    { 1u, 2u, 0u },
    { 2u, 1u, 0u },
};

void calpage_parse(const uint8_t *page, size_t len, calpage_t *out)
{
    memset(out, 0, sizeof(*out));
    memset(out->src_bank, -1, sizeof(out->src_bank));
    out->status = CALPAGE_ST_NONE;
    if (page == NULL) return;
    if (len < CALPAGE_LEN) { out->status = CALPAGE_ST_SHORT; return; }

    out->signature  = page[0x00];
    out->hdr_pos[0] = (int8_t)page[0x05];
    out->hdr_pos[1] = (int8_t)page[0x06];
    out->hdr_mask   = page[0x09];
    out->hdr_tb     = page[0x0D];
    out->trim[0]    = (int8_t)page[0x128];
    out->trim[1]    = (int8_t)page[0x129];
    out->m[0]       = page[0x12C];
    out->m[1]       = page[0x12D];

    for (unsigned k = 0; k < 60u; k++) {
        const uint8_t *w = page + 0x38u + 4u * k;
        const unsigned e = k / 30u, b = (k % 30u) / 10u, r = k % 10u;
        out->raw[0][e][b][r] = rd16(w);       /* low16  -> CH1 (ms[0x260 + 2k]) */
        out->raw[1][e][b][r] = rd16(w + 2);   /* high16 -> CH2 (ms[0x2D8 + 2k]) */
    }
    out->sentinel = out->raw[1][1][2][9];     /* ms[0x34E] */

    if (out->signature != 0x55u && out->signature != 0xAAu) {
        out->status = CALPAGE_ST_SIGNATURE;
        return;
    }
    if (out->sentinel == 0u || out->sentinel == 0xFFFFu) {
        out->status = CALPAGE_ST_SENTINEL;
        return;
    }

    validate(out);

    for (unsigned c = 0; c < CALPAGE_CHANNELS; c++)
        for (unsigned b = 0; b < CALPAGE_BANKS; b++)
            for (unsigned r = 0; r < CALPAGE_RANGES; r++) {
                if (pair_clean(out, c, b, r)) out->n_clean_pairs++;
                for (unsigned i = 0; i < CALPAGE_BANKS; i++) {
                    const uint8_t s = subst_order[b][i];
                    if (pair_clean(out, c, s, r)) {
                        out->src_bank[c][b][r] = (int8_t)s;
                        if (s != b) out->n_substituted++;
                        break;
                    }
                }
                if (out->src_bank[c][b][r] < 0) out->n_unusable++;
            }

    /* A V1.2 page with a few bad entries has ~50+ clean pairs of 60; a
     * V1.0.3-layout page (signed trims in the same words) or random data has
     * almost none. Half is a wide margin between the two. */
    out->status = (out->n_clean_pairs >= 30u) ? CALPAGE_ST_OK : CALPAGE_ST_IMPLAUSIBLE;
}

bool calpage_usable(const calpage_t *p)
{
    return p != NULL && p->status == CALPAGE_ST_OK;
}

uint8_t calpage_bank_select(uint8_t tb, uint8_t mask)
{
    if (tb >= 5u) return 0u;
    if (tb == 4u || mask == 3u) return 1u;
    return 2u;
}

int calpage_clamp_pos(int pos)
{
    if (pos < CALPAGE_POS_MIN) return CALPAGE_POS_MIN;
    if (pos > CALPAGE_POS_MAX) return CALPAGE_POS_MAX;
    return pos;
}

uint32_t calpage_stock_formula(uint16_t base, uint16_t upper, int pos)
{
    pos = calpage_clamp_pos(pos);
    /* Same operation order as stock 0x080089FC..0x08008A3C: integer
     * upper - base (subs), vcvt.f32.s32, vdiv by the 200.0f literal
     * (0x08008A54), vmul by pos + 100, vadd base (vcvt.f32.u32), then
     * vcvt.u32.f32 = truncation, visible in the live S3 data: 1980.8 -> 1980. */
    const float f = (float)((int32_t)upper - (int32_t)base) / 200.0f * (float)(pos + 100)
                    + (float)base;
    if (f <= 0.0f) return 0u;
    return (uint32_t)f;
}

/* Both endpoints of a clean pair are <= 4095, so the interpolation is too;
 * clamp anyway so a wrapped value can never reach the 12-bit writers (which
 * mask, and masking is exactly how stock turned 12004 into 3812). */
static uint16_t dac_from_pair(uint16_t base, uint16_t upper, int pos)
{
    uint32_t v = calpage_stock_formula(base, upper, pos);
    return (uint16_t)(v > 4095u ? 4095u : v);
}

bool calpage_dac_code(const calpage_t *p, uint8_t ch, uint8_t bank,
                      uint8_t range, int pos, uint16_t *code)
{
    if (!calpage_usable(p) || ch >= CALPAGE_CHANNELS || bank >= CALPAGE_BANKS ||
        range >= CALPAGE_RANGES || code == NULL)
        return false;
    const int8_t s = p->src_bank[ch][bank][range];
    if (s < 0) return false;
    *code = dac_from_pair(p->raw[ch][0][s][range], p->raw[ch][1][s][range], pos);
    return true;
}

uint16_t calpage_entry(const uint8_t *page, uint8_t ch, uint8_t ep,
                       uint8_t bank, uint8_t range)
{
    const unsigned k = (unsigned)ep * 30u + (unsigned)bank * 10u + range;
    return rd16(page + 0x38u + 4u * k + (ch ? 2u : 0u));
}

bool calpage_dac_code_page(const uint8_t *page,
                           const int8_t src_bank[CALPAGE_CHANNELS][CALPAGE_BANKS][CALPAGE_RANGES],
                           uint8_t ch, uint8_t bank, uint8_t range, int pos,
                           uint16_t *code)
{
    if (page == NULL || src_bank == NULL || ch >= CALPAGE_CHANNELS ||
        bank >= CALPAGE_BANKS || range >= CALPAGE_RANGES || code == NULL)
        return false;
    const int8_t s = src_bank[ch][bank][range];
    if (s < 0) return false;
    *code = dac_from_pair(calpage_entry(page, ch, 0u, (uint8_t)s, range),
                          calpage_entry(page, ch, 1u, (uint8_t)s, range), pos);
    return true;
}

bool calpage_pair_page(const uint8_t *page,
                       const int8_t src_bank[CALPAGE_CHANNELS][CALPAGE_BANKS][CALPAGE_RANGES],
                       uint8_t ch, uint8_t bank, uint8_t range,
                       uint16_t *base, uint16_t *upper)
{
    if (page == NULL || src_bank == NULL || ch >= CALPAGE_CHANNELS ||
        bank >= CALPAGE_BANKS || range >= CALPAGE_RANGES || base == NULL || upper == NULL)
        return false;
    const int8_t s = src_bank[ch][bank][range];
    if (s < 0) return false;
    *base  = calpage_entry(page, ch, 0u, (uint8_t)s, range);
    *upper = calpage_entry(page, ch, 1u, (uint8_t)s, range);
    return true;
}

uint16_t calpage_dac_from_pair(uint16_t base, uint16_t upper, int pos)
{
    return dac_from_pair(base, upper, pos);
}

float calpage_gain(const calpage_t *p, uint8_t ch)
{
    if (!calpage_usable(p) || ch >= CALPAGE_CHANNELS) return 0.0f;
    const uint8_t m = p->m[ch];
    if (m < CALPAGE_M_MIN || m > CALPAGE_M_MAX) return 0.0f;
    /* Stock: g = (m - 28)/150 (literals s16 = -28.0, s28 = 150, pool
     * 0x0803E674). m - 28 = 150 counts = 6 div at r7 = the ~12 V amplitude
     * step; g is that range's gain against nominal, applied to every range. */
    return ((float)m - 28.0f) / 150.0f;
}

const char *calpage_status_str(calpage_status_t s)
{
    switch (s) {
    case CALPAGE_ST_OK:          return "ok";
    case CALPAGE_ST_NONE:        return "not loaded";
    case CALPAGE_ST_SHORT:       return "short read";
    case CALPAGE_ST_SIGNATURE:   return "bad signature (stock would not restore it)";
    case CALPAGE_ST_SENTINEL:    return "sentinel 0/FFFF (stock would load its defaults)";
    case CALPAGE_ST_IMPLAUSIBLE: return "implausible table (not a V1.2 page?)";
    }
    return "?";
}
