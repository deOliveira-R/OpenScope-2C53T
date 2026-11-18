/*
 * OpenScope 2C53T - the bench shell (FEATURE_BENCHSHELL)
 *
 * The reverse-engineering and bring-up commands of the debug shell: raw FPGA
 * frames and wire words, SPI3, GPIO, USART, memory and W25Q pokes, the stock
 * state shadow, the acquisition experiments (EXP-xx knobs), the pin log. No
 * user function depends on them; an image built without the feature (profile
 * `full`) does not compile this file, `help` does not list the commands and
 * they answer "Unknown command".
 *
 * The handlers were moved here unchanged from usb_debug.c (feature manifest
 * F3). They reach the shell core through shell_internal.h; the rows of the
 * command table are in shell_bench.h.
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
#include "../util/shell_persist.h"
#include "../util/lcd_health.h"
#include "../ui/meter_voltage_wave.h"
#include "../ui/gallery_ui.h"
#include "shared_mem.h"
#include "../util/gallery_store.h"
#include "../ui/logo_ui.h"
#include "watchdog.h"
#include "../util/cal_backup_core.h"
#include "../util/fpga_slot.h"

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
#define SHELL_FEATURE_FILE   /* shell_internal.h: the helper names these handlers were written with */
#include "shell_internal.h"
#include "shell_bench.h"

/* Parse GPIO port letter + pin number, e.g. "A7" "B11" "C6" */
static int parse_gpio(const char *s, gpio_type **port, uint16_t *pin)
{
    char c = s[0];
    if (c >= 'a' && c <= 'e') c -= 32;  /* to upper */

    switch (c) {
        case 'A': *port = GPIOA; break;
        case 'B': *port = GPIOB; break;
        case 'C': *port = GPIOC; break;
        case 'D': *port = GPIOD; break;
        case 'E': *port = GPIOE; break;
        default: return -1;
    }

    uint32_t n;
    if (parse_int(s + 1, &n) != 0 || n > 15) return -1;
    *pin = (uint16_t)(1 << n);
    return 0;
}

typedef struct {
    uint16_t tx_count;
    uint16_t rx_byte_count;
    uint16_t frame_count;
    uint16_t echo_count;
    uint16_t spi3_ok_count;
} fpga_diag_snapshot_t;

static void fpga_diag_snapshot_take(fpga_diag_snapshot_t *snap)
{
    snap->tx_count = fpga.tx_count;
    snap->rx_byte_count = fpga.rx_byte_count;
    snap->frame_count = fpga.frame_count;
    snap->echo_count = fpga.echo_count;
    snap->spi3_ok_count = fpga.spi3_ok_count;
}

static void fpga_diag_print_delta(const fpga_diag_snapshot_t *before)
{
    uint16_t tx_delta = fpga.tx_count - before->tx_count;
    uint16_t rx_delta = fpga.rx_byte_count - before->rx_byte_count;
    uint16_t frame_delta = fpga.frame_count - before->frame_count;
    uint16_t echo_delta = fpga.echo_count - before->echo_count;
    uint16_t spi_delta = fpga.spi3_ok_count - before->spi3_ok_count;

    usb_debug_printf("Delta: TX %+d RX %+d DF %+d EF %+d SPI %+d\r\n",
                     (int)tx_delta, (int)rx_delta, (int)frame_delta,
                     (int)echo_delta, (int)spi_delta);

    if ((frame_delta > 0 || echo_delta > 0) && fpga.rx_frame_valid) {
        usb_debug_printf("RX:");
        for (int i = 0; i < FPGA_RX_FRAME_SIZE; i++) {
            usb_debug_printf(" %02X", fpga.rx_frame[i]);
        }
        usb_send_str("\r\n");
    }
}

static void fpga_diag_clear(void)
{
    taskENTER_CRITICAL();
    fpga.tx_count = 0;
    fpga.rx_byte_count = 0;
    fpga.frame_count = 0;
    fpga.echo_count = 0;
    fpga.rx_sync_data_start_count = 0;
    fpga.rx_sync_echo_start_count = 0;
    fpga.rx_sync_data_header_count = 0;
    fpga.rx_sync_echo_header_count = 0;
    fpga.rx_sync_bad_second_count = 0;
    fpga.rx_sync_stray_count = 0;
    fpga.rx_data_tx_busy_drop_count = 0;
    fpga.rx_echo_valid_count = 0;
    fpga.rx_echo_bad_count = 0;
    memset((void *)fpga.rx_raw_history, 0, sizeof(fpga.rx_raw_history));
    memset((void *)fpga.rx_raw_history_tx_count, 0,
           sizeof(fpga.rx_raw_history_tx_count));
    memset((void *)fpga.rx_raw_history_tx_index, 0,
           sizeof(fpga.rx_raw_history_tx_index));
    memset((void *)fpga.rx_raw_history_rx_index, 0,
           sizeof(fpga.rx_raw_history_rx_index));
    fpga.rx_raw_history_head = 0;
    fpga.rx_raw_history_count = 0;
    fpga_meter_probe_tail_override = -1;
    fpga.spi3_ok_count = 0;
    fpga.spi3_timeout_count = 0;
    fpga.spi3_total_timeouts = 0;
    fpga.h2_rx_00_count = 0;
    fpga.h2_rx_ff_count = 0;
    fpga.h2_rx_other_count = 0;
    fpga.h2_close_rx_len = 0;
    memset((void *)fpga.h2_close_rx, 0, sizeof(fpga.h2_close_rx));
    fpga.rx_frame_valid = false;
    memset((void *)fpga.rx_frame, 0, sizeof(fpga.rx_frame));
    memset((void *)fpga.last_rx_echo_frame, 0, sizeof(fpga.last_rx_echo_frame));
    memset((void *)fpga.last_tx_frame, 0, sizeof(fpga.last_tx_frame));
    memset((void *)fpga.tx_frame_history, 0, sizeof(fpga.tx_frame_history));
    memset((void *)fpga.tx_frame_history_tx_count, 0,
           sizeof(fpga.tx_frame_history_tx_count));
    fpga.tx_frame_history_head = 0;
    fpga.tx_frame_history_count = 0;
    memset((void *)fpga.tx_control_frame_history, 0,
           sizeof(fpga.tx_control_frame_history));
    memset((void *)fpga.tx_control_frame_history_tx_count, 0,
           sizeof(fpga.tx_control_frame_history_tx_count));
    fpga.tx_control_frame_history_head = 0;
    fpga.tx_control_frame_history_count = 0;
    memset((void *)fpga.rx_frame_history, 0, sizeof(fpga.rx_frame_history));
    memset((void *)fpga.rx_history_frame_count, 0, sizeof(fpga.rx_history_frame_count));
    memset((void *)fpga.rx_history_tx_count, 0, sizeof(fpga.rx_history_tx_count));
    memset((void *)fpga.rx_history_echo_count, 0, sizeof(fpga.rx_history_echo_count));
    memset((void *)fpga.rx_history_sequence_count, 0, sizeof(fpga.rx_history_sequence_count));
    memset((void *)fpga.rx_history_sequence_submode, 0, sizeof(fpga.rx_history_sequence_submode));
    memset((void *)fpga.rx_history_discard_remaining, 0, sizeof(fpga.rx_history_discard_remaining));
    memset((void *)fpga.rx_history_transition_busy, 0, sizeof(fpga.rx_history_transition_busy));
    fpga.rx_frame_history_head = 0;
    fpga.rx_frame_history_count = 0;
    memset((void *)fpga.meter_transition_history_submode, 0, sizeof(fpga.meter_transition_history_submode));
    memset((void *)fpga.meter_transition_history_config, 0, sizeof(fpga.meter_transition_history_config));
    memset((void *)fpga.meter_transition_history_selector, 0, sizeof(fpga.meter_transition_history_selector));
    memset((void *)fpga.meter_transition_history_apply, 0, sizeof(fpga.meter_transition_history_apply));
    memset((void *)fpga.meter_transition_history_bank, 0, sizeof(fpga.meter_transition_history_bank));
    memset((void *)fpga.meter_transition_history_bank_first, 0, sizeof(fpga.meter_transition_history_bank_first));
    memset((void *)fpga.meter_transition_history_bank_second, 0, sizeof(fpga.meter_transition_history_bank_second));
    memset((void *)fpga.meter_transition_history_probe, 0, sizeof(fpga.meter_transition_history_probe));
    memset((void *)fpga.meter_transition_history_start, 0, sizeof(fpga.meter_transition_history_start));
    memset((void *)fpga.meter_transition_history_sequence_count, 0, sizeof(fpga.meter_transition_history_sequence_count));
    memset((void *)fpga.meter_transition_history_tx_before, 0, sizeof(fpga.meter_transition_history_tx_before));
    memset((void *)fpga.meter_transition_history_tx_after, 0, sizeof(fpga.meter_transition_history_tx_after));
    memset((void *)fpga.meter_transition_history_frame_before, 0, sizeof(fpga.meter_transition_history_frame_before));
    memset((void *)fpga.meter_transition_history_frame_after, 0, sizeof(fpga.meter_transition_history_frame_after));
    memset((void *)fpga.meter_transition_history_planned_gpio, 0, sizeof(fpga.meter_transition_history_planned_gpio));
    memset((void *)fpga.meter_transition_history_actual_gpio, 0, sizeof(fpga.meter_transition_history_actual_gpio));
    fpga.meter_transition_history_head = 0;
    fpga.meter_transition_history_count = 0;
    memset((void *)fpga.diag_ch1_raw, 0, sizeof(fpga.diag_ch1_raw));
    memset((void *)fpga.diag_ch2_raw, 0, sizeof(fpga.diag_ch2_raw));
    fpga.diag_data_varies = 0;
    taskEXIT_CRITICAL();
}

/* Can a byte actually leave PA2 right now?
 *
 * USART-silent builds (FPGA_USART_SILENT_SCOPE — coldtrace, warm handoff)
 * configure USART2's registers but never set UEN/TE, so every send path in
 * this shell, queued or polled, is a no-op that reports success. Print the
 * refusal here once and let each command bail. */
static bool usart_tx_wire_live(void)
{
    uint32_t ctrl1 = fpga_usart_ctrl1();
    unsigned uen = (unsigned)((ctrl1 >> 13) & 1u);
    unsigned ten = (unsigned)((ctrl1 >> 3) & 1u);

    if (uen && ten) return true;

    usb_debug_printf("REFUSED: USART2 is dark (CTRL1=%08lX UEN=%u TE=%u) — nothing can\r\n"
                     "         leave PA2. This is a USART-silent build. Run `fpga usart on`\r\n"
                     "         first, then re-issue this command.\r\n",
                     (unsigned long)ctrl1, uen, ten);
    return false;
}

/* Send one 2-byte command and report HOW it went out, not just that it did.
 * Returns false when nothing was transmitted. */
static bool usart_send_cmd_reporting(uint8_t cmd_hi, uint8_t cmd_lo)
{
    if (!usart_tx_wire_live()) return false;

    if (!fpga_usart_tx_task_exists()) {
        /* The queue has no consumer on this build — go straight out the
         * polled path rather than parking the frame forever. */
        fpga_send_cmd_direct(cmd_hi, cmd_lo);
        usb_debug_printf("TX [%02X %02X]: sent DIRECT (polled) — dvom_TX drain task does not\r\n"
                         "  exist on this build, so the queue would never have been emptied.\r\n",
                         cmd_hi, cmd_lo);
        return true;
    }

    if (fpga_send_cmd(cmd_hi, cmd_lo) != pdTRUE) {
        usb_debug_printf("TX [%02X %02X]: QUEUE FULL — NOT SENT\r\n", cmd_hi, cmd_lo);
        return false;
    }
    usb_debug_printf("TX [%02X %02X]: queued (dvom_TX will drain)\r\n", cmd_hi, cmd_lo);
    return true;
}

void cmd_usart_tx(const char *args)
{
    /* Parse space-separated hex bytes, e.g. "00 09 00 00 00 00 00 00" */
    uint8_t bytes[8];
    int count = 0;

    const char *p = args;
    while (*p && count < 8) {
        while (*p == ' ') p++;
        if (!*p) break;
        uint32_t val;
        if (parse_hex32(p, &val) != 0 || val > 0xFF) {
            usb_debug_printf("ERR: bad hex byte at '%s'\r\n", p);
            return;
        }
        bytes[count++] = (uint8_t)val;
        while (*p && *p != ' ') p++;
    }

    if (count < 2) {
        usb_send_str("Usage: usart tx <cmd_hi> <cmd_lo>\r\n"
                      "  e.g.: usart tx 00 09\r\n");
        return;
    }
    /* Only bytes[0] and bytes[1] are ever transmitted. The loop above happily
     * parsed up to eight, so `usart tx 00 09 AA BB CC` sent two bytes and
     * reported "queued" with no hint that three were dropped — an operator
     * building a frame here got a two-byte command and a plausible receipt.
     * `fpga frame` is the command that takes a whole frame; say so instead of
     * quietly truncating. */
    while (*p == ' ') p++;
    if (count > 2 || *p != '\0') {
        usb_send_str("ERR: usart tx sends exactly two bytes (cmd_hi cmd_lo).\r\n"
                     "     For a full 10-byte frame use: fpga frame <hi> <lo> [p1..p5 [ck]]\r\n");
        return;
    }

    /* Two ways this command used to lie, both of which have already voided a
     * bench experiment (exp(02), 2026-08-16 — recorded VOID, not negative):
     *
     *   1. fpga_send_cmd() only ENQUEUES. On coldtrace/warm-handoff builds the
     *      dvom_TX drain task is never created, so the frame sits in the queue
     *      forever while this command prints "queued" and the operator reads
     *      the absence of a reply as a measurement.
     *   2. Those same builds leave USART2 disabled (CTRL1 UEN clear,
     *      FPGA_USART_SILENT_SCOPE), so even the polled path cannot shift a
     *      byte out of PA2.
     *
     * So: check the wire first and refuse when it is dark, and when only the
     * drain task is missing, send on the polled path and SAY that is what
     * happened. */
    if (!usart_send_cmd_reporting(bytes[0], bytes[1])) return;

    /* Wait briefly for echo/response, then show last RX frame */
    vTaskDelay(pdMS_TO_TICKS(200));
    if (fpga.rx_frame_valid) {
        usb_debug_printf("RX:");
        for (int i = 0; i < FPGA_RX_FRAME_SIZE; i++)
            usb_debug_printf(" %02X", fpga.rx_frame[i]);
        usb_send_str("\r\n");
    } else {
        usb_send_str("RX: (no frame)\r\n");
    }
}

void cmd_usart_raw(const char *args)
{
    /* Parse exactly 10 space-separated hex bytes for a raw USART2 frame */
    uint8_t frame[10];
    int count = 0;
    fpga_diag_snapshot_t before;

    const char *p = args;
    while (*p && count < 10) {
        while (*p == ' ') p++;
        if (!*p) break;
        uint32_t val;
        if (parse_hex32(p, &val) != 0 || val > 0xFF) {
            usb_debug_printf("ERR: bad hex byte at '%s'\r\n", p);
            return;
        }
        frame[count++] = (uint8_t)val;
        while (*p && *p != ' ') p++;
    }

    if (count != 10) {
        usb_send_str("Usage: usart raw <10 hex bytes>\r\n"
                      "  e.g.: usart raw 00 00 00 0B 01 00 00 00 00 0B\r\n"
                      "  Format: [hdr0][hdr1][cmd_hi][cmd_lo][p1][p2][p3][p4][p5][cksum]\r\n");
        return;
    }

    if (!usart_tx_wire_live()) return;

    usb_debug_printf("TX raw:");
    for (int i = 0; i < 10; i++)
        usb_debug_printf(" %02X", frame[i]);
    usb_send_str("\r\n");

    fpga_diag_snapshot_take(&before);
    fpga_send_raw_frame(frame);

    /* Wait for response */
    vTaskDelay(pdMS_TO_TICKS(200));
    fpga_diag_print_delta(&before);
}

void cmd_fpga_frame(const char *args)
{
    uint8_t frame[10] = {0};
    uint8_t bytes[8];
    int count = 0;
    const char *p = args;
    fpga_diag_snapshot_t before;

    while (*p && count < 8) {
        while (*p == ' ') p++;
        if (!*p) break;

        uint32_t val;
        /* HEX — matches `usart raw` and the examples in this command's own
         * help, which decimal parsing would have rejected (EXP-23). */
        if (parse_hex32(p, &val) != 0 || val > 0xFF) {
            usb_debug_printf("ERR: bad hex byte at '%s'\r\n", p);
            return;
        }

        bytes[count++] = (uint8_t)val;
        while (*p && *p != ' ') p++;
    }

    while (*p == ' ') p++;
    if (*p != '\0') {
        usb_send_str("ERR: too many bytes\r\n");
        return;
    }

    if (count < 2) {
        usb_send_str("Usage: fpga frame <hi> <lo> [p1 p2 p3 p4 p5 [ck]]\r\n"
                     "  Builds [00 00 hi lo p1 p2 p3 p4 p5 ck]\r\n"
                     "  Default ck = (hi + lo) & 0xFF\r\n"
                     "  e.g.: fpga frame 00 0B 01 00 00 00 00\r\n");
        return;
    }

    frame[2] = bytes[0];
    frame[3] = bytes[1];
    for (int i = 2; i < count && i < 7; i++) {
        frame[i + 2] = bytes[i];
    }

    if (count >= 8) {
        frame[9] = bytes[7];
    } else {
        frame[9] = (uint8_t)((frame[2] + frame[3]) & 0xFF);
    }

    if (!usart_tx_wire_live()) return;

    usb_debug_printf("TX frame:");
    for (int i = 0; i < 10; i++) {
        usb_debug_printf(" %02X", frame[i]);
    }
    usb_send_str("\r\n");

    fpga_diag_snapshot_take(&before);
    fpga_send_raw_frame(frame);
    vTaskDelay(pdMS_TO_TICKS(200));
    fpga_diag_print_delta(&before);
}

/* ═══════════════════════════════════════════════════════════════════
 * GPIO mode inspection — the instrument check `gpio set` never had
 *
 * WHY THIS EXISTS. `gpio set` writes scr/clr unconditionally. On a pin left
 * as an INPUT those writes only pick a pull resistor; the pin is not driven,
 * the command still prints "PA6 -> HIGH", and the experiment records a false
 * negative. That has happened three separate times (PA6 and PD12 both boot as
 * floating inputs; a stale PC12 HIGH from an earlier command silently killed
 * the analog path so a later test measured a dead one).
 *
 * The cure is the same one the FPGA work already learned from the /2 reads
 * and the floating MISO: never let a command report success for something it
 * cannot actually have done. So `gpio set` now reads the pin's config nibble
 * and REFUSES when the pin is not a plain output, and `gpio mode` exists to
 * change it deliberately.
 *
 * Nibble layout (AT32 cfglr/cfghr, STM32F1-compatible — the docs and bench
 * scripts in this repo call these registers CRL/CRH, so the shell does too):
 *   bits[1:0] MODE  0 = input, 1 = out 10MHz, 2 = out 2MHz, 3 = out 50MHz
 *   bits[3:2] CNF   input:  0 analog, 1 floating, 2 pull (up/down per ODT)
 *                   output: 0 push-pull, 1 open-drain, 2 AF-PP, 3 AF-OD
 * So nibble 0/4/8 = input, 1/2/3 = output PP, 5/6/7 = output OD, 9/A/B =
 * AF push-pull, D/E/F = AF open-drain.
 * ═══════════════════════════════════════════════════════════════════ */

/* 'A'..'E' for a port pointer (GPIO blocks are 0x400 apart). */
static char gpio_port_letter(gpio_type *port)
{
    return (char)('A' + (((uint32_t)port - (uint32_t)GPIOA) / 0x400));
}

/* &cfglr for pins 0-7, &cfghr for pins 8-15. */
static volatile uint32_t *gpio_cfg_reg(gpio_type *port, uint8_t pin_no)
{
    return (pin_no < 8) ? &port->cfglr : &port->cfghr;
}

static const char *gpio_cfg_reg_name(uint8_t pin_no)
{
    return (pin_no < 8) ? "CRL" : "CRH";
}

static uint8_t gpio_cfg_nibble(gpio_type *port, uint8_t pin_no)
{
    return (uint8_t)((*gpio_cfg_reg(port, pin_no) >> ((pin_no & 7) * 4)) & 0xF);
}

/* Read-modify-write of ONLY this pin's nibble — the pattern the bench scripts
 * have been doing by hand through `mem read`/`mem write`. */
static void gpio_cfg_nibble_set(gpio_type *port, uint8_t pin_no, uint8_t nibble)
{
    volatile uint32_t *reg = gpio_cfg_reg(port, pin_no);
    uint32_t shift = (uint32_t)(pin_no & 7) * 4;
    *reg = (*reg & ~(0xFu << shift)) | (((uint32_t)nibble & 0xFu) << shift);
}

static bool gpio_mode_is_output(uint8_t nibble)  { return (nibble & 0x3) != 0; }
static bool gpio_mode_is_af(uint8_t nibble)      { return gpio_mode_is_output(nibble) && (nibble & 0x8) != 0; }

/* Human name for a nibble, rendered into a CALLER-OWNED buffer (>= 16 bytes)
 * so two nibbles can be named in one printf — a shared static would alias and
 * print the same mode twice. `odt_bit` only disambiguates pull-up vs pull-down
 * on an input, where ODT selects the resistor rather than a drive level. */
#define GPIO_MODE_NAME_LEN 16
static const char *gpio_mode_name(uint8_t nibble, uint8_t odt_bit, char *out)
{
    static const char *const SPEED[4] = { "?", "10MHz", "2MHz", "50MHz" };
    const char *cnf;

    if (!gpio_mode_is_output(nibble)) {
        switch ((nibble >> 2) & 0x3) {
            case 0:  cnf = "analog in";   break;
            case 1:  cnf = "floating in"; break;
            case 2:  cnf = odt_bit ? "pull-up in" : "pull-down in"; break;
            default: cnf = "reserved in"; break;
        }
        snprintf(out, GPIO_MODE_NAME_LEN, "%s", cnf);
        return out;
    }

    cnf = ((nibble >> 2) & 0x3) == 0 ? "out PP" :
          ((nibble >> 2) & 0x3) == 1 ? "out OD" :
          ((nibble >> 2) & 0x3) == 2 ? "AF PP"  : "AF OD";
    snprintf(out, GPIO_MODE_NAME_LEN, "%s %s", cnf, SPEED[nibble & 0x3]);
    return out;
}

/* ═══════════════════════════════════════════════════════════════════
 * Bench pin registry
 *
 * Every pin this firmware DRIVES on purpose. One list, used by three
 * consumers so they cannot drift apart: `fpga selftest` (is the instrument
 * sane before I trust a measurement?), `bench snapshot`/`bench restore`
 * (did a previous experiment leak state into this one?), and the "we treat
 * this as an output" claim that `fpga selftest` checks against reality.
 *
 * The ROLE strings are deliberately hedged where the project's own evidence
 * is hedged — PB11 in particular (see cmd_gpio_scan). A table that asserts
 * a pin's function more confidently than the evidence does is the same
 * defect as printing an unmeasured value.
 * ═══════════════════════════════════════════════════════════════════ */
typedef struct {
    gpio_type  *port;
    uint8_t     pin_no;
    const char *role;
} bench_pin_t;

static const bench_pin_t BENCH_PINS[] = {
    { GPIOC, 12, "scope input coupling (HIGH = DC passes)" },
    { GPIOE,  4, "CH1 attenuator ladder bit 0" },
    { GPIOE,  5, "CH1 attenuator ladder bit 1" },
    { GPIOE,  6, "CH1 attenuator ladder bit 2" },
    { GPIOA, 15, "CH2 input relay (bench 2026-08-15)" },
    { GPIOB, 11, "\"active mode\" — DISPUTED, likely CH2 attenuator" },
    { GPIOB, 10, "gain select (CH2 bank)" },
    { GPIOA, 10, "gain select (CH2 bank)" },
    { GPIOA,  6, "analog frontend control (boots as FLOATING INPUT)" },
    { GPIOC, 11, "meter MUX enable" },
    { GPIOC,  6, "FPGA SPI enable (must be HIGH)" },
    { GPIOB,  6, "FPGA SPI3 chip select (idle HIGH)" },
    { GPIOD, 12, "coupling (bench 2026-08-15; boots as FLOATING INPUT)" },
    { GPIOD, 13, "coupling (bench 2026-08-15)" },
};
#define BENCH_PIN_COUNT (sizeof(BENCH_PINS) / sizeof(BENCH_PINS[0]))

/* One line of the pin table: mode nibble decoded, plus the LEVEL. For an
 * output the meaningful level is what we drive (ODT); for an input it is what
 * the world puts there (IDT). Both are printed so neither can be mistaken for
 * the other — reading IDT on an output and calling it "what we set" is how a
 * shorted relay line reads back as success. */
static void bench_pin_print_row(const bench_pin_t *bp, bool flag_inputs)
{
    uint8_t nib = gpio_cfg_nibble(bp->port, bp->pin_no);
    uint8_t odt = (bp->port->odt & (1u << bp->pin_no)) ? 1 : 0;
    uint8_t idt = (bp->port->idt & (1u << bp->pin_no)) ? 1 : 0;
    char mode[GPIO_MODE_NAME_LEN];

    usb_debug_printf("  P%c%-2u  %s %X  %-13s  odt=%u idt=%u%s\r\n",
                     gpio_port_letter(bp->port), bp->pin_no,
                     gpio_cfg_reg_name(bp->pin_no), nib,
                     gpio_mode_name(nib, odt, mode), odt, idt,
                     (flag_inputs && !gpio_mode_is_output(nib))
                         ? "   <== INPUT, but we drive it: writes are INERT"
                         : (flag_inputs && gpio_mode_is_af(nib))
                               ? "   <== ALT-FUNCTION: a peripheral owns it"
                               : "");
    usb_debug_printf("        %s\r\n", bp->role);
}

/* Saved GPIO posture, so an experiment can be made hermetic. */
typedef struct {
    uint8_t nibble;
    uint8_t odt;
} bench_pin_state_t;

static bench_pin_state_t bench_snap[BENCH_PIN_COUNT];
static bool              bench_snap_valid = false;

void cmd_bench_snapshot(void)
{
    usb_send_str("=== bench snapshot: frontend/FPGA pin posture saved ===\r\n");
    for (unsigned i = 0; i < BENCH_PIN_COUNT; i++) {
        bench_snap[i].nibble = gpio_cfg_nibble(BENCH_PINS[i].port, BENCH_PINS[i].pin_no);
        bench_snap[i].odt    = (BENCH_PINS[i].port->odt & (1u << BENCH_PINS[i].pin_no)) ? 1 : 0;
        bench_pin_print_row(&BENCH_PINS[i], false);
    }
    bench_snap_valid = true;
}

void cmd_bench_restore(void)
{
    unsigned changed = 0;

    if (!bench_snap_valid) {
        usb_send_str("ERR: nothing snapshotted this boot — run `bench snapshot` first\r\n");
        return;
    }

    usb_send_str("=== bench restore ===\r\n");
    for (unsigned i = 0; i < BENCH_PIN_COUNT; i++) {
        const bench_pin_t *bp = &BENCH_PINS[i];
        uint8_t nib_now = gpio_cfg_nibble(bp->port, bp->pin_no);
        uint8_t odt_now = (bp->port->odt & (1u << bp->pin_no)) ? 1 : 0;

        if (nib_now == bench_snap[i].nibble && odt_now == bench_snap[i].odt) continue;

        /* Level first, then mode: writing ODT while the pin is still an input
         * only moves the pull, so the pin cannot glitch to the wrong drive
         * level on the way back to being an output. */
        if (bench_snap[i].odt) bp->port->scr = (uint16_t)(1u << bp->pin_no);
        else                   bp->port->clr = (uint16_t)(1u << bp->pin_no);
        gpio_cfg_nibble_set(bp->port, bp->pin_no, bench_snap[i].nibble);

        usb_debug_printf("  P%c%-2u  %s %X odt=%u  ->  %s %X odt=%u\r\n",
                         gpio_port_letter(bp->port), bp->pin_no,
                         gpio_cfg_reg_name(bp->pin_no), nib_now, odt_now,
                         gpio_cfg_reg_name(bp->pin_no), bench_snap[i].nibble,
                         bench_snap[i].odt);
        changed++;
    }
    usb_debug_printf("%u pin(s) restored, %u already matched\r\n",
                     changed, (unsigned)BENCH_PIN_COUNT - changed);
}

/* `gpio mode <port><pin> out|in` — deliberate, single-pin read-modify-write of
 * the config nibble. `out` = push-pull 50MHz (nibble 3, what gpio_init gives
 * with GPIO_DRIVE_STRENGTH_STRONGER); `in` = floating input (nibble 4). */
void cmd_gpio_mode(const char *args)
{
    gpio_type *port;
    uint16_t pin;
    uint8_t pin_no, old_nib, new_nib, odt;
    char old_name[GPIO_MODE_NAME_LEN], new_name[GPIO_MODE_NAME_LEN];
    char pin_str[8];
    const char *space = strchr(args, ' ');
    int len;

    if (!space) {
        usb_send_str("Usage: gpio mode <port><pin> <out|in>\r\n"
                     "  e.g.: gpio mode A6 out   (push-pull 50MHz, nibble 3)\r\n"
                     "        gpio mode A6 in    (floating input, nibble 4)\r\n");
        return;
    }

    len = space - args;
    if (len >= (int)sizeof(pin_str)) { usb_send_str("ERR: bad pin\r\n"); return; }
    memcpy(pin_str, args, len);
    pin_str[len] = '\0';

    if (parse_gpio(pin_str, &port, &pin) != 0) {
        usb_send_str("ERR: bad pin (e.g. A7, B11, C6)\r\n");
        return;
    }
    pin_no = (uint8_t)__builtin_ctz(pin);

    while (*space == ' ') space++;
    if      (strcmp(space, "out") == 0) new_nib = 0x3;
    else if (strcmp(space, "in")  == 0) new_nib = 0x4;
    else {
        usb_send_str("ERR: mode must be 'out' (push-pull 50MHz) or 'in' (floating)\r\n");
        return;
    }

    old_nib = gpio_cfg_nibble(port, pin_no);
    odt = (port->odt & pin) ? 1 : 0;
    (void)gpio_mode_name(old_nib, odt, old_name);
    (void)gpio_mode_name(new_nib, odt, new_name);

    if (gpio_mode_is_af(old_nib)) {
        usb_debug_printf("WARN: P%c%u was alternate-function (%s %X = %s) — a peripheral\r\n"
                         "      owned this pin and will now be disconnected from it.\r\n",
                         gpio_port_letter(port), pin_no,
                         gpio_cfg_reg_name(pin_no), old_nib, old_name);
    }
    gpio_cfg_nibble_set(port, pin_no, new_nib);

    usb_debug_printf("P%c%u %s nibble %X (%s) -> %X (%s)\r\n",
                     gpio_port_letter(port), pin_no, gpio_cfg_reg_name(pin_no),
                     old_nib, old_name, new_nib, new_name);
}

void cmd_gpio_set(const char *args)
{
    /* Parse "<port><pin> <0|1>" e.g. "B11 1" */
    gpio_type *port;
    uint16_t pin;

    const char *space = strchr(args, ' ');
    if (!space) {
        usb_send_str("Usage: gpio set <port><pin> <0|1>\r\n");
        return;
    }

    /* Temporary null-terminate the pin spec */
    char pin_str[8];
    int len = space - args;
    if (len >= (int)sizeof(pin_str)) { usb_send_str("ERR: bad pin\r\n"); return; }
    memcpy(pin_str, args, len);
    pin_str[len] = '\0';

    if (parse_gpio(pin_str, &port, &pin) != 0) {
        usb_send_str("ERR: bad pin (e.g. A7, B11, C6)\r\n");
        return;
    }

    uint32_t val;
    if (parse_int(space + 1, &val) != 0 || val > 1) {
        usb_send_str("ERR: value must be 0 or 1\r\n");
        return;
    }

    /* REFUSE rather than pretend. On an input pin scr/clr only picks a pull
     * resistor — the pin is not driven — and three experiments have already
     * been invalidated by this command reporting HIGH/LOW anyway. On an AF
     * pin a peripheral owns the output and our write is equally inert. */
    {
        uint8_t pin_no = (uint8_t)__builtin_ctz(pin);
        uint8_t nib = gpio_cfg_nibble(port, pin_no);
        char mode[GPIO_MODE_NAME_LEN];

        if (!gpio_mode_is_output(nib)) {
            usb_debug_printf("REFUSED: P%c%u is a %s (%s nibble %X) — not driven.\r\n"
                             "         Use `gpio mode %c%u out` first.\r\n",
                             gpio_port_letter(port), pin_no,
                             gpio_mode_name(nib, (port->odt & pin) ? 1 : 0, mode),
                             gpio_cfg_reg_name(pin_no), nib,
                             gpio_port_letter(port), pin_no);
            return;
        }
        if (gpio_mode_is_af(nib)) {
            usb_debug_printf("REFUSED: P%c%u is %s (%s nibble %X) — a peripheral drives it,\r\n"
                             "         scr/clr writes are inert. Use `gpio mode %c%u out` to take it.\r\n",
                             gpio_port_letter(port), pin_no,
                             gpio_mode_name(nib, 0, mode),
                             gpio_cfg_reg_name(pin_no), nib,
                             gpio_port_letter(port), pin_no);
            return;
        }
    }

    if (val)
        port->scr = pin;    /* Set */
    else
        port->clr = pin;    /* Clear */

    /* Read back IDT: on a driven pin it is the pin itself, so a line clamped
     * by the outside world shows up here instead of being reported as set. */
    /* SCR/CLR take effect on the next APB cycle: an IDT read in the same
     * instruction stream can still show the old level ("idt reads 0" right
     * after driving high). Let the output settle, then read. */
    for (volatile int i = 0; i < 200; i++) { }
    usb_debug_printf("P%c%d -> %s  (odt %u, idt reads %u)\r\n",
                     gpio_port_letter(port),
                     __builtin_ctz(pin),
                     val ? "HIGH" : "LOW",
                     (port->odt & pin) ? 1u : 0u,
                     (port->idt & pin) ? 1u : 0u);
}

void cmd_buzzer_test(const char *args)
{
    uint32_t duration = 750;
    bool started = false;
    bool active = false;
    uint32_t toggles = 0;
    uint32_t create_failures = 0;

    if (args && *args) {
        if (parse_int(args, &duration) != 0 || duration > 5000U) {
            usb_send_str("Usage: buzzer test [ms<=5000]\r\n");
            return;
        }
    }

    continuity_buzzer_force_ms(duration);
    continuity_buzzer_snapshot(&started, &active, &toggles, &create_failures);
    usb_debug_printf("buzzer forced_ms=%lu task=%u active=%u toggles=%lu create_fail=%lu\r\n",
                     duration,
                     started ? 1U : 0U,
                     active ? 1U : 0U,
                     toggles,
                     create_failures);
}

void cmd_gpio_read(const char *args)
{
    gpio_type *port;
    uint16_t pin;

    if (parse_gpio(args, &port, &pin) != 0) {
        usb_send_str("ERR: bad pin (e.g. A7, B11, C6)\r\n");
        return;
    }

    /* Print the MODE alongside the level. "PA6 = 0" on a floating input and on
     * a driven-low output are the same three characters and mean entirely
     * different things; the shell should not make the reader guess which. */
    uint8_t pin_no = (uint8_t)__builtin_ctz(pin);
    uint8_t nib = gpio_cfg_nibble(port, pin_no);
    uint8_t odt = (port->odt & pin) ? 1 : 0;
    char mode[GPIO_MODE_NAME_LEN];

    usb_debug_printf("P%c%u = %u  [%s %X = %s, odt=%u]\r\n",
                     gpio_port_letter(port), pin_no,
                     (port->idt & pin) ? 1u : 0u,
                     gpio_cfg_reg_name(pin_no), nib,
                     gpio_mode_name(nib, odt, mode), odt);
}

void cmd_gpio_scan(void)
{
    /* Live IDT reads throughout — every number below is measured, not cached.
     * The LABELS are the fallible part: PB11's is flagged because the project
     * no longer believes it (ripcord static sweep 2026-08-12 — stock writes
     * PB11 only from CH2 analog range/cal code, never in master_init or the
     * config window, so it is probably a CH2 attenuator relay). A diagnostic
     * that asserts a pin's function more confidently than the evidence does is
     * the same failure as printing an unmeasured value. */
    usb_send_str("=== FPGA Control Pins ===\r\n");
    usb_debug_printf("PC6  (SPI enable):  %d\r\n", (GPIOC->idt & (1 << 6))  ? 1 : 0);
    usb_debug_printf("PB11 (\"active mode\" — name DISPUTED, likely CH2 atten): %d\r\n",
                     (GPIOB->idt & (1 << 11)) ? 1 : 0);
    usb_debug_printf("PC0  (data ready):  %d\r\n", (GPIOC->idt & (1 << 0))  ? 1 : 0);
    usb_debug_printf("PC11 (meter MUX):   %d\r\n", (GPIOC->idt & (1 << 11)) ? 1 : 0);

    usb_send_str("\r\n=== SPI3 Pins ===\r\n");
    usb_debug_printf("PB3  (SCK):  %d\r\n",  (GPIOB->idt & (1 << 3)) ? 1 : 0);
    usb_debug_printf("PB4  (MISO): %d\r\n",  (GPIOB->idt & (1 << 4)) ? 1 : 0);
    usb_debug_printf("PB5  (MOSI): %d\r\n",  (GPIOB->idt & (1 << 5)) ? 1 : 0);
    usb_debug_printf("PB6  (CS):   %d\r\n",  (GPIOB->idt & (1 << 6)) ? 1 : 0);

    usb_send_str("\r\n=== Analog Frontend ===\r\n");
    usb_debug_printf("PC12 (input route): %d\r\n", (GPIOC->idt & (1 << 12)) ? 1 : 0);
    usb_debug_printf("PE4  (range):       %d\r\n", (GPIOE->idt & (1 << 4))  ? 1 : 0);
    usb_debug_printf("PE5  (atten):       %d\r\n", (GPIOE->idt & (1 << 5))  ? 1 : 0);
    usb_debug_printf("PE6  (atten):       %d\r\n", (GPIOE->idt & (1 << 6))  ? 1 : 0);

    usb_send_str("\r\n=== Gain Resistors ===\r\n");
    usb_debug_printf("PA15 (gain):  %d\r\n", (GPIOA->idt & (1 << 15)) ? 1 : 0);
    usb_debug_printf("PA10 (gain):  %d\r\n", (GPIOA->idt & (1 << 10)) ? 1 : 0);
    usb_debug_printf("PB10 (gain):  %d\r\n", (GPIOB->idt & (1 << 10)) ? 1 : 0);
    usb_debug_printf("PB9  (afe):   %d\r\n", (GPIOB->idt & (1 << 9))  ? 1 : 0);
    usb_debug_printf("PA6  (afe):   %d\r\n", (GPIOA->idt & (1 << 6))  ? 1 : 0);
}

void cmd_mem_read(const char *args)
{
    /* Parse "<addr> [count]" */
    uint32_t addr;
    const char *space = strchr(args, ' ');
    char addr_str[16];

    if (space) {
        int len = space - args;
        if (len >= (int)sizeof(addr_str)) { usb_send_str("ERR: addr too long\r\n"); return; }
        memcpy(addr_str, args, len);
        addr_str[len] = '\0';
    } else {
        strncpy(addr_str, args, sizeof(addr_str) - 1);
        addr_str[sizeof(addr_str) - 1] = '\0';
    }

    if (parse_hex32(addr_str, &addr) != 0) {
        usb_send_str("Usage: mem read <hex_addr> [count]\r\n");
        return;
    }

    uint32_t count = 1;
    /* Checked, not discarded: parse_int() writes *out from strtoul() even when
     * it fails, so `mem read 0x40021000 four` used to set count=0 and print
     * absolutely nothing — indistinguishable from a region that reads back
     * empty. */
    if (space && parse_int(space + 1, &count) != 0) {
        usb_send_str("Usage: mem read <hex_addr> [count]\r\n");
        return;
    }
    if (count > 64) count = 64;

    /* Align to 4 bytes */
    addr &= ~3u;

    for (uint32_t i = 0; i < count; i++) {
        volatile uint32_t *p = (volatile uint32_t *)(addr + i * 4);
        if (i % 4 == 0) usb_debug_printf("0x%08lX:", addr + i * 4);
        usb_debug_printf(" %08lX", *p);
        if (i % 4 == 3 || i == count - 1) usb_send_str("\r\n");
    }
}

void cmd_mem_write(const char *args)
{
    /* Parse "<addr> <value>" */
    uint32_t addr, value;
    const char *space = strchr(args, ' ');
    if (!space) {
        usb_send_str("Usage: mem write <hex_addr> <hex_value>\r\n");
        return;
    }

    char addr_str[16];
    int len = space - args;
    if (len >= (int)sizeof(addr_str)) { usb_send_str("ERR: addr too long\r\n"); return; }
    memcpy(addr_str, args, len);
    addr_str[len] = '\0';

    if (parse_hex32(addr_str, &addr) != 0 || parse_hex32(space + 1, &value) != 0) {
        usb_send_str("Usage: mem write <hex_addr> <hex_value>\r\n");
        return;
    }

    addr &= ~3u;
    *(volatile uint32_t *)addr = value;
    usb_debug_printf("0x%08lX <- 0x%08lX\r\n", addr, value);
}

/*
 * Self-protecting write-primitive bench test (V2 confirmation for the faithful
 * reimpl of the stock W25Q write driver — flash_fs_raw_program/sector_erase/
 * write_block). NON-DESTRUCTIVE by contract:
 *   - addr MUST be 4KB-sector-aligned;
 *   - the ENTIRE 4KB sector must already be erased (all 0xFF) — refuses otherwise,
 *     so it can only ever touch blank flash;
 *   - exercises both write paths (in-place page-program + erase/read-modify-write)
 *     then erases the sector back to 0xFF, restoring the pre-test state exactly.
 * Requires an explicit CONFIRM token: `flash wtest <addr> CONFIRM`.
 */
void cmd_flash_diag(void)
{
    uint8_t sr[4] = {0};
    if (flash_fs_raw_status_diag(sr) != FLASH_FS_OK) { usb_send_str("ERR: status diag\r\n"); return; }
    usb_debug_printf("SR1=0x%02X SR2=0x%02X SR3=0x%02X | SR1-after-WREN=0x%02X\r\n",
                     sr[0], sr[1], sr[2], sr[3]);
    usb_debug_printf("  BP/protect bits (SR1&0x7C)=0x%02X  WEL-after-WREN=%d  BUSY=%d\r\n",
                     sr[0] & 0x7C, (sr[3] >> 1) & 1, sr[0] & 1);
}

void cmd_flash_wtest(const char *args)
{
    /* Lean stack footprint: the usb_dbg task has only 2KB of stack, so this uses a
     * single 64-byte work buffer (NOT 256-byte arrays — that overflowed the task). */
    char abuf[40];
    char *saveptr = NULL;
    uint32_t addr;
    uint8_t b[64];
    const uint32_t TLEN = sizeof(b);   /* 64-byte test window within the sector */

    if (strlen(args) >= sizeof(abuf)) { usb_send_str("Usage: flash wtest <addr> CONFIRM\r\n"); return; }
    strcpy(abuf, args);

    char *t_addr = strtok_r(abuf, " \t", &saveptr);
    char *t_conf = strtok_r(NULL, " \t", &saveptr);
    if (t_addr == NULL || parse_int(t_addr, &addr) != 0) {
        usb_send_str("Usage: flash wtest <addr> CONFIRM\r\n"); return;
    }
    if (t_conf == NULL || strcmp(t_conf, "CONFIRM") != 0) {
        usb_send_str("Refused: append CONFIRM. This writes external flash.\r\n"); return;
    }
    if (addr & 0xFFFu) {
        usb_send_str("Refused: addr must be 4KB-sector-aligned (mask 0xFFF).\r\n"); return;
    }

    /* Safety: the whole 4KB sector must be blank (0xFF) so the restoring erase is
     * guaranteed non-destructive. */
    for (uint32_t off = 0; off < 4096; off += sizeof(b)) {
        if (flash_fs_raw_read_bytes(addr + off, b, sizeof(b)) != FLASH_FS_OK) {
            usb_send_str("ERR: pre-read failed\r\n"); return;
        }
        for (uint32_t i = 0; i < sizeof(b); i++) {
            if (b[i] != 0xFF) {
                usb_debug_printf("Refused: sector not blank (byte 0x%lX = 0x%02X). Pick an erased sector.\r\n",
                                 (unsigned long)(addr + off + i), b[i]);
                return;
            }
        }
    }
    usb_debug_printf("wtest @0x%lX: sector blank, OK to proceed\r\n", (unsigned long)addr);

    /* Path 1: program-in-place (target erased) — ascending pattern. */
    for (uint32_t i = 0; i < TLEN; i++) b[i] = (uint8_t)i;
    if (flash_fs_raw_write_block(addr, b, TLEN) != FLASH_FS_OK) { usb_send_str("ERR: write_block#1\r\n"); goto restore; }
    if (flash_fs_raw_read_bytes(addr, b, TLEN) != FLASH_FS_OK) { usb_send_str("ERR: readback#1\r\n"); goto restore; }
    {
        int bad = -1;
        for (uint32_t i = 0; i < TLEN; i++) if (b[i] != (uint8_t)i) { bad = (int)i; break; }
        if (bad >= 0) {
            usb_debug_printf("FAIL: in-place mismatch @+%d; readback[0..7]=%02X %02X %02X %02X %02X %02X %02X %02X\r\n",
                             bad, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
            goto restore;
        }
    }
    usb_send_str("PASS: page-program (in-place)\r\n");

    /* Path 2: erase + read-modify-write (target now non-0xFF) — inverse pattern. */
    for (uint32_t i = 0; i < TLEN; i++) b[i] = (uint8_t)(0xFF - i);
    if (flash_fs_raw_write_block(addr, b, TLEN) != FLASH_FS_OK) { usb_send_str("ERR: write_block#2\r\n"); goto restore; }
    if (flash_fs_raw_read_bytes(addr, b, TLEN) != FLASH_FS_OK) { usb_send_str("ERR: readback#2\r\n"); goto restore; }
    for (uint32_t i = 0; i < TLEN; i++) if (b[i] != (uint8_t)(0xFF - i)) { usb_send_str("FAIL: erase+RMW mismatch\r\n"); goto restore; }
    usb_send_str("PASS: erase + read-modify-write\r\n");

restore:
    /* Restore: erase the sector back to all-0xFF (its pre-test state). */
    if (flash_fs_raw_sector_erase(addr) != FLASH_FS_OK) { usb_send_str("ERR: restore erase\r\n"); return; }
    {
        bool blank = true;
        for (uint32_t off = 0; off < 4096 && blank; off += sizeof(b)) {
            if (flash_fs_raw_read_bytes(addr + off, b, sizeof(b)) != FLASH_FS_OK) { blank = false; break; }
            for (uint32_t i = 0; i < sizeof(b); i++) if (b[i] != 0xFF) { blank = false; break; }
        }
        usb_debug_printf("%s: sector restored to 0xFF\r\n", blank ? "PASS" : "FAIL");
    }
}

/*
 * Scope trigger-comparator DAC (faithful reimpl of stock FUN_080018a4 CH1 path).
 *   trig raw <0-4095>     direct 12-bit DAC1 write + software trigger
 *   trig <range> <level>  full cal-formula path; range 0-9, level -100..+100
 * Output appears on PA4 (DAC1). Scope PA4 to verify: V = code/4095 * Vref(~3.3V).
 */
void cmd_scope_trig(const char *args)
{
    char buf[48];
    if (strlen(args) >= sizeof(buf)) { usb_send_str("Usage: trig raw <code> | trig <range> <level>\r\n"); return; }
    strcpy(buf, args);

    char *saveptr = NULL;
    char *t1 = strtok_r(buf, " \t", &saveptr);
    char *t2 = strtok_r(NULL, " \t", &saveptr);

    scope_trigger_dac_init();

    if (t1 != NULL && strcmp(t1, "raw") == 0 && t2 != NULL) {
        uint32_t code = 0;
        if (parse_int(t2, &code) != 0) { usb_send_str("Usage: trig raw <0-4095>\r\n"); return; }
        if (code > 4095) code = 4095;
        scope_trigger_dac_raw((uint16_t)code);
    } else if (t1 != NULL && t2 != NULL) {
        uint32_t r = 0, lv = 0; int level;
        const char *ls = t2;
        int neg = 0;
        if (*ls == '-') { neg = 1; ls++; }
        if (parse_int(t1, &r) != 0 || parse_int(ls, &lv) != 0) {
            usb_send_str("Usage: trig <range 0-9> <level -100..100>\r\n"); return;
        }
        level = neg ? -(int)lv : (int)lv;
        scope_trigger_dac_set((int)r, level);
    } else {
        usb_send_str("Usage: trig raw <code> | trig <range> <level>\r\n");
        return;
    }

    uint16_t code = scope_trigger_dac_last();
    /* Vref assumed 3.3V; mV = code * 3300 / 4095. */
    uint32_t mv = ((uint32_t)code * 3300u) / 4095u;
    usb_debug_printf("DAC1(PA4) = code %u (0x%03X)  ~%lu.%03lu V expected\r\n",
                     code, code, (unsigned long)(mv / 1000), (unsigned long)(mv % 1000));
}

/*
 * CH2 vertical-offset reference: TMR13 CH1 PWM-DAC on PA6.
 *   trig2 raw <0-4095>     direct duty write to TMR13_C1DT
 *   trig2 <range> <level>  same cal formula as CH1 (ripcord contract 38)
 *
 * WHY THIS EXISTS (2026-08-17). CH1's DAC1 (PA4) turned out to be a vertical-
 * OFFSET injector, not a trigger threshold: `fpga scope center` centers CH1 by
 * binary-searching DAC1 until the capture mean lands on 128, and that was
 * bench-validated (648492a). The FPGA's actual trigger level is digital —
 * SPI3 reg 0x08, an ADC code. CH2 has no DAC channel; stock drives its offset
 * from a TMR13 CH1 PWM-DAC on PA6. Our firmware never programmed TMR13, which
 * is the leading explanation for EXP-06: every CH2 mux tap parked at a fixed DC
 * level and railed regardless of drive amplitude, so only one tap was usable.
 *
 * PA6's identity is DECODED, NOT CONFIRMED (HARDWARE_PINOUT.md marks it
 * "candidate, unconfirmed"). The point of this command is to settle that on the
 * bench, with the CH1 DAC1 sweep as the positive control in the same session:
 * if sweeping DAC1 moves CH1's mean but sweeping this does not move CH2's, the
 * pin is wrong and the failure is clean rather than ambiguous.
 *
 * scope_trigger_ch2_raw() self-inits, so this works on ANY build — no
 * FPGA_CH2_TRIGGER flag needed. That flag only arms TMR13 at boot.
 */
void cmd_scope_trig2(const char *args)
{
    char buf[48];
    if (strlen(args) >= sizeof(buf)) { usb_send_str("Usage: trig2 raw <code> | trig2 <range> <level>\r\n"); return; }
    strcpy(buf, args);

    char *saveptr = NULL;
    char *t1 = strtok_r(buf, " \t", &saveptr);
    char *t2 = strtok_r(NULL, " \t", &saveptr);

    scope_trigger_ch2_init();

    if (t1 != NULL && strcmp(t1, "raw") == 0 && t2 != NULL) {
        uint32_t code = 0;
        if (parse_int(t2, &code) != 0) { usb_send_str("Usage: trig2 raw <0-4095>\r\n"); return; }
        if (code > 4095) code = 4095;
        scope_trigger_ch2_raw((uint16_t)code);
    } else if (t1 != NULL && t2 != NULL) {
        uint32_t r = 0, lv = 0; int level;
        const char *ls = t2;
        int neg = 0;
        if (*ls == '-') { neg = 1; ls++; }
        if (parse_int(t1, &r) != 0 || parse_int(ls, &lv) != 0) {
            usb_send_str("Usage: trig2 <range 0-9> <level -100..100>\r\n"); return;
        }
        level = neg ? -(int)lv : (int)lv;
        scope_trigger_ch2_set((int)r, level);
    } else {
        usb_send_str("Usage: trig2 raw <code> | trig2 <range> <level>\r\n");
        return;
    }

    uint16_t code = scope_trigger_ch2_last();
    /* Duty ratio against the stock ARR (4094). The RC-filtered mean at the pin
     * is duty * 3.3V, so the arithmetic matches `trig`'s DAC1 line — but this
     * is a PWM average, not a DAC output, so it is only valid once filtered. */
    uint32_t mv = ((uint32_t)code * 3300u) / 4095u;
    usb_debug_printf("TMR13_C1DT(PA6) = code %u (0x%03X)  duty ~%lu.%03lu V after RC\r\n",
                     code, code, (unsigned long)(mv / 1000), (unsigned long)(mv % 1000));
}

void cmd_fpga_cmd(const char *args)
{
    uint32_t cmd_hi = 0, cmd_lo = 0;
    const char *space = strchr(args, ' ');
    fpga_diag_snapshot_t before;

    char cmd_str[8];
    if (space) {
        int len = space - args;
        if (len >= (int)sizeof(cmd_str)) { usb_send_str("ERR\r\n"); return; }
        memcpy(cmd_str, args, len);
        cmd_str[len] = '\0';
        /* HEX, not parse_int(). FPGA command bytes are written in hex
         * everywhere in this project; parsing them as decimal made
         * `fpga cmd 05 14` transmit 0x050E and print a plausible
         * confirmation of the wrong command (EXP-23). */
        if (parse_hex32(space + 1, &cmd_lo) != 0 || cmd_lo > 0xFF) {
            usb_send_str("Usage: fpga cmd <hi> <lo>   (HEX)\r\n");
            return;
        }
    } else {
        strncpy(cmd_str, args, sizeof(cmd_str) - 1);
        cmd_str[sizeof(cmd_str) - 1] = '\0';
    }

    if (parse_hex32(cmd_str, &cmd_hi) != 0) {
        usb_send_str("Usage: fpga cmd <hi> <lo>   (HEX)\r\n");
        return;
    }

    if (space == NULL) {
        /* Single combined value form: fpga cmd 0x0509 */
        if (cmd_hi > 0xFFFF) {
            usb_send_str("Usage: fpga cmd <hi> <lo>   (HEX)\r\n");
            return;
        }
        cmd_lo = cmd_hi & 0xFF;
        cmd_hi = (cmd_hi >> 8) & 0xFF;
    } else if (cmd_hi > 0xFF) {
        usb_send_str("Usage: fpga cmd <hi> <lo>   (HEX)\r\n");
        return;
    }

    fpga_diag_snapshot_take(&before);
    if (!usart_send_cmd_reporting((uint8_t)cmd_hi, (uint8_t)cmd_lo)) return;

    /* Wait for response */
    vTaskDelay(pdMS_TO_TICKS(200));
    fpga_diag_print_delta(&before);
}

/* `fpga usart [on|off]` — bring USART2 up AFTER config (stock's order) and
 * report the registers back. Coldtrace builds leave it dark; stock's channel
 * configuration command lives on this bus. Prints RX counters so "did anything
 * answer" is a number rather than an impression. */
void cmd_fpga_usart(const char *args)
{
    char buf[160];
    while (*args == ' ') args++;
    if      (strncmp(args, "on",  2) == 0) fpga_usart_scope_enable(true);
    else if (strncmp(args, "off", 3) == 0) fpga_usart_scope_enable(false);
    else if (*args) { usb_send_str("usage: fpga usart [on|off]\r\n"); return; }

    uint32_t c1 = fpga_usart_ctrl1();
    snprintf(buf, sizeof(buf),
             "USART2 CTRL1=%08lX BAUDR=%08lX  UEN=%u TE=%u RE=%u RDBFIEN=%u\r\n"
             "  rx_bytes=%u echo_frames=%u\r\n",
             (unsigned long)c1, (unsigned long)fpga_usart_baudr(),
             (unsigned)((c1 >> 13) & 1), (unsigned)((c1 >> 3) & 1),
             (unsigned)((c1 >> 2) & 1),  (unsigned)((c1 >> 5) & 1),
             (unsigned)fpga.rx_byte_count, (unsigned)fpga.echo_count);
    usb_send_str(buf);
}

/* `fpga autowait [ms]` — AUTO-mode budget to wait for a PC0 edge before the
 * free-run fallback read (EXP-42). A read starts a capture; completion pulses
 * PC0; the compiled 25 ms is shorter than the capture at slow timebases. */
void cmd_fpga_autowait(const char *args)
{
    char buf[112];
    while (*args == ' ') args++;
    if (*args) {
        uint32_t ms = 0;
        if (parse_int(args, &ms) != 0 || ms > 60000u) {
            usb_send_str("usage: fpga autowait [ms 0..60000, 0 = derive from timebase]\r\n"); return;
        }
        fpga_acq_auto_wait_set((uint16_t)ms);
    }
    snprintf(buf, sizeof(buf), "acq AUTO waits %u ms for a trigger before an untriggered record (%s, rate idx 0x%02X)\r\n",
             (unsigned)fpga_acq_auto_wait_get(),
             fpga_acq_auto_wait_is_override() ? "override" : "derived from timebase",
             fpga_acq_rate_idx_get());
    usb_send_str(buf);
}

/* `fpga pollgap [ms]` — poll cadence after the poll start (EXP-54). */
void cmd_fpga_pollgap(const char *args)
{
    while (*args == ' ') args++;
    if (*args) {
        uint32_t ms = 0;
        if (parse_int(args, &ms) != 0 || ms < 1u || ms > 1000u) { usb_send_str("usage: fpga pollgap [ms 1..1000]\r\n"); return; }
        fpga_acq_poll_gap_set((uint16_t)ms);
    }
    usb_debug_printf("acq poll gap %u ms; poll starts %u ms after a handover (%s); last handover after %lu poll reads, %lu total\r\n",
                     (unsigned)fpga_acq_poll_gap_get(), (unsigned)fpga_acq_post_edge_get(),
                     "derived, dsp/acq_timing.h",
                     (unsigned long)fpga.acq_polls_last, (unsigned long)fpga.acq_poll_reads);
}

/* `fpga scope coupling` — READ-ONLY: each channel's coupling label beside the
 * relay pin it is supposed to describe (PD12 CH1 / PD13 CH2, HIGH = DC) and
 * the pin's GPIO mode. Change it with the property chN.coupling (or the keys):
 * that is the path under test, and a second writer here would be a second
 * place for the label and the relay to disagree. */
void cmd_fpga_scope_coupling(const char *args)
{
    (void)args;
    const scope_state_t *ss = scope_state_get();
    for (int ch = 0; ch < 2; ch++) {
        const channel_state_t *c = ch ? &ss->ch2 : &ss->ch1;
        unsigned pin = ch ? 13u : 12u;
        unsigned mode = (unsigned)((GPIOD->cfghr >> ((pin - 8u) * 4u)) & 0xFu);
        unsigned odt = (unsigned)((GPIOD->odt >> pin) & 1u);
        bool gpio_out = (mode >= 1u && mode <= 3u);
        const char *relay = !gpio_out ? "not-gpio" : odt ? "DC" : "AC";
        bool agree = gpio_out && ((c->coupling == COUPLING_DC) == (odt != 0u));
        usb_debug_printf("CH%d label %s  PD%u mode 0x%X odt %u -> relay %s  %s\r\n",
                         ch + 1, coupling_labels[c->coupling], pin, mode, odt, relay,
                         agree ? "AGREE" : "DISAGREE");
    }
}

/* `fpga acqbr [0-7|off]` — SPI3 clock divider the acq task sets before each pair (EXP-46). */
void cmd_fpga_acqbr(const char *args)
{
    while (*args == ' ') args++;
    if (*args) {
        if (strncmp(args, "off", 3) == 0) fpga_acq_read_br_set(0xFF);
        else {
            uint32_t br = 0;
            if (parse_int(args, &br) != 0 || br > 7u) { usb_send_str("usage: fpga acqbr [0-7|off]\r\n"); return; }
            fpga_acq_read_br_set((uint8_t)br);
        }
    }
    if (fpga_acq_read_br_get() == 0xFF) usb_send_str("acq read clock: not set by the task (whatever is in force)\r\n");
    else usb_debug_printf("acq read clock: br=%u (/%u) set before each pair\r\n", fpga_acq_read_br_get(), 2u << fpga_acq_read_br_get());
}

/* Raw SPI3 byte exchange on the shared FPGA bus (defined later in this file). */
static uint8_t spi3_raw_xfer(uint8_t tx);

/* `fpga scope zero` — does each channel's record sit where its 0 V is
 * expected? Trace, ground tag and measurements all use one zero, the
 * page formula (scope_measure_zero_for: 100 + pos); the record sits there
 * only if the offset DAC was written for this channel's (bank, range). Prints
 * the expected code, the record's middle-500 mean and the difference in
 * codes, divisions and volts, and the DAC write in force, flagged when it
 * was for another bank or range (scope_vpos leaves the DAC alone where the
 * page has no clean pair). Take it with the input open. */
void cmd_fpga_scope_zero(const char *args)
{
    (void)args;
    const scope_state_t *ss = scope_state_get();
    const volatile uint8_t *bufs[2] = { fpga_get_ch1_buf(), fpga_get_ch2_buf() };
    const uint8_t bank_now = calpage_bank_select(fpga_acq_rate_idx_get(), fpga.channel_mask);
    for (uint8_t ch = 1u; ch <= 2u; ch++) {
        const channel_state_t *cs = (ch == 1u) ? &ss->ch1 : &ss->ch2;
        const uint8_t z = scope_measure_zero_for(scope_vpos_active(), cs->position).v;
        const volatile uint8_t *b = bufs[ch - 1u];
        uint32_t sum = 0;
        for (unsigned i = 262u; i < 762u; i++) sum += b ? b[i] : 0u;
        const int mean_x10 = (int)((sum * 10u + 250u) / 500u);
        const int d_x10 = mean_x10 - 10 * (int)z;
        const float cpd = scope_cal_codes_per_div(ch, cs->vdiv_idx);
        const float vpc = scope_cal_volts_per_count(ch, cs->vdiv_idx);
        const int div_x100 = (cpd > 0.0f) ? (int)((float)d_x10 * 10.0f / cpd) : 0;
        const int mv = (int)((float)d_x10 * 0.1f * vpc * 1000.0f);
        usb_debug_printf("CH%u: en %u range %u pos %d zero %u, record mean %d.%d = %+d.%d cnt "
                         "(%+d.%02d div, %+d mV)\r\n",
                         (unsigned)ch, (unsigned)cs->enabled, (unsigned)cs->vdiv_idx,
                         (int)cs->position, (unsigned)z, mean_x10 / 10, mean_x10 % 10,
                         d_x10 / 10, abs(d_x10 % 10), div_x100 / 100, abs(div_x100 % 100), mv);
        uint8_t bk = 0, rg = 0;
        uint16_t dc = 0;
        if (!scope_vpos_last(ch, &bk, &rg, &dc))
            usb_debug_printf("     dac never written (page %s)\r\n",
                             scope_vpos_active() ? "in use" : "not in use");
        else
            usb_debug_printf("     dac bank %u range %u code %u%s (now bank %u range %u)\r\n",
                             (unsigned)bk, (unsigned)rg, (unsigned)dc,
                             (bk != bank_now || rg != cs->vdiv_idx) ? "  STALE" : "",
                             (unsigned)bank_now, (unsigned)cs->vdiv_idx);
    }
}

void cmd_fpga_scope_freq(const char *args)
{
    uint32_t reps = 10;
    /* Checked: a failed parse_int() still writes 0 into reps, so a bad
     * argument silently ran ONE rep and reported `answered 1/1` as though a
     * single rep had been requested. */
    if (args && *args && parse_int(args, &reps) != 0) {
        usb_send_str("Usage: fpga scope freq [reps 1-100]\r\n");
        return;
    }
    if (reps == 0u) reps = 1u;
    if (reps > 100u) reps = 100u;

    const scope_state_t *ss = scope_state_get();
    const float fs = scope_timebase_sample_rate(ss->timebase_idx);
    const uint8_t in_force = fpga_acq_rate_idx_get();

    usb_debug_printf("timebase 0x%02X -> %lu S/s  (reg 0x01 in force: 0x%02X)\r\n",
                     (unsigned)ss->timebase_idx, (unsigned long)(fs + 0.5f),
                     (unsigned)in_force);

    /* The whole point of printing both: on 2026-08-19 these were found to
     * disagree on a stock boot (display 0x0A, hardware 0x08), which would
     * have made every derived Hz wrong by the ratio of the two rates. A
     * frequency computed from the wrong rate is not worth printing. */
    if (in_force != ss->timebase_idx) {
        usb_send_str("MISMATCH: the display's timebase is not the one the "
                     "hardware is sampling at — refusing to derive a "
                     "frequency. Use `set tb.code <code>`.\r\n");
        return;
    }

    if (fs <= 0.0f) {
        usb_send_str("no trustworthy rate for this code — no frequency is "
                     "derivable (see scope_timebase.h)\r\n");
        return;
    }

    const volatile uint8_t *ch1 = fpga_get_ch1_buf();
    if (!ch1) { usb_send_str("FPGA not initialized\r\n"); return; }

    uint32_t answered = 0;
    for (uint32_t i = 0; i < reps; i++) {
        scope_freq_t r;
        const bool ok = scope_freq_estimate((const uint8_t *)ch1,
                                            FPGA_ADC_BUF_SIZE, fs, &r);
        const uint32_t mhz = (uint32_t)(r.hz * 1000.0f + 0.5f);
        usb_debug_printf("  %2lu  %-8s  %6lu.%03lu Hz  bin %4lu.%02lu  "
                         "sharp %lu.%02lu  win %u\r\n",
                         (unsigned long)i,
                         ok ? "ANSWER" : "refuse",
                         (unsigned long)(mhz / 1000u), (unsigned long)(mhz % 1000u),
                         (unsigned long)r.bin,
                         (unsigned long)((r.bin - (float)(uint32_t)r.bin) * 100.0f),
                         (unsigned long)r.sharpness,
                         (unsigned long)((r.sharpness -
                                          (float)(uint32_t)r.sharpness) * 100.0f),
                         r.window);
        if (ok) answered++;
        usb_delay_ms(60);
    }
    usb_debug_printf("answered %lu/%lu\r\n",
                     (unsigned long)answered, (unsigned long)reps);
}

/* `fpga scope cal` — print what the firmware believes about vertical scale.
 *
 * This exists so the number on the screen can be checked against the number
 * in the source without a rebuild, and so a bench session can see at a glance
 * which ranges are cross-validated and which are only provisional. It reads
 * the table; it cannot change it. Recalibration is a source edit plus a
 * rebuild, deliberately, because a runtime-adjustable gain that nobody
 * records is how an instrument ends up lying with confidence.
 */
void cmd_fpga_scope_cal(void)
{
    /* Per channel: this unit's stock page (nominal ladder / g, scope_cal.h)
     * when `cal page` found a valid amplitude mean, else the compiled table. */
    for (uint8_t ch = 1u; ch <= 2u; ch++) {
        const float g = scope_cal_stock_gain(ch);
        if (g > 0.0f)
            usb_debug_printf("CH%u: this unit's cal page, nominal / g, g=%lu.%04lu\r\n",
                             (unsigned)ch, (unsigned long)g,
                             (unsigned long)((g - (float)(unsigned long)g) * 10000.0f + 0.5f));
        else
            usb_debug_printf("CH%u: compiled table (bench unit #1, EXP-08 2026-08-17)\r\n",
                             (unsigned)ch);
    }
    usb_debug_printf("source scale %d.%03d  nominal counts/div %d (drawn: codes/div)\r\n",
                     (int)SCOPE_CAL_SOURCE_SCALE,
                     (int)((SCOPE_CAL_SOURCE_SCALE - (int)SCOPE_CAL_SOURCE_SCALE)
                           * 1000.0f + 0.5f),
                     (int)SCOPE_CAL_COUNTS_PER_DIV);
    usb_send_str("rng  CH1 uV/ct  CH1 V/div  c/div  CH2 uV/ct  CH2 V/div  c/div  tier\r\n");

    for (uint8_t r = 0; r < SCOPE_CAL_RANGE_COUNT; r++) {
        char l1[12], l2[12];
        scope_cal_range_label_bnc(1u, r, l1, sizeof(l1));   /* BNC: this is the */
        scope_cal_range_label_bnc(2u, r, l2, sizeof(l2));   /* cal table        */

        const scope_cal_tier_t t = scope_cal_get_tier(1u, r);
        const char *tn = (t == SCOPE_CAL_FACTORY)     ? "factory (CH1)"
                       : (t == SCOPE_CAL_MEASURED)    ? "measured"
                       : (t == SCOPE_CAL_PROVISIONAL) ? "provisional"
                                                      : "none (rails)";

        /* Microvolts per count keeps this integer-only — the shell printf is
         * not guaranteed to carry %f on every build. */
        const unsigned c1 = (unsigned)(scope_cal_codes_per_div(1u, r) * 10.0f + 0.5f);
        const unsigned c2 = (unsigned)(scope_cal_codes_per_div(2u, r) * 10.0f + 0.5f);
        usb_debug_printf("%2u  %9lu  %9s  %2u.%u  %9lu  %9s  %2u.%u  %s\r\n",
                         (unsigned)r,
                         (unsigned long)(scope_cal_mv_per_count(1u, r) * 1000.0f + 0.5f),
                         l1, c1 / 10u, c1 % 10u,
                         (unsigned long)(scope_cal_mv_per_count(2u, r) * 1000.0f + 0.5f),
                         l2, c2 / 10u, c2 % 10u,
                         tn);
    }

    usb_debug_printf("table is at the BNC; on-screen volts are at the probe tip: "
                     "probe CH1 x%u  CH2 x%u\r\n",
                     (unsigned)(scope_cal_probe_factor(1u) + 0.5f),
                     (unsigned)(scope_cal_probe_factor(2u) + 0.5f));
    usb_send_str("\r\ntimebase (reg 0x01) -> sample rate\r\n");
    usb_send_str("code    S/s    s/div   tier\r\n");
    for (uint8_t c = 0; c < SCOPE_TIMEBASE_CODE_COUNT; c++) {
        const float fs = scope_timebase_sample_rate(c);
        if (fs <= 0.0f)
            continue;                      /* skip the many unmeasured codes */
        char lbl[12];
        /* Display label (C5 zoom at 0x00-0x03), per-converter record: the
         * merged record reads the same label at these codes. */
        scope_timebase_view_label(c, false, lbl, sizeof(lbl));
        const scope_tb_tier_t t = scope_timebase_get_tier(c);
        usb_debug_printf("0x%02X  %7lu  %7s  %s\r\n",
                         (unsigned)c, (unsigned long)(fs + 0.5f), lbl,
                         (t == SCOPE_TB_MEASURED) ? "measured" : "provisional");
    }
    usb_send_str("codes not listed have no trustworthy rate, for three "
                 "DIFFERENT reasons:\r\n"
                 "  0x06-0x09  INCOHERENT — measured, and all four return the "
                 "same ~1.2-1.6 kS/s\r\n"
                 "             regardless of the code, with reads that do not "
                 "reproduce (EXP-15).\r\n"
                 "  0x0A-0x0C  need a faster source than ours — a statement "
                 "about our bench,\r\n"
                 "             not about the device.\r\n"
                 "  the rest   never measured.\r\n"
                 "See scope_timebase.h\r\n");
}

void cmd_fpga_diag_clear(void)
{
    fpga_diag_clear();
    usb_send_str("FPGA diagnostics cleared\r\n");
}

/* EXPERIMENTAL (experimental/esp32-bringup) — UNTESTED. Hand the SPI3 bus to
 * an external SSPI master (ESP32) on the back-side test pads. Re-flash to undo. */
void cmd_fpga_bus_release(void)
{
    fpga_bus_release();
    usb_send_str("SPI3 bus RELEASED to external master.\r\n");
    usb_send_str("  PB3(SCK)/PB5(MOSI)/PB6(CS) -> Hi-Z, MCU off the bus.\r\n");
    usb_send_str("  PB4(MISO) input (FPGA-driven, shared read).\r\n");
    usb_send_str("  PC6=HIGH (SPI en), PB11=HIGH (active), PC9 power-hold kept.\r\n");
    usb_send_str("  ESP32 may now drive SSPI. Re-flash/power-cycle to reclaim.\r\n");
}

void cmd_fpga_bus_reacquire(void)
{
    fpga_spi3_bus_reacquire();
    usb_send_str("SPI3 bus REACQUIRED by MCU.\r\n");
    usb_send_str("  PB3(SCK)/PB5(MOSI) -> SPI3 AF, PB6(CS) -> GPIO out HIGH.\r\n");
    usb_send_str("  PB4(MISO) input pull-up (stock idle), PC6=HIGH, SPE=1.\r\n");
    usb_send_str("  Config port left untouched. `fpga reinit` now drives it.\r\n");
}

/*
 * FPGA design slot (util/fpga_slot.h) and runtime reload (fpga_reload()).
 *   fpga slot [info]                          what is stored, and whether its CRC holds
 *   fpga slot put <len> <crc32hex> <name> [force]   erase, start an upload
 *   fpga slot data <off> <hex>                <= 48 bytes at the next expected offset
 *   fpga slot pos | end | clear
 *   fpga reload [builtin|slot]                PC6 power cycle of the FPGA, then upload
 * scripts/fpga_put.py drives this; every reply starts "fpga slot:" / "fpga reload:"
 * and a failure says "err".
 */
static fslot_upload_t s_fslot_up;
static char s_fpga_loaded[FSLOT_NAME_LEN + 88];   /* "" until a reload: then what the boot upload sent */

static void fslot_tick(void)
{
    health_checkin_self();
    vTaskDelay(1);
}

static int fslot_hexbyte(const char *p)
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

void cmd_fpga_slot(const char *args)
{
    static const char usage[] =
        "Usage: fpga slot [info] | put <len> <crc32hex> <name> [force] | data <off> <hex> | pos | end | boot on|off | clear\r\n";
    while (*args == ' ') args++;
    if (*args == '\0' || strcmp(args, "info") == 0) {
        fslot_info_t info;
        fslot_status_t st = fslot_info(&info);
        if (st != FSLOT_OK) { usb_debug_printf("fpga slot: err %s\r\n", fslot_status_str(st)); return; }
        const flash_region_t *r = flash_region_get(FSLOT_REGION);
        if (info.state == FSLOT_VALID) {
            st = fslot_verify(&info, fslot_tick);
            usb_debug_printf("fpga slot: valid '%s' %lu B crc %08lX payload %s @0x%06lX boot %s\r\n", info.name,
                             (unsigned long)info.len, (unsigned long)info.crc,
                             st == FSLOT_OK ? "OK" : "MISMATCH", (unsigned long)(r ? r->start : 0),
                             info.boot ? "on" : "off");
        } else {
            bool blank = false;
            st = fslot_span_blank(&blank, fslot_tick);
            usb_debug_printf("fpga slot: %s; span %s\r\n",
                             info.state == FSLOT_EMPTY ? "none stored" : "damaged slot",
                             st != FSLOT_OK ? "unreadable" : blank ? "blank" : "NOT blank (foreign data or a torn upload)");
        }
#if FPGA_CONFIG_B
        if (s_fpga_loaded[0] == '\0') {
            const char *b = fpga_boot_design();
            const uint8_t live = fpga_boot_live();
            if (live == 2) snprintf(s_fpga_loaded, sizeof s_fpga_loaded, "nothing (boot found a running design that PC6 did not remove; it stays)");
            else if (b)    snprintf(s_fpga_loaded, sizeof s_fpga_loaded, "slot '%s' (boot%s)", b, live ? ", after removing a running design" : "");
            else           snprintf(s_fpga_loaded, sizeof s_fpga_loaded, "builtin (boot%s)", live ? ", after removing a running design" : "");
        }
#endif
        usb_debug_printf("fpga slot: FPGA was sent: %s\r\n", s_fpga_loaded);
        return;
    }
    if (strcmp(args, "boot on") == 0 || strcmp(args, "boot off") == 0) {
        fslot_status_t st = fslot_set_boot(args[6] == 'n');
        if (st == FSLOT_OK) usb_debug_printf("fpga slot: ok boot %s\r\n", args[6] == 'n' ? "on (the slot loads at power-on)" : "off (the compiled-in design loads at power-on)");
        else usb_debug_printf("fpga slot: err %s\r\n", fslot_status_str(st));
        return;
    }
    if (strncmp(args, "put ", 4) == 0) {
        uint32_t len, crc;
        const char *p = args + 4;
        char *e;
        len = strtoul(p, &e, 10);
        if (e == p || *e != ' ') { usb_send_str(usage); return; }
        char hex[9]; uint32_t k = 0; p = e + 1;
        while (k < 8u && p[k] != '\0' && p[k] != ' ') { hex[k] = p[k]; k++; }
        hex[k] = '\0';
        if (k == 0u || p[k] != ' ' || parse_hex32(hex, &crc) != 0) { usb_send_str(usage); return; }
        p += k + 1;
        char name[FSLOT_NAME_LEN + 1]; k = 0;
        while (k < FSLOT_NAME_LEN && p[k] != '\0' && p[k] != ' ') { name[k] = p[k]; k++; }
        name[k] = '\0';
        if (k == 0u) { usb_send_str(usage); return; }
        while (p[k] != '\0' && p[k] != ' ') k++;
        const bool force = (strcmp(p + k, " force") == 0);
        /* First use of this flash range: never erase data nobody identified. */
        fslot_info_t info; bool blank = false;
        fslot_status_t st = fslot_info(&info);
        if (st == FSLOT_OK && info.state != FSLOT_VALID) st = fslot_span_blank(&blank, fslot_tick);
        if (st != FSLOT_OK) { usb_debug_printf("fpga slot: err %s\r\n", fslot_status_str(st)); return; }
        if (info.state != FSLOT_VALID && !blank && !force) {
            usb_send_str("fpga slot: err the slot's flash range is not blank and holds no valid design; "
                         "look at it first, then repeat with `force`\r\n");
            return;
        }
        st = fslot_put_begin(&s_fslot_up, len, crc, name, fslot_tick);
        if (st == FSLOT_OK) usb_debug_printf("fpga slot: ok put %lu\r\n", (unsigned long)len);
        else usb_debug_printf("fpga slot: err %s\r\n", fslot_status_str(st));
        return;
    }
    if (strncmp(args, "data ", 5) == 0) {
        const char *p = args + 5;
        char *e;
        uint32_t off = strtoul(p, &e, 10);
        if (e == p || *e != ' ') { usb_send_str(usage); return; }
        p = e + 1;
        uint8_t buf[FSLOT_CHUNK_MAX];
        uint32_t n = 0;
        while (p[0] != '\0' && p[1] != '\0') {
            int b = fslot_hexbyte(p);
            if (b < 0 || n >= sizeof buf) { usb_send_str("fpga slot: err bad hex\r\n"); return; }
            buf[n++] = (uint8_t)b;
            p += 2;
        }
        if (n == 0u || *p != '\0') { usb_send_str("fpga slot: err bad hex\r\n"); return; }
        fslot_status_t st = fslot_put_data(&s_fslot_up, off, buf, n);
        if (st == FSLOT_OK) usb_debug_printf("fpga slot: ok %lu\r\n", (unsigned long)s_fslot_up.received);
        else usb_debug_printf("fpga slot: err %s (expect %lu)\r\n", fslot_status_str(st),
                              (unsigned long)s_fslot_up.received);
        return;
    }
    if (strcmp(args, "pos") == 0) {
        usb_debug_printf("fpga slot: pos %lu %s\r\n", (unsigned long)s_fslot_up.received,
                         s_fslot_up.active ? "active" : "idle");
        return;
    }
    if (strcmp(args, "end") == 0) {
        fslot_status_t st = fslot_put_end(&s_fslot_up, fslot_tick);
        if (st == FSLOT_OK) usb_send_str("fpga slot: ok committed\r\n");
        else usb_debug_printf("fpga slot: err %s (nothing committed)\r\n", fslot_status_str(st));
        return;
    }
    if (strcmp(args, "clear") == 0) {
        fslot_status_t st = fslot_clear();
        usb_debug_printf(st == FSLOT_OK ? "fpga slot: ok cleared\r\n" : "fpga slot: err %s\r\n", fslot_status_str(st));
        return;
    }
    usb_send_str(usage);
}

#if FPGA_CONFIG_B
static bool fslot_payload(void *ctx, uint32_t off, uint8_t *buf, uint32_t n)
{
    (void)ctx;
    return fslot_read(off, buf, n) == FSLOT_OK;
}
#endif

void cmd_fpga_reload(const char *args)
{
#if FPGA_CONFIG_B
    while (*args == ' ') args++;
    const bool slot = (strcmp(args, "slot") == 0);
    if (!slot && *args != '\0' && strcmp(args, "builtin") != 0) {
        usb_send_str("Usage: fpga reload [builtin|slot]\r\n");
        return;
    }
    fslot_info_t info;
    memset(&info, 0, sizeof info);
    if (slot) {
        /* Check the whole stored design BEFORE the FPGA loses its supply. */
        fslot_status_t st = fslot_verify(&info, fslot_tick);
        if (st != FSLOT_OK) {
            usb_debug_printf("fpga reload: err slot: %s (FPGA not touched)\r\n", fslot_status_str(st));
            return;
        }
    }
    fpga_reload_report_t rep;
    const fpga_reload_status_t rs = slot ? fpga_reload(fslot_payload, NULL, info.len, &rep)
                                         : fpga_reload(NULL, NULL, 0, &rep);
    switch (rs) {
    case FPGA_RELOAD_OK:
        if (slot) snprintf(s_fpga_loaded, sizeof s_fpga_loaded, "slot '%s'", info.name);
        else      snprintf(s_fpga_loaded, sizeof s_fpga_loaded, "builtin");
        usb_debug_printf("fpga reload: ok %s, %lu B sent, STATUS %08lX (DONE)\r\n", s_fpga_loaded,
                         (unsigned long)rep.sent, (unsigned long)rep.status);
        break;
    case FPGA_RELOAD_NOT_EMPTY:
        usb_debug_printf("fpga reload: err the FPGA did not power down (port answered %08lX, not 0120681B); "
                         "nothing sent, the old design still runs. A design with input clamps on (stock) "
                         "needs a power cycle of the unit.\r\n", (unsigned long)rep.idcode_before);
        break;
    case FPGA_RELOAD_SRC_FAIL:
        snprintf(s_fpga_loaded, sizeof s_fpga_loaded, "nothing (empty)");
        usb_debug_printf("fpga reload: err the flash read failed after %lu B; the FPGA is EMPTY. "
                         "`fpga reload builtin` loads the compiled-in design.\r\n", (unsigned long)rep.sent);
        break;
    case FPGA_RELOAD_NOT_DONE:
        snprintf(s_fpga_loaded, sizeof s_fpga_loaded, "nothing (empty)");
        usb_debug_printf("fpga reload: err upload sent (%lu B) but DONE is not set, STATUS %08lX; the FPGA is "
                         "EMPTY. `fpga reload builtin` loads the compiled-in design.\r\n",
                         (unsigned long)rep.sent, (unsigned long)rep.status);
        break;
    default:
        usb_send_str("fpga reload: err not supported by this build's upload variant\r\n");
        break;
    }
#else
    (void)args;
    usb_send_str("fpga reload: err not compiled (needs FPGA_CONFIG_B)\r\n");
#endif
}

/* fpga configbb — run the GPIO bit-bang SSPI config on demand (H7 step 1).
 * Fires the SAME bit-bang loader coldtrace uses at boot, but from the shell on
 * guest-bringup-bb, so bit-bang (succeeds) and hardware-SPI (`fpga reinit`, fails)
 * run from ONE build with identical off-SPI pin state. If bit-bang hits DONE_FINAL
 * here, the wall is in the SPI3 clocking character (LA-diffable), not off-bus.
 * Needs a cold/open port (power-cycle first); a successful config CLOSES the port,
 * so re-run only after another power cycle. */
void cmd_fpga_configbb(void)
{
#if FPGA_CONFIG_B
    usb_send_str("bit-bang SSPI config (05-less V0.4 framing) — firing...\r\n");
    (void)fpga_bitbang_config_sequence();
    uint32_t sr = ((uint32_t)fpga.cfg_status_reg[0] << 24) |
                  ((uint32_t)fpga.cfg_status_reg[1] << 16) |
                  ((uint32_t)fpga.cfg_status_reg[2] << 8) |
                  (uint32_t)fpga.cfg_status_reg[3];
    usb_debug_printf("post-upload STATUS(0x41): %02X %02X %02X %02X (raw=%08lX)\r\n",
                     fpga.cfg_status_reg[0], fpga.cfg_status_reg[1],
                     fpga.cfg_status_reg[2], fpga.cfg_status_reg[3], sr);
    usb_debug_printf("  DONE_FINAL(bit13)=%s  flags:%s%s%s%s\r\n",
                     (sr & (1u << 13)) ? "YES -- CONFIG TOOK" : "no -- the wall",
                     (sr & (1u << 0))  ? " CRC_ERR" : "",
                     (sr & (1u << 2))  ? " ID_FAIL" : "",
                     (sr & (1u << 12)) ? " GWVLD"   : "",
                     (sr & (1u << 15)) ? " READY"   : "");
    usb_debug_printf("  close IDCODE(0x11)=%s (silent=port CLOSED=configured)\r\n",
                     fpga.probe_id_bit_close == 0 ? "0x0120681B ANSWERS (still open)"
                                                  : "silent/unanchored");
#else
    usb_send_str("configbb: not compiled — rebuild with FPGA_CONFIG_B "
                 "(make guest-bringup-bb)\r\n");
#endif
}

void cmd_fpga_wire_words(const char *args)
{
    char buf[192];
    char *saveptr = NULL;
    char *tok;
    uint32_t value;
    uint16_t words[48];
    size_t count = 0;
    fpga_diag_snapshot_t before;

    if (args == NULL || *args == '\0' || strlen(args) >= sizeof(buf)) {
        usb_send_str("Usage: fpga wire words <word1> [word2 ...]\r\n");
        return;
    }

    strcpy(buf, args);

    /* Parse the WHOLE list before transmitting any of it. This used to send
     * each word as it was parsed, so `fpga wire words 0501 0502 oops` drove
     * two words onto the wire and then printed nothing but a usage line — the
     * operator reads "syntax error, nothing happened" while the FPGA has been
     * half-driven. Validate first, then commit. */
    tok = strtok_r(buf, " \t", &saveptr);
    while (tok != NULL) {
        if (count >= (sizeof(words) / sizeof(words[0]))) {
            usb_debug_printf("ERR: at most %u words\r\n",
                             (unsigned)(sizeof(words) / sizeof(words[0])));
            return;
        }
        if (parse_int(tok, &value) != 0 || value > 0xFFFF) {
            usb_send_str("Usage: fpga wire words <word1> [word2 ...]\r\n");
            return;
        }
        words[count++] = (uint16_t)value;
        tok = strtok_r(NULL, " \t", &saveptr);
    }

    fpga_diag_snapshot_take(&before);
    for (size_t i = 0; i < count; i++)
        fpga_wire_send_word(words[i], 15);

    usb_debug_printf("Wire words sent: %u\r\n", (unsigned)count);
    fpga_diag_print_delta(&before);
}

void cmd_fpga_meter_reinit(const char *args)
{
    extern volatile uint8_t meter_submode;
    uint32_t submode = meter_submode;

    if (args && *args) {
        if (parse_int(args, &submode) != 0 || submode >= METER_SUBMODE_COUNT) {
            usb_debug_printf("Usage: fpga meter reinit [0-%u]\r\n",
                             (unsigned)(METER_SUBMODE_COUNT - 1));
            return;
        }
    }

    /* The function is a property (meter.func): set through the input task.
     * With the meter link up that set has the meter task switch the function
     * in the background, so wait for it to finish before the transport is
     * re-initialised from here: one actor on the meter link at a time. */
    if (submode != meter_submode) {
        if (!model_shell_request(PROP_ID_METER_FUNC, (int32_t)submode)) return;
        int left = 300;                             /* 6 s: a transition takes up to ~4.5 s */
        while (left-- > 0 && (fpga_meter_mode_request_pending() || fpga_meter_transition_busy()))
            vTaskDelay(pdMS_TO_TICKS(20));
        if (fpga_meter_mode_request_pending() || fpga_meter_transition_busy()) {
            usb_send_str("Meter still switching function after 6 s: not re-initialised\r\n");
            return;
        }
    }
    fpga_meter_reinit((uint8_t)submode);
    usb_debug_printf("Meter reinit complete: submode %lu (%s)\r\n",
                     submode, meter_submode_name((uint8_t)submode));
}

void cmd_spi3_read(const char *args)
{
    uint32_t len = 64;
    /* Checked: `spi3 read all` used to set len = 0 and print
     * "CH1 buffer (0 bytes):" with nothing under it — an empty dump that is
     * indistinguishable from a genuinely empty capture, from the one command
     * whose header comment is about exactly that confusion. */
    if (args && *args && parse_int(args, &len) != 0) {
        usb_send_str("Usage: spi3 read [len]\r\n");
        return;
    }
    if (len > FPGA_ADC_BUF_SIZE) len = FPGA_ADC_BUF_SIZE;

    const volatile uint8_t *ch1 = fpga_get_ch1_buf();
    if (!ch1) {
        usb_send_str("FPGA not initialized\r\n");
        return;
    }

    /*
     * SNAPSHOT FIRST. This used to hex-print `ch1[i]` straight out of the live
     * acquisition buffer, one byte at a time, interleaved with USB output — so
     * a full 1024-byte dump walked the buffer for ~270 ms while the acq task
     * refilled it every ~29 ms, rewriting it about nine times underneath.
     *
     * The records that came out were spliced, and it was not subtle: 11 of 12
     * consecutive dumps of one unchanged 500 Hz tone were spectrally torn
     * (mean sharpness 0.520), against 7 of 12 for `spi3 opread`, which already
     * snapshots. The tearing rate tracks READ DURATION, which is the signature
     * of the reader, not the device.
     *
     * That artifact reached a published conclusion — EXP-16 originally reported
     * "a fifth to a half of capture records are torn" as a property of
     * acquisition. It was a property of this loop. Corrected 2026-08-19.
     *
     * Copy under one pass, then print at leisure. The shared shell scratch
     * (file scope) rather than the stack: this runs on the shell task and 1 KB
     * is more than it should borrow.
     */
    uint8_t *snap = shell_bus_scratch;
    for (uint32_t i = 0; i < len; i++)
        snap[i] = ch1[i];

    usb_debug_printf("CH1 buffer (%lu bytes):\r\n", len);
    for (uint32_t i = 0; i < len; i++) {
        if (i % 16 == 0) usb_debug_printf("%04lX:", i);
        usb_debug_printf(" %02X", snap[i]);
        if (i % 16 == 15 || i == len - 1) usb_send_str("\r\n");
    }
}

static void spi3_frame_dump(const char *name, const uint8_t *buf)
{
    usb_debug_printf("%s (%u bytes):\r\n", name, (unsigned)FPGA_ADC_BUF_SIZE);
    for (uint32_t i = 0; i < FPGA_ADC_BUF_SIZE; i++) {
        if (i % 16 == 0) usb_debug_printf("%04lX:", i);
        usb_debug_printf(" %02X", buf[i]);
        if (i % 16 == 15) usb_send_str("\r\n");
    }
}

/* spi3 frame — one COHERENT two-channel snapshot of the acquisition buffers,
 * plus the render-path facts a host needs to reproduce what the screen shows:
 * the frame generation (proves successive grabs are distinct captures) and
 * the soft-trigger offset computed by the RENDERER'S OWN code
 * (scope_ui_soft_trigger_offset), not a host-side replica.
 *
 * Coherence: the acq task commits CH1+CH2 atomically under acq_frame_gen
 * (odd = commit in progress), so a copy bracketed by an unchanged EVEN
 * generation is one capture on both channels — which is what makes the
 * CH1-vs-CH2 relative-phase measurement in scripts/exp22_stability.py valid.
 * Touches RAM only (no SPI3), so acq stays live, same as `spi3 read`. */
void cmd_spi3_frame(void)
{
    const volatile uint8_t *c1 = fpga_get_ch1_buf();
    const volatile uint8_t *c2 = fpga_get_ch2_buf();
    if (!c1 || !c2) {
        usb_send_str("FPGA not initialized\r\n");
        return;
    }

    uint8_t *s1 = shell_bus_scratch;
    uint8_t *s2 = shell_bus_scratch + FPGA_ADC_BUF_SIZE;
    uint32_t g0 = 0, g1 = 1;
    for (uint8_t tries = 0; tries < 8; tries++) {
        g0 = fpga_acq_frame_generation();
        if (g0 & 1u) {                      /* mid-commit: wait it out */
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        for (uint32_t i = 0; i < FPGA_ADC_BUF_SIZE; i++) s1[i] = c1[i];
        for (uint32_t i = 0; i < FPGA_ADC_BUF_SIZE; i++) s2[i] = c2[i];
        g1 = fpga_acq_frame_generation();
        if (g1 == g0) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    const scope_state_t *ss = scope_state_get();
    uint8_t src_ch = (ss && ss->trigger.source == TRIG_SRC_CH2) ? 2u : 1u;
    uint16_t off = scope_ui_soft_trigger_offset((src_ch == 2u) ? s2 : s1);

    /* L: columns per sample of the window (fast-code zoom, parity C5); the
     * window shows record positions off + x / L, x = 0..300. R: record
     * samples per column x 1000 (1000 / L). L=0: a held record re-sampled at
     * R about column tx (zoom while paused, C14); off is then the
     * one-sample-per-column window the pivot sample (off + tx) comes from. */
    usb_debug_printf("FRAME gen=%lu coherent=%u src=CH%u off=%u soft=%u tx=%d anchor=%u ord=%u L=%u R=%lu exact=%u\r\n",
                     (unsigned long)g1,
                     (unsigned)(g1 == g0 && !(g0 & 1u)),
                     (unsigned)src_ch, (unsigned)off,
                     (unsigned)(ss ? (ss->soft_trigger ? 1 : 0) : 0),
                     (int)scope_ui_trig_x_actual(), (unsigned)scope_ui_trig_anchor(),
                     (unsigned)fpga_acq_record_time_ordered(),
                     (unsigned)scope_ui_view_factor(),
                     (unsigned long)scope_ui_view_ratio(),
                     (unsigned)scope_ui_trig_anchor_exact());
    spi3_frame_dump("CH1", s1);
    spi3_frame_dump("CH2", s2);
}

/* pinlog [start]: bench tool. Once a second (usb_debug_pinlog_tick, from the
 * 1 s timer) store every GPIO port's input word and the last battery ADC
 * result, so a pin that follows "USB attached" can be found: start it,
 * unplug the cable for ~10 s, plug it back, then `pinlog` prints the rows. */
#define PINLOG_N 90
static struct { uint16_t t; uint16_t idt[5]; uint16_t adc; } s_pinlog[PINLOG_N];
static volatile uint8_t s_pinlog_n;
static volatile uint8_t s_pinlog_on;
static uint16_t s_pinlog_t;

/* Rows are stored only on a change: a quiet pin flips (the LCD bus on D/E,
 * PA6, PB4-PB6, PB8 (PWM, backlight) and PC0 strobe are masked out) or the battery ADC moves by 12
 * counts (~20 mV). So the log can stay armed for hours. */
void benchshell_pinlog_tick(void)
{
    if (!s_pinlog_on) return;
    s_pinlog_t++;
    if (s_pinlog_n >= PINLOG_N) return;
    const uint16_t a = (uint16_t)GPIOA->idt, b = (uint16_t)GPIOB->idt, c = (uint16_t)GPIOC->idt;
    const uint16_t adc = (uint16_t)(ADC1->odt & 0xFFFu);
    const uint8_t i = s_pinlog_n;
    if (i > 0) {
        const int d = (int)adc - (int)s_pinlog[i - 1].adc;
        if (((a ^ s_pinlog[i - 1].idt[0]) & (uint16_t)~(1u << 6)) == 0 &&
            ((b ^ s_pinlog[i - 1].idt[1]) & (uint16_t)~0x0170u) == 0 &&
            ((c ^ s_pinlog[i - 1].idt[2]) & (uint16_t)~(1u << 0)) == 0 &&
            d > -12 && d < 12)
            return;
    }
    s_pinlog[i].t = s_pinlog_t;
    s_pinlog[i].idt[0] = a; s_pinlog[i].idt[1] = b; s_pinlog[i].idt[2] = c;
    s_pinlog[i].idt[3] = (uint16_t)GPIOD->idt;
    s_pinlog[i].idt[4] = (uint16_t)GPIOE->idt;
    s_pinlog[i].adc    = adc;
    s_pinlog_n = (uint8_t)(i + 1u);
}

void cmd_pinlog(const char *args)
{
    while (*args == ' ') args++;
    if (strncmp(args, "start", 5) == 0) {
        s_pinlog_on = 0; s_pinlog_n = 0; s_pinlog_t = 0; s_pinlog_on = 1;
        usb_debug_printf("pinlog: armed, %d change rows\r\n", PINLOG_N);
        return;
    }
    const uint8_t n = s_pinlog_n;
    usb_debug_printf("pinlog: %u rows at %u s (s: A B C D E adc)\r\n", (unsigned)n, (unsigned)s_pinlog_t);
    for (uint8_t i = 0; i < n; i++)
        usb_debug_printf("%u: %04X %04X %04X %04X %04X %u\r\n", (unsigned)s_pinlog[i].t,
                         s_pinlog[i].idt[0], s_pinlog[i].idt[1], s_pinlog[i].idt[2],
                         s_pinlog[i].idt[3], s_pinlog[i].idt[4], (unsigned)s_pinlog[i].adc);
}

/* lcd [selftest]: panel health (util/lcd_health.h). selftest sends the
 * ST7789 a software reset (0x01), the state seen once on unit #2, so the
 * display task's check has to find it and recover within ~2 s. */
void cmd_lcd(const char *args)
{
    while (*args == ' ') args++;
    if (strncmp(args, "selftest", 8) == 0) {
        taskENTER_CRITICAL();
        lcd_write_cmd(0x01);
        taskEXIT_CRITICAL();
        usb_debug_printf("lcd: software reset sent; the health check should repaint within ~2 s\r\n");
        return;
    }
    taskENTER_CRITICAL();
    const uint8_t pm = lcd_read_power_mode();
    taskEXIT_CRITICAL();
    const lcd_health_t h = g_lcd_health;
    usb_debug_printf("lcd: power mode now 0x%02X (%s) | checks %lu bad %lu re-inits %lu",
                     pm, lcd_health_pm_ok(pm) ? "ok" : "NOT ok",
                     (unsigned long)h.checks, (unsigned long)h.bad_reads, (unsigned long)h.events);
    if (h.events)
        usb_debug_printf(" | last at %lu ms, pm 0x%02X, mode %u, %lu ms after key 0x%X, buzzer %u, backlight %u",
                         (unsigned long)h.last_event_ms, h.event_pm, h.event_mode,
                         (unsigned long)h.event_since_key_ms, (unsigned)h.event_key,
                         (unsigned)h.event_buzzer, (unsigned)h.event_backlight);
    usb_debug_printf("\r\n");
}

/* `lcd readback [x y w h] [w565]` — an EXPERIMENT, in no automatic path.
 * Reads the window back from the panel's frame memory (lcd_readback_row:
 * RAMRD, 18-bit packed by default, `w565` for one word per pixel), CRC32s
 * it as RGB565 (little-endian, row by row) next to the same window of the
 * screenshot shadow expanded through its palette as the screenshot does,
 * and counts the pixels that differ. Nothing is stored. To decide whether
 * the 38,400 B shadow could go and screenshots read the glass instead.
 *
 * Exclusion: the read runs on the display task, the only task that touches
 * the LCD, between two of its frames (display_call_sync, as a screenshot is
 * taken); this handler only waits and prints. Each row is still read with
 * the scheduler suspended (one row, ~0.3-0.5 ms), and the display task checks
 * in with the health monitor every 32 rows. Interrupts stay on. */
typedef struct {
    uint32_t crc_g, crc_s, diff, n;
    uint16_t g[4], s[4], raw[4];
    const uint8_t  *bits;
    const uint16_t *pal;
    uint16_t page;
    uint16_t x, y, w, h;        /* the window */
    uint8_t  fmt;
    uint32_t ms;                /* how long the read took */
} lcd_rb_ctx_t;

static void lcd_rb_px(uint16_t x, uint16_t y, uint16_t g, uint16_t raw, void *vc)
{
    lcd_rb_ctx_t *c = (lcd_rb_ctx_t *)vc;
    uint8_t idx = 0;
    if (y >= c->page && y < (uint32_t)c->page + LCD_SHADOW_HEIGHT) {
        const uint8_t byte = c->bits[(uint32_t)(y - c->page) * LCD_SHADOW_STRIDE + (x >> 1)];
        idx = (x & 1u) ? (byte & 0x0Fu) : (byte >> 4);
    }
    const uint16_t s = c->pal[idx & 0x0Fu];
    const uint8_t gb[2] = { (uint8_t)g, (uint8_t)(g >> 8) }, sb[2] = { (uint8_t)s, (uint8_t)(s >> 8) };
    c->crc_g = crc32_update(c->crc_g, gb, 2);
    c->crc_s = crc32_update(c->crc_s, sb, 2);
    if (g != s) c->diff++;
    if (c->n < 4u) { c->g[c->n] = g; c->s[c->n] = s; c->raw[c->n] = raw; }
    c->n++;
}

/* The read itself: display task (display_call_sync). */
static void lcd_rb_run(void *vc)
{
    lcd_rb_ctx_t *c = (lcd_rb_ctx_t *)vc;
    const TickType_t t0 = xTaskGetTickCount();
    for (uint32_t r = 0; r < c->h; r++) {
        vTaskSuspendAll();
        lcd_readback_row(c->x, (uint16_t)(c->y + r), c->w, c->fmt, lcd_rb_px, c);
        (void)xTaskResumeAll();
        if ((r & 31u) == 31u) health_checkin_self();
    }
    c->ms = (uint32_t)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS);
}

void cmd_lcd_readback(const char *args)
{
    static const char usage[] = "usage: lcd readback [x y w h] [w565]\r\n";
    uint32_t v[4] = { 0, 0, LCD_WIDTH, LCD_HEIGHT };
    uint8_t fmt = LCD_RB_PACKED18;
    char buf[48], *save = NULL, *tok;
    unsigned k = 0;
    if (strlen(args) >= sizeof buf) { usb_send_str(usage); return; }
    strcpy(buf, args);
    for (tok = strtok_r(buf, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
        if (strcmp(tok, "w565") == 0) { fmt = LCD_RB_WORD565; continue; }
        if (k >= 4u || parse_int(tok, &v[k]) != 0) { usb_send_str(usage); return; }
        k++;
    }
    if ((k != 0u && k != 4u) || v[2] == 0u || v[3] == 0u || v[0] + v[2] > LCD_WIDTH ||
        v[1] + v[3] > LCD_HEIGHT) {
        usb_send_str(usage);
        return;
    }
    lcd_rb_ctx_t c;
    memset(&c, 0, sizeof c);
    c.bits = lcd_shadow_bits();
    c.pal = lcd_shadow_palette_get();
    c.page = lcd_shadow_page_y();
    c.x = (uint16_t)v[0]; c.y = (uint16_t)v[1]; c.w = (uint16_t)v[2]; c.h = (uint16_t)v[3];
    c.fmt = fmt;

    if (!display_call_sync(lcd_rb_run, &c, 500u)) {
        usb_send_str("lcd readback: the display task did not take the read in 500 ms; nothing read\r\n");
        return;
    }
    const uint32_t ms = c.ms;

    usb_debug_printf("lcd readback x=%lu y=%lu w=%lu h=%lu fmt=%s: %lu px in %lu ms\r\n",
                     (unsigned long)v[0], (unsigned long)v[1], (unsigned long)v[2], (unsigned long)v[3],
                     fmt == LCD_RB_WORD565 ? "w565" : "packed18", (unsigned long)c.n, (unsigned long)ms);
    usb_debug_printf("  glass  crc32 %08lX\r\n  shadow crc32 %08lX  %s, %lu px differ\r\n",
                     (unsigned long)c.crc_g, (unsigned long)c.crc_s,
                     c.crc_g == c.crc_s ? "MATCH" : "differ", (unsigned long)c.diff);
    usb_send_str("  first px (glass rgb565 / raw word / shadow rgb565):");
    for (unsigned i = 0; i < 4u && i < c.n; i++)
        usb_debug_printf(" %04X/%04X/%04X", (unsigned)c.g[i], (unsigned)c.raw[i], (unsigned)c.s[i]);
    usb_send_str("\r\n");
}

/* Inline SPI3 exchange using raw registers (usb_debug.c can't see static spi3_xfer) */
static uint8_t spi3_raw_xfer(uint8_t tx)
{
    volatile uint32_t *sts = (volatile uint32_t *)0x40003C08;  /* SPI3_STS */
    volatile uint32_t *dt  = (volatile uint32_t *)0x40003C0C;  /* SPI3_DT */
    uint32_t timeout;

    timeout = 100000;
    while (!(*sts & 0x02) && --timeout);  /* Wait TXE */
    if (timeout == 0) return 0xEE;  /* Distinguish timeout from FPGA 0xFF */
    *dt = tx;

    timeout = 100000;
    while (!(*sts & 0x01) && --timeout);  /* Wait RXNE */
    if (timeout == 0) return 0xEE;
    return (uint8_t)*dt;
}

/* spi3 xfer <hex...> — send an arbitrary MOSI byte sequence over SPI3 with
 * software CS and dump the MISO bytes clocked back. This is the generic
 * primitive needed to replay ripcord's literal command frames — e.g.
 * "spi3 xfer 04" to test whether a command-FIRST byte (which our acquisition
 * path never sends) wakes the FPGA, or "spi3 xfer 05" for the ID query.
 * CS (PB6) is asserted LOW only after every byte parses, and is ALWAYS
 * deasserted on exit so the FPGA slave is never left gated off. */
#define SPI3_XFER_MAX 64
void cmd_spi3_xfer(const char *args)
{
    uint8_t tx[SPI3_XFER_MAX];
    uint8_t rx[SPI3_XFER_MAX];
    char buf[200];
    char *saveptr = NULL;
    char *tok;
    uint32_t n = 0;

    if (strlen(args) >= sizeof(buf)) { usb_send_str("ERR: line too long\r\n"); return; }
    strcpy(buf, args);

    for (tok = strtok_r(buf, " \t", &saveptr); tok; tok = strtok_r(NULL, " \t", &saveptr)) {
        char *end;
        unsigned long v = strtoul(tok, &end, 16);   /* bare hex; "0x" optional */
        if (n >= SPI3_XFER_MAX) { usb_debug_printf("ERR: max %d bytes\r\n", SPI3_XFER_MAX); return; }
        if (*end != '\0' || v > 0xFF) { usb_debug_printf("ERR: bad hex byte '%s'\r\n", tok); return; }
        tx[n++] = (uint8_t)v;
    }
    if (n == 0) { usb_send_str("Usage: spi3 xfer <b0> <b1> ...\r\n"); return; }

    uint32_t pc0_before = (GPIOC->idt & 1) ? 1 : 0;
    GPIOB->clr = (1 << 6);          /* CS assert (LOW) */
    for (uint32_t i = 0; i < n; i++)
        rx[i] = spi3_raw_xfer(tx[i]);
    GPIOB->scr = (1 << 6);          /* CS deassert (HIGH) — always */
    uint32_t pc0_after = (GPIOC->idt & 1) ? 1 : 0;

    usb_send_str("MOSI:");
    for (uint32_t i = 0; i < n; i++) usb_debug_printf(" %02X", tx[i]);
    usb_send_str("\r\nMISO:");
    uint32_t nonff = 0;
    uint32_t timeouts = 0;
    for (uint32_t i = 0; i < n; i++) {
        usb_debug_printf(" %02X", rx[i]);
        if (rx[i] != 0xFF) nonff++;
        /* spi3_raw_xfer() returns 0xEE on a TXE/RXNE timeout. It is NOT a
         * reply, and it was being counted as one: on a hung bus this printed
         * "non-FF: 1/1", i.e. the exact positive signal this command exists to
         * detect. Every other counter in this file already excludes 0xEE. The
         * nonff figure is left alone (bench notes quote it); the timeouts are
         * reported alongside it so the number can be read honestly. */
        if (rx[i] == 0xEE) timeouts++;
    }
    usb_debug_printf("\r\nnon-FF: %lu/%lu  timeouts(0xEE): %lu  PC0 %lu->%lu\r\n",
                     (unsigned long)nonff, (unsigned long)n,
                     (unsigned long)timeouts, pc0_before, pc0_after);
    if (timeouts)
        usb_send_str("WARN: 0xEE is the SPI3 timeout sentinel, not FPGA data — "
                     "treat the non-FF count as unproven\r\n");
}

/* spi3 acqread — read one acquisition frame per channel using the REAL
 * stock protocol decoded from the issue-#18 capture: per-channel 1026-byte
 * reads, opcode 0x04 (CH1) / 0x05 (CH2), in a single CS-LOW window each.
 * MISO layout per frame: [resp0][resp1][resp2] then ~1023 unsigned samples.
 * Reports PC0 before/after, the 3 status bytes, the first 16 samples, and
 * min/max/mean of the sample region — enough to tell a real waveform from a
 * flat line (feed the siggen into CH1 for a known signal). Read-only at the
 * FPGA, but it DRIVES THE SHARED SPI3 BUS — the old claim here that it "does
 * not touch the acquisition task" was false (audit 2026-08-20, P0.1): the
 * acq task preempts this task mid-CS-window unless parked. All callers now
 * run under the dispatch-level spi3_shell_claim(). */
static void cmd_spi3_acqread_one(uint8_t opcode)
{
    uint8_t first16[16];
    uint16_t smin = 255, smax = 0;
    uint32_t ssum = 0, scount = 0;

    uint32_t pc0_before = (GPIOC->idt & 1) ? 1 : 0;
    GPIOB->clr = (1 << 6);                 /* CS assert (LOW) */
    uint8_t r0 = spi3_raw_xfer(opcode);    /* MISO during opcode */
    uint8_t r1 = spi3_raw_xfer(0xFF);
    uint8_t r2 = spi3_raw_xfer(0xFF);
    for (uint32_t i = 0; i < 1023; i++) {  /* 3 status + 1023 = 1026-byte frame */
        uint8_t s = spi3_raw_xfer(0xFF);
        if (i < 16) first16[i] = s;
        if (s < smin) smin = s;
        if (s > smax) smax = s;
        ssum += s;
        scount++;
    }
    GPIOB->scr = (1 << 6);                 /* CS deassert (HIGH) */
    uint32_t pc0_after = (GPIOC->idt & 1) ? 1 : 0;

    usb_debug_printf("CH (0x%02X): status %02X %02X %02X  PC0 %lu->%lu\r\n",
                     opcode, r0, r1, r2, pc0_before, pc0_after);
    usb_send_str("  first16:");
    for (int i = 0; i < 16; i++) usb_debug_printf(" %02X", first16[i]);
    usb_debug_printf("\r\n  samples: min=%u max=%u mean=%lu span=%u\r\n",
                     smin, smax, scount ? ssum / scount : 0,
                     (unsigned)(smax - smin));
}

void cmd_spi3_acqread(void)
{
    usb_send_str("=== acqread (real 0x04/0x05 protocol) ===\r\n");
    cmd_spi3_acqread_one(0x04);
    cmd_spi3_acqread_one(0x05);
    usb_send_str("(span>0 = live signal; span=0 = flat. Feed siggen->CH1 to verify.)\r\n");
}

/* ── Step 0b (bench plan 2026-08-14): the BSRAM_1/2 read-opcode hunt ──────
 *
 * The netlist work (gw1n2-apicula progress log M9–M11) identified BSRAM_1/2
 * as a separately-clocked, accumulate-in-place buffer pair — the shape of a
 * decimated/roll buffer, i.e. plausibly the slow timebase — and showed all
 * four BSRAMs feed the SAME readout mux (→ SPI SO + DRDY). So the MCU can
 * read it, under some opcode other than 0x04/0x05. These commands sweep the
 * opcode space for it on the live coldtrace rig.
 *
 * Framing is byte-identical to the proven 0x04/0x05 path: ONE CS-LOW window,
 * opcode + 2 filler bytes + payload. The continuous acquisition task is
 * parked first via fpga_acq_pause() — a shell CS assert interleaved with its
 * frames is the same desync class as the 30 ms cadence finding (fpga.c),
 * which needed a true FPGA power cycle to clear.
 *
 * Known opcode map on the CONFIGURED user design (not the Gowin config
 * port, which is closed — Exp L): 0x01/0x02/0x06/0x07/0x08 = arm-sequence
 * WRITES (opsweep skips them: the 0xFF payload filler would smash live
 * registers — 0x01 is the run register); 0x03 = status read; 0x04/0x05 =
 * channel reads (kept in the sweep as positive controls). */
#define SPI3_OPREAD_MAX_LEN 4096u

static bool spi3_opread_window(uint8_t opcode, uint32_t len, bool dump)
{
    uint8_t first16[16];
    uint8_t r0, r1, r2;
    uint16_t smin = 255, smax = 0;
    uint32_t ssum = 0, nonff = 0;

    GPIOB->clr = (1 << 6);                 /* CS assert (LOW) */
    r0 = spi3_raw_xfer(opcode);
    r1 = spi3_raw_xfer(0xFF);
    r2 = spi3_raw_xfer(0xFF);
    for (uint32_t i = 0; i < len; i++) {
        uint8_t s = spi3_raw_xfer(0xFF);
        if (i < 16) first16[i] = s;
        if (s < smin) smin = s;
        if (s > smax) smax = s;
        if (s != 0xFF) nonff++;
        ssum += s;
        if (dump) {
            if (i % 16 == 0) usb_debug_printf("%04lX:", (unsigned long)i);
            usb_debug_printf(" %02X", s);
            if (i % 16 == 15 || i == len - 1) usb_send_str("\r\n");
        }
    }
    GPIOB->scr = (1 << 6);                 /* CS deassert (HIGH) */

    usb_debug_printf("op %02X: s=%02X %02X %02X nff=%lu/%lu "
                     "min=%u max=%u mean=%lu span=%u first16:",
                     opcode, r0, r1, r2,
                     (unsigned long)nonff, (unsigned long)len,
                     smin, smax, (unsigned long)(len ? ssum / len : 0),
                     (unsigned)(smax - smin));
    for (int i = 0; i < 16 && (uint32_t)i < len; i++)
        usb_debug_printf(" %02X", first16[i]);
    usb_send_str("\r\n");
    return smax > smin;                    /* payload varies */
}

/* Full proven-shape 0x04 read; returns the sample span. Used as the wedge
 * canary between sweep steps. Full 1026-byte window on purpose — do NOT
 * shorten it (frame shape is part of the protocol; see the 0x05 hazard note
 * on fpga_warmtest_read_channel). Consumes one capture frame. */
static unsigned spi3_canary_span(void)
{
    uint8_t smin = 255, smax = 0;
    GPIOB->clr = (1 << 6);
    (void)spi3_raw_xfer(0x04);
    (void)spi3_raw_xfer(0xFF);
    (void)spi3_raw_xfer(0xFF);
    for (uint32_t i = 0; i < 1023; i++) {
        uint8_t s = spi3_raw_xfer(0xFF);
        if (s < smin) smin = s;
        if (s > smax) smax = s;
    }
    GPIOB->scr = (1 << 6);
    return (unsigned)(smax - smin);
}

/* spi3 opread <op-hex> [len [dump]] — one read window of `len` payload bytes
 * (default 1026, matching the channel-read shape; max 4096) under an
 * arbitrary opcode, with stats and optional full hex dump. */
void cmd_spi3_opread(const char *args)
{
    char buf[64];
    char *saveptr = NULL;
    char *tok, *end;
    uint32_t len = 1026;
    bool dump = false;
    unsigned long op;

    if (strlen(args) >= sizeof(buf)) { usb_send_str("ERR: line too long\r\n"); return; }
    strcpy(buf, args);

    tok = strtok_r(buf, " \t", &saveptr);
    if (!tok) { usb_send_str("Usage: spi3 opread <op-hex> [len [dump]]\r\n"); return; }
    op = strtoul(tok, &end, 16);
    if (*end != '\0' || op > 0xFF) { usb_send_str("ERR: bad opcode\r\n"); return; }

    tok = strtok_r(NULL, " \t", &saveptr);
    if (tok) {
        len = strtoul(tok, &end, 0);
        if (*end != '\0' || len == 0 || len > SPI3_OPREAD_MAX_LEN) {
            usb_debug_printf("ERR: len 1..%u\r\n", SPI3_OPREAD_MAX_LEN);
            return;
        }
        tok = strtok_r(NULL, " \t", &saveptr);
        /* An unrecognised third token used to be swallowed: the window ran
         * with dump=false and the operator read "stats only" as an empty
         * dump rather than a rejected argument. */
        if (tok) {
            if (strcmp(tok, "dump") != 0) {
                usb_debug_printf("ERR: expected 'dump', got '%s'\r\n", tok);
                return;
            }
            dump = true;
            if (strtok_r(NULL, " \t", &saveptr) != NULL) {
                usb_send_str("ERR: too many arguments. Usage: spi3 opread <op> [len [dump]]\r\n");
                return;
            }
        }
    }

    if (!fpga_acq_pause()) {
        usb_send_str("ERR: acq task did not park — bus not safe, aborting\r\n");
        return;
    }
    spi3_opread_window((uint8_t)op, len, dump);
    fpga_acq_resume();
}

/* spi3 opsweep [start end [len]] — sweep opcodes (hex bounds, default
 * 00..3F, known writes skipped), one window each, 0x04 canary between
 * steps. Run with the ESP32 feeding a LIVE slow signal into CH1: the
 * canary then has span>0, so a wedged readout mux is detectable and the
 * sweep aborts naming the offending opcode. A hit = an unknown opcode
 * whose payload varies / is non-FF — re-read it twice with different
 * siggen settings (spi3 opread <op> 2048 dump) to confirm it tracks. */
void cmd_spi3_opsweep(const char *args)
{
    char buf[64];
    char *saveptr = NULL;
    char *tok, *end;
    uint32_t start = 0x00, stop = 0x3F, len = 2048;  /* 1024 words: BSRAM_1/2
                                                        is a word-wide pair */
    static const uint8_t skip[] = { 0x01, 0x02, 0x06, 0x07, 0x08 };
    unsigned flat_streak = 0;

    if (strlen(args) >= sizeof(buf)) { usb_send_str("ERR: line too long\r\n"); return; }
    strcpy(buf, args);

    tok = strtok_r(buf, " \t", &saveptr);
    if (tok) {
        start = strtoul(tok, &end, 16);
        if (*end != '\0' || start > 0xFF) { usb_send_str("ERR: bad start\r\n"); return; }
        tok = strtok_r(NULL, " \t", &saveptr);
        if (!tok) { usb_send_str("Usage: spi3 opsweep [start end [len]]\r\n"); return; }
        stop = strtoul(tok, &end, 16);
        if (*end != '\0' || stop > 0xFF || stop < start) { usb_send_str("ERR: bad end\r\n"); return; }
        tok = strtok_r(NULL, " \t", &saveptr);
        if (tok) {
            len = strtoul(tok, &end, 0);
            if (*end != '\0' || len == 0 || len > SPI3_OPREAD_MAX_LEN) {
                usb_debug_printf("ERR: len 1..%u\r\n", SPI3_OPREAD_MAX_LEN);
                return;
            }
        }
    }

    if (!fpga_acq_pause()) {
        usb_send_str("ERR: acq task did not park — bus not safe, aborting\r\n");
        return;
    }

    unsigned base_span = spi3_canary_span();
    usb_debug_printf("=== opsweep %02lX..%02lX len=%lu  baseline 0x04 span=%u ===\r\n",
                     (unsigned long)start, (unsigned long)stop,
                     (unsigned long)len, base_span);
    if (base_span == 0)
        usb_send_str("WARN: CH1 is flat — feed the siggen into CH1 first, or the\r\n"
                     "      canary cannot tell a wedged mux from no signal.\r\n");

    for (uint32_t op = start; op <= stop; op++) {
        bool skipped = false;
        for (unsigned i = 0; i < sizeof(skip); i++)
            if (op == skip[i]) { skipped = true; break; }
        if (skipped) {
            usb_debug_printf("op %02lX: SKIP (known write reg)\r\n", (unsigned long)op);
            continue;
        }

        spi3_opread_window((uint8_t)op, len, false);
        vTaskDelay(pdMS_TO_TICKS(150));    /* cadence floor, see fpga.c */
        unsigned cspan = spi3_canary_span();
        usb_debug_printf("      canary span=%u\r\n", cspan);
        if (base_span > 0) {
            flat_streak = (cspan == 0) ? flat_streak + 1 : 0;
            if (flat_streak >= 2) {
                usb_debug_printf("ABORT: canary flat twice — engine likely wedged "
                                 "by op %02lX or %02lX. FPGA power cycle to recover\r\n"
                                 "(POWER->Goodbye->UNPLUG USB->replug).\r\n",
                                 (unsigned long)op, (unsigned long)(op - 1));
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    usb_send_str("=== opsweep done ===\r\n");
    fpga_acq_resume();
}

/* spi3 gowin — read the Gowin SSPI ID / USERCODE / STATUS registers using
 * rosenrot00's PROVEN-WORKING 2C23T framing and decode the status bits.
 *
 * Framing (per fpga_hw4_read_register32 in the 2C23T loader): one flush byte
 * clocked with CS HIGH, then CS LOW, then opcode + 3 zero bytes, then 4 read
 * bytes, then CS HIGH. The opcode sits in the MSB of a 32-bit "address word"
 * (0x11<<24 etc), exactly as their read_register32(0x11000000) does.
 *
 * Why this matters: our `spi3 xfer 41 ...` returned 0x00000000 on a *running*
 * NV-booted FPGA. A configured GW1N must report DONE_FINAL / GOWIN_VLD / READY
 * in its status register — zero means either our READ FRAMING is wrong (this
 * command tests that by also reading IDCODE, which must equal 0x0120681B for
 * the GW1N-2) or the part is genuinely unconfigured. Either answer unblocks us.
 *
 * Gowin status-register bit names from openFPGALoader src/gowin.cpp. */
static uint32_t spi3_gowin_read_reg(uint8_t opcode)
{
    /* flush byte with CS HIGH (CS is idle-high here) */
    (void)spi3_raw_xfer(0x00);
    GPIOB->clr = (1 << 6);                 /* CS assert (PB6 LOW) */
    (void)spi3_raw_xfer(opcode);           /* opcode in MSB position */
    (void)spi3_raw_xfer(0x00);
    (void)spi3_raw_xfer(0x00);
    (void)spi3_raw_xfer(0x00);
    uint8_t b0 = spi3_raw_xfer(0x00);
    uint8_t b1 = spi3_raw_xfer(0x00);
    uint8_t b2 = spi3_raw_xfer(0x00);
    uint8_t b3 = spi3_raw_xfer(0x00);
    GPIOB->scr = (1 << 6);                 /* CS deassert (PB6 HIGH) */
    return ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) |
           ((uint32_t)b2 << 8) | (uint32_t)b3;
}

/* Set SPI3 baud divider on the fly (CTRL1 bits[5:3]); needs SPE off/on.
 * br: 0=/2(60MHz) .. 7=/256(~470kHz). Returns the previous CTRL1. */
static uint32_t spi3_set_baud(uint32_t br)
{
    volatile uint32_t *ctrl1 = (volatile uint32_t *)0x40003C00;
    uint32_t prev = *ctrl1;
    *ctrl1 &= ~(1u << 6);                                   /* SPE = 0 */
    *ctrl1 = (*ctrl1 & ~(7u << 3)) | ((br & 7u) << 3);      /* set MDIV */
    *ctrl1 |= (1u << 6);                                    /* SPE = 1 */
    return prev;
}

void cmd_spi3_gowin(void)
{
    static const struct { uint32_t mask; const char *name; } STATUS_BITS[] = {
        { 1u << 0,  "CRC Error" },
        { 1u << 1,  "Bad Command" },
        { 1u << 2,  "ID Verify Failed" },
        { 1u << 3,  "Timeout" },
        { 1u << 5,  "Memory Erase" },
        { 1u << 6,  "Preamble" },
        { 1u << 7,  "System Edit Mode" },
        { 1u << 8,  "Program spiFlash directly" },
        { 1u << 10, "Non-JTAG config active" },
        { 1u << 11, "Bypass" },
        { 1u << 12, "Gowin VLD" },
        { 1u << 13, "Done Final" },
        { 1u << 14, "Security Final" },
        { 1u << 15, "Ready" },
        { 1u << 16, "POR" },
        { 1u << 17, "FLASH lock" },
    };

    usb_send_str("=== Gowin SSPI registers (rosenrot00 framing) ===\r\n");

    /* Read at two clock rates: /2 (60MHz, our normal) and /256 (~470kHz,
     * closer to rosenrot00's bit-bang). If the slow read returns the real
     * IDCODE but the fast one doesn't, the SSPI read path is clock-limited. */
    static const struct { uint32_t br; const char *label; } SPEEDS[] = {
        { 0u, "/2  (60MHz)" },
        { 7u, "/256(~470kHz)" },
    };

    uint32_t saved = 0;
    for (unsigned s = 0; s < sizeof(SPEEDS) / sizeof(SPEEDS[0]); s++) {
        if (s == 0) saved = spi3_set_baud(SPEEDS[s].br);
        else        (void)spi3_set_baud(SPEEDS[s].br);

        uint32_t id  = spi3_gowin_read_reg(0x11);   /* READ_IDCODE   */
        uint32_t usr = spi3_gowin_read_reg(0x13);   /* READ_USERCODE */
        uint32_t st  = spi3_gowin_read_reg(0x41);   /* READ_STATUS   */

        /* The IDCODE read in THIS window, at THIS clock, is the anchor: it is
         * the only value here with an independently known correct answer
         * (0x0120681B, from the Gowin .fs preamble at file offset 0x4AD19).
         * If it does not match, the read path is not trustworthy and neither
         * is the STATUS taken through it — so the decode is withheld rather
         * than printed as a list of named bits.
         *
         * This is not hypothetical. The /2 pass below is KNOWN BAD (fpga.c
         * ~1564: at /2 the MISO bit is latched one edge early). Six weeks of
         * this project ran on "0x8001C810 = READY POR", which was 0x00039020
         * shifted by one bit and set bits 4/11/31 that are not defined Gowin
         * bits at all. The old code here printed that decode with no caveat. */
        int anchored = (id == 0x0120681BUL);

        usb_debug_printf("\r\n[clk %s]%s\r\n", SPEEDS[s].label,
                         SPEEDS[s].br == 0
                             ? "  <-- KNOWN-INVALID READ CLOCK (fpga.c:1564);"
                               " kept only as the negative control"
                             : "");
        usb_debug_printf("IDCODE  (0x11): 0x%08lX  %s\r\n", id,
                         anchored ? "== GW1N-2 OK (read path ANCHORED)"
                                  : "!= 0x0120681B -> READ PATH NOT ANCHORED");
        usb_debug_printf("USERCODE(0x13): 0x%08lX%s\r\n", usr,
                         anchored ? "" : "  (unanchored — do not quote)");
        usb_debug_printf("STATUS  (0x41): 0x%08lX%s\r\n", st,
                         anchored ? "" : "  (unanchored — do not quote)");
        if (!anchored) {
            usb_send_str("  decode: WITHHELD — the anchor failed, so this word is not\r\n"
                         "          known to be a register value. A stable wrong number\r\n"
                         "          is indistinguishable from a right one.\r\n");
        } else {
            usb_send_str("  decode:");
            int any = 0;
            for (unsigned i = 0; i < sizeof(STATUS_BITS) / sizeof(STATUS_BITS[0]); i++) {
                if (st & STATUS_BITS[i].mask) {
                    usb_debug_printf(" [%s]", STATUS_BITS[i].name);
                    any = 1;
                }
            }
            usb_send_str(any ? "\r\n" : " (no bits set)\r\n");
        }
    }

    /* Restore the original CTRL1 (baud + SPE) for the rest of the system. */
    {
        volatile uint32_t *ctrl1 = (volatile uint32_t *)0x40003C00;
        *ctrl1 &= ~(1u << 6);
        *ctrl1 = saved;
    }

    usb_send_str("\r\nHow to read this (Exps J and L, 2026-07-28):\r\n"
                 "  IDCODE 0x0120681B at /256 = read path is valid; STATUS can be trusted.\r\n"
                 "  IDCODE silent/zero at /256 = the SSPI config port has CLOSED. That is\r\n"
                 "    what a SUCCESSFULLY CONFIGURED part looks like (the pins belong to the\r\n"
                 "    user design from then on) -- but a dead bus looks identical, so\r\n"
                 "    corroborate with DONE_FINAL from the config path and a live trace.\r\n"
                 "  The /2 pass is expected to be garbage; it is the negative control.\r\n");
}

/* spi3 edgecap [reps] — EXP-37 LA edge-capture. Emits [reps] identical 0x15
 * CONFIG_ENABLE frames over AF hardware-SPI3 (/256), a 60ms gap, then [reps] over
 * GPIO bit-bang — same pins/rate/frame, drive type the only variable. Isolated
 * frames; does NOT configure. Arm sigrok (D0=SCK/D1=MISO/D2=MOSI/D3=CS) BEFORE
 * running, then diff the two bursts.
 *
 * The rig needs the bit-bang transport, which only exists under FPGA_CONFIG_B.
 * The command stays in the table on every build and says so, rather than
 * disappearing from `help` and leaving the operator guessing. */
void cmd_spi3_edgecap(const char *args)
{
#if !FPGA_CONFIG_B
    (void)args;
    usb_send_str("edgecap: FPGA_CONFIG_B builds only — the GPIO bit-bang half is not"
                 " compiled here. Flash `make guest-bringup-bb`.\r\n");
#else
    uint32_t reps = 16;
    /* parse_int(), not a bare strtoul(): a non-numeric argument used to become
     * 0 and then be clamped back to the default, so the command silently ran
     * something other than what was typed. Numeric out-of-range still clamps,
     * as it always has. */
    if (args && *args && parse_int(args, &reps) != 0) {
        usb_send_str("Usage: spi3 edgecap [reps]\r\n");
        return;
    }
    if (reps == 0 || reps > 255) reps = 16;
    usb_debug_printf("edgecap: %lu AF 0x15 frames (/256), 60ms gap, %lu bit-bang 0x15 frames\r\n",
                     reps, reps);
    usb_send_str("  emitting now — capture window should be armed on D0=SCK D1=MISO D2=MOSI D3=CS\r\n");
    fpga_edgecap_15((uint8_t)reps);
    usb_send_str("edgecap: done. AF burst | gap | GPIO burst are in the capture.\r\n");
#endif
}

/* ─── fpga selftest ────────────────────────────────────────────────────────
 *
 * ONE precondition table, printed before an experiment, so the instrument is
 * verified rather than assumed. Every entry here is something that has, at
 * least once, silently invalidated a measurement in this project:
 *
 *   SPI3 baud     reads at /2 are garbage (fpga.c:1564) — six weeks of this
 *                 project ran on a status word sampled one bit early.
 *   IDCODE anchor the only value on this bus with an independently known
 *                 correct answer (0x0120681B, Gowin .fs preamble at file
 *                 offset 0x4AD19). CLAUDE.md makes it mandatory: a stable
 *                 wrong number is indistinguishable from a right one.
 *   USART2        UEN clear on coldtrace builds, and the dvom_TX drain task
 *                 absent, so "queued" frames never reach the wire.
 *   pin modes     PA6 and PD12 boot as floating INPUTS; `gpio set` on them
 *                 used to report success while driving nothing.
 *
 * STATUS (0x41) is deliberately NOT read here. On a configured part a config-
 * port read desynchronises acquisition (Exp L, 2026-07-28), and this command
 * is meant to be safe to run in the middle of a capture session. IDCODE is
 * the safe anchor; its DISAPPEARANCE is itself the "configured" signature.
 */
void cmd_fpga_selftest(void)
{
    volatile uint32_t *spi3_ctrl1 = (volatile uint32_t *)0x40003C00;
    uint32_t ctrl1, usart_c1;
    uint32_t br, id;
    unsigned inert_pins = 0;
    bool acq_parked;

    usb_send_str("=== fpga selftest — instrument preconditions ===\r\n");

    /* ---- SPI3 --------------------------------------------------------- */
    ctrl1 = *spi3_ctrl1;
    br = (ctrl1 >> 3) & 7u;
    usb_debug_printf("[SPI3 ] CTRL1=%08lX  BR=%lu -> /%lu (%lu kHz)  SPE=%u MSTEN=%u CPOL=%u CPHA=%u\r\n",
                     (unsigned long)ctrl1, (unsigned long)br,
                     (unsigned long)(2u << br),
                     (unsigned long)((system_core_clock / 2u) / (2u << br) / 1000u),
                     (unsigned)((ctrl1 >> 6) & 1u), (unsigned)((ctrl1 >> 2) & 1u),
                     (unsigned)((ctrl1 >> 1) & 1u), (unsigned)(ctrl1 & 1u));
    if (br != 7u)
        usb_send_str("        NOTE: SSPI *config-port* reads are only clean at /256 (BR=7);\r\n"
                     "        at /2 MISO is latched one edge early (fpga.c:1564). The anchor\r\n"
                     "        read below switches to /256 and restores CTRL1 afterwards.\r\n");

    /* ---- IDCODE anchor ------------------------------------------------ */
    acq_parked = fpga_acq_pause();
    if (!acq_parked) {
        usb_send_str("[anchr] SKIPPED — acq task would not park, so a read here would\r\n"
                     "        interleave with its capture frames. Bus state UNVERIFIED.\r\n");
    } else {
        uint32_t saved = spi3_set_baud(7u);       /* /256 — the only valid read clock */
        id = spi3_gowin_read_reg(0x11);           /* READ_IDCODE */
        *spi3_ctrl1 &= ~(1u << 6);                /* SPE off before restoring CTRL1 */
        *spi3_ctrl1 = saved;
        fpga_acq_resume();

        usb_debug_printf("[anchr] IDCODE(0x11) @/256 = 0x%08lX  %s\r\n",
                         (unsigned long)id,
                         id == 0x0120681BUL
                             ? "== GW1N-2, read path ANCHORED (part UNCONFIGURED)"
                             : (id == 0UL
                                    ? "config port CLOSED — expected on a CONFIGURED part (Exp L)"
                                    : "!= 0x0120681B and != 0 -> READ PATH NOT TRUSTWORTHY"));
        if (id != 0x0120681BUL && id != 0UL)
            usb_send_str("        Do not quote any SSPI value taken through this bus until the\r\n"
                         "        anchor reads correctly.\r\n");
    }
    usb_send_str("        (STATUS 0x41 not read — it desyncs a running configured part.)\r\n");

    /* ---- USART2 ------------------------------------------------------- */
    usart_c1 = fpga_usart_ctrl1();
    usb_debug_printf("[USART] CTRL1=%08lX BAUDR=%08lX  UEN=%u TE=%u RE=%u RDBFIEN=%u\r\n",
                     (unsigned long)usart_c1, (unsigned long)fpga_usart_baudr(),
                     (unsigned)((usart_c1 >> 13) & 1u), (unsigned)((usart_c1 >> 3) & 1u),
                     (unsigned)((usart_c1 >> 2) & 1u), (unsigned)((usart_c1 >> 5) & 1u));
    usb_debug_printf("        dvom_TX drain task: %s\r\n",
                     fpga_usart_tx_task_exists()
                         ? "present (queued sends will go out)"
                         : "ABSENT — queued sends would never transmit; the shell now\r\n"
                           "        falls back to the polled path and says so");
    if (((usart_c1 >> 13) & 1u) == 0)
        usb_send_str("        UEN CLEAR: the wire is dark. No USART command can reach the\r\n"
                     "        FPGA, so silence is NOT a measurement. `fpga usart on` first.\r\n");

    /* ---- driven pins -------------------------------------------------- */
    usb_send_str("[pins ] pins this firmware drives (mode nibble / level):\r\n");
    for (unsigned i = 0; i < BENCH_PIN_COUNT; i++) {
        uint8_t nib = gpio_cfg_nibble(BENCH_PINS[i].port, BENCH_PINS[i].pin_no);
        if (!gpio_mode_is_output(nib) || gpio_mode_is_af(nib)) inert_pins++;
        bench_pin_print_row(&BENCH_PINS[i], true);
    }

    usb_debug_printf("\r\nverdict: %u of %u driven pins are NOT plain outputs — `gpio set` on\r\n"
                     "         those is inert. %s\r\n",
                     inert_pins, (unsigned)BENCH_PIN_COUNT,
                     inert_pins ? "Fix with `gpio mode <pin> out` before measuring."
                                : "Frontend posture is drivable.");
    usb_send_str("         Snapshot before an experiment with `bench snapshot`, and put the\r\n"
                 "         posture back with `bench restore` so it cannot leak into the next.\r\n");
}

#if defined(FPGA_ALT_BITSTREAM)
/* ═══════════════════════════════════════════════════════════════════
 * Diagnostic-bitstream debug interface  (guest-debugclk / FPGA_ALT_BITSTREAM)
 * ═══════════════════════════════════════════════════════════════════
 *
 * The debugclk_hw_top image drives SYNTHETIC lanes (CH1 = free-running 8-bit
 * ramp, CH2 = walking one) through the REAL capture/readout path, so the bytes
 * coming back are known a priori. That is what makes the open question
 * answerable: in the 2026-08-15 desk sweep op 0x04 and op 0x05 both returned
 * CH1, and nothing since has separated "the readout mux does not split the
 * channels" from "our framing is wrong". With a known ramp on one lane and a
 * one-hot pattern on the other, the two look nothing alike.
 *
 * Contract for these two commands, from the image's RTL constraints file
 * (fnirsi_2c53t_qn48.cst), not inferred:
 *
 *   PC6  = QN48 pin 30 = IOB7B = dbg_clk. No PLL in this image — the MCU paces
 *          the ENTIRE capture core, ONE SAMPLE PER RISING EDGE. At least 8
 *          clocks must be sent before anything else (an 8-cycle power-on-reset
 *          shifter runs off this clock).
 *   PB11 = QN48 pin 46 = IOR1B = run_enable. capture_enable = run_enable AND
 *          sample_tick; a low->high edge clears the trigger latch and re-arms,
 *          and capture free-runs while it is HIGH. No SPI command is involved.
 *
 * ⚠ Both pins mean something ELSE on a stock-payload build — PC6 is "FPGA SPI
 * enable (must be HIGH)" and PB11 is "FPGA active mode" (which the 2026-08-15
 * desk sweep re-read as a CH2 attenuator relay). So this whole block is
 * compiled ONLY under FPGA_ALT_BITSTREAM: the commands cannot be typed at a
 * stock-payload build, and gating them also keeps every existing image
 * byte-identical, which is what makes the payload swap auditable.
 *
 * Reading the result needs no new code: `spi3 opread 04 1026 dump` prints raw
 * bytes from index 0, so the image's 3-byte "R1V" header is visible directly.
 * Do NOT retarget fpga_warmtest_read_channel at it — that function is
 * stock-framed (2-byte discard, 1024 samples) on purpose, and this image is
 * 3-byte-header/1023-sample. One byte apart, opposite requirements.
 */

/* Busy-wait microseconds off the FREE-RUNNING FreeRTOS SysTick without
 * reprogramming it — fpga.c's systick_delay_us seizes LOAD/VAL/CTRL outright,
 * which is only safe pre-scheduler and would eat the RTOS tick here.
 * Accumulates down-counter deltas so it stays correct across reload wraps.
 * Not jitter-free: the tick ISR or a higher-priority task can stretch a half
 * period, so half_us is a floor, not a guarantee. The FPGA does not care —
 * it counts edges, not time. */
static void dbg_delay_us(uint32_t us)
{
    if (us == 0) return;

    uint32_t load = SysTick->LOAD + 1u;
    if (load <= 1u) {                       /* SysTick not running (pre-scheduler) */
        for (uint32_t i = 0; i < us * 30u; i++) __asm__ volatile("nop");
        return;
    }

    uint64_t target = (uint64_t)us * (system_core_clock / 1000000u);
    uint64_t acc = 0;
    uint32_t prev = SysTick->VAL;
    while (acc < target) {
        uint32_t now = SysTick->VAL;
        acc += (prev >= now) ? (uint32_t)(prev - now)
                             : (uint32_t)(prev + load - now);
        prev = now;
    }
}

#define DBG_CLK_MAX_COUNT    1000000u
#define DBG_CLK_MAX_HALF_US  100000u

/* fpga dbgclk <count> [half_us] — emit `count` RISING edges on PC6, the
 * diagnostic image's sample clock. One rising edge = one sample, so this is
 * the sample-rate knob in its entirety. Default half_us=10 (~50 kHz).
 *
 * Idles HIGH and pulses LOW->HIGH rather than the other way round: that yields
 * exactly `count` rising edges AND leaves PC6 at the level a stock-payload
 * build expects, so the restore below never has to manufacture an extra edge. */
void cmd_fpga_dbgclk(const char *args)
{
    char buf[48];
    char *saveptr = NULL, *tok;
    uint32_t count = 0, half_us = 10;

    if (strlen(args) >= sizeof(buf)) { usb_send_str("ERR: line too long\r\n"); return; }
    strcpy(buf, args);

    tok = strtok_r(buf, " \t", &saveptr);
    if (!tok || parse_int(tok, &count) != 0 || count == 0 || count > DBG_CLK_MAX_COUNT) {
        usb_debug_printf("Usage: fpga dbgclk <count 1..%lu> [half_us 0..%lu]\r\n",
                         (unsigned long)DBG_CLK_MAX_COUNT,
                         (unsigned long)DBG_CLK_MAX_HALF_US);
        return;
    }
    tok = strtok_r(NULL, " \t", &saveptr);
    if (tok && (parse_int(tok, &half_us) != 0 || half_us > DBG_CLK_MAX_HALF_US)) {
        usb_debug_printf("ERR: half_us 0..%lu\r\n", (unsigned long)DBG_CLK_MAX_HALF_US);
        return;
    }

    /* PC6 is the SPI-enable line on a stock payload, and on ANY payload the
     * continuous acq task may be mid-CS-frame. Interleaving is the desync class
     * that needed a true FPGA power cycle to clear (see spi3_opread_window). */
    if (!fpga_acq_pause()) {
        usb_send_str("ERR: acq task did not park — bus not safe, aborting\r\n");
        return;
    }

    /* Save PC6's config nibble and output level so this is non-destructive.
     * PC6 is pin 6 => CRL/cfglr bits 27:24. Read-modify-write THAT NIBBLE ONLY:
     * a past session wrote a whole cfghr word for pins that live in cfglr,
     * floated PC8 (POWER) and PC13, and the device read a phantom POWER press
     * and shut down mid-test. gpio_init() with a single-pin mask does the
     * per-nibble RMW for us on the way in; the restore does it by hand. */
    uint32_t saved_nib = (GPIOC->cfglr >> 24) & 0xFu;
    uint32_t saved_lvl = (GPIOC->odt >> 6) & 1u;

    gpio_init_type gpio_cfg;
    gpio_default_para_init(&gpio_cfg);
    gpio_cfg.gpio_pins            = GPIO_PINS_6;
    gpio_cfg.gpio_mode            = GPIO_MODE_OUTPUT;
    gpio_cfg.gpio_out_type        = GPIO_OUTPUT_PUSH_PULL;
    gpio_cfg.gpio_pull            = GPIO_PULL_NONE;
    gpio_cfg.gpio_drive_strength  = GPIO_DRIVE_STRENGTH_STRONGER;
    GPIOC->scr = (1u << 6);          /* stage HIGH before the driver turns on */
    gpio_init(GPIOC, &gpio_cfg);     /* `gpio set` alone does nothing on a pin
                                      * that is not already an output — that has
                                      * cost this project a session before. */

    for (uint32_t i = 0; i < count; i++) {
        GPIOC->clr = (1u << 6);
        dbg_delay_us(half_us);
        GPIOC->scr = (1u << 6);      /* rising edge = one sample */
        dbg_delay_us(half_us);
    }

    /* Restore level first (still driven, so no float glitch), then the mode. */
    if (saved_lvl) GPIOC->scr = (1u << 6);
    else           GPIOC->clr = (1u << 6);
    GPIOC->cfglr = (GPIOC->cfglr & ~(0xFu << 24)) | (saved_nib << 24);

    fpga_acq_resume();

    usb_debug_printf("dbgclk: %lu rising edges on PC6, half=%luus; "
                     "PC6 restored (cfg nibble %lX, level %lu); PC0=%d PB11=%d\r\n",
                     (unsigned long)count, (unsigned long)half_us,
                     (unsigned long)saved_nib, (unsigned long)saved_lvl,
                     (GPIOC->idt & (1u << 0))  ? 1 : 0,
                     (GPIOB->idt & (1u << 11)) ? 1 : 0);
    if (!saved_lvl)
        usb_send_str("NOTE: PC6 was LOW on entry and was restored LOW. On a stock payload\r\n"
                     "      that is the FPGA-SPI-disabled state.\r\n");
}

/* fpga dbgarm [low_ms] — re-arm the diagnostic image: PB11 (run_enable) LOW
 * for low_ms, then HIGH. The low->high edge clears the trigger latch; capture
 * then free-runs for as long as PB11 stays HIGH (gated by dbg_clk ticks).
 * Default 2 ms; 0 gives a ~100 us pulse.
 *
 * Unlike dbgclk this deliberately does NOT restore PB11's previous mode: HIGH
 * and driven IS the run state, and handing the pin back to a floating input
 * would undo the arm we just performed. The previous config nibble is printed
 * so it can be put back by hand if needed. */
void cmd_fpga_dbgarm(const char *args)
{
    uint32_t low_ms = 2;

    if (args && *args) {
        if (parse_int(args, &low_ms) != 0 || low_ms > 1000u) {
            usb_send_str("Usage: fpga dbgarm [low_ms 0..1000]\r\n");
            return;
        }
    }

    if (!fpga_acq_pause()) {
        usb_send_str("ERR: acq task did not park — bus not safe, aborting\r\n");
        return;
    }

    /* PB11 is pin 11 => CRH/cfghr nibble (11-8)*4 = 12. Single-pin gpio_init,
     * never a whole-word write (see the PC8/PC13 incident noted on dbgclk). */
    uint32_t saved_nib = (GPIOB->cfghr >> 12) & 0xFu;
    uint32_t saved_lvl = (GPIOB->odt >> 11) & 1u;

    gpio_init_type gpio_cfg;
    gpio_default_para_init(&gpio_cfg);
    gpio_cfg.gpio_pins            = GPIO_PINS_11;
    gpio_cfg.gpio_mode            = GPIO_MODE_OUTPUT;
    gpio_cfg.gpio_out_type        = GPIO_OUTPUT_PUSH_PULL;
    gpio_cfg.gpio_pull            = GPIO_PULL_NONE;
    gpio_cfg.gpio_drive_strength  = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(GPIOB, &gpio_cfg);

    GPIOB->clr = (1u << 11);                     /* run_enable LOW  */
    if (low_ms) vTaskDelay(pdMS_TO_TICKS(low_ms));
    else        dbg_delay_us(100);
    GPIOB->scr = (1u << 11);                     /* LOW->HIGH = re-arm + run */

    fpga_acq_resume();

    usb_debug_printf("dbgarm: PB11 LOW %lums -> HIGH (armed, free-running). "
                     "was cfg nibble %lX level %lu; left as output-pp HIGH. PC0=%d\r\n",
                     (unsigned long)low_ms,
                     (unsigned long)saved_nib, (unsigned long)saved_lvl,
                     (GPIOC->idt & (1u << 0)) ? 1 : 0);
    usb_send_str("Next: `fpga dbgclk <n>` to clock samples in, then "
                 "`spi3 opread 04 1026 dump`.\r\n");
}
#endif /* FPGA_ALT_BITSTREAM */

/* fpga reinit [br] [prelude_gap_ms] [post_close_ms] — replay the full SPI3
 * config handshake on demand (prelude → 0x3B bitstream → 0x3A close → scope
 * config) and report the result. Lets us sweep the handshake parameters in
 * seconds without reflashing, chasing why our upload activates the FPGA slave
 * but never reaches stock's configured state (close F8 / status 00 01 42 2E).
 * Defaults match the stock-captured timing: br=0 (/2), gap=100ms, close=600ms. */
void cmd_fpga_reinit(const char *args)
{
    /* guest-bringup leaves the bus released (PB3/5/6 Hi-Z, SPE off). reinit means
     * "take the bus and configure", so reacquire it first — otherwise the whole
     * handshake clocks into a dead wire (H7, 2026-08-26). No-op on other builds. */
    if (fpga.bus_released) {
        fpga_spi3_bus_reacquire();
        usb_send_str("reinit: bus was released -> reacquired SPI3 (PB3/4/5/6, SPE)\r\n");
    }

    fpga_cfg_seq_opts_t opt = {
        .upload_br = 0, .prelude_gap_ms = 100, .post_close_ms = 600, .arm_pb11 = 1,
        .reset_port = 0, .reset_pin = 0, .reset_low_ms = 10,
        .prelude_frame_mode = 0, .pre_upload_gap_ms = 0, .cmd_br = 0,
        .strap_pd2 = 0, .strap_pd1213 = 0, .trailing_clocks = 0,
        .probe_edit = 0, .reload_3c = 0,
    };
    char buf[80];
    if (args && *args) {
        if (strlen(args) >= sizeof(buf)) { usb_send_str("ERR: line too long\r\n"); return; }
        strcpy(buf, args);
        char *save = NULL;
        char *t0 = strtok_r(buf, " \t", &save);
        char *t1 = t0 ? strtok_r(NULL, " \t", &save) : NULL;
        char *t2 = t1 ? strtok_r(NULL, " \t", &save) : NULL;
        /* Every number below is parse_int()-checked. Bare strtoul() used to
         * turn an unparseable token into 0 SILENTLY, which on this command is
         * the worst possible failure: `fpga reinit slow` discarded all three
         * stock-captured defaults (br=0/gap=100/close=600), ran a different
         * handshake, and echoed "br=0 gap=0ms close=0ms" as though that were
         * what was asked for. 0 is a legal value, so the echo could not
         * disambiguate it. This is a sweep tool; a typo must not quietly
         * become a data point. */
        if ((t0 && parse_int(t0, &opt.upload_br) != 0) ||
            (t1 && parse_int(t1, &opt.prelude_gap_ms) != 0) ||
            (t2 && parse_int(t2, &opt.post_close_ms) != 0)) {
            usb_send_str("ERR: br/gap/close must be numbers. See `help`.\r\n");
            return;
        }
        /* Remaining tokens are optional, order-independent, prefix-classified:
         *   a..e<pin> = FPGA reset pulse pin (e.g. b9)
         *   f<0|1|2>  = prelude frame mode (split/combined/merge)
         *   u<ms>     = pre-upload digest gap after CONFIG_ENABLE */
        for (char *tk = strtok_r(NULL, " \t", &save); tk;
             tk = strtok_r(NULL, " \t", &save)) {
            uint32_t n;
            if (tk[0] >= 'a' && tk[0] <= 'e' && tk[1]) {   /* <port><pin> */
                if (parse_int(tk + 1, &n) != 0 || n > 15u) goto reinit_bad_token;
                opt.reset_port = (uint8_t)(tk[0] - 'a' + 1);
                opt.reset_pin  = (uint8_t)n;
            } else if (tk[0] == 'f') {
                if (parse_int(tk + 1, &n) != 0 || n > 2u) goto reinit_bad_token;
                opt.prelude_frame_mode = (uint8_t)n;
            } else if (tk[0] == 'u') {
                if (parse_int(tk + 1, &n) != 0) goto reinit_bad_token;
                opt.pre_upload_gap_ms = n;
            } else if (tk[0] == 'k') {  /* 'k' (clock) — NOT 'c', which collides
                                         * with reset-port c (a..e) above */
                if (parse_int(tk + 1, &n) != 0 || n > 7u) goto reinit_bad_token;
                opt.cmd_br = n;
            } else if (tk[0] == 's' && tk[1] != '\0') {
                                        /* strap-hold: s2[h|l]=PD2, sd[h|l]=PD12+13
                                         * (default HIGH = stock). GPIO-audit lead.
                                         * sb = single-BR (EXP-34): no SPE toggle
                                         * through the config transaction.
                                         * tk[1] is tested first now: reading tk[2]
                                         * on a bare "s" ran off the end of the
                                         * token. */
                uint8_t lvl = (tk[2] == 'l') ? 2 : 1;
                if (tk[1] == '2')      opt.strap_pd2    = lvl;
                else if (tk[1] == 'd') opt.strap_pd1213 = lvl;
                else if (tk[1] == 'b') opt.single_br    = 1;
                else goto reinit_bad_token;
            } else if (tk[0] == 't' && tk[1] == 'c') {  /* tc<N> = trailing clocks
                                         * after bitstream, before 0x3A (sibling ~200) */
                if (parse_int(tk + 2, &n) != 0 || n > 0xFFFFu) goto reinit_bad_token;
                opt.trailing_clocks = (uint16_t)n;
            } else if (strcmp(tk, "pe") == 0) {  /* pe = probe SYSTEM_EDIT_MODE:
                                         * read STATUS at /256 right after 0x15 */
                opt.probe_edit = 1;
            } else if (strcmp(tk, "rl") == 0) {  /* rl = send 0x3C RELOAD before
                                         * the prelude (software reconfig trigger) */
                opt.reload_3c = 1;
            } else {
                /* No silent drop. The chain used to end here with nothing, so a
                 * mistyped sweep option ran the BASELINE handshake and reported
                 * success — and got written down as a data point for a
                 * parameter that was never applied. */
reinit_bad_token:
                usb_debug_printf("ERR: unknown or malformed reinit option '%s' — "
                                 "nothing was sent. See `help`.\r\n", tk);
                return;
            }
        }
    }

    static const char *const frame_name[] = { "split", "combined", "merge" };
    const char *fn = opt.prelude_frame_mode < 3 ? frame_name[opt.prelude_frame_mode] : "?";
    if (opt.reset_port)
        usb_debug_printf("reinit: br=%lu gap=%lums close=%lums frame=%s(%u) upgap=%lums cmdbr=%lu RESET=%c%u(%ums)\r\n",
                         opt.upload_br, opt.prelude_gap_ms, opt.post_close_ms,
                         fn, opt.prelude_frame_mode, opt.pre_upload_gap_ms, opt.cmd_br,
                         'A' + opt.reset_port - 1, opt.reset_pin, opt.reset_low_ms);
    else
        usb_debug_printf("reinit: br=%lu prelude_gap=%lums post_close=%lums frame=%s(%u) upgap=%lums cmdbr=%lu (no reset)\r\n",
                         opt.upload_br, opt.prelude_gap_ms, opt.post_close_ms,
                         fn, opt.prelude_frame_mode, opt.pre_upload_gap_ms, opt.cmd_br);
    if (opt.strap_pd2 || opt.strap_pd1213)
        usb_debug_printf("reinit: STRAP PD2=%s PD12/13=%s (held thru handshake)\r\n",
                         opt.strap_pd2 == 1 ? "HIGH" : opt.strap_pd2 == 2 ? "LOW" : "-",
                         opt.strap_pd1213 == 1 ? "HIGH" : opt.strap_pd1213 == 2 ? "LOW" : "-");
    if (opt.trailing_clocks)
        usb_debug_printf("reinit: trailing_clocks=%u (after bitstream, before 0x3A)\r\n",
                         opt.trailing_clocks);
    if (opt.single_br)
        usb_send_str("reinit: SINGLE-BR — no spi3_set_br/SPE toggle through the transaction (EXP-34)\r\n");

    uint8_t close = fpga_spi3_config_sequence(&opt);

    usb_debug_printf("0x3A close: %02X (stock F8)\r\n", close);
    usb_debug_printf("0x03 status: %02X %02X %02X %02X (stock 00 01 42 2E)\r\n",
                     fpga.scope_status[0], fpga.scope_status[1],
                     fpga.scope_status[2], fpga.scope_status[3]);

    /* Gowin STATUS_REGISTER (0x41) — the authoritative config status. Decode the
     * named bits so one reinit run tells us WHICH side of the wall we're on:
     *   all-FF  -> FPGA not driving MISO (never entered config-receive)
     *   CRC/IDV -> bytes reached the config engine (wire/content problem)
     *   DONE    -> config actually took. */
    uint32_t sr = ((uint32_t)fpga.cfg_status_reg[0] << 24) |
                  ((uint32_t)fpga.cfg_status_reg[1] << 16) |
                  ((uint32_t)fpga.cfg_status_reg[2] << 8) |
                  (uint32_t)fpga.cfg_status_reg[3];
    usb_debug_printf("0x41 STATUS: %02X %02X %02X %02X (raw=%08lX)\r\n",
                     fpga.cfg_status_reg[0], fpga.cfg_status_reg[1],
                     fpga.cfg_status_reg[2], fpga.cfg_status_reg[3], sr);
    if (sr == 0xFFFFFFFFu || sr == 0x00000000u) {
        usb_debug_printf("  -> %s (no valid status; FPGA not in SSPI config-receive)\r\n",
                         sr == 0xFFFFFFFFu ? "all-FF" : "all-00");
    } else {
        usb_debug_printf("  flags:%s%s%s%s%s%s%s\r\n",
                         (sr & (1u << 0))  ? " CRC_ERR"   : "",
                         (sr & (1u << 1))  ? " BAD_CMD"   : "",
                         (sr & (1u << 2))  ? " ID_FAIL"   : "",
                         (sr & (1u << 12)) ? " GWVLD"     : "",
                         (sr & (1u << 13)) ? " DONE"      : "",
                         (sr & (1u << 15)) ? " READY"     : "",
                         (sr & (1u << 16)) ? " POR"       : "");
    }
    /* Post-CONFIG_ENABLE STATUS (probe_edit): did 0x15 engage SYSTEM_EDIT_MODE
     * (bit7)? Read at /256 right after 0x15, before the bitstream — the precise
     * wall test (openFPGALoader's enableCfg() polls exactly this bit). */
    if (opt.reload_3c)
        usb_send_str("reinit: sent 0x3C RELOAD before prelude\r\n");
    if (opt.probe_edit) {
        uint32_t es = ((uint32_t)fpga.edit_mode_status[0] << 24) |
                      ((uint32_t)fpga.edit_mode_status[1] << 16) |
                      ((uint32_t)fpga.edit_mode_status[2] << 8) |
                      (uint32_t)fpga.edit_mode_status[3];
        usb_debug_printf("post-0x15 STATUS@/256: %02X %02X %02X %02X (raw=%08lX)\r\n",
                         fpga.edit_mode_status[0], fpga.edit_mode_status[1],
                         fpga.edit_mode_status[2], fpga.edit_mode_status[3], es);
        usb_debug_printf("  EDIT_MODE(bit7)=%s  flags:%s%s%s%s%s%s\r\n",
                         (es & (1u << 7))  ? "YES (config-receive engaged!)" : "no (0x15 did NOT engage)",
                         (es & (1u << 0))  ? " CRC_ERR" : "",
                         (es & (1u << 2))  ? " ID_FAIL" : "",
                         (es & (1u << 12)) ? " GWVLD"   : "",
                         (es & (1u << 13)) ? " DONE"    : "",
                         (es & (1u << 15)) ? " READY"   : "",
                         (es & (1u << 17)) ? " FLASH_LOCK" : "");
    }
    usb_send_str("--- acqread after reinit ---\r\n");
    cmd_spi3_acqread_one(0x04);
    cmd_spi3_acqread_one(0x05);
}

/* spi3 seq <bytes> | <bytes> [| ...] — like "spi3 xfer" but pulse CS (PB6
 * HIGH then LOW, ~tens of us inside this one handler) at each "|" separator.
 * Reproduces ripcord's cmd-09 pattern "09 FF FF | 0A FF FF", where a
 * mid-sequence CS pulse splits the command byte from the embedded 0x0A
 * sub-opcode. CS is always deasserted on exit. */
void cmd_spi3_seq(const char *args)
{
    uint8_t tx[SPI3_XFER_MAX];
    uint8_t rx[SPI3_XFER_MAX];
    uint8_t pulse_after[SPI3_XFER_MAX];   /* 1 = pulse CS after this byte */
    char buf[200];
    char *saveptr = NULL;
    char *tok;
    uint32_t n = 0;

    if (strlen(args) >= sizeof(buf)) { usb_send_str("ERR: line too long\r\n"); return; }
    strcpy(buf, args);

    for (tok = strtok_r(buf, " \t", &saveptr); tok; tok = strtok_r(NULL, " \t", &saveptr)) {
        if (strcmp(tok, "|") == 0) {
            if (n == 0) { usb_send_str("ERR: '|' before any byte\r\n"); return; }
            pulse_after[n - 1] = 1;       /* pulse CS after the previous byte */
            continue;
        }
        char *end;
        unsigned long v = strtoul(tok, &end, 16);   /* bare hex; "0x" optional */
        if (n >= SPI3_XFER_MAX) { usb_debug_printf("ERR: max %d bytes\r\n", SPI3_XFER_MAX); return; }
        if (*end != '\0' || v > 0xFF) { usb_debug_printf("ERR: bad hex byte '%s'\r\n", tok); return; }
        pulse_after[n] = 0;
        tx[n++] = (uint8_t)v;
    }
    if (n == 0) { usb_send_str("Usage: spi3 seq <b..> | <b..>\r\n"); return; }

    GPIOB->clr = (1 << 6);          /* CS assert */
    for (uint32_t i = 0; i < n; i++) {
        rx[i] = spi3_raw_xfer(tx[i]);
        if (pulse_after[i]) {
            GPIOB->scr = (1 << 6);              /* CS HIGH */
            for (volatile int d = 0; d < 4000; d++);
            GPIOB->clr = (1 << 6);              /* CS LOW */
            for (volatile int d = 0; d < 4000; d++);
        }
    }
    GPIOB->scr = (1 << 6);          /* CS deassert — always */

    usb_send_str("MISO:");
    for (uint32_t i = 0; i < n; i++) {
        usb_debug_printf(" %02X", rx[i]);
        if (pulse_after[i]) usb_send_str(" |");
    }
    usb_send_str("\r\n");
}

/* ═══════════════════════════════════════════════════════════════════
 * Command Dispatcher
 * ═══════════════════════════════════════════════════════════════════ */

/* Extracted from the old dispatch chain (was an inline block). SPI3 claim/
 * release is handled by the dispatcher via SC_SPI3. */
void cmd_spi3_probe(void)
{
    /* Bit-bang SPI3 probe: disable SPI peripheral, manually toggle
     * SCK and read MISO to test if the FPGA drives the line. This pokes the
     * SPI3 registers directly, so an unparked acq task would corrupt far
     * more than one frame — SC_SPI3 on the table row handles the park. */
    usb_send_str("=== SPI3 Bit-Bang Probe ===\r\n");

    /* Read PB4 (MISO) idle state */
    uint32_t miso_idle = (GPIOB->idt & (1 << 4)) ? 1 : 0;
    usb_debug_printf("MISO idle (CS high): %lu\r\n", miso_idle);

    /* Assert CS (PB6 LOW) */
    GPIOB->clr = (1 << 6);
    for (volatile int d = 0; d < 1000; d++);  /* brief delay */
    uint32_t miso_cs = (GPIOB->idt & (1 << 4)) ? 1 : 0;
    usb_debug_printf("MISO after CS assert: %lu\r\n", miso_cs);

    /* Try reading through SPI peripheral */
    volatile uint32_t *spi_sts = (volatile uint32_t *)0x40003C08;
    volatile uint32_t *spi_dt  = (volatile uint32_t *)0x40003C0C;

    /* Clear any pending RX data */
    if (*spi_sts & 0x01) { (void)*spi_dt; }

    /* Send 0x00 and read response */
    uint32_t timeout = 100000;
    while (!(*spi_sts & 0x02) && --timeout);  /* Wait TXE */
    *spi_dt = 0x00;  /* Send dummy byte */
    timeout = 100000;
    while (!(*spi_sts & 0x01) && --timeout);  /* Wait RXNE */
    uint8_t rx = (uint8_t)*spi_dt;
    usb_debug_printf("SPI3 xfer(0x00) = 0x%02X (timeout=%lu)%s\r\n", rx, timeout,
                     timeout ? "" : "  <-- RXNE TIMED OUT: byte is stale, not a reply");

    /* Send 0x05 (FPGA query cmd) */
    timeout = 100000;
    while (!(*spi_sts & 0x02) && --timeout);
    *spi_dt = 0x05;
    timeout = 100000;
    while (!(*spi_sts & 0x01) && --timeout);
    rx = (uint8_t)*spi_dt;
    usb_debug_printf("SPI3 xfer(0x05) = 0x%02X (timeout=%lu)%s\r\n", rx, timeout,
                     timeout ? "" : "  <-- RXNE TIMED OUT: byte is stale, not a reply");

    /* Send another 0x00 */
    timeout = 100000;
    while (!(*spi_sts & 0x02) && --timeout);
    *spi_dt = 0x00;
    timeout = 100000;
    while (!(*spi_sts & 0x01) && --timeout);
    rx = (uint8_t)*spi_dt;
    usb_debug_printf("SPI3 xfer(0x00) = 0x%02X (timeout=%lu)%s\r\n", rx, timeout,
                     timeout ? "" : "  <-- RXNE TIMED OUT: byte is stale, not a reply");

    /* Deassert CS */
    GPIOB->scr = (1 << 6);
    usb_debug_printf("SPI3 STS: 0x%08lX  CTRL1: 0x%08lX\r\n",
                     *spi_sts, *(volatile uint32_t *)0x40003C00);

    /* Also check PC6 state */
    usb_debug_printf("PC6 (SPI enable): %d\r\n",
                     (GPIOC->idt & (1 << 6)) ? 1 : 0);
}
