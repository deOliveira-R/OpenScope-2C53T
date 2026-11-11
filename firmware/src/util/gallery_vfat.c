/*
 * gallery_vfat.c — see gallery_vfat.h for the geometry and why no real FAT
 * is involved.
 */
#include "gallery_vfat.h"

#include <string.h>

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* A fixed timestamp: the gallery header records no time (no RTC worth
 * trusting), and a constant keeps every read of the volume identical.
 * 2026-01-01 12:00:00 in FAT encoding. */
#define VFAT_DATE   (uint16_t)(((2026u - 1980u) << 9) | (1u << 5) | 1u)
#define VFAT_TIME   (uint16_t)(12u << 11)

void gallery_vfat_build(gallery_vfat_t *v, const gallery_index_t *idx)
{
    memset(v, 0, sizeof *v);
    if (idx == NULL) return;
    /* gallery_scan() lists newest first; the volume lists ascending. */
    for (uint32_t i = idx->count; i > 0u; i--) {
        uint32_t k = i - 1u;
        if (k >= GALLERY_MAX_SLOTS || idx->slot[k] >= GALLERY_MAX_SLOTS) continue;
        bool dup = false;
        for (uint32_t j = 0; j < v->count; j++) {
            if (v->slot[j] == idx->slot[k]) { dup = true; break; }
        }
        if (dup) continue;    /* a slot owns one cluster run: list it once */
        /* insertion keeps ascending order even if idx was not sorted */
        uint32_t p = v->count;
        while (p > 0u && v->seq[p - 1u] > idx->seq[k]) {
            v->seq[p] = v->seq[p - 1u];
            v->slot[p] = v->slot[p - 1u];
            p--;
        }
        v->seq[p] = idx->seq[k];
        v->slot[p] = idx->slot[k];
        v->count++;
    }
}

void gallery_vfat_short_name(uint32_t seq, uint32_t slot, uint8_t out[11])
{
    memset(out, ' ', 11);
    memcpy(out + 8, "BMP", 3);
    if (seq > 99999999u) {
        memcpy(out, "SHOT~", 5);
        out[5] = (uint8_t)('0' + (slot % 10u));
        return;
    }
    char digits[8];
    uint32_t n = 0;
    do {
        digits[n++] = (char)('0' + (seq % 10u));
        seq /= 10u;
    } while (seq != 0u);
    for (uint32_t i = 0; i < n; i++) out[i] = (uint8_t)digits[n - 1u - i];
}

/* Which listed file (index into v) owns slot s, or -1. */
static int file_of_slot(const gallery_vfat_t *v, uint32_t s)
{
    for (uint32_t i = 0; i < v->count; i++) {
        if (v->slot[i] == s) return (int)i;
    }
    return -1;
}

static uint32_t first_cluster(uint32_t slot) { return 2u + slot * VFAT_CLUSTERS_PER_SLOT; }

uint16_t gallery_vfat_fat_entry(const gallery_vfat_t *v, uint32_t n)
{
    if (n == 0u) return (uint16_t)(0xF00u | VFAT_MEDIA);
    if (n == 1u) return 0xFFFu;
    if (n >= 2u + VFAT_DATA_CLUSTERS) return 0u;
    const uint32_t slot = (n - 2u) / VFAT_CLUSTERS_PER_SLOT;
    const uint32_t k = (n - 2u) % VFAT_CLUSTERS_PER_SLOT;
    if (slot >= GALLERY_MAX_SLOTS || file_of_slot(v, slot) < 0) return 0u;   /* free */
    if (k >= VFAT_FILE_CLUSTERS) return 0u;
    if (k == VFAT_FILE_CLUSTERS - 1u) return 0xFFFu;                       /* end of chain */
    return (uint16_t)(n + 1u);
}

/* Byte b of the packed FAT12 table: entry 2j fills byte 3j and the low
 * nibble of 3j+1, entry 2j+1 the high nibble of 3j+1 and byte 3j+2. */
static uint8_t fat_byte(const gallery_vfat_t *v, uint32_t b)
{
    const uint32_t j = b / 3u;
    switch (b % 3u) {
    case 0:  return (uint8_t)gallery_vfat_fat_entry(v, 2u * j);
    case 1:  return (uint8_t)(((gallery_vfat_fat_entry(v, 2u * j) >> 8) & 0x0Fu) |
                              ((gallery_vfat_fat_entry(v, 2u * j + 1u) & 0x0Fu) << 4));
    default: return (uint8_t)(gallery_vfat_fat_entry(v, 2u * j + 1u) >> 4);
    }
}

static void boot_sector(uint8_t *o)
{
    o[0] = 0xEB; o[1] = 0x3C; o[2] = 0x90;            /* jmp short; nop */
    memcpy(o + 3, "MSDOS5.0", 8);                       /* the OEM name hosts expect */
    put16(o + 11, VFAT_SECTOR_SIZE);
    o[13] = VFAT_SECTORS_PER_CLUSTER;
    put16(o + 14, VFAT_RESERVED_SECTORS);
    o[16] = VFAT_NUM_FATS;
    put16(o + 17, VFAT_ROOT_ENTRIES);
    put16(o + 19, VFAT_TOTAL_SECTORS);                  /* < 65536: 16-bit field */
    o[21] = VFAT_MEDIA;
    put16(o + 22, VFAT_FAT_SECTORS);
    put16(o + 24, 32u);                                 /* sectors per track (CHS, cosmetic) */
    put16(o + 26, 2u);                                  /* heads */
    put32(o + 28, 0u);                                  /* hidden sectors: no partition table */
    put32(o + 32, 0u);                                  /* total sectors (32-bit) unused */
    o[36] = 0x80;                                       /* drive number */
    o[38] = 0x29;                                       /* extended boot signature */
    put32(o + 39, VFAT_VOLUME_ID);
    memcpy(o + 43, "OPENSCOPE  ", 11);
    memcpy(o + 54, "FAT12   ", 8);
    o[62] = 0xF4;                                       /* hlt: never booted */
    o[510] = 0x55; o[511] = 0xAA;
}

static void dir_entry(uint8_t *e, const uint8_t name[11], uint8_t attr, uint8_t ntres,
                      uint16_t cluster, uint32_t size)
{
    memcpy(e, name, 11);
    e[11] = attr;
    e[12] = ntres;
    put16(e + 14, VFAT_TIME);       /* created */
    put16(e + 16, VFAT_DATE);
    put16(e + 18, VFAT_DATE);       /* accessed */
    put16(e + 20, 0u);              /* cluster high (FAT32 only) */
    put16(e + 22, VFAT_TIME);       /* modified */
    put16(e + 24, VFAT_DATE);
    put16(e + 26, cluster);
    put32(e + 28, size);
}

/* Root directory entry i: 0 = the volume label, 1..count = the files, then
 * zeroes (a 0x00 first byte ends the listing). */
static void root_entry(const gallery_vfat_t *v, uint32_t i, uint8_t *e)
{
    memset(e, 0, 32);
    if (i == 0u) {
        dir_entry(e, (const uint8_t *)"OPENSCOPE  ", 0x08u, 0u, 0u, 0u);
        return;
    }
    if (i - 1u >= v->count) return;
    const uint32_t f = i - 1u;
    uint8_t name[11];
    gallery_vfat_short_name(v->seq[f], v->slot[f], name);
    /* attr 0x21 = read-only + archive: honest about the volume. ntres 0x18 =
     * base and extension shown lower case, i.e. "12.bmp". */
    dir_entry(e, name, 0x21u, 0x18u, (uint16_t)first_cluster(v->slot[f]), GALLERY_BMP_LEN);
}

vfat_status_t gallery_vfat_read_sector(const gallery_vfat_t *v, uint32_t lba,
                                       uint8_t out[VFAT_SECTOR_SIZE],
                                       vfat_read_fn read, void *ctx)
{
    if (lba >= VFAT_TOTAL_SECTORS) return VFAT_ERR_RANGE;
    memset(out, 0, VFAT_SECTOR_SIZE);

    if (lba == 0u) {
        boot_sector(out);
        return VFAT_OK;
    }
    if (lba < VFAT_ROOT_LBA) {
        /* both FAT copies are the same arithmetic */
        const uint32_t s = (lba - VFAT_FAT_LBA) % VFAT_FAT_SECTORS;
        const uint32_t used = ((VFAT_DATA_CLUSTERS + 2u) * 3u + 1u) / 2u;
        for (uint32_t i = 0; i < VFAT_SECTOR_SIZE; i++) {
            const uint32_t b = s * VFAT_SECTOR_SIZE + i;
            if (b >= used) break;
            out[i] = fat_byte(v, b);
        }
        return VFAT_OK;
    }
    if (lba < VFAT_DATA_LBA) {
        const uint32_t first = (lba - VFAT_ROOT_LBA) * (VFAT_SECTOR_SIZE / 32u);
        for (uint32_t i = 0; i < VFAT_SECTOR_SIZE / 32u; i++) root_entry(v, first + i, out + i * 32u);
        return VFAT_OK;
    }

    /* data region */
    const uint32_t rel = lba - VFAT_DATA_LBA;
    const uint32_t cluster_idx = rel / VFAT_SECTORS_PER_CLUSTER;     /* cluster - 2 */
    const uint32_t slot = cluster_idx / VFAT_CLUSTERS_PER_SLOT;
    if (slot >= GALLERY_MAX_SLOTS || file_of_slot(v, slot) < 0) return VFAT_OK;  /* free: zeroes */
    const uint32_t off = (cluster_idx % VFAT_CLUSTERS_PER_SLOT) * VFAT_CLUSTER_SIZE +
                         (rel % VFAT_SECTORS_PER_CLUSTER) * VFAT_SECTOR_SIZE;
    if (off >= GALLERY_BMP_LEN) return VFAT_OK;                       /* slack after EOF */
    uint32_t n = GALLERY_BMP_LEN - off;
    if (n > VFAT_SECTOR_SIZE) n = VFAT_SECTOR_SIZE;
    if (read == NULL || !read(ctx, slot, off, out, n)) {
        memset(out, 0, VFAT_SECTOR_SIZE);
        return VFAT_ERR_IO;
    }
    return VFAT_OK;
}
