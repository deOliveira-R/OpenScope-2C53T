/*
 * OpenScope 2C53T - USB serial port (CDC)
 *
 * The USB device, its interrupt, the bounded send and the switch between
 * the serial-only device and serial plus the gallery drive.
 */

#include "shell_parts.h"

/* ── USB device instance ────────────────────────────────────────────── */

usbd_core_type usb_core_dev;

/* ── Delay helpers the USB middleware needs (usb_conf.h) ────────────── */

void usb_delay_ms(uint32_t ms)
{
    /* FreeRTOS delay when the scheduler runs, else an approximate busy
     * loop. */
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        vTaskDelay(pdMS_TO_TICKS(ms));
    } else {
        volatile uint32_t count;
        while (ms--) {
            count = system_core_clock / 10000;
            while (count--) __asm volatile("nop");
        }
    }
}

void usb_delay_us(uint32_t us)
{
    volatile uint32_t count;
    count = (system_core_clock / 1000000) * us;
    while (count--) __asm volatile("nop");
}

#if USB_MSC
/* ── Enumeration trace and the USB interrupt handler ─────────────────── */
/* Enumeration trace: the first USB_TRACE_N bus resets and SETUP packets
 * after each attach, timestamped, with "stalled" when the device answered
 * with a STALL. `usb trace` prints it. After a fallback it shows exactly
 * how far the host got with the composite device (the last request before
 * the 3 s silence is the one to look at). 12 B per entry; the CDC re-attach
 * after a fallback keeps the failed attempt's trace. */
#define USB_TRACE_N 48

enum { UTR_RESET = 1, UTR_SETUP, UTR_CONFIGURED, UTR_TIMEOUT };

typedef struct {
    uint16_t t_ms;
    uint8_t  kind;
    uint8_t  stalled;
    uint8_t  setup[8];
} usb_trace_ent_t;

static usb_trace_ent_t s_utrace[USB_TRACE_N];

static volatile uint8_t s_utrace_n;

static uint32_t s_utrace_t0;

static uint8_t s_utrace_dev;          /* usb_dev_kind_t of this attach */

/* Append one trace entry (setup: 8 bytes or NULL), time relative to the
 * attach; dropped when the trace is full. Called from the USB interrupt, or
 * with it masked. */
static void usb_trace_add(uint8_t kind, const uint8_t *setup)
{
    uint8_t n = s_utrace_n;
    if (n >= USB_TRACE_N) return;
    usb_trace_ent_t *e = &s_utrace[n];
    e->t_ms = (uint16_t)(xTaskGetTickCountFromISR() - s_utrace_t0);
    e->kind = kind;
    e->stalled = 0;
    if (setup != NULL) memcpy(e->setup, setup, 8); else memset(e->setup, 0, 8);
    s_utrace_n = (uint8_t)(n + 1u);
}

static void usb_trace_reset(uint8_t dev)
{
    s_utrace_n = 0;
    s_utrace_t0 = xTaskGetTickCount();
    s_utrace_dev = dev;
}

#endif
/* USB interrupt. In a USB_MSC build it first records bus resets and SETUP
 * packets (read from packet memory before the library consumes them), and
 * after the library has run marks the SETUP it answered with a STALL. */
void USBFS_L_CAN1_RX0_IRQHandler(void)
{
#if USB_MSC
    const uint8_t n0 = s_utrace_n;
    const uint32_t sts = USB->intsts;
    if (n0 < USB_TRACE_N) {
        if (sts & USB_RST_FLAG) usb_trace_add(UTR_RESET, NULL);
        if ((sts & USB_TC_FLAG) && (sts & USB_EPT_NUM_FLAG) == 0u &&
            (USB->ept[0] & USB_SETUPTC)) {
            uint8_t pkt[8];
            usb_read_packet(pkt, EPT0_RX_ADDR, 8);
            usb_trace_add(UTR_SETUP, pkt);
        }
    }
#endif
    usbd_irq_handler(&usb_core_dev);
#if USB_MSC
    if (s_utrace_n > n0 && s_utrace[s_utrace_n - 1u].kind == UTR_SETUP &&
        (USB->ept[0] & USB_TXSTS) == USB_TX_STALL)
        s_utrace[s_utrace_n - 1u].stalled = 1;
#endif
}

#if USB_MSC
usb_mode_sm_t s_usb_mode;

#endif
/* ── Initialisation ─────────────────────────────────────────────────── */

void usb_debug_init(void)
{
#ifdef EMULATOR_BUILD
    return;  /* No USB in emulator */
#else
    /* At 240 MHz no USB divider of the PLL gives 48 MHz, so the USB clock
     * comes from HICK (internal RC, 48 MHz), trimmed by ACC. */
    crm_usb_clock_source_select(CRM_USB_CLOCK_SOURCE_HICK);

    /* ACC trims HICK against the host's 1 ms SOF: C2 is the target count
     * per SOF, C1 and C3 the window around it. */
    crm_periph_clock_enable(CRM_ACC_PERIPH_CLOCK, TRUE);
    acc_write_c1(7980);
    acc_write_c2(8000);
    acc_write_c3(8020);
    acc_calibration_mode_enable(ACC_CAL_HICKTRIM, TRUE);

    /* Enable USB peripheral clock */
    crm_periph_clock_enable(CRM_USB_PERIPH_CLOCK, TRUE);

    /* Enable USB interrupt (low priority, below FreeRTOS syscall ceiling) */
    nvic_irq_enable(USBFS_L_CAN1_RX0_IRQn, 6, 0);

    /* Initialize USB device core: CDC, or CDC + read-only MSC (USB Sharing).
     * The composite keeps the CDC on the same interfaces, endpoints and
     * VID:PID, so the shell below is unaware of the difference. */
#if USB_MSC
    /* Boot as the plain CDC device (exactly the USB_MSC=0 descriptors).
     * The composite one is presented only on request, behind the
     * fallback in usb_mode_poll(): a host that rejects it never costs the
     * shell. */
    usb_mode_sm_init(&s_usb_mode);
    usb_trace_reset(USB_DEV_CDC);
#endif
    usbd_core_init(&usb_core_dev, USB, &cdc_class_handler, &cdc_desc_handler, 0);

    /* Enable the USB pull-up: the device becomes visible to the host. */
    usbd_connect(&usb_core_dev);
#endif
}

/* ── Output ─────────────────────────────────────────────────────────── */

bool usb_debug_connected(void)
{
#ifdef EMULATOR_BUILD
    return false;
#else
    return usbd_connect_state_get(&usb_core_dev) == USB_CONN_STATE_CONFIGURED;
#endif
}

/* ── CDC endpoint health ──────────────────────────────────────────────
 * The CDC bulk pair shares endpoint register 1. A completion the library
 * lost (usb_conf.h guards the CTR read-modify-write race that causes it)
 * would wedge the shell until a USB replug: g_tx_completed never returns,
 * so every send waits its 1 s and is dropped; or the OUT endpoint stays NAK
 * and nothing more is received. These checks recognise the stuck state from
 * the hardware itself and recover, with the USB interrupt masked so they
 * cannot interleave with it:
 *   TX lost:  g_tx_completed == 0, STAT_TX == NAK (the packet went out: the
 *             hardware only leaves VALID after the host ACKed it), no TXTC
 *             pending  ->  the completion happened; mark it.
 *   RX lost:  g_rx_completed == 0, STAT_RX == NAK (a packet arrived and the
 *             hardware stopped accepting), no RXTC pending  ->  read the
 *             packet out of packet memory exactly as the ISR would have.
 * Neither state exists legitimately: this task is the only one that clears
 * the flags, and it re-arms the endpoint (VALID) in the same breath.
 * Counters in `usb`. A host that simply is not reading (port closed) keeps
 * STAT_TX == VALID: after one full 1 s timeout further sends are dropped at
 * once until the host reads again, so the shell stays responsive. */
#define USB_CDC_EPT   (USBD_CDC_BULK_IN_EPT & 0x7Fu)

static struct {
    uint32_t tx_heals, rx_heals, tx_drops, tx_timeouts;
    bool     host_absent;
} s_cdc;

/* Mask the USB interrupt; returns whether it was enabled, for
 * usb_irq_restore(). */
static bool usb_irq_mask(void)
{
    const bool was = NVIC_GetEnableIRQ(USBFS_L_CAN1_RX0_IRQn) != 0u;
    NVIC_DisableIRQ(USBFS_L_CAN1_RX0_IRQn);
    __DSB();
    __ISB();
    return was;
}

static void usb_irq_restore(bool was)
{
    if (was) NVIC_EnableIRQ(USBFS_L_CAN1_RX0_IRQn);
}

/* "TX lost" above: mark the completion. Returns true when it healed. */
static bool cdc_tx_heal(cdc_struct_type *pcdc)
{
    bool healed = false;
    const bool was = usb_irq_mask();
    const uint32_t ep = USB->ept[USB_CDC_EPT];
    if (!pcdc->g_tx_completed && (ep & USB_TXSTS) == USB_TX_NAK && (ep & USB_TXTC) == 0u) {
        pcdc->g_tx_completed = 1;
        s_cdc.tx_heals++;
        healed = true;
    }
    usb_irq_restore(was);
    return healed;
}

/* "RX lost" above: read the waiting packet (at most one max packet) into
 * the class buffer and mark it received. Shell task, before each read. */
void cdc_rx_heal(cdc_struct_type *pcdc)
{
    if (pcdc->g_rx_completed) return;
    const bool was = usb_irq_mask();
    const uint32_t ep = USB->ept[USB_CDC_EPT];
    if (!pcdc->g_rx_completed && (ep & USB_RXSTS) == USB_RX_NAK && (ep & USB_RXTC) == 0u) {
        uint16_t n = USB_GET_RX_LEN(USB_CDC_EPT);
        if (n > USBD_CDC_OUT_MAXPACKET_SIZE) n = USBD_CDC_OUT_MAXPACKET_SIZE;
        usb_read_packet(pcdc->g_rx_buff, usb_core_dev.ept_out[USB_CDC_EPT].rx_addr, n);
        pcdc->g_rxlen = n;
        pcdc->g_rx_completed = 1;
        s_cdc.rx_heals++;
    }
    usb_irq_restore(was);
}

/* Send raw bytes to the console: always to RTT (SWD), and to USB CDC when
 * the device is configured (not in a DEBUG_SHELL_RTT_ONLY build). CDC goes
 * in max-packet chunks, each waiting for the previous one; a lost
 * completion is healed after 2 waits, and a host that does not read for
 * 1 s marks itself absent (see CDC endpoint health). No lock: shell task
 * callers only. */
void usb_send_bytes(const uint8_t *data, uint16_t len)
{
    rtt_write(data, len);

#if DEBUG_SHELL_RTT_ONLY
    (void)data; (void)len;
    return;
#else
    if (!usb_debug_connected()) return;

    cdc_struct_type *pcdc = (cdc_struct_type *)usb_core_dev.class_handler->pdata;
    const bool sched = xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;

    /* Send in 64-byte chunks (USB full-speed max packet) */
    while (len > 0) {
        uint16_t chunk = (len > USBD_CDC_IN_MAXPACKET_SIZE) ?
                         USBD_CDC_IN_MAXPACKET_SIZE : len;

        /* Wait for the previous packet to leave (or recover a lost
         * completion, or give up on a host that is not reading). */
        uint32_t waited = 0;
        while (!pcdc->g_tx_completed) {
            if (waited >= 2u && cdc_tx_heal(pcdc)) break;
            if (s_cdc.host_absent || waited >= 1000u) {
                if (!s_cdc.host_absent) s_cdc.tx_timeouts++;
                s_cdc.host_absent = true;
                s_cdc.tx_drops++;
                return;
            }
            if (sched) vTaskDelay(1);
            else usb_delay_us(1000);
            waited++;
        }
        s_cdc.host_absent = false;

        usb_vcp_send_data(&usb_core_dev, (uint8_t *)data, chunk);
        data += chunk;
        len -= chunk;
    }
#endif
}

#if USB_MSC
/* ── USB Sharing: which device we are (usb_mode_sm.h) ─────────────── */
static volatile int8_t s_share_req = -1;      /* -1 none, 0 off, 1 on: any task */

void usb_share_request(bool on)
{
    s_share_req = on ? 1 : 0;
}

bool usb_share_wanted(void)
{
    const int8_t r = s_share_req;
    return r >= 0 ? (r != 0) : usb_mode_sm_sharing(&s_usb_mode);
}

/* The composite device is up but the host ejected the drive. */
static bool usb_share_medium_out(void)
{
    return s_share_req < 0 && s_usb_mode.active == USB_DEV_COMPOSITE &&
           s_usb_mode.state == USB_ST_STEADY && !usb_msc_shared();
}

/* Settings "USB Sharing" OK: re-insert an ejected drive, else toggle. */
void usb_share_toggle(void)
{
    if (usb_share_medium_out()) usb_msc_set_shared(true);
    else usb_share_request(!usb_share_wanted());
}

const char *usb_share_label(void)
{
    if (!usb_share_wanted()) return "Off";
    return usb_share_medium_out() ? "Ejected" : "On";
}

/* Drop off the bus: close the class endpoints, pull-up off, connection
 * state back to DEFAULT, MSC state reset. Leaves the USB interrupt masked. */
static void usb_core_detach(void)
{
    (void)usb_irq_mask();
    /* close the class endpoints first so none stays armed across the switch */
    if (usb_core_dev.conn_state == USB_CONN_STATE_CONFIGURED &&
        usb_core_dev.class_handler->clear_handler != NULL)
        usb_core_dev.class_handler->clear_handler(&usb_core_dev);
    usbd_disconnect(&usb_core_dev);              /* D+ pull-up off: the host sees an unplug */
    usb_core_dev.conn_state = USB_CONN_STATE_DEFAULT;
    usb_msc_bus_reset();
    /* the interrupt stays off until the next attach */
}

/* Re-initialise the device core as `kind` (CDC, or CDC + MSC with the
 * medium present), restart the trace, unmask the interrupt, pull-up on. */
static void usb_core_attach(usb_dev_kind_t kind)
{
    (void)usb_irq_mask();
    if (kind == USB_DEV_COMPOSITE) {
        usb_msc_trace_reset();
        usb_msc_set_shared(true);                /* the medium is the point of sharing */
        usbd_core_init(&usb_core_dev, USB, &usb_composite_class_handler,
                       &usb_composite_desc_handler, 0);
    } else {
        usb_msc_set_shared(false);
        usbd_core_init(&usb_core_dev, USB, &cdc_class_handler, &cdc_desc_handler, 0);
    }
    /* keep the failed composite attempt's trace through the fallback */
    if (!(kind == USB_DEV_CDC && s_usb_mode.fallback_pending)) usb_trace_reset((uint8_t)kind);
    s_cdc.host_absent = false;
    NVIC_ClearPendingIRQ(USBFS_L_CAN1_RX0_IRQn);
    NVIC_EnableIRQ(USBFS_L_CAN1_RX0_IRQn);
    usbd_connect(&usb_core_dev);
}

/* Called every pass of the shell task (<= 10 ms apart when idle). */
void usb_mode_poll(void)
{
    taskENTER_CRITICAL();
    const int8_t r = s_share_req;
    s_share_req = -1;
    taskEXIT_CRITICAL();
    if (r >= 0) usb_mode_sm_request(&s_usb_mode, r != 0);

    const usb_mode_state_t before = s_usb_mode.state;
    const bool configured = usb_core_dev.conn_state == USB_CONN_STATE_CONFIGURED;
    const uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    switch (usb_mode_sm_step(&s_usb_mode, now, configured)) {
    case USB_ACT_DETACH:
        usb_core_detach();                       /* masks the USB interrupt */
        if (before == USB_ST_ATTACHING) usb_trace_add(UTR_TIMEOUT, NULL);
        break;
    case USB_ACT_ATTACH:
        /* index the gallery first: the host's first look sees the final
         * volume, no UNIT ATTENTION in the middle of its probe */
        if (s_usb_mode.active == USB_DEV_COMPOSITE) usb_msc_prepare();
        usb_core_attach(s_usb_mode.active);
        break;
    default:
        if (before == USB_ST_ATTACHING && s_usb_mode.state == USB_ST_STEADY && configured) {
            const bool was = usb_irq_mask();
            usb_trace_add(UTR_CONFIGURED, NULL);
            usb_irq_restore(was);
        }
        break;
    }
}

#endif
void usb_send_str(const char *str)
{
    usb_send_bytes((const uint8_t *)str, strlen(str));
}

/* usb_send_str() for the feature shell files (shell_internal.h). */
void usb_debug_sh_send_str(const char *str)
{
    usb_send_str(str);
}

int usb_debug_printf(const char *fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (len > 0) {
        if (len > (int)sizeof(buf) - 1) len = sizeof(buf) - 1;
        usb_send_bytes((const uint8_t *)buf, len);
    }
    return len;
}

/* The stream sender's port into the CDC IN path (src/stream/stream.h).
 * tx_ready: true when the IN endpoint is free (with heal, after trying to
 * recover a lost completion). write: usb_send_bytes(), true when nothing was
 * dropped. Shell task only, like every other writer here. Built in every
 * image: without the stream nothing calls them and the linker drops them. */
bool usb_debug_stream_tx_ready(bool heal)
{
#if DEBUG_SHELL_RTT_ONLY
    (void)heal;
    return false;
#else
    if (!usb_debug_connected()) return false;
    cdc_struct_type *pcdc = (cdc_struct_type *)usb_core_dev.class_handler->pdata;
    if (pcdc->g_tx_completed) return true;
    return heal && cdc_tx_heal(pcdc);
#endif
}

bool usb_debug_stream_write(const uint8_t *data, uint16_t len)
{
    const uint32_t drops = s_cdc.tx_drops;
    usb_send_bytes(data, len);
    return s_cdc.tx_drops == drops;
}

/* ── usb: USB Sharing, the read-only gallery drive ─────────────────────
 * The unit boots as the plain CDC device. `usb share on` (or Settings "USB
 * Sharing") re-enumerates it as CDC + MSC: this port closes, and comes back
 * about a second later with the drive. If the host has not configured the
 * composite device within 3 s, the unit returns as CDC-only on its own and
 * `usb` reports FALLBACK; `usb trace` then shows the host's requests. */
/* One line: configured, heal, timeout and drop counters, host not reading. */
static void cmd_usb_cdc_counters(void)
{
    usb_debug_printf("CDC: conf=%d tx_heals=%lu rx_heals=%lu tx_timeouts=%lu tx_drops=%lu%s\r\n",
                     usb_debug_connected() ? 1 : 0,
                     (unsigned long)s_cdc.tx_heals, (unsigned long)s_cdc.rx_heals,
                     (unsigned long)s_cdc.tx_timeouts, (unsigned long)s_cdc.tx_drops,
                     s_cdc.host_absent ? " (host not reading)" : "");
}

#if USB_MSC
/* usb trace: the enumeration trace of the last attach, then the SCSI
 * commands the MSC side recorded, with sense data and repeat counts. */
static void cmd_usb_trace(void)
{
    static const char *const kind[] = { "?", "RESET", "SETUP", "CONFIGURED", "TIMEOUT" };
    const uint8_t n = s_utrace_n;
    usb_debug_printf("usb trace: attach as %s, %u events\r\n",
                     s_utrace_dev == USB_DEV_COMPOSITE ? "CDC+MSC" : "CDC", (unsigned)n);
    for (uint8_t i = 0; i < n; i++) {
        const usb_trace_ent_t *e = &s_utrace[i];
        if (e->kind == UTR_SETUP) {
            const uint8_t *q = e->setup;
            usb_debug_printf("  +%5u ms SETUP %02X %02X wValue=%04X wIndex=%04X wLength=%u%s\r\n",
                             (unsigned)e->t_ms, q[0], q[1], (unsigned)(q[2] | (q[3] << 8)),
                             (unsigned)(q[4] | (q[5] << 8)), (unsigned)(q[6] | (q[7] << 8)),
                             e->stalled ? "  -> STALL" : "");
        } else {
            usb_debug_printf("  +%5u ms %s\r\n", (unsigned)e->t_ms,
                             kind[e->kind < 5u ? e->kind : 0]);
        }
    }
    if (n >= USB_TRACE_N) usb_send_str("  (trace full)\r\n");

    uint32_t lost = 0;
    const uint32_t m = usb_msc_trace_count(&lost);
    usb_debug_printf("scsi trace: %lu entries%s\r\n", (unsigned long)m,
                     lost ? " (full: later commands not recorded)" : "");
    for (uint32_t i = 0; i < m; i++) {
        usb_msc_trace_t e;
        if (!usb_msc_trace_get(i, &e)) break;
        const char *name;
        switch (e.cdb[0]) {
        case 0x00: name = "TEST UNIT READY"; break;
        case 0x03: name = "REQUEST SENSE"; break;
        case 0x12: name = "INQUIRY"; break;
        case 0x1A: name = "MODE SENSE(6)"; break;
        case 0x1B: name = "START STOP UNIT"; break;
        case 0x1E: name = "PREVENT ALLOW"; break;
        case 0x23: name = "READ FMT CAPS"; break;
        case 0x25: name = "READ CAPACITY(10)"; break;
        case 0x28: name = "READ(10)"; break;
        case 0x2A: name = "WRITE(10)"; break;
        case 0x2F: name = "VERIFY(10)"; break;
        case 0x35: name = "SYNC CACHE(10)"; break;
        case 0x5A: name = "MODE SENSE(10)"; break;
        case 0x9E: name = "SERVICE ACT IN(16)"; break;
        case 0xA8: name = "READ(12)"; break;
        default:   name = "?"; break;
        }
        char hex[3 * 9 + 1];
        const uint32_t nb = e.cdb_len < 9u ? e.cdb_len : 9u;
        for (uint32_t k = 0; k < nb; k++) snprintf(hex + 3 * k, 4, "%02X ", e.cdb[k]);
        hex[nb ? 3 * nb - 1 : 0] = '\0';
        const char *st = e.status == 0 ? "PASS" : e.status == 1 ? "FAIL" : e.status == 2 ? "PHASE" : "no CSW";
        usb_debug_printf("  +%6lu ms %-18s %s -> %s", (unsigned long)e.t_ms, name, hex, st);
        if (e.sk != 0 || e.asc != 0) usb_debug_printf(" sense %02X/%02X/%02X", e.sk, e.asc, e.ascq);
        if (e.repeat) usb_debug_printf(" (x%u more)", (unsigned)e.repeat);
        usb_send_str("\r\n");
    }
}

#endif
/* usb [status] | usb share [on|off] | usb trace. In a USB_MSC=0 build only
 * the status line and the CDC counters exist. */
void cmd_usb(const char *args)
{
#if USB_MSC
    if (args != NULL && strncmp(args, "share", 5) == 0) {
        const char *a = args + 5;
        while (*a == ' ') a++;
        bool on;
        if (strcmp(a, "on") == 0 || strcmp(a, "1") == 0) on = true;
        else if (strcmp(a, "off") == 0 || strcmp(a, "0") == 0) on = false;
        else if (*a == '\0') {
            usb_debug_printf("usb: share %s\r\n", usb_share_wanted() ? "on" : "off");
            return;
        } else { usb_send_str("Usage: usb share [on|off]\r\n"); return; }
        if (on && usb_share_medium_out()) {
            /* the host ejected the drive: put the medium back (UNIT
             * ATTENTION tells the host), no re-enumeration */
            usb_msc_set_shared(true);
            usb_send_str("usb: share on: medium re-inserted (the host had ejected it)\r\n");
            return;
        }
        if (on == usb_share_wanted() && s_usb_mode.state == USB_ST_STEADY) {
            usb_debug_printf("usb: share already %s\r\n", on ? "on" : "off");
            return;
        }
        usb_debug_printf("usb: share %s: re-enumerating as %s. This port closes now and returns in"
                         " ~1 s (same name); reconnect. %s\r\n",
                         on ? "on" : "off", on ? "CDC + read-only drive" : "CDC only",
                         on ? "If the host does not configure it within 3 s the unit comes back"
                              " CDC-only by itself: 'usb' then says FALLBACK, 'usb trace' shows why."
                            : "");
        usb_share_request(on);
        return;
    }
    if (args != NULL && strcmp(args, "trace") == 0) {
        cmd_usb_trace();
        return;
    }
    if (args != NULL && *args != '\0' && strcmp(args, "status") != 0) {
        usb_send_str("Usage: usb [status] | usb share [on|off] | usb trace\r\n");
        return;
    }
    usb_debug_printf("USB: mode=%s state=%s share=%s last_switch=%s switches=%lu fallbacks=%lu"
                     " attach_ms=%lu\r\n",
                     s_usb_mode.active == USB_DEV_COMPOSITE ? "cdc+msc" : "cdc",
                     usb_mode_state_str(s_usb_mode.state), usb_share_wanted() ? "on" : "off",
                     usb_mode_result_str(s_usb_mode.last),
                     (unsigned long)s_usb_mode.switches, (unsigned long)s_usb_mode.fallbacks,
                     (unsigned long)s_usb_mode.attach_ms);
    cmd_usb_cdc_counters();
    if (s_usb_mode.active != USB_DEV_COMPOSITE) return;
    usb_msc_stats_t st;
    usb_msc_get_stats(&st);
    usb_debug_printf("MSC: conf=%d medium=%s%s files=%u blocks=%lux512 state=%u "
                     "cmds=%lu reads=%lu sectors=%lu ioerr=%lu badcbw=%lu resets=%lu "
                     "wrref=%lu rescans=%lu last_op=%02X last_lba=%lu\r\n",
                     st.configured ? 1 : 0, st.shared ? "in" : "out",
                     (!st.shared && st.host_ejected) ? " (host ejected; 'usb share on' re-inserts)" : "",
                     (unsigned)st.files, (unsigned long)st.blocks, (unsigned)st.state,
                     (unsigned long)st.commands, (unsigned long)st.reads,
                     (unsigned long)st.sectors, (unsigned long)st.io_errors,
                     (unsigned long)st.bad_cbw, (unsigned long)st.resets,
                     (unsigned long)st.writes_refused, (unsigned long)st.rescans,
                     (unsigned)st.last_op, (unsigned long)st.last_lba);
#else
    if (args != NULL && (strncmp(args, "share", 5) == 0 || strcmp(args, "trace") == 0)) {
        usb_send_str("usb: built with USB_MSC=0 (CDC only): no sharing\r\n");
        return;
    }
    usb_send_str("USB: mode=cdc (built with USB_MSC=0: CDC only)\r\n");
    cmd_usb_cdc_counters();
#endif
}
