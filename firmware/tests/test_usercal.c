/*
 * Host tests for OpenScope's persisted calibration corrections
 * (src/util/usercal_rec.c): the v1 record format, its CRC, the version gate,
 * corrupt and foreign records, the page fingerprint, and the storage path in
 * FLASH_REGION_USER_CAL through the real region layer (flash_regions.c) on a
 * NOR model (erase -> 0xFF, program only clears bits, 256 B pages), with
 * power cuts injected into the write path.
 *
 * The values are synthetic: nothing unit-specific is asserted, because none
 * is compiled in. The page used for the fingerprint is unit #1's reference
 * page (src/util/cal_reference.c, already in the tree) as a realistic shape.
 */
#include "../src/util/usercal_rec.h"
#include "../src/util/cal_backup_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const uint8_t cal_reference_page[4096];   /* unit #1, src/util/cal_reference.c */

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        checks++;                                                             \
        if (!(cond)) {                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
            failures++;                                                       \
        }                                                                     \
    } while (0)

/* ── NOR model ───────────────────────────────────────────────────── */

static uint8_t *chip;
static bool     powered = true;
static int32_t  program_budget = -1;    /* bytes until the power dies; -1 = never */
static uint32_t erases;

static int m_read(void *ctx, uint32_t addr, void *buf, uint32_t len)
{
    (void)ctx;
    if (!powered || addr + len > FLASH_REGION_CHIP_SIZE) return -1;
    memcpy(buf, chip + addr, len);
    return 0;
}

static int m_erase(void *ctx, uint32_t addr)
{
    (void)ctx;
    if (!powered || (addr % FLASH_REGION_SECTOR_SIZE) != 0u) return -1;
    memset(chip + addr, 0xFF, FLASH_REGION_SECTOR_SIZE);
    erases++;
    return 0;
}

static int m_program(void *ctx, uint32_t addr, const void *data, uint32_t len)
{
    (void)ctx;
    if (!powered) return -1;
    if ((addr / FLASH_REGION_PAGE_SIZE) != ((addr + len - 1u) / FLASH_REGION_PAGE_SIZE)) return -1;
    const uint8_t *d = data;
    for (uint32_t i = 0; i < len; i++) {
        if (program_budget == 0) { powered = false; return -1; }   /* cut mid-program */
        if (program_budget > 0) program_budget--;
        chip[addr + i] &= d[i];
    }
    return 0;
}

static const flash_region_backend_t backend = { m_read, m_erase, m_program, NULL };

static void power_cycle(void)
{
    powered = true;
    program_budget = -1;
    CHECK(flash_regions_init(&backend) == FLASH_REGION_OK, "region layer init");
}

static void chip_blank(void)
{
    memset(chip, 0xFF, FLASH_REGION_CHIP_SIZE);
    erases = 0;
    power_cycle();
}

/* ── Helpers ─────────────────────────────────────────────────────── */

static void sample(usercal_t *u, int seed)
{
    usercal_clear(u);
    u->table_crc = 0x12345678u + (uint32_t)seed;
    u->il_present = true;
    u->il_gain_ppm = 1012345 + seed;
    u->il_off_e4 = -23456 - seed;
    /* Unit #2's bench levels (2026-11-11) as knots, and its skew. Test data. */
    static const int16_t ka[5] = { 977, 4961, 9898, 14860, 19902 };
    static const int16_t kb[5] = { 1396, 5163, 10096, 15059, 19862 };
    u->il_n_knots = 5;
    for (unsigned i = 0; i < 5; i++) { u->il_knot_a[i] = ka[i]; u->il_knot_b[i] = kb[i]; }
    u->il_skew_ppm = -49000 + seed;
    /* A few deltas on both channels and all three banks, signed both ways. */
    const struct { unsigned c, b, r; int16_t db, du; } e[] = {
        {0, 0, 0,  12, -7}, {0, 1, 6, -40, 33}, {0, 2, 9, 4095, -4095},
        {1, 0, 3,  -1,  1}, {1, 2, 0, 100, 200},
    };
    for (unsigned i = 0; i < sizeof(e) / sizeof(e[0]); i++) {
        u->valid[e[i].c] |= 1u << (e[i].b * CALPAGE_RANGES + e[i].r);
        /* Only entry 3 varies with the seed, and stays well inside 12 bits. */
        u->corr[e[i].c][e[i].b][e[i].r][0] = (int16_t)(e[i].db + (i == 3u ? seed % 1000 : 0));
        u->corr[e[i].c][e[i].b][e[i].r][1] = e[i].du;
    }
}

static bool same(const usercal_t *a, const usercal_t *b)
{
    if (a->table_crc != b->table_crc || a->il_present != b->il_present) return false;
    if (a->il_present && (a->il_gain_ppm != b->il_gain_ppm || a->il_off_e4 != b->il_off_e4)) return false;
    if (a->il_n_knots != b->il_n_knots || a->il_skew_ppm != b->il_skew_ppm) return false;
    for (unsigned i = 0; i < a->il_n_knots; i++)
        if (a->il_knot_a[i] != b->il_knot_a[i] || a->il_knot_b[i] != b->il_knot_b[i]) return false;
    for (unsigned c = 0; c < CALPAGE_CHANNELS; c++) {
        if (a->valid[c] != b->valid[c]) return false;
        for (unsigned b2 = 0; b2 < CALPAGE_BANKS; b2++)
            for (unsigned r = 0; r < CALPAGE_RANGES; r++) {
                if (!((a->valid[c] >> (b2 * CALPAGE_RANGES + r)) & 1u)) continue;
                if (a->corr[c][b2][r][0] != b->corr[c][b2][r][0] ||
                    a->corr[c][b2][r][1] != b->corr[c][b2][r][1]) return false;
            }
    }
    return true;
}

/* Re-stamp the trailing CRC after a deliberate edit, so the test reaches the
 * check behind it instead of stopping at the CRC. */
static void restamp_len(uint8_t *buf, unsigned len)
{
    const uint32_t c = cal_backup_crc32(buf, len - 4u);
    buf[len - 4u] = (uint8_t)c; buf[len - 3u] = (uint8_t)(c >> 8);
    buf[len - 2u] = (uint8_t)(c >> 16); buf[len - 1u] = (uint8_t)(c >> 24);
}
static void restamp(uint8_t *buf) { restamp_len(buf, USERCAL_REC_LEN); }

/* ── Format ──────────────────────────────────────────────────────── */

static void test_round_trip(void)
{
    usercal_t a, b;
    uint8_t buf[USERCAL_REC_LEN];
    sample(&a, 0);
    CHECK(usercal_encode(&a, buf, sizeof(buf)) == USERCAL_REC_LEN, "encode length");
    CHECK(usercal_encode(&a, buf, sizeof(buf) - 1u) == 0u, "short buffer refused");
    /* Header bytes, as the shell's `flash read 0xF00008 16` will show them
     * after the layer's 8-byte header. */
    CHECK(buf[0] == 'O' && buf[1] == 'S' && buf[2] == 'U' && buf[3] == 'C', "magic OSUC");
    CHECK(buf[4] == 2 && buf[5] == 0 && buf[6] == (USERCAL_REC_LEN & 0xFF) && buf[7] == (USERCAL_REC_LEN >> 8),
          "version 2, length 304");
    CHECK(buf[12] == (USERCAL_F_IL | USERCAL_F_IL_KNOTS | USERCAL_F_IL_SKEW), "flags line+knots+skew");
    CHECK(usercal_decode(buf, sizeof(buf), &b) == USERCAL_ST_OK, "decode ok");
    CHECK(same(&a, &b), "round trip preserves every field");
    CHECK(usercal_count(&b) == 5u, "count 5, got %u", usercal_count(&b));

    /* No interleave block, no deltas: the empty (clear) record. */
    usercal_clear(&a);
    CHECK(usercal_encode(&a, buf, sizeof(buf)) == USERCAL_REC_LEN, "encode empty");
    CHECK(usercal_decode(buf, sizeof(buf), &b) == USERCAL_ST_OK && !b.il_present && usercal_count(&b) == 0u,
          "empty record decodes as empty");
}

static void test_canonical_encoding(void)
{
    /* Stray deltas behind a clear valid bit and stale interleave numbers with
     * the block absent must not reach flash: equal meaning -> equal bytes, so
     * the layer elides a repeated save. */
    usercal_t a, b;
    uint8_t ba[USERCAL_REC_LEN], bb[USERCAL_REC_LEN];
    sample(&a, 3);
    b = a;
    b.corr[1][1][1][0] = 77;     /* no valid bit for (1,1,1) */
    CHECK(usercal_encode(&a, ba, sizeof(ba)) && usercal_encode(&b, bb, sizeof(bb)) &&
          memcmp(ba, bb, sizeof(ba)) == 0, "unflagged delta does not change the bytes");
    a.il_present = b.il_present = false;
    b.il_gain_ppm = 1; b.il_off_e4 = 99;
    CHECK(usercal_encode(&a, ba, sizeof(ba)) && usercal_encode(&b, bb, sizeof(bb)) &&
          memcmp(ba, bb, sizeof(ba)) == 0, "absent interleave block is canonical");
}

static void test_every_byte_is_covered(void)
{
    /* Flip each bit of a valid record: none may decode OK, and the caller's
     * struct must be left exactly as it was. */
    usercal_t a, out, sentinel;
    uint8_t buf[USERCAL_REC_LEN];
    sample(&a, 1);
    memset(&sentinel, 0xA5, sizeof(sentinel));
    unsigned accepted = 0, touched = 0;
    for (unsigned i = 0; i < USERCAL_REC_LEN; i++)
        for (unsigned bit = 0; bit < 8u; bit++) {
            usercal_encode(&a, buf, sizeof(buf));
            buf[i] ^= (uint8_t)(1u << bit);
            out = sentinel;
            if (usercal_decode(buf, sizeof(buf), &out) == USERCAL_ST_OK) accepted++;
            if (memcmp(&out, &sentinel, sizeof(out)) != 0) touched++;
        }
    CHECK(accepted == 0u, "%u single-bit corruptions were accepted", accepted);
    CHECK(touched == 0u, "%u rejected decodes wrote the caller's struct", touched);
}

static void test_header_rejections(void)
{
    usercal_t a, out;
    uint8_t buf[USERCAL_REC_LEN];
    sample(&a, 2);

    usercal_encode(&a, buf, sizeof(buf));
    buf[4] = 3; restamp(buf);
    CHECK(usercal_decode(buf, sizeof(buf), &out) == USERCAL_ST_VERSION, "version 3 -> unknown version");
    buf[4] = 0; restamp(buf);
    CHECK(usercal_decode(buf, sizeof(buf), &out) == USERCAL_ST_VERSION, "version 0 -> unknown version");

    usercal_encode(&a, buf, sizeof(buf));
    buf[0] ^= 0xFF; restamp(buf);
    CHECK(usercal_decode(buf, sizeof(buf), &out) == USERCAL_ST_MAGIC, "bad magic");

    usercal_encode(&a, buf, sizeof(buf));
    CHECK(usercal_decode(buf, sizeof(buf) - 4u, &out) == USERCAL_ST_SIZE, "short record");
    CHECK(usercal_decode(buf, 4u, &out) == USERCAL_ST_SIZE, "tiny record");
    CHECK(usercal_decode(NULL, sizeof(buf), &out) == USERCAL_ST_SIZE, "null buffer");
    buf[6] = 0x10; restamp(buf);
    CHECK(usercal_decode(buf, sizeof(buf), &out) == USERCAL_ST_SIZE, "length field disagrees");

    usercal_encode(&a, buf, sizeof(buf));
    buf[USERCAL_REC_LEN - 4u] ^= 1;
    CHECK(usercal_decode(buf, sizeof(buf), &out) == USERCAL_ST_CRC, "bad crc");

    /* A config record (settings log) in the wrong region is foreign. */
    uint8_t foreign[USERCAL_REC_LEN];
    memset(foreign, 0, sizeof(foreign));
    foreign[0] = '2'; foreign[1] = 'C'; foreign[2] = 'S'; foreign[3] = 'O';   /* CONFIG_MAGIC "OSC2" LE */
    CHECK(usercal_decode(foreign, sizeof(foreign), &out) == USERCAL_ST_MAGIC, "foreign record");
}

static void test_value_rejections(void)
{
    /* Records that pass their CRC but hold what no writer produces. */
    usercal_t a, out;
    uint8_t buf[USERCAL_REC_LEN];

    sample(&a, 0);
    a.corr[0][0][0][0] = -4095;
    usercal_encode(&a, buf, sizeof(buf));
    CHECK(usercal_decode(buf, sizeof(buf), &out) == USERCAL_ST_OK, "+-4095 is still a delta");
    a.corr[0][0][0][0] = 4096;
    usercal_encode(&a, buf, sizeof(buf));
    CHECK(usercal_decode(buf, sizeof(buf), &out) == USERCAL_ST_RANGE, "delta beyond 12 bits");

    sample(&a, 0);
    a.il_gain_ppm = USERCAL_IL_GAIN_MAX + 1;
    usercal_encode(&a, buf, sizeof(buf));
    CHECK(usercal_decode(buf, sizeof(buf), &out) == USERCAL_ST_RANGE, "interleave gain too high");
    a.il_gain_ppm = USERCAL_IL_GAIN_MIN - 1;
    usercal_encode(&a, buf, sizeof(buf));
    CHECK(usercal_decode(buf, sizeof(buf), &out) == USERCAL_ST_RANGE, "interleave gain too low");
    a.il_gain_ppm = 1000000; a.il_off_e4 = -USERCAL_IL_OFF_MAX - 1;
    usercal_encode(&a, buf, sizeof(buf));
    CHECK(usercal_decode(buf, sizeof(buf), &out) == USERCAL_ST_RANGE, "interleave offset too large");
    /* ...but only while the block is present. */
    a.il_present = false;
    usercal_encode(&a, buf, sizeof(buf));
    CHECK(usercal_decode(buf, sizeof(buf), &out) == USERCAL_ST_OK, "absent block is not range-checked");

    sample(&a, 0);
    usercal_encode(&a, buf, sizeof(buf));
    buf[24 + 3] |= 0x40u; restamp(buf);   /* valid bit 30: no such (bank, range) */
    CHECK(usercal_decode(buf, sizeof(buf), &out) == USERCAL_ST_RANGE, "valid bit beyond bank 2 range 9");

    usercal_encode(&a, buf, sizeof(buf));
    buf[12] |= 0x08u; restamp(buf);       /* a flag v2 does not define */
    CHECK(usercal_decode(buf, sizeof(buf), &out) == USERCAL_ST_RANGE, "unknown flag bit");

    /* v2 interleave fields */
    sample(&a, 0);
    a.il_knot_b[2] = a.il_knot_b[1];      /* flat segment: not a converter */
    usercal_encode(&a, buf, sizeof(buf));
    CHECK(usercal_decode(buf, sizeof(buf), &out) == USERCAL_ST_RANGE, "non-monotonic knots");
    sample(&a, 0);
    usercal_encode(&a, buf, sizeof(buf));
    buf[272] = 6; restamp(buf);           /* more knots than the map holds */
    CHECK(usercal_decode(buf, sizeof(buf), &out) == USERCAL_ST_RANGE, "6 knots");
    sample(&a, 0);
    a.il_skew_ppm = IL_SKEW_PPM_MAX + 1;
    usercal_encode(&a, buf, sizeof(buf));
    CHECK(usercal_decode(buf, sizeof(buf), &out) == USERCAL_ST_RANGE, "skew out of range");
    sample(&a, 0);
    usercal_encode(&a, buf, sizeof(buf));
    buf[12] &= (uint8_t)~USERCAL_F_IL_SKEW; restamp(buf);   /* a skew without its flag */
    CHECK(usercal_decode(buf, sizeof(buf), &out) == USERCAL_ST_RANGE, "skew without flag");
    sample(&a, 0);
    a.il_n_knots = 0; a.il_skew_ppm = 0;
    usercal_encode(&a, buf, sizeof(buf));
    CHECK(usercal_decode(buf, sizeof(buf), &out) == USERCAL_ST_OK && out.il_n_knots == 0 &&
          out.il_skew_ppm == 0 && buf[12] == USERCAL_F_IL, "line only");
}

/* A v1 record (276 B, CRC at 272), built byte by byte as v1 firmware wrote
 * it: decodes with its line and deltas, no knots, no skew; loads from the
 * log; and the next save is a v2 record. */
static void test_v1_migration(void)
{
    usercal_t a, out;
    uint8_t v2[USERCAL_REC_LEN], v1[USERCAL_V1_LEN];
    sample(&a, 4);
    usercal_encode(&a, v2, sizeof(v2));
    memcpy(v1, v2, USERCAL_V1_LEN);
    v1[4] = 1; v1[5] = 0;
    v1[6] = (uint8_t)USERCAL_V1_LEN; v1[7] = (uint8_t)(USERCAL_V1_LEN >> 8);
    v1[12] = USERCAL_F_IL;
    restamp_len(v1, USERCAL_V1_LEN);
    memset(&out, 0xA5, sizeof(out));
    CHECK(usercal_decode(v1, sizeof(v1), &out) == USERCAL_ST_OK, "v1 record refused");
    CHECK(out.il_present && out.il_gain_ppm == a.il_gain_ppm && out.il_off_e4 == a.il_off_e4,
          "v1 line lost");
    CHECK(out.il_n_knots == 0 && out.il_skew_ppm == 0, "v1 migrated with knots/skew");
    CHECK(usercal_count(&out) == usercal_count(&a) && out.table_crc == a.table_crc, "v1 deltas lost");
    CHECK(usercal_decode(v1, sizeof(v1) - 1u, &out) == USERCAL_ST_SIZE, "short v1");
    v1[12] |= USERCAL_F_IL_KNOTS; restamp_len(v1, USERCAL_V1_LEN);
    CHECK(usercal_decode(v1, sizeof(v1), &out) == USERCAL_ST_RANGE, "v1 with a v2 flag");
    v1[12] = USERCAL_F_IL; restamp_len(v1, USERCAL_V1_LEN);
    /* A v1 length with a v2 version, and the reverse, are not records. */
    CHECK(usercal_decode(v2, USERCAL_V1_LEN, &out) == USERCAL_ST_SIZE, "v2 header at v1 length");

    chip_blank();
    CHECK(flash_region_append(USERCAL_REGION, v1, sizeof(v1)) == FLASH_REGION_OK, "plant v1");
    CHECK(usercal_store_load(&out) == USERCAL_ST_OK && out.il_gain_ppm == a.il_gain_ppm &&
          out.il_n_knots == 0, "v1 record from the log");
    CHECK(usercal_store_save(&a) == FLASH_REGION_OK, "save after a v1");
    CHECK(usercal_store_load(&out) == USERCAL_ST_OK && same(&a, &out), "v2 save over v1 loads");
}

static void test_table_fingerprint(void)
{
    /* Covers the endpoint table 0x38..0x127 and nothing else: stock's UI
     * bytes and the trims/means after the table move without invalidating
     * a baseline delta; any endpoint word moving invalidates it. */
    static uint8_t page[4096];
    memcpy(page, cal_reference_page, sizeof(page));
    const uint32_t ref = usercal_table_crc(page);
    CHECK(ref == cal_backup_crc32(page + 0x38, 240), "fingerprint is the CRC32 of 0x38..0x127");
    page[0x05] ^= 0x10;  page[0x09] ^= 3;  page[0x0D] ^= 1;   /* positions, mask, timebase */
    page[0x128] ^= 1;    page[0x12C] ^= 1;                    /* trims, amplitude means */
    CHECK(usercal_table_crc(page) == ref, "UI state and trims do not change the fingerprint");
    page[0x38] ^= 1;
    CHECK(usercal_table_crc(page) != ref, "first endpoint word changes it");
    page[0x38] ^= 1; page[0x127] ^= 0x80;
    CHECK(usercal_table_crc(page) != ref, "last endpoint word changes it");
    CHECK(usercal_table_crc(NULL) == 0u, "null page");
}

/* ── Storage ─────────────────────────────────────────────────────── */

static uint32_t region_crc_outside_usercal(void)
{
    /* Everything on the chip but our region, so a test can prove a save or a
     * compaction never touched the settings log, the cal backup or stock's
     * volumes. */
    const flash_region_t *r = flash_region_get(USERCAL_REGION);
    uint32_t c = cal_backup_crc32_update(CAL_BACKUP_CRC32_INIT, chip, r->start);
    c = cal_backup_crc32_update(c, chip + r->start + r->length,
                                FLASH_REGION_CHIP_SIZE - r->start - r->length);
    return cal_backup_crc32_final(c);
}

static void test_store_round_trip(void)
{
    chip_blank();
    /* Something else on the chip, so "untouched" means something. */
    memset(chip + 0xF10000u, 0x5A, 64);   /* settings log start */
    memset(chip + 0xFFD000u, 0xC3, 64);   /* factory-cal backup */
    const uint32_t outside = region_crc_outside_usercal();

    usercal_t a, b, out;
    CHECK(usercal_store_load(&out) == USERCAL_ST_EMPTY, "blank log -> no record");

    sample(&a, 0);
    CHECK(usercal_store_save(&a) == FLASH_REGION_OK, "save");
    power_cycle();
    CHECK(usercal_store_load(&out) == USERCAL_ST_OK && same(&a, &out), "loads after a power cycle");

    uint32_t nrec = 0;
    CHECK(usercal_store_save(&a) == FLASH_REGION_OK, "identical save");
    flash_region_log_info(USERCAL_REGION, NULL, NULL, &nrec);
    CHECK(nrec == 1u, "identical save elided, %u records", (unsigned)nrec);

    sample(&b, 9);
    CHECK(usercal_store_save(&b) == FLASH_REGION_OK, "second save");
    power_cycle();
    CHECK(usercal_store_load(&out) == USERCAL_ST_OK && same(&b, &out), "newest record wins");

    /* clear = an empty record on top: no erase. */
    const uint32_t e0 = erases;
    usercal_clear(&a);
    CHECK(usercal_store_save(&a) == FLASH_REGION_OK, "clear record");
    CHECK(erases == e0, "clear does not erase");
    power_cycle();
    CHECK(usercal_store_load(&out) == USERCAL_ST_OK && usercal_count(&out) == 0u && !out.il_present,
          "cleared: loads as empty");
    CHECK(region_crc_outside_usercal() == outside, "nothing outside the user-cal region changed");
}

static void test_torn_and_damaged(void)
{
    usercal_t a, b, out;
    chip_blank();
    sample(&a, 0);
    sample(&b, 5);
    CHECK(usercal_store_save(&a) == FLASH_REGION_OK, "good save");

    /* Power dies half-way through the next record's payload. */
    program_budget = 8 + USERCAL_REC_LEN / 2;
    CHECK(usercal_store_save(&b) != FLASH_REGION_OK, "torn save reports failure");
    power_cycle();
    CHECK(usercal_store_load(&out) == USERCAL_ST_OK && same(&a, &out), "torn record skipped, previous wins");
    /* And the log still takes the next save. */
    CHECK(usercal_store_save(&b) == FLASH_REGION_OK, "save after a torn record");
    power_cycle();
    CHECK(usercal_store_load(&out) == USERCAL_ST_OK && same(&b, &out), "new record after the tear loads");

    /* Bit rot in the newest record's payload: the layer's CRC skips it. */
    const uint32_t base = flash_region_get(USERCAL_REGION)->start;
    uint32_t used = 0;
    flash_region_log_info(USERCAL_REGION, &used, NULL, NULL);
    /* The newest record's first payload byte ('O' = 0x4F): clearing bits is
     * what NOR rot (and a stray program) can do. */
    CHECK(chip[base + used - USERCAL_REC_LEN] == 'O', "located the newest record");
    chip[base + used - USERCAL_REC_LEN] &= 0x0F;
    power_cycle();
    CHECK(usercal_store_load(&out) == USERCAL_ST_OK && same(&a, &out), "rotten newest record skipped");
}

static void test_foreign_and_future_records(void)
{
    usercal_t out;
    uint8_t buf[USERCAL_REC_LEN];

    /* A well-formed layer record that is a v3 user-cal of the same size. */
    chip_blank();
    usercal_t a;
    sample(&a, 0);
    usercal_encode(&a, buf, sizeof(buf));
    buf[4] = 3; restamp(buf);
    CHECK(flash_region_append(USERCAL_REGION, buf, sizeof(buf)) == FLASH_REGION_OK, "plant v3");
    memset(&out, 0xA5, sizeof(out));
    usercal_t before = out;
    CHECK(usercal_store_load(&out) == USERCAL_ST_VERSION, "v3 record -> unknown version");
    CHECK(memcmp(&out, &before, sizeof(out)) == 0, "rejected record leaves *out untouched");

    /* A later, longer layout. */
    static uint8_t big[512];
    memset(big, 0, sizeof(big));
    CHECK(flash_region_append(USERCAL_REGION, big, sizeof(big)) == FLASH_REGION_OK, "plant 512 B");
    CHECK(usercal_store_load(&out) == USERCAL_ST_SIZE, "longer record -> wrong size");

    /* Our own save on top recovers. */
    CHECK(usercal_store_save(&a) == FLASH_REGION_OK, "save over foreign records");
    CHECK(usercal_store_load(&out) == USERCAL_ST_OK && same(&a, &out), "own record loads again");
}

static void test_compaction(void)
{
    chip_blank();
    const uint32_t outside = region_crc_outside_usercal();
    usercal_t a, out;
    unsigned saves = 0;
    uint32_t free_bytes = UINT32_MAX;
    /* Distinct records until the log is too full for one more. */
    for (int i = 0; free_bytes >= 8u + USERCAL_REC_LEN; i++) {
        sample(&a, i);
        CHECK(usercal_store_save(&a) == FLASH_REGION_OK, "fill save %d", i);
        flash_region_log_info(USERCAL_REGION, NULL, &free_bytes, NULL);
        saves++;
        if (saves > 1000u) break;
    }
    CHECK(saves > 200u, "a 64 KB log holds >200 records, got %u", saves);
    const uint32_t e0 = erases;
    sample(&a, 7777);
    CHECK(usercal_store_save(&a) == FLASH_REGION_OK, "save into a full log compacts");
    CHECK(erases > e0 && erases - e0 <= 16u, "compaction erased only our 16 sectors (%u)",
          (unsigned)(erases - e0));
    uint32_t nrec = 0;
    flash_region_log_info(USERCAL_REGION, NULL, NULL, &nrec);
    CHECK(nrec == 1u, "one record after compaction, %u", (unsigned)nrec);
    power_cycle();
    CHECK(usercal_store_load(&out) == USERCAL_ST_OK && same(&a, &out), "newest survives compaction");
    CHECK(region_crc_outside_usercal() == outside, "compaction stayed inside the region");
}

static void test_no_storage(void)
{
    /* An uninitialised layer (a malformed table, the emulator) refuses. */
    flash_region_t bad[2] = {
        { "a", 0x000000u, 0x1000u, FLASH_REGION_KIND_APPEND },
        { "b", 0x000800u, 0x1000u, FLASH_REGION_KIND_APPEND },   /* overlaps, misaligned */
    };
    CHECK(flash_regions_init_table(&backend, bad, 2) != FLASH_REGION_OK, "bad table refused");
    usercal_t a, out;
    sample(&a, 0);
    CHECK(usercal_store_save(&a) == FLASH_REGION_ERR_NOT_INIT, "save refused without storage");
    CHECK(usercal_store_load(&out) == USERCAL_ST_NO_STORAGE, "load reports no storage");
    power_cycle();
}

int main(void)
{
    chip = malloc(FLASH_REGION_CHIP_SIZE);
    if (!chip) { printf("no memory for the chip model\n"); return 1; }

    test_round_trip();
    test_canonical_encoding();
    test_every_byte_is_covered();
    test_header_rejections();
    test_value_rejections();
    test_v1_migration();
    test_table_fingerprint();
    test_store_round_trip();
    test_torn_and_damaged();
    test_foreign_and_future_records();
    test_compaction();
    test_no_storage();

    free(chip);
    if (failures == 0) {
        printf("test_usercal: all %d checks passed\n", checks);
        return 0;
    }
    printf("test_usercal: %d of %d check(s) FAILED\n", failures, checks);
    return 1;
}
