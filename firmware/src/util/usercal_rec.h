/*
 * usercal_rec.h — OpenScope's OWN calibration corrections, persisted (parity
 * C2/C10 follow-up): the baseline-correction deltas (long AUTO, scope_vpos'
 * corr[] table) and the 250 MS/s converter-B correction (`fpga interleave
 * cal`, fpga_interleave_corr_*).
 *
 * WHAT THIS IS NOT
 * ----------------
 * Not the stock calibration page. Stock's baseline correction rewrites its
 * page at 0x08006000 (bank tables ms+0x260.., writer 0x08029652..0x08029A44);
 * OpenScope never writes that page (the only writer is the explicit `cal
 * restore CONFIRM`). Our corrections are deltas ON TOP of the page, kept in
 * our own W25Q append log FLASH_REGION_USER_CAL (0xF00000, 64 KB) — not the
 * factory-cal backup, which is FLASH_REGION_FACTORY_CAL_BACKUP at 0xFFD000.
 * The region layer bounds every write, so the two cannot collide.
 *
 * OWNER POLICY: saved only on an explicit command (`cal user save`) or a UI
 * confirm, never silently. Nothing in this module saves on its own; the
 * baseline run and the interleave fit still change RAM only.
 *
 * WHY THE PAGE FINGERPRINT
 * ------------------------
 * A baseline delta is an offset on the page's (base, upper) endpoint pair for
 * one (ch, bank, range). Against a different table it is wrong by
 * construction: after `cal restore`, after stock's own baseline correction
 * (booting stock rewrites the table), or on another unit's page. So the
 * record carries a CRC32 of the endpoint table as it was when the deltas were
 * measured (page 0x38..0x127, the 60 words calpage_pair_page() reads and the
 * substitution map is derived from). Only the table: bytes 0x01..0x37 are
 * stock's UI state (positions, mask, timebase) and change whenever stock
 * saves its settings, which says nothing about the endpoints. On a mismatch
 * the baseline block is refused and the interleave block, which describes
 * converter B against converter A and not the page, still loads.
 *
 * RECORD LAYOUT v1 (little-endian, packed by hand: no struct is memcpy'd, so
 * compiler padding never reaches flash)
 *   off  size  field
 *   0    4     magic        USERCAL_MAGIC "OSUC"
 *   4    2     version      USERCAL_VERSION
 *   6    2     length       USERCAL_REC_LEN (the whole record, checksum incl.)
 *   8    4     table_crc    CRC32 of the page endpoint table at save time
 *   12   4     flags        bit0 = interleave block present
 *   16   4     il_gain_ppm  int32, B gain x 1e6   (500000..2000000)
 *   20   4     il_off_e4    int32, B offset x 1e4 record units (+-1000000)
 *   24   8     valid[2]     u32 per channel: bit bank*10+range holds a delta
 *   32   240   corr         int16 [ch 2][bank 3][range 10][base, upper]
 *   272  4     crc          CRC32 of bytes [0, 272)
 *   276        end
 * The region layer adds its own CRC32 over the payload (defence in depth, as
 * config.c does): a torn record is skipped by the layer and the previous one
 * wins; a well-formed record that is not a v1 user-cal fails here.
 *
 * VERSIONING: a record of another version is refused (USERCAL_ST_VERSION) and
 * nothing is applied — the unit runs on the page alone until the next save,
 * which is the safe failure for a correction. A later layout bumps the
 * version and may add a decoder for v1 here.
 *
 * Pure apart from the region layer (flash_regions.c, itself host-portable):
 * tests/test_usercal.c runs it against a NOR model. The capture/apply glue
 * that talks to scope_vpos and fpga is src/util/usercal.c.
 */
#ifndef USERCAL_REC_H
#define USERCAL_REC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "calpage.h"          /* CALPAGE_CHANNELS / _BANKS / _RANGES / _LEN */
#include "flash_regions.h"

#define USERCAL_MAGIC        0x4355534Fu   /* 'O','S','U','C' little-endian */
#define USERCAL_VERSION      1u
#define USERCAL_REC_LEN      276u
#define USERCAL_REGION       FLASH_REGION_USER_CAL

#define USERCAL_F_IL         0x01u         /* interleave block present */

/* Page bytes the fingerprint covers: the 60-word endpoint table. */
#define USERCAL_TABLE_OFF    0x38u
#define USERCAL_TABLE_LEN    (60u * 4u)

/* Plausibility bounds a decoded record must meet. The interleave bounds are
 * the shell's (`fpga interleave corr`: gain 5000..20000 x1e-4, |offset| <=
 * 100); a delta can never exceed the DAC's 12 bits. */
#define USERCAL_IL_GAIN_MIN  500000
#define USERCAL_IL_GAIN_MAX  2000000
#define USERCAL_IL_OFF_MAX   1000000
#define USERCAL_DELTA_MAX    4095

typedef struct {
    uint32_t table_crc;
    bool     il_present;
    int32_t  il_gain_ppm;
    int32_t  il_off_e4;
    uint32_t valid[CALPAGE_CHANNELS];
    int16_t  corr[CALPAGE_CHANNELS][CALPAGE_BANKS][CALPAGE_RANGES][2];
} usercal_t;

typedef enum {
    USERCAL_ST_OK = 0,
    USERCAL_ST_EMPTY,        /* no record in the log (first boot, never saved) */
    USERCAL_ST_NO_STORAGE,   /* region layer not bound                         */
    USERCAL_ST_IO,           /* flash read failed                              */
    USERCAL_ST_SIZE,         /* record is not USERCAL_REC_LEN bytes            */
    USERCAL_ST_MAGIC,
    USERCAL_ST_VERSION,
    USERCAL_ST_CRC,
    USERCAL_ST_RANGE,        /* passed its CRC but holds an implausible value  */
} usercal_status_t;

const char *usercal_status_str(usercal_status_t st);

/* Zero record: no deltas, no interleave block. */
void usercal_clear(usercal_t *u);

/* CRC32 of the endpoint table of a page of at least CALPAGE_LEN bytes. */
uint32_t usercal_table_crc(const uint8_t *page);

/* Pack *u into buf (cap >= USERCAL_REC_LEN). Returns USERCAL_REC_LEN, or 0. */
size_t usercal_encode(const usercal_t *u, uint8_t *buf, size_t cap);

/* Validate and unpack. *out is written ONLY on USERCAL_ST_OK. */
usercal_status_t usercal_decode(const uint8_t *buf, size_t len, usercal_t *out);

/* Number of deltas a record holds. */
unsigned usercal_count(const usercal_t *u);

/* ── Storage (FLASH_REGION_USER_CAL) ─────────────────────────────────
 * Save appends one record. A record identical to the newest is elided by the
 * layer. When the log takes no more (FULL / LOG_DAMAGED) the region is reset
 * and the record re-appended — the only erase here, of our own region, and
 * only inside an explicit save. Returns the layer status. */
flash_region_status_t usercal_store_save(const usercal_t *u);

/* Newest record. *out is written only on USERCAL_ST_OK. */
usercal_status_t usercal_store_load(usercal_t *out);

#endif /* USERCAL_REC_H */
