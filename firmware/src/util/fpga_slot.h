/*
 * fpga_slot — one FPGA design (a raw Gowin SSPI stream) on the W25Q, loadable
 * at runtime with `fpga reload slot` (fpga.c: fpga_reload()).
 *
 * Why: a new FPGA design used to need a firmware rebuild (the design compiled
 * in), a flash and a power cycle. With the slot the host sends the stream over
 * the shell and the firmware power-cycles the FPGA (PC6) and uploads it.
 *
 * The slot lives at the start of FLASH_REGION_MODULES ("module data assets",
 * never written by any build before this), as ONE record, on the pattern of
 * logo_store.h:
 *
 *   +0      header, 64 B (written LAST: it is the commit record)
 *   +4096   payload, 1..FSLOT_MAX_LEN bytes (its own sectors)
 *
 * Header (little-endian):
 *    0  u32  magic  "OSF1" = 0x3146534F
 *    4  u16  version (1)
 *    6  u16  reserved (0)
 *    8  u32  payload length
 *   12  u32  payload CRC32 (the project's CRC, cal_backup_crc32)
 *   16  char name[40], NUL padded (what `version`/`fpga slot info` show)
 *   56  u32  reserved (0)
 *   60  u32  CRC32 of bytes 0..59
 *
 * Upload = begin (erase), data chunks at strictly sequential offsets, end
 * (re-read the stored payload from flash, compare with the CRC the host
 * declared, then write the header). A power cut or a CRC mismatch before the
 * header leaves the slot EMPTY/DAMAGED, and a reload from it is refused before
 * the FPGA is touched. Plain C over flash_regions.h: host-testable.
 */
#ifndef FPGA_SLOT_H
#define FPGA_SLOT_H

#include <stdbool.h>
#include <stdint.h>

#include "flash_regions.h"

#define FSLOT_REGION        FLASH_REGION_MODULES
#define FSLOT_HDR_LEN       64u
#define FSLOT_NAME_LEN      40u
#define FSLOT_DATA_OFFSET   4096u            /* own sector: begin erases the header first */
#define FSLOT_SPAN          0x20000u         /* 128 KB of the region: header sector + payload */
#define FSLOT_MAX_LEN       (FSLOT_SPAN - FSLOT_DATA_OFFSET)   /* 126,976 B; a GW1N-2 stream is 115,638 */
#define FSLOT_MAGIC         0x3146534Fu      /* "OSF1" */
#define FSLOT_VERSION       1u
#define FSLOT_CHUNK_MAX     64u              /* bytes per fslot_put_data() call (shell: 48) */

typedef enum {
    FSLOT_EMPTY = 0,   /* header bytes all 0xFF                    */
    FSLOT_VALID,       /* magic + header CRC + length good          */
    FSLOT_DAMAGED,     /* anything else: torn upload, foreign data  */
} fslot_state_t;

typedef enum {
    FSLOT_OK = 0,
    FSLOT_ERR_NOT_READY,    /* region layer not bound                   */
    FSLOT_ERR_IO,           /* region layer refused or failed           */
    FSLOT_ERR_BAD_ARG,      /* bad length, no session, chunk too big    */
    FSLOT_ERR_SEQUENCE,     /* data offset is not the next expected one */
    FSLOT_ERR_INCOMPLETE,   /* end before all bytes arrived             */
    FSLOT_ERR_CRC,          /* stored payload does not match the CRC    */
    FSLOT_ERR_NONE,         /* no valid design stored                   */
} fslot_status_t;

typedef struct {
    fslot_state_t state;
    uint32_t      len;                      /* VALID: payload length */
    uint32_t      crc;                      /* VALID: payload CRC32  */
    char          name[FSLOT_NAME_LEN + 1]; /* VALID: NUL terminated */
} fslot_info_t;

/* Upload session; lives with the caller. */
typedef struct {
    bool     active;
    uint32_t declared_len;
    uint32_t declared_crc;
    uint32_t received;
    char     name[FSLOT_NAME_LEN + 1];
} fslot_upload_t;

const char *fslot_status_str(fslot_status_t st);

/* Header serialisation / classification (pure). */
void fslot_write_header(uint8_t out[FSLOT_HDR_LEN], uint32_t len, uint32_t payload_crc, const char *name);
void fslot_classify_header(const uint8_t hdr[FSLOT_HDR_LEN], fslot_info_t *info);

fslot_status_t fslot_info(fslot_info_t *info);

/* True when every byte of the slot's span reads 0xFF (nothing was ever stored
 * there). The shell refuses the first upload over foreign data unless forced. */
fslot_status_t fslot_span_blank(bool *blank, void (*tick)(void));

/* Start an upload: invalidates the stored design FIRST (header sector erased
 * before anything else), then erases the sectors the payload needs. */
fslot_status_t fslot_put_begin(fslot_upload_t *up, uint32_t declared_len, uint32_t declared_crc,
                               const char *name, void (*tick)(void));

/* Append n (1..FSLOT_CHUNK_MAX) bytes; off must equal up->received. */
fslot_status_t fslot_put_data(fslot_upload_t *up, uint32_t off, const void *data, uint32_t n);

/* Verify the stored payload against the declared CRC and commit the header. */
fslot_status_t fslot_put_end(fslot_upload_t *up, void (*tick)(void));

/* Remove the design (header sector erased: slot reads EMPTY). */
fslot_status_t fslot_clear(void);

/* Read payload bytes [off, off+len) of a stored design (no state check: the
 * caller validated the slot with fslot_verify() first). */
fslot_status_t fslot_read(uint32_t off, void *buf, uint32_t len);

/* Header valid AND the payload's CRC, recomputed from flash, matches it. */
fslot_status_t fslot_verify(fslot_info_t *info, void (*tick)(void));

#endif /* FPGA_SLOT_H */
