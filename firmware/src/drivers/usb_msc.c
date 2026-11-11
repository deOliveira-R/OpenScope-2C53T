/*
 * usb_msc.c — bulk-only transport of the read-only gallery LUN. See
 * usb_msc.h for the interrupt/task split and the RAM budget.
 *
 * BOT state machine (BOT rev 1.0, 5 and 6):
 *   IDLE     OUT armed for a CBW
 *   DATA_IN  a chunk is on the IN pipe; on completion: next chunk, fill, CSW
 *   FILL     waiting for the task to compute the next sector of a READ
 *   DATA_OUT receiving (and dropping) host data
 *   CSW      the CSW is on the IN pipe; on completion back to IDLE
 *   STALLED  invalid CBW: both pipes halted until a Reset Recovery (6.6.1)
 */
#include "usb_msc.h"
#include "usb_composite.h"

#include "usbd_sdr.h"
#include "FreeRTOS.h"
#include "task.h"

#include "gallery_store.h"
#include "gallery_vfat.h"
#include "msc_scsi.h"

#include <string.h>

enum { BOT_IDLE = 0, BOT_DATA_IN, BOT_FILL, BOT_DATA_OUT, BOT_CSW, BOT_STALLED };

/* The one sector buffer: CBW reception (a host may send up to a full 64-byte
 * packet there), fixed responses, READ sectors, zero fill and dropped writes.
 * Sharing it is safe because BOT is strictly sequential, with one exception:
 * a bus or BO reset in the middle of a READ while the task is computing a
 * sector. The task may then overwrite the buffer under the next command's
 * reply; the epoch check below drops its stale send, and the only replies
 * that read the buffer after the interrupt returns are multi-packet zero
 * fills of failed commands, whose content the host ignores. */
static uint8_t msc_buf[VFAT_SECTOR_SIZE] __attribute__((aligned(4)));

static struct {
    volatile uint8_t  state;
    volatile uint32_t epoch;        /* bumped by every reset: a fill in flight is dropped */
    volatile bool     configured;
    msc_cbw_t  cbw;
    msc_lun_t  lun;
    uint32_t   send_left, pad_left, recv_left;
    uint32_t   next_lba;
    uint32_t   residue;
    uint8_t    status;
    bool       io_error;
    uint8_t    csw[16] __attribute__((aligned(4)));

    /* task-only */
    gallery_vfat_t vfat;
    bool       vfat_valid;
    uint32_t   gallery_gen;
    TaskHandle_t notify;

    usb_msc_stats_t st;
} m = {
    .lun = { .block_count = VFAT_TOTAL_SECTORS, .block_size = VFAT_SECTOR_SIZE,
             .present = USB_MSC_SHARE_DEFAULT != 0 },
};

/* ── SCSI trace (usb_msc.h) ── written in interrupt context ── */
static usb_msc_trace_t s_tr[USB_MSC_TRACE_N];
static uint32_t s_tr_n, s_tr_lost, s_tr_t0;
static int32_t  s_tr_cur = -1;      /* entry of the command in flight */

static uint32_t now_ms_isr(void) { return (uint32_t)(xTaskGetTickCountFromISR() * portTICK_PERIOD_MS); }

static void trace_begin(const msc_cbw_t *cbw, const msc_result_t *r, const msc_lun_t *lun)
{
    s_tr_cur = -1;
    if (s_tr_n >= USB_MSC_TRACE_N) { s_tr_lost++; return; }
    usb_msc_trace_t *e = &s_tr[s_tr_n];
    memset(e, 0, sizeof *e);
    e->t_ms = now_ms_isr() - s_tr_t0;
    e->cdb_len = cbw->cb_len;
    memcpy(e->cdb, cbw->cb, cbw->cb_len < sizeof e->cdb ? cbw->cb_len : sizeof e->cdb);
    if (cbw->cb[0] == 0x03u && r->xfer == MSC_XFER_IN_BUF) {
        e->sk = r->buf[2]; e->asc = r->buf[12]; e->ascq = r->buf[13];   /* what it reported */
    } else {
        e->sk = lun->sense_key; e->asc = lun->asc; e->ascq = lun->ascq;
    }
    e->status = 0xFFu;              /* until the CSW */
    s_tr_cur = (int32_t)s_tr_n++;
}

static void trace_end(uint8_t status, const msc_lun_t *lun, bool io_error)
{
    if (s_tr_cur < 0) return;
    usb_msc_trace_t *e = &s_tr[s_tr_cur];
    e->status = status;
    if (io_error) { e->sk = lun->sense_key; e->asc = lun->asc; e->ascq = lun->ascq; }
    if (s_tr_cur > 0) {
        usb_msc_trace_t *p = &s_tr[s_tr_cur - 1];
        if (p->repeat < 0xFFFFu && p->cdb_len == e->cdb_len && memcmp(p->cdb, e->cdb, sizeof e->cdb) == 0 &&
            p->status == e->status && p->sk == e->sk && p->asc == e->asc && p->ascq == e->ascq) {
            p->repeat++;
            s_tr_n--;
        }
    }
    s_tr_cur = -1;
}

void usb_msc_trace_reset(void)
{
    taskENTER_CRITICAL();
    s_tr_n = 0;
    s_tr_lost = 0;
    s_tr_cur = -1;
    s_tr_t0 = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    taskEXIT_CRITICAL();
}

uint32_t usb_msc_trace_count(uint32_t *not_recorded)
{
    taskENTER_CRITICAL();
    const uint32_t n = s_tr_n;
    if (not_recorded != NULL) *not_recorded = s_tr_lost;
    taskEXIT_CRITICAL();
    return n;
}

bool usb_msc_trace_get(uint32_t i, usb_msc_trace_t *out)
{
    bool ok = false;
    taskENTER_CRITICAL();
    if (i < s_tr_n) { *out = s_tr[i]; ok = true; }
    taskEXIT_CRITICAL();
    return ok;
}

static void notify_task_from_isr(void)
{
    if (m.notify == NULL) return;
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(m.notify, &woken);
    portYIELD_FROM_ISR(woken);
}

/* ── Transport (interrupt context, or task context inside a critical section) ── */

static void arm_cbw(usbd_core_type *udev)
{
    m.state = BOT_IDLE;
    usbd_ept_recv(udev, USB_COMP_MSC_OUT_EPT, msc_buf, USB_MSC_MPS);
}

static void send_csw(usbd_core_type *udev)
{
    uint8_t status = m.status;
    if (m.io_error && status == MSC_CSW_PASS) status = MSC_CSW_FAIL;
    trace_end(status, &m.lun, m.io_error);
    msc_csw_build(m.csw, m.cbw.tag, m.residue, status);
    m.state = BOT_CSW;
    usbd_ept_send(udev, USB_COMP_MSC_IN_EPT, m.csw, MSC_CSW_LEN);
}

/* After a chunk went out: more medium data (task), zero fill, or the CSW. */
static void continue_in(usbd_core_type *udev)
{
    if (m.send_left > 0u) {
        m.state = BOT_FILL;
        notify_task_from_isr();
        return;
    }
    if (m.pad_left > 0u) {
        uint32_t n = m.pad_left < sizeof msc_buf ? m.pad_left : sizeof msc_buf;
        memset(msc_buf, 0, n);
        m.pad_left -= n;
        m.state = BOT_DATA_IN;
        usbd_ept_send(udev, USB_COMP_MSC_IN_EPT, msc_buf, (uint16_t)n);
        return;
    }
    send_csw(udev);
}

/* Dropped host data is received ONE PACKET per completion: the core's
 * multi-packet receive re-calls usbd_ept_recv() for every continuation
 * packet (usbd_int.c), which zeroes trans_len, so usbd_get_recv_len() after a
 * longer receive would report only the last packet and the transport would
 * send its CSW while the host is still writing. Speed is irrelevant here:
 * hosts see write-protect and do not write; this path exists so one that
 * does anyway stays in step. */
static void arm_data_out(usbd_core_type *udev)
{
    uint32_t n = m.recv_left < USB_MSC_MPS ? m.recv_left : USB_MSC_MPS;
    m.state = BOT_DATA_OUT;
    usbd_ept_recv(udev, USB_COMP_MSC_OUT_EPT, msc_buf, (uint16_t)n);
}

static void start_command(usbd_core_type *udev)
{
    msc_result_t r;
    msc_plan_t p;
    msc_scsi_command(&m.lun, &m.cbw, &r);
    msc_plan(&m.cbw, &r, &p);
    trace_begin(&m.cbw, &r, &m.lun);

    m.st.commands++;
    m.st.last_op = m.cbw.cb[0];
    if (r.xfer == MSC_XFER_OUT || m.cbw.cb[0] == 0x04u) m.st.writes_refused++;
    m.status = p.status;
    m.residue = p.residue;
    m.send_left = p.send;
    m.pad_left = p.pad;
    m.recv_left = p.recv;
    m.io_error = false;

    if (m.recv_left > 0u) {
        arm_data_out(udev);
        return;
    }
    if (m.send_left > 0u && r.xfer == MSC_XFER_IN_BUF) {
        memcpy(msc_buf, r.buf, m.send_left);
        uint32_t n = m.send_left;
        m.send_left = 0;
        m.state = BOT_DATA_IN;
        usbd_ept_send(udev, USB_COMP_MSC_IN_EPT, msc_buf, (uint16_t)n);
        return;
    }
    if (m.send_left > 0u) {          /* READ: sectors come from the task */
        m.st.reads++;
        m.st.last_lba = r.lba;
        m.next_lba = r.lba;
    }
    continue_in(udev);
}

void usb_msc_out_done(void *udev)
{
    usbd_core_type *pudev = (usbd_core_type *)udev;
    const uint32_t got = usbd_get_recv_len(pudev, USB_COMP_MSC_OUT_EPT);

    if (m.state == BOT_IDLE) {
        if (!msc_cbw_parse(msc_buf, got, 0u, &m.cbw)) {
            /* BOT 6.6.1: not valid or not meaningful -> halt both pipes;
             * they stay halted until a Bulk-Only Mass Storage Reset. */
            m.st.bad_cbw++;
            m.state = BOT_STALLED;
            usbd_set_stall(pudev, USB_COMP_MSC_IN_EPT);
            usbd_set_stall(pudev, USB_COMP_MSC_OUT_EPT);
            return;
        }
        start_command(pudev);
        return;
    }
    if (m.state == BOT_DATA_OUT) {
        const uint32_t asked = m.recv_left < USB_MSC_MPS ? m.recv_left : USB_MSC_MPS;
        m.recv_left -= (got < m.recv_left) ? got : m.recv_left;
        /* a short packet means the host has nothing more to send */
        if (m.recv_left > 0u && got >= asked) {
            arm_data_out(pudev);
            return;
        }
        send_csw(pudev);
        return;
    }
    /* any other state: a packet we did not ask for; ignore it */
}

void usb_msc_in_done(void *udev)
{
    usbd_core_type *pudev = (usbd_core_type *)udev;
    if (m.state == BOT_DATA_IN) {
        continue_in(pudev);
    } else if (m.state == BOT_CSW) {
        arm_cbw(pudev);
    }
}

void usb_msc_class_init(void *udev)
{
    usbd_core_type *pudev = (usbd_core_type *)udev;
    /* packet memory: usb_conf.h EPT3_* (the CDC endpoints keep EPT1/EPT2) */
    usbd_ept_buf_custom_define(pudev, USB_COMP_MSC_IN_EPT, EPT3_TX_ADDR);
    usbd_ept_buf_custom_define(pudev, USB_COMP_MSC_OUT_EPT, EPT3_RX_ADDR);
    usbd_ept_open(pudev, USB_COMP_MSC_IN_EPT, EPT_BULK_TYPE, USB_MSC_MPS);
    usbd_ept_open(pudev, USB_COMP_MSC_OUT_EPT, EPT_BULK_TYPE, USB_MSC_MPS);
    m.epoch++;
    m.configured = true;
    arm_cbw(pudev);
}

void usb_msc_class_clear(void *udev)
{
    usbd_core_type *pudev = (usbd_core_type *)udev;
    usbd_ept_close(pudev, USB_COMP_MSC_IN_EPT);
    usbd_ept_close(pudev, USB_COMP_MSC_OUT_EPT);
    m.epoch++;
    m.configured = false;
    m.state = BOT_IDLE;
}

void usb_msc_bus_reset(void)
{
    m.epoch++;
    m.configured = false;
    m.state = BOT_IDLE;
}

usb_sts_type usb_msc_setup(void *udev, usb_setup_type *setup)
{
    usbd_core_type *pudev = (usbd_core_type *)udev;
    static uint8_t zero_byte;   /* max LUN = 0, alternate setting = 0 */

    if ((setup->bmRequestType & USB_REQ_TYPE_RESERVED) == USB_REQ_TYPE_CLASS) {
        if (setup->bRequest == 0xFEu && (setup->bmRequestType & USB_REQ_DIR_DTH) && setup->wLength >= 1u) {
            zero_byte = 0;                               /* GET MAX LUN: one LUN */
            usbd_ctrl_send(pudev, &zero_byte, 1);
            return USB_OK;
        }
        if (setup->bRequest == 0xFFu && setup->wLength == 0u) {
            /* Bulk-Only Mass Storage Reset (BOT 3.1): ready for a CBW. The
             * host clears both halts next (Reset Recovery, 5.3.4); the core
             * resets the data toggles there. The core sends the status stage. */
            m.st.resets++;
            m.epoch++;
            arm_cbw(pudev);
            return USB_OK;
        }
        usbd_ctrl_unsupport(pudev);
        return USB_NOT_SUPPORT;
    }

    switch (setup->bRequest) {
    case USB_STD_REQ_GET_INTERFACE:
        zero_byte = 0;
        usbd_ctrl_send(pudev, &zero_byte, 1);
        return USB_OK;
    case USB_STD_REQ_SET_INTERFACE:
        return USB_OK;
    case USB_STD_REQ_CLEAR_FEATURE:
        /* The core already cleared the halt. After an invalid CBW the pipes
         * must stay halted until the reset (6.6.1), so halt it again. */
        if (m.state == BOT_STALLED) usbd_set_stall(pudev, LBYTE(setup->wIndex));
        return USB_OK;
    default:
        usbd_ctrl_unsupport(pudev);
        return USB_NOT_SUPPORT;
    }
}

/* ── Task side ─────────────────────────────────────────────────────────── */

void usb_msc_set_notify_task(void *task_handle) { m.notify = (TaskHandle_t)task_handle; }

static bool read_slot(void *ctx, uint32_t slot, uint32_t off, void *buf, uint32_t len)
{
    (void)ctx;
    return gallery_read_bmp(slot, off, buf, len) == GALLERY_OK;
}

/* Rebuild the volume index if the gallery changed (a save or delete bumps
 * gallery_generation()). Never in the middle of a READ, so one command
 * always sees one consistent volume; the host is told with UNIT ATTENTION
 * and re-reads the FAT. */
static bool refresh_index(void)
{
    const uint32_t gen = gallery_generation();
    if (m.vfat_valid && gen == m.gallery_gen) return false;
    const uint8_t s = m.state;
    if (s == BOT_FILL || s == BOT_DATA_IN) return false;

    gallery_index_t idx;
    if (gallery_scan(&idx) != GALLERY_OK) return false;   /* flash not ready/busy: next poll */
    gallery_vfat_t v;
    gallery_vfat_build(&v, &idx);

    taskENTER_CRITICAL();
    m.vfat = v;
    m.vfat_valid = true;
    m.gallery_gen = gen;
    msc_lun_changed(&m.lun);     /* only queues UNIT ATTENTION while shared */
    m.st.rescans++;
    taskEXIT_CRITICAL();
    return true;
}

void usb_msc_prepare(void)
{
    (void)refresh_index();
}

bool usb_msc_poll(void *udev)
{
    usbd_core_type *pudev = (usbd_core_type *)udev;
    bool work = refresh_index();

    if (m.state != BOT_FILL) return work;

    const uint32_t epoch = m.epoch;
    const uint32_t lba = m.next_lba;
    uint32_t n = m.send_left < sizeof msc_buf ? m.send_left : sizeof msc_buf;
    vfat_status_t vs = gallery_vfat_read_sector(&m.vfat, lba, msc_buf, read_slot, NULL);
    m.st.sectors++;

    taskENTER_CRITICAL();
    if (m.epoch == epoch && m.state == BOT_FILL) {
        if (vs != VFAT_OK) {
            /* The data phase is already promised; finish it with zeros and
             * fail the CSW with the sense a host retries on. */
            m.io_error = true;
            m.st.io_errors++;
            msc_lun_set_sense(&m.lun, SCSI_SK_MEDIUM_ERROR, SCSI_ASC_UNRECOVERED_READ);
        }
        m.next_lba = lba + 1u;
        m.send_left -= n;
        m.state = BOT_DATA_IN;
        usbd_ept_send(pudev, USB_COMP_MSC_IN_EPT, msc_buf, (uint16_t)n);
    }
    taskEXIT_CRITICAL();
    return true;
}

void usb_msc_set_shared(bool on)
{
    taskENTER_CRITICAL();
    msc_lun_set_present(&m.lun, on);
    taskEXIT_CRITICAL();
}

bool usb_msc_shared(void) { return m.lun.present; }

void usb_msc_get_stats(usb_msc_stats_t *out)
{
    taskENTER_CRITICAL();
    *out = m.st;
    out->shared = m.lun.present;
    out->host_ejected = m.lun.eject_request;
    out->configured = m.configured;
    out->state = m.state;
    out->files = m.vfat.count;
    out->blocks = m.lun.block_count;
    taskEXIT_CRITICAL();
}
