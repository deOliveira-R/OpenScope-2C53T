/*
 * gallery_store — on-device screenshot slots on the W25Q (parity C16).
 *
 * Stock (V1.2.0): short SAVE writes `2:/Screenshot file/%d.bmp` into its FatFs
 * volume "2:" (W25Q 0x200000..0xFFFFFF; path string 0x080BC1B4, popups
 * "Saving..."/"Saved"/"Save failed" = ms[0xE10] 1/2/3 around 0x0801C7B0), and
 * long SAVE opens a 3x3 "nine palace grid" gallery with delete (manual).
 *
 * OpenScope does NOT write stock's FAT. Everything we own on the W25Q lives at
 * 0xF00000+ in what stock's FAT calls free space (flash_regions.c, "KNOWN
 * TRADE-OFF"); a FAT writer on "2:" would be the first code able to allocate
 * those clusters to a file and so to clobber our own calibration backup. The
 * screenshots therefore go into FLASH_REGION_SCRATCH (0xFA0000, 372 KB, ours,
 * RW, documented as the screenshot area since the region table was written),
 * as fixed slots, through the bounds-checked region layer only:
 *
 *   slot k = scratch offset k * GALLERY_SLOT_SIZE   (10 sectors = 40 KB)
 *     +0     slot header, 24 B (written LAST: it is the commit record)
 *     +256   a complete 4-bpp BMP file, 38518 B, byte for byte what
 *            `screenshot get` streams and what a PC opens
 *
 * 372 KB / 40 KB = 9 slots, which is exactly one 3x3 gallery page.
 *
 * The pixels are the LCD's 4-bit indexed software shadow (lcd.c lcd_shadow,
 * the same source scripts/screenshot.py has read over USB since 2026-04), so a
 * save needs no RAM beyond a few hundred bytes of stack: the 38400-byte shadow
 * is already top-down, two pixels per byte, high nibble first and 160 bytes a
 * row, which is precisely the BMP 4-bpp row format (stride 160 is a multiple
 * of 4, so no padding). The cost is colour fidelity: the shadow keeps only the
 * nearest of 16 palette colours, as the USB screenshot always has.
 *
 * Naming: each saved slot carries a monotonic number `seq`; its name is
 * "<seq>.bmp", the same pattern as stock's `%d.bmp`. Deleting a shot zeroes
 * only the header's magic (a 1 -> 0 program, no erase), so seq survives and
 * numbers are never reused while the record is readable.
 *
 * Plain C over flash_regions.h: host-testable against the NOR model.
 */
#ifndef GALLERY_STORE_H
#define GALLERY_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "flash_regions.h"

#define GALLERY_REGION          FLASH_REGION_SCRATCH
#define GALLERY_SLOT_SECTORS    10u
#define GALLERY_SLOT_SIZE       (GALLERY_SLOT_SECTORS * FLASH_REGION_SECTOR_SIZE)
#define GALLERY_MAX_SLOTS       9u     /* one 3x3 page; also the array bound */

#define GALLERY_IMG_W           320u
#define GALLERY_IMG_H           240u
#define GALLERY_ROW_BYTES       (GALLERY_IMG_W / 2u)                  /* 160 */
#define GALLERY_PIXEL_BYTES     (GALLERY_ROW_BYTES * GALLERY_IMG_H)   /* 38400 */

#define GALLERY_BMP_HDR_LEN     118u   /* 14 file + 40 info + 16 x 4 palette */
#define GALLERY_BMP_LEN         (GALLERY_BMP_HDR_LEN + GALLERY_PIXEL_BYTES)  /* 38518 */

#define GALLERY_HDR_LEN         24u
#define GALLERY_BMP_OFFSET      256u   /* own page: the header program never shares one */
#define GALLERY_MAGIC           0x3147534Fu  /* "OSG1" little-endian */
#define GALLERY_VERSION         1u

_Static_assert(GALLERY_BMP_OFFSET + GALLERY_BMP_LEN <= GALLERY_SLOT_SIZE,
               "a BMP does not fit its slot");
_Static_assert(GALLERY_ROW_BYTES % 4u == 0u, "BMP rows would need padding");

typedef enum {
    GALLERY_SLOT_EMPTY = 0,   /* header bytes all 0xFF                        */
    GALLERY_SLOT_VALID,       /* magic + header CRC good                      */
    GALLERY_SLOT_DELETED,     /* magic zeroed by gallery_delete()            */
    GALLERY_SLOT_DAMAGED,     /* anything else: torn save, foreign data       */
} gallery_slot_state_t;

typedef enum {
    GALLERY_OK = 0,
    GALLERY_ERR_NOT_READY,    /* region layer not bound                       */
    GALLERY_ERR_FULL,         /* every slot holds a valid shot                */
    GALLERY_ERR_IO,           /* region layer refused or failed               */
    GALLERY_ERR_NOT_FOUND,    /* no valid shot with that number              */
    GALLERY_ERR_BAD_ARG,
} gallery_status_t;

typedef struct {
    gallery_slot_state_t state;
    uint32_t seq;             /* valid for VALID, and for DELETED when seq_known */
    bool     seq_known;       /* header body CRC good (VALID or DELETED)      */
    uint32_t bmp_len;
    uint32_t bmp_crc;
} gallery_slot_info_t;

/* All slots, newest first. Lives on the caller's stack (~60 B): nothing about
 * the gallery is kept in static RAM between key presses. */
typedef struct {
    uint8_t  count;                        /* valid shots                     */
    uint8_t  slot[GALLERY_MAX_SLOTS];      /* slot index, newest (highest seq) first */
    uint32_t seq[GALLERY_MAX_SLOTS];       /* matching numbers                */
    uint32_t next_seq;                     /* number the next save gets       */
    uint8_t  slots_total;                  /* usable slots in the region      */
} gallery_index_t;

/* Slots the gallery region can hold (0 if the region layer is not bound). */
uint32_t gallery_slot_count(void);

/* "<seq>.bmp" into out (>= 16 bytes). Returns the string length. */
uint32_t gallery_name(uint32_t seq, char *out, uint32_t out_size);

/* Parse "12" or "12.bmp" (case-insensitive extension). False on junk/zero. */
bool gallery_parse_name(const char *s, uint32_t *seq);

/* The 118-byte BMP header (BITMAPINFOHEADER, BI_RGB, 4 bpp, 16-colour palette,
 * negative height = top-down) for a 320x240 indexed image. Palette entries are
 * RGB565, expanded to 8-bit BGRX the way a PC viewer expects. */
void gallery_bmp_header(uint8_t out[GALLERY_BMP_HDR_LEN], const uint16_t pal565[16]);

/* Serialise / classify the 24-byte slot header. */
void gallery_write_slot_header(uint8_t out[GALLERY_HDR_LEN], uint32_t seq,
                               uint32_t bmp_len, uint32_t bmp_crc);
void gallery_classify_header(const uint8_t hdr[GALLERY_HDR_LEN], gallery_slot_info_t *info);

gallery_status_t gallery_slot_info(uint32_t slot, gallery_slot_info_t *info);
gallery_status_t gallery_scan(gallery_index_t *idx);

/* Slot of shot number seq, or GALLERY_ERR_NOT_FOUND. */
gallery_status_t gallery_find(uint32_t seq, uint32_t *slot);

/* Which slot the next save goes to: the lowest EMPTY one, else the lowest
 * DELETED/DAMAGED one (which then needs an erase). GALLERY_ERR_FULL when all
 * slots are VALID — saving never overwrites a shot the user has not deleted. */
gallery_status_t gallery_pick_slot(uint32_t *slot);

/* Save a screenshot: erase the target slot (sectors already blank are skipped
 * by the region layer), write BMP header + pixels a sector at a time, then the
 * slot header as the commit record. A power cut at any point leaves either the
 * old state or a slot that classifies EMPTY/DAMAGED and is reused next time.
 * pixels = 38400 bytes, 4-bpp top-down rows. tick (may be NULL) is called
 * between flash operations so the caller can check in with the watchdog. */
gallery_status_t gallery_save(const uint8_t *pixels, const uint16_t pal565[16],
                              void (*tick)(void), uint32_t *seq_out);

/* Delete shot seq: zero its magic (a program, never an erase). */
gallery_status_t gallery_delete(uint32_t seq);

/* Bumped by every gallery_save()/gallery_delete() that reached the flash
 * (success or not), so a reader holding an index (the USB mass-storage view,
 * usb_msc.c) knows to rescan instead of polling the headers. */
uint32_t gallery_generation(void);

/* Read len bytes of the stored BMP of slot at offset off (0 = 'B'). */
gallery_status_t gallery_read_bmp(uint32_t slot, uint32_t off, void *buf, uint32_t len);

/* Read one 160-byte row of pixels (row 0 = top) of slot. */
gallery_status_t gallery_read_row(uint32_t slot, uint32_t row, uint8_t out[GALLERY_ROW_BYTES]);

/* Read the 16 RGB565 palette entries back out of a stored BMP header. */
gallery_status_t gallery_read_palette(uint32_t slot, uint16_t pal565[16]);

/* CRC32 of a stored BMP, recomputed by streaming it (stack buffer only). */
gallery_status_t gallery_crc_bmp(uint32_t slot, uint32_t *crc);

#endif /* GALLERY_STORE_H */
