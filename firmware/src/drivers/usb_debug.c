/*
 * OpenScope 2C53T - USB Debug Shell
 *
 * USB CDC virtual serial port for interactive FPGA reverse engineering.
 * Provides a text command interface over USB for sending FPGA commands,
 * reading GPIO/registers, and triggering SPI3 acquisitions without
 * needing to reflash firmware.
 *
 * Usage: screen /dev/tty.usbmodem* 115200
 */

#include "usb_debug.h"
#include "battery.h"
#include "at32f403a_407.h"
#include "usbd_core.h"
#include "usbd_int.h"
#include "cdc_class.h"
#include "cdc_desc.h"
/* USB_MSC (Makefile, default 1): enumerate as CDC + the read-only gallery
 * mass-storage LUN (usb_composite.c, usb_msc.c). 0 = the CDC-only device. */
#ifndef USB_MSC
#define USB_MSC 0
#endif
#if USB_MSC
#include "usb_composite.h"
#include "usb_msc.h"
#include "../util/usb_mode_sm.h"
#endif
#include "../util/raw_block.h"
#include "dfu_boot.h"
#include "flash_fs.h"
#include "fw_loader.h"
#include "cal_backup.h"
#include "can_scope.h"
#include "usercal.h"
#include "rtt.h"
#include "continuity_buzzer.h"

/* On the bench unit USB CDC never enumerates (error -71, unsolved), yet
 * usbd_connect_state_get() still reports CONFIGURED — so usb_send_bytes()
 * walks into the CDC TX path and the shell task stalls there instead of
 * returning to poll RTT. Measured 2026-08-11: the banner reached the RTT
 * buffer (rtt_write runs first) and the task never came back.
 *
 * RTT exists precisely to bypass that transport, so this flag cuts the CDC
 * half out entirely. Build with -DDEBUG_SHELL_RTT_ONLY=1.
 *
 * NOTE this is an isolation switch, not the real fix. The underlying defect is
 * that a half-alive USB stack can wedge the shell; the proper repair is to
 * count consecutive CDC TX timeouts and latch the transport dead. */
#ifndef DEBUG_SHELL_RTT_ONLY
#define DEBUG_SHELL_RTT_ONLY 0
#endif
#include "scope_trigger.h"
#include "signal_gen.h"
#include "dac_output.h"
#include "scope_vpos.h"
#include "scope_selfcal_run.h"
#include "../util/calpage.h"
#include "fpga.h"
#include "lcd.h"
#include "meter_auto.h"
#include "meter_autoselect.h"
#include "meter_data.h"
#include "meter_stock.h"
#include "ui.h"
#include "model.h"              /* meter state: the link policy (meter.link) */
#include "meter_link_policy.h"
#include "../ui/scope_state.h"
#include "../util/settings_store.h"
#include "../ui/siggen_edit.h"
#include "../ui/scope_cal.h"
#include "../ui/scope_freq.h"
#include "../ui/scope_measure.h"
#include "../ui/meas_select.h"
#include "../ui/measure_strip.h"   /* measure_strip_shown */
#include "../ui/scope_timebase.h"
#include "../ui/scope_roll.h"
#include "../ui/scope_geom.h"
#include "../ui/scope_mask.h"
#include "../dsp/trig_policy.h"
#include "../ui/key_model.h"
#include "../tasks/input_handler.h"
#include "../ui/settings_screen.h"   /* `ui dump`: the Settings page and rows */
#include "../util/shell_persist.h"
#include "../util/lcd_health.h"
#include "../ui/meter_voltage_wave.h"
#include "../ui/gallery_ui.h"
#include "shared_mem.h"
#include "../util/gallery_store.h"
#include "../ui/logo_ui.h"
#include "watchdog.h"
#include "../util/cal_backup_core.h"

#include "fpga_bitstream_stock.h"
#include "FreeRTOS.h"
#include "task.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include "model_shell.h"   /* get / set / list / describe (ARCH-1 M1 rows) */
#include "ui_shell.h"      /* ui press / long / seq / screen, btn, keys (ARCH-1 M2 rows) */
#include "../util/features.h"   /* version, features: what this build contains */
#include "stream.h"        /* binary stream over this channel (src/stream/): no-ops when off */
#include "shell_internal.h" /* the accessors below, for the feature shell files */
#include "model_exec.h"      /* `fpga roll code`: the re-apply goes through the executor */
#include "shell_bench.h"     /* bench shell rows (FEATURE_BENCHSHELL): empty when off */
#include "shell_meterdebug.h" /* meter diagnostics rows (FEATURE_METERDEBUG): empty when off */

/* ═══════════════════════════════════════════════════════════════════
 * USB Device Instance
 * ═══════════════════════════════════════════════════════════════════ */

static usbd_core_type usb_core_dev;

/* ═══════════════════════════════════════════════════════════════════
 * USB Delay Helpers (required by USB middleware via usb_conf.h)
 * ═══════════════════════════════════════════════════════════════════ */

void usb_delay_ms(uint32_t ms)
{
    /* Use FreeRTOS delay if scheduler is running, otherwise spin */
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

/* ═══════════════════════════════════════════════════════════════════
 * USB Interrupt Handler
 * ═══════════════════════════════════════════════════════════════════ */

#if USB_MSC
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
static usb_mode_sm_t s_usb_mode;
#endif

/* ═══════════════════════════════════════════════════════════════════
 * USB CDC Initialization
 * ═══════════════════════════════════════════════════════════════════ */

void usb_debug_init(void)
{
#ifdef EMULATOR_BUILD
    return;  /* No USB in emulator */
#else
    /* At 240MHz, PLL dividers can't produce 48MHz for USB.
     * Use HICK (internal RC oscillator) with ACC calibration instead.
     * This is the standard AT32 approach for non-48/72/96/etc. clocks. */
    crm_usb_clock_source_select(CRM_USB_CLOCK_SOURCE_HICK);

    /* Enable ACC and configure calibration for USB SOF sync */
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

    /* Enable USB pull-up — device becomes visible to host */
    usbd_connect(&usb_core_dev);
#endif
}

/* ═══════════════════════════════════════════════════════════════════
 * Output Helpers
 * ═══════════════════════════════════════════════════════════════════ */

bool usb_debug_connected(void)
{
#ifdef EMULATOR_BUILD
    return false;
#else
    return usbd_connect_state_get(&usb_core_dev) == USB_CONN_STATE_CONFIGURED;
#endif
}

/* Send raw bytes to the console.
 *
 * Two transports, both optional: RTT over the SWD wires and USB CDC. Output
 * goes to whichever is live. On the bench unit USB CDC has never enumerated
 * (CLAUDE.local.md), so in practice this is the RTT path — but the CDC path is
 * left intact so a unit with working USB behaves as before. */
/* ── CDC endpoint health ──────────────────────────────────────────────
 * The CDC bulk pair shares endpoint register 1. A completion the library
 * lost (usb_conf.h: the CTR read-modify-write race, now fixed there) used to
 * wedge the shell until a USB replug: g_tx_completed never returned, so
 * every send waited its 1 s and was dropped; or the OUT endpoint stayed NAK
 * and nothing more was received. These checks recognise the stuck state from
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

static void cdc_rx_heal(cdc_struct_type *pcdc)
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

static void usb_send_bytes(const uint8_t *data, uint16_t len)
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

/* ── USB Sharing: which device we are (usb_mode_sm.h) ─────────────── */
#if USB_MSC
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
static void usb_mode_poll(void)
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

static void usb_send_str(const char *str)
{
    usb_send_bytes((const uint8_t *)str, strlen(str));
}

/* For the feature shell files (shell_internal.h); usb_send_str() unchanged. */
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
 * Additions beside the frozen write path, which they call unchanged; shell
 * task only, like every other writer here. Built in every image: without
 * the stream nothing calls them and the linker drops them. */
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

/* ═══════════════════════════════════════════════════════════════════
 * Hex Parsing Helpers
 * ═══════════════════════════════════════════════════════════════════ */

/* Parse a hex string like "0x40021000" or "40021000" into uint32_t.
 * Returns 0 on success, -1 on error. */
static int parse_hex32(const char *s, uint32_t *out)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    char *end;
    *out = strtoul(s, &end, 16);
    return (*end == '\0' || *end == ' ' || *end == '\r' || *end == '\n') ? 0 : -1;
}

/* For the feature shell files (shell_internal.h); parse_hex32() unchanged. */
int usb_debug_sh_parse_hex32(const char *s, uint32_t *out)
{
    return parse_hex32(s, out);
}

/* Parse a decimal or hex string into uint32_t */
static int parse_int(const char *s, uint32_t *out)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        return parse_hex32(s, out);
    char *end;
    *out = strtoul(s, &end, 10);
    return (*end == '\0' || *end == ' ' || *end == '\r' || *end == '\n') ? 0 : -1;
}

/* For the feature shell files (shell_internal.h); parse_int() unchanged. */
int usb_debug_sh_parse_int(const char *s, uint32_t *out)
{
    return parse_int(s, out);
}

/* ═══════════════════════════════════════════════════════════════════
 * Which FPGA config path ran this boot — the witness the status shell
 * needs before it is allowed to report anything about configuration.
 *
 * WHY THIS EXISTS. On 2026-08-13 the `status` command printed an FPGA
 * config REFUSAL on a device that was configured and capturing: it read
 * hardware-SPI-only fields on a bit-bang (FPGA_CONFIG_B) build, where
 * that path never runs and the fields sit at their zero-init values.
 * Zero is a plausible reading, so the shell reported it as one.
 *
 * The same hole was still open in the other direction. On a build where
 * NO config path runs at all — FPGA_WARM_HANDOFF_TEST (stock configured
 * the part before the handoff) or FPGA_BUS_RELEASED_BOOT — fpga_init()
 * returns before any config sequence, every cfg_* field stays zero, and
 * the old two-way test (marker present? no) labelled that "hardware
 * SPI3" and decoded the all-zero STATUS as "NOT configured". Both halves
 * of that sentence were fabricated.
 *
 * Evidence used, in order:
 *   h2_upload_done  set to 1 by BOTH config paths (fpga.c bit-bang ~2209,
 *                   hardware-SPI ~2562) and by nothing else. Clear => no
 *                   config sequence ran this boot, so NOTHING downstream
 *                   of it may be reported as a measurement.
 *   diag_spi_ctrl1  == 0xBBBB is not a register value; the bit-bang
 *                   sequence stores it as a path marker (fpga.c ~2169).
 *                   The hardware-SPI path leaves the real CTRL1 there.
 *
 * This is inference from runtime witnesses on purpose: the config
 * sequences themselves are bench-validated and are not to be edited to
 * add a flag. `fpga reinit` re-runs the hardware-SPI path from the shell
 * and correctly re-labels the result, because it rewrites both witnesses.
 * ═══════════════════════════════════════════════════════════════════ */
typedef enum {
    CFG_PATH_NONE = 0,   /* no config sequence ran this boot */
    CFG_PATH_BITBANG,    /* fpga_bitbang_config_sequence() (FPGA_CONFIG_B) */
    CFG_PATH_HWSPI,      /* fpga_spi3_config_sequence() */
} cfg_path_t;

static cfg_path_t fpga_cfg_path(void)
{
    if (!fpga.h2_upload_done) return CFG_PATH_NONE;
    return (fpga.diag_spi_ctrl1 == 0xBBBBu) ? CFG_PATH_BITBANG : CFG_PATH_HWSPI;
}

static const char *fpga_cfg_path_name(cfg_path_t p)
{
    switch (p) {
    case CFG_PATH_BITBANG: return "GPIO bit-bang (FPGA_CONFIG_B)";
    case CFG_PATH_HWSPI:   return "hardware SPI3";
    default:               return "NONE — no config sequence ran this boot";
    }
}

static void usb_print_last_tx_frame(void)
{
    usb_send_str("last_tx_frame:");
    for (int i = 0; i < FPGA_TX_FRAME_SIZE; i++) {
        usb_debug_printf(" %02X", (unsigned)fpga.last_tx_frame[i]);
    }
    usb_send_str("\r\n");
}

/* For the feature shell files (shell_internal.h); usb_print_last_tx_frame() unchanged. */
void usb_debug_sh_print_last_tx_frame(void)
{
    usb_print_last_tx_frame();
}

static void usb_print_recent_tx_frames(void)
{
    uint8_t count = fpga.tx_frame_history_count;

    usb_send_str("tx_frames_recent:");
    for (uint8_t i = 0; i < count; i++) {
        uint8_t idx = (uint8_t)((fpga.tx_frame_history_head +
                                 FPGA_TX_FRAME_HISTORY - count + i) %
                                FPGA_TX_FRAME_HISTORY);
        usb_send_str(" [");
        for (int j = 0; j < FPGA_TX_FRAME_SIZE; j++) {
            usb_debug_printf("%s%02X",
                             j == 0 ? "" : " ",
                             (unsigned)fpga.tx_frame_history[idx][j]);
        }
        usb_send_str("]");
    }
    usb_send_str("\r\n");
}

/* For the feature shell files (shell_internal.h); usb_print_recent_tx_frames() unchanged. */
void usb_debug_sh_print_recent_tx_frames(void)
{
    usb_print_recent_tx_frames();
}

static void fpga_stock_diag_print(void)
{
    /* MCU-SIDE MODEL, NOT A DEVICE READ. Every field here is written by our own
     * `fpga stock ...` commands (fpga_stock_shadow_write); nothing in it is
     * clocked off the FPGA. All-zero means "not seeded", not "the FPGA reports
     * zero". Labelled because it prints next to blocks that ARE device reads. */
    usb_debug_printf(
        "\r\n=== Stock Shadow (MCU-side model — not read from the FPGA) ===\r\n"
        "F68..6B: %u / %u / %u / %u\r\n"
        "E1A..D:  %u / %u / %u / %u\r\n"
        "0x355:   %u\r\n",
        fpga.stock_shadow.visible_state,
        fpga.stock_shadow.phase,
        fpga.stock_shadow.substate,
        fpga.stock_shadow.flags,
        fpga.stock_shadow.e1a,
        fpga.stock_shadow.e1b,
        fpga.stock_shadow.e1c,
        fpga.stock_shadow.e1d,
        fpga.stock_shadow.latch_355
    );

    usb_debug_printf("E12..19:");
    for (size_t i = 0; i < sizeof(fpga.stock_shadow.detail_bits); i++) {
        usb_debug_printf(" %02X", fpga.stock_shadow.detail_bits[i]);
    }
    usb_send_str("\r\n");
}

/* For the feature shell files (shell_internal.h); fpga_stock_diag_print() unchanged. */
void usb_debug_sh_stock_diag_print(void)
{
    fpga_stock_diag_print();
}

/* ═══════════════════════════════════════════════════════════════════
 * Command Handlers
 * ═══════════════════════════════════════════════════════════════════ */

/* cmd_help is defined next to the command table at the bottom of the file —
 * it renders the per-row help strings, so it must live after the table. */
static void cmd_help(void);

/* `status` ends with the timebase in force (defined with the scope commands). */
static void timebase_print_status(bool after_set);

static void cmd_version(void)
{
    /* The compiler rides on the Build line: whether the RAM installer called
     * flash-resident memset (issue #42) depended on the toolchain, and every
     * bench script already logs the line that starts with "Build:". */
    usb_debug_printf(
        "OpenScope 2C53T\r\n"
        "Build: " __DATE__ " " __TIME__ ", gcc " __VERSION__ "\r\n"
#ifdef FPGA_ALT_BITSTREAM
        "FPGA payload: " FPGA_BITSTREAM_NAME " (ALT — not the stock scope design)\r\n"
#elif defined(FPGA_CORE_BITSTREAM)
        "FPGA built-in design: " FPGA_BITSTREAM_NAME " (OpenScope's own capture core)\r\n"
#else
        "FPGA built-in design: stock\r\n"
#endif
        "MCU: AT32F403A @ %uMHz\r\n"
        "SRAM: 224KB (EOPB0=0xFE)\r\n",
        system_core_clock / 1000000
    );
#ifdef MODEL_PROFILE_NAME
    /* Built with a defaults profile (make DEFAULTS=..., model.c): the
     * built-in defaults of this image are not all the table's. */
    usb_debug_printf("Defaults profile: " MODEL_PROFILE_NAME " (%d built-in defaults changed)\r\n",
                     MODEL_PROFILE_OVERRIDES);
#endif
    /* What this build contains (firmware/features.mk); `features` lists the
     * ones that are off too. */
    usb_send_str("Features:");
    for (uint8_t i = 0; i < features_count(); i++) {
        const feature_info_t *f = features_at(i);
        if (f->on) usb_debug_printf(" %s", f->name);
    }
    usb_debug_printf(" (mask 0x%08lx)\r\n", (unsigned long)features_mask());
}

/* features: every feature the manifest knows, one per line:
 *   <name> on|off bit <n>  <description>
 * so a script can tell "off in this build" from "never heard of it". */
static void cmd_features(void)
{
    for (uint8_t i = 0; i < features_count(); i++) {
        const feature_info_t *f = features_at(i);
        usb_debug_printf("%-10s %-3s bit %-2u %s\r\n", f->name, f->on ? "on" : "off", (unsigned)f->bit, f->desc);
    }
    usb_debug_printf("mask 0x%08lx\r\n", (unsigned long)features_mask());
}

static void cmd_status(void)
{
    extern volatile uint32_t uptime_seconds;

    /*
     * Keep the status header in chunks smaller than usb_debug_printf()'s
     * 256-byte buffer. A single oversized format string silently truncates the
     * output and can glue the following line onto "SPI3 OK", which makes live
     * DMM evidence harder to compare across firmware builds.
     */
    usb_debug_printf(
        "=== System ===\r\n"
        "Uptime: %lus\r\n"
        "SYSCLK: %uMHz\r\n"
        "\r\n=== FPGA ===\r\n"
        "Initialized: %s\r\n"
        "SPI3 active: %s\r\n"
        "TX count: %u\r\n"
        "RX bytes: %u\r\n"
        "Data frames: %u\r\n"
        "Echo frames: %u\r\n"
        "RX sync: data_start=%u echo_start=%u data_hdr=%u echo_hdr=%u bad2=%u stray=%u\r\n",
        (unsigned long)uptime_seconds,
        system_core_clock / 1000000,
        fpga.initialized ? "YES" : "NO",
        fpga.spi3_active ? "YES" : "NO",
        fpga.tx_count,
        fpga.rx_byte_count,
        fpga.frame_count,
        fpga.echo_count,
        fpga.rx_sync_data_start_count,
        fpga.rx_sync_echo_start_count,
        fpga.rx_sync_data_header_count,
        fpga.rx_sync_echo_header_count,
        fpga.rx_sync_bad_second_count,
        fpga.rx_sync_stray_count
    );
    usb_debug_printf(
        "RX gates: data_tx_busy_drop=%u echo_valid=%u echo_bad=%u\r\n",
        fpga.rx_data_tx_busy_drop_count,
        fpga.rx_echo_valid_count,
        fpga.rx_echo_bad_count
    );
    usb_debug_printf(
        "SPI3 OK: %u\r\n"
        "SPI3 timeouts: %u (total %u, hw poll expiries %u)\r\n"
        "SPI3 first byte: 0x%02X\r\n",
        fpga.spi3_ok_count,
        fpga.spi3_timeout_count, fpga.spi3_total_timeouts,
        fpga.spi3_hw_timeouts,
        fpga.spi3_first_byte
    );
    /* Trigger-regime instruments (2026-08-14): PC0 falling edges = fresh
     * data-ready events (sample twice to get a rate); acq headers are the 3
     * MISO bytes of the last 04/05 read — stock's CH1 b2==01 = buffer valid. */
    usb_debug_printf(
        "PC0 edges: %lu\r\n"
        "acq latency: %lu ms (strobe -> commit, last cycle); poll reads before it: %lu (total %lu)\r\n"
        "acq edge filter: %s  kept %lu  dropped %lu  unclassified %lu\r\n"
        "acq rotation: %ld samples (seam of the last strobed record; -1 = none)\r\n"
        "acq hdr CH1: %02X %02X %02X  CH2: %02X %02X %02X\r\n",
        (unsigned long)fpga.pc0_edges,
        (unsigned long)fpga.acq_last_latency_ms, (unsigned long)fpga.acq_polls_last, (unsigned long)fpga.acq_poll_reads,
        fpga_acq_edge_filter_get() ? "ON" : "OFF", (unsigned long)fpga.acq_edge_kept,
        (unsigned long)fpga.acq_edge_dropped, (unsigned long)fpga.acq_edge_unknown,
        (long)fpga.acq_last_rot,
        fpga.acq_hdr_ch1[0], fpga.acq_hdr_ch1[1], fpga.acq_hdr_ch1[2],
        fpga.acq_hdr_ch2[0], fpga.acq_hdr_ch2[1], fpga.acq_hdr_ch2[2]
    );
    /* The timebase in force (reg 0x01, sample rate, s/div) and what the boot
     * reconcile did: `get tb.code` gives the code and the label only. */
    timebase_print_status(false);
    usb_print_last_tx_frame();
    usb_print_recent_tx_frames();

    /* spi3_first_byte is written only when an acquisition read is actually
     * attempted (fpga.c ~1589 normal path, ~1718 warm-handoff 0x04 path). With
     * no attempt it is still 0x00 from .bss, which is indistinguishable from a
     * real 0x00 off the wire. Counters are the witness: a read attempt either
     * succeeds (ok_count) or times out (total_timeouts). */
    if (fpga.spi3_ok_count == 0 && fpga.spi3_total_timeouts == 0)
        usb_send_str("SPI3 first byte: n/a (no acquisition read attempted yet)\r\n");
    else
        usb_debug_printf("SPI3 first byte: 0x%02X\r\n", fpga.spi3_first_byte);

    /* Split FPGA diag into separate printf to avoid buffer overflow */
    usb_debug_printf(
        "\r\n=== FPGA Diag ===\r\n"
        "IOMUX remap: 0x%08lX (init)\r\n"
        "IOMUX remap5: 0x%08lX (init)\r\n"
        "IOMUX remap LIVE: 0x%08lX\r\n"
        "IOMUX remap5 LIVE: 0x%08lX\r\n"
        /* 0xBBBB is not a register value — fpga_bitbang_config_sequence()
         * stores it as a "this was the bit-bang build" marker. Printing it
         * bare invites reading it as a real CTRL1. */
        "SPI3 CTRL1: 0x%04lX%s  STS: 0x%04lX (both captured at SPI3 init)\r\n"
        "PB4(MISO) IDT: %d  PC6(EN): %d  PB6(CS): %d (live pin reads)\r\n",
        fpga.diag_remap5,
        fpga.diag_remap7,
        (unsigned long)IOMUX->remap,
        (unsigned long)IOMUX->remap5,
        fpga.diag_spi_ctrl1,
        (fpga.diag_spi_ctrl1 == 0xBBBBu) ? " (marker: bit-bang build, not a register read)" : "",
        fpga.diag_spi_sts,
        (GPIOB->idt & (1 << 4)) ? 1 : 0,
        (GPIOC->idt & (1 << 6)) ? 1 : 0,
        (GPIOB->idt & (1 << 6)) ? 1 : 0
    );

    /* ── SPI3 handshake MISO capture ──────────────────────────────────────
     *
     * init_hs[0..10] is written ONLY by fpga_spi3_config_sequence() (fpga.c
     * ~2449-2550). The bit-bang path does not touch it, and a build that runs
     * no config path at all does not either — so on those builds G1/G2/G3 read
     * 00 00 00 00, which is also what a dead MISO line looks like. That is the
     * exact defect fixed for the H2 block on 2026-08-13, still open here.
     *
     * init_hs[11] ("Probe") is separate: it is written in fpga_init() Step 10
     * (fpga.c ~3493), which runs on the hardware-SPI AND bit-bang boots but is
     * skipped by the warm-handoff / bus-released early return. diag_probe_valid
     * is set on the same line, so it is reported on its own witness rather than
     * being lumped in with the handshake bytes. */
    {
        cfg_path_t path = fpga_cfg_path();

        usb_send_str("\r\n=== SPI3 Handshake (11 bytes) ===\r\n");
        if (path == CFG_PATH_HWSPI) {
            usb_debug_printf(
                "G1: %02X %02X %02X %02X  G2: %02X %02X %02X\r\n"
                "G3: %02X %02X %02X %02X\r\n",
                fpga.init_hs[0], fpga.init_hs[1], fpga.init_hs[2], fpga.init_hs[3],
                fpga.init_hs[4], fpga.init_hs[5], fpga.init_hs[6],
                fpga.init_hs[7], fpga.init_hs[8], fpga.init_hs[9], fpga.init_hs[10]);
        } else {
            usb_debug_printf("G1/G2/G3: n/a — never captured (config path: %s)\r\n",
                             fpga_cfg_path_name(path));
        }

        if (fpga.diag_probe_valid)
            usb_debug_printf("Probe (post-init 0xFF): %02X\r\n", fpga.init_hs[11]);
        else
            usb_send_str("Probe (post-init 0xFF): n/a — fpga_init Step 10 did not run\r\n");
    }

    /* ── Config status: the field that actually says whether we configured ──
     *
     * 2026-08-13: this block used to print only the hardware-SPI fields below,
     * unconditionally. Under a bit-bang build (FPGA_CONFIG_B) that path never
     * runs, so `0x3A close status` and `0x03 scope status` sit at their
     * zero-init values — and the shell confidently printed 00 / 00 00 00 00,
     * the exact signature of a REFUSED config, on a device that was configured
     * and capturing. Same family as the /2 status reads and the floating MISO:
     * an instrument reporting on something it cannot see.
     *
     * cfg_status_reg[] is the Gowin STATUS register (0x41), populated by
     * whichever config path ran. It is the authoritative answer, so it goes
     * first and is decoded rather than left as raw hex. */
    {
        uint32_t sr = ((uint32_t)fpga.cfg_status_reg[0] << 24) |
                      ((uint32_t)fpga.cfg_status_reg[1] << 16) |
                      ((uint32_t)fpga.cfg_status_reg[2] << 8)  |
                      (uint32_t)fpga.cfg_status_reg[3];
        cfg_path_t path = fpga_cfg_path();

        usb_debug_printf("\r\n=== FPGA Config ===\r\n"
                         "path: %s\r\n", fpga_cfg_path_name(path));

        if (path == CFG_PATH_NONE) {
            /* Nothing below this line was measured. Say so and print nothing
             * that could be mistaken for a reading — cfg_status_reg,
             * probe_id_bit_close, h2_close_status and scope_status are all
             * still at their .bss zeros, and probe_id_bit_close == 0 in
             * particular would otherwise decode as "anchor matched at bit 0". */
            usb_send_str(
                "STATUS(0x41):  n/a — not read\r\n"
                "IDCODE anchor: n/a — not read\r\n"
                "H2 upload:     n/a — no bitstream sent this boot\r\n"
                "0x3A close status: n/a\r\n"
                "0x03 scope status: n/a\r\n"
                "NOTE: on a warm-handoff or bus-released build the FPGA may be\r\n"
                "      fully configured by stock; this shell simply cannot see it.\r\n");
        } else {
            /* ANCHOR FIRST — the project's mandatory method (CLAUDE.md, Exp J).
             * fpga.c reads IDCODE at /256 in the SAME window as this status, so
             * the status is only believed on a read path validated against a
             * known answer. The shell never showed it, which left STATUS looking
             * like a bare fact. Exp L: a part that configured successfully STOPS
             * answering SSPI, so id_bit_close < 0 is the success-shaped reading
             * and id_bit_close >= 0 proves the port never closed. */
            usb_debug_printf(
                "STATUS(0x41): %08lX  DONE_FINAL(13): %s\r\n"
                "IDCODE anchor after close: %s\r\n",
                (unsigned long)sr,
                ((sr >> 13) & 1u) ? "SET — configured" : "clear — NOT configured",
                (fpga.probe_id_bit_close < 0)
                    ? "silent (config port closed — consistent with configured;"
                      " a dead bus reads the same)"
                    : "still answering 0x0120681B => port OPEN => NOT configured");

            usb_debug_printf("\r\n=== H2 Bitstream Upload ===\r\n"
                             "Bytes sent: %lu / 115638\r\n"
                             "Upload done: %s\r\n",
                             fpga.h2_bytes_sent,
                             fpga.h2_upload_done ? "YES" : "NO");

            /* The two hardware-SPI-only readbacks. Suppress them on the bit-bang
             * path rather than printing zeros that read as a refusal. */
            if (path == CFG_PATH_BITBANG) {
                usb_send_str("0x3A close status: n/a (bit-bang path does not read it)\r\n"
                             "0x03 scope status: n/a (bit-bang path does not read it)\r\n");
            } else {
                usb_debug_printf(
                    "0x3A close status: %02X (stock: F8)\r\n"
                    "0x03 scope status: %02X %02X %02X %02X (stock: 00 01 42 2E)\r\n",
                    fpga.h2_close_status,
                    fpga.scope_status[0], fpga.scope_status[1],
                    fpga.scope_status[2], fpga.scope_status[3]);
            }
        }

        usb_debug_printf(
            "post-H2 SPI3 boot: enq=%u run=%u drop=%u mask=0x%02X\r\n",
            fpga.post_h2_spi3_boot_enqueued,
            fpga.post_h2_spi3_boot_run_count,
            fpga.post_h2_spi3_boot_dropped,
            fpga.post_h2_spi3_boot_mask);
        for (uint8_t i = 0; i < FPGA_POST_H2_TRIGGER_HISTORY; i++) {
            uint8_t len = fpga.post_h2_spi3_rx_len[i];
            usb_debug_printf("post-H2 rx[%u]: trigger=%02X len=%u bytes=",
                             (unsigned)i,
                             (unsigned)fpga.post_h2_spi3_trigger[i],
                             (unsigned)len);
            uint8_t shown = len;
            if (shown > FPGA_POST_H2_RX_HISTORY) shown = FPGA_POST_H2_RX_HISTORY;
            for (uint8_t j = 0; j < shown; j++) {
                usb_debug_printf("%s%02X", j == 0 ? "" : " ",
                                 (unsigned)fpga.post_h2_spi3_rx[i][j]);
            }
            if (len > FPGA_POST_H2_RX_HISTORY) usb_send_str(" ...");
            usb_send_str("\r\n");
        }
    }

    fpga_stock_diag_print();

    /* Show last RX frame if valid */
    if (fpga.rx_frame_valid) {
        usb_debug_printf("Last RX frame:");
        for (int i = 0; i < FPGA_RX_FRAME_SIZE; i++)
            usb_debug_printf(" %02X", fpga.rx_frame[i]);
        usb_send_str("\r\n");
    }
}

static void cmd_flash_jedec(void)
{
    uint8_t manufacturer = 0;
    uint8_t memory_type = 0;
    uint8_t capacity = 0;

    flash_fs_error_t err = flash_fs_raw_read_jedec(&manufacturer, &memory_type, &capacity);
    if (err != FLASH_FS_OK) {
        usb_debug_printf("ERR: flash jedec failed (%d)\r\n", (int)err);
        return;
    }

    usb_debug_printf("SPI flash JEDEC: %02X %02X %02X\r\n",
                     manufacturer, memory_type, capacity);
}

static void cmd_flash_read(const char *args)
{
    char buf[64];
    char *saveptr = NULL;
    char *tok;
    uint32_t addr;
    uint32_t len;
    uint8_t data[256];

    if (strlen(args) >= sizeof(buf)) {
        usb_send_str("Usage: flash read <addr> <len>\r\n");
        return;
    }

    strcpy(buf, args);
    tok = strtok_r(buf, " \t", &saveptr);
    if (tok == NULL || parse_int(tok, &addr) != 0) {
        usb_send_str("Usage: flash read <addr> <len>\r\n");
        return;
    }

    tok = strtok_r(NULL, " \t", &saveptr);
    if (tok == NULL || parse_int(tok, &len) != 0 || len == 0 || len > sizeof(data)) {
        usb_send_str("Usage: flash read <addr> <len>\r\n");
        usb_send_str("  len must be 1..256\r\n");
        return;
    }

    flash_fs_error_t err = flash_fs_raw_read_bytes(addr, data, len);
    if (err != FLASH_FS_OK) {
        usb_debug_printf("ERR: flash read failed (%d)\r\n", (int)err);
        return;
    }

    usb_debug_printf("Flash read 0x%06lX (%lu bytes):\r\n", addr, len);
    for (uint32_t i = 0; i < len; i++) {
        if ((i % 16) == 0) {
            usb_debug_printf("0x%06lX:", addr + i);
        }
        usb_debug_printf(" %02X", data[i]);
        if ((i % 16) == 15 || i == (len - 1)) {
            usb_send_str("\r\n");
        }
    }
}

static void cmd_flash_dump(const char *args)
{
    char buf[64];
    char *saveptr = NULL;
    char *tok;
    uint32_t addr;
    uint32_t len;
    uint8_t chunk[256];

    if (strlen(args) >= sizeof(buf)) {
        usb_send_str("Usage: flash dump <addr> <len>\r\n");
        return;
    }

    strcpy(buf, args);
    tok = strtok_r(buf, " \t", &saveptr);
    if (tok == NULL || parse_int(tok, &addr) != 0) {
        usb_send_str("Usage: flash dump <addr> <len>\r\n");
        return;
    }

    tok = strtok_r(NULL, " \t", &saveptr);
    if (tok == NULL || parse_int(tok, &len) != 0 || len == 0 || len > 4096) {
        usb_send_str("Usage: flash dump <addr> <len>\r\n");
        usb_send_str("  len must be 1..4096\r\n");
        return;
    }

    usb_debug_printf("FLASHDUMP %lu\r\n", len);

    while (len > 0) {
        uint32_t this_len = (len > sizeof(chunk)) ? sizeof(chunk) : len;
        flash_fs_error_t err = flash_fs_raw_read_bytes(addr, chunk, this_len);
        if (err != FLASH_FS_OK) {
            usb_debug_printf("\r\nERR: flash dump failed (%d)\r\n", (int)err);
            return;
        }

        usb_send_bytes(chunk, (uint16_t)this_len);
        addr += this_len;
        len -= this_len;
    }
}

/*
 * Factory-cal self-protection (src/util/cal_backup.c).
 *   cal status              report the MCU cal page and the W25Q backup
 *   cal backup              copy the MCU cal page to the W25Q (safe: W25Q only)
 *   cal restore [force] CONFIRM   write the W25Q backup back to MCU flash
 * Restore refuses to overwrite a programmed (precious) page unless `force`.
 */
/* ── CAN bus analyser (CH1 = CAN_H, CH2 = CAN_L) ───────────────────────────
 *   can decode              decode the current record, frame by frame
 *   can stats [n]           n fresh records (default 20): per-ID counts, ACK/NAK/CRC
 *   can screen [on|off]     readout on badge row 2, refreshed 5x per second
 * Best timebase for decoding a 500 kbit/s bus: 0x0A (whole frame + ACK). */
static const char *can_flags_str(uint8_t f)
{
    if (f & DECODE_FLAG_ERROR) return "ERR";
    if (f & DECODE_FLAG_TRUNC) return "TRUNC";
    if (f & DECODE_FLAG_CRC)   return "CRC-FAIL";
    if (f & DECODE_FLAG_ACK)   return "ACK";
    if (f & DECODE_FLAG_NAK)   return "NAK";
    return "?";
}

static void can_print_report(const can_bus_report_t *r, uint32_t gen)
{
    unsigned spb100 = (unsigned)(r->spb * 100.0f + 0.5f);
    usb_debug_printf("record gen=%lu tb=0x%02X fs=%lu S/s  rot=%ld  spb=%u.%02u  bit rate=%lu  "
                     "recessive=%d dominant=%d (CH1-CH2 counts)\r\n",
                     (unsigned long)gen, (unsigned)fpga_acq_rate_idx_get(),
                     (unsigned long)(can_scope_sample_rate(fpga_acq_rate_idx_get()) + 0.5f),
                     (long)r->rotation, spb100 / 100u, spb100 % 100u,
                     (unsigned long)(r->bit_rate + 0.5f), (int)r->level_rec, (int)r->level_dom);
}

static void cmd_can_decode(void)
{
    can_bus_report_t rep;
    const decode_result_t *res = NULL;
    uint32_t gen = 0;
    if (!can_scope_capture(&rep, &res, &gen)) {
        usb_send_str("can decode: no capture or no memory\r\n");
        return;
    }
    if (!rep.signal) {
        usb_send_str("can decode: no CAN signal in this record (flat or no two levels)\r\n");
        return;
    }
    can_print_report(&rep, gen);
    for (uint16_t i = 0; i < res->num_frames; i++) {
        const decode_frame_t *f = &res->frames[i];
        usb_debug_printf("  #%u @%lu..%lu  ID %s%0*lX  DLC %u ", (unsigned)i,
                         (unsigned long)f->start_sample, (unsigned long)f->end_sample,
                         (f->flags & DECODE_FLAG_EXT) ? "x" : "",
                         (f->flags & DECODE_FLAG_EXT) ? 8 : 3, (unsigned long)f->id,
                         (unsigned)f->data_len);
        for (uint8_t k = 0; k < f->data_len; k++) usb_debug_printf("%02X ", f->data[k]);
        usb_debug_printf("%s%s\r\n", (f->flags & DECODE_FLAG_RTR) ? "RTR " : "",
                         can_flags_str(f->flags));
    }
    if (res->num_frames == 0) usb_send_str("  no frame start in this record\r\n");
}

static void cmd_can_stats(const char *args)
{
    uint32_t n = 20;
    while (args && *args == ' ') args++;
    if (args && *args && (parse_int(args, &n) != 0 || n == 0 || n > 1000)) {
        usb_send_str("Usage: can stats [records 1-1000]\r\n");
        return;
    }
    can_bus_stats_t *st = can_scope_stats();
    if (!st) { usb_send_str("can stats: no memory\r\n"); return; }
    can_bus_stats_reset(st);
    can_bus_report_t rep, last;
    memset(&last, 0, sizeof(last));
    uint32_t last_gen = 0xFFFFFFFFu, got = 0;
    TickType_t t0 = xTaskGetTickCount();
    while (got < n && (xTaskGetTickCount() - t0) < pdMS_TO_TICKS(1000u * n + 2000u)) {
        const decode_result_t *res = NULL;
        uint32_t gen = 0;
        if (can_scope_capture(&rep, &res, &gen) && gen != last_gen) {
            can_bus_stats_add(st, &rep, res);
            last_gen = gen;
            if (rep.signal) last = rep;
            got++;
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    char line[64];
    can_bus_summary(st, &last, line, sizeof(line));
    usb_debug_printf("%s\r\n", line);
    usb_debug_printf("records %lu (idle %lu)  frames %lu  ACK %lu  NAK %lu  CRC-fail %lu  ERR %lu  "
                     "cut-off %lu\r\n",
                     (unsigned long)st->records, (unsigned long)st->idle, (unsigned long)st->frames,
                     (unsigned long)st->ack, (unsigned long)st->nak, (unsigned long)st->crc,
                     (unsigned long)st->err, (unsigned long)st->trunc);
    for (uint8_t k = 0; k < st->nids; k++) {
        const can_bus_id_t *e = &st->ids[k];
        usb_debug_printf("  ID %s%0*lX  seen %lu  ACK %lu  NAK %lu  last:", e->ext ? "x" : "",
                         e->ext ? 8 : 3, (unsigned long)e->id, (unsigned long)e->seen,
                         (unsigned long)e->ack, (unsigned long)e->nak);
        for (uint8_t b = 0; b < e->dlc; b++) usb_debug_printf(" %02X", e->data[b]);
        usb_send_str("\r\n");
    }
    if (st->ids_dropped)
        usb_debug_printf("  (%lu frames with IDs beyond the %u-entry table)\r\n",
                         (unsigned long)st->ids_dropped, (unsigned)CAN_BUS_MAX_IDS);
    if (last.signal) can_print_report(&last, last_gen);
}

static void cmd_can_screen(const char *args)
{
    while (args && *args == ' ') args++;
    if (args && *args) {
        if      (strncmp(args, "on",  2) == 0) can_scope_screen_set(true);
        else if (strncmp(args, "off", 3) == 0) can_scope_screen_set(false);
        else { usb_send_str("Usage: can screen [on|off]\r\n"); return; }
    }
    usb_debug_printf("can screen %s\r\n", can_scope_screen_on() ? "ON (badge row 2)" : "OFF");
}

static void cmd_cal_status(void)
{
    cal_backup_report_t r;
    if (cal_backup_status(&r) != CAL_BK_OK) {
        usb_send_str("cal status: read failed\r\n");
        return;
    }
    usb_debug_printf("live  @0x%08lX  %-14s crc=0x%08lX\r\n",
                     (unsigned long)CAL_BACKUP_SRC_ADDR,
                     cal_page_class_str(r.live_class),
                     (unsigned long)r.live_crc);
    if (!r.backup_present) {
        usb_send_str("backup: region unreadable\r\n");
    } else if (r.backup_status != CAL_REC_OK) {
        usb_debug_printf("backup: %s (no valid record)\r\n",
                         cal_rec_status_str(r.backup_status));
    } else {
        usb_debug_printf("backup: valid v%u  payload_crc=0x%08lX  src=0x%08lX\r\n",
                         (unsigned)r.backup_version,
                         (unsigned long)r.backup_payload_crc,
                         (unsigned long)r.backup_src_addr);
        usb_debug_printf("match : %s\r\n", r.match ? "yes (live == backup)"
                                                   : "NO (live differs from backup)");
    }
}

static void cmd_cal_backup(void)
{
    cal_bk_status_t st = cal_backup_store();
    usb_debug_printf("cal backup: %s\r\n", cal_bk_status_str(st));
    if (st == CAL_BK_ERR_LIVE_BLANK) {
        usb_send_str("  (MCU page is blank/zeroed — nothing to preserve, existing backup kept)\r\n");
    }
}

static void cmd_cal_restore(const char *args)
{
    char abuf[40];
    char *saveptr = NULL;
    bool force = false;
    const char *confirm = NULL;

    if (strlen(args) >= sizeof(abuf)) {
        usb_send_str("Usage: cal restore [force] CONFIRM\r\n"); return;
    }
    strcpy(abuf, args);
    char *t1 = strtok_r(abuf, " \t", &saveptr);
    char *t2 = strtok_r(NULL, " \t", &saveptr);
    if (t1 != NULL && strcmp(t1, "force") == 0) {
        force = true;
        confirm = t2;
    } else {
        confirm = t1;
    }
    if (confirm == NULL || strcmp(confirm, "CONFIRM") != 0) {
        usb_send_str("Refused: append CONFIRM. This WRITES MCU flash 0x08006000.\r\n"
                     "  cal restore CONFIRM        (refuses over a programmed page)\r\n"
                     "  cal restore force CONFIRM  (overwrites a programmed page)\r\n");
        return;
    }
    cal_bk_status_t st = cal_backup_restore(force);
    usb_debug_printf("cal restore%s: %s\r\n", force ? " force" : "", cal_bk_status_str(st));
    if (st == CAL_BK_ERR_LIVE_PRECIOUS) {
        usb_send_str("  live page holds data — use 'cal restore force CONFIRM' only if you are sure\r\n");
    }
}

static void cmd_screen_dump(const char *args)
{
    char buf[64];
    char *saveptr = NULL;
    char *tok;
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t w = LCD_WIDTH;
    uint32_t h = LCD_HEIGHT;

    if (args && *args) {
        if (strlen(args) >= sizeof(buf)) {
            usb_send_str("Usage: screen dump [shadow] [x y w h]\r\n");
            return;
        }

        strcpy(buf, args);
        tok = strtok_r(buf, " \t", &saveptr);
        if (tok && strcmp(tok, "shadow") == 0) {
            tok = strtok_r(NULL, " \t", &saveptr);
            if (tok == NULL) goto parsed_screen_args;
        }
        if (tok == NULL || parse_int(tok, &x) != 0) {
            usb_send_str("Usage: screen dump [shadow] [x y w h]\r\n");
            return;
        }
        tok = strtok_r(NULL, " \t", &saveptr);
        if (tok == NULL || parse_int(tok, &y) != 0) {
            usb_send_str("Usage: screen dump [shadow] [x y w h]\r\n");
            return;
        }
        tok = strtok_r(NULL, " \t", &saveptr);
        if (tok == NULL || parse_int(tok, &w) != 0) {
            usb_send_str("Usage: screen dump [shadow] [x y w h]\r\n");
            return;
        }
        tok = strtok_r(NULL, " \t", &saveptr);
        if (tok == NULL || parse_int(tok, &h) != 0) {
            usb_send_str("Usage: screen dump [shadow] [x y w h]\r\n");
            return;
        }
        if (strtok_r(NULL, " \t", &saveptr) != NULL) {
            usb_send_str("Usage: screen dump [shadow] [x y w h]\r\n");
            return;
        }
    }

parsed_screen_args:
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT ||
        w == 0 || h == 0 ||
        w > LCD_WIDTH || h > LCD_HEIGHT ||
        x > LCD_WIDTH - w || y > LCD_HEIGHT - h) {
        usb_send_str("Usage: screen dump [shadow] [x y w h]\r\n");
        usb_debug_printf("  bounds: x<%u y<%u x+w<=%u y+h<=%u\r\n",
                         (unsigned)LCD_WIDTH,
                         (unsigned)LCD_HEIGHT,
                         (unsigned)LCD_WIDTH,
                         (unsigned)LCD_HEIGHT);
        return;
    }

    usb_debug_printf("SCREENDUMP x=%lu y=%lu w=%lu h=%lu format=%s\r\n",
                     x, y, w, h, "indexed4");

    const uint8_t *bits = lcd_shadow_bits();
    static const char hexdigits[] = "0123456789ABCDEF";
    uint16_t page_y = lcd_shadow_page_y();
    for (uint32_t row = 0; row < h; row++) {
        usb_debug_printf("ROW %lu ", row);
        for (uint32_t col = 0; col < w; col++) {
            uint32_t sx = x + col;
            uint32_t sy = y + row;
            uint8_t idx = 0;
            if (sy >= page_y && sy < (uint32_t)page_y + LCD_SHADOW_HEIGHT) {
                uint32_t shadow_y = sy - page_y;
                uint8_t byte = bits[shadow_y * LCD_SHADOW_STRIDE + (sx >> 1)];
                idx = (sx & 1U) ? (byte & 0x0FU) : (byte >> 4);
            }
            char c = hexdigits[idx & 0x0F];
            usb_send_bytes((const uint8_t *)&c, 1);
        }
        usb_send_str("\r\n");
    }
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    crc = ~crc;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

/* For the feature shell files (shell_internal.h); crc32_update() unchanged. */
uint32_t usb_debug_sh_crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    return crc32_update(crc, data, len);
}

static bool parse_screen_region_args(const char *args,
                                     uint32_t *x,
                                     uint32_t *y,
                                     uint32_t *w,
                                     uint32_t *h)
{
    char buf[64];
    char *saveptr = NULL;
    char *tok;

    *x = 0;
    *y = 0;
    *w = LCD_WIDTH;
    *h = LCD_HEIGHT;

    if (args && *args) {
        if (strlen(args) >= sizeof(buf)) return false;
        strcpy(buf, args);

        tok = strtok_r(buf, " \t", &saveptr);
        if (tok == NULL || parse_int(tok, x) != 0) return false;
        tok = strtok_r(NULL, " \t", &saveptr);
        if (tok == NULL || parse_int(tok, y) != 0) return false;
        tok = strtok_r(NULL, " \t", &saveptr);
        if (tok == NULL || parse_int(tok, w) != 0) return false;
        tok = strtok_r(NULL, " \t", &saveptr);
        if (tok == NULL || parse_int(tok, h) != 0) return false;
        if (strtok_r(NULL, " \t", &saveptr) != NULL) return false;
    }

    return *x < LCD_WIDTH && *y < LCD_HEIGHT &&
           *w > 0 && *h > 0 &&
           *w <= LCD_WIDTH && *h <= LCD_HEIGHT &&
           *x <= LCD_WIDTH - *w && *y <= LCD_HEIGHT - *h;
}

static void cmd_screen_dumpbin(const char *args)
{
    uint32_t x, y, w, h;
    uint8_t out_row[LCD_SHADOW_STRIDE];

    if (!parse_screen_region_args(args, &x, &y, &w, &h)) {
        usb_send_str("Usage: screen dumpbin [x y w h]\r\n");
        usb_debug_printf("  bounds: x<%u y<%u x+w<=%u y+h<=%u\r\n",
                         (unsigned)LCD_WIDTH,
                         (unsigned)LCD_HEIGHT,
                         (unsigned)LCD_WIDTH,
                         (unsigned)LCD_HEIGHT);
        return;
    }

    uint32_t row_len = (w + 1U) / 2U;
    uint32_t len = row_len * h;
    uint32_t crc = 0;
    const uint8_t *bits = lcd_shadow_bits();

    for (uint32_t row = 0; row < h; row++) {
        uint32_t sy = y + row;
        memset(out_row, 0, row_len);
        for (uint32_t col = 0; col < w; col++) {
            uint32_t sx = x + col;
            uint8_t src = bits[sy * LCD_SHADOW_STRIDE + (sx >> 1)];
            uint8_t idx = (sx & 1U) ? (src & 0x0FU) : (src >> 4);
            if ((col & 1U) == 0) {
                out_row[col >> 1] = (uint8_t)(idx << 4);
            } else {
                out_row[col >> 1] |= idx;
            }
        }
        crc = crc32_update(crc, out_row, row_len);
    }

    usb_debug_printf("SCREENBIN x=%lu y=%lu w=%lu h=%lu format=indexed4 len=%lu crc32=%08lX\r\n",
                     x, y, w, h, len, crc);

    for (uint32_t row = 0; row < h; row++) {
        uint32_t sy = y + row;
        memset(out_row, 0, row_len);
        for (uint32_t col = 0; col < w; col++) {
            uint32_t sx = x + col;
            uint8_t src = bits[sy * LCD_SHADOW_STRIDE + (sx >> 1)];
            uint8_t idx = (sx & 1U) ? (src & 0x0FU) : (src >> 4);
            if ((col & 1U) == 0) {
                out_row[col >> 1] = (uint8_t)(idx << 4);
            } else {
                out_row[col >> 1] |= idx;
            }
        }
        usb_send_bytes(out_row, (uint16_t)row_len);
    }

    usb_send_str("\r\nSCREENBIN END\r\n");
}

/* ── screenshot — the on-device gallery over the shell (parity C16) ─────
 * Same store as SAVE / long SAVE (gallery_store.h: 9 slots in our W25Q
 * scratch region, never stock's FAT). `get` frames a raw BMP the way
 * `screen dumpbin` frames its bytes: a header line with len + the CRC32 the
 * save recorded, the raw file, then an END line with the CRC recomputed from
 * what was actually sent, so a host can check both. */
static const char *gallery_status_str(gallery_status_t st)
{
    switch (st) {
    case GALLERY_OK:            return "ok";
    case GALLERY_ERR_NOT_READY: return "flash region layer not ready";
    case GALLERY_ERR_FULL:      return "gallery full (delete one)";
    case GALLERY_ERR_IO:        return "flash io error or busy";
    case GALLERY_ERR_NOT_FOUND: return "no such screenshot";
    case GALLERY_ERR_BAD_ARG:   return "bad argument";
    }
    return "?";
}

static void cmd_screenshot(const char *args)
{
    static const char usage[] =
        "Usage: screenshot list | save | get <n> | delete <n> | check <n>\r\n";
    char name[16];
    uint32_t seq = 0, slot = 0;
    gallery_status_t st;

    if (args == NULL || *args == '\0' || strcmp(args, "list") == 0) {
        gallery_index_t idx;
        st = gallery_scan(&idx);
        if (st != GALLERY_OK) { usb_debug_printf("screenshot: %s\r\n", gallery_status_str(st)); return; }
        const flash_region_t *r = flash_region_get(GALLERY_REGION);
        usb_debug_printf("SHOTS count=%u slots=%u next=%lu region=%s 0x%06lX+0x%lX slot=0x%lX\r\n",
                         (unsigned)idx.count, (unsigned)idx.slots_total, (unsigned long)idx.next_seq,
                         r->name, (unsigned long)r->start, (unsigned long)r->length,
                         (unsigned long)GALLERY_SLOT_SIZE);
        for (uint32_t i = 0; i < idx.count; i++) {
            gallery_slot_info_t info;
            (void)gallery_slot_info(idx.slot[i], &info);
            gallery_name(idx.seq[i], name, sizeof name);
            usb_debug_printf("  %-14s slot %u @0x%06lX len=%lu crc32=%08lX\r\n", name,
                             (unsigned)idx.slot[i],
                             (unsigned long)(r->start + idx.slot[i] * GALLERY_SLOT_SIZE + GALLERY_BMP_OFFSET),
                             (unsigned long)info.bmp_len, (unsigned long)info.bmp_crc);
        }
        return;
    }
    if (strcmp(args, "save") == 0) {
        st = gallery_ui_capture(&seq);
        if (st == GALLERY_OK) {
            gallery_name(seq, name, sizeof name);
            usb_debug_printf("screenshot: saved %s\r\n", name);
        } else {
            usb_debug_printf("screenshot: save failed: %s\r\n", gallery_status_str(st));
        }
        return;
    }

    const bool is_get = strncmp(args, "get ", 4) == 0;
    const bool is_del = strncmp(args, "delete ", 7) == 0;
    const bool is_chk = strncmp(args, "check ", 6) == 0;
    if (!is_get && !is_del && !is_chk) { usb_send_str(usage); return; }
    const char *arg = args + (is_get ? 4 : is_del ? 7 : 6);
    while (*arg == ' ') arg++;
    if (!gallery_parse_name(arg, &seq)) { usb_send_str(usage); return; }
    gallery_name(seq, name, sizeof name);

    if (is_del) {
        st = gallery_delete(seq);
        usb_debug_printf("screenshot: delete %s: %s\r\n", name, gallery_status_str(st));
        return;
    }
    st = gallery_find(seq, &slot);
    if (st != GALLERY_OK) { usb_debug_printf("screenshot: %s: %s\r\n", name, gallery_status_str(st)); return; }
    gallery_slot_info_t info;
    (void)gallery_slot_info(slot, &info);
    if (is_chk) {
        uint32_t crc = 0;
        st = gallery_crc_bmp(slot, &crc);
        usb_debug_printf("screenshot: %s stored=%08lX read=%08lX %s\r\n", name,
                         (unsigned long)info.bmp_crc, (unsigned long)crc,
                         (st == GALLERY_OK && crc == info.bmp_crc) ? "OK" : "MISMATCH");
        return;
    }
    usb_debug_printf("SHOTBIN name=%s len=%lu crc32=%08lX\r\n", name,
                     (unsigned long)info.bmp_len, (unsigned long)info.bmp_crc);
    uint8_t buf[64];
    uint32_t crc = CAL_BACKUP_CRC32_INIT;
    for (uint32_t off = 0; off < GALLERY_BMP_LEN; off += sizeof buf) {
        uint32_t n = GALLERY_BMP_LEN - off;
        if (n > sizeof buf) n = sizeof buf;
        if (gallery_read_bmp(slot, off, buf, n) != GALLERY_OK) memset(buf, 0, n);
        crc = cal_backup_crc32_update(crc, buf, n);
        usb_send_bytes(buf, (uint16_t)n);
    }
    usb_debug_printf("\r\nSHOTBIN END crc32=%08lX\r\n", (unsigned long)cal_backup_crc32_final(crc));
}

/*
 * Boot logo (logo_store.h): upload a 320x240 RGB565 image over the shell.
 *   logo put <len> <crc32hex>   erase the slot, start an upload (len = 153600)
 *   logo data <off> <hex>       <= 48 bytes at the next expected offset
 *   logo end                    verify CRC from flash, commit
 * scripts/logo_put.py drives this; every reply starts "logo:" and a failure
 * says "err". The upload session is 12 bytes of static RAM.
 */
static logo_upload_t s_logo_up;

/* Raw block receive for `logo blk` (raw_block.h): armed by the command,
 * fed by the task loop (shell_rx) before the line editor sees anything. The
 * block lands in the shell bus scratch, like fw_loader's: same task, never
 * nested with the spi3/skew commands that also borrow it. */
static raw_block_t s_raw;
static uint8_t shell_bus_scratch[2 * FPGA_ADC_BUF_SIZE] __attribute__((aligned(4)));   /* defined below */
#define LOGO_BLK_MAX 1024u

static uint32_t shell_now_ms(void) { return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); }

/* A raw block finished (complete or timed out): write it, answer. */
static void logo_blk_finish(void)
{
    const raw_block_state_t st = s_raw.state;
    const uint32_t off = s_raw.tag, n = s_raw.want;
    raw_block_reset(&s_raw);
    if (st == RAW_TIMEOUT) {
        usb_debug_printf("logo: err timeout (block @%lu: %lu of %lu bytes; expect %lu)\r\n> ",
                         (unsigned long)off, (unsigned long)s_raw.got, (unsigned long)n,
                         (unsigned long)s_logo_up.received);
        return;
    }
    if (st == RAW_DONE_CRC) {
        usb_debug_printf("logo: err crc (block @%lu; expect %lu)\r\n> ",
                         (unsigned long)off, (unsigned long)s_logo_up.received);
        return;
    }
    logo_status_t ls = LOGO_OK;
    for (uint32_t k = 0; k < n && ls == LOGO_OK; k += LOGO_CHUNK_MAX) {
        const uint32_t m = (n - k) < LOGO_CHUNK_MAX ? (n - k) : LOGO_CHUNK_MAX;
        ls = logo_put_data(&s_logo_up, off + k, shell_bus_scratch + k, m);
    }
    if (ls == LOGO_OK) usb_debug_printf("logo: ok %lu\r\n> ", (unsigned long)s_logo_up.received);
    else usb_debug_printf("logo: err %s (expect %lu)\r\n> ", logo_status_str(ls),
                          (unsigned long)s_logo_up.received);
}

static void logo_tick(void)
{
    health_checkin_self();
    vTaskDelay(1);
}

static int logo_hexbyte(const char *p)
{
    int v = 0;
    for (int i = 0; i < 2; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

static void cmd_logo(const char *args)
{
    static const char usage[] =
        "Usage: logo info | put <len> <crc32hex> | blk <off> <len> <crc32hex> | data <off> <hex> | pos | end | show | clear\r\n";
    if (args == NULL || *args == '\0' || strcmp(args, "info") == 0) {
        logo_info_t info;
        logo_status_t st = logo_info(&info);
        if (st != LOGO_OK) { usb_debug_printf("logo: err %s\r\n", logo_status_str(st)); return; }
        const flash_region_t *r = flash_region_get(LOGO_REGION);
        if (info.state == LOGO_SLOT_VALID) {
            uint32_t crc = 0;
            st = logo_crc_payload(&crc, logo_tick);
            usb_debug_printf("logo: valid %lux%lu rgb565 %lu B crc %08lX payload %s @0x%06lX\r\n",
                             (unsigned long)LOGO_W, (unsigned long)LOGO_H, (unsigned long)info.len,
                             (unsigned long)info.crc,
                             (st == LOGO_OK && crc == info.crc) ? "OK" : "MISMATCH",
                             (unsigned long)(r ? r->start : 0));
        } else {
            usb_debug_printf("logo: %s (built-in splash)\r\n",
                             info.state == LOGO_SLOT_EMPTY ? "none stored" : "damaged slot");
        }
        return;
    }
    if (strncmp(args, "put ", 4) == 0) {
        uint32_t len, crc;
        const char *p = args + 4;
        char *e;
        len = strtoul(p, &e, 10);
        if (e == p || *e != ' ' || parse_hex32(e + 1, &crc) != 0) { usb_send_str(usage); return; }
        logo_status_t st = logo_put_begin(&s_logo_up, len, crc, logo_tick);
        if (st == LOGO_OK) usb_debug_printf("logo: ok put %lu\r\n", (unsigned long)len);
        else usb_debug_printf("logo: err %s\r\n", logo_status_str(st));
        return;
    }
    if (strncmp(args, "data ", 5) == 0) {
        uint32_t off;
        const char *p = args + 5;
        char *e;
        off = strtoul(p, &e, 10);
        if (e == p || *e != ' ') { usb_send_str(usage); return; }
        p = e + 1;
        uint8_t buf[LOGO_CHUNK_MAX];
        uint32_t n = 0;
        while (p[0] != '\0' && p[1] != '\0') {
            int b = logo_hexbyte(p);
            if (b < 0 || n >= sizeof buf) { usb_send_str("logo: err bad hex\r\n"); return; }
            buf[n++] = (uint8_t)b;
            p += 2;
        }
        if (n == 0u || *p != '\0') { usb_send_str("logo: err bad hex\r\n"); return; }
        logo_status_t st = logo_put_data(&s_logo_up, off, buf, n);
        if (st == LOGO_OK) usb_debug_printf("logo: ok %lu\r\n", (unsigned long)s_logo_up.received);
        else usb_debug_printf("logo: err %s (expect %lu)\r\n", logo_status_str(st),
                              (unsigned long)s_logo_up.received);
        return;
    }
    if (strncmp(args, "blk ", 4) == 0) {
        /* logo blk <off> <len> <crc32hex>: the next <len> raw bytes on this
         * transport are pixel data for <off>; answered "logo: ready", then
         * "logo: ok <received>" once they are in, CRC-checked and written. */
        uint32_t off, n, crc;
        const char *p = args + 4;
        char *e;
        off = strtoul(p, &e, 10);
        if (e == p || *e != ' ') { usb_send_str(usage); return; }
        p = e + 1;
        n = strtoul(p, &e, 10);
        if (e == p || *e != ' ' || parse_hex32(e + 1, &crc) != 0) { usb_send_str(usage); return; }
        if (!s_logo_up.active) { usb_send_str("logo: err no upload in progress (logo put first)\r\n"); return; }
        if (off != s_logo_up.received) {
            usb_debug_printf("logo: err sequence (expect %lu)\r\n", (unsigned long)s_logo_up.received);
            return;
        }
        if (n == 0u || n > LOGO_BLK_MAX || n > LOGO_PIXEL_BYTES - off) {
            usb_debug_printf("logo: err bad length (1..%u)\r\n", (unsigned)LOGO_BLK_MAX);
            return;
        }
        (void)raw_block_begin(&s_raw, shell_bus_scratch, LOGO_BLK_MAX, n, crc, off, shell_now_ms());
        usb_send_str("logo: ready\r\n");
        return;
    }
    if (strcmp(args, "pos") == 0) {
        usb_debug_printf("logo: pos %lu %s\r\n", (unsigned long)s_logo_up.received,
                         s_logo_up.active ? "active" : "idle");
        return;
    }
    if (strcmp(args, "end") == 0) {
        logo_status_t st = logo_put_end(&s_logo_up, logo_tick);
        usb_debug_printf(st == LOGO_OK ? "logo: ok committed\r\n" : "logo: err %s\r\n",
                         logo_status_str(st));
        return;
    }
    if (strcmp(args, "clear") == 0) {
        logo_status_t st = logo_clear();
        usb_debug_printf("logo: %s\r\n", st == LOGO_OK ? "ok cleared" : logo_status_str(st));
        return;
    }
    if (strncmp(args, "show", 4) == 0) {
        uint32_t ms = 2000u;
        if (args[4] == ' ') parse_int(args + 5, &ms);
        if (ms > 10000u) ms = 10000u;
        logo_status_t st = logo_ui_preview(ms);
        usb_debug_printf("logo: %s\r\n", st == LOGO_OK ? "ok shown" : logo_status_str(st));
        return;
    }
    usb_send_str(usage);
}

/* ── usb — USB Sharing (parity C16): the read-only gallery drive ───────
 * The unit boots as the plain CDC device. `usb share on` (or Settings "USB
 * Sharing") re-enumerates it as CDC + MSC: this port closes, and comes back
 * about a second later with the drive. If the host has not configured the
 * composite device within 3 s, the unit returns as CDC-only on its own and
 * `usb` reports FALLBACK; `usb trace` then shows the host's requests. */
static void cmd_usb_cdc_counters(void)
{
    usb_debug_printf("CDC: conf=%d tx_heals=%lu rx_heals=%lu tx_timeouts=%lu tx_drops=%lu%s\r\n",
                     usb_debug_connected() ? 1 : 0,
                     (unsigned long)s_cdc.tx_heals, (unsigned long)s_cdc.rx_heals,
                     (unsigned long)s_cdc.tx_timeouts, (unsigned long)s_cdc.tx_drops,
                     s_cdc.host_absent ? " (host not reading)" : "");
}

#if USB_MSC
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

static void cmd_usb(const char *args)
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

static void cmd_screen_shadow(const char *args)
{
    if (args == NULL || *args == '\0') {
        usb_debug_printf("shadow_page_y=%u height=%u\r\n",
                         (unsigned)lcd_shadow_page_y(),
                         (unsigned)LCD_SHADOW_HEIGHT);
        return;
    }

    if (strncmp(args, "page", 4) == 0 &&
        (args[4] == '\0' || args[4] == ' ' || args[4] == '\t')) {
        const char *value = args + 4;
        uint32_t y = 0;
        while (*value == ' ' || *value == '\t') value++;
        if (*value != '\0' && parse_int(value, &y) != 0) {
            usb_send_str("Usage: screen shadow page [y]\r\n");
            return;
        }
        lcd_shadow_set_page((uint16_t)y);
        usb_debug_printf("shadow_page_y=%u height=%u\r\n",
                         (unsigned)lcd_shadow_page_y(),
                         (unsigned)LCD_SHADOW_HEIGHT);
        return;
    }

    usb_send_str("Usage: screen shadow page [y]\r\n");
}


/* ── Stock-parity meter shell (M1/M4/M5) ────────────────────────────────
 *
 * (the function is the property meter.func, degC/degF is meter.degf)
 * `meter state`           everything decoded from the last frame, the switch
 *                         counters, the beep source and the guard
 * `meter fast [on|off]`   one-word switching (default on); off = old PC11 path
 * `meter keepalive [..]`  the 4 Hz unobeyed poll (default on)
 * `meter beep ...`        the SoC-edge buzzer (default off)
 */
static const char *onoff(bool b) { return b ? "on" : "off"; }

/* For the feature shell files (shell_internal.h); onoff() unchanged. */
const char *usb_debug_sh_onoff(bool b)
{
    return onoff(b);
}

static bool shell_onoff_arg(const char *args, bool *out)
{
    while (*args == ' ') args++;
    if (strncmp(args, "on", 2) == 0)  { *out = true;  return true; }
    if (strncmp(args, "off", 3) == 0) { *out = false; return true; }
    return false;
}

/* For the feature shell files (shell_internal.h); shell_onoff_arg() unchanged. */
bool usb_debug_sh_onoff_arg(const char *args, bool *out)
{
    return shell_onoff_arg(args, out);
}

static void cmd_meter_relays(const char *args)
{
    bool on;
    if (shell_onoff_arg(args, &on)) fpga_meter_relays_set(on);
    else if (*args) { usb_send_str("usage: meter relays [on|off]\r\n"); return; }
    usb_debug_printf("meter relays: %s (on = meter entry moves the scope's range relays, the old"
                     " behaviour, and the meter link follows the screen only; applies at the next"
                     " meter entry or function change)\r\n",
                     onoff(fpga_meter_relays_get()));
}

static void cmd_meter_state(void)
{
    meter_reading_t r;
    uint8_t sel = 0, f36 = 0;
    bool m_on, active;
    int8_t port;
    uint32_t edges, pulses, gated, tick;
    char hex[12 * 3 + 1];

    memset(&r, 0, sizeof r);
    if (!meter_data_snapshot(&r)) {
        usb_send_str("meter state: no snapshot\r\n");
        return;
    }
    (void)meter_stock_selector_for_submode(meter_submode, &sel, &f36);
    usb_debug_printf("meter state: mode=%lu submode=%u (%s) stock_sel=%u F36=%u word=%04X\r\n",
                     (uint32_t)current_mode, (unsigned)meter_submode,
                     meter_submode_name(meter_submode), (unsigned)sel, (unsigned)f36,
                     (unsigned)fpga_meter_stock_cmd_word_for_submode(meter_submode));
    {
        /* The link policy as the display task last evaluated it (main.c). */
        bool usb_host = false, background = false, want_on = false;
        meter_link_status(&usb_host, &background, &want_on);
        usb_debug_printf("  link: on=%u want_on=%u policy=%s usb_host=%u background=%u soc_live=%u PC11=%u"
                         " fast=%s keepalive=%s fast_switches=%u fallbacks=%u"
                         " wake_ms=%u echo_ok=%u tries=%u sequences=%u data_frames=%u\r\n",
                         fpga_meter_link_on() ? 1U : 0U, want_on ? 1U : 0U,
                         prop_get(P(METER_LINK)) == METER_LINK_ALWAYS ? "always" : "screen",
                         usb_host ? 1U : 0U, background ? 1U : 0U,
                         fpga_meter_soc_live() ? 1U : 0U, (GPIOC->odt & (1U << 11)) ? 1U : 0U,
                         onoff(fpga_meter_fast_switch_get()), onoff(fpga_meter_keepalive_get()),
                         (unsigned)fpga.meter_fast_switch_count, (unsigned)fpga.meter_fast_fallback_count,
                         (unsigned)fpga.meter_soc_wake_ms, (unsigned)fpga.meter_selector_confirmed,
                         (unsigned)fpga.meter_selector_attempts, (unsigned)fpga.meter_mode_sequence_count,
                         (unsigned)fpga.frame_count);
    }
    usb_debug_printf("  reading: valid=%u sub=%u class=%u status=%u(%s) text=\"%s\" unit=\"%s\" value=%d.%03d\r\n",
                     r.valid ? 1U : 0U, (unsigned)r.submode, (unsigned)r.result_class,
                     (unsigned)r.status_code,
                     meter_stock_status_text((meter_stock_status_t)r.status_code),
                     r.display_str, r.unit_suffix ? r.unit_suffix : "",
                     (int)r.value,
                     (int)((r.value < 0 ? -r.value : r.value) * 1000.0f) % 1000);
    usb_debug_printf("  range: index=%u label=%s  f6=%02X f7=%02X f8=%02X  degF=%s"
                     "  auto_fn=%u (0 none,1 DCV,2 ACV,3 Ohm,4 Cont) f2=%02X\r\n",
                     (unsigned)r.range_index, r.range_label,
                     (unsigned)r.dbg_frame[6], (unsigned)r.dbg_frame[7], (unsigned)r.dbg_frame[8],
                     onoff(meter_data_fahrenheit()), (unsigned)r.auto_fn,
                     (unsigned)r.dbg_frame[2]);
    for (int i = 0; i < 12; i++)
        snprintf(hex + i * 3, 4, "%02X ", (unsigned)r.dbg_frame[i]);
    usb_debug_printf("  frame: %s\r\n", hex);
    continuity_buzzer_soc_status(&m_on, &port, &edges, &pulses, &gated, &active, &tick);
    usb_debug_printf("  beep: soc_edge=%s port=%c edges=%lu pulses=%lu gated_out=%lu active=%u tick_ms=%lu gate=%u\r\n",
                     onoff(m_on), port < 0 ? '-' : (char)('A' + port), edges, pulses, gated,
                     active ? 1U : 0U, tick,
                     meter_stock_beep_gate_for_submode(meter_submode) ? 1U : 0U);
    usb_debug_printf("  cal-wizard guard: refused=%u last=%04X (0x02A0 0x0503 0x0501 are never sent obeyed)\r\n",
                     (unsigned)fpga.meter_cal_word_refused, (unsigned)fpga.meter_cal_word_last);
}

/* `fpga cfgsettle [ms|off|default]` -- the capture-configuration rule
 * (dsp/acq_cfg.h): a record is published only after routing, range,
 * position, probe, timebase and trigger have been stable for fill + poll gap
 * + this margin. `off` admits every record (the behaviour before the rule),
 * for an A/B in one boot. */
static void cmd_fpga_cfgsettle(const char *args)
{
    while (*args == ' ') args++;
    if (strcmp(args, "off") == 0) {
        fpga_acq_cfg_settle_set(0xFFFFu);
    } else if (strcmp(args, "default") == 0) {
        fpga_acq_cfg_settle_set(0u);
    } else if (*args) {
        uint32_t ms = 0;
        if (parse_int(args, &ms) != 0 || ms < 1u || ms > 2000u) {
            usb_send_str("usage: fpga cfgsettle [ms 1..2000|off|default]\r\n");
            return;
        }
        fpga_acq_cfg_settle_set((uint16_t)ms);
    }
    const uint16_t c = fpga_acq_cfg_settle_get();
    if (c == 0xFFFFu)
        usb_debug_printf("cfgsettle: off (every record admitted); refused so far %lu\r\n",
                         (unsigned long)fpga_acq_cfg_drops());
    else
        usb_debug_printf("cfgsettle: %u ms%s after fill + poll gap; records refused %lu\r\n",
                         (unsigned)(c ? c : ACQ_CFG_SETTLE_MS), c ? "" : " (default)",
                         (unsigned long)fpga_acq_cfg_drops());
}

/* `fpga edgefilter [on|off]` — the MCU-side trigger edge filter. */
static void cmd_fpga_edgefilter(const char *args)
{
    while (*args == ' ') args++;
    if (*args) {
        if      (strncmp(args, "on",  2) == 0) fpga_acq_edge_filter_set(true);
        else if (strncmp(args, "off", 3) == 0) fpga_acq_edge_filter_set(false);
        else { usb_send_str("usage: fpga edgefilter [on|off]\r\n"); return; }
    }
    const scope_state_t *ss = scope_state_get();
    usb_debug_printf("edge filter %s, edge %s: kept %lu dropped %lu unclassified %lu\r\n",
                     fpga_acq_edge_filter_get() ? "ON" : "OFF",
                     trigger_edge_labels[ss->trigger.edge],
                     (unsigned long)fpga.acq_edge_kept, (unsigned long)fpga.acq_edge_dropped,
                     (unsigned long)fpga.acq_edge_unknown);
}

/* `fpga unrotate [on|off]` — time-order strobed records at their seam (dev plan 2.3). */
static void cmd_fpga_unrotate(const char *args)
{
    /* Seam-based since 2026-09-22 (dev plan 2.3); the EXP-52 offset argument
     * belonged to the retired latency model and is no longer accepted. */
    while (*args == ' ') args++;
    if (*args) {
        if      (strncmp(args, "on",  2) == 0) fpga_acq_unrotate_set(true);
        else if (strncmp(args, "off", 3) == 0) fpga_acq_unrotate_set(false);
        else if (strncmp(args, "check on",  8) == 0) fpga_acq_seam_check_set(true);
        else if (strncmp(args, "check off", 9) == 0) fpga_acq_seam_check_set(false);
        else { usb_send_str("usage: fpga unrotate [on|off|check on|check off]\r\n"); return; }
    }
    usb_debug_printf("acq unrotate %s (seam-based); last strobed record's seam at %ld (-1 = none found)\r\n",
                     fpga_acq_unrotate_get() ? "ON" : "OFF", (long)fpga.acq_last_rot);
    usb_debug_printf("seam from: core header %lu  search %lu  none %lu;  check %s: same %lu  differ %lu  search found none %lu\r\n",
                     (unsigned long)fpga_acq_seam_count(0), (unsigned long)fpga_acq_seam_count(1),
                     (unsigned long)fpga_acq_seam_count(2), fpga_acq_seam_check_get() ? "ON" : "off",
                     (unsigned long)fpga_acq_seam_count(3), (unsigned long)fpga_acq_seam_count(4),
                     (unsigned long)fpga_acq_seam_count(5));
}

/* `fpga core` — which capture core runs, by its own account (op E2, asked once when it was loaded). */
static void cmd_fpga_core(const char *args)
{
    (void)args;
    const core_ident_t id = fpga_core_ident();
    if (!fpga_acq_design_ours()) { usb_send_str("core: the running design is not known to be ours; not asked\r\n"); return; }
    if (!id.valid) {
        usb_debug_printf("core: ours, no identity (older than D5, or protocol %u unknown); reply %02X %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
                         (unsigned)id.proto, fpga.core_ident[0], fpga.core_ident[1], fpga.core_ident[2], fpga.core_ident[3],
                         fpga.core_ident[4], fpga.core_ident[5], fpga.core_ident[6], fpga.core_ident[7], fpga.core_ident[8]);
        return;
    }
    usb_debug_printf("core: tag %02X  protocol %u  record %u samples  caps %02X:%s%s%s%s\r\n",
                     (unsigned)id.tag, (unsigned)id.proto, 1u << id.depth_log2, (unsigned)id.caps,
                     core_ident_has(&id, FPGA_CORE_CAP_SEAM)    ? " seam"     : "",
                     core_ident_has(&id, FPGA_CORE_CAP_EDGE)    ? " edge"     : "",
                     core_ident_has(&id, FPGA_CORE_CAP_REAL50)  ? " real50"   : "",
                     core_ident_has(&id, FPGA_CORE_CAP_LOCKRST) ? " lock-rst" : "");
    if (id.tag != FPGA_CORE_TAG)
        usb_debug_printf("      (this firmware's header was made with tag %02X)\r\n", (unsigned)FPGA_CORE_TAG);
}

/* `fpga interleave cal` (parity C2): converter-B self-calibration. CH2 off,
 * timebase <= 3, a quiet CH1 input. Sweeps the CH1 offset DAC over five codes
 * of this unit's calibration page (base+80, base+400, mid, upper-400, upper —
 * bench B4's set), reads the op04 (A) / op05 (B) means at each, fits
 * B = gain * A + offset (dsp/interleave_cal.c) and applies it in RAM only.
 * Refuses, leaving the correction as it was, on a noisy input, rail-clipped
 * levels, a short span, a gain outside 0.5..2 or a poor fit. The DAC is put
 * back to the position-controlled code whatever happens. Integer printing: the
 * shell's printf has no floats. */
static long x100(float v) { return (long)(v * 100.0f + (v < 0.0f ? -0.5f : 0.5f)); }
/* "%s%ld.%02ld": sign apart, or -0.6 would print as 0.60. */
#define F2(v)  (x100(v) < 0 ? "-" : ""), (long)((x100(v) < 0 ? -x100(v) : x100(v)) / 100), \
               (long)((x100(v) < 0 ? -x100(v) : x100(v)) % 100)

static void cmd_fpga_interleave_cal(bool piecewise)
{
    scope_state_t *ss = scope_state_get();
    if (ss->ch2.enabled || fpga.channel_mask != 1u || !fpga_interleave_active()) {
        usb_send_str("refused: needs CH2 off (mask 1), timebase <= 3 and interleave ON "
                     "(set ch2.enabled=off; set tb.code <= 3)\r\n");
        return;
    }
    uint16_t base, upper, codes[5];
    if (!scope_vpos_ch1_endpoints(&base, &upper) || !il_cal_dac_points(base, upper, codes)) {
        usb_send_str("refused: needs a usable calibration page (cal page) with a 1000+ code span\r\n");
        return;
    }
    static il_cal_point_t pt[5];
    unsigned got = 0;
    /* Park the trigger level above every sweep level for the sweep. At the
     * user's level (0 = mid-screen) the middle level sat on it: noise crossing
     * it got those records triggered and biased, ~8 counts of record-to-record
     * drift on both converters, refused every time (unit #2, 2026-11-11). The
     * UI's level is untouched and written back after the sweep. */
    const bool parked = fpga_apply_trigger_level(100);
    usb_send_str("interleave cal: keep CH1 quiet (shorted or a steady DC); sweeping the CH1 offset DAC\r\n");
    for (unsigned i = 0; i < 5u; i++) {
        if (!scope_vpos_ch1_raw(codes[i])) break;
        vTaskDelay(pdMS_TO_TICKS(250));            /* the offset RC settles */
        fpga_ilcal_begin(2);
        if (!fpga_ilcal_collect(8, 8000, &pt[i])) {
            usb_debug_printf("  DAC %u: no records (acquisition not running?)\r\n", (unsigned)codes[i]);
            break;
        }
        got++;
        usb_debug_printf("  DAC %4u  A %s%ld.%02ld  B %s%ld.%02ld  p2p %u/%u  drift %s%ld.%02ld/%s%ld.%02ld%s\r\n",
                         (unsigned)codes[i], F2(pt[i].a_mean), F2(pt[i].b_mean),
                         (unsigned)pt[i].a_p2p, (unsigned)pt[i].b_p2p,
                         F2(pt[i].a_drift), F2(pt[i].b_drift),
                         pt[i].clipped ? "  CLIPPED (not used)" : "");
    }
    scope_vpos_apply_ch(1u, ss->ch1.vdiv_idx);     /* DAC back to the position code */
    if (parked && !fpga_apply_trigger_level(ss->trigger.level))
        usb_debug_printf("  WARNING: trigger level not restored (reg 0x08); `set trig.level %d`\r\n",
                         ss->trigger.level);
    {   /* apply_ch is silent when it cannot write (no clean bank after a
         * range change): say so rather than leave a sweep code. */
        uint16_t pos_code = 0;
        if (scope_vpos_active() &&
            (!scope_vpos_last(1u, NULL, NULL, &pos_code) || scope_trigger_dac_last() != pos_code))
            usb_debug_printf("  WARNING: CH1 offset DAC not restored (DAC %u, position code %u); "
                             "re-select the CH1 range or position\r\n",
                             (unsigned)scope_trigger_dac_last(), (unsigned)pos_code);
    }
    if (got < 5u) {
        usb_send_str("interleave cal: aborted, correction unchanged\r\n");
        return;
    }
    il_cal_params_t prm;
    il_cal_default_params(&prm);
    il_cal_fit_t fit;
    const il_cal_status_t st = il_cal_fit(pt, 5u, &prm, &fit);
    usb_debug_printf("  fit: B = gain %ld/10000 * A + offset %ld/100   used %u  span %s%ld.%02ld  "
                     "residual max %s%ld.%02ld rms %s%ld.%02ld\r\n",
                     (long)(fit.gain * 10000.0f + 0.5f), x100(fit.offset), (unsigned)fit.used,
                     F2(fit.a_span), F2(fit.resid_max), F2(fit.resid_rms));
    if (st != IL_CAL_OK) {
        usb_debug_printf("interleave cal: REFUSED, %s; correction unchanged\r\n", il_cal_status_str(st));
        return;
    }
    /* The sweep takes seconds: a CH2 long-press or a timebase change in the
     * meantime moves op05 off CH1 (or ends the merge), and the points no
     * longer describe converter B against A. */
    if (ss->ch2.enabled || fpga.channel_mask != 1u || !fpga_interleave_active()) {
        usb_send_str("interleave cal: REFUSED, channel/timebase changed during the sweep; "
                     "correction unchanged\r\n");
        return;
    }
    /* The line, then the knots through the levels (parity ilplus): the line
     * is what gated the fit; the knots are what removes unit #2's ~1 count
     * level-dependent odd-even step. Setting the line clears old knots. */
    /* The line by default. The knots pass exactly through the five DC levels
     * and so keep their ~0.5-count static errors: on unit #2 (2026-11-11,
     * skew on, 10 MHz) the image was -52.0 dBc with the line and -45.2 dBc
     * with the knots. `cal piecewise` still applies them, for comparison. */
    fpga_interleave_corr_set(fit.gain, fit.offset);
    if (piecewise && fit.n_knots >= 2u &&
        fpga_interleave_knots_set(fit.n_knots, fit.knot_a, fit.knot_b))
        usb_debug_printf("interleave cal: applied, piecewise through %u levels (RAM; `cal user save` "
                         "keeps it). Skew: `fpga interleave skew measure` with a tone.\r\n",
                         (unsigned)fit.n_knots);
    else
        usb_send_str("interleave cal: applied, line only (RAM; `cal user save` keeps it). "
                     "Skew: `fpga interleave skew measure` with a tone.\r\n");
}

/* The shell bus scratch (defined with the spi3 commands below) holds the
 * merged record `skew measure` copies: same task, never nested. */
static uint8_t shell_bus_scratch[2 * FPGA_ADC_BUF_SIZE];

/* "0.451234" from ppm-of-a-period deviation: 500000 + dev. */
static void print_skew_line(const char *what, int32_t dev)
{
    const int32_t t = 500000 + dev;
    usb_debug_printf("%s B at 0.%06ld of a converter period (%s%ld ppm from 0.5)\r\n",
                     what, (long)t, dev < 0 ? "-" : "+", (long)(dev < 0 ? -dev : dev));
}

/* `fpga interleave skew measure` (parity ilplus): CH2 off, timebase <= 3,
 * a steady tone of 5..50 MHz on CH1 (the G431's 10 MHz square is the bench
 * source), triggering. Takes 16 fresh merged records, estimates B's delay
 * after A in each (least-squares phase at the tone, dsp/interleave_cal.c) and
 * applies the median. With a skew correction already on, the records read
 * the residual, which is added: a second `measure` should read ~0.500. */
#define SKEW_RECORDS   16u
#define SKEW_MIN_OK     8u
static bool cmd_fpga_interleave_skew_measure(bool with_gain)
{
    scope_state_t *ss = scope_state_get();
    if (ss->ch2.enabled || fpga.channel_mask != 1u || !fpga_interleave_active()) {
        usb_send_str("refused: needs CH2 off (mask 1), timebase <= 3 and interleave ON "
                     "(set ch2.enabled=off; set tb.code <= 3)\r\n");
        return false;
    }
    const volatile uint8_t *c1 = fpga_get_ch1_buf();
    if (!c1) { usb_send_str("FPGA not initialized\r\n"); return false; }
    const int32_t cur = fpga_interleave_skew_get();
    float tau[SKEW_RECORDS];
    float gr[SKEW_RECORDS];             /* B'/A amplitude per usable record */
    unsigned ok = 0;
    uint8_t *rec = shell_bus_scratch;
    usb_send_str("interleave skew: keep a steady 5..50 MHz tone on CH1, triggering\r\n");
    for (unsigned r = 0; r < SKEW_RECORDS; r++) {
        /* One fresh, whole, merged record: an even generation past the last
         * one, unchanged across the copy (the acq task commits under it). */
        const uint32_t want = (fpga_acq_frame_generation() & ~1u) + 2u;
        const TickType_t t0 = xTaskGetTickCount();
        bool got = false;
        while (!got && (xTaskGetTickCount() - t0) < pdMS_TO_TICKS(2000)) {
            const uint32_t g0 = fpga_acq_frame_generation();
            if ((g0 & 1u) || (int32_t)(g0 - want) < 0 || !fpga_acq_record_interleaved()) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            for (uint32_t i = 0; i < FPGA_ADC_BUF_SIZE; i++) rec[i] = c1[i];
            got = (fpga_acq_frame_generation() == g0) && fpga_acq_record_interleaved();
        }
        if (!got) {
            usb_debug_printf("  %2u  no fresh merged record in 2 s (not triggering? STOP?)\r\n", r);
            continue;
        }
        il_skew_est_t e;
        const il_skew_status_t st = il_skew_estimate(rec, &e);
        const long khz = (long)(e.cycles * 125000.0f + 0.5f);
        const float tdev = (e.tau - 0.5f) * 1e6f;
        usb_debug_printf("  %2u  %-8s f %ld.%03ld MHz  amp A %s%ld.%02ld B %s%ld.%02ld  B %ld ppm from 0.5%s%s\r\n",
                         r, st == IL_SKEW_OK ? "ok" : "refused", khz / 1000, khz % 1000,
                         F2(e.amp_a), F2(e.amp_b), (long)(tdev + (tdev < 0.0f ? -0.5f : 0.5f)),
                         st == IL_SKEW_OK ? "" : "  ", st == IL_SKEW_OK ? "" : il_skew_status_str(st));
        if (st == IL_SKEW_OK) {
            gr[ok] = (e.amp_a > 1.0f) ? e.amp_b / e.amp_a : 0.0f;
            tau[ok++] = e.tau;
        }
    }
    if (ok < SKEW_MIN_OK) {
        usb_debug_printf("interleave skew: REFUSED, %u of %u records usable (need %u); skew unchanged\r\n",
                         ok, SKEW_RECORDS, SKEW_MIN_OK);
        return false;
    }
    for (unsigned i = 1; i < ok; i++)                    /* insertion sort, <= 16 */
        for (unsigned j = i; j > 0 && tau[j - 1] > tau[j]; j--) { float t = tau[j]; tau[j] = tau[j - 1]; tau[j - 1] = t; }
    const float med = (ok & 1u) ? tau[ok / 2u] : 0.5f * (tau[ok / 2u - 1u] + tau[ok / 2u]);
    /* The middle half's spread: a tone at 10 MHz with unit #2's noise
     * scatters ~0.005 per record; a spread past 0.02 is not a steady tone. */
    const float iqr = tau[(3u * ok) / 4u] - tau[ok / 4u];
    const int32_t dev = (int32_t)((med - 0.5f) * 1e6f + (med < 0.5f ? -0.5f : 0.5f));
    print_skew_line("  median:", dev);
    usb_debug_printf("  spread (middle half) %ld ppm over %u records; correction was %ld ppm\r\n",
                     (long)(iqr * 1e6f + 0.5f), ok, (long)cur);
    if (iqr > 0.02f) {
        usb_send_str("interleave skew: REFUSED, records disagree (not a steady tone); skew unchanged\r\n");
        return false;
    }
    if (ss->ch2.enabled || fpga.channel_mask != 1u || !fpga_interleave_active() ||
        fpga_interleave_skew_get() != cur) {
        usb_send_str("interleave skew: REFUSED, channel/timebase/skew changed during the measurement; "
                     "skew unchanged\r\n");
        return false;
    }
    if (!fpga_interleave_skew_set(cur + dev)) {
        usb_debug_printf("interleave skew: REFUSED, %ld ppm is beyond +-%ld; skew unchanged\r\n",
                         (long)(cur + dev), (long)IL_SKEW_PPM_MAX);
        return false;
    }
    usb_send_str("interleave skew: applied (RAM; `cal user save` keeps it)\r\n");
    if (!with_gain) return true;

    /* Converter B's gain at the tone. The records are already corrected,
     * B' = (B - off) / g, so the true B/A ratio is g x median(B'/A). On unit
     * #2 (2026-11-11, 10 MHz) the image is lowest at 0.982..0.986 while the DC
     * sweep gives 0.980; the tone's own ratio read 0.984. */
    for (unsigned i = 1; i < ok; i++)
        for (unsigned k = i; k > 0 && gr[k - 1] > gr[k]; k--) { float t = gr[k]; gr[k] = gr[k - 1]; gr[k - 1] = t; }
    const float gmed = (ok & 1u) ? gr[ok / 2u] : 0.5f * (gr[ok / 2u - 1u] + gr[ok / 2u]);
    const float gspread = gr[(3u * ok) / 4u] - gr[ok / 4u];
    float g_cur = 1.0f, o_cur = 0.0f;
    fpga_interleave_corr_get(&g_cur, &o_cur);
    const float g_new = g_cur * gmed;
    usb_debug_printf("  gain: B'/A median %ld/10000 (spread %ld/10000) -> B gain %ld/10000 (was %ld), offset kept %ld/100\r\n",
                     (long)(gmed * 10000.0f + 0.5f), (long)(gspread * 10000.0f + 0.5f),
                     (long)(g_new * 10000.0f + 0.5f), (long)(g_cur * 10000.0f + 0.5f), x100(o_cur));
    if (gspread > 0.01f || g_new < 0.90f || g_new > 1.10f) {
        usb_send_str("interleave gain: REFUSED (records disagree or out of 0.90..1.10); gain unchanged\r\n");
        return false;
    }
    fpga_interleave_corr_set(g_new, o_cur);     /* line; clears knots, keeps skew */
    usb_send_str("interleave gain: applied (RAM; `cal user save` keeps it)\r\n");
    return true;
}

/* `fpga interleave skew [measure|set <ppm>|off]` — converter-B timing skew.
 * <ppm>: B's deviation from half a converter period, ppm of a period (signed;
 * unit #2 reads 0.451 -> -49000); `set 0` = `off`. */
/* The skew line `fpga interleave skew ...` ends with. */
static void interleave_skew_print_status(void)
{
    const int32_t ppm = fpga_interleave_skew_get();
    if (ppm == 0) usb_send_str("interleave skew: correction OFF (B taken at 0.500)\r\n");
    else          print_skew_line("interleave skew: correcting", ppm);
}

/* The single-channel merge and its converter-B correction (B' = (B - off) /
 * gain, record units): il.enabled, il.gain, il.offset, il.knots, il.skew. */
/* The status block every `fpga interleave ...` ends with. */
static void interleave_print_status(void)
{
    float g, o;
    fpga_interleave_corr_get(&g, &o);
    int16_t ka[IL_KNOTS_MAX], kb[IL_KNOTS_MAX];
    const uint8_t nk = fpga_interleave_knots_get(ka, kb);
    const int32_t g4 = (int32_t)(g * 10000.0f + 0.5f);
    const int32_t o2 = (int32_t)(o * 100.0f + (o < 0 ? -0.5f : 0.5f));
    usb_debug_printf("interleave %s, %s; last record %s; B corr gain %ld/10000 offset %ld/100; "
                     "untriggered reads held %lu (only triggered records are merged; AUTO holds like NORMAL)\r\n",
                     fpga_interleave_get() ? "ON" : "OFF",
                     fpga_interleave_active() ? "active (mask 1, tb <= 3)" : "not active",
                     fpga_acq_record_interleaved() ? "merged" : "single converter",
                     (long)g4, (long)o2, (unsigned long)fpga_interleave_roll_holds());
    if (nk >= 2u) {   /* the knots override the line */
        usb_send_str("  B map, piecewise (A <- B, x100):");
        for (uint8_t i = 0; i < nk; i++)
            usb_debug_printf(" %d<-%d", (int)ka[i], (int)kb[i]);
        usb_send_str("\r\n");
    }
    const int32_t sk = fpga_interleave_skew_get();
    if (sk) print_skew_line("  skew: correcting", sk);
    else    usb_send_str("  skew: correction OFF\r\n");
}

/* `fpga interleave`: the status block and the procedures (`cal`, `tone`,
 * `skew measure`: they drive the front end over many records and then write
 * the fit they found). The settings are the il.* properties. */
static void cmd_fpga_interleave(const char *args)
{
    while (*args == ' ') args++;
    if (strncmp(args, "cal", 3) == 0) {
        cmd_fpga_interleave_cal(strstr(args + 3, "piecewise") != NULL);
        interleave_print_status();
        return;
    }
    if (strncmp(args, "tone", 4) == 0) {
        /* Skew and B's gain from a 5..50 MHz tone, twice: the gain error
         * biases the skew estimate and the skew the amplitudes. The offset
         * stays the DC sweep's (`cal`). */
        for (unsigned pass = 1; pass <= 2u; pass++) {
            usb_debug_printf("interleave tone: pass %u of 2\r\n", pass);
            if (!cmd_fpga_interleave_skew_measure(true)) break;
        }
        interleave_print_status();
        return;
    }
    if (strncmp(args, "skew", 4) == 0) {
        const char *p = args + 4;
        while (*p == ' ') p++;
        if (strncmp(p, "measure", 7) == 0) {
            (void)cmd_fpga_interleave_skew_measure(false);
            interleave_skew_print_status();
            return;
        }
    }
    if (*args) {
        usb_send_str("usage: fpga interleave [cal [piecewise]|tone|skew measure]  (no argument: status)\r\n"
                     "  the settings are properties: set il.enabled on|off; set il.knots=0 il.gain=<g> "
                     "il.offset=<o>; set il.skew <ppm>\r\n");
        return;
    }
    interleave_print_status();
}

/* Parity C3: the whole trigger in two lines -- settings and the wire, then
 * what the acquisition did (trig_policy.h). `record` is the PUBLISHED
 * record's kind: handed-over (coherent), freerun (one channel), or
 * freerun-split (AUTO roll read with both channels on: CH1 and CH2 are two
 * moments, #84 -- the badge shows "Auto*"). */
static void print_trigger_status(void)
{
    const scope_state_t *ss = scope_state_get();
    static const char *const kind[] = { "none", "handed-over", "freerun", "freerun-split" };
    const uint8_t k = fpga_acq_record_kind();
    const trig_status_t st = (trig_status_t)fpga_trigger_status();
    usb_debug_printf("trigger mode %s edge %s source %s level %d code 0x%02X reg06 0x%02X reg07 0x%02X\r\n",
                     trigger_mode_labels[ss->trigger.mode], trigger_edge_labels[ss->trigger.edge],
                     trigger_source_labels[ss->trigger.source], (int)ss->trigger.level,
                     (unsigned)fpga_acq_trig_code_get(), (unsigned)fpga_acq_trig_src_code_get(),
                     (unsigned)fpga_acq_edge_reg_get());
    usb_debug_printf("trigger status %s badge %s record %s coherent %s single %s\r\n",
                     trig_status_word(st), trig_status_label(st),
                     k < 4u ? kind[k] : "?", fpga_acq_record_coherent() ? "yes" : "no",
                     ss->trigger.mode != TRIG_SINGLE ? "-" :
                     fpga_acq_single_done() ? "captured" : "armed");
    /* What no property carries (the lines the retired `fpga scope level` and
     * `fpga scope hpos` printed): the level code the boot reconcile wrote,
     * and where the last drawn window put the trigger point. */
    const uint8_t a = (uint8_t)scope_ui_trig_anchor();
    usb_debug_printf("trigger level %d -> reg 0x08 code 0x%02X in force (boot reconcile wrote 0x%02X)\r\n",
                     (int)ss->trigger.level, (unsigned)fpga_acq_trig_code_get(),
                     (unsigned)fpga_trigger_reconcile_code());
    usb_debug_printf("trigger position: asked column %d; last window landed at %d (anchor %s)\r\n",
                     (int)prop_get(P(TRIG_X)), (int)scope_ui_trig_x_actual(),
                     a == 2 ? "hardware" : a == 1 ? "soft" : "none");
}

/* `fpga scope trigger` -- print_trigger_status(). The settings themselves
 * are the trig.* properties. */
static void cmd_fpga_scope_trigger(const char *args)
{
    (void)args;
    print_trigger_status();
}

/* `fpga scope range <n> <1|2|both>` — apply coarse frontend range n (0-9) to
 * one channel or, with the explicit `both` keyword, to both. The two channels
 * have independent relay banks (CH1 PC12/PE4/PE5/PE6, CH2 PA15/PB11/PB10/PA10)
 * driven from the same 10-case stock table. Coupling is NOT touched here — it
 * is PD12/PD13, set in the frontend init (bench 2026-08-15).
 *
 * The channel argument is MANDATORY and 1-based on purpose. It used to be
 * optional, and anything that was not 1 or 2 — including a `0` typed by an
 * experimenter who reasonably read the argument as 0-based — fell through to
 * "both", drove both relay banks, and invalidated the run while printing a
 * success line. Ambiguity now errors instead of guessing. */
static void cmd_fpga_scope_range(const char *args)
{
    static const char USAGE[] =
        "Usage: fpga scope range <0-9> <1|2|both>\r\n"
        "  channel is MANDATORY and 1-BASED: 1 = CH1, 2 = CH2, both = CH1+CH2\r\n";
    uint32_t n = 0;
    uint32_t chn = 0;
    uint8_t ch_sel;
    const char *space;

    if (args == NULL || *args == '\0' || parse_int(args, &n) != 0 || n > 9) {
        usb_send_str(USAGE);
        return;
    }

    space = strchr(args, ' ');
    if (space == NULL) {
        usb_send_str("ERR: no channel given — refusing to assume.\r\n");
        usb_send_str(USAGE);
        return;
    }
    while (*space == ' ') space++;

    if (strcmp(space, "both") == 0) {
        ch_sel = 0xFF;
    } else if (parse_int(space, &chn) == 0 && (chn == 1 || chn == 2)) {
        ch_sel = (uint8_t)(chn - 1);
    } else {
        usb_debug_printf("ERR: bad channel '%s' — 1, 2 or both (there is no channel 0)\r\n",
                         space);
        usb_send_str(USAGE);
        return;
    }

    fpga_scope_set_range_diag_ch(ch_sel, (uint8_t)n);
    usb_send_str("(raw relay drive — display state and counts->volts k NOT "
                 "updated; use `set chN.vdiv`)\r\n");
    usb_debug_printf("scope frontend range = %lu on %s\r\n",
                     (unsigned long)n,
                     ch_sel == 0 ? "CH1" : (ch_sel == 1 ? "CH2" : "CH1+CH2"));
}

/* ---- fpga scope center [range] -------------------------------------------
 *
 * Per-range DC-offset CENTERING for scope gain calibration.
 *
 * Each frontend voltage range has its own DC operating point, so before per-
 * range GAIN can be calibrated the vertical-offset DAC (DAC1 / PA4) must be
 * positioned so a quiet/DC input lands at mid-scale (ADC code ~128). Doing
 * this by hand for every range is slow; this command automates it on-device.
 *
 * For each range it applies the frontend (DC coupling) via
 * fpga_scope_set_range_diag(), then binary-searches DAC1 (0..4095) for the
 * value that drives the mean of a 0x04 CH1 capture closest to 128. DAC1 is
 * monotonic-increasing vs ADC code (bench: 500->~0, 2500->~140, 3500->~255),
 * so the search raises DAC1 when the mean is below target and lowers it when
 * above. The operator must keep the input quiet/DC while this runs (no AC
 * signal) — it averages a static level.
 *
 * Diagnostic / bench only. It runs with the acq task LIVE (it reads the acq
 * task's RAM buffer, not SPI3, so there is no bus contention) — the task must
 * keep running to refresh that buffer at each new DC operating point.
 */
#define SCOPE_CENTER_TARGET   128u   /* ADC mid-scale code we center on    */
#define SCOPE_CENTER_AVG      2u     /* buffer medians averaged per step    */
#define SCOPE_CENTER_ITERS    11u    /* binary-search steps over 0..4095    */
#define SCOPE_CENTER_SETTLE_MS 480u  /* >= one ~430ms buffer fill after a DAC
                                        move, so the read is not stale         */
#define SCOPE_CENTER_COMMIT_TIMEOUT_MS 4000u /* bound on waiting for fresh
                                        commits; 0x12 AUTO fallback ~1.5 s   */
static uint32_t scope_center_stale_steps;  /* steps that timed out waiting */

/* One capture read window (opcode 0x04 = CH1 buffer, 0x05 = CH2); returns the
 * mean of the first `nbytes` payload bytes. Same framing as spi3_opread_window
 * (one CS-LOW window, opcode + 2 filler bytes, then payload). The two filler
 * bytes are the header stock discards. Caller must already hold the acq pause. */
/*
 * Median of the acq task's LIVE RAM buffer via an 8-bit histogram.
 *
 * Two design choices, both learned on the bench (2026-08-28/29):
 *
 * 1. MEDIAN, not mean. The old servo optimised the arithmetic mean, which a
 *    clipped AC signal biases toward the OPEN rail: at large amplitude the sine
 *    clips the top, the mean reads < 128, the search raises DAC1, that clips
 *    MORE, and it runs away to a rail (0.5 Vpp centred perfectly at 2415/128,
 *    but 4 Vpp diverged to 3071). The median equals the DC baseline regardless
 *    of one-rail clipping until more than half the samples clip, and stays
 *    monotonic in DAC1, so the binary search converges at any amplitude.
 *
 * 2. Read the RAM buffer the RUNNING acq task keeps fresh, NOT a direct SPI3
 *    opread with the acq task paused. The capture buffer is a rolling window;
 *    with acq paused nothing re-arms it, so after a DAC step it FREEZES holding
 *    a bimodal old+new mix and no statistic recovers the new operating point
 *    (bench: dac=3071 read 120, frozen, while its true settled level is 207).
 *    The acq task re-arms and commits continuously, so its buffer tracks the
 *    DAC — that is exactly the path `spi3 acqread` reads, which settles in
 *    ~350 ms. Centering therefore runs with acq LIVE and reads RAM (no SPI3
 *    from this side, so no bus contention).
 */
static uint32_t scope_center_buf_median(uint8_t ch)
{
    const volatile uint8_t *b = (ch == 2) ? fpga_get_ch2_buf()
                                          : fpga_get_ch1_buf();
    if (b == NULL) return 128;
    uint16_t hist[256] = {0};
    for (uint32_t i = 0; i < FPGA_ADC_BUF_SIZE; i++)
        hist[b[i]]++;
    uint32_t half = FPGA_ADC_BUF_SIZE / 2u, cum = 0;
    for (uint32_t v = 0; v < 256; v++) {
        cum += hist[v];
        if (cum >= half) return v;
    }
    return 128;
}

/* Binary-search the channel's offset reference for a mean of ~128. Assumes the
 * range is already applied and the acq task paused. `ch` is 1 or 2: CH1 drives
 * DAC1 (PA4) and reads opcode 0x04; CH2 drives TMR13_C1DT (PA6, PWM-DAC) and
 * reads 0x05. Returns the best code found and its resulting mean.
 *
 * The monotonic assumption below was measured for DAC1 on CH1. For CH2 it is
 * ASSUMED, not verified — TMR13's channel polarity is active-low (stock sets
 * CCTRL C1P), so the duty->voltage sense could invert. If a CH2 search
 * converges on a rail, suspect the direction before suspecting the pin: sweep
 * `trig2 raw` by hand and watch which way the mean moves. */
/*
 * Settle the acq buffer after a DAC step: the running acq task re-commits its
 * RAM buffer every ~33 ms poll, and the operating point settles in ~350 ms
 * (bench), so wait a generous fixed budget then average a couple of RAM-buffer
 * medians to knock down per-frame noise.
 */
/* Wait until the acq task has committed `n` records whose reads began after
 * this call. The frame generation is a seqlock: +2 per commit, odd while a
 * commit is in progress. Returns false on timeout.
 *
 * Why not a fixed delay (the pre-2026-09-22 version slept 480 ms): under the
 * poll loop (EXP-54) the RAM buffer changes only on a commit, and with no
 * trigger crossing AUTO commits one free-run record per fallback budget,
 * 3 x fill + 230 ms = 475 ms at 0x10 and ~1.46 s at 0x12. A fixed 480 ms
 * therefore read the buffer from BEFORE the DAC step at slow codes and
 * sometimes at 0x10; the binary search steered on stale medians and the
 * servo reported "DAC1=2047 (median=101)" while the record then read 185. The
 * first commit after the write may have been read before the DAC settled, so
 * the caller asks for two. */
static bool scope_center_wait_commits(uint32_t n)
{
    uint32_t g = fpga_acq_frame_generation() & ~1u;
    uint32_t target = g + 2u * n;
    TickType_t t0 = xTaskGetTickCount();
    while ((int32_t)((fpga_acq_frame_generation() & ~1u) - target) < 0) {
        if ((xTaskGetTickCount() - t0) > pdMS_TO_TICKS(SCOPE_CENTER_COMMIT_TIMEOUT_MS))
            return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

static uint32_t scope_center_settle_level(uint8_t ch)
{
    /* Keep the historical floor for the analog settle (~350 ms bench), then
     * require fresh commits so the median is of a record read after the DAC
     * moved. On timeout the median is still taken but the search is steering
     * blind; the caller reports the count. */
    vTaskDelay(pdMS_TO_TICKS(SCOPE_CENTER_SETTLE_MS));
    if (!scope_center_wait_commits(2u)) scope_center_stale_steps++;
    uint32_t acc = 0;
    for (uint32_t k = 0; k < SCOPE_CENTER_AVG; k++) {
        if (k > 0 && !scope_center_wait_commits(1u)) scope_center_stale_steps++;
        acc += scope_center_buf_median(ch);
    }
    return acc / SCOPE_CENTER_AVG;
}

static void scope_center_one(uint8_t ch, uint16_t *out_dac, uint32_t *out_mean)
{
    uint16_t lo = 0, hi = 4095;
    uint16_t best_dac = 2048;
    uint32_t best_mean = 0;
    uint32_t best_err = 0xFFFFFFFFu;

    for (uint32_t it = 0; it < SCOPE_CENTER_ITERS; it++) {
        uint16_t mid = (uint16_t)((lo + hi) / 2);
        if (ch == 2) scope_trigger_ch2_raw(mid);
        else         scope_trigger_dac_raw(mid);
        /* Wait for the running acq task to re-commit its RAM buffer at the new
         * DC operating point, then read the settled median from that buffer.
         * (The old paused-SPI path froze the rolling buffer mid-transition —
         * see scope_center_buf_median.) */
        uint32_t level = scope_center_settle_level(ch);
        uint32_t err = (level > SCOPE_CENTER_TARGET)
                       ? level - SCOPE_CENTER_TARGET
                       : SCOPE_CENTER_TARGET - level;
        if (err < best_err) {
            best_err = err;
            best_dac = mid;
            best_mean = level;
        }
        if (level < SCOPE_CENTER_TARGET) lo = mid;   /* need higher DAC1 */
        else                             hi = mid;   /* need lower  DAC1 */
        if (hi - lo <= 1) break;
    }
    /* Leave the reference at the value we REPORT. Until 2026-09-22 the DAC
     * stayed at the last probe (`mid`), not at best_dac, so the printed
     * "center DAC1=..." could differ from what the hardware held. */
    if (ch == 2) scope_trigger_ch2_raw(best_dac);
    else         scope_trigger_dac_raw(best_dac);
    *out_dac = best_dac;
    *out_mean = best_mean;
}

/*
 * `fpga scope freq [n]` — run the SHIPPED frequency estimator on the live
 * acquisition buffer, n times, and report every outcome including refusals.
 *
 * This exists because reading a number off the LCD cannot distinguish "the
 * estimator is right" from "the estimator is wrong and the badge is holding a
 * stale value". It reports the diagnostics the badge throws away: sharpness,
 * which window was used, and the interpolated bin.
 */
/*
 * `settings` — why did the boot reconcile say what it said?
 *
 * Added 2026-08-19 after three consecutive power cycles reported "pulled the
 * display down to the arm block's 0x08", i.e. the restored timebase had no
 * measured rate — including one run where the change was committed through a
 * documented immediate-flush point (a mode switch). That is either a write
 * that never reached flash or a load that never came back, and NOTHING ON THE
 * DEVICE DISTINGUISHED THEM: settings_store_status_t has carried
 * storage_bound, load_result, writes, write_failures and changes_seen since
 * the store was written, and no caller ever read them.
 *
 * Same shape as the /2 SSPI reads, the floating MISO and the invented
 * volts/div labels: a conclusion drawn from one bit, while the instrument that
 * would have settled it already existed and was simply never wired up.
 */
/* reset settings CONFIRM: "Reset to defaults" on the direct route. The
 * built-in defaults (the property table's) go over the user settings (the
 * saved record) through the same function as the Settings row: the input
 * task runs every setter, so the hardware follows, and writes the record
 * (input_handler.c settings_reset_now -> settings_store_reset_to_defaults).
 * The word CONFIRM is the confirm step. Never touched: calibration of any
 * kind, the gallery, the logo, the firmware cache. */
static void reset_emit(const char *line) { (void)usb_debug_printf("%s\r\n", line); }

static void cmd_reset_settings(const char *args)
{
    while (*args == ' ') args++;
    if (strcmp(args, "SESSION") == 0) {
        /* Defaults for this session without saving: not offered. The store
         * saves the live state on the next key press or screen change
         * (settings_store.h), so "not saved" could not be kept. */
        usb_send_str("ERR reset settings SESSION is not available: the settings store saves the live "
                     "state on the next key press, so the defaults would be saved anyway\r\n");
        return;
    }
    if (strcmp(args, "CONFIRM") != 0) {
        usb_send_str("usage: reset settings CONFIRM\r\n"
                     "  every setting back to its built-in default (list changed shows which), applied "
                     "and saved\r\n"
                     "  kept: calibration (factory, backup, cal user, il.*), gallery, logo, firmware "
                     "cache, the screen shown\r\n");
        return;
    }
    settings_reset_result_t r;
    bool in_progress = false;
    /* Each scope register a reset moves may wait for an acquisition park
     * (<= ~1 s); a dozen of them is the worst case. */
    if (!input_settings_reset_sync(20000u, &r, &in_progress)) {
        usb_send_str(in_progress
                     ? "TIMEOUT: the reset is still running on the input task; check with: list changed\r\n"
                     : "TIMEOUT: input task busy (gallery/countdown/selfcal?); nothing changed, nothing saved\r\n");
        return;
    }
    model_shell_reset_report(r.status, r.changed, r.left, settings_flush_outcome_text(r.saved), reset_emit);
}

static void cmd_settings(void)
{
    const settings_store_status_t *st = settings_store_get_status();
    const scope_state_t *ss = scope_state_get();

    if (st == NULL) {
        usb_send_str("settings: no status\r\n");
        return;
    }

    usb_debug_printf("storage bound : %s\r\n",
                     st->storage_bound ? "YES" : "NO - nothing can persist");
    usb_debug_printf("boot load     : %s\r\n",
                     config_load_result_name(st->load_result));
    usb_debug_printf("writes ok     : %lu\r\n", (unsigned long)st->writes);
    usb_debug_printf("write failures: %lu\r\n", (unsigned long)st->write_failures);
    usb_debug_printf("changes seen  : %lu\r\n", (unsigned long)st->changes_seen);
    usb_debug_printf("live timebase : 0x%02X\r\n", (unsigned)ss->timebase_idx);

    /* The store's "write failures" above conflates a refusal we chose with a
     * failure we suffered — config.h:162 warns about exactly that lie, and on
     * 2026-08-20 it cost a bench session: three power-cycle tests blamed the
     * write path when the build had writes disabled. These lines make the
     * distinction unmissable. */
    const config_persist_stats_t *cs = config_persist_stats();
    usb_debug_printf("writes enabled: %s\r\n",
                     cs->writes_enabled
                         ? "YES (SETTINGS_PERSIST_WRITES=1)"
                         : "NO - SETTINGS_PERSIST_WRITES=0 in this build "
                           "(default is 1 since the 2026-08-20 bench pass)");
    usb_debug_printf("saves ok/fail/disabled: %lu/%lu/%lu  last save status: %ld\r\n",
                     (unsigned long)cs->saves_ok,
                     (unsigned long)cs->saves_failed,
                     (unsigned long)cs->saves_disabled,
                     (long)cs->last_save_status);
    usb_send_str("(a change carries only if a later button press or an "
                 "orderly power-off follows it - see settings_store.h)\r\n");
}

/* The timebase in force, for `status`: the boot reconcile, then the code,
 * reg 0x01, the sample rate and the s/div label (the lines the retired
 * `fpga scope timebase` printed; the code itself is the property tb.code).
 * after_set: a code was just set (the published record is still the
 * previous one). */
static void timebase_print_status(bool after_set)
{
    const scope_state_t *ss = scope_state_get();
    static const char *const recon[] = {
        "NEVER RAN — this build's arm path does not call it",
        "pushed the persisted code to the FPGA",
        "pulled the display down to the arm block's 0x08",
    };
    const uint8_t ra = fpga_timebase_reconcile_action();
    usb_debug_printf("boot reconcile: %s\r\n", recon[ra < 3u ? ra : 0u]);

    char lbl[12];
    /* What the status bar shows: x2 merged, times the fast-code zoom (C5). */
    /* After a set, the published record is still the previous one, so ask
     * what the NEW code will produce (fpga_interleave_active) rather than
     * scope_ui_view_merged(), which reads that stale record. */
    scope_timebase_view_label(ss->timebase_idx,
                              after_set ? fpga_interleave_active()
                                        : scope_ui_view_merged(),
                              lbl, sizeof(lbl));
    usb_debug_printf("timebase 0x%02X (reg 0x01 = 0x%02X)  %lu S/s  %s/div\r\n",
                     (unsigned)ss->timebase_idx,
                     (unsigned)fpga_acq_rate_idx_get(),
                     (unsigned long)(scope_timebase_sample_rate(ss->timebase_idx) + 0.5f),
                     lbl);
}

/* Millivolts of one roll point as a signed "x.yyy" string, or counts when the
 * range is uncalibrated -- the same rule and units the Y-T badges use. */
static void fmt_roll_point(char *out, size_t n, uint8_t ch, uint8_t code)
{
    const scope_state_t *ss = scope_state_get();
    const channel_state_t *c = (ch == 1u) ? &ss->ch1 : &ss->ch2;
    const uint8_t zero = scope_measure_zero_for(scope_vpos_active(), c->position,
                                                (int)FPGA_ADC_OFFSET);
    const float vpc = scope_cal_volts_per_count(ch, c->vdiv_idx);
    if (!(vpc > 0.0f)) {
        snprintf(out, n, "code %u (zero %u, no cal)", (unsigned)code, (unsigned)zero);
        return;
    }
    const float v = scope_roll_point_volts(code, zero, vpc);
    long mv = (long)(v * 1000.0f + (v >= 0.0f ? 0.5f : -0.5f));
    const char sign = (mv < 0) ? '-' : '+';
    if (mv < 0) mv = -mv;
    snprintf(out, n, "code %u = %c%ld.%03ld V", (unsigned)code, sign, mv / 1000, mv % 1000);
}

/* `fpga roll [code <hex>|code stock]` -- roll mode (C11) status, with the
 * last op03 frame exactly as received so the live slots can be checked.
 * Roll itself is tb.roll and its step tb.rollstep. `code` picks the reg-0x01
 * byte written on entry (stock writes 0x14 + step; 0x14 is the only code
 * with a measured rate): a debug knob of fpga.c that is no property. */
static void roll_print_status(void)
{
    const scope_state_t *ss = scope_state_get();
    fpga_roll_stats_t st;
    fpga_roll_stats_get(&st);
    char lbl[12];
    scope_roll_label(ss->roll_step, lbl, sizeof(lbl));
    const int ov = fpga_roll_code_override_get();
    usb_debug_printf("roll: %s  step %u = %s/div, %lu ms/point  (ui %s, acq %s)\r\n",
                     ss->roll ? "ON" : "off", (unsigned)ss->roll_step, lbl,
                     (unsigned long)scope_roll_period_ms(ss->roll_step),
                     ss->roll ? "roll" : "Y-T",
                     fpga_roll_active() ? "op03" : "04/05");
    usb_debug_printf("reg 0x01 = 0x%02X (last apply; entry byte %s%02X)\r\n",
                     (unsigned)fpga_acq_rate_idx_get(),
                     ov >= 0 ? "override 0x" : "stock 0x",
                     (unsigned)(ov >= 0 ? (unsigned)ov : scope_roll_stock_code(ss->roll_step)));
    const scope_roll_fifo_t *f = fpga_roll_fifo();
    usb_debug_printf("fifo %u/%u points, %lu pushed; reads %lu rejects %lu late %lu\r\n",
                     (unsigned)f->count, (unsigned)SCOPE_ROLL_POINTS,
                     (unsigned long)f->total, (unsigned long)st.reads,
                     (unsigned long)st.rejects, (unsigned long)st.late);
    usb_debug_printf("last op03 rx: %02X %02X [CH1 %02X] %02X [CH2 %02X]\r\n",
                     st.rx[0], st.rx[1], st.rx[2], st.rx[3], st.rx[4]);
    usb_debug_printf("trigger: mode %u (0 AUTO 1 NORM 2 SINGLE), roll hits %lu, run %u\r\n",
                     (unsigned)ss->trigger.mode, (unsigned long)st.trig_hits,
                     (unsigned)ss->running);
    uint8_t c1, c2;
    if (scope_roll_fifo_get(f, 0u, 0u, &c1) && scope_roll_fifo_get(f, 1u, 0u, &c2)) {
        char a[40], b[40];
        fmt_roll_point(a, sizeof(a), 1u, c1);
        fmt_roll_point(b, sizeof(b), 2u, c2);
        usb_debug_printf("newest: CH1 %s  CH2 %s\r\n", a, b);
        /* Where each channel's 0 V is expected and what the offset DAC was
         * last told: the zero in the lines above is the cal-page formula
         * (100 + pos), so a channel whose DAC was never written (or written
         * for another range) reads off by exactly the difference. */
        for (uint8_t ch = 1u; ch <= 2u; ch++) {
            const channel_state_t *cs = (ch == 1u) ? &ss->ch1 : &ss->ch2;
            const uint8_t z = scope_measure_zero_for(scope_vpos_active(), cs->position,
                                                     (int)FPGA_ADC_OFFSET);
            uint8_t bk = 0, rg = 0;
            uint16_t dc = 0;
            const bool w = scope_vpos_last(ch, &bk, &rg, &dc);
            const uint8_t c = (ch == 1u) ? c1 : c2;
            usb_debug_printf("CH%u: en %u pos %d range %u zero %u, newest %u = %+d cnt; "
                             "dac %s",
                             (unsigned)ch, (unsigned)cs->enabled, (int)cs->position,
                             (unsigned)cs->vdiv_idx, (unsigned)z, (unsigned)c,
                             (int)c - (int)z, w ? "" : "never written\r\n");
            if (w)
                usb_debug_printf("bank %u range %u code %u\r\n", (unsigned)bk,
                                 (unsigned)rg, (unsigned)dc);
        }
    }
}

static void cmd_fpga_roll(const char *args)
{
    while (*args == ' ') args++;
    if (strncmp(args, "code", 4) == 0) {
        const char *q = args + 4;
        while (*q == ' ') q++;
        uint32_t v;
        if (strncmp(q, "stock", 5) == 0) {
            fpga_roll_code_override_set(-1);
        } else if (*q && parse_hex32(q, &v) == 0 && v <= 0xFFu) {
            fpga_roll_code_override_set((int)v);
        } else {
            usb_send_str("usage: fpga roll code <hex 00-FF>|stock\r\n");
            return;
        }
        /* Rolling: re-enter with the new byte, by asking the input task for
         * the step in force again (the setter re-enters roll with it). */
        if (prop_get(P(TB_ROLL))) {
            const prop_pair_t p = { PROP_ID_TB_ROLLSTEP, prop_get(P(TB_ROLLSTEP)) };
            model_result_t res;
            const prop_status_t st = prop_request(&p, 1, ORIGIN_SHELL, prop_request_timeout(&p, 1), &res);
            if (st != PS_OK && st != PS_CLAMPED) {
                usb_debug_printf("%s tb.rollstep (%s): the entry byte is set, roll was not re-entered\r\n",
                                 prop_status_name(st), res.note.text[0] ? res.note.text : "no reason given");
                return;
            }
        }
    } else if (*args) {
        usb_send_str("usage: fpga roll [code <hex 00-FF>|code stock]  (no argument: status)\r\n"
                     "  roll is a property: set tb.rollstep=<0-8> tb.roll=on; set tb.roll off\r\n");
        return;
    }
    roll_print_status();
}

/* `fpga scope measure [reps]` — print what the measurement badges compute,
 * raw and machine-parseable: one line per fresh record. This is the bench
 * instrument for EXP-19 badge validation, and it deliberately calls the SAME
 * sources the badges call (scope_measure_record, scope_cal_volts_per_count,
 * scope_timebase_sample_rate, scope_freq_estimate) rather than reimplementing
 * any of them — two implementations of one quantity is the two-renderers bug.
 *
 * Integer-only output (shell printf carries no %f): voltages in MICROVOLTS,
 * duty in permille, period in samples x100, frequency in mHz. A field the
 * instrument refuses (k = 0, no rate, estimator declined) prints "-" so a
 * refusal can never be confused with a measured zero. */
static void cmd_fpga_scope_measure(const char *args)
{
    uint32_t reps = 5;
    /* Checked — see cmd_fpga_scope_freq: an unparseable argument used to
     * degrade this to a single record without saying so. */
    if (args && *args && parse_int(args, &reps) != 0) {
        usb_send_str("Usage: fpga scope measure [reps 1-50]\r\n");
        return;
    }
    if (reps == 0u) reps = 1u;
    if (reps > 50u) reps = 50u;

    const scope_state_t *ss = scope_state_get();
    const float fs_tab = scope_timebase_sample_rate(ss->timebase_idx);
    const uint8_t in_force = fpga_acq_rate_idx_get();
    /* The PUBLISHED record's rate (x2 for an interleaved record) when the
     * hardware runs the code the UI shows; `fs` in the header and the Freq
     * estimate both use it, so an interleaved record is not read at half. */
    const float fs = (in_force == ss->timebase_idx) ? fpga_record_sample_rate()
                                                    : fs_tab;
    const bool tb_ok = (in_force == ss->timebase_idx) && (fs > 0.0f);
    const float k1 = scope_cal_volts_per_count(1u, ss->ch1.vdiv_idx);
    const float k2 = scope_cal_volts_per_count(2u, ss->ch2.vdiv_idx);

    /* k1/k2 are probe-TIP volts per count (scope_cal applies the probe
     * factor); probeN says which factor, so a bench script can tell a 10x
     * setting from a calibration change. Appended, so parsers keep working. */
    usb_debug_printf("badge sources: rng1=%u k1_uV=%lu  rng2=%u k2_uV=%lu  "
                     "tb=0x%02X inforce=0x%02X fs=%lu  probe1=x%u probe2=x%u\r\n",
                     (unsigned)ss->ch1.vdiv_idx,
                     (unsigned long)(k1 * 1e6f + 0.5f),
                     (unsigned)ss->ch2.vdiv_idx,
                     (unsigned long)(k2 * 1e6f + 0.5f),
                     (unsigned)ss->timebase_idx, (unsigned)in_force,
                     (unsigned long)(fs + 0.5f),
                     (unsigned)(scope_cal_probe_factor(1u) + 0.5f),
                     (unsigned)(scope_cal_probe_factor(2u) + 0.5f));
    if (in_force != ss->timebase_idx)
        usb_send_str("TB MISMATCH: frequency suppressed (see fpga scope freq)\r\n");

    const volatile uint8_t *ch1 = fpga_get_ch1_buf();
    const volatile uint8_t *ch2 = fpga_get_ch2_buf();
    if (!ch1) { usb_send_str("FPGA not initialized\r\n"); return; }

    for (uint32_t i = 0; i < reps; i++) {
        scope_measure_t m1, m2;
        scope_measure_record((const uint8_t *)ch1, FPGA_ADC_BUF_SIZE, &m1);
        scope_measure_record(ch2 ? (const uint8_t *)ch2 : NULL,
                             ch2 ? FPGA_ADC_BUF_SIZE : 0u, &m2);

        char vpp1[12] = "-", vrms1[12] = "-", per[12] = "-", f[16] = "-";
        float freq_hz = 0.0f;
        char vpp2[12] = "-";
        if (k1 > 0.0f) {
            snprintf(vpp1, sizeof vpp1, "%lu",
                     (unsigned long)((float)m1.pp_robust * k1 * 1e6f + 0.5f));
            snprintf(vrms1, sizeof vrms1, "%lu",
                     (unsigned long)(m1.ac_rms * k1 * 1e6f + 0.5f));
        }
        if (m1.period_valid)
            snprintf(per, sizeof per, "%lu",
                     (unsigned long)(m1.period_samples * 100.0f + 0.5f));
        if (tb_ok) {
            scope_freq_t fr;
            if (scope_freq_estimate((const uint8_t *)ch1, FPGA_ADC_BUF_SIZE,
                                    fs, &fr)) {
                freq_hz = fr.hz;
                snprintf(f, sizeof f, "%lu",
                         (unsigned long)(fr.hz * 1000.0f + 0.5f));
            }
        }
        if (m2.valid && k2 > 0.0f)
            snprintf(vpp2, sizeof vpp2, "%lu",
                     (unsigned long)((float)m2.pp_robust * k2 * 1e6f + 0.5f));

        /* Edge timing in SAMPLES x100 (M4). "-" when no clean edge was found,
         * same convention as per1. Kept in samples, not seconds: multiply by
         * 1/fs on the host when a rate is in force. */
        char rise[12] = "-", fall[12] = "-";
        if (m1.rise_valid)
            snprintf(rise, sizeof rise, "%lu",
                     (unsigned long)(m1.rise_samples * 100.0f + 0.5f));
        if (m1.fall_valid)
            snprintf(fall, sizeof fall, "%lu",
                     (unsigned long)(m1.fall_samples * 100.0f + 0.5f));

        usb_debug_printf("M %2lu pp1=%u ppr1=%u Vpp1_uV=%s Vrms1_uV=%s duty1_pm=%lu "
                         "per1_smp100=%s f1_mHz=%s rise1_smp100=%s fall1_smp100=%s "
                         "pp2=%u Vpp2_uV=%s\r\n",
                         (unsigned long)i, (unsigned)m1.pp,
                         (unsigned)m1.pp_robust, vpp1, vrms1,
                         (unsigned long)(m1.level_valid
                                         ? m1.duty_pct * 10.0f + 0.5f : 0.0f),
                         per, f, rise, fall, (unsigned)m2.pp, vpp2);

        /* All 12 of stock's parameters (C6), for CH1, through the SAME
         * scope_params_compute() the badges use. "--" = refused. Hidden or
         * not, all 12 are listed: selection only decides what the screen
         * shows. */
        {
            scope_prm_ctx_t ctx;
            scope_params_t pp;
            /* Only the code in force describes the record in hand; with a
             * mismatch the widths stay in samples, like Freq stays "-". */
            ctx.sample_rate     = tb_ok ? fs : 0.0f;
            ctx.record_whole    = fpga_acq_record_time_ordered();
            ctx.volts_per_count = k1;
            ctx.freq_hz         = freq_hz;
            ctx.zero_count      = scope_measure_zero_for(scope_vpos_active(), ss->ch1.position, (int)FPGA_ADC_OFFSET);
            scope_params_compute(&m1, &ctx, &pp);
            usb_debug_printf("P %2lu", (unsigned long)i);
            for (unsigned id = 0; id < SCOPE_PRM_COUNT; id++) {
                char b[16];
                scope_prm_format(&pp.v[id], b, sizeof b);
                usb_debug_printf(" %s=%s", scope_prm_label(id), b);
            }
            usb_send_str("\r\n");
        }
        usb_delay_ms(60);
    }
}

/* Which of the 12 measurements each channel shows (chN.meas, bit i =
 * parameter i in stock's order: Freq Peri Duty+ Duty- Wid+ Wid- Vmax Vmin
 * Vp-p Amp Vrms Avg) and long PRM's hide (view.meashide), as the strip
 * draws them, for `ui dump`: with more than MSTRIP_MAX_PER_CH (6) on, the
 * strip shows the 6 measure_strip_shown() picks (levels first); the others
 * are listed "(no slot)". */
static void prm_print_status(void)
{
    const scope_state_t *ss = scope_state_get();
    usb_debug_printf("prm hidden=%u\r\n", (unsigned)ss->prm_hidden);
    for (uint8_t c = 0; c < 2u; c++) {
        const uint16_t m = meas_select_get(c);
        usb_debug_printf("CH%u mask=0x%03X shown:", (unsigned)c + 1u, (unsigned)m);
        uint8_t ids[SCOPE_PRM_COUNT];
        const unsigned n = scope_prm_select(m, ids, SCOPE_PRM_COUNT);
        const uint16_t shown = measure_strip_shown(m);
        for (unsigned i = 0; i < n; i++)
            usb_debug_printf(" %s%s", scope_prm_label(ids[i]),
                             (shown >> ids[i]) & 1u ? "" : "(no slot)");
        usb_send_str("\r\n");
    }
}

/* `fpga scope cursor` — the cursor readouts (C7) as the screen computes them:
 * X1 X2 X1-X2 1/|dX| from the PUBLISHED record's rate, Y1 Y2 Y1-Y2 from the
 * last drawn band. Y1/Y2 are "--" unless the true-scale graticule is on (the
 * autofit render re-anchors the vertical every frame). Move the cursors first
 * with the CUR key; the cursors' mode does not matter here. */
static void cmd_fpga_scope_cursor(const char *args)
{
    (void)args;
    char x1[16], x2[16], dx[16], ix[16], y1[16], y2[16], dy[16];
    float fs = 0.0f;
    scope_ui_cursor_report(x1, x2, dx, ix, y1, y2, dy, &fs);
    /* fs is the COLUMN rate, 1000 x record rate / R: record rate x zoom L
     * (parity C5), or the selected code's on a re-sampled held record (L=0,
     * C14). */
    usb_debug_printf("cursor fs=%lu L=%u R=%lu X1=%s X2=%s X1-X2=%s Y1=%s Y2=%s Y1-Y2=%s 1/dX=%s\r\n",
                     (unsigned long)(fs + 0.5f), (unsigned)scope_ui_view_factor(),
                     (unsigned long)scope_ui_view_ratio(), x1, x2, dx, y1, y2, dy, ix);
}

/* `fpga scope center [ch1|ch2] [0-9]` — auto-center the channel's vertical-
 * offset reference for one range (arg given) or for every range 0..9 (no arg),
 * one report line per range. Channel defaults to CH1, preserving the original
 * bench-validated `fpga scope center [0-9]` usage byte-for-byte.
 *
 * The channel is a NAMED TOKEN, not a numeric suffix, deliberately. On
 * 2026-08-17 `fpga scope range <n> 0` silently addressed BOTH channels because
 * its channel argument was 1-based and 0 fell through to "both", which invalidated
 * a whole relay test. A `center2` spelling would have been worse still: the
 * dispatcher matches "fpga scope center" on 17 chars, so `center2` would have
 * matched the CH1 handler with empty args and quietly swept all ten CH1 ranges. */
static void cmd_fpga_scope_center(const char *args)
{
    char buf[32];
    uint8_t ch = 1;

    if (args == NULL) args = "";
    if (strlen(args) >= sizeof(buf)) {
        usb_send_str("Usage: fpga scope center [ch1|ch2] [0-9]\r\n");
        return;
    }
    strcpy(buf, args);

    char *saveptr = NULL;
    char *t1 = strtok_r(buf, " \t", &saveptr);
    if (t1 != NULL && (strcmp(t1, "ch1") == 0 || strcmp(t1, "ch2") == 0)) {
        ch = (t1[2] == '2') ? 2u : 1u;
        t1 = strtok_r(NULL, " \t", &saveptr);
    }

    bool all = (t1 == NULL || *t1 == '\0');
    uint32_t n = 0;

    if (!all && (parse_int(t1, &n) != 0 || n > 9)) {
        usb_send_str("Usage: fpga scope center [ch1|ch2] [0-9]\r\n");
        return;
    }

    /* Centering runs with the acq task LIVE (it keeps the RAM buffer fresh at
     * the new DC operating point — see scope_center_buf_median). The servo
     * touches only the DAC and reads RAM, never SPI3, so there is no bus
     * contention with the acq task's reads and no need to pause it. */

    /* BUGFIX 2026-08-14: the search's scope_trigger_dac_raw() writes were inert
     * without this init (bench: reported means clustered 120-170 regardless of
     * DAC1, never spanning 0-255, so the binary search flailed around a fixed
     * operating point). `trig raw` calls it too — the DAC peripheral must be
     * enabled before raw writes take effect. */
    if (ch == 2) scope_trigger_ch2_init();
    else         scope_trigger_dac_init();

    uint32_t first = all ? 0 : n;
    uint32_t last  = all ? 9 : n;
    for (uint32_t r = first; r <= last; r++) {
        uint16_t dac;
        uint32_t mean;
        /* Deliberately applies the range to BOTH banks even when centering CH2:
         * that is what the bench-validated CH1 path did (648492a), and the two
         * frontends are independent, so restricting it would be an untested
         * change to a working measurement for no measurable gain. */
        fpga_scope_set_range_diag((uint8_t)r);
        vTaskDelay(pdMS_TO_TICKS(20));     /* relay/frontend settle */
        scope_center_stale_steps = 0;
        scope_center_one(ch, &dac, &mean);
        /* Name the reference in the output: a bare "center=" line would read
         * identically whichever channel ran, and a mislabelled log is how this
         * project has repeatedly convinced itself of the wrong pin. */
        usb_debug_printf("CH%u range %lu: center %s=%u (median=%lu)%s\r\n",
                         (unsigned)ch, (unsigned long)r,
                         (ch == 2) ? "TMR13_C1DT" : "DAC1",
                         dac, (unsigned long)mean,
                         scope_center_stale_steps
                             ? "  STALE: no fresh commit on some steps -- result unreliable"
                             : "");
    }
    /* acq was never paused (see the note above) — nothing to resume. */
}

/*
 * Stock calibration page in use (parity C4; src/util/calpage.h, scope_vpos.h).
 *   cal page [on|off|reload]     decoded table, bank in force, substitutions
 *   cal page calc <1|2> <bank> <range> <pos>   one code, ours vs stock's
 * The vertical position is the property chN.pos; `cal page` prints the
 * offset code it gave and the reference read back, per channel.
 * The page is only READ here; `cal restore` is still the one writer.
 */
static void fmt_g(char *out, size_t n, float g)
{
    if (g <= 0.0f) { snprintf(out, n, "-- (compiled table)"); return; }
    const unsigned long x = (unsigned long)(g * 10000.0f + 0.5f);
    snprintf(out, n, "%lu.%04lu", x / 10000ul, x % 10000ul);
}

static void print_vpos_channel(uint8_t ch)
{
    uint8_t bank = 0, range = 0;
    uint16_t code = 0;
    /* The record median is post FPGA_ADC_OFFSET (raw - 28, clamped at 0), so
     * raw ~= median + 28; a quiet input at pos p should sit at raw ~128 + p. */
    const uint32_t med = scope_center_buf_median(ch);
    const uint16_t hw = (ch == 2u) ? scope_trigger_ch2_last() : scope_trigger_dac_last();
    if (scope_vpos_last(ch, &bank, &range, &code))
        usb_debug_printf("CH%u: range %u pos %d -> %s %u (bank %u)  ref now %u  "
                         "rec median %lu (raw ~%lu)\r\n",
                         (unsigned)ch, (unsigned)range, scope_vpos_get(ch),
                         ch == 2u ? "TMR13_C1DT" : "DAC1", (unsigned)code, (unsigned)bank,
                         (unsigned)hw, (unsigned long)med, (unsigned long)(med + 28u));
    else
        usb_debug_printf("CH%u: pos %d, not written from the page  ref now %u  "
                         "rec median %lu (raw ~%lu)\r\n",
                         (unsigned)ch, scope_vpos_get(ch), (unsigned)hw,
                         (unsigned long)med, (unsigned long)(med + 28u));
}

static void cmd_cal_page_calc(const char *args)
{
    char *end;
    long v[4];
    for (int i = 0; i < 4; i++) {
        while (*args == ' ') args++;
        v[i] = strtol(args, &end, 10);
        if (end == args) {
            usb_send_str("Usage: cal page calc <1|2> <bank 0-2> <range 0-9> <pos -100..100>\r\n");
            return;
        }
        args = end;
    }
    if ((v[0] != 1 && v[0] != 2) || v[1] < 0 || v[1] > 2 || v[2] < 0 || v[2] > 9) {
        usb_send_str("Usage: cal page calc <1|2> <bank 0-2> <range 0-9> <pos -100..100>\r\n");
        return;
    }
    calpage_t p;
    calpage_parse((const uint8_t *)CAL_BACKUP_SRC_ADDR, CALPAGE_LEN, &p);
    const uint8_t c = (uint8_t)(v[0] - 1), b = (uint8_t)v[1], r = (uint8_t)v[2];
    const int pos = calpage_clamp_pos((int)v[3]);
    /* What stock writes for the entry AS STORED: CH1 low 12 bits (bfi at
     * 0x08008A44), CH2 the full value into TMR13 C1DT. */
    const uint32_t st = calpage_stock_formula(p.raw[c][0][b][r], p.raw[c][1][b][r], pos);
    uint16_t ours = 0;
    const bool ok = calpage_dac_code(&p, c, b, r, pos, &ours);
    usb_debug_printf("CH%ld b%ld r%ld pos %d: stock %lu%s  ours ", v[0], v[1], v[2], pos,
                     (unsigned long)(c == 0u ? (st & 0x0FFFu) : st),
                     c == 0u ? " (12-bit cut)" : " (unmasked)");
    if (ok)
        usb_debug_printf("%u (pair from bank %d)\r\n", (unsigned)ours, (int)p.src_bank[c][b][r]);
    else
        usb_debug_printf("none (%s)\r\n", calpage_usable(&p) ? "no clean bank" : calpage_status_str(p.status));
}

static void cmd_cal_page(const char *args)
{
    while (args && *args == ' ') args++;
    if (args && strncmp(args, "calc", 4) == 0) { cmd_cal_page_calc(args + 4); return; }
    if (args && *args) {
        if (strcmp(args, "on") == 0) {
            if (!scope_vpos_enable(true)) usb_send_str("refused: page not usable\r\n");
        } else if (strcmp(args, "off") == 0) {
            (void)scope_vpos_enable(false);
        } else if (strcmp(args, "reload") == 0) {
            scope_vpos_init();
        } else {
            usb_send_str("Usage: cal page [on|off|reload] | cal page calc <1|2> <bank> <range> <pos>\r\n");
            return;
        }
    }

    calpage_t p;   /* ~440 B on this task's 3 KB stack */
    calpage_parse((const uint8_t *)CAL_BACKUP_SRC_ADDR, CALPAGE_LEN, &p);
    usb_debug_printf("cal page @0x%08lX: %s, %s  sig 0x%02X  sentinel 0x%04X\r\n",
                     (unsigned long)CAL_BACKUP_SRC_ADDR, calpage_status_str(p.status),
                     scope_vpos_active() ? "IN USE" : "not in use",
                     (unsigned)p.signature, (unsigned)p.sentinel);
    char g1[24], g2[24];
    fmt_g(g1, sizeof(g1), scope_cal_stock_gain(1u));
    fmt_g(g2, sizeof(g2), scope_cal_stock_gain(2u));
    usb_debug_printf("gain g=(m-28)/150: CH1 m=%u g=%s  CH2 m=%u g=%s\r\n",
                     (unsigned)p.m[0], g1, (unsigned)p.m[1], g2);
    usb_debug_printf("page header (stock UI state, not used): pos %d/%d mask %u tb 0x%02X\r\n",
                     (int)p.hdr_pos[0], (int)p.hdr_pos[1], (unsigned)p.hdr_mask, (unsigned)p.hdr_tb);
    const uint8_t tb = fpga_acq_rate_idx_get(), mask = fpga.channel_mask;
    usb_debug_printf("in force: tb 0x%02X mask %u -> bank %u (tb>=5: 0; tb==4 or mask 3: 1; else 2)\r\n",
                     (unsigned)tb, (unsigned)mask, (unsigned)calpage_bank_select(tb, mask));
    print_vpos_channel(1u);
    print_vpos_channel(2u);
    if (p.status == CALPAGE_ST_SIGNATURE || p.status == CALPAGE_ST_SHORT ||
        p.status == CALPAGE_ST_NONE)
        return;

    usb_debug_printf("clean pairs %u/60, substituted %u, unusable %u\r\n",
                     (unsigned)p.n_clean_pairs, (unsigned)p.n_substituted, (unsigned)p.n_unusable);
    for (uint8_t c = 0; c < CALPAGE_CHANNELS; c++)
        for (uint8_t b = 0; b < CALPAGE_BANKS; b++)
            for (uint8_t r = 0; r < CALPAGE_RANGES; r++) {
                const int8_t sb = p.src_bank[c][b][r];
                if (sb == (int8_t)b) continue;
                const uint8_t f = p.flags[c][0][b][r] | p.flags[c][1][b][r];
                usb_debug_printf("  CH%u b%u r%u: base %u upper %u [%s%s%s] -> ",
                                 (unsigned)(c + 1u), (unsigned)b, (unsigned)r,
                                 (unsigned)p.raw[c][0][b][r], (unsigned)p.raw[c][1][b][r],
                                 (f & CALPAGE_F_RANGE) ? " >4095/blank" : "",
                                 (f & CALPAGE_F_SPAN)  ? " span" : "",
                                 (f & CALPAGE_F_XBANK) ? " cross-bank" : "");
                if (sb < 0) usb_send_str("NO clean bank: page not used for this range\r\n");
                else        usb_debug_printf("bank %d used\r\n", (int)sb);
            }
    usb_send_str("table ('!' >4095/blank, '?' span or cross-bank outlier):\r\n");
    for (uint8_t c = 0; c < CALPAGE_CHANNELS; c++)
        for (uint8_t e = 0; e < CALPAGE_ENDPOINTS; e++)
            for (uint8_t b = 0; b < CALPAGE_BANKS; b++) {
                char line[96];
                int n = snprintf(line, sizeof(line), "CH%u %-5s b%u:", (unsigned)(c + 1u),
                                 e ? "upper" : "base", (unsigned)b);
                for (uint8_t r = 0; r < CALPAGE_RANGES && n > 0 && n < (int)sizeof(line); r++) {
                    const uint8_t f = p.flags[c][e][b][r];
                    n += snprintf(line + n, sizeof(line) - (size_t)n, "%6u%c",
                                  (unsigned)p.raw[c][e][b][r],
                                  (f & CALPAGE_F_RANGE) ? '!' : (f ? '?' : ' '));
                }
                usb_debug_printf("%s\r\n", line);
            }
}

/* ---- autoset / baseline correction / 50 % centre (parity C9/C10) --------
 * The shell only queues a run; the key task's service does it, exactly as
 * for the AUTO / long AUTO / long OK keys (scope_selfcal_run.h). Poll with
 * `fpga scope selfcal`. Nothing here writes flash. */
static const char *selfcal_kind_str(scr_kind_t k)
{
    return k == SCR_AUTOSET ? "autoset" : k == SCR_BASECAL ? "baseline" : "idle";
}

static void cmd_fpga_scope_autoset(void)
{
    if (current_mode != MODE_OSCILLOSCOPE) { usb_send_str("refused: not in scope mode\r\n"); return; }
    usb_send_str(scope_selfcal_request_autoset()
                 ? "autoset queued (poll: fpga scope selfcal)\r\n" : "busy\r\n");
}

/* FFT AUTO as the AUTO key asks for it in a spectrum view (fft.h): the
 * display task fits the span to the record of its next spectrum frame. */
static void cmd_fft_auto(void)
{
#if FEATURE_FFT
    extern void fft_auto_request(void);
    extern bool fft_is_initialized(void);
    if (!fft_is_initialized()) { usb_send_str("refused: no spectrum view (set view.mode fft|split|waterfall)\r\n"); return; }
    fft_auto_request();
    usb_send_str("fft auto requested (next spectrum frame)\r\n");
#else
    usb_send_str("refused: not in this build\r\n");
#endif
}

static void cmd_fpga_scope_basecal(const char *args)
{
    while (*args == ' ') args++;
    uint32_t banks = 3u;
    if (*args && (parse_hex32(args, &banks) != 0 || banks == 0u || banks > 7u)) {
        usb_send_str("usage: fpga scope basecal [banks 1-7, bit b = bank b; default 3]\r\n");
        return;
    }
    if (current_mode != MODE_OSCILLOSCOPE) { usb_send_str("refused: not in scope mode\r\n"); return; }
    if (!scope_vpos_active()) { usb_send_str("refused: calibration page not in use (cal page)\r\n"); return; }
    if (!scope_selfcal_request_basecal((uint8_t)banks)) { usb_send_str("busy\r\n"); return; }
    usb_debug_printf("baseline correction queued, banks 0x%lX; input must be open/grounded "
                     "(poll: fpga scope selfcal; cancel: fpga scope selfcal cancel)\r\n",
                     (unsigned long)banks);
}

static void cmd_fpga_scope_center50(void)
{
    if (current_mode != MODE_OSCILLOSCOPE) { usb_send_str("refused: not in scope mode\r\n"); return; }
    if (!scope_selfcal_request_center50()) { usb_send_str("busy\r\n"); return; }
    vTaskDelay(pdMS_TO_TICKS(250));   /* the key task acts within 100 ms */
    const scope_state_t *ss = scope_state_get();
    usb_debug_printf("50%% centre: pos CH1 %d CH2 %d, trig_x %d, level %d (code %u)\r\n",
                     (int)ss->ch1.position, (int)ss->ch2.position, (int)ss->trig_x,
                     (int)ss->trigger.level, (unsigned)fpga_acq_trig_code_get());
}

static void cmd_fpga_scope_selfcal(const char *args)
{
    while (*args == ' ') args++;
    if (strncmp(args, "cancel", 6) == 0) {
        scope_selfcal_request_cancel();
        vTaskDelay(pdMS_TO_TICKS(250));
    } else if (*args) {
        usb_send_str("usage: fpga scope selfcal [cancel]\r\n");
        return;
    }
    scope_selfcal_status_t st;
    scope_selfcal_get_status(&st);
    const scope_state_t *ss = scope_state_get();
    usb_debug_printf("selfcal: %s", selfcal_kind_str(st.kind));
    if (st.kind == SCR_BASECAL)
        usb_debug_printf(" %s b%u r%u", st.prechecking ? "pre-check" : "walk",
                         (unsigned)st.bank, (unsigned)st.range);
    usb_debug_printf("  last: %s %s  records %lu  timeouts %lu  torn %lu\r\n",
                     selfcal_kind_str(st.last_kind), st.last_result,
                     (unsigned long)st.records, (unsigned long)st.timeouts,
                     (unsigned long)st.torn);
    if (st.kind == SCR_BASECAL || st.last_kind == SCR_BASECAL)
        usb_debug_printf("baseline: %u ok %u fail (last fail: CH%u b%u r%u %s)\r\n",
                         (unsigned)st.n_ok, (unsigned)st.n_fail, (unsigned)st.last_fail_ch + 1u,
                         (unsigned)st.last_fail_bank, (unsigned)st.last_fail_range,
                         st.last_fail ? st.last_fail : "-");
    if (st.kind == SCR_IDLE && st.last_kind == SCR_BASECAL) {
        if (st.open_mask)
            usb_debug_printf("baseline: input open/noisy on%s%s (refused, not corrected)\r\n",
                             (st.open_mask & 1u) ? " CH1" : "", (st.open_mask & 2u) ? " CH2" : "");
        /* One line per refused entry (<= 60), as `ch b r: reason`; corrected ones are counted. */
        char why[96];
        for (unsigned ch = 0; ch < 2u; ch++) {
            unsigned okn = 0, bad = 0;
            for (unsigned bk = 0; bk < 3u; bk++)
                for (unsigned r = 0; r < 10u; r++) {
                    uint8_t es = 0;
                    if (!scope_selfcal_basecal_entry((uint8_t)ch, (uint8_t)bk, (uint8_t)r,
                                                     &es, why, sizeof(why)) || es == 0u) continue;
                    if (es == 1u) { okn++; continue; }
                    bad++;
                    usb_debug_printf("  refused CH%u b%u r%u: %s\r\n", ch + 1u, bk, r, why);
                }
            usb_debug_printf("  CH%u: %u corrected, %u refused\r\n", ch + 1u, okn, bad);
        }
    }
    usb_debug_printf("corrections in RAM: %u   now: CH1 r%u pos %d  CH2 r%u pos %d  tb 0x%02X  "
                     "trig %s src CH%u level %d\r\n",
                     (unsigned)st.corr_count, (unsigned)ss->ch1.vdiv_idx, (int)ss->ch1.position,
                     (unsigned)ss->ch2.vdiv_idx, (int)ss->ch2.position, (unsigned)ss->timebase_idx,
                     trigger_mode_labels[ss->trigger.mode],
                     ss->trigger.source == TRIG_SRC_CH2 ? 2u : 1u, (int)ss->trigger.level);
}

/* `cal base [clear]`: the baseline correction's RAM deltas on the page pair. */
static void cmd_cal_base(const char *args)
{
    while (*args == ' ') args++;
    if (strcmp(args, "clear") == 0) {
        if (scope_selfcal_busy()) { usb_send_str("busy\r\n"); return; }
        scope_vpos_corr_clear();
        usb_send_str("corrections cleared (page values in force)\r\n");
        return;
    }
    if (*args) { usb_send_str("usage: cal base [clear]\r\n"); return; }
    usb_debug_printf("baseline corrections in RAM: %u (`cal user save` keeps them across power-off)\r\n",
                     scope_vpos_corr_count());
    for (uint8_t ch = 1; ch <= 2u; ch++)
        for (uint8_t b = 0; b < CALPAGE_BANKS; b++)
            for (uint8_t r = 0; r < CALPAGE_RANGES; r++) {
                int16_t db, du;
                uint16_t pb, pu;
                if (!scope_vpos_corr_get(ch, b, r, &db, &du)) continue;
                const bool pair = scope_vpos_page_pair(ch, b, r, &pb, &pu);
                usb_debug_printf("  CH%u b%u r%u: base %+d upper %+d", (unsigned)ch, (unsigned)b,
                                 (unsigned)r, (int)db, (int)du);
                if (pair)
                    usb_debug_printf("  -> %u/%u (page %u/%u)", (unsigned)(pb + db),
                                     (unsigned)(pu + du), (unsigned)pb, (unsigned)pu);
                usb_send_str("\r\n");
            }
}

/* `cal user [show|save|clear]` — OpenScope's own corrections in the W25Q
 * (usercal.h, FLASH_REGION_USER_CAL 0xF00000): the baseline deltas (long
 * AUTO / `fpga scope basecal`) and the interleave B correction (`fpga
 * interleave cal`). Owner policy: nothing is saved unless asked here (or by
 * a UI confirm); the stock page is never written. `clear` appends an empty
 * record (no erase) and drops the RAM corrections too, so the live state and
 * the next boot agree; `cal base clear` is RAM only. */
static void cmd_cal_user(const char *args)
{
    while (*args == ' ') args++;
    const usercal_report_t *r = usercal_report();
    if (strcmp(args, "save") == 0 || strcmp(args, "clear") == 0) {
        if (scope_selfcal_busy()) { usb_send_str("busy\r\n"); return; }
        const bool clr = (args[0] == 'c');
        const flash_region_status_t st = clr ? usercal_clear_saved() : usercal_save();
        if (st != FLASH_REGION_OK) {
            usb_debug_printf("cal user %s: FAILED, %s; nothing changed\r\n", args, flash_region_strerror(st));
            return;
        }
        if (clr) usb_send_str("cal user clear: empty record saved; RAM corrections dropped (page values in force)\r\n");
        else usb_debug_printf("cal user save: saved %u baseline deltas\r\n", scope_vpos_corr_count());
        /* fall through to the status below */
    } else if (*args && strcmp(args, "show") != 0) {
        usb_send_str("usage: cal user [show|save|clear]\r\n");
        return;
    }
    usercal_t u;   /* ~290 B on this task's 3 KB stack */
    const usercal_status_t st = usercal_store_load(&u);
    const uint32_t live = usercal_live_table_crc();
    uint32_t used = 0, nrec = 0;
    (void)flash_region_log_info(USERCAL_REGION, &used, NULL, &nrec);
    usb_debug_printf("user cal @0x%06lX: %s; log %lu B, %lu records\r\n",
                     (unsigned long)flash_region_get(USERCAL_REGION)->start, usercal_status_str(st),
                     (unsigned long)used, (unsigned long)nrec);
    if (st == USERCAL_ST_OK) {
        usb_debug_printf("  saved: %u baseline deltas, table %08lX (%s live %08lX); interleave %s",
                         usercal_count(&u), (unsigned long)u.table_crc,
                         u.table_crc == live ? "matches" : "STALE vs", (unsigned long)live,
                         u.il_present ? "gain " : "none");
        if (u.il_present)
            usb_debug_printf("%ld/1000000 offset %ld/10000", (long)u.il_gain_ppm, (long)u.il_off_e4);
        usb_debug_printf(", %u knots, skew %ld ppm\r\n", (unsigned)u.il_n_knots, (long)u.il_skew_ppm);
    }
    usb_debug_printf("  boot: %s", usercal_status_str(r->load_st));
    if (r->loaded)
        usb_debug_printf(", %u applied, %u not applied%s, interleave %s",
                         (unsigned)r->applied, (unsigned)r->refused,
                         r->table_match ? "" : " (page table changed since the save)",
                         r->il_applied ? "applied" : "none");
    usb_debug_printf("\r\n  live: %u baseline deltas in RAM\r\n", scope_vpos_corr_count());
}

static void cmd_reboot_bootloader(void)
{
    usb_send_str("Rebooting to bootloader...\r\n");
    usb_delay_ms(20);
    dfu_request_reboot();
}

/* One scratch buffer shared by the shell's bus-snapshot commands (`spi3 read`
 * uses the first half; `spi3 frame` uses both halves for a coherent CH1+CH2
 * grab). They all run on the single shell task and never nest, so sharing is
 * safe — and it keeps guest RAM in budget: two per-command 1 KB buffers
 * overflowed the region by 368 bytes (2ff992c, caught once #31 unblocked the
 * guest link). One 2 KB buffer costs +1 KB over the old lone snapshot. */
static uint8_t shell_bus_scratch[2 * FPGA_ADC_BUF_SIZE] __attribute__((aligned(4)));   /* declared above */

/* For the feature shell files (shell_internal.h); shell_bus_scratch unchanged. */
uint8_t *usb_debug_sh_scratch(void)
{
    return shell_bus_scratch;
}

static const char *meter_layout_name(uint8_t layout)
{
    switch (layout) {
    case METER_LAYOUT_BIG:    return "big";
    case METER_LAYOUT_CHART:  return "graph";
    case METER_LAYOUT_STATS:  return "stats";
    case METER_LAYOUT_FUSE:   return "fuse";
    case METER_LAYOUT_LIMITS: return "limits";
    default:                 return "?";
    }
}

/* For the feature shell files (shell_internal.h); meter_layout_name() unchanged. */
const char *usb_debug_sh_meter_layout_name(uint8_t layout)
{
    return meter_layout_name(layout);
}

/* The meter wave snapshot lives in the shell's bus scratch: both belong to
 * the shell task, which runs one command at a time, and `meter wave` is the
 * only user of the snapshot. Saves its ~930 B of static RAM (the warm-handoff
 * bench build with the CAN analyser overflowed RAM by 48 B). */
_Static_assert(sizeof(meter_voltage_wave_snapshot_t) <= sizeof(shell_bus_scratch),
               "meter wave snapshot must fit the shell bus scratch");

/* persist [on|off]: whether shell commands save the settings they change.
 * Off is for bench sweeps (one settings record per changed command otherwise);
 * RAM only, on at every boot. Key presses save as usual either way. */
static void cmd_persist(const char *args)
{
    while (*args == ' ') args++;
    if (strncmp(args, "off", 3) == 0) {
        shell_persist_set(false);
        usb_debug_printf("persist off: shell changes stay unsaved until 'persist on' or a key press (back on at boot)\r\n");
        return;
    }
    if (strncmp(args, "on", 2) == 0) {
        const bool was_on = shell_persist_enabled();
        const uint32_t skipped = shell_persist_skipped();
        shell_persist_set(true);
        settings_flush_outcome_t o;
        if (input_settings_flush_sync(500u, &o))
            usb_debug_printf("persist on (%lu save requests were skipped; now %s)\r\n",
                             was_on ? 0ul : (unsigned long)skipped, settings_flush_outcome_text(o));
        else
            usb_debug_printf("persist on (save request timed out; the input task will save within 100 ms)\r\n");
        return;
    }
    usb_debug_printf("persist %s", shell_persist_enabled() ? "on" : "off");
    if (!shell_persist_enabled())
        usb_debug_printf(" (%lu save requests skipped)", (unsigned long)shell_persist_skipped());
    usb_debug_printf("\r\n");
}

/* The 1 s timer hook of the bench pin log (`pinlog`, shell_bench.c): a no-op
 * in an image without the bench shell. */
void usb_debug_pinlog_tick(void)
{
    benchshell_pinlog_tick();
}

/* batt: the gauge's inputs and outputs in one line. */
static void cmd_batt(const char *args)
{
    (void)args;
    usb_debug_printf("batt: raw %u mV, rest %u mV, %u%%, external %u (PC7=%u), critical %u\r\n",
                     (unsigned)battery_raw_mv(), (unsigned)battery_read_mv(), (unsigned)battery_percent(),
                     (unsigned)battery_is_charging(), (unsigned)((GPIOC->idt >> 7) & 1u),
                     (unsigned)battery_is_critical());
}

static void cmd_meter_auto_async(const char *args)
{
    meter_autoselect_status_t st;

    while (args && *args == ' ') args++;
    if (args == NULL || *args == '\0' || strcmp(args, "start") == 0) {
        if (meter_autoselect_start(700U)) {
            usb_send_str("meter auto started settle_ms=700\r\n");
        } else {
            usb_send_str("meter auto start failed\r\n");
        }
    } else if (strncmp(args, "start ", 6) == 0) {
        uint32_t settle_ms;
        if (parse_int(args + 6, &settle_ms) != 0 || settle_ms > 3000U) {
            usb_send_str("Usage: meter auto [start [settle_ms<=3000]|status|cancel]\r\n");
            return;
        }
        if (meter_autoselect_start(settle_ms)) {
            usb_debug_printf("meter auto started settle_ms=%lu\r\n", settle_ms);
        } else {
            usb_send_str("meter auto start failed\r\n");
        }
    } else if (strcmp(args, "status") == 0) {
        /* handled below */
    } else if (strcmp(args, "cancel") == 0) {
        meter_autoselect_cancel();
        usb_send_str("meter auto cancel requested\r\n");
    } else {
        usb_send_str("Usage: meter auto [start [settle_ms<=3000]|status|cancel]\r\n");
        return;
    }

    meter_autoselect_get_status(&st);
    usb_debug_printf("meter auto state=%s current=%u (%s) index=%u/%u "
                     "best=%u (%s) best_score=%u last_score=%u "
                     "settle_ms=%lu wait_budget_ms=%lu cancel=%u\r\n",
                     meter_autoselect_state_name(st.state),
                     (unsigned)st.current_submode,
                     meter_submode_name(st.current_submode),
                     (unsigned)st.current_index,
                     (unsigned)st.candidate_count,
                     (unsigned)st.best_submode,
                     meter_submode_name(st.best_submode),
                     (unsigned)st.best_score,
                     (unsigned)st.last_score,
                     st.settle_ms,
                     st.wait_budget_ms,
                     st.cancel_pending ? 1U : 0U);
}

static void cmd_ui_dump(void)
{
    meter_autoselect_status_t auto_st;
    meter_autoselect_get_status(&auto_st);

    usb_debug_printf("mode=%lu startup=%s meter_submode=%u (%s) layout=%u (%s) "
                     "settings_depth=%d selected=%d sub_selected=%d\r\n",
                     (uint32_t)current_mode,
                     startup_mode_name(startup_mode),
                     (unsigned)meter_submode,
                     meter_submode_name(meter_submode),
                     (unsigned)meter_layout,
                     meter_layout_name(meter_layout),
                     (int)settings_screen_view().page,
                     (int)settings_screen_view().row,
                     (int)settings_screen_view().sub);
    {
        extern volatile uint32_t ui_scope_full_draws, ui_scope_live_draws, ui_redraw_all_count;
        usb_debug_printf("draws scope_full=%lu scope_live=%lu redraw_all=%lu\r\n",
                         (unsigned long)ui_scope_full_draws, (unsigned long)ui_scope_live_draws,
                         (unsigned long)ui_redraw_all_count);
        usb_debug_printf("lcd foreign_calls=%lu\r\n", (unsigned long)lcd_foreign_calls);
#if FEATURE_FFT
        static const char *const wfs[] = { "never drawn", "drawn", "no FFT", "waiting for capture",
                                           "pool not FFT's" };
        const uint8_t w = scope_ui_waterfall_status();
        usb_debug_printf("pool size=%lu owner=%s transitions=%lu | waterfall rows %u x %u at %lu..%lu "
                         "(%s), last frame: %s, newest row %u\r\n",
                         (unsigned long)shared_mem_size(), shared_mem_owner_name(),
                         (unsigned long)shared_mem_transition_count(),
                         (unsigned)SHMEM_FFT_WATERFALL_ROWS, (unsigned)SHMEM_FFT_WATERFALL_COLS,
                         (unsigned long)SHMEM_FFT_WATERFALL_OFFSET,
                         (unsigned long)(SHMEM_FFT_WATERFALL_OFFSET + SHMEM_FFT_WATERFALL_SIZE),
                         (SHMEM_FFT_WATERFALL_OFFSET + SHMEM_FFT_WATERFALL_SIZE <= shared_mem_size())
                             ? "fits" : "DOES NOT FIT",
                         (w < 5u) ? wfs[w] : "?", (unsigned)scope_ui_waterfall_row());
        /* The transform's own setup (the CMSIS tables are selected at build
         * time for the one size): a refusal must be seen, not drawn. */
        usb_debug_printf("fft engine: %s\r\n",
                         !fft_is_initialized() ? "not initialized (enter the FFT view)"
                         : fft_engine_ok() ? "ok" : "INIT FAILED (no tables for this FFT size)");
#endif
    }
    usb_debug_printf("meter_ui draws=%lu full_clears=%lu partial_clears=%lu draw_us=%lu max_draw_us=%lu over_budget=%lu rendered_update=%lu last_full=%u live=%u continuity_flash=%u reading_valid=%u "
                     "reading_submode=%u updates=%lu display_updates=%lu\r\n",
                     meter_screen_draw_count,
                     meter_screen_full_clear_count,
                     meter_screen_partial_clear_count,
                     meter_screen_last_draw_us,
                     meter_screen_max_draw_us,
                     meter_screen_over_budget_count,
                     meter_screen_last_reading_display_update,
                     (unsigned)meter_screen_last_full_clear,
                     (unsigned)meter_screen_last_live,
                     (unsigned)meter_screen_last_continuity_flash,
                     meter_reading.valid ? 1U : 0U,
                     (unsigned)meter_reading.submode,
                     meter_reading.update_count,
                     meter_reading.display_update_count);
    usb_debug_printf("autoselect state=%s current=%u (%s) index=%u/%u best=%u (%s) best_score=%u last_score=%u cancel=%u\r\n",
                     meter_autoselect_state_name(auto_st.state),
                     (unsigned)auto_st.current_submode,
                     meter_submode_name(auto_st.current_submode),
                     (unsigned)auto_st.current_index,
                     (unsigned)auto_st.candidate_count,
                     (unsigned)auto_st.best_submode,
                     meter_submode_name(auto_st.best_submode),
                     (unsigned)auto_st.best_score,
                     (unsigned)auto_st.last_score,
                     auto_st.cancel_pending ? 1U : 0U);
    /* What the strip and the graticule do with the properties (chN.meas,
     * view.meashide, view.truescale), and the key timing: the read-only
     * lines of the retired `fpga scope prm`, `fpga scope graticule`, `keys`. */
    prm_print_status();
    {
        const scope_state_t *ss = scope_state_get();
        usb_debug_printf("graticule: %s\r\n",
                         ss->true_scale ? "TRUE SCALE (grid == volts/div)"
                                        : "autofit (grid == position only)");
        for (uint8_t ch = 1u; ch <= 2u; ch++) {
            const uint8_t r = (ch == 1u) ? ss->ch1.vdiv_idx : ss->ch2.vdiv_idx;
            char l[12];
            scope_cal_range_label(ch, r, l, sizeof(l));
            usb_debug_printf("  CH%u rng %u -> %s/div %s\r\n", (unsigned)ch, (unsigned)r, l,
                             scope_cal_true_scale_ok(ch, r) ? "" : "(no cal -> autofit)");
        }
    }
    usb_debug_printf("long press %u ms (%u ticks), short on release for the long-capable keys\r\n",
                     (unsigned)(KEY_LONG_TICKS * 2u), (unsigned)KEY_LONG_TICKS);
}

static void cmd_uptime(void)
{
    extern volatile uint32_t uptime_seconds;
    uint32_t s = uptime_seconds;
    usb_debug_printf("Uptime: %lu:%02lu:%02lu\r\n", s / 3600, (s % 3600) / 60, s % 60);
}

/* Claim the SPI3 bus for a raw shell command (audit 2026-08-20, P0.1).
 * The acquisition task runs at priority 3, ABOVE this shell task at 2, so
 * without a park it preempts any spi3_raw_xfer sequence mid-CS-window and
 * both transfers interleave into garbage — a live source of bad experimental
 * data before 2026-08-20. Used at the DISPATCH level around every command
 * that drives the bus raw; pair with fpga_acq_resume() after the command
 * returns (dispatch-level pairing survives the commands' early returns). */
static bool spi3_shell_claim(void)
{
    if (!fpga_acq_pause()) {
        usb_send_str("ERR: SPI3 busy — acquisition task would not park\r\n");
        return false;
    }
    return true;
}

/* ── siggen — signal generator state + the registers behind it ─────────
 * Bench check for parity G1/G3/G4: every field the stock SWD session read
 * (TMR7 DIV/PR/CTRL1, DAC CTRL both halves, DMA2 ch4, flex select, PA5
 * mode) printed next to what we asked for, so a GEN-lead -> CH2 capture
 * can be tied to the programming. The settings are the gen.* properties
 * (set gen.wave=3 gen.amp=1.5 gen.freq=2000 gen.out=on); `siggen dump`
 * adds the cycle in the DMA buffer. */
static void siggen_print_state(void)
{
    const siggen_config_t *c = siggen_get_config();
    siggen_timing_t t, st;
    bool run = siggen_get_timing(&t);
    uint32_t clk = dac_output_timer_clock_hz();
    bool st_ok = siggen_timing_stock(c->frequency_hz, clk, &st);

    usb_debug_printf("siggen: %s wave=%u(%s) freq=%lu Hz amp=%u.%u V duty=%u%%\r\n",
                     run ? "ON" : (c->output_enabled ? "OFF(freq 0)" : "OFF"),
                     c->waveform, siggen_wave_name(c->waveform),
                     (unsigned long)c->frequency_hz,
                     c->amplitude_dv / 10u, c->amplitude_dv % 10u, c->duty_pct);
    if (run)
        usb_debug_printf("  timing: clk=%lu DIV=%u PR=%u -> %lu.%03lu Hz (stock DIV=%u PR=%u -> %lu.%03lu Hz)\r\n",
                         (unsigned long)clk, t.div, t.pr,
                         (unsigned long)(t.actual_mhz / 1000u), (unsigned long)(t.actual_mhz % 1000u),
                         st.div, st.pr,
                         (unsigned long)(st_ok ? st.actual_mhz / 1000u : 0u),
                         (unsigned long)(st_ok ? st.actual_mhz % 1000u : 0u));
    else
        usb_debug_printf("  timing: clk=%lu (off)\r\n", (unsigned long)clk);

    {
        const siggen_edit_t *ed = siggen_ui_editor();
        usb_debug_printf("  boot=%s  editor=%s digit=%u value=%lu\r\n",
                         c->start_on ? "restore-on" : "off",
                         ed->active ? "ACTIVE" : "idle", (unsigned)ed->digit,
                         (unsigned long)ed->value);
    }

    uint32_t dctrl = DAC->ctrl;
    usb_debug_printf("  TMR7: CTRL1=0x%04lX CTRL2=0x%04lX DIV=%lu PR=%lu CNT=%lu\r\n",
                     (unsigned long)TMR7->ctrl1, (unsigned long)TMR7->ctrl2,
                     (unsigned long)TMR7->div, (unsigned long)TMR7->pr,
                     (unsigned long)TMR7->cval);
    usb_debug_printf("  DAC: CTRL=0x%08lX (ch2 0x%04lX, ch1 0x%04lX) D2DTH12R=%lu D2ODT=%lu D1ODT=%lu\r\n",
                     (unsigned long)dctrl, (unsigned long)(dctrl >> 16),
                     (unsigned long)(dctrl & 0xFFFFu),
                     (unsigned long)(DAC->d2dth12r & 0xFFFu),
                     (unsigned long)(DAC->d2odt & 0xFFFu),
                     (unsigned long)(DAC->d1odt & 0xFFFu));
    usb_debug_printf("  DMA2 ch4: CTRL=0x%04lX DTCNT=%lu PADDR=0x%08lX MADDR=0x%08lX SRCSEL0=0x%08lX SRCSEL1=0x%08lX\r\n",
                     (unsigned long)DMA2_CHANNEL4->ctrl, (unsigned long)DMA2_CHANNEL4->dtcnt,
                     (unsigned long)DMA2_CHANNEL4->paddr, (unsigned long)DMA2_CHANNEL4->maddr,
                     (unsigned long)DMA2->src_sel0, (unsigned long)DMA2->src_sel1);
    usb_debug_printf("  PA5 CFG nibble=0x%lX (0 = analog)  buffer=0x%08lX\r\n",
                     (unsigned long)((GPIOA->cfglr >> 20) & 0xFu),
                     (unsigned long)(uint32_t)dac_output_get_buffer());
    const uint16_t *cy = siggen_get_cycle();
    uint16_t mn = 0xFFFF, mx = 0;
    for (unsigned i = 0; i < SIGGEN_CYCLE_SAMPLES; i++) {
        if (cy[i] < mn) mn = cy[i];
        if (cy[i] > mx) mx = cy[i];
    }
    usb_debug_printf("  cycle: [0]=%u [25]=%u [50]=%u [75]=%u min=%u max=%u\r\n",
                     cy[0], cy[25], cy[50], cy[75], mn, mx);
}

static void cmd_siggen(const char *args)
{
    while (*args == ' ') args++;
    if (strcmp(args, "dump") == 0) {
        const uint16_t *cy = siggen_get_cycle();
        for (unsigned i = 0; i < SIGGEN_CYCLE_SAMPLES; i += 10)
            usb_debug_printf("  %3u: %4u %4u %4u %4u %4u %4u %4u %4u %4u %4u\r\n", i,
                             cy[i], cy[i + 1], cy[i + 2], cy[i + 3], cy[i + 4],
                             cy[i + 5], cy[i + 6], cy[i + 7], cy[i + 8], cy[i + 9]);
    } else if (*args) {
        usb_send_str("usage: siggen [dump]  (no argument: state and registers)\r\n"
                     "  the settings are properties: set gen.wave|gen.freq|gen.amp|gen.duty|gen.out|gen.bootout"
                     " (gen.amp in volts: 1.5)\r\n");
        return;
    }
    siggen_print_state();
}

/* ═══════════════════════════════════════════════════════════════════
 * Shell command table (2026-08-20; audit item 8.9)
 *
 * Replaces the ~110-entry if/strcmp dispatch chain. Each row binds ONE
 * command to its handler, its bus-safety requirement and its help text, so
 * the three can never drift apart again — the P0.1 fix had to hand-wrap
 * eleven dispatch entries in spi3_shell_claim(); here that is one flag.
 *
 * Matching: a row matches when its name is a whole-word prefix of the line
 * (next char is NUL/space/tab). The dispatcher picks the LONGEST matching
 * name, so ordering in this table carries no semantics and near-miss pairs
 * ("meter auto" / "meter autoscan", "trig" / "trig2") cannot shadow each
 * other. Flags:
 *   SC_EXACT    command takes no arguments; trailing text = no match
 *               (falls through to "Unknown command", as before).
 *   SC_NEEDARGS command requires arguments; a bare name prints the row's
 *               help instead of running the handler — this preserves the
 *               old chain's "trailing space required" contract and matters
 *               because parse_int("") SUCCEEDS with 0, so e.g. a bare
 *               "fpga cmd" would otherwise send 0x00 0x00 at the FPGA.
 *   SC_SPI3     handler drives the raw SPI3 bus: the dispatcher parks the
 *               acquisition task around the call (audit P0.1).
 *
 * Adding a command = adding ONE row (CMD_A if the handler takes an args
 * string, CMD_V if it takes void). Help is part of the row; keep the
 * left-aligned name+argspec column at 32 chars like the rows around it.
 * ═══════════════════════════════════════════════════════════════════ */
/* ── fwload / fwstat / fwapply — image loading over this shell ──────────
 * The state machine and every flash touch live in fw_loader.c; these
 * handlers are parsing and printing only. The RX rerouting that feeds the
 * transfer is in vUsbDebugTask below. */

static const char *fwl_err_name(fw_loader_error_t e)
{
    switch (e) {
    case FW_LOADER_ERR_NONE:    return "none";
    case FW_LOADER_ERR_SIZE:    return "size (need even 8192..757760)";
    case FW_LOADER_ERR_FLASH:   return "w25q erase/write/read";
    case FW_LOADER_ERR_CRC:     return "crc mismatch";
    case FW_LOADER_ERR_VECTOR:  return "vector table not app-shaped";
    case FW_LOADER_ERR_TIMEOUT: return "rx went silent; run fwload again";
    case FW_LOADER_ERR_NO_IMAGE: return "slot holds no valid image";
    case FW_LOADER_ERR_NO_BUFFER: return "no scratch attached (shell task bug)";
    }
    return "?";
}

static void fwl_print_status(void)
{
    static const char *names[] = { "IDLE", "RECEIVING", "STAGED", "ERROR" };
    char buf[160];
    snprintf(buf, sizeof(buf),
             "fwload: %s slot=%c %lu/%lu crc=%08lX err=%s\r\n"
             "cache:  A=%lu/%08lX B=%lu/%08lX\r\n",
             names[fw_loader_state()],
             fw_loader_slot() ? 'b' : 'a',
             (unsigned long)fw_loader_bytes(),
             (unsigned long)fw_loader_expected(),
             (unsigned long)fw_loader_crc_announced(),
             fwl_err_name(fw_loader_error()),
             (unsigned long)fw_loader_slot_size(0), (unsigned long)fw_loader_slot_crc(0),
             (unsigned long)fw_loader_slot_size(1), (unsigned long)fw_loader_slot_crc(1));
    usb_send_str(buf);
    const fwl_breadcrumb_t *bc = fw_loader_breadcrumb_last();
    if (bc->present)
        usb_debug_printf("last install: %s (code %u)%s, page 0x%08lX, flash sts 0x%04X\r\n",
                         fw_loader_breadcrumb_name(bc->code), (unsigned)bc->code,
                         bc->spi2_stall ? ", SPI2 reclaim timed out" : "",
                         (unsigned long)bc->addr, (unsigned)bc->sts);
    else
        usb_send_str("last install: no record since power-up (none, or the board lost power)\r\n");
}

static void cmd_fwload(const char *args)
{
    char *end = NULL;
    unsigned long size = strtoul(args, &end, 10);
    char *end2 = NULL;
    bool crc_bad = false;
    unsigned long crc = 0;
    if (end && *end) {
        crc = strtoul(end, &end2, 16);
        /* strtoul() leaves endptr == nptr when it converted nothing, so this
         * separates "crc32 token is not hex" from "crc32 is literally 0" and
         * from "no crc32 given" — all three used to print the same usage
         * line, which sends the operator hunting a syntax error they do not
         * have. */
        if (end2 == end) crc_bad = true;
    }
    uint8_t slot = 1; /* default: slot b — "the other firmware" by custom */
    bool slot_bad = false;
    if (end2 != NULL) {
        while (*end2 == ' ') end2++;
        if (*end2 == 'a' || *end2 == 'A') slot = 0;
        else if (*end2 == 'b' || *end2 == 'B') slot = 1;
        else if (*end2 != '\0') slot_bad = true;
        /* The letter must END the argument. `fwload 8192 DEADBEEF apple`
         * matched 'a', dropped "pple" and armed the raw-byte intake — the
         * same wedge the comment below describes, reached through the
         * recognised-letter door instead of the unrecognised one. */
        if (!slot_bad && *end2 != '\0') {
            const char *tail = end2 + 1;
            while (*tail == ' ') tail++;
            if (*tail != '\0') slot_bad = true;
        }
    }
    /* An argument that is present but is neither a nor b used to fall back to
     * the default and arm the intake anyway: `fwload <size> <crc> c` put the
     * shell into raw mode while the operator was still reading the reply, and
     * the next line they typed went into the image instead of the parser.
     * (Wedged the 2C23T port that way on 2026-08-24 — same parser, same trap.)
     * Refusing costs nothing: cdc_flash.py never sends anything but a or b. */
    if (crc_bad) {
        /* Checked before slot_bad: an unparseable crc32 leaves end2 pointing at
         * that same token, which can then be mis-blamed on the slot letter. */
        usb_send_str("fwload: ERROR crc32 must be hex\r\n");
        return;
    }
    if (slot_bad) {
        usb_send_str("fwload: ERROR slot must be exactly a or b\r\n");
        return;
    }
    if (end == args || crc == 0) {
        usb_send_str("usage: fwload <size bytes, decimal> <crc32 hex> [a|b]\r\n"
                     "       then stream exactly that many raw bytes\r\n");
        return;
    }
    if (!fw_loader_begin((uint32_t)size, (uint32_t)crc, slot)) {
        fwl_print_status();
        return;
    }
    char buf[72];
    snprintf(buf, sizeof(buf), "GO %lu bytes to slot %c, crc %08lX expected\r\n",
             size, slot ? 'b' : 'a', crc);
    usb_send_str(buf);
}

static void cmd_fwstat(void)
{
    fwl_print_status();
}

static void cmd_fwswap(const char *args)
{
    uint8_t slot;
    while (*args == ' ') args++;
    /* The token must be exactly one letter. A first-character match meant
     * `fwswap apple` and `fwswap b0rked` both reached erase+program+system
     * reset — the most destructive command in this shell, accepting the
     * loosest argument in it. */
    const char *sw_tail = (*args != '\0') ? args + 1 : args;
    while (*sw_tail == ' ') sw_tail++;
    if (*sw_tail != '\0') slot = 0xFF;
    else if (*args == 'a' || *args == 'A') slot = 0;
    else if (*args == 'b' || *args == 'B') slot = 1;
    else slot = 0xFF;
    if (slot == 0xFF) {
        usb_send_str("usage: fwswap a|b   (install a cached image, no transfer)\r\n");
        fwl_print_status();
        return;
    }
    usb_send_str("verifying slot, then: erase+program+verify from RAM and\r\n"
                 "SYSTEM RESET into the image. keep USB attached (it carries\r\n"
                 "the rail through the reset). recovery = hold MENU through\r\n"
                 "a pinhole reset (IAP), not MENU+Power.\r\n");
    vTaskDelay(pdMS_TO_TICKS(300));
    if (!fw_loader_install_slot(slot)) {
        fwl_print_status();
    }
}

static void cmd_fwapply(void)
{
    if (fw_loader_state() != FW_LOADER_STAGED) {
        usb_send_str("nothing staged — fwload first (or fwswap a|b)\r\n");
        fwl_print_status();
        return;
    }
    usb_send_str("applying: erase+program+verify from RAM, then SYSTEM RESET\r\n"
                 "into the new image (a clean boot, not a jump). this port\r\n"
                 "drops now. keep USB attached — it carries the rail through\r\n"
                 "the reset. recovery = hold MENU through a pinhole reset\r\n"
                 "(IAP), not MENU+Power.\r\n");
    vTaskDelay(pdMS_TO_TICKS(300));   /* let the goodbye reach the host */
    if (!fw_loader_apply()) {
        fwl_print_status();           /* only reached on a refused apply */
    }
}

/* ── `mask` — waveform pass/fail (docs/specs/scope/mask-pass-fail.md) ──────────
 *
 * Every mutation is a request applied by the acquisition task (scope_mask.h).
 * It is posted on the input task, where the keys and the mask's properties
 * post theirs (model_fw_mask_request, through input_call_sync), so a shell
 * request and a key never interleave; each command then waits for the
 * acknowledgement and SAYS when it did not arrive, instead of reporting the
 * intent as the result. */
typedef struct { uint8_t kind, a, b; uint16_t n; uint32_t seq; } mask_call_t;

static void mask_call_run(void *vc)
{
    mask_call_t *c = (mask_call_t *)vc;
    c->seq = model_fw_mask_request(c->kind, c->a, c->b, c->n);
}

static bool mask_post_wait(scope_mask_req_t k, uint8_t a, uint8_t b, uint16_t n)
{
    mask_call_t c = { (uint8_t)k, a, b, n, 0u };
    if (!input_call_sync(mask_call_run, &c, 1000u)) {
        usb_send_str("NOT POSTED: the input task did not take the request in 1 s "
                     "(gallery / countdown / self-cal?).\r\n");
        return false;
    }
    for (int i = 0; i < 30; i++) {                  /* 400 ms in all with the input task's 100 */
        if (scope_mask_acked(c.seq)) return true;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    usb_send_str("NOT APPLIED: the acquisition loop did not pick the request up in 400 ms\r\n"
                 "(not a live-scope build, scope not initialised, or acq parked).\r\n");
    return false;
}

static const char *mask_state_str(mask_pf_state_t s)
{
    return s == MASK_PF_READY ? "READY" : s == MASK_PF_TEACHING ? "TEACHING" : "EMPTY";
}

static const char *mask_verdict_str(mask_pf_verdict_t v)
{
    return v == MASK_PF_V_PASS ? "PASS" : v == MASK_PF_V_FAIL ? "FAIL" :
           v == MASK_PF_V_SKIP ? "SKIP" : "-";
}

static void mask_print_cond_diff(uint8_t d)
{
    if (d & MASK_PF_C_RANGE1)     usb_send_str(" CH1-range");
    if (d & MASK_PF_C_RANGE2)     usb_send_str(" CH2-range");
    if (d & MASK_PF_C_COUPLING)   usb_send_str(" coupling");
    if (d & MASK_PF_C_TIMEBASE)   usb_send_str(" timebase");
    if (d & MASK_PF_C_TRIG_LEVEL) usb_send_str(" trig-level");
    if (d & MASK_PF_C_TRIG_EDGE)  usb_send_str(" trig-edge");
    if (d & MASK_PF_C_TRIG_SRC)   usb_send_str(" trig-source");
}

static void mask_print_status(void)
{
    scope_mask_summary_t s;
    scope_mask_summary(&s);
    usb_debug_printf("mask %s  chans %s%s  span 320 from anchor-%d  tol v=%u cnt h=%u smp%s\r\n",
                     mask_state_str(s.state), (s.chans & 1u) ? "CH1" : "",
                     (s.chans & 2u) ? ((s.chans & 1u) ? "+CH2" : "CH2") : "",
                     (int)s.pre, (unsigned)s.tol_v, (unsigned)s.tol_h,
                     s.rail_limited ? "  RAIL-LIMITED" : "");
    if (s.state == MASK_PF_TEACHING || s.teach_rejects) {
        usb_debug_printf("teach %u/%u  rejected %lu", (unsigned)s.teach_got,
                         (unsigned)s.teach_target, (unsigned long)s.teach_rejects);
        if (s.teach_last_reject != MASK_PF_R_OK)
            usb_debug_printf(" (last: %s)", mask_pf_reason_str(s.teach_last_reject));
        usb_send_str("\r\n");
    }
    if (s.state == MASK_PF_READY) {
        usb_debug_printf("taught under: range %u/%u coupling %u/%u tb 0x%02X trig code %u %s src CH%u  spread %u cnt\r\n",
                         s.cond.range[0], s.cond.range[1], s.cond.coupling[0], s.cond.coupling[1],
                         s.cond.timebase, s.cond.trig_code, s.cond.trig_edge ? "falling" : "rising",
                         (unsigned)s.cond.trig_src + 1u, (unsigned)s.spread);
    }
    usb_debug_printf("tested %lu  pass %lu  fail %lu  skipped %lu  missed %lu  max-consec-fail %lu\r\n",
                     (unsigned long)s.tested, (unsigned long)s.passed, (unsigned long)s.failed,
                     (unsigned long)s.skipped, (unsigned long)s.missed,
                     (unsigned long)s.max_consec_fail);
    const mask_pf_t *m = scope_mask_raw();
    if (s.skipped && m != NULL) {
        usb_send_str("skips:");
        for (unsigned r = 1; r < MASK_PF_R_COUNT; r++)
            if (m->skipped[r]) usb_debug_printf(" %s=%lu", mask_pf_reason_str((mask_pf_reason_t)r),
                                                (unsigned long)m->skipped[r]);
        usb_send_str("\r\n");
    }
    usb_debug_printf("last %s gen %lu", mask_verdict_str(s.last.verdict), (unsigned long)s.last.gen);
    if (s.last.verdict == MASK_PF_V_SKIP) {
        usb_debug_printf(" (%s", mask_pf_reason_str(s.last.reason));
        if (s.last.reason == MASK_PF_R_SETTINGS) mask_print_cond_diff(s.last.cond_diff);
        usb_send_str(")");
    }
    usb_send_str("\r\n");
    if (s.failed)
        usb_debug_printf("last FAIL gen %lu: %u samples out, first CH%d col %d, worst %d cnt\r\n",
                         (unsigned long)s.last_fail.gen, (unsigned)s.last_fail.violations,
                         (int)s.last_fail.first_ch + 1, (int)s.last_fail.first_col,
                         (int)s.last_fail.worst);
    usb_debug_printf("stop-on-fail %s%s\r\n", s.stop_on_fail ? "ON" : "OFF",
                     s.hold ? "  -- HOLDING a failed record (mask run / OK to resume)" : "");
    usb_debug_printf("move-focus %s\r\n", key_focus_name((key_focus_t)key_focus_get()));
    usb_debug_printf("heap free %lu (min ever %lu)\r\n", (unsigned long)xPortGetFreeHeapSize(),
                     (unsigned long)xPortGetMinimumEverFreeHeapSize());
}

static void cmd_mask(const char *args)
{
    while (*args == ' ') args++;
    if (*args == 0 || strncmp(args, "status", 6) == 0) {
        mask_print_status();
        return;
    }
    if (strncmp(args, "teach", 5) == 0) {
        const char *p = args + 5;
        uint32_t n = 0;
        uint8_t ch = 0;
        while (*p) {
            while (*p == ' ') p++;
            if (!*p) break;
            if (strncmp(p, "ch1", 3) == 0)       ch = 1;
            else if (strncmp(p, "ch2", 3) == 0)  ch = 2;
            else if (strncmp(p, "both", 4) == 0) ch = 3;
            else if (parse_int(p, &n) != 0 || n == 0 || n > MASK_PF_TEACH_MAX) {
                usb_debug_printf("usage: mask teach [1..%u] [ch1|ch2|both]\r\n", MASK_PF_TEACH_MAX);
                return;
            }
            while (*p && *p != ' ') p++;
        }
        if (!mask_post_wait(SCOPE_MASK_REQ_TEACH, ch, 0, (uint16_t)n)) return;
        const char *why = scope_mask_teach_refusal();
        if (why) { usb_debug_printf("teach REFUSED: %s\r\n", why); return; }
        mask_print_status();
        return;
    }
    if (strncmp(args, "clear", 5) == 0) {
        if (mask_post_wait(SCOPE_MASK_REQ_CLEAR, 0, 0, 0)) usb_send_str("mask cleared\r\n");
        return;
    }
    if (strncmp(args, "reset", 5) == 0) {
        if (mask_post_wait(SCOPE_MASK_REQ_RESET, 0, 0, 0)) mask_print_status();
        return;
    }
    if (strncmp(args, "run", 3) == 0) {
        if (mask_post_wait(SCOPE_MASK_REQ_RELEASE, 0, 0, 0)) usb_send_str("hold released\r\n");
        return;
    }
    if (strncmp(args, "dump", 4) == 0) {
        /* One line per column: col lo hi [lo2 hi2] [*] -- * = outside in the
         * last FAIL. Machine-readable for bench scripts. */
        const mask_pf_t *m = scope_mask_raw();
        if (m == NULL || m->state != MASK_PF_READY) { usb_send_str("no finished mask\r\n"); return; }
        usb_debug_printf("mask dump pre %d chans %u\r\n", (int)m->pre, (unsigned)m->chans);
        for (unsigned i = 0; i < MASK_PF_SPAN; i++) {
            uint8_t l1 = 0, h1 = 0, l2 = 0, h2 = 0;
            (void)mask_pf_bounds(m, 0, (uint16_t)i, &l1, &h1);   /* 0 0 = not in mask */
            (void)mask_pf_bounds(m, 1, (uint16_t)i, &l2, &h2);
            usb_debug_printf("%u %u %u %u %u%s%s\r\n", i, l1, h1, l2, h2,
                             mask_pf_fail_col(m, 0, (uint16_t)i) ? " *1" : "",
                             mask_pf_fail_col(m, 1, (uint16_t)i) ? " *2" : "");
        }
        usb_send_str("end\r\n");
        return;
    }
    usb_send_str("usage: mask [status|teach [n] [ch1|ch2|both]|clear|reset|run|dump]\r\n"
                 "  the tolerance and stop-on-fail are properties: set mask.tolv=<counts> mask.tolh=<samples>; "
                 "set mask.stopfail on|off\r\n");
}

/* ── `tasks`: every FreeRTOS task's stack and the heap ──────────────────
 * One line per task: name, priority, state, stack size and the least stack
 * that was ever left, both in words (4 bytes), and that margin as a share
 * of the stack. The size is read from the task's control block
 * (configRECORD_STACK_HIGH_ADDRESS: the top is aligned down to 8 bytes, so
 * it is the created depth rounded to an even number of words). The key
 * (input) task is the one named "key" (main.c: every key, every screen and
 * every property setter runs on it); its line is marked and repeated at the
 * end. uxTaskGetSystemState suspends the scheduler while it walks the task
 * lists and scans each stack for its untouched fill (not timed on the unit
 * yet: a few thousand byte compares). Lines are in creation order. */
static void cmd_tasks(void)
{
    static const char *const state[] = { "run", "ready", "block", "susp", "del", "?" };
    TaskStatus_t *t = (TaskStatus_t *)(void *)shell_bus_scratch;      /* 4-aligned, shell task only */
    const UBaseType_t max = (UBaseType_t)(sizeof(shell_bus_scratch) / sizeof(TaskStatus_t));
    const UBaseType_t n = uxTaskGetSystemState(t, max, NULL);
    long key_size = -1, key_free = -1;

    if (n == 0u) {
        usb_debug_printf("tasks: more than %u tasks (%u)\r\n", (unsigned)max,
                         (unsigned)uxTaskGetNumberOfTasks());
        return;
    }
    usb_debug_printf("tasks: %u (stack and min-free in words of 4 bytes; min-free = the least"
                     " ever left)\r\n", (unsigned)n);
    usb_send_str("  name             prio state  stack min-free  free%\r\n");
    UBaseType_t last = 0;
    for (UBaseType_t k = 0; k < n; k++) {                             /* in creation order */
        UBaseType_t i = n;
        for (UBaseType_t j = 0; j < n; j++)
            if (t[j].xTaskNumber > last && (i == n || t[j].xTaskNumber < t[i].xTaskNumber)) i = j;
        if (i == n) break;
        last = t[i].xTaskNumber;
        const unsigned size = (unsigned)(((uintptr_t)t[i].pxEndOfStack -
                                          (uintptr_t)t[i].pxStackBase) / sizeof(StackType_t) + 2u) & ~1u;
        const unsigned hwm = (unsigned)t[i].usStackHighWaterMark;
        const bool key = strcmp(t[i].pcTaskName, "key") == 0;
        if (key) { key_size = (long)size; key_free = (long)hwm; }
        usb_debug_printf("  %-16s %4u %-5s %6u %8u %5u%%%s\r\n", t[i].pcTaskName,
                         (unsigned)t[i].uxCurrentPriority,
                         state[t[i].eCurrentState <= eDeleted ? t[i].eCurrentState : 5],
                         size, hwm, size ? hwm * 100u / size : 0u,
                         key ? "  <- key (input) task" : "");
    }
    usb_debug_printf("heap: free %lu B, min ever %lu B, of %lu B\r\n",
                     (unsigned long)xPortGetFreeHeapSize(),
                     (unsigned long)xPortGetMinimumEverFreeHeapSize(),
                     (unsigned long)configTOTAL_HEAP_SIZE);
    if (key_size >= 0)
        usb_debug_printf("key (input) task: stack %ld words, min-free %ld words (%ld B)\r\n",
                         key_size, key_free, key_free * 4L);
    else
        usb_send_str("key (input) task: not found (no task named \"key\")\r\n");
}

typedef struct {
    const char *name;                    /* full command word(s) */
    void (*fn_args)(const char *args);   /* exactly one of these two is set */
    void (*fn_void)(void);
    uint8_t flags;
    const char *help;                    /* preformatted line(s); NULL = alias row */
} shell_cmd_t;

#define SC_EXACT    0x01u
#define SC_NEEDARGS 0x02u
#define SC_SPI3     0x04u

#define CMD_A(name, fn, flags, help)  { name, fn, NULL, flags, help }
#define CMD_V(name, fn, flags, help)  { name, NULL, fn, flags, help }

static const shell_cmd_t shell_cmds[] = {
    CMD_V("help", cmd_help, SC_EXACT,
          "help                            Show this help\r\n"),
    CMD_V("?", cmd_help, SC_EXACT,
          NULL /* alias */),
    CMD_V("version", cmd_version, SC_EXACT,
          "version                         Firmware info\r\n"),
    CMD_V("features", cmd_features, SC_EXACT,
          "features                        Features of this build: name, on/off, HELLO bit\r\n"),
    CMD_V("status", cmd_status, SC_EXACT,
          "status                          FPGA & system status, the timebase in force\r\n"),
    CMD_A("fwload", cmd_fwload, SC_NEEDARGS,
          "fwload <size> <crc32hex> [a|b]  Stage an image into W25Q cache slot a/b\r\n"
          "  then stream exactly <size> raw bytes (scripts/cdc_flash.py does both)\r\n"),
    CMD_V("fwstat", cmd_fwstat, SC_EXACT,
          "fwstat                          Staging state / byte count / verdict\r\n"),
    CMD_V("fwapply", cmd_fwapply, SC_EXACT,
          "fwapply                         Install the staged image over the app slot\r\n"),
    CMD_A("fwswap", cmd_fwswap, SC_NEEDARGS,
          "fwswap a|b                      Install a cached image (no transfer needed)\r\n"),
    CMD_A("siggen", cmd_siggen, 0,
          "siggen [dump]                   Signal generator state: gen.*, TMR7/DAC/DMA2 ch4; dump = the cycle\r\n"),
    CMD_V("flash jedec", cmd_flash_jedec, SC_EXACT,
          "flash jedec                     Read external W25Q128 JEDEC ID\r\n"),
    CMD_A("flash read", cmd_flash_read, SC_NEEDARGS,
          "flash read <addr> <len>         Read external flash bytes (max 256)\r\n"),
    CMD_A("flash dump", cmd_flash_dump, SC_NEEDARGS,
          "flash dump <addr> <len>         Stream external flash bytes (max 4096)\r\n"),
    CMD_V("can decode", cmd_can_decode, SC_EXACT,
          "can decode                      Decode CAN in the record (CH1=CAN_H, CH2=CAN_L)\r\n"),
    CMD_A("can stats", cmd_can_stats, 0,
          "can stats [n]                   CAN over n records: IDs, ACK/NAK, CRC\r\n"),
    CMD_A("can screen", cmd_can_screen, 0,
          "can screen [on|off]             CAN readout on the scope screen\r\n"),
    CMD_V("cal status", cmd_cal_status, SC_EXACT,
          "cal status                      Factory-cal page + W25Q backup state\r\n"),
    CMD_V("cal backup", cmd_cal_backup, SC_EXACT,
          "cal backup                      Copy MCU factory-cal page to W25Q (safe)\r\n"),
    CMD_A("cal restore", cmd_cal_restore, SC_NEEDARGS,
          "cal restore [force] CONFIRM     Write W25Q backup back to MCU flash 0x08006000\r\n"),
    CMD_A("cal page", cmd_cal_page, 0,
          "cal page [on|off|reload]        Stock cal page in use: table, bank, substitutions, DAC codes\r\n"
          "cal page calc <1|2> <b> <r> <p> One offset code from the page: stock's vs ours\r\n"),
    CMD_A("cal base", cmd_cal_base, 0,
          "cal base [clear]                Baseline-correction deltas held in RAM (long AUTO)\r\n"),
    CMD_A("cal user", cmd_cal_user, 0,
          "cal user [show|save|clear]      Save/clear OpenScope's own corrections (baseline + interleave) in W25Q\r\n"),
    CMD_V("fpga scope autoset", cmd_fpga_scope_autoset, SC_EXACT,
          "fpga scope autoset              Autoset, as short AUTO (V/div, position, timebase, trigger)\r\n"),
    CMD_V("fft auto", cmd_fft_auto, SC_EXACT,
          "fft auto                        Fit the spectrum's span, as AUTO in a spectrum view\r\n"),
    CMD_A("fpga scope basecal", cmd_fpga_scope_basecal, 0,
          "fpga scope basecal [banks]      Baseline correction, as long AUTO; input open (default banks 3)\r\n"),
    CMD_V("fpga scope center50", cmd_fpga_scope_center50, SC_EXACT,
          "fpga scope center50             50% centre, as long OK (positions, trigger column/level)\r\n"),
    CMD_A("fpga scope selfcal", cmd_fpga_scope_selfcal, 0,
          "fpga scope selfcal [cancel]     Autoset/baseline progress and result\r\n"),
    CMD_A("screen dump", cmd_screen_dump, 0,
          "screen dump [shadow] [x y w h]  Dump text indexed4 LCD shadow\r\n"),
    CMD_A("screen dumpbin", cmd_screen_dumpbin, 0,
          "screen dumpbin [x y w h]        Binary indexed4 LCD shadow dump\r\n"),
    CMD_A("screenshot", cmd_screenshot, 0,
          "screenshot [list]               On-device gallery: <n>.bmp in W25Q scratch\r\n"
          "screenshot save|get|delete|check <n>  Save the screen / raw BMP (SHOTBIN) / delete\r\n"),
    CMD_A("logo", cmd_logo, 0,
          "logo [info]                     Boot logo status (CRC-checked)\r\n"
          "logo put <len> <crc>|blk <off> <n> <crc>|end  Upload 320x240 RGB565 (scripts/logo_put.py)\r\n"
          "logo pos|data <off> <hex>       Upload position / legacy 48-byte hex chunk\r\n"
          "logo show [ms]|clear            Preview now / remove (built-in splash)\r\n"),

    CMD_A("usb", cmd_usb, 0,
          "usb [status]                    USB device mode, switch result, CDC/MSC counters\r\n"
          "usb share [on|off]              Re-enumerate as CDC + read-only gallery drive / CDC only\r\n"
          "usb trace                       Last attach: resets/SETUPs, then the SCSI commands\r\n"),
    CMD_A("screen shadow", cmd_screen_shadow, 0,
          "screen shadow page [y]          Clear full-screen shadow capture\r\n"),
    CMD_A("fpga scope range", cmd_fpga_scope_range, SC_NEEDARGS,
          "fpga scope range <0-9> <1|2|both>  Coarse frontend range; channel is MANDATORY\r\n"),
    CMD_A("fpga scope measure", cmd_fpga_scope_measure, 0,
          "fpga scope measure [reps]       Badge values raw (uV/permille/mHz) for bench validation\r\n"),
    CMD_A("fpga scope cursor", cmd_fpga_scope_cursor, 0,
          "fpga scope cursor               X1 X2 X1-X2 Y1 Y2 Y1-Y2 1/dX as the screen computes them\r\n"),
    CMD_V("settings", cmd_settings, SC_EXACT,
          "settings                        Persistence status: bound, load result, writes, failures\r\n"),
    CMD_A("reset settings", cmd_reset_settings, 0,
          "reset settings CONFIRM          Built-in defaults over the user settings, applied and saved\r\n"
          "  (never calibration, gallery, logo); list changed shows what differs now\r\n"),
    CMD_A("session", cmd_session, 0,
          "session [status]|list|save|clear  Session snapshot (Startup = Restore): what the boot found\r\n"
          "  and applied; save = what a power-off writes; clear = none; list = members and exclusions\r\n"),
    /* The property model's raw route (src/model/model_shell.c): the same
     * setters as the keys, applied by the input task. */
    CMD_A("get", cmd_model_get, SC_NEEDARGS,
          "get <prop|glob> [...] | changed Property values: get ch1.vdiv, get ch1.*, get *; changed =\r\n"
          "  only what is not at its built-in default\r\n"),
    CMD_A("set", cmd_model_set, SC_NEEDARGS,
          "set <prop> <value> | set p=v .. Set through the input task (label, number, step +=N/-=N)\r\n"),
    CMD_A("list", cmd_model_list, 0,
          "list [prefix|glob]              Properties: name, type, range, value, flags\r\n"
          "list changed [prefix|glob]      Values that differ from the built-in defaults, with the default\r\n"),
    CMD_A("describe", cmd_model_describe, SC_NEEDARGS,
          "describe <prop>                 Range/labels, built-in default, unit, saved where, reset, owner\r\n"),
    STREAM_SHELL_ROWS       /* `stream`: from src/stream/stream.h, empty when the feature is off */
    /* The UI route (src/model/ui_shell.c): exact key replay into the key
     * queue, each press waited for until the input task has handled it. */
    CMD_A("ui press", cmd_ui_press, SC_NEEDARGS,
          "ui press <KEY> [n]              Press a key n times (not POWER), wait until handled\r\n"),
    CMD_A("ui long", cmd_ui_long, SC_NEEDARGS,
          "ui long <KEY>                   The long-press event of a key that has one\r\n"),
    CMD_A("ui seq", cmd_ui_seq, SC_NEEDARGS,
          "ui seq <tok> ...                tok = KEY | KEY*n | long:KEY | wait:ms (all parsed first)\r\n"),
    CMD_V("ui screen", cmd_ui_screen, SC_EXACT,
          "ui screen                       Where the UI is: one key=value line for scripts\r\n"),
    CMD_A("mask", cmd_mask, 0,
          "mask [status]                   Waveform pass/fail: state, counts, last verdict\r\n"
          "mask teach [n] [ch1|ch2|both]   Teach from the next n good records (default 8)\r\n"
          "mask clear|reset|run            Drop mask | zero counts | release a fail hold\r\n"
          "mask dump                       Bounds per column (bench scripts)\r\n"),
    CMD_A("fpga edgefilter", cmd_fpga_edgefilter, 0,
          "fpga edgefilter [on|off]        MCU trigger edge filter: keep Rising/Falling records (default on)\r\n"),
    CMD_A("fpga cfgsettle", cmd_fpga_cfgsettle, 0,
          "fpga cfgsettle [ms|off|default] Records only after the capture config settled; drops so far\r\n"),
    CMD_A("fpga interleave", cmd_fpga_interleave, 0,
          "fpga interleave [cal [piecewise]|tone]  Single-channel merge status (il.*); cal = B from a DC sweep (shorted CH1); tone = skew + B gain from a 5-50 MHz tone\r\n"
          "fpga interleave skew measure    B timing skew from a 5-50 MHz tone\r\n"),
    CMD_A("fpga core", cmd_fpga_core, 0,
          "fpga core                       Which capture core runs: tag, protocol, capabilities (its own answer)\r\n"),
    CMD_A("fpga unrotate", cmd_fpga_unrotate, 0,
          "fpga unrotate [on|off]          Time-order strobed records at the seam; trigger at 512 (default on)\r\n"),
    CMD_A("fpga scope trigger", cmd_fpga_scope_trigger, 0,
          "fpga scope trigger              Trigger: trig.* in force, reg 0x06/0x08, status, record kind, where the window landed\r\n"),
    CMD_A("fpga roll", cmd_fpga_roll, 0,
          "fpga roll [code <hex>|code stock]  Roll (tb.roll, tb.rollstep) status: fifo, op03, newest point; code = entry byte\r\n"),
    CMD_A("fpga scope center", cmd_fpga_scope_center, 0,
          "fpga scope center [ch2] [0-9]   Auto-center offset ref per range (CH1/DAC1 default); all if omitted\r\n"),
    CMD_V("meter state", cmd_meter_state, SC_EXACT,
          "meter state                     Decoded frame, range, status text, link, beep, cal guard\r\n"),
    CMD_A("meter relays", cmd_meter_relays, 0,
          "meter relays [on|off]           Meter entry moves the scope's range relays (default off)\r\n"),
    CMD_A("persist", cmd_persist, 0,
          "persist [on|off]                Shell commands save settings (off: bench sweeps, RAM only)\r\n"),
    CMD_A("batt", cmd_batt, 0,
          "batt                            Battery gauge: raw/resting mV, percent, external power\r\n"),
    CMD_A("meter auto", cmd_meter_auto_async, 0,
          "meter auto [start|status|cancel] Async DMM function auto-select\r\n"),
    CMD_V("ui dump", cmd_ui_dump, SC_EXACT,
          "ui dump                         UI state: mode, redraws, meter page, measurement strip, graticule, key timing\r\n"),
    CMD_V("reboot bootloader", cmd_reboot_bootloader, SC_EXACT,
          "reboot bootloader               Reboot into USB HID updater\r\n"),
    CMD_V("uptime", cmd_uptime, SC_EXACT,
          "uptime                          Show uptime\r\n"),
    CMD_V("tasks", cmd_tasks, SC_EXACT,
          "tasks                           FreeRTOS tasks: priority, state, stack size and least free (words); heap\r\n"),
    BENCHSHELL_SHELL_ROWS   /* the bench shell: src/drivers/shell_bench.h, empty when the feature is off */
    METERDEBUG_SHELL_ROWS   /* the meter diagnostics: src/drivers/shell_meterdebug.h, empty when off */
    { NULL, NULL, NULL, 0, NULL }   /* terminator */
};

static void cmd_help(void)
{
    usb_send_str("\r\n=== OpenScope 2C53T Debug Shell ===\r\n");
    for (const shell_cmd_t *c = shell_cmds; c->name != NULL; c++) {
        if (c->help != NULL)
            usb_send_str(c->help);
    }
    usb_send_str("\r\n");
}

static void dispatch_command(char *line)
{
    /* Strip trailing \r\n */
    int len = strlen(line);
    while (len > 0 && (line[len-1] == '\r' || line[len-1] == '\n'))
        line[--len] = '\0';

    if (len == 0) return;

    /* Longest whole-word-prefix match over the table (see table comment). */
    const shell_cmd_t *best = NULL;
    size_t best_len = 0;
    for (const shell_cmd_t *c = shell_cmds; c->name != NULL; c++) {
        size_t n = strlen(c->name);
        if (strncmp(line, c->name, n) != 0)
            continue;
        char next = line[n];
        if (next != '\0' && next != ' ' && next != '\t')
            continue;                     /* word boundary required */
        if ((c->flags & SC_EXACT) && next != '\0')
            continue;                     /* takes no arguments */
        if (n > best_len) {
            best = c;
            best_len = n;
        }
    }

    if (best == NULL) {
        usb_debug_printf("Unknown command: '%s'  (type 'help')\r\n", line);
        return;
    }

    const char *args = line + best_len;
    while (*args == ' ' || *args == '\t') args++;

    if ((best->flags & SC_NEEDARGS) && *args == '\0') {
        usb_send_str("Missing arguments. Usage:\r\n");
        if (best->help != NULL)
            usb_send_str(best->help);
        return;
    }

    if (best->flags & SC_SPI3) {
        if (!spi3_shell_claim())
            return;
        if (best->fn_args) best->fn_args(args); else best->fn_void();
        fpga_acq_resume();
        if (shell_persist_after_command())
            input_request_settings_flush();
        return;
    }

    if (best->fn_args) best->fn_args(args); else best->fn_void();
    /* Any command may have changed a persisted setting (timebase, vdiv,
     * coupling, probe, trigger, siggen, meter mode, prefs, ...) and none of
     * them has a key press behind it, so settings_after_button() never sees
     * it. One request per command, served by the input task within 100 ms: a
     * no-op (no flash access) when nothing differs from the saved image.
     * `persist off` (bench sweeps) skips the request; see shell_persist.h. */
    if (shell_persist_after_command())
        input_request_settings_flush();
}

/* ═══════════════════════════════════════════════════════════════════
 * FreeRTOS Task — USB Debug Shell
 * ═══════════════════════════════════════════════════════════════════ */

#define CMD_BUF_SIZE 128

/* Line accumulator shared by both transports. `echo` is on for USB (a raw
 * serial terminal shows nothing otherwise) and off for RTT, where the host
 * telnet client is line-buffered and echoes locally — echoing there would
 * double every character. */
static char cmd_buf[CMD_BUF_SIZE];
static int  cmd_pos = 0;

/* Returns how many bytes were consumed. Less than len only when a command
 * armed a raw block: the rest of the packet is that block's data.
 *
 * Echo is batched: one packet per received chunk instead of one IN transfer
 * per character. Per-character echo meant ~115 one-byte IN transfers for
 * every 115-byte `logo data` line, interleaved with the host's OUT packets
 * on the same endpoint: slow, and exactly the IN/OUT overlap that triggered
 * the lost-completion race (usb_conf.h). */
static uint16_t shell_feed(const uint8_t *bytes, uint16_t len, bool echo)
{
    uint8_t eb[64];
    uint16_t ne = 0;
#define ECHO_PUT(ch) do { if (ne == sizeof eb) { usb_send_bytes(eb, ne); ne = 0; } eb[ne++] = (uint8_t)(ch); } while (0)
#define ECHO_FLUSH() do { if (ne) { usb_send_bytes(eb, ne); ne = 0; } } while (0)

    for (uint16_t i = 0; i < len; i++) {
        char c = (char)bytes[i];

        if (c == '\r' || c == '\n') {
            if (echo) { ECHO_PUT('\r'); ECHO_PUT('\n'); }
            ECHO_FLUSH();
            cmd_buf[cmd_pos] = '\0';
            if (cmd_pos > 0) {
                dispatch_command(cmd_buf);
            }
            cmd_pos = 0;
            if (raw_block_active(&s_raw)) {
                /* the line ending's second half is not block data */
                if (c == '\r' && i + 1u < len && bytes[i + 1u] == '\n') i++;
                return (uint16_t)(i + 1u);
            }
            usb_send_str("> ");
        } else if (c == '\b' || c == 0x7F) {
            if (cmd_pos > 0) {
                cmd_pos--;
                if (echo) { ECHO_PUT('\b'); ECHO_PUT(' '); ECHO_PUT('\b'); }
            }
        } else if (c >= ' ' && cmd_pos < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_pos++] = c;
            if (echo) ECHO_PUT(c);
        }
    }
    ECHO_FLUSH();
#undef ECHO_PUT
#undef ECHO_FLUSH
    return len;
}

/* All shell input goes through here: a raw block in progress takes its
 * bytes first, the line editor gets the rest. */
static void shell_rx(const uint8_t *buf, uint16_t n, bool echo)
{
    while (n > 0u) {
        if (raw_block_active(&s_raw)) {
            const uint32_t used = raw_block_feed(&s_raw, buf, n, shell_now_ms());
            buf += used;
            n = (uint16_t)(n - used);
            if (!raw_block_active(&s_raw)) logo_blk_finish();
            continue;
        }
        const uint16_t used = shell_feed(buf, n, echo);
        buf += used;
        n = (uint16_t)(n - used);
    }
}

static const char shell_banner[] =
    "\r\n\r\n"
    "+----------------------------------+\r\n"
    "|  OpenScope 2C53T Debug Shell     |\r\n"
    "|  Type 'help' for commands        |\r\n"
    "+----------------------------------+\r\n"
    "\r\n> ";

/* Instrumentation for the 2026-08-11 "shell task never polls RTT" hunt.
 * Non-static so the addresses come out of the ELF with nm and can be read over
 * SWD on a running target — no console needed, which is the whole problem.
 *   dbg_shell_entered == 0  -> the task never ran; look at task creation
 *   dbg_shell_loops   == 0  -> it started but never completed an iteration
 *   dbg_shell_loops growing -> the loop is fine and rtt_read() is the fault */
volatile uint32_t dbg_shell_entered;
volatile uint32_t dbg_shell_loops;
volatile uint32_t dbg_shell_rtt_bytes;

static void vUsbDebugTask(void *pvParameters)
{
    (void)pvParameters;

    dbg_shell_entered = 0xA5A5A5A5u;

    uint8_t rx_buf[USBD_CDC_OUT_MAXPACKET_SIZE];
    uint8_t rtt_buf[64];
    bool usb_banner_sent = false;
    bool rtt_banner_sent = false;
    uint32_t usb_settle = 0;
    /* When a transfer dies mid-stream the host still has bytes in flight, and
     * without this they land in shell_feed() and are parsed as command lines —
     * a binary typed at the prompt. cdc_flash.py stops at the first chunk
     * carrying ERROR, so the tail is short, but any other host makes it long.
     * Swallow exactly what the aborted image still owed, and give up on the
     * wait after the same silence the loader itself uses, so a host that never
     * sends the rest cannot leave the shell deaf to the operator. */
    uint32_t fwl_discard = 0;
    uint32_t fwl_discard_idle = 0;

    /* The firmware loader borrows this task's bus scratch (the arena `spi3
     * read` / `spi3 frame` already share, 545a9d8) instead of owning a 512 B
     * buffer of its own — the RAM ceiling is that close. Safe for the same
     * reason theirs is: every fw_loader entry point runs on this task, and no
     * other command can interleave with a transfer (RX is routed to the
     * loader) or an install (interrupts are off). */
    fw_loader_attach_scratch(shell_bus_scratch, sizeof(shell_bus_scratch));
    fw_loader_breadcrumb_capture();   /* last install's record, read once and cleared */

    for (;;) {
        bool did_work = false;
        dbg_shell_loops++;

#if defined(FAULT_SELFTEST) && FAULT_SELFTEST
        /* Deliberate, self-inflicted fault ~10 s after boot, from a task (so the
         * frame lands on the PSP exactly like the fault under investigation).
         *
         * Two questions at once:
         *  1. does the fault handler actually record anything? If this writes a
         *     valid g_fault, the handler and vector wiring are sound and the
         *     ~55 s fault is special in some way (most likely an unusable stack
         *     frame). If it records nothing, the instrument itself is broken.
         *  2. does the device freeze at ~10 s WITH NO DEBUGGER ATTACHED? The UI
         *     stopping on its own is proof the fault is real and not something
         *     the OpenOCD attach provokes — which is the confound that makes the
         *     whole "~55 s HardFault" reading suspect.
         *
         * udf #0 is the permanently-undefined encoding: guaranteed UNDEFINSTR
         * -> UsageFault, no memory access involved, nothing ambiguous. */
        if (dbg_shell_loops == 1000u) {
            __asm volatile ("udf #0");
        }
#endif

        /* ---- RTT (SWD) transport ---- */
        if (!rtt_banner_sent && rtt_host_attached()) {
            usb_send_str(shell_banner);
            rtt_banner_sent = true;
        }

        size_t rtt_len = rtt_read(rtt_buf, sizeof(rtt_buf));
        dbg_shell_rtt_bytes += (uint32_t)rtt_len;
        if (rtt_len > 0) {
            /* Attaching mid-session: the host may never have moved read_pos
             * before typing, so print the banner on first input too. */
            if (!rtt_banner_sent) {
                usb_send_str(shell_banner);
                rtt_banner_sent = true;
            }
            shell_rx(rtt_buf, (uint16_t)rtt_len, false);
            did_work = true;
        }

        /* ---- USB CDC transport ---- */
#if !DEBUG_SHELL_RTT_ONLY
        if (usb_debug_connected()) {
            if (!usb_banner_sent) {
                /* Let the host finish enumerating before the first write. */
                if (usb_settle < 50) {
                    usb_settle++;
                } else {
                    usb_send_str(shell_banner);
                    usb_banner_sent = true;
                }
            } else {
                /* recover an OUT completion the library lost (see cdc_rx_heal) */
                cdc_rx_heal((cdc_struct_type *)usb_core_dev.class_handler->pdata);
                uint16_t rx_len = usb_vcp_get_rxdata(&usb_core_dev, rx_buf);
                if (rx_len > 0) {
                    if (fw_loader_active()) {
                        /* Raw image bytes for fw_loader — no echo, no line
                         * editor. Progress every 64 KB so a long transfer
                         * is visibly alive without flooding the port. */
                        uint32_t before = fw_loader_bytes();
                        fw_loader_feed(rx_buf, rx_len);
                        if ((fw_loader_bytes() >> 16) != (before >> 16)) {
                            char pbuf[24];
                            snprintf(pbuf, sizeof(pbuf), "..%luK\r\n",
                                     (unsigned long)(fw_loader_bytes() >> 10));
                            usb_send_str(pbuf);
                        }
                        if (!fw_loader_active()) {
                            fwl_print_status();  /* staged or failed — say so */
                            if (fw_loader_state() == FW_LOADER_ERROR) {
                                /* The host has sent `before + rx_len` bytes
                                 * so far — the tail of THIS packet is already
                                 * off the wire even when the loader refused
                                 * it, so `expected - bytes()` would over-
                                 * count and the drain would eat up to a
                                 * packet of the operator's next keystrokes.
                                 * Count what is still in flight instead. */
                                uint32_t sent = before + rx_len;
                                fwl_discard =
                                    sent < fw_loader_expected()
                                        ? fw_loader_expected() - sent
                                        : 0;
                                fwl_discard_idle = 0;
                            }
                        }
                    } else if (fwl_discard > 0) {
                        uint32_t drop = rx_len < fwl_discard ? rx_len
                                                             : fwl_discard;
                        fwl_discard -= drop;
                        fwl_discard_idle = 0;
                        if (fwl_discard == 0) {
                            usb_send_str("fwload: aborted image drained; "
                                         "shell is listening again\r\n");
                        }
                        if (rx_len > drop) {
                            shell_rx(rx_buf + drop,
                                       (uint16_t)(rx_len - drop), true);
                        }
                    } else {
                        shell_rx(rx_buf, rx_len, true);
                    }
                    did_work = true;
                }
            }
        } else {
            usb_banner_sent = false;
            usb_settle = 0;
        }
#else
        (void)rx_buf; (void)usb_banner_sent; (void)usb_settle;
#endif

        /* Age the fw_loader RX-silence timeout, and report if it fires —
         * the poll is what turns a wedged transfer back into a live shell. */
        {
            bool was_loading = fw_loader_active();
            fw_loader_poll();
            if (was_loading && !fw_loader_active()) {
                fwl_print_status();
                /* A timeout kills the transfer too, and a host that merely
                 * stalled (>3 s of silence mid-image) may well resume: arm
                 * the same drain the feed-path error arms, or the rest of
                 * the image lands in shell_feed() as command lines. No
                 * packet is in flight here, so expected - received is the
                 * exact figure. A host that is truly gone costs one more
                 * 3 s drain expiry, which reports itself. */
                if (fw_loader_state() == FW_LOADER_ERROR) {
                    fwl_discard = fw_loader_expected() - fw_loader_bytes();
                    fwl_discard_idle = 0;
                }
            }
        }

        /* The drain waits on the same 3 s of silence the loader does: a host
         * that aborted and walked away must not cost the operator a shell. */
        if (fwl_discard > 0 && !did_work) {
            if (++fwl_discard_idle >= FW_LOADER_TIMEOUT_POLLS) {
                fwl_discard = 0;
                fwl_discard_idle = 0;
                usb_send_str("fwload: stopped waiting for the aborted image; "
                             "shell is listening again\r\n");
            }
        }

        /* A raw `logo blk` block whose host went quiet: abandon it. */
        raw_block_poll(&s_raw, shell_now_ms());
        if (s_raw.state == RAW_TIMEOUT) logo_blk_finish();

#if USB_MSC
        /* Which device we are: switch on request, fall back to CDC-only when
         * the host does not configure the composite device (usb_mode_sm.h). */
        usb_mode_poll();

        /* USB Sharing: serve READ sectors (W25Q reads need this task's
         * context; the USB interrupt only latches them) and pick up gallery
         * changes. Only while the composite device is up: as the CDC device
         * there is no drive to keep current. */
        if (s_usb_mode.active == USB_DEV_COMPOSITE && usb_msc_poll(&usb_core_dev)) did_work = true;
#endif

        /* Binary stream frames, between commands (src/stream/stream.h; a
         * constant false without the stream). */
        if (fwl_discard == 0u && !raw_block_active(&s_raw) && stream_poll()) did_work = true;

        if (!did_work) {
            /* 10 ms idle cadence, 1 ms while an upload is in flight (each
             * `logo blk` round trip would otherwise wait out a poll period
             * twice). */
            const TickType_t idle = (raw_block_active(&s_raw) || s_logo_up.active)
                                        ? 1 : pdMS_TO_TICKS(10);
#if USB_MSC
            /* The MSC interrupt can cut it short (task notification) so a
             * READ does not wait a poll period per sector. Nothing else
             * notifies this task. */
            (void)ulTaskNotifyTake(pdTRUE, idle);
#else
            vTaskDelay(idle);
#endif
        }
    }
}

void usb_debug_create_task(void)
{
#ifndef EMULATOR_BUILD
    TaskHandle_t h = NULL;
    xTaskCreate(vUsbDebugTask, "usb_dbg", 768, NULL, 2, &h);
#if USB_MSC
    usb_msc_set_notify_task(h);
#else
    (void)h;
#endif
#endif
}
