/*
 * Host tests for USB Sharing (parity C16):
 *   src/util/gallery_vfat.c — the read-only FAT12 volume synthesised from
 *                             the screenshot gallery
 *   src/util/msc_scsi.c     — SCSI commands and bulk-only bookkeeping
 *
 * The volume is checked the way a host would see it: an independent FAT12
 * reader in this file (it shares no code with the generator) parses the
 * boot sector, decodes the packed 12-bit FAT from the raw sector bytes,
 * walks the root directory, follows every cluster chain and compares the
 * file it reassembles with the BMP gallery_store wrote, CRC for CRC. The
 * shots themselves are saved through the REAL gallery_store and region layer
 * onto the NOR model test_gallery.c uses.
 *
 * Set VFAT_IMAGE_OUT=/path/img to also dump the 512 KB image (for
 * `fsck_msdos -n` / `hdiutil attach -imagekey diskimage-class=CRawDiskImage`).
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flash_regions.h"
#include "gallery_store.h"
#include "gallery_vfat.h"
#include "msc_scsi.h"
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

/* ── NOR model (as test_gallery.c) ────────────────────────────────────── */

#define MODEL_SIZE FLASH_REGION_CHIP_SIZE
static uint8_t *mem;
static bool fail_reads;

static int m_read(void *ctx, uint32_t a, void *b, uint32_t n)
{
    (void)ctx;
    if (a >= MODEL_SIZE || n > MODEL_SIZE - a) { printf("MODEL: bad read\n"); exit(3); }
    if (fail_reads) return -1;
    memcpy(b, mem + a, n);
    return 0;
}
static int m_erase(void *ctx, uint32_t a)
{
    (void)ctx;
    if (a % FLASH_REGION_SECTOR_SIZE || a >= MODEL_SIZE) { printf("MODEL: bad erase\n"); exit(3); }
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
    const uint8_t *s = d;
    for (uint32_t i = 0; i < n; i++) mem[a + i] &= s[i];
    return 0;
}
static const flash_region_backend_t backend = { m_read, m_erase, m_program, NULL };

static void fresh(void)
{
    if (!mem) mem = malloc(MODEL_SIZE);
    if (!mem) { printf("FATAL alloc\n"); exit(2); }
    memset(mem, 0xFF, MODEL_SIZE);
    fail_reads = false;
    if (flash_regions_init(&backend) != FLASH_REGION_OK) { printf("FATAL init\n"); exit(2); }
}

static uint8_t pixels[GALLERY_PIXEL_BYTES];
static const uint16_t pal[16] = {
    0x0000, 0xFFFF, 0xF800, 0x07E0, 0x001F, 0xFFE0, 0x07FF, 0xF81F,
    0x4208, 0x8410, 0xFD20, 0xC618, 0x2945, 0x18E3, 0x39E7, 0x0841,
};

static uint32_t save_shot(uint32_t salt)
{
    for (uint32_t i = 0; i < GALLERY_PIXEL_BYTES; i++)
        pixels[i] = (uint8_t)((i * 31u + salt * 17u) ^ (i >> 7) ^ (salt << 3));
    uint32_t seq = 0;
    gallery_status_t st = gallery_save(pixels, pal, NULL, &seq);
    CHECK(st == GALLERY_OK, "save %u -> %d", salt, st);
    return seq;
}

static bool read_slot(void *ctx, uint32_t slot, uint32_t off, void *buf, uint32_t len)
{
    (void)ctx;
    return gallery_read_bmp(slot, off, buf, len) == GALLERY_OK;
}

static void build(gallery_vfat_t *v)
{
    gallery_index_t idx;
    CHECK(gallery_scan(&idx) == GALLERY_OK, "scan");
    gallery_vfat_build(v, &idx);
}

/* The whole volume, sector by sector, as a host would read it. */
static uint8_t image[VFAT_TOTAL_SECTORS * VFAT_SECTOR_SIZE];

static void read_image(const gallery_vfat_t *v)
{
    for (uint32_t lba = 0; lba < VFAT_TOTAL_SECTORS; lba++) {
        vfat_status_t st = gallery_vfat_read_sector(v, lba, image + lba * VFAT_SECTOR_SIZE, read_slot, NULL);
        CHECK(st == VFAT_OK, "lba %u -> %d", lba, st);
    }
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

/* ── An independent FAT12 reader ──────────────────────────────────────── */

typedef struct {
    uint32_t bps, spc, rsvd, nfats, root_ents, total, fatsz;
    uint32_t fat_off, root_off, data_off, clusters;
} bpb_t;

static void parse_bpb(const uint8_t *img, bpb_t *b)
{
    b->bps = rd16(img + 11);
    b->spc = img[13];
    b->rsvd = rd16(img + 14);
    b->nfats = img[16];
    b->root_ents = rd16(img + 17);
    b->total = rd16(img + 19) ? rd16(img + 19) : rd32(img + 32);
    b->fatsz = rd16(img + 22);
    b->fat_off = b->rsvd * b->bps;
    b->root_off = b->fat_off + b->nfats * b->fatsz * b->bps;
    const uint32_t root_secs = (b->root_ents * 32u + b->bps - 1u) / b->bps;
    b->data_off = b->root_off + root_secs * b->bps;
    /* Microsoft FAT spec: CountofClusters decides the FAT type */
    b->clusters = (b->total - (b->rsvd + b->nfats * b->fatsz + root_secs)) / b->spc;
}

/* FAT12 entry n straight from the packed bytes of FAT copy `copy`. */
static uint16_t fat12_get(const uint8_t *img, const bpb_t *b, uint32_t copy, uint32_t n)
{
    const uint8_t *fat = img + b->fat_off + copy * b->fatsz * b->bps;
    const uint32_t o = n + n / 2u;
    const uint16_t w = (uint16_t)(fat[o] | (fat[o + 1] << 8));
    return (n & 1u) ? (uint16_t)(w >> 4) : (uint16_t)(w & 0x0FFFu);
}

/* Reassemble a file by its chain; returns bytes copied, -1 on a bad chain. */
static int32_t read_file(const uint8_t *img, const bpb_t *b, uint16_t first, uint32_t size,
                         uint8_t *out, uint32_t *nclusters)
{
    const uint32_t csize = b->spc * b->bps;
    uint32_t got = 0, guard = 0;
    uint16_t c = first;
    *nclusters = 0;
    while (got < size) {
        if (c < 2u || c >= 2u + b->clusters || ++guard > b->clusters) return -1;
        uint32_t n = size - got < csize ? size - got : csize;
        memcpy(out + got, img + b->data_off + (uint32_t)(c - 2u) * csize, n);
        got += n;
        (*nclusters)++;
        uint16_t next = fat12_get(img, b, 0, c);
        if (got >= size) {
            if (next < 0xFF8u) return -1;      /* chain must end exactly at EOF */
            break;
        }
        c = next;
    }
    return (int32_t)got;
}

/* ── vfat tests ───────────────────────────────────────────────────────── */

static void test_boot_sector(void)
{
    fresh();
    gallery_vfat_t v;
    build(&v);
    uint8_t s[512];
    CHECK(gallery_vfat_read_sector(&v, 0, s, read_slot, NULL) == VFAT_OK, "lba 0");
    CHECK(s[0] == 0xEB && s[2] == 0x90, "jump");
    CHECK(rd16(s + 11) == 512, "bytes/sector %u", rd16(s + 11));
    CHECK(s[13] == 8, "sectors/cluster %u", s[13]);
    CHECK(rd16(s + 14) == 1, "reserved");
    CHECK(s[16] == 2, "fats");
    CHECK(rd16(s + 17) == 32, "root entries");
    CHECK(rd16(s + 19) == 1024, "total sectors");
    CHECK(s[21] == 0xF8, "media");
    CHECK(rd16(s + 22) == 1, "fat size");
    CHECK(rd32(s + 28) == 0 && rd32(s + 32) == 0, "hidden/total32");
    CHECK(s[38] == 0x29, "ext signature");
    CHECK(memcmp(s + 43, "OPENSCOPE  ", 11) == 0, "label");
    CHECK(memcmp(s + 54, "FAT12   ", 8) == 0, "fs type");
    CHECK(s[510] == 0x55 && s[511] == 0xAA, "signature");
    bpb_t b;
    parse_bpb(s, &b);
    CHECK(b.clusters == VFAT_DATA_CLUSTERS && b.clusters < 4085u, "cluster count %u (FAT12 needs < 4085)", b.clusters);
    CHECK(b.data_off == VFAT_DATA_LBA * 512u, "data region at lba %u", b.data_off / 512u);
    CHECK((b.clusters + 2u) * 3u / 2u <= b.fatsz * b.bps, "FAT holds every cluster");
    CHECK(b.clusters >= GALLERY_MAX_SLOTS * VFAT_CLUSTERS_PER_SLOT, "room for every slot");
}

static void test_empty_volume(void)
{
    fresh();
    gallery_vfat_t v;
    build(&v);
    CHECK(v.count == 0, "no shots");
    read_image(&v);
    bpb_t b;
    parse_bpb(image, &b);
    CHECK(fat12_get(image, &b, 0, 0) == 0xFF8 && fat12_get(image, &b, 0, 1) == 0xFFF, "reserved entries");
    for (uint32_t n = 2; n < 2u + b.clusters; n++)
        CHECK(fat12_get(image, &b, 0, n) == 0, "cluster %u free", n);
    const uint8_t *root = image + b.root_off;
    CHECK(memcmp(root, "OPENSCOPE  ", 11) == 0 && root[11] == 0x08, "label entry");
    CHECK(root[32] == 0, "listing ends after the label");
    for (uint32_t i = b.data_off; i < sizeof image; i++)
        if (image[i]) { CHECK(0, "data byte %u not zero", i); break; }
}

static void test_files_roundtrip(void)
{
    fresh();
    uint32_t seqs[4];
    for (uint32_t i = 0; i < 4; i++) seqs[i] = save_shot(i + 1u);
    CHECK(gallery_delete(seqs[1]) == GALLERY_OK, "delete 2");   /* leave a hole in slot 1 */
    gallery_vfat_t v;
    build(&v);
    CHECK(v.count == 3, "3 shots listed, got %u", v.count);
    read_image(&v);
    bpb_t b;
    parse_bpb(image, &b);

    /* both FAT copies identical */
    CHECK(memcmp(image + b.fat_off, image + b.fat_off + b.fatsz * b.bps, b.fatsz * b.bps) == 0, "FAT copies");

    const uint8_t *root = image + b.root_off;
    const uint32_t want_seq[3] = { seqs[0], seqs[2], seqs[3] };
    static uint8_t file[GALLERY_BMP_LEN], ref[GALLERY_BMP_LEN];
    for (uint32_t f = 0; f < 3; f++) {
        const uint8_t *e = root + 32u * (f + 1u);
        char want[12];
        snprintf(want, sizeof want, "%-8uBMP", (unsigned)want_seq[f]);
        CHECK(memcmp(e, want, 11) == 0, "entry %u name '%.11s' want '%s'", f, e, want);
        CHECK(e[11] == 0x21, "entry %u attr 0x%02X (read-only+archive)", f, e[11]);
        CHECK(e[12] == 0x18, "entry %u lower-case flags", f);
        CHECK(rd32(e + 28) == GALLERY_BMP_LEN, "entry %u size %u", f, rd32(e + 28));
        CHECK(rd16(e + 20) == 0, "cluster high");
        uint32_t slot = 0;
        CHECK(gallery_find(want_seq[f], &slot) == GALLERY_OK, "find");
        CHECK(rd16(e + 26) == 2u + slot * 10u, "entry %u first cluster %u (slot %u)", f, rd16(e + 26), slot);

        uint32_t nc = 0;
        int32_t got = read_file(image, &b, rd16(e + 26), rd32(e + 28), file, &nc);
        CHECK(got == (int32_t)GALLERY_BMP_LEN, "file %u read %d", f, got);
        CHECK(nc == 10, "file %u spans %u clusters", f, nc);
        CHECK(gallery_read_bmp(slot, 0, ref, GALLERY_BMP_LEN) == GALLERY_OK, "ref read");
        CHECK(memcmp(file, ref, GALLERY_BMP_LEN) == 0, "file %u bytes differ from the slot", f);
        gallery_slot_info_t info;
        (void)gallery_slot_info(slot, &info);
        uint32_t crc = cal_backup_crc32_final(cal_backup_crc32_update(CAL_BACKUP_CRC32_INIT, file, GALLERY_BMP_LEN));
        CHECK(crc == info.bmp_crc, "file %u crc %08X want %08X", f, crc, info.bmp_crc);
        CHECK(file[0] == 'B' && file[1] == 'M', "a BMP");
    }
    CHECK(root[32u * 4u] == 0, "listing ends after the files");

    /* the deleted shot's slot: free chain, zero data */
    uint32_t hole = 1;
    for (uint32_t k = 0; k < 10; k++)
        CHECK(fat12_get(image, &b, 0, 2u + hole * 10u + k) == 0, "deleted slot cluster %u free", k);
    const uint8_t *hd = image + b.data_off + hole * 10u * 4096u;
    bool zero = true;
    for (uint32_t i = 0; i < 40960u; i++) if (hd[i]) { zero = false; break; }
    CHECK(zero, "deleted shot's clusters read zero (no stale pixels)");

    /* the generated FAT agrees with the accessor, entry by entry */
    for (uint32_t n = 0; n < 2u + b.clusters; n++)
        CHECK(fat12_get(image, &b, 1, n) == gallery_vfat_fat_entry(&v, n), "FAT entry %u", n);

    const char *out = getenv("VFAT_IMAGE_OUT");
    if (out && *out) {
        FILE *fp = fopen(out, "wb");
        if (fp) { fwrite(image, 1, sizeof image, fp); fclose(fp); printf("      image -> %s\n", out); }
    }
}

static void test_arbitrary_lbas(void)
{
    fresh();
    for (uint32_t i = 0; i < 9; i++) save_shot(10u + i);   /* every slot full */
    gallery_vfat_t v;
    build(&v);
    CHECK(v.count == 9, "9 listed");
    uint8_t s[512], ref[512];
    /* slot 4, cluster 7, sector 3: an interior sector */
    uint32_t slot = 4, cl = 7, sec = 3;
    uint32_t lba = VFAT_DATA_LBA + (slot * 10u + cl) * 8u + sec;
    CHECK(gallery_vfat_read_sector(&v, lba, s, read_slot, NULL) == VFAT_OK, "interior");
    CHECK(gallery_read_bmp(slot, cl * 4096u + sec * 512u, ref, 512) == GALLERY_OK, "ref");
    CHECK(memcmp(s, ref, 512) == 0, "interior sector");
    /* the sector holding EOF: 38518 = 75 * 512 + 118 */
    lba = VFAT_DATA_LBA + slot * 80u + 75u;
    CHECK(gallery_vfat_read_sector(&v, lba, s, read_slot, NULL) == VFAT_OK, "eof sector");
    CHECK(gallery_read_bmp(slot, 75u * 512u, ref, 118) == GALLERY_OK, "ref tail");
    CHECK(memcmp(s, ref, 118) == 0, "tail bytes");
    bool z = true;
    for (uint32_t i = 118; i < 512; i++) if (s[i]) z = false;
    CHECK(z, "zero after EOF in the last sector");
    /* slack sectors 76..79 of the last cluster */
    for (uint32_t k = 76; k < 80; k++) {
        CHECK(gallery_vfat_read_sector(&v, VFAT_DATA_LBA + slot * 80u + k, s, read_slot, NULL) == VFAT_OK, "slack");
        z = true;
        for (uint32_t i = 0; i < 512; i++) if (s[i]) z = false;
        CHECK(z, "slack sector %u zero", k);
    }
    /* first and last sector of every slot's file */
    for (uint32_t k = 0; k < 9; k++) {
        CHECK(gallery_vfat_read_sector(&v, VFAT_DATA_LBA + k * 80u, s, read_slot, NULL) == VFAT_OK, "first");
        CHECK(s[0] == 'B' && s[1] == 'M' && rd32(s + 2) == GALLERY_BMP_LEN, "slot %u starts with its BMP header", k);
    }
    /* clusters beyond the nine slots, and the trailing sectors past the last cluster */
    for (uint32_t l = VFAT_DATA_LBA + 9u * 80u; l < VFAT_TOTAL_SECTORS; l++) {
        memset(s, 0xAB, sizeof s);
        CHECK(gallery_vfat_read_sector(&v, l, s, read_slot, NULL) == VFAT_OK, "lba %u", l);
        z = true;
        for (uint32_t i = 0; i < 512; i++) if (s[i]) z = false;
        if (!z) { CHECK(0, "unallocated lba %u not zero", l); break; }
    }
    CHECK(gallery_vfat_read_sector(&v, VFAT_TOTAL_SECTORS, s, read_slot, NULL) == VFAT_ERR_RANGE, "lba = total");
    CHECK(gallery_vfat_read_sector(&v, 0xFFFFFFFFu, s, read_slot, NULL) == VFAT_ERR_RANGE, "lba max");

    /* a full volume, end to end through the independent reader */
    read_image(&v);
    bpb_t b;
    parse_bpb(image, &b);
    uint32_t listed = 0;
    static uint8_t file[GALLERY_BMP_LEN];
    for (uint32_t i = 1; i < b.root_ents; i++) {
        const uint8_t *e = image + b.root_off + 32u * i;
        if (e[0] == 0) break;
        uint32_t nc;
        CHECK(read_file(image, &b, rd16(e + 26), rd32(e + 28), file, &nc) == (int32_t)GALLERY_BMP_LEN, "file %u", i);
        listed++;
    }
    CHECK(listed == 9, "9 files walked, got %u", listed);
}

static void test_io_error(void)
{
    fresh();
    save_shot(1);
    gallery_vfat_t v;
    build(&v);
    uint8_t s[512];
    fail_reads = true;
    memset(s, 0xAB, sizeof s);
    CHECK(gallery_vfat_read_sector(&v, VFAT_DATA_LBA, s, read_slot, NULL) == VFAT_ERR_IO, "io error surfaces");
    bool z = true;
    for (uint32_t i = 0; i < 512; i++) if (s[i]) z = false;
    CHECK(z, "failed sector is zeroed, not stale");
    /* metadata needs no flash */
    CHECK(gallery_vfat_read_sector(&v, 0, s, read_slot, NULL) == VFAT_OK, "boot sector without flash");
    CHECK(gallery_vfat_read_sector(&v, VFAT_ROOT_LBA, s, read_slot, NULL) == VFAT_OK, "root without flash");
    CHECK(gallery_vfat_read_sector(&v, VFAT_DATA_LBA, s, NULL, NULL) == VFAT_ERR_IO, "no callback");
    fail_reads = false;
}

static void test_names_and_build(void)
{
    uint8_t n[11];
    gallery_vfat_short_name(1, 0, n);
    CHECK(memcmp(n, "1       BMP", 11) == 0, "1");
    gallery_vfat_short_name(12345678, 3, n);
    CHECK(memcmp(n, "12345678BMP", 11) == 0, "8 digits");
    gallery_vfat_short_name(99999999, 3, n);
    CHECK(memcmp(n, "99999999BMP", 11) == 0, "max 8.3");
    gallery_vfat_short_name(100000000, 7, n);
    CHECK(memcmp(n, "SHOT~7  BMP", 11) == 0, "9 digits fall back, got '%.11s'", n);

    gallery_index_t idx;
    memset(&idx, 0, sizeof idx);
    idx.count = 4;
    idx.slot[0] = 2; idx.seq[0] = 30;
    idx.slot[1] = 0; idx.seq[1] = 7;
    idx.slot[2] = 2; idx.seq[2] = 31;      /* duplicate slot: listed once */
    idx.slot[3] = 12; idx.seq[3] = 99;     /* no such slot */
    gallery_vfat_t v;
    gallery_vfat_build(&v, &idx);
    CHECK(v.count == 2, "dedup + bound, got %u", v.count);
    CHECK(v.seq[0] < v.seq[1], "ascending");
    CHECK(v.slot[0] == 0 && v.seq[0] == 7, "first");
    gallery_vfat_build(&v, NULL);
    CHECK(v.count == 0, "null index");
    /* FAT chain accessor at the edges */
    gallery_vfat_build(&v, &idx);
    CHECK(gallery_vfat_fat_entry(&v, 2) == 3, "slot 0 chain start");
    CHECK(gallery_vfat_fat_entry(&v, 11) == 0xFFF, "slot 0 chain end");
    CHECK(gallery_vfat_fat_entry(&v, 12) == 0, "slot 1 free");
    CHECK(gallery_vfat_fat_entry(&v, 22) == 23 && gallery_vfat_fat_entry(&v, 31) == 0xFFF, "slot 2");
    CHECK(gallery_vfat_fat_entry(&v, 2u + VFAT_DATA_CLUSTERS) == 0, "past the end");
}

static void test_generation(void)
{
    fresh();
    uint32_t g0 = gallery_generation();
    uint32_t seq = save_shot(5);
    uint32_t g1 = gallery_generation();
    CHECK(g1 != g0, "save bumps the generation");
    CHECK(gallery_delete(seq) == GALLERY_OK, "delete");
    CHECK(gallery_generation() != g1, "delete bumps the generation");
    uint32_t g2 = gallery_generation();
    gallery_index_t idx;
    (void)gallery_scan(&idx);
    CHECK(gallery_generation() == g2, "a scan does not");
}

/* ── SCSI / BOT tests ─────────────────────────────────────────────────── */

static void mk_cbw(uint8_t *b, uint32_t tag, uint32_t len, bool in, const uint8_t *cb, uint8_t cblen)
{
    memset(b, 0, 31);
    b[0] = 'U'; b[1] = 'S'; b[2] = 'B'; b[3] = 'C';
    b[4] = (uint8_t)tag; b[5] = (uint8_t)(tag >> 8); b[6] = (uint8_t)(tag >> 16); b[7] = (uint8_t)(tag >> 24);
    b[8] = (uint8_t)len; b[9] = (uint8_t)(len >> 8); b[10] = (uint8_t)(len >> 16); b[11] = (uint8_t)(len >> 24);
    b[12] = in ? 0x80 : 0x00;
    b[14] = cblen;
    memcpy(b + 15, cb, cblen);
}

static msc_lun_t lun;

static void cmd(const uint8_t *cb, uint8_t cblen, uint32_t H, bool in, msc_result_t *r, msc_plan_t *p)
{
    uint8_t raw[31];
    msc_cbw_t c;
    mk_cbw(raw, 0x1234, H, in, cb, cblen);
    CHECK(msc_cbw_parse(raw, 31, 0, &c), "cbw parse");
    msc_scsi_command(&lun, &c, r);
    msc_plan(&c, r, p);
}

static void sense(uint8_t *key, uint8_t *asc)
{
    const uint8_t rs[6] = { 0x03, 0, 0, 0, 18, 0 };
    msc_result_t r; msc_plan_t p;
    cmd(rs, 6, 18, true, &r, &p);
    CHECK(r.ok && r.xfer == MSC_XFER_IN_BUF && r.len == 18 && r.buf[0] == 0x70, "request sense");
    *key = r.buf[2]; *asc = r.buf[12];
}

static void test_cbw_parse(void)
{
    uint8_t b[64];
    msc_cbw_t c;
    const uint8_t tur[6] = { 0 };
    mk_cbw(b, 0xA1B2C3D4u, 36, true, tur, 6);
    CHECK(msc_cbw_parse(b, 31, 0, &c), "valid");
    CHECK(c.tag == 0xA1B2C3D4u && c.data_len == 36 && c.dir_in && c.cb_len == 6, "fields");
    CHECK(!msc_cbw_parse(b, 30, 0, &c), "short");
    CHECK(!msc_cbw_parse(b, 32, 0, &c), "long");
    CHECK(!msc_cbw_parse(b, 64, 0, &c), "full packet");
    b[0] = 'X';
    CHECK(!msc_cbw_parse(b, 31, 0, &c), "signature");
    mk_cbw(b, 1, 0, false, tur, 6); b[13] = 1;
    CHECK(!msc_cbw_parse(b, 31, 0, &c), "lun 1 with one LUN");
    mk_cbw(b, 1, 0, false, tur, 6); b[14] = 0;
    CHECK(!msc_cbw_parse(b, 31, 0, &c), "cb length 0");
    mk_cbw(b, 1, 0, false, tur, 6); b[14] = 17;
    CHECK(!msc_cbw_parse(b, 31, 0, &c), "cb length 17");
    mk_cbw(b, 1, 0, false, tur, 6); b[12] = 0x40;
    CHECK(!msc_cbw_parse(b, 31, 0, &c), "reserved flag bits");
}

static void test_scsi_medium_states(void)
{
    msc_lun_init(&lun, VFAT_TOTAL_SECTORS, VFAT_SECTOR_SIZE);
    msc_result_t r; msc_plan_t p;
    uint8_t k, a;
    const uint8_t tur[6] = { 0x00 };

    cmd(tur, 6, 0, false, &r, &p);
    CHECK(!r.ok && p.status == MSC_CSW_FAIL, "no medium: TUR fails");
    sense(&k, &a);
    CHECK(k == SCSI_SK_NOT_READY && a == SCSI_ASC_MEDIUM_NOT_PRESENT, "sense %02X/%02X", k, a);

    const uint8_t rc[10] = { 0x25 };
    cmd(rc, 10, 8, true, &r, &p);
    CHECK(!r.ok && p.pad == 8 && p.residue == 8 && p.status == MSC_CSW_FAIL, "capacity refused, filled");

    msc_lun_set_present(&lun, true);
    cmd(tur, 6, 0, false, &r, &p);
    CHECK(!r.ok, "UNIT ATTENTION once");
    sense(&k, &a);
    CHECK(k == SCSI_SK_UNIT_ATTENTION && a == SCSI_ASC_MEDIUM_CHANGED, "UA sense %02X/%02X", k, a);
    cmd(tur, 6, 0, false, &r, &p);
    CHECK(r.ok && p.status == MSC_CSW_PASS, "then ready");

    msc_lun_changed(&lun);
    sense(&k, &a);   /* REQUEST SENSE alone reports (and consumes) the UA */
    CHECK(k == SCSI_SK_UNIT_ATTENTION, "UA via REQUEST SENSE");
    cmd(tur, 6, 0, false, &r, &p);
    CHECK(r.ok, "consumed");

    cmd(rc, 10, 8, true, &r, &p);
    CHECK(r.ok && r.len == 8 && p.send == 8 && p.pad == 0, "capacity");
    CHECK(r.buf[0] == 0 && r.buf[1] == 0 && r.buf[2] == 0x03 && r.buf[3] == 0xFF, "last lba 1023");
    CHECK(r.buf[6] == 0x02 && r.buf[7] == 0x00, "block 512");

    /* eject from the host */
    const uint8_t ss[6] = { 0x1B, 0, 0, 0, 0x02, 0 };
    cmd(ss, 6, 0, false, &r, &p);
    CHECK(r.ok && !lun.present && lun.eject_request, "eject");
    cmd(tur, 6, 0, false, &r, &p);
    CHECK(!r.ok, "gone after eject");
    msc_lun_changed(&lun);
    CHECK(!lun.unit_attention, "no UA queued without a medium");
}

static void test_scsi_commands(void)
{
    msc_lun_init(&lun, VFAT_TOTAL_SECTORS, VFAT_SECTOR_SIZE);
    msc_lun_set_present(&lun, true);
    lun.unit_attention = false;
    msc_result_t r; msc_plan_t p;
    uint8_t k, a;

    const uint8_t inq[6] = { 0x12, 0, 0, 0, 36, 0 };
    cmd(inq, 6, 36, true, &r, &p);
    CHECK(r.ok && r.len == 36 && p.send == 36 && p.residue == 0, "inquiry");
    CHECK(r.buf[0] == 0 && r.buf[1] == 0x80 && r.buf[4] == 31, "direct access, removable");
    CHECK(memcmp(r.buf + 8, "OpenScop", 8) == 0, "vendor");
    const uint8_t inq5[6] = { 0x12, 0, 0, 0, 5, 0 };
    cmd(inq5, 6, 5, true, &r, &p);
    CHECK(r.len == 5, "allocation length clamps");
    const uint8_t evpd[6] = { 0x12, 1, 0x80, 0, 255, 0 };
    cmd(evpd, 6, 255, true, &r, &p);
    CHECK(!r.ok, "no VPD pages");

    const uint8_t ms6[6] = { 0x1A, 0, 0x3F, 0, 192, 0 };
    cmd(ms6, 6, 192, true, &r, &p);
    CHECK(r.ok && r.len == 4 && (r.buf[2] & 0x80), "mode sense(6) WP");
    CHECK(p.send == 4 && p.pad == 0 && p.residue == 188, "short reply, short packet");
    const uint8_t ms10[10] = { 0x5A, 0, 0x3F, 0, 0, 0, 0, 0, 0xFF, 0 };
    cmd(ms10, 10, 255, true, &r, &p);
    CHECK(r.ok && r.len == 8 && (r.buf[3] & 0x80) && r.buf[1] == 6, "mode sense(10) WP");

    const uint8_t rfc[10] = { 0x23, 0, 0, 0, 0, 0, 0, 0, 0xFC, 0 };
    cmd(rfc, 10, 0xFC, true, &r, &p);
    CHECK(r.ok && r.len == 12 && r.buf[3] == 8 && r.buf[8] == 0x02, "read format capacities");

    const uint8_t rd[10] = { 0x28, 0, 0, 0, 0x01, 0x00, 0, 0x00, 0x80, 0 };   /* lba 256, 128 blocks */
    cmd(rd, 10, 128u * 512u, true, &r, &p);
    CHECK(r.ok && r.xfer == MSC_XFER_IN_READ && r.lba == 256 && r.blocks == 128, "read(10)");
    CHECK(p.send == 65536 && p.pad == 0 && p.residue == 0 && p.status == MSC_CSW_PASS, "read plan");
    const uint8_t rd_end[10] = { 0x28, 0, 0, 0, 0x03, 0xFF, 0, 0x00, 0x01, 0 };  /* last block */
    cmd(rd_end, 10, 512, true, &r, &p);
    CHECK(r.ok, "last block readable");
    const uint8_t rd_oor[10] = { 0x28, 0, 0, 0, 0x03, 0xFF, 0, 0x00, 0x02, 0 };  /* one past */
    cmd(rd_oor, 10, 1024, true, &r, &p);
    CHECK(!r.ok && p.send == 0 && p.pad == 1024 && p.residue == 1024, "past the end refused, filled");
    sense(&k, &a);
    CHECK(k == SCSI_SK_ILLEGAL_REQUEST && a == SCSI_ASC_LBA_OUT_OF_RANGE, "range sense");
    const uint8_t rd12[12] = { 0xA8, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 2, 0, 0 };
    cmd(rd12, 12, 1024, true, &r, &p);
    CHECK(!r.ok, "read(12) lba overflow refused");

    const uint8_t wr[10] = { 0x2A, 0, 0, 0, 0, 0, 0, 0, 8, 0 };
    cmd(wr, 10, 4096, false, &r, &p);
    CHECK(!r.ok && r.xfer == MSC_XFER_OUT && r.len == 4096, "write refused");
    CHECK(p.recv == 4096 && p.send == 0 && p.residue == 4096 && p.status == MSC_CSW_FAIL, "write data dropped");
    sense(&k, &a);
    CHECK(k == SCSI_SK_DATA_PROTECT && a == SCSI_ASC_WRITE_PROTECTED, "write-protect sense %02X/%02X", k, a);
    const uint8_t wr12[12] = { 0xAA, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0 };
    cmd(wr12, 12, 512, false, &r, &p);
    CHECK(!r.ok && r.len == 0xFFFFFFFFu && p.status == MSC_CSW_PHASE_ERROR, "write(12) huge count: no overflow, phase error");
    const uint8_t fmt[6] = { 0x04 };
    cmd(fmt, 6, 0, false, &r, &p);
    CHECK(!r.ok, "format refused");

    const uint8_t unk[16] = { 0x9E, 0x10 };   /* READ CAPACITY(16) */
    cmd(unk, 16, 32, true, &r, &p);
    CHECK(!r.ok && p.pad == 32 && p.status == MSC_CSW_FAIL, "unknown op: filled + failed");
    sense(&k, &a);
    CHECK(k == SCSI_SK_ILLEGAL_REQUEST && a == SCSI_ASC_INVALID_OPCODE, "invalid opcode");
    sense(&k, &a);
    CHECK(k == SCSI_SK_NO_SENSE, "sense cleared after being read");

    const uint8_t pa[6] = { 0x1E, 0, 0, 0, 1, 0 }, sc[10] = { 0x35 }, vf[10] = { 0x2F };
    cmd(pa, 6, 0, false, &r, &p);  CHECK(r.ok, "prevent removal accepted");
    cmd(sc, 10, 0, false, &r, &p); CHECK(r.ok, "sync cache");
    cmd(vf, 10, 0, false, &r, &p); CHECK(r.ok, "verify");
}

static void test_plan_cases(void)
{
    msc_cbw_t c;
    msc_result_t r;
    msc_plan_t p;
    memset(&c, 0, sizeof c);
    memset(&r, 0, sizeof r);

    /* case 1 Hn = Dn */
    r.ok = true; r.xfer = MSC_XFER_NONE; c.data_len = 0;
    msc_plan(&c, &r, &p);
    CHECK(p.send == 0 && p.pad == 0 && p.recv == 0 && p.status == MSC_CSW_PASS, "case 1");
    /* case 2 Hn < Di */
    r.xfer = MSC_XFER_IN_BUF; r.len = 18;
    msc_plan(&c, &r, &p);
    CHECK(p.send == 0 && p.status == MSC_CSW_PHASE_ERROR, "case 2");
    /* case 6 Hi = Di */
    c.dir_in = true; c.data_len = 18;
    msc_plan(&c, &r, &p);
    CHECK(p.send == 18 && p.pad == 0 && p.residue == 0 && p.status == MSC_CSW_PASS, "case 6");
    /* case 5 Hi > Di, short packet ends it */
    c.data_len = 252;
    msc_plan(&c, &r, &p);
    CHECK(p.send == 18 && p.pad == 0 && p.residue == 234, "case 5 short");
    /* case 5 with a whole-packet shortfall: fill to H */
    r.len = 128; c.data_len = 512;
    msc_plan(&c, &r, &p);
    CHECK(p.send == 128 && p.pad == 384 && p.residue == 384, "case 5 packet-multiple: fill");
    /* case 4 Hi > Dn */
    r.xfer = MSC_XFER_NONE; r.ok = false;
    msc_plan(&c, &r, &p);
    CHECK(p.send == 0 && p.pad == 512 && p.residue == 512 && p.status == MSC_CSW_FAIL, "case 4");
    /* case 7 Hi < Di */
    r.xfer = MSC_XFER_IN_READ; r.ok = true; r.len = 1024; c.data_len = 600;
    msc_plan(&c, &r, &p);
    CHECK(p.send == 600 && p.pad == 0 && p.residue == 0 && p.status == MSC_CSW_PHASE_ERROR, "case 7");
    /* case 8 Ho <> Di */
    c.dir_in = false; c.data_len = 512;
    msc_plan(&c, &r, &p);
    CHECK(p.recv == 512 && p.send == 0 && p.status == MSC_CSW_PHASE_ERROR, "case 8");
    /* case 9 Ho > Dn */
    r.xfer = MSC_XFER_NONE; r.ok = false;
    msc_plan(&c, &r, &p);
    CHECK(p.recv == 512 && p.residue == 512 && p.status == MSC_CSW_FAIL, "case 9");
    /* case 10 Hi <> Do */
    c.dir_in = true; r.xfer = MSC_XFER_OUT; r.len = 512;
    msc_plan(&c, &r, &p);
    CHECK(p.pad == 512 && p.recv == 0 && p.status == MSC_CSW_PHASE_ERROR, "case 10");
    /* case 3 Hn < Do */
    c.data_len = 0; c.dir_in = false;
    msc_plan(&c, &r, &p);
    CHECK(p.recv == 0 && p.status == MSC_CSW_PHASE_ERROR, "case 3");

    uint8_t csw[13];
    msc_csw_build(csw, 0xDEADBEEFu, 0x1234u, MSC_CSW_FAIL);
    CHECK(memcmp(csw, "USBS", 4) == 0 && rd32(csw + 4) == 0xDEADBEEFu && rd32(csw + 8) == 0x1234u && csw[12] == 1, "csw");
}

int main(void)
{
    printf("test_msc: virtual FAT12 gallery volume + SCSI/BOT layer\n");
    run("boot sector / BPB", test_boot_sector);
    run("empty volume", test_empty_volume);
    run("files round-trip through an independent FAT12 reader", test_files_roundtrip);
    run("arbitrary LBAs, EOF, slack, unallocated, range", test_arbitrary_lbas);
    run("read errors", test_io_error);
    run("names, index build, FAT accessor", test_names_and_build);
    run("gallery generation counter", test_generation);
    run("CBW validation", test_cbw_parse);
    run("medium states: no medium, UA, eject", test_scsi_medium_states);
    run("SCSI commands, read-only refusals", test_scsi_commands);
    run("BOT 13-case plan, CSW", test_plan_cases);
    printf("%d/%d passed\n", tests_run - tests_failed, tests_run);
    free(mem);
    return tests_failed ? 1 : 0;
}
