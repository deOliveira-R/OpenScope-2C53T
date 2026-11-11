/*
 * Host tests for the screenshot gallery (parity C16):
 *   src/util/gallery_store.c  — slot layout, BMP format, naming, save/delete
 *   src/ui/gallery_nav.c      — 3x3 grid key logic
 *
 * Linked against the REAL region layer (src/drivers/flash_regions.c) over a
 * NOR model with real semantics (erase -> 0xFF, program only clears bits, a
 * program never crosses a 256 B page), the same model test_flash_regions.c
 * uses. The headline property of the item is asserted byte for byte: saving,
 * filling, deleting and re-saving screenshots changes NOTHING outside the
 * scratch region — not the calibration backup (0xFFD000), not the user-cal
 * log (0xF00000), not the settings log, not stock's FAT volumes.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flash_regions.h"
#include "gallery_store.h"
#include "gallery_nav.h"
#include "cal_backup_core.h"

static int tests_run, tests_failed, current_failed;

#define CHECK(cond, ...) do {                                   \
    if (!(cond)) {                                              \
        printf("  FAIL (line %d): ", __LINE__);                 \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
        current_failed++;                                       \
    }                                                           \
} while (0)

static void run(const char *name, void (*fn)(void))
{
    current_failed = 0;
    tests_run++;
    fn();
    if (current_failed) { tests_failed++; printf("FAIL  %s\n", name); }
    else printf("ok    %s\n", name);
}

/* ── NOR model ────────────────────────────────────────────────────────── */

#define MODEL_SIZE FLASH_REGION_CHIP_SIZE
static uint8_t *mem;
static uint32_t n_erases, n_programs;
static bool drop_programs;

static int m_read(void *ctx, uint32_t a, void *b, uint32_t n)
{
    (void)ctx;
    if (a >= MODEL_SIZE || n > MODEL_SIZE - a) { printf("MODEL: bad read\n"); exit(3); }
    memcpy(b, mem + a, n);
    return 0;
}
static int m_erase(void *ctx, uint32_t a)
{
    (void)ctx;
    if (a % FLASH_REGION_SECTOR_SIZE || a >= MODEL_SIZE) { printf("MODEL: bad erase\n"); exit(3); }
    n_erases++;
    memset(mem + a, 0xFF, FLASH_REGION_SECTOR_SIZE);
    return 0;
}
static int m_program(void *ctx, uint32_t a, const void *d, uint32_t n)
{
    (void)ctx;
    if (a >= MODEL_SIZE || n > MODEL_SIZE - a || n > FLASH_REGION_PAGE_SIZE ||
        a / FLASH_REGION_PAGE_SIZE != (a + n - 1) / FLASH_REGION_PAGE_SIZE) {
        printf("MODEL: bad program 0x%X+%u\n", a, n); exit(3);
    }
    n_programs++;
    if (drop_programs) return 0;
    const uint8_t *s = d;
    for (uint32_t i = 0; i < n; i++) mem[a + i] &= s[i];
    return 0;
}
static const flash_region_backend_t backend = { m_read, m_erase, m_program, NULL };

/* Every byte outside scratch gets irreplaceable-looking content, so a stray
 * erase or program anywhere shows up as a changed byte. */
static void fresh(void)
{
    if (!mem) mem = malloc(MODEL_SIZE);
    if (!mem) { printf("FATAL alloc\n"); exit(2); }
    memset(mem, 0xFF, MODEL_SIZE);
    for (uint32_t i = 0; i < FLASH_REGION_COUNT; i++) {
        const flash_region_t *r = &flash_region_table[i];
        if (i == GALLERY_REGION) continue;
        for (uint32_t k = 0; k < r->length; k++)
            mem[r->start + k] = (uint8_t)(0xA5u ^ (k * 7u) ^ i);
    }
    n_erases = n_programs = 0;
    drop_programs = false;
    if (flash_regions_init(&backend) != FLASH_REGION_OK) { printf("FATAL init\n"); exit(2); }
}

static uint8_t *snap(void)
{
    uint8_t *s = malloc(MODEL_SIZE);
    memcpy(s, mem, MODEL_SIZE);
    return s;
}

/* True when nothing outside the gallery region differs from s. */
static bool outside_gallery_unchanged(const uint8_t *s)
{
    const flash_region_t *g = flash_region_get(GALLERY_REGION);
    return memcmp(s, mem, g->start) == 0 &&
           memcmp(s + g->start + g->length, mem + g->start + g->length,
                  MODEL_SIZE - g->start - g->length) == 0;
}

/* ── fixtures ─────────────────────────────────────────────────────────── */

static uint8_t pixels[GALLERY_PIXEL_BYTES];
static const uint16_t pal[16] = {
    0x0000, 0xFFFF, 0xF800, 0x07E0, 0x001F, 0xFFE0, 0x07FF, 0xF81F,
    0x4208, 0x8410, 0xFD20, 0xC618, 0x2945, 0x18E3, 0x39E7, 0x0841,
};

static void make_pixels(uint32_t salt)
{
    for (uint32_t i = 0; i < GALLERY_PIXEL_BYTES; i++)
        pixels[i] = (uint8_t)((i * 31u + salt * 17u) ^ (i >> 7));
}

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t save_ok(uint32_t salt)
{
    make_pixels(salt);
    uint32_t seq = 0;
    gallery_status_t st = gallery_save(pixels, pal, NULL, &seq);
    CHECK(st == GALLERY_OK, "save %u -> %d", salt, st);
    return seq;
}

/* ── tests ────────────────────────────────────────────────────────────── */

static void test_not_ready_before_bind(void)
{
    /* runs first: the region layer is not bound yet */
    gallery_index_t idx;
    CHECK(gallery_slot_count() == 0, "slot count without a backend");
    CHECK(gallery_scan(&idx) == GALLERY_ERR_NOT_READY, "scan must say not ready");
    CHECK(gallery_save(pixels, pal, NULL, NULL) == GALLERY_ERR_NOT_READY, "save must say not ready");
}

static void test_layout_inside_scratch_only(void)
{
    fresh();
    const flash_region_t *g = flash_region_get(GALLERY_REGION);
    CHECK(g->kind == FLASH_REGION_KIND_RW, "gallery region must be plain RW");
    CHECK(gallery_slot_count() == 9, "9 slots expected, got %u", gallery_slot_count());
    CHECK(GALLERY_SLOT_SIZE % FLASH_REGION_SECTOR_SIZE == 0, "slot not sector-aligned");
    const uint32_t lo = g->start, hi = g->start + gallery_slot_count() * GALLERY_SLOT_SIZE;
    CHECK(hi <= g->start + g->length, "slots run past the region");
    /* disjoint from everything we must never touch */
    const flash_region_id_t keep[] = { FLASH_REGION_USER_CAL, FLASH_REGION_SETTINGS,
        FLASH_REGION_FACTORY_CAL_BACKUP, FLASH_REGION_TAIL, FLASH_REGION_SYSVOL,
        FLASH_REGION_USERVOL, FLASH_REGION_FWCACHE, FLASH_REGION_MODULES };
    for (unsigned i = 0; i < sizeof keep / sizeof keep[0]; i++) {
        const flash_region_t *r = flash_region_get(keep[i]);
        CHECK(hi <= r->start || lo >= r->start + r->length, "slots overlap %s", r->name);
    }
    /* the item's named hazard, by address */
    CHECK(hi <= 0xFFD000u && !(lo < 0xF10000u && hi > 0xF00000u), "slots reach a cal region");
}

static void test_bmp_header_format(void)
{
    uint8_t h[GALLERY_BMP_HDR_LEN];
    gallery_bmp_header(h, pal);
    CHECK(h[0] == 'B' && h[1] == 'M', "signature");
    CHECK(rd32(h + 2) == 38518u, "file size %u", rd32(h + 2));
    CHECK(rd32(h + 10) == 118u, "pixel offset");
    CHECK(rd32(h + 14) == 40u, "info header size");
    CHECK(rd32(h + 18) == 320u && (int32_t)rd32(h + 22) == -240, "dimensions (top-down)");
    CHECK(rd16(h + 26) == 1u && rd16(h + 28) == 4u, "planes/bpp");
    CHECK(rd32(h + 30) == 0u, "BI_RGB");
    CHECK(rd32(h + 34) == 38400u, "image size");
    CHECK(rd32(h + 46) == 16u, "colours used");
    /* palette: white -> FF FF FF 00, pure red -> 00 00 FF 00 (BGRX) */
    CHECK(memcmp(h + 54 + 1 * 4, "\xFF\xFF\xFF\x00", 4) == 0, "white entry");
    CHECK(memcmp(h + 54 + 2 * 4, "\x00\x00\xFF\x00", 4) == 0, "red entry is BGRX");
    CHECK(memcmp(h + 54 + 4 * 4, "\xFF\x00\x00\x00", 4) == 0, "blue entry is BGRX");
}

static void test_names(void)
{
    char b[16];
    CHECK(gallery_name(1, b, sizeof b) == 5 && strcmp(b, "1.bmp") == 0, "1.bmp: '%s'", b);
    CHECK(gallery_name(4294967295u, b, sizeof b) == 14 && strcmp(b, "4294967295.bmp") == 0, "max: '%s'", b);
    CHECK(gallery_name(123, b, 8) == 7 && strcmp(b, "123.bmp") == 0, "exact fit");
    CHECK(gallery_name(1234, b, 8) == 0 && b[0] == '\0', "too small must fail empty");
    uint32_t s = 0;
    CHECK(gallery_parse_name("12", &s) && s == 12, "12");
    CHECK(gallery_parse_name("12.bmp", &s) && s == 12, "12.bmp");
    CHECK(gallery_parse_name("7.BMP", &s) && s == 7, "7.BMP");
    CHECK(!gallery_parse_name("0", &s), "0 is no shot");
    CHECK(!gallery_parse_name("", &s), "empty");
    CHECK(!gallery_parse_name("12x", &s), "12x");
    CHECK(!gallery_parse_name("12.bmpx", &s), "12.bmpx");
    CHECK(!gallery_parse_name("12.jpg", &s), "12.jpg");
    CHECK(!gallery_parse_name("4294967296", &s), "overflow");
    CHECK(!gallery_parse_name("abc", &s), "abc");
}

static void test_save_roundtrip_and_bytes(void)
{
    fresh();
    uint8_t *s = snap();
    uint32_t seq = save_ok(1);
    CHECK(seq == 1, "first shot is 1.bmp, got %u", seq);
    CHECK(outside_gallery_unchanged(s), "a save changed flash outside scratch");

    /* the stored file is exactly header + the caller's pixels */
    uint32_t slot = 99;
    CHECK(gallery_find(1, &slot) == GALLERY_OK && slot == 0, "found in slot %u", slot);
    const uint8_t *f = mem + flash_region_get(GALLERY_REGION)->start + GALLERY_BMP_OFFSET;
    uint8_t h[GALLERY_BMP_HDR_LEN];
    gallery_bmp_header(h, pal);
    CHECK(memcmp(f, h, sizeof h) == 0, "BMP header bytes");
    CHECK(memcmp(f + GALLERY_BMP_HDR_LEN, pixels, GALLERY_PIXEL_BYTES) == 0, "pixel bytes");

    gallery_slot_info_t info;
    CHECK(gallery_slot_info(0, &info) == GALLERY_OK && info.state == GALLERY_SLOT_VALID, "valid");
    CHECK(info.bmp_crc == cal_backup_crc32(f, GALLERY_BMP_LEN), "stored CRC is the file's CRC");
    uint32_t c = 0;
    CHECK(gallery_crc_bmp(0, &c) == GALLERY_OK && c == info.bmp_crc, "streamed CRC");

    uint8_t row[GALLERY_ROW_BYTES];
    CHECK(gallery_read_row(0, 239, row) == GALLERY_OK &&
          memcmp(row, pixels + 239 * GALLERY_ROW_BYTES, GALLERY_ROW_BYTES) == 0, "last row");
    CHECK(gallery_read_row(0, 240, row) == GALLERY_ERR_BAD_ARG, "row 240 refused");
    uint16_t p2[16];
    CHECK(gallery_read_palette(0, p2) == GALLERY_OK && memcmp(p2, pal, sizeof pal) == 0,
          "RGB565 palette survives the BGRX round trip");
    uint8_t x;
    CHECK(gallery_read_bmp(0, GALLERY_BMP_LEN, &x, 1) == GALLERY_ERR_BAD_ARG, "read past file");
    free(s);
}

static void test_fill_full_delete_reuse(void)
{
    fresh();
    uint8_t *s = snap();
    for (uint32_t i = 1; i <= 9; i++)
        CHECK(save_ok(i) == i, "shot %u numbered %u", i, i);

    gallery_index_t idx;
    CHECK(gallery_scan(&idx) == GALLERY_OK && idx.count == 9, "9 shots");
    CHECK(idx.seq[0] == 9 && idx.seq[8] == 1, "newest first");
    CHECK(idx.next_seq == 10, "next is 10");

    /* full: refused, and nothing written anywhere */
    uint8_t *full = snap();
    const uint32_t e0 = n_erases, p0 = n_programs;
    make_pixels(99);
    CHECK(gallery_save(pixels, pal, NULL, NULL) == GALLERY_ERR_FULL, "10th save must be refused");
    CHECK(n_erases == e0 && n_programs == p0 && memcmp(full, mem, MODEL_SIZE) == 0,
          "a refused save touched flash");
    free(full);

    /* delete is a program, never an erase */
    const uint32_t e1 = n_erases;
    CHECK(gallery_delete(5) == GALLERY_OK, "delete 5");
    CHECK(n_erases == e1, "delete erased a sector");
    CHECK(gallery_delete(5) == GALLERY_ERR_NOT_FOUND, "deleted twice");
    CHECK(gallery_scan(&idx) == GALLERY_OK && idx.count == 8, "8 left");
    for (uint32_t i = 0; i < idx.count; i++) CHECK(idx.seq[i] != 5, "5 still listed");
    CHECK(idx.next_seq == 10, "numbers never go back after a delete");

    /* the freed slot (slot 4, where 5 lived) takes the next shot, #10 */
    CHECK(save_ok(10) == 10, "re-save numbered 10");
    uint32_t slot = 0;
    CHECK(gallery_find(10, &slot) == GALLERY_OK && slot == 4, "10 reused slot %u", slot);
    CHECK(gallery_find(1, &slot) == GALLERY_OK && slot == 0, "1 untouched");
    uint32_t c;
    CHECK(gallery_crc_bmp(0, &c) == GALLERY_OK, "crc 1");
    gallery_slot_info_t info;
    gallery_slot_info(0, &info);
    CHECK(c == info.bmp_crc, "shot 1 intact after the reuse of a neighbour");

    CHECK(outside_gallery_unchanged(s), "filling the gallery changed flash outside scratch");
    free(s);
}

static void test_torn_and_foreign_slots(void)
{
    fresh();
    const uint32_t base = flash_region_get(GALLERY_REGION)->start;
    /* slot 0: a torn save — pixels programmed, header never written */
    memset(mem + base + GALLERY_BMP_OFFSET, 0x12, 5000);
    /* slot 1: foreign junk in the header */
    memset(mem + base + GALLERY_SLOT_SIZE, 0x3C, GALLERY_HDR_LEN);
    gallery_slot_info_t i0, i1;
    gallery_slot_info(0, &i0);
    gallery_slot_info(1, &i1);
    CHECK(i0.state == GALLERY_SLOT_EMPTY, "torn save reads EMPTY");
    CHECK(i1.state == GALLERY_SLOT_DAMAGED, "junk header reads DAMAGED");

    /* the torn slot is reused and fully erased first */
    save_ok(3);
    uint32_t slot = 9, c = 0;
    CHECK(gallery_find(1, &slot) == GALLERY_OK && slot == 0, "torn slot reused");
    gallery_slot_info(0, &i0);
    CHECK(gallery_crc_bmp(0, &c) == GALLERY_OK && c == i0.bmp_crc, "no torn bytes left behind");
    gallery_index_t idx;
    gallery_scan(&idx);
    CHECK(idx.count == 1, "damaged slot is not listed");
}

static void test_write_failure_is_reported(void)
{
    fresh();
    drop_programs = true;           /* the "write silently did nothing" fault */
    make_pixels(5);
    CHECK(gallery_save(pixels, pal, NULL, NULL) == GALLERY_ERR_IO, "dropped programs must fail");
    drop_programs = false;
    gallery_index_t idx;
    gallery_scan(&idx);
    CHECK(idx.count == 0, "a failed save must not appear in the gallery");
}

static unsigned ticks;
static void count_tick(void) { ticks++; }

static void test_tick_between_flash_ops(void)
{
    fresh();
    ticks = 0;
    make_pixels(7);
    CHECK(gallery_save(pixels, pal, count_tick, NULL) == GALLERY_OK, "save");
    /* 10 sector erases + 10 pixel chunks */
    CHECK(ticks >= 20, "tick called %u times", ticks);
}

/* ── nav ──────────────────────────────────────────────────────────────── */

static void test_nav_grid(void)
{
    gallery_nav_t n;
    gallery_nav_open(&n);
    CHECK(gallery_nav_key(&n, GNK_RIGHT, 5) == GNA_MOVE && n.sel == 1 && n.prev_sel == 0, "right");
    CHECK(gallery_nav_key(&n, GNK_LEFT, 5) == GNA_MOVE && n.sel == 0, "left");
    CHECK(gallery_nav_key(&n, GNK_LEFT, 5) == GNA_MOVE && n.sel == 4, "left wraps to last");
    CHECK(gallery_nav_key(&n, GNK_RIGHT, 5) == GNA_MOVE && n.sel == 0, "right wraps to first");
    CHECK(gallery_nav_key(&n, GNK_DOWN, 5) == GNA_MOVE && n.sel == 3, "down = +3");
    CHECK(gallery_nav_key(&n, GNK_DOWN, 5) == GNA_NONE && n.sel == 3, "down past the last row");
    CHECK(gallery_nav_key(&n, GNK_UP, 5) == GNA_MOVE && n.sel == 0, "up = -3");
    CHECK(gallery_nav_key(&n, GNK_UP, 5) == GNA_NONE, "up past the top");
    CHECK(gallery_nav_key(&n, GNK_MENU, 5) == GNA_CLOSE, "MENU closes");
    gallery_nav_open(&n);
    CHECK(gallery_nav_key(&n, GNK_SAVE_LONG, 5) == GNA_CLOSE, "long SAVE closes");
    CHECK(gallery_nav_key(&n, GNK_OK, 0) == GNA_NONE && !n.full, "OK on an empty gallery");
    CHECK(gallery_nav_key(&n, GNK_SAVE, 0) == GNA_NONE && !n.armed, "no delete when empty");
    CHECK(gallery_nav_key(&n, GNK_RIGHT, 0) == GNA_NONE, "no move when empty");
}

static void test_nav_full_and_delete(void)
{
    gallery_nav_t n;
    gallery_nav_open(&n);
    CHECK(gallery_nav_key(&n, GNK_OK, 3) == GNA_SHOW_FULL && n.full, "OK opens");
    CHECK(gallery_nav_key(&n, GNK_RIGHT, 3) == GNA_SHOW_FULL && n.sel == 1, "next shot");
    CHECK(gallery_nav_key(&n, GNK_DOWN, 3) == GNA_NONE, "no rows in full view");
    CHECK(gallery_nav_key(&n, GNK_SAVE, 3) == GNA_ARM_DELETE && n.armed, "SAVE arms");
    CHECK(gallery_nav_key(&n, GNK_RIGHT, 3) == GNA_DISARM && !n.armed && n.sel == 1,
          "another key only disarms, does not move");
    CHECK(gallery_nav_key(&n, GNK_SAVE, 3) == GNA_ARM_DELETE, "re-arm");
    CHECK(gallery_nav_key(&n, GNK_SAVE, 3) == GNA_DELETE && !n.armed, "SAVE again deletes");
    CHECK(gallery_nav_deleted(&n, 2) == GNA_SHOW_FULL && n.sel == 1, "stays on the next shot");
    n.sel = 1;
    CHECK(gallery_nav_deleted(&n, 1) == GNA_SHOW_FULL && n.sel == 0, "clamps to the last");
    CHECK(gallery_nav_deleted(&n, 0) == GNA_SHOW_GRID && !n.full, "empty -> grid");
    gallery_nav_open(&n);
    gallery_nav_key(&n, GNK_OK, 2);
    CHECK(gallery_nav_key(&n, GNK_MENU, 2) == GNA_SHOW_GRID && !n.full, "MENU backs out of full view");
    gallery_nav_key(&n, GNK_SAVE, 2);
    CHECK(gallery_nav_key(&n, GNK_MENU, 2) == GNA_CLOSE, "MENU while armed closes");
}

int main(void)
{
    run("not ready before bind", test_not_ready_before_bind);
    run("slots sit inside scratch, clear of every cal region", test_layout_inside_scratch_only);
    run("BMP header (4 bpp, top-down, BGRX palette)", test_bmp_header_format);
    run("names <seq>.bmp and parsing", test_names);
    run("save round trip, bytes, CRC, rows, palette", test_save_roundtrip_and_bytes);
    run("fill, full refusal, delete, reuse, nothing outside", test_fill_full_delete_reuse);
    run("torn save and foreign header", test_torn_and_foreign_slots);
    run("failed programs are reported, not listed", test_write_failure_is_reported);
    run("watchdog tick between flash operations", test_tick_between_flash_ops);
    run("nav: grid moves and close", test_nav_grid);
    run("nav: full view and delete confirm", test_nav_full_and_delete);
    printf("\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
