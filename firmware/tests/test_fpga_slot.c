/*
 * Host tests for the FPGA design slot, src/util/fpga_slot.c: record format,
 * header CRC, sequential chunked upload, commit-last, CRC refusal, verify.
 * Real region layer over a NOR model; an upload must not change one byte
 * outside the slot's 128 KB span at the start of the modules region.
 * The payloads here are synthetic bytes, not FPGA designs.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flash_regions.h"
#include "fpga_slot.h"
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

/* Every byte outside the slot's span gets irreplaceable-looking content, so a
 * stray erase or program anywhere shows up as a changed byte. */
static uint32_t span_start(void) { return flash_region_table[FSLOT_REGION].start; }
static void fresh(void)
{
    if (!mem) mem = malloc(MODEL_SIZE);
    if (!mem) { printf("FATAL alloc\n"); exit(2); }
    memset(mem, 0xFF, MODEL_SIZE);
    for (uint32_t i = 0; i < FLASH_REGION_COUNT; i++) {
        const flash_region_t *r = &flash_region_table[i];
        for (uint32_t k = 0; k < r->length; k++) {
            if (i == FSLOT_REGION && k < FSLOT_SPAN) continue;
            mem[r->start + k] = (uint8_t)(0xA5u ^ (k * 7u) ^ i);
        }
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

static bool outside_span_unchanged(const uint8_t *s)
{
    const uint32_t a = span_start();
    return memcmp(s, mem, a) == 0 &&
           memcmp(s + a + FSLOT_SPAN, mem + a + FSLOT_SPAN, MODEL_SIZE - a - FSLOT_SPAN) == 0;
}

/* ── fixtures ─────────────────────────────────────────────────────────── */

#define LEN_A 115638u          /* the size of a GW1N-2 stream with block RAM frames */
#define LEN_B 74678u
static uint8_t img[FSLOT_MAX_LEN];
static void make_img(uint32_t seed)
{
    for (uint32_t i = 0; i < FSLOT_MAX_LEN; i++) img[i] = (uint8_t)((i * 131u) ^ (i >> 8) ^ seed);
}
static uint32_t ticks;
static void tick(void) { ticks++; }

static bool upload(fslot_upload_t *up, uint32_t len, uint32_t crc, const char *name, uint32_t chunk)
{
    if (fslot_put_begin(up, len, crc, name, tick) != FSLOT_OK) return false;
    for (uint32_t off = 0; off < len; off += chunk) {
        uint32_t n = len - off; if (n > chunk) n = chunk;
        if (fslot_put_data(up, off, img + off, n) != FSLOT_OK) return false;
    }
    return true;
}

static void test_layout(void)
{
    fresh();
    const flash_region_t *r = flash_region_get(FSLOT_REGION);
    CHECK(r != NULL && r->kind == FLASH_REGION_KIND_RW, "slot region is not a writable region");
    CHECK(FSLOT_SPAN <= r->length, "span leaves the region");
    CHECK(FSLOT_SPAN % FLASH_REGION_SECTOR_SIZE == 0 && FSLOT_DATA_OFFSET % FLASH_REGION_SECTOR_SIZE == 0, "alignment");
    CHECK(FSLOT_MAX_LEN >= LEN_A, "a full GW1N-2 stream does not fit");
    CHECK(FSLOT_HDR_LEN <= FLASH_REGION_PAGE_SIZE, "header crosses a page");
}

static void test_header_format(void)
{
    uint8_t h[FSLOT_HDR_LEN]; fslot_info_t info;
    fslot_write_header(h, 1234u, 0xDEADBEEFu, "design-x");
    CHECK(h[0] == 'O' && h[1] == 'S' && h[2] == 'F' && h[3] == '1', "magic bytes");
    fslot_classify_header(h, &info);
    CHECK(info.state == FSLOT_VALID && info.len == 1234u && info.crc == 0xDEADBEEFu, "round trip");
    CHECK(strcmp(info.name, "design-x") == 0, "name '%s'", info.name);
    for (uint32_t i = 0; i < FSLOT_HDR_LEN * 8u; i++) {
        uint8_t g[FSLOT_HDR_LEN]; memcpy(g, h, sizeof g); g[i / 8u] ^= (uint8_t)(1u << (i % 8u));
        fslot_classify_header(g, &info);
        CHECK(info.state == FSLOT_DAMAGED, "bit %u flip accepted", i);
    }
    memset(h, 0xFF, sizeof h); fslot_classify_header(h, &info);
    CHECK(info.state == FSLOT_EMPTY, "blank header not EMPTY");
    fslot_write_header(h, 0u, 1u, "x"); fslot_classify_header(h, &info);
    CHECK(info.state == FSLOT_DAMAGED, "zero length accepted");
    fslot_write_header(h, FSLOT_MAX_LEN + 1u, 1u, "x"); fslot_classify_header(h, &info);
    CHECK(info.state == FSLOT_DAMAGED, "oversize accepted");
    char longname[80]; memset(longname, 'n', sizeof longname); longname[79] = '\0';
    fslot_write_header(h, 10u, 1u, longname); fslot_classify_header(h, &info);
    CHECK(info.state == FSLOT_VALID && strlen(info.name) == FSLOT_NAME_LEN, "long name not cut to %u", FSLOT_NAME_LEN);
}

static void test_not_ready(void)
{
    fslot_info_t info; fslot_upload_t up; uint8_t b[4]; bool blank;
    flash_regions_init_table(&backend, NULL, 0);
    CHECK(fslot_info(&info) == FSLOT_ERR_NOT_READY, "info");
    CHECK(fslot_put_begin(&up, 100u, 0u, "x", NULL) == FSLOT_ERR_NOT_READY, "begin");
    CHECK(fslot_read(0, b, 4) == FSLOT_ERR_NOT_READY, "read");
    CHECK(fslot_clear() == FSLOT_ERR_NOT_READY, "clear");
    CHECK(fslot_verify(&info, NULL) == FSLOT_ERR_NOT_READY, "verify");
    CHECK(fslot_span_blank(&blank, NULL) == FSLOT_ERR_NOT_READY, "blank");
}

static void test_roundtrip(void)
{
    fresh(); make_img(1);
    uint8_t *s = snap(); fslot_info_t info; fslot_upload_t up; bool blank = false;
    CHECK(fslot_span_blank(&blank, tick) == FSLOT_OK && blank, "fresh span not blank");
    CHECK(fslot_info(&info) == FSLOT_OK && info.state == FSLOT_EMPTY, "fresh slot not EMPTY");
    CHECK(fslot_verify(&info, tick) == FSLOT_ERR_NONE, "verify of an empty slot");
    const uint32_t crc = cal_backup_crc32(img, LEN_A);
    CHECK(upload(&up, LEN_A, crc, "alpha", 48), "upload");
    CHECK(fslot_info(&info) == FSLOT_OK && info.state == FSLOT_EMPTY, "valid before the commit");
    CHECK(fslot_put_end(&up, tick) == FSLOT_OK, "end");
    CHECK(fslot_verify(&info, tick) == FSLOT_OK, "verify");
    CHECK(info.len == LEN_A && info.crc == crc && strcmp(info.name, "alpha") == 0, "info after commit");
    static uint8_t back[LEN_A];
    for (uint32_t off = 0; off < LEN_A; off += 256u) {
        uint32_t n = LEN_A - off; if (n > 256u) n = 256u;
        CHECK(fslot_read(off, back + off, n) == FSLOT_OK, "read @%u", off);
    }
    CHECK(memcmp(back, img, LEN_A) == 0, "payload differs");
    CHECK(fslot_span_blank(&blank, tick) == FSLOT_OK && !blank, "span reads blank after an upload");
    CHECK(outside_span_unchanged(s), "bytes outside the span changed");
    free(s);
}

static void test_crc_mismatch_commits_nothing(void)
{
    fresh(); make_img(2);
    fslot_upload_t up; fslot_info_t info;
    CHECK(upload(&up, LEN_B, cal_backup_crc32(img, LEN_B) ^ 1u, "bad", 64), "upload");
    CHECK(fslot_put_end(&up, tick) == FSLOT_ERR_CRC, "wrong CRC accepted");
    CHECK(fslot_info(&info) == FSLOT_OK && info.state == FSLOT_EMPTY, "header written after a CRC mismatch");
    CHECK(fslot_put_end(&up, tick) == FSLOT_ERR_BAD_ARG, "session still open");
}

static void test_corrupted_in_flash_is_caught(void)
{
    fresh(); make_img(3);
    fslot_upload_t up; fslot_info_t info;
    const uint32_t crc = cal_backup_crc32(img, LEN_B);
    CHECK(upload(&up, LEN_B, crc, "c", 48), "upload");
    mem[span_start() + FSLOT_DATA_OFFSET + 777u] ^= 0x10u;
    CHECK(fslot_put_end(&up, tick) == FSLOT_ERR_CRC, "corruption before the commit not caught");
    CHECK(upload(&up, LEN_B, crc, "c", 48) && fslot_put_end(&up, tick) == FSLOT_OK, "second upload");
    mem[span_start() + FSLOT_DATA_OFFSET + LEN_B - 1u] &= 0x7Fu;
    mem[span_start() + FSLOT_DATA_OFFSET + LEN_B - 1u] ^= 0x01u;
    CHECK(fslot_verify(&info, tick) == FSLOT_ERR_CRC, "corruption after the commit not caught by verify");
}

static void test_sequence_and_limits(void)
{
    fresh(); make_img(4);
    fslot_upload_t up;
    CHECK(fslot_put_begin(&up, 0u, 0u, "x", tick) == FSLOT_ERR_BAD_ARG, "zero length");
    CHECK(fslot_put_begin(&up, FSLOT_MAX_LEN + 1u, 0u, "x", tick) == FSLOT_ERR_BAD_ARG, "oversize");
    CHECK(fslot_put_data(&up, 0, img, 16) == FSLOT_ERR_BAD_ARG, "data without a session");
    CHECK(fslot_put_begin(&up, 1000u, 0u, NULL, tick) == FSLOT_OK, "begin");
    CHECK(fslot_put_data(&up, 16, img, 16) == FSLOT_ERR_SEQUENCE, "out of sequence");
    CHECK(fslot_put_data(&up, 0, img, FSLOT_CHUNK_MAX + 1u) == FSLOT_ERR_BAD_ARG, "chunk too big");
    CHECK(fslot_put_data(&up, 0, img, 64) == FSLOT_OK && up.received == 64u, "first chunk");
    CHECK(fslot_put_data(&up, 0, img, 64) == FSLOT_ERR_SEQUENCE, "repeat");
    CHECK(fslot_put_end(&up, tick) == FSLOT_ERR_INCOMPLETE, "incomplete end");
    CHECK(fslot_put_begin(&up, 100u, 0u, "x", tick) == FSLOT_OK, "begin 2");
    CHECK(fslot_put_data(&up, 0, img, 64) == FSLOT_OK, "chunk");
    CHECK(fslot_put_data(&up, 64, img + 64, 64) == FSLOT_ERR_BAD_ARG, "overrun past the declared length");
    CHECK(fslot_put_data(&up, 64, img + 64, 36) == FSLOT_OK, "last chunk");
    uint8_t b[8];
    CHECK(fslot_read(FSLOT_MAX_LEN - 4u, b, 8) == FSLOT_ERR_BAD_ARG, "read past the span");
}

static void test_reupload_replaces_and_clear(void)
{
    fresh(); make_img(5);
    uint8_t *s = snap(); fslot_upload_t up; fslot_info_t info;
    CHECK(upload(&up, LEN_A, cal_backup_crc32(img, LEN_A), "one", 48) && fslot_put_end(&up, tick) == FSLOT_OK, "first");
    make_img(6);
    const uint32_t crc2 = cal_backup_crc32(img, LEN_B);
    CHECK(fslot_put_begin(&up, LEN_B, crc2, "two", tick) == FSLOT_OK, "begin 2");
    CHECK(fslot_info(&info) == FSLOT_OK && info.state == FSLOT_EMPTY, "begin did not invalidate the old design");
    for (uint32_t off = 0; off < LEN_B; off += 64u) {
        uint32_t n = LEN_B - off; if (n > 64u) n = 64u;
        CHECK(fslot_put_data(&up, off, img + off, n) == FSLOT_OK, "data @%u", off);
    }
    CHECK(fslot_put_end(&up, tick) == FSLOT_OK, "end 2");
    CHECK(fslot_verify(&info, tick) == FSLOT_OK && info.len == LEN_B && strcmp(info.name, "two") == 0, "second design");
    CHECK(fslot_clear() == FSLOT_OK, "clear");
    CHECK(fslot_info(&info) == FSLOT_OK && info.state == FSLOT_EMPTY, "not EMPTY after clear");
    CHECK(outside_span_unchanged(s), "bytes outside the span changed");
    free(s);
}

static void test_write_failure_reported(void)
{
    fresh(); make_img(7);
    fslot_upload_t up;
    CHECK(fslot_put_begin(&up, 1000u, 0u, "x", tick) == FSLOT_OK, "begin");
    drop_programs = true;
    CHECK(fslot_put_data(&up, 0, img, 48) == FSLOT_ERR_IO, "dropped program not reported");
    CHECK(up.received == 0, "received advanced on a failed write");
}

int main(void)
{
    run("not ready before bind", test_not_ready);
    run("layout: inside a writable region, a full stream fits", test_layout);
    run("header bytes, CRC, single-bit flips, lengths, name", test_header_format);
    run("upload round trip, commit last, nothing outside the span", test_roundtrip);
    run("declared CRC mismatch commits nothing", test_crc_mismatch_commits_nothing);
    run("flash corruption caught at end and by verify", test_corrupted_in_flash_is_caught);
    run("sequence, size and overrun limits", test_sequence_and_limits);
    run("re-upload, begin invalidates, clear", test_reupload_replaces_and_clear);
    run("failed programs are reported", test_write_failure_reported);
    printf("\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
