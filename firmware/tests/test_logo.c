/*
 * Host tests for the boot logo store (parity C15), src/util/logo_store.c:
 * record format, header CRC, sequential chunked upload, commit-last, CRC
 * refusal, streaming. Real region layer over a NOR model; an upload must not
 * change one byte outside the logo region.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flash_regions.h"
#include "logo_store.h"
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

/* Every byte outside logo gets irreplaceable-looking content, so a stray
 * erase or program anywhere shows up as a changed byte. */
static void fresh(void)
{
    if (!mem) mem = malloc(MODEL_SIZE);
    if (!mem) { printf("FATAL alloc\n"); exit(2); }
    memset(mem, 0xFF, MODEL_SIZE);
    for (uint32_t i = 0; i < FLASH_REGION_COUNT; i++) {
        const flash_region_t *r = &flash_region_table[i];
        if (i == LOGO_REGION) continue;
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
static bool outside_logo_unchanged(const uint8_t *s)
{
    const flash_region_t *g = flash_region_get(LOGO_REGION);
    return memcmp(s, mem, g->start) == 0 &&
           memcmp(s + g->start + g->length, mem + g->start + g->length,
                  MODEL_SIZE - g->start - g->length) == 0;
}

/* ── fixtures ─────────────────────────────────────────────────────────── */

static uint8_t img[LOGO_PIXEL_BYTES];
static void make_img(void)
{
    for (uint32_t i = 0; i < LOGO_PIXEL_BYTES; i++) img[i] = (uint8_t)(i * 131u ^ (i >> 8));
}
static uint32_t ticks;
static void tick(void) { ticks++; }

static bool upload(logo_upload_t *up, uint32_t crc, uint32_t chunk)
{
    if (logo_put_begin(up, LOGO_PIXEL_BYTES, crc, tick) != LOGO_OK) return false;
    for (uint32_t off = 0; off < LOGO_PIXEL_BYTES; off += chunk) {
        uint32_t n = LOGO_PIXEL_BYTES - off; if (n > chunk) n = chunk;
        if (logo_put_data(up, off, img + off, n) != LOGO_OK) return false;
    }
    return true;
}

static void test_layout(void)
{
    fresh();
    const flash_region_t *r = flash_region_get(LOGO_REGION);
    CHECK(r->start == 0xF70000u && r->kind == FLASH_REGION_KIND_RW, "logo region moved");
    CHECK(LOGO_DATA_OFFSET + LOGO_PIXEL_BYTES <= r->length, "image does not fit");
    CHECK(LOGO_SECTORS * FLASH_REGION_SECTOR_SIZE <= r->length, "erase span outside region");
    CHECK(LOGO_PIXEL_BYTES == 153600u && LOGO_PIXEL_BYTES % LOGO_STREAM_PX == 0, "geometry");
    const flash_region_id_t keep[] = { FLASH_REGION_USER_CAL, FLASH_REGION_SETTINGS,
        FLASH_REGION_MODULES, FLASH_REGION_SCRATCH, FLASH_REGION_FACTORY_CAL_BACKUP,
        FLASH_REGION_TAIL, FLASH_REGION_SYSVOL, FLASH_REGION_USERVOL, FLASH_REGION_FWCACHE };
    for (unsigned i = 0; i < sizeof keep / sizeof keep[0]; i++) {
        const flash_region_t *o = flash_region_get(keep[i]);
        CHECK(r->start + r->length <= o->start || r->start >= o->start + o->length,
              "logo overlaps %s", o->name);
    }
    CHECK(r->start + r->length <= 0xFA0000u, "logo reaches scratch/usercal/calbackup addresses");
}

static void test_header_format(void)
{
    uint8_t h[LOGO_HDR_LEN];
    logo_write_header(h, 0xDEADBEEFu);
    CHECK(h[0] == 'O' && h[1] == 'S' && h[2] == 'L' && h[3] == '1', "magic bytes");
    CHECK(h[4] == 1 && h[5] == 0 && h[6] == 1 && h[7] == 0, "version/format");
    CHECK(h[8] == 0x40 && h[9] == 0x01 && h[10] == 240 && h[11] == 0, "320x240");
    CHECK(h[12] == 0x00 && h[13] == 0x58 && h[14] == 0x02 && h[15] == 0, "len 153600 LE");
    CHECK(h[16] == 0xEF && h[17] == 0xBE && h[18] == 0xAD && h[19] == 0xDE, "payload crc LE");
    CHECK(cal_backup_crc32(h, 20) == (uint32_t)(h[20] | h[21] << 8 | h[22] << 16 | (uint32_t)h[23] << 24),
          "header crc");
    logo_info_t i;
    logo_classify_header(h, &i);
    CHECK(i.state == LOGO_SLOT_VALID && i.len == LOGO_PIXEL_BYTES && i.crc == 0xDEADBEEFu, "classify");
    uint8_t bad[LOGO_HDR_LEN];
    for (unsigned k = 0; k < LOGO_HDR_LEN; k++) {      /* any single flipped byte is caught */
        memcpy(bad, h, sizeof bad); bad[k] ^= 0x01;
        logo_classify_header(bad, &i);
        CHECK(i.state == LOGO_SLOT_DAMAGED, "flip byte %u not detected", k);
    }
    memset(bad, 0xFF, sizeof bad);
    logo_classify_header(bad, &i);
    CHECK(i.state == LOGO_SLOT_EMPTY, "blank = empty");
    /* a header with a good CRC but another geometry is refused */
    memcpy(bad, h, sizeof bad); bad[8] = 0x80;
    { uint32_t c = cal_backup_crc32(bad, 20); bad[20]=(uint8_t)c; bad[21]=(uint8_t)(c>>8); bad[22]=(uint8_t)(c>>16); bad[23]=(uint8_t)(c>>24); }
    logo_classify_header(bad, &i);
    CHECK(i.state == LOGO_SLOT_DAMAGED, "other geometry accepted");
}

static void test_not_ready(void)
{
    /* runs first: the region layer is not bound yet */
    logo_info_t i;
    logo_upload_t up;
    CHECK(logo_info(&i) == LOGO_ERR_NOT_READY, "info before bind");
    CHECK(logo_put_begin(&up, LOGO_PIXEL_BYTES, 0, NULL) == LOGO_ERR_NOT_READY, "put before bind");
    CHECK(logo_clear() == LOGO_ERR_NOT_READY, "clear before bind");
}

static void test_roundtrip(void)
{
    fresh(); make_img();
    uint8_t *before = snap();
    logo_info_t i;
    CHECK(logo_info(&i) == LOGO_OK && i.state == LOGO_SLOT_EMPTY, "fresh slot is empty");
    CHECK(logo_stream(NULL, NULL) == LOGO_ERR_NONE, "stream without logo");
    uint32_t crc = cal_backup_crc32(img, sizeof img);
    logo_upload_t up;
    CHECK(upload(&up, crc, 48), "upload");
    CHECK(logo_info(&i) == LOGO_OK && i.state == LOGO_SLOT_EMPTY, "not valid before end (commit last)");
    CHECK(logo_put_end(&up, tick) == LOGO_OK, "end");
    CHECK(logo_info(&i) == LOGO_OK && i.state == LOGO_SLOT_VALID && i.crc == crc, "valid after end");
    CHECK(ticks > 0, "tick not called");
    uint32_t c2 = 0;
    CHECK(logo_crc_payload(&c2, NULL) == LOGO_OK && c2 == crc, "stored crc");
    CHECK(memcmp(mem + flash_region_get(LOGO_REGION)->start + LOGO_DATA_OFFSET, img, sizeof img) == 0,
          "payload bytes");
    CHECK(outside_logo_unchanged(before), "upload touched flash outside the logo region");
    free(before);
}

typedef struct { uint32_t n; uint8_t bad; } stream_ctx_t;
static void sink(const uint16_t *px, uint32_t count, void *c)
{
    stream_ctx_t *s = c;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t o = (s->n + i) * 2u;
        if (px[i] != (uint16_t)(img[o] | (img[o + 1] << 8))) s->bad = 1;
    }
    s->n += count;
}

static void test_stream(void)
{
    fresh(); make_img();
    logo_upload_t up;
    CHECK(upload(&up, cal_backup_crc32(img, sizeof img), 64) && logo_put_end(&up, NULL) == LOGO_OK, "setup");
    stream_ctx_t s = {0, 0};
    CHECK(logo_stream(sink, &s) == LOGO_OK, "stream");
    CHECK(s.n == LOGO_W * LOGO_H && !s.bad, "pixels %u bad=%d (RGB565 little-endian, row-major)", s.n, s.bad);
}

static void test_crc_mismatch_commits_nothing(void)
{
    fresh(); make_img();
    logo_upload_t up;
    CHECK(upload(&up, cal_backup_crc32(img, sizeof img) ^ 1u, 48), "upload");
    CHECK(logo_put_end(&up, NULL) == LOGO_ERR_CRC, "wrong declared crc accepted");
    logo_info_t i;
    CHECK(logo_info(&i) == LOGO_OK && i.state == LOGO_SLOT_EMPTY, "header written despite CRC mismatch");
    CHECK(logo_put_end(&up, NULL) == LOGO_ERR_BAD_ARG, "session survived a failed end");
}

static void test_corrupted_in_flash_is_caught(void)
{
    fresh(); make_img();
    logo_upload_t up;
    CHECK(upload(&up, cal_backup_crc32(img, sizeof img), 48), "upload");
    mem[flash_region_get(LOGO_REGION)->start + LOGO_DATA_OFFSET + 5000] &= 0xFEu;  /* bit rot */
    CHECK(logo_put_end(&up, NULL) == LOGO_ERR_CRC, "flash corruption not caught at end");
}

static void test_sequence_and_limits(void)
{
    fresh(); make_img();
    logo_upload_t up = {0};
    CHECK(logo_put_data(&up, 0, img, 16) == LOGO_ERR_BAD_ARG, "data without put");
    CHECK(logo_put_begin(&up, LOGO_PIXEL_BYTES - 1u, 0, NULL) == LOGO_ERR_BAD_ARG, "wrong length accepted");
    CHECK(logo_put_begin(&up, LOGO_PIXEL_BYTES, 0, NULL) == LOGO_OK, "begin");
    CHECK(logo_put_data(&up, 16, img, 16) == LOGO_ERR_SEQUENCE, "gap accepted");
    CHECK(logo_put_data(&up, 0, img, 16) == LOGO_OK, "first chunk");
    CHECK(logo_put_data(&up, 0, img, 16) == LOGO_ERR_SEQUENCE, "replay accepted");
    CHECK(logo_put_data(&up, 16, img, LOGO_CHUNK_MAX + 1u) == LOGO_ERR_BAD_ARG, "oversize chunk");
    CHECK(logo_put_data(&up, 16, img, 0) == LOGO_ERR_BAD_ARG, "empty chunk");
    CHECK(logo_put_end(&up, NULL) == LOGO_ERR_INCOMPLETE, "short upload committed");
    logo_info_t i;
    CHECK(logo_info(&i) == LOGO_OK && i.state == LOGO_SLOT_EMPTY, "slot not empty after short upload");
    /* overrun of the last chunk */
    CHECK(logo_put_begin(&up, LOGO_PIXEL_BYTES, 0, NULL) == LOGO_OK, "begin2");
    up.received = LOGO_PIXEL_BYTES - 8u;
    CHECK(logo_put_data(&up, LOGO_PIXEL_BYTES - 8u, img, 16) == LOGO_ERR_BAD_ARG, "overrun past image end");
}

static void test_reupload_replaces_and_clear(void)
{
    fresh(); make_img();
    logo_upload_t up;
    uint32_t crc = cal_backup_crc32(img, sizeof img);
    CHECK(upload(&up, crc, 48) && logo_put_end(&up, NULL) == LOGO_OK, "first");
    for (uint32_t i = 0; i < sizeof img; i++) img[i] = (uint8_t)~img[i];
    uint32_t crc2 = cal_backup_crc32(img, sizeof img);
    CHECK(upload(&up, crc2, 48) && logo_put_end(&up, NULL) == LOGO_OK, "second");
    logo_info_t i;
    CHECK(logo_info(&i) == LOGO_OK && i.state == LOGO_SLOT_VALID && i.crc == crc2, "second not live");
    /* torn re-upload: begin erases the header first, so the old logo is gone, not half-mixed */
    CHECK(logo_put_begin(&up, LOGO_PIXEL_BYTES, crc, NULL) == LOGO_OK, "begin3");
    CHECK(logo_info(&i) == LOGO_OK && i.state == LOGO_SLOT_EMPTY, "old header survived begin");
    CHECK(logo_clear() == LOGO_OK && logo_info(&i) == LOGO_OK && i.state == LOGO_SLOT_EMPTY, "clear");
    CHECK(upload(&up, crc2, 48) && logo_put_end(&up, NULL) == LOGO_OK, "third");
    CHECK(logo_clear() == LOGO_OK, "clear2");
    CHECK(logo_stream(NULL, NULL) == LOGO_ERR_NONE, "stream after clear");
}

static void test_write_failure_reported(void)
{
    fresh(); make_img();
    logo_upload_t up;
    CHECK(logo_put_begin(&up, LOGO_PIXEL_BYTES, 0, NULL) == LOGO_OK, "begin");
    drop_programs = true;
    CHECK(logo_put_data(&up, 0, img, 48) == LOGO_ERR_IO, "dropped program not reported");
    CHECK(up.received == 0, "received advanced on failed write");
}

int main(void)
{
    run("not ready before bind", test_not_ready);
    run("layout: inside own region, clear of every other", test_layout);
    run("header bytes, CRC, single-byte flips, geometry", test_header_format);
    run("upload round trip, commit last, nothing outside", test_roundtrip);
    run("stream: RGB565 LE, row-major, 76800 px", test_stream);
    run("declared CRC mismatch commits nothing", test_crc_mismatch_commits_nothing);
    run("flash corruption caught at end", test_corrupted_in_flash_is_caught);
    run("sequence, size and overrun limits", test_sequence_and_limits);
    run("re-upload, begin invalidates, clear", test_reupload_replaces_and_clear);
    run("failed programs are reported", test_write_failure_reported);
    printf("\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
