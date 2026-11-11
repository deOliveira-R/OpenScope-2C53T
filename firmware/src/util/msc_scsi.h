/*
 * msc_scsi — the SCSI command set and bulk-only-transport bookkeeping of
 * OpenScope's read-only USB mass-storage LUN (parity C16, "USB Sharing").
 *
 * Pure C (no USB stack): the device glue (src/drivers/usb_msc.c) hands in the
 * 31-byte CBW, gets back what to move and what CSW to send. Host-tested in
 * tests/test_msc.c.
 *
 * Read-only by construction, not by policy: there is no write callback at
 * all. MODE SENSE reports write-protect so hosts mount read-only, WRITE(10/12)
 * and friends fail with DATA PROTECT / WRITE PROTECTED, and any data a host
 * pushes anyway is received and dropped by the transport.
 *
 * Medium state: the LUN is a removable drive whose "card" is the gallery.
 * No medium = NOT READY / MEDIUM NOT PRESENT: before the device inserts it,
 * and after a host eject (START STOP UNIT LoEj=1 Start=0) until the device
 * re-inserts it or the host loads it (LoEj=1 Start=1).
 * A gallery change, or the medium coming back, is reported once as UNIT
 * ATTENTION / MEDIUM MAY HAVE CHANGED so the host drops its cached FAT.
 */
#ifndef MSC_SCSI_H
#define MSC_SCSI_H

#include <stdbool.h>
#include <stdint.h>

#define MSC_CBW_LEN          31u
#define MSC_CSW_LEN          13u
#define MSC_CBW_SIGNATURE    0x43425355u   /* "USBC" */
#define MSC_CSW_SIGNATURE    0x53425355u   /* "USBS" */
#define MSC_BULK_MPS         64u           /* full-speed bulk max packet */

#define MSC_CSW_PASS         0u
#define MSC_CSW_FAIL         1u
#define MSC_CSW_PHASE_ERROR  2u

/* Sense keys / additional sense codes used here (SPC-4). */
#define SCSI_SK_NO_SENSE        0x00u
#define SCSI_SK_NOT_READY       0x02u
#define SCSI_SK_MEDIUM_ERROR    0x03u
#define SCSI_SK_ILLEGAL_REQUEST 0x05u
#define SCSI_SK_UNIT_ATTENTION  0x06u
#define SCSI_SK_DATA_PROTECT    0x07u
#define SCSI_ASC_INVALID_OPCODE      0x20u
#define SCSI_ASC_LBA_OUT_OF_RANGE    0x21u
#define SCSI_ASC_INVALID_FIELD_CDB   0x24u
#define SCSI_ASC_WRITE_PROTECTED     0x27u
#define SCSI_ASC_MEDIUM_CHANGED      0x28u
#define SCSI_ASC_MEDIUM_NOT_PRESENT  0x3Au
#define SCSI_ASC_UNRECOVERED_READ    0x11u

typedef struct {
    uint32_t tag;
    uint32_t data_len;     /* dCBWDataTransferLength (H) */
    bool     dir_in;       /* bmCBWFlags bit 7 */
    uint8_t  lun;
    uint8_t  cb_len;
    uint8_t  cb[16];
} msc_cbw_t;

typedef struct {
    uint32_t block_count;  /* medium size in blocks                          */
    uint16_t block_size;
    bool     present;      /* sharing on                                      */
    bool     unit_attention;
    bool     eject_request;/* the host sent START STOP UNIT with LoEj, Start=0 */
    uint8_t  sense_key, asc, ascq;
} msc_lun_t;

typedef enum {
    MSC_XFER_NONE = 0,     /* no data phase from the device's side            */
    MSC_XFER_IN_BUF,       /* send buf[0..len)                                */
    MSC_XFER_IN_READ,      /* send blocks lba..lba+blocks-1 from the medium   */
    MSC_XFER_OUT,          /* the command carries data to the device          */
} msc_xfer_t;

#define MSC_RESP_MAX  36u  /* INQUIRY is the longest fixed response          */

typedef struct {
    msc_xfer_t xfer;
    bool       ok;         /* CSW status pass/fail (before phase checks)      */
    uint32_t   len;        /* bytes the device intends to move (Di / Do)      */
    uint32_t   lba, blocks;
    uint8_t    buf[MSC_RESP_MAX];
} msc_result_t;

/* What the transport does with one command, after reconciling the host's
 * expectation (H, direction) with the device's (the 13 cases of BOT 6.7). */
typedef struct {
    uint32_t send;         /* device data bytes to send (buf or medium)       */
    uint32_t pad;          /* zero bytes after them (fill: BOT 6.7.2 allows it)*/
    uint32_t recv;         /* bytes to receive and discard                    */
    uint32_t residue;      /* dCSWDataResidue                                 */
    uint8_t  status;       /* bCSWStatus                                      */
} msc_plan_t;

/* Fresh LUN: no medium yet, no sense. */
void msc_lun_init(msc_lun_t *lun, uint32_t block_count, uint16_t block_size);

/* Sharing on/off. Turning it on queues UNIT ATTENTION. */
void msc_lun_set_present(msc_lun_t *lun, bool present);

/* The medium's content changed under the host: queue UNIT ATTENTION. */
void msc_lun_changed(msc_lun_t *lun);

/* Parse a CBW; false if it is not a valid, meaningful one (BOT 6.2), in which
 * case the transport must stall both pipes until a Reset Recovery. */
bool msc_cbw_parse(const uint8_t *buf, uint32_t len, uint8_t max_lun, msc_cbw_t *cbw);

/* Execute the SCSI command in cbw->cb against lun. */
void msc_scsi_command(msc_lun_t *lun, const msc_cbw_t *cbw, msc_result_t *r);

/* Reconcile host and device views of the data phase. */
void msc_plan(const msc_cbw_t *cbw, const msc_result_t *r, msc_plan_t *p);

/* The 13-byte CSW. */
void msc_csw_build(uint8_t out[MSC_CSW_LEN], uint32_t tag, uint32_t residue, uint8_t status);

/* Record a medium error discovered while streaming data (the CSW then fails). */
void msc_lun_set_sense(msc_lun_t *lun, uint8_t key, uint8_t asc);

#endif /* MSC_SCSI_H */
