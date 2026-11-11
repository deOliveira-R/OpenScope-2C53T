/*
 * calpage.h — decode and VALIDATE stock's calibration page (MCU flash
 * 0x08006000), and compute the per-channel offset-DAC code from it the way
 * stock V1.2.0 does — minus stock's corruption bugs.
 *
 * WHAT IS ON THE PAGE (stock V1.2.0 restore 0x0802CB16..0x0802D198, writer
 * 0x08029652..0x08029A44; V1.0.7 and V1.1.2 use the same page bytes)
 * ---------------------------------------------------------------------------
 *   0x00          signature: 0x55 = normal save, 0xAA = "settings reset,
 *                 calibration kept" (both restore the table; 0x0802CD92).
 *                 Anything else: stock ignores the page.
 *   0x01..0x37    UI state only. 0x05/0x06 = int8 vertical positions,
 *                 0x09 = channel mask, 0x0D = timebase index. Display only
 *                 here: OpenScope's own state is the source of those.
 *   0x38 + 4k     k = 0..59. low16 = CH1, high16 = CH2.
 *                 k < 30 = BASE endpoint, k >= 30 = UPPER endpoint;
 *                 bank = (k mod 30) / 10, range = k mod 10.
 *   0x128         ms[0x350] | ms[0x351] << 8 | sentinel << 16: interleave
 *                 trims (C2, not used here).
 *   0x12C         ms[0x352] | ms[0x353] << 8 | sentinel << 16: m1, m2, the
 *                 amplitude-cal means (handler 0x0800D954 / 0x080240BA).
 *   sentinel      = high16 of word 59 (CH2 upper, bank 2, range 9) = ms[0x34E];
 *                 0 or 0xFFFF makes stock discard the whole page
 *                 (0x0802D1B4/B8). There is no CRC and no version field.
 *
 * HOW STOCK USES IT
 * ---------------------------------------------------------------------------
 *   Offset DAC (CH1 0x080089C2..0x08008A52, CH2 0x08008BD4..0x08008C5A):
 *       DAC = (u32)((upper - base) / 200.0f * (pos + 100) + base)
 *   pos = int8 vertical position clamped to +-100 (0x08008F32). CH1's result
 *   is cut to 12 bits by a bfi into DAC1 D1DTH12R (0x08008A44); CH2's goes
 *   UNMASKED into TMR13 C1DT. Live-confirmed on unit #2 (2026-11-11 SWD
 *   session S3): CH1 r9 pos -48 -> 1980, CH2 r6 pos -44 -> 2033 (bank 0).
 *
 *   Bank (same rule for both channels and in stock's cal setup):
 *       timebase >= 5 -> bank 0; timebase == 4 or both channels on -> bank 1;
 *       otherwise (timebase <= 3, one channel: the interleaved regime) -> 2.
 *   Live: tb 3, mask 1, CH1 r6 pos -49 -> 1964 (bank 2) on unit #2.
 *
 *   Gain: g = (m - 28) / 150 per channel; stock's volts are the nominal
 *   ladder divided by g (S7). m = 0 or 0xFF means "not calibrated".
 *
 * WHAT THIS MODULE DOES DIFFERENTLY, AND WHY
 * ---------------------------------------------------------------------------
 * Real pages carry corrupt entries: unit #2 (CRC 1AAE272B) has CH1 upper
 * bank-0 r0/r1 = 0xDC5B/0x3C59 (> 12 bits) and in-family-looking but wrong
 * bank-0 values on both channels. Stock uses them as stored: at pos -62 its
 * CH1 r0 DAC is 12004.4, masked to 3812 — measured live — which pushes the
 * trace ~240 counts off screen on the most sensitive range at every slow
 * timebase. So every entry is validated (rules ported from the research
 * importer, pitwall research tierS/S12/calpage.py):
 *   - BAD:     0, 0xFFFF or > 4095 (never trust CH1's 12-bit-cut value either:
 *              which of the two meanings stock "intended" is unknowable);
 *   - SUSPECT: span upper - base outside 1580 +- 60, or the entry differs by
 *              more than 40 from the median of the same (ch, endpoint, range)
 *              over the three banks. The cross-bank test is needed: unit #2's
 *              CH2 bank-0 entries have in-family spans (1542/1640/1542) and
 *              only it catches them.
 * A (ch, bank, range) pair is used only when BOTH endpoints are clean; if
 * not, the pair of the nearest clean bank is substituted (bank 0 -> 1 -> 2,
 * bank 1 -> 2 -> 0, bank 2 -> 1 -> 0) and the substitution is recorded so
 * the shell can show it. Banks 1 and 2 are the fast-timebase banks and agree
 * within a few codes on both known units; bank 0 differs from them by
 * ~10-20 codes, which is a far smaller error than the corruption it replaces.
 * If no bank of a (ch, range) is clean, that range gets no page value and the
 * caller keeps its previous behaviour.
 *
 * A page whose table is mostly implausible (fewer than half the pairs clean,
 * e.g. a V1.0.3-layout page, whose words are signed trims, ADDENDUM_A S2) is
 * rejected as a whole, gain included.
 *
 * Nothing here is unit-specific: no table values are compiled in. A unit
 * without a page keeps today's behaviour.
 *
 * Pure and host-buildable (tests/test_calpage.c). The device glue that reads
 * the live page and writes the DACs is src/drivers/scope_vpos.c.
 */
#ifndef CALPAGE_H
#define CALPAGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define CALPAGE_LEN          0x130u  /* bytes that carry meaning (0x130+ = stock scratch) */
#define CALPAGE_CHANNELS     2u
#define CALPAGE_ENDPOINTS    2u      /* 0 = base (pos -100), 1 = upper (pos +100) */
#define CALPAGE_BANKS        3u
#define CALPAGE_RANGES       10u
#define CALPAGE_POS_MIN      (-100)
#define CALPAGE_POS_MAX      100

/* Validation rules (S12). The span window is a plausibility band around the
 * ~1580-1600 codes per 200 position steps seen on both known units, not a
 * value that is ever applied. */
#define CALPAGE_SPAN_NOM     1580
#define CALPAGE_SPAN_TOL     60
#define CALPAGE_XBANK_TOL    40

/* Amplitude-cal sanity window. Stock only rejects 0 and 0xFF; a mean near 28
 * (a 0 V "calibration") would make g ~ 0 and every volts readout explode, so
 * anything outside +-20% of nominal g = 1 (m = 178) is refused too. */
#define CALPAGE_M_MIN        148u
#define CALPAGE_M_MAX        216u

/* Why an entry is not used (bit set). */
#define CALPAGE_F_RANGE      0x01u   /* 0, 0xFFFF or > 4095               -> BAD     */
#define CALPAGE_F_SPAN       0x02u   /* span outside 1580 +- 60           -> SUSPECT */
#define CALPAGE_F_XBANK      0x04u   /* > 40 from the cross-bank median   -> SUSPECT */

typedef enum {
    CALPAGE_ST_OK = 0,
    CALPAGE_ST_NONE,          /* never parsed / no page given */
    CALPAGE_ST_SHORT,         /* fewer than CALPAGE_LEN bytes */
    CALPAGE_ST_SIGNATURE,     /* byte 0 not 0x55/0xAA: stock would not restore it */
    CALPAGE_ST_SENTINEL,      /* sentinel 0/0xFFFF: stock loads compiled defaults */
    CALPAGE_ST_IMPLAUSIBLE,   /* fewer than half the pairs clean (e.g. V1.0.3 layout) */
} calpage_status_t;

typedef struct {
    calpage_status_t status;
    uint8_t  signature;
    int8_t   hdr_pos[CALPAGE_CHANNELS];   /* page 0x05/0x06, display only */
    uint8_t  hdr_mask;                    /* page 0x09, display only */
    uint8_t  hdr_tb;                      /* page 0x0D, display only */
    uint16_t sentinel;
    uint8_t  m[CALPAGE_CHANNELS];         /* page 0x12C/0x12D */
    int8_t   trim[CALPAGE_CHANNELS];      /* page 0x128/0x129, display only */
    uint16_t raw[CALPAGE_CHANNELS][CALPAGE_ENDPOINTS][CALPAGE_BANKS][CALPAGE_RANGES];
    uint8_t  flags[CALPAGE_CHANNELS][CALPAGE_ENDPOINTS][CALPAGE_BANKS][CALPAGE_RANGES];
    /* Bank whose pair is used for (ch, bank, range); -1 = none clean. */
    int8_t   src_bank[CALPAGE_CHANNELS][CALPAGE_BANKS][CALPAGE_RANGES];
    uint8_t  n_clean_pairs;               /* of 60 */
    uint8_t  n_substituted;               /* pairs served from another bank */
    uint8_t  n_unusable;                  /* pairs with no clean bank at all */
} calpage_t;

/* Decode and validate. Never fails to fill `out`; check out->status. */
void calpage_parse(const uint8_t *page, size_t len, calpage_t *out);

/* True when the table may be used (status OK). */
bool calpage_usable(const calpage_t *p);

/* Stock's bank rule. tb = timebase index in force (FPGA reg 0x01 code; the
 * same numbering as stock's ms[0x2D] for 0x00..0x13); mask = 1 CH1, 2 CH2,
 * 3 both. */
uint8_t calpage_bank_select(uint8_t tb, uint8_t mask);

/* Clamp a vertical position to stock's +-100. */
int calpage_clamp_pos(int pos);

/* Stock's arithmetic on two raw endpoints, unmasked: (u32) of the float
 * result, exactly as 0x080089C2. Exposed so the tests can reproduce stock's
 * corrupt outputs; firmware must use calpage_dac_code(). */
uint32_t calpage_stock_formula(uint16_t base, uint16_t upper, int pos);

/* The offset-DAC code for (ch 0/1, bank, range, pos) from the clean pair
 * (after substitution), 0..4095. Returns false, and leaves *code alone, when
 * the page is unusable or no bank of that range is clean. */
bool calpage_dac_code(const calpage_t *p, uint8_t ch, uint8_t bank,
                      uint8_t range, int pos, uint16_t *code);

/* The same, for a caller that keeps only the substitution map and reads the
 * endpoints from the page bytes on demand (the device: the page is
 * memory-mapped flash, so holding a 240-B copy of the table in RAM would buy
 * nothing). `src_bank` is calpage_t.src_bank from a parse of the same bytes;
 * `page` must be at least CALPAGE_LEN long. */
bool calpage_dac_code_page(const uint8_t *page,
                           const int8_t src_bank[CALPAGE_CHANNELS][CALPAGE_BANKS][CALPAGE_RANGES],
                           uint8_t ch, uint8_t bank, uint8_t range, int pos,
                           uint16_t *code);

/* The clean pair (after substitution) that calpage_dac_code_page() would use
 * for (ch, bank, range), for a caller that corrects it first (the baseline
 * correction's RAM deltas, scope_vpos.c). False when no bank is clean. */
bool calpage_pair_page(const uint8_t *page,
                       const int8_t src_bank[CALPAGE_CHANNELS][CALPAGE_BANKS][CALPAGE_RANGES],
                       uint8_t ch, uint8_t bank, uint8_t range,
                       uint16_t *base, uint16_t *upper);

/* Stock's interpolation on one pair, clamped to 0..4095. */
uint16_t calpage_dac_from_pair(uint16_t base, uint16_t upper, int pos);

/* One raw table entry straight from page bytes (ch 0/1, ep 0 base / 1 upper). */
uint16_t calpage_entry(const uint8_t *page, uint8_t ch, uint8_t ep,
                       uint8_t bank, uint8_t range);

/* Per-channel gain g = (m - 28)/150, or 0.0f when the page is unusable or m
 * is outside the sanity window. ch = 0/1. */
float calpage_gain(const calpage_t *p, uint8_t ch);

/* Human-readable status. */
const char *calpage_status_str(calpage_status_t s);

#endif /* CALPAGE_H */
