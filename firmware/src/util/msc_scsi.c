/*
 * msc_scsi.c — see msc_scsi.h. SCSI references are SPC-4 / SBC-3; transport
 * references are "USB Mass Storage Class Bulk-Only Transport" rev 1.0 (BOT).
 */
#include "msc_scsi.h"

#include <string.h>

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

void msc_lun_init(msc_lun_t *lun, uint32_t block_count, uint16_t block_size)
{
    memset(lun, 0, sizeof *lun);
    lun->block_count = block_count;
    lun->block_size = block_size;
}

void msc_lun_set_present(msc_lun_t *lun, bool present)
{
    if (present && !lun->present) lun->unit_attention = true;
    lun->present = present;
    if (present) lun->eject_request = false;
}

void msc_lun_changed(msc_lun_t *lun)
{
    if (lun->present) lun->unit_attention = true;
}

void msc_lun_set_sense(msc_lun_t *lun, uint8_t key, uint8_t asc)
{
    lun->sense_key = key;
    lun->asc = asc;
    lun->ascq = 0;
}

bool msc_cbw_parse(const uint8_t *buf, uint32_t len, uint8_t max_lun, msc_cbw_t *cbw)
{
    /* BOT 6.2.1 "valid": 31 bytes with the signature. 6.2.2 "meaningful":
     * reserved bits zero, LUN supported, 1..16 CB bytes. */
    if (buf == NULL || cbw == NULL || len != MSC_CBW_LEN) return false;
    if (le32(buf) != MSC_CBW_SIGNATURE) return false;
    if ((buf[12] & 0x7Fu) != 0u) return false;
    if ((buf[13] & 0xF0u) != 0u || (buf[13] & 0x0Fu) > max_lun) return false;
    if ((buf[14] & 0xE0u) != 0u || buf[14] == 0u || buf[14] > 16u) return false;
    memset(cbw, 0, sizeof *cbw);
    cbw->tag = le32(buf + 4);
    cbw->data_len = le32(buf + 8);
    cbw->dir_in = (buf[12] & 0x80u) != 0u;
    cbw->lun = buf[13] & 0x0Fu;
    cbw->cb_len = buf[14];
    memcpy(cbw->cb, buf + 15, cbw->cb_len);
    return true;
}

static void fail(msc_lun_t *lun, msc_result_t *r, uint8_t key, uint8_t asc)
{
    msc_lun_set_sense(lun, key, asc);
    r->ok = false;
}

/* The medium-access gate: a pending UNIT ATTENTION is reported (once) before
 * anything else, then a missing medium. */
static bool ready(msc_lun_t *lun, msc_result_t *r)
{
    if (lun->unit_attention) {
        lun->unit_attention = false;
        fail(lun, r, SCSI_SK_UNIT_ATTENTION, SCSI_ASC_MEDIUM_CHANGED);
        return false;
    }
    if (!lun->present) {
        fail(lun, r, SCSI_SK_NOT_READY, SCSI_ASC_MEDIUM_NOT_PRESENT);
        return false;
    }
    return true;
}

static void reply(msc_result_t *r, uint32_t n, uint32_t alloc)
{
    r->xfer = MSC_XFER_IN_BUF;
    r->len = (n < alloc) ? n : alloc;
}

static void read_blocks(msc_lun_t *lun, msc_result_t *r, uint32_t lba, uint32_t blocks)
{
    if (!ready(lun, r)) return;
    if (lba >= lun->block_count || blocks > lun->block_count - lba) {
        fail(lun, r, SCSI_SK_ILLEGAL_REQUEST, SCSI_ASC_LBA_OUT_OF_RANGE);
        return;
    }
    r->xfer = MSC_XFER_IN_READ;
    r->lba = lba;
    r->blocks = blocks;
    r->len = blocks * lun->block_size;
}

static void write_blocks(msc_lun_t *lun, msc_result_t *r, uint32_t blocks)
{
    /* Device-side length so the transport can tell a host that disagrees
     * (BOT cases 8/13); the bytes themselves are dropped. */
    r->xfer = MSC_XFER_OUT;
    const uint64_t bytes = (uint64_t)blocks * lun->block_size;   /* WRITE(12) counts are 32-bit */
    r->len = bytes > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)bytes;
    if (!ready(lun, r)) return;
    fail(lun, r, SCSI_SK_DATA_PROTECT, SCSI_ASC_WRITE_PROTECTED);
}

void msc_scsi_command(msc_lun_t *lun, const msc_cbw_t *cbw, msc_result_t *r)
{
    memset(r, 0, sizeof *r);
    r->ok = true;
    const uint8_t *cb = cbw->cb;
    const uint8_t op = cb[0];

    /* Sense data describes the last command only; REQUEST SENSE reads it. */
    if (op != 0x03u) msc_lun_set_sense(lun, SCSI_SK_NO_SENSE, 0u);

    switch (op) {
    case 0x00:  /* TEST UNIT READY */
        (void)ready(lun, r);
        return;

    case 0x03: { /* REQUEST SENSE, fixed format */
        uint8_t key = lun->sense_key, asc = lun->asc, ascq = lun->ascq;
        if (key == SCSI_SK_NO_SENSE) {
            /* Nothing latched: say why the next command would fail, so a
             * host polling with REQUEST SENSE alone still learns it. */
            if (lun->unit_attention) {
                lun->unit_attention = false;
                key = SCSI_SK_UNIT_ATTENTION; asc = SCSI_ASC_MEDIUM_CHANGED; ascq = 0;
            } else if (!lun->present) {
                key = SCSI_SK_NOT_READY; asc = SCSI_ASC_MEDIUM_NOT_PRESENT; ascq = 0;
            }
        }
        r->buf[0] = 0x70u;          /* current error, fixed format */
        r->buf[2] = key;
        r->buf[7] = 10u;            /* additional sense length: 18 bytes total */
        r->buf[12] = asc;
        r->buf[13] = ascq;
        msc_lun_set_sense(lun, SCSI_SK_NO_SENSE, 0u);
        reply(r, 18u, cb[4]);
        return;
    }

    case 0x12: { /* INQUIRY */
        if (cb[1] & 0x01u) {        /* EVPD: no vital product data pages */
            fail(lun, r, SCSI_SK_ILLEGAL_REQUEST, SCSI_ASC_INVALID_FIELD_CDB);
            return;
        }
        r->buf[0] = 0x00u;          /* direct-access block device */
        r->buf[1] = 0x80u;          /* removable: the "card" is the gallery */
        r->buf[2] = 0x04u;          /* SPC-2 */
        r->buf[3] = 0x02u;          /* response data format */
        r->buf[4] = 36u - 5u;
        memcpy(r->buf + 8, "OpenScop", 8);
        memcpy(r->buf + 16, "2C53T Gallery RO", 16);
        memcpy(r->buf + 32, "0001", 4);
        reply(r, 36u, be16(cb + 3));
        return;
    }

    case 0x1A:  /* MODE SENSE(6): header only, WP set */
        if (!ready(lun, r)) return;
        r->buf[0] = 3u;             /* mode data length (excludes itself) */
        r->buf[2] = 0x80u;          /* device-specific parameter: WP */
        reply(r, 4u, cb[4]);
        return;

    case 0x5A:  /* MODE SENSE(10) */
        if (!ready(lun, r)) return;
        r->buf[1] = 6u;
        r->buf[3] = 0x80u;
        reply(r, 8u, be16(cb + 7));
        return;

    case 0x1B:  /* START STOP UNIT */
        if ((cb[4] & 0x03u) == 0x02u) {   /* LoEj, !Start: the host ejected us */
            lun->present = false;
            lun->unit_attention = false;
            lun->eject_request = true;
        }
        return;

    case 0x1E:  /* PREVENT ALLOW MEDIUM REMOVAL: nothing to lock */
        return;

    case 0x23: { /* READ FORMAT CAPACITIES (Windows asks first) */
        r->buf[3] = 8u;             /* capacity list length */
        put_be32(r->buf + 4, lun->block_count);
        r->buf[8] = lun->present ? 0x02u : 0x03u;   /* formatted media / no media */
        r->buf[9] = (uint8_t)(lun->block_size >> 16);
        r->buf[10] = (uint8_t)(lun->block_size >> 8);
        r->buf[11] = (uint8_t)lun->block_size;
        reply(r, 12u, be16(cb + 7));
        return;
    }

    case 0x25:  /* READ CAPACITY(10) */
        if (!ready(lun, r)) return;
        put_be32(r->buf, lun->block_count - 1u);
        put_be32(r->buf + 4, lun->block_size);
        reply(r, 8u, 8u);
        return;

    case 0x28:  /* READ(10) */
        read_blocks(lun, r, be32(cb + 2), be16(cb + 7));
        return;

    case 0xA8:  /* READ(12) */
        read_blocks(lun, r, be32(cb + 2), be32(cb + 6));
        return;

    case 0x2F:  /* VERIFY(10): nothing to compare against but the arithmetic */
        if (!ready(lun, r)) return;
        if (cb[1] & 0x02u) {        /* BYTCHK would send data: refuse it */
            fail(lun, r, SCSI_SK_ILLEGAL_REQUEST, SCSI_ASC_INVALID_FIELD_CDB);
            return;
        }
        return;

    case 0x35:  /* SYNCHRONIZE CACHE(10): nothing is cached */
        (void)ready(lun, r);
        return;

    case 0x2A:  /* WRITE(10) */
    case 0x2E:  /* WRITE AND VERIFY(10) */
        write_blocks(lun, r, be16(cb + 7));
        return;

    case 0xAA:  /* WRITE(12) */
        write_blocks(lun, r, be32(cb + 6));
        return;

    case 0x04:  /* FORMAT UNIT: there is nothing to format */
        if (!ready(lun, r)) return;
        fail(lun, r, SCSI_SK_DATA_PROTECT, SCSI_ASC_WRITE_PROTECTED);
        return;

    default:
        fail(lun, r, SCSI_SK_ILLEGAL_REQUEST, SCSI_ASC_INVALID_OPCODE);
        return;
    }
}

void msc_plan(const msc_cbw_t *cbw, const msc_result_t *r, msc_plan_t *p)
{
    memset(p, 0, sizeof *p);
    const uint32_t H = cbw->data_len;
    const uint32_t D = (r->xfer == MSC_XFER_NONE) ? 0u : r->len;
    const bool dev_in = r->xfer == MSC_XFER_IN_BUF || r->xfer == MSC_XFER_IN_READ;
    p->status = r->ok ? MSC_CSW_PASS : MSC_CSW_FAIL;

    if (H == 0u) {                  /* cases 1-3 */
        if (D > 0u) p->status = MSC_CSW_PHASE_ERROR;
        return;
    }
    if (cbw->dir_in) {
        if (r->xfer == MSC_XFER_OUT) {          /* case 10: Hi <> Do */
            p->pad = H;
            p->residue = H;
            p->status = MSC_CSW_PHASE_ERROR;
            return;
        }
        p->send = (D < H) ? D : H;              /* cases 4-7 */
        p->residue = H - p->send;
        if (D > H) p->status = MSC_CSW_PHASE_ERROR;   /* case 7: Hi < Di */
        /* A short packet ends the data phase by itself. A shortfall that is a
         * whole number of packets (including nothing at all) would leave the
         * host waiting, so fill it with zeros up to H (BOT 6.7.2: "the device
         * may send fill data"); the residue still says how much was real. */
        if (p->send < H && (p->send % MSC_BULK_MPS) == 0u) p->pad = H - p->send;
        return;
    }
    /* host to device: take everything the host sends and drop it */
    p->recv = H;
    p->residue = H;                             /* nothing was written */
    if (dev_in) p->status = MSC_CSW_PHASE_ERROR;              /* case 8 */
    else if (r->xfer == MSC_XFER_OUT && D > H) p->status = MSC_CSW_PHASE_ERROR;  /* case 13 */
}

void msc_csw_build(uint8_t out[MSC_CSW_LEN], uint32_t tag, uint32_t residue, uint8_t status)
{
    put_le32(out, MSC_CSW_SIGNATURE);
    put_le32(out + 4, tag);
    put_le32(out + 8, residue);
    out[12] = status;
}
