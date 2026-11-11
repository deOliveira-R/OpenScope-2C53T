/*
 * gallery_vfat — a read-only FAT12 volume synthesised on the fly from the
 * screenshot gallery (parity C16, "USB Sharing").
 *
 * Stock (V1.2.0): Settings "USB Sharing" (OK handler 0x0800BF08, ms[0xF68] =
 * 4) exposes its FatFs volume "2:" (W25Q 0x200000..0xFFFFFF, FAT12, 4 KB
 * sectors) as a mass-storage device, writable by the host.
 *
 * OpenScope does not expose any real FAT. Our screenshots live in fixed slots
 * of our own scratch region (gallery_store.h), and a writable view of stock's
 * volume would let a host allocate clusters over OpenScope's records at
 * 0xD00000+ (docs/re/parity2_static.md 4.5). So the host gets a volume that
 * exists only as arithmetic: every 512-byte sector it reads is computed here
 * — boot sector, both FAT copies and the root directory from a ~50-byte index
 * of the valid slots, file data straight out of the slot through a callback.
 * Nothing is ever written; there is no write path to get wrong.
 *
 * GEOMETRY (fixed, so READ CAPACITY never touches flash):
 *   512 B/sector, 8 sectors = 4 KB per cluster, 1 reserved sector,
 *   2 FATs x 1 sector, 32 root entries (2 sectors), 1024 sectors total
 *   (512 KB) -> 127 data clusters: FAT12 by cluster count (< 4085).
 * Gallery slot k owns clusters 2 + 10k .. 11 + 10k (10 x 4 KB = the slot's
 * 40 KB, a 38518-byte BMP needs 10), whether or not it holds a shot, so a
 * data sector maps to (slot, byte offset) with one division and a file never
 * moves when another is deleted. Clusters 92..128 are always free.
 *
 * NAMES: "<seq>.BMP" as an 8.3 short name with the NT lower-case flags (byte
 * 12 = 0x18), so Windows, macOS and Linux all list "12.bmp" like the gallery
 * and stock's "%d.bmp". A seq above 99999999 (more than 10^8 saves; not
 * reachable in practice) does not fit 8.3: it is listed as "SHOT~<slot>.bmp"
 * instead of growing a long-file-name writer for a case that cannot happen.
 *
 * Pure C, no flash or USB includes: host-tested in tests/test_msc.c.
 */
#ifndef GALLERY_VFAT_H
#define GALLERY_VFAT_H

#include <stdbool.h>
#include <stdint.h>

#include "gallery_store.h"

#define VFAT_SECTOR_SIZE        512u
#define VFAT_SECTORS_PER_CLUSTER 8u
#define VFAT_CLUSTER_SIZE       (VFAT_SECTOR_SIZE * VFAT_SECTORS_PER_CLUSTER)
#define VFAT_RESERVED_SECTORS   1u
#define VFAT_NUM_FATS           2u
#define VFAT_FAT_SECTORS        1u
#define VFAT_ROOT_ENTRIES       32u
#define VFAT_ROOT_SECTORS       ((VFAT_ROOT_ENTRIES * 32u) / VFAT_SECTOR_SIZE)
#define VFAT_TOTAL_SECTORS      1024u

#define VFAT_FAT_LBA            VFAT_RESERVED_SECTORS                                  /* 1 */
#define VFAT_ROOT_LBA           (VFAT_FAT_LBA + VFAT_NUM_FATS * VFAT_FAT_SECTORS)      /* 3 */
#define VFAT_DATA_LBA           (VFAT_ROOT_LBA + VFAT_ROOT_SECTORS)                    /* 5 */
#define VFAT_DATA_CLUSTERS      ((VFAT_TOTAL_SECTORS - VFAT_DATA_LBA) / VFAT_SECTORS_PER_CLUSTER)  /* 127 */

#define VFAT_CLUSTERS_PER_SLOT  (GALLERY_SLOT_SIZE / VFAT_CLUSTER_SIZE)                /* 10 */
#define VFAT_FILE_CLUSTERS      ((GALLERY_BMP_LEN + VFAT_CLUSTER_SIZE - 1u) / VFAT_CLUSTER_SIZE)  /* 10 */

#define VFAT_MEDIA              0xF8u
#define VFAT_VOLUME_ID          0x2C53A11Eu   /* fixed: the volume is the same arithmetic on every unit */

_Static_assert(VFAT_FILE_CLUSTERS <= VFAT_CLUSTERS_PER_SLOT, "a BMP outgrows its slot's clusters");
_Static_assert(GALLERY_MAX_SLOTS * VFAT_CLUSTERS_PER_SLOT <= VFAT_DATA_CLUSTERS, "slots do not fit the volume");
_Static_assert(VFAT_DATA_CLUSTERS < 4085u, "must stay FAT12 by cluster count");
/* FAT12 needs 1.5 bytes per entry, entries 0..(clusters + 1). */
_Static_assert(((VFAT_DATA_CLUSTERS + 2u) * 3u + 1u) / 2u <= VFAT_FAT_SECTORS * VFAT_SECTOR_SIZE, "FAT too small");
/* label + one entry per file */
_Static_assert(1u + GALLERY_MAX_SLOTS <= VFAT_ROOT_ENTRIES, "root directory too small");

/* What the volume shows: the valid shots, ascending by number (the order a
 * host listing falls into anyway). Built from a gallery_scan(); ~50 bytes. */
typedef struct {
    uint8_t  count;
    uint8_t  slot[GALLERY_MAX_SLOTS];
    uint32_t seq[GALLERY_MAX_SLOTS];
} gallery_vfat_t;

typedef enum {
    VFAT_OK = 0,
    VFAT_ERR_RANGE,      /* lba >= VFAT_TOTAL_SECTORS                        */
    VFAT_ERR_IO,         /* the slot read callback failed                    */
} vfat_status_t;

/* Read len bytes of slot's stored BMP at offset off (the device passes a
 * wrapper around gallery_read_bmp()). Return true on success. */
typedef bool (*vfat_read_fn)(void *ctx, uint32_t slot, uint32_t off, void *buf, uint32_t len);

/* Index -> volume contents. Invalid slot numbers in idx are skipped. */
void gallery_vfat_build(gallery_vfat_t *v, const gallery_index_t *idx);

/* The 11-byte 8.3 directory name of shot seq stored in slot ("12      BMP"). */
void gallery_vfat_short_name(uint32_t seq, uint32_t slot, uint8_t out[11]);

/* The 12-bit FAT entry for cluster n (0xFF8 / 0xFFF for 0 and 1). */
uint16_t gallery_vfat_fat_entry(const gallery_vfat_t *v, uint32_t n);

/* One 512-byte sector of the volume. read may be NULL if no file is listed. */
vfat_status_t gallery_vfat_read_sector(const gallery_vfat_t *v, uint32_t lba,
                                       uint8_t out[VFAT_SECTOR_SIZE],
                                       vfat_read_fn read, void *ctx);

#endif /* GALLERY_VFAT_H */
