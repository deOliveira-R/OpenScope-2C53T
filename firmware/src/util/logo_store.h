/*
 * logo_store — boot logo image on the W25Q (parity C15, "Boot logo").
 *
 * Stock (V1.2.0, docs/re/parity2_static.md 4.6): boot opens
 * `2:/LOGO/LOGO2C53T.jpg` (string 0x080BC66B) on its own FAT volume, draws it at
 * (0,0) with the backlight off, then raises the backlight to the stored
 * brightness and holds (0x0802B420..0x0802C124); otherwise an embedded image.
 *
 * OpenScope does not read or write stock's FAT (see gallery_store.h for why).
 * The logo lives in its own region, FLASH_REGION_LOGO (0xF70000, 192 KB, carved
 * from the unused tail of "modules"), as ONE record:
 *
 *   +0     header, 24 B (written LAST: it is the commit record)
 *   +256   320x240 RGB565 little-endian, row 0 first, 153,600 B
 *
 * Raw RGB565 and not a compressed format on purpose: boot needs no decoder, no
 * work RAM (rows stream from the flash into the LCD through a 256-byte stack
 * buffer), and the image is exactly what the panel shows. The price is the
 * upload size (153,600 B), which the shell protocol (hex chunks, scripts/
 * logo_put.py) handles in a few seconds.
 *
 * Header (little-endian):
 *    0  u32  magic  "OSL1" = 0x314C534F
 *    4  u16  version (1)
 *    6  u16  format  (1 = RGB565 LE)
 *    8  u16  width   (320)
 *   10  u16  height  (240)
 *   12  u32  payload length (153,600)
 *   16  u32  payload CRC32 (the project's CRC, cal_backup_crc32)
 *   20  u32  CRC32 of bytes 0..19
 *
 * Upload = begin (erase), data chunks at strictly sequential offsets, end
 * (re-read the stored payload from flash, compare with the CRC the host
 * declared, then write the header). A power cut or a CRC mismatch before the
 * header leaves the slot EMPTY/DAMAGED and boot keeps the built-in splash.
 * Plain C over flash_regions.h: host-testable against the NOR model.
 */
#ifndef LOGO_STORE_H
#define LOGO_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "flash_regions.h"

#define LOGO_REGION        FLASH_REGION_LOGO
#define LOGO_W             320u
#define LOGO_H             240u
#define LOGO_ROW_BYTES     (LOGO_W * 2u)
#define LOGO_PIXEL_BYTES   (LOGO_ROW_BYTES * LOGO_H)    /* 153600 */
#define LOGO_HDR_LEN       24u
#define LOGO_DATA_OFFSET   256u    /* own page: the header program never shares one */
#define LOGO_MAGIC         0x314C534Fu   /* "OSL1" */
#define LOGO_VERSION       1u
#define LOGO_FMT_RGB565    1u
#define LOGO_STREAM_PX     64u     /* pixels per sink call (divides 76,800) */
#define LOGO_CHUNK_MAX     64u     /* bytes per logo_put_data() call (shell: 48) */

/* Sectors holding header + payload: the upload erases exactly these. */
#define LOGO_SECTORS  ((LOGO_DATA_OFFSET + LOGO_PIXEL_BYTES + FLASH_REGION_SECTOR_SIZE - 1u) \
                       / FLASH_REGION_SECTOR_SIZE)

_Static_assert(LOGO_DATA_OFFSET + LOGO_PIXEL_BYTES <= 0x30000u, "logo does not fit its region");

typedef enum {
    LOGO_SLOT_EMPTY = 0,   /* header bytes all 0xFF                  */
    LOGO_SLOT_VALID,       /* magic + header CRC + geometry good      */
    LOGO_SLOT_DAMAGED,     /* anything else: torn upload, foreign data */
} logo_state_t;

typedef enum {
    LOGO_OK = 0,
    LOGO_ERR_NOT_READY,    /* region layer not bound                  */
    LOGO_ERR_IO,           /* region layer refused or failed          */
    LOGO_ERR_BAD_ARG,      /* wrong length, no session, chunk too big */
    LOGO_ERR_SEQUENCE,     /* data offset is not the next expected one */
    LOGO_ERR_INCOMPLETE,   /* end before all bytes arrived            */
    LOGO_ERR_CRC,          /* stored payload does not match declared CRC */
    LOGO_ERR_NONE,         /* no valid logo stored                    */
} logo_status_t;

typedef struct {
    logo_state_t state;
    uint32_t     len;      /* VALID: payload length */
    uint32_t     crc;      /* VALID: payload CRC32  */
} logo_info_t;

/* Upload session; lives with the caller (16 bytes of static RAM in the shell). */
typedef struct {
    bool     active;
    uint32_t declared_crc;
    uint32_t received;
} logo_upload_t;

const char *logo_status_str(logo_status_t st);

/* Header serialisation / classification (pure). */
void logo_write_header(uint8_t out[LOGO_HDR_LEN], uint32_t payload_crc);
void logo_classify_header(const uint8_t hdr[LOGO_HDR_LEN], logo_info_t *info);

logo_status_t logo_info(logo_info_t *info);

/* Start an upload: invalidates the stored logo FIRST (header sector erased
 * before anything else), then erases the payload sectors. tick (may be NULL)
 * runs between sector erases. declared_len must be LOGO_PIXEL_BYTES. */
logo_status_t logo_put_begin(logo_upload_t *up, uint32_t declared_len,
                             uint32_t declared_crc, void (*tick)(void));

/* Append n (1..LOGO_CHUNK_MAX) bytes; off must equal up->received. */
logo_status_t logo_put_data(logo_upload_t *up, uint32_t off, const void *data, uint32_t n);

/* Verify the stored payload against the declared CRC (streamed from flash,
 * tick between blocks) and commit the header. On CRC mismatch nothing is
 * committed and the session ends. */
logo_status_t logo_put_end(logo_upload_t *up, void (*tick)(void));

/* Remove the logo (header sector erased: slot reads EMPTY, boot shows the splash). */
logo_status_t logo_clear(void);

/* Read payload bytes [off, off+len) of a VALID logo. */
logo_status_t logo_read(uint32_t off, void *buf, uint32_t len);

/* CRC32 of the stored payload, recomputed from flash. */
logo_status_t logo_crc_payload(uint32_t *crc, void (*tick)(void));

/* Stream the whole image to sink() in row-major order, LOGO_STREAM_PX pixels at a
 * time; returns LOGO_ERR_NONE when no valid logo is stored (sink never called).
 * The device sink writes straight into the LCD window. */
typedef void (*logo_pixel_sink_t)(const uint16_t *px, uint32_t count, void *ctx);
logo_status_t logo_stream(logo_pixel_sink_t sink, void *ctx);

#endif /* LOGO_STORE_H */
