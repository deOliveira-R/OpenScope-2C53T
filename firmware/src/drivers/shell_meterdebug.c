/*
 * OpenScope 2C53T - the meter diagnostics of the shell (FEATURE_METERDEBUG)
 *
 * The DMM link diagnostics: `meter trace/dump/frontend/stream/mux-...`, the
 * boot-sequence and PC11 timing replays, the waveform sampler state, the
 * debug overlay and the link experiments (hdr, fast, keepalive, beep). The
 * meter itself does not need them: `meter state`, `meter func`, `meter
 * relays`, `meter degf` and `meter auto` are core (usb_debug.c).
 *
 * The handlers were moved here unchanged from usb_debug.c (feature manifest
 * F3). They reach the shell core through shell_internal.h; the rows of the
 * command table are in shell_meterdebug.h.
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
#include "../ui/trig_menu.h"
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
#include "shell_meterdebug.h"

static uint16_t count_non_ff_bytes(const uint8_t *bytes, uint8_t len)
{
    uint16_t count = 0;

    for (uint8_t i = 0; i < len; i++) {
        if (bytes[i] != 0xFFU) {
            count++;
        }
    }

    return count;
}

static uint16_t count_post_h2_non_ff_snapshot(
    uint8_t rx_len[FPGA_POST_H2_TRIGGER_HISTORY],
    uint8_t rx[FPGA_POST_H2_TRIGGER_HISTORY][FPGA_POST_H2_RX_HISTORY])
{
    uint16_t count = 0;

    for (uint8_t i = 0; i < FPGA_POST_H2_TRIGGER_HISTORY; i++) {
        uint8_t shown = rx_len[i];
        if (shown > FPGA_POST_H2_RX_HISTORY) {
            shown = FPGA_POST_H2_RX_HISTORY;
        }
        count = (uint16_t)(count + count_non_ff_bytes(rx[i], shown));
    }

    return count;
}

/* `meter hdr [on|off]` — EXP-25 (issue #15). A/B the meter TX frame header
 * between AA 55 (the default since the word table was corrected; what the
 * meter SoC requires, measured on two units) and 00 00 (what this project sent
 * before 2026-09-12, kept as the negative control: a transition under 00 00
 * must produce zero echoes). Prints the echo ladder every time, so the A/B is
 * one command and the numbers are on the same screen as the setting.
 *
 * Read the ladder from the bottom up when it stays at zero:
 *   rx_bytes    moving => the RX ISR runs at all
 *   data_frames moving => the frame sync + parser work (POSITIVE CONTROL:
 *                         this is the 5A A5 branch, proven every session)
 *   echo_start  moving => an 0xAA actually arrived and started a frame
 *   echo_hdr    moving => the 0x55 second byte confirmed it
 *   echo_valid  moving => byte[3] matched our cmd and byte[7] was 0xAA
 *   echo_bad    moving => an echo arrived but failed that validation
 * echo_start == 0 with data_frames climbing puts the fault on the WIRE, not
 * in our parser. That distinction is the whole point of printing all six. */
void cmd_meter_hdr(const char *args)
{
    while (*args == ' ') args++;
    if      (strncmp(args, "on",  2) == 0) fpga_meter_tx_header_set(true);
    else if (strncmp(args, "off", 3) == 0) fpga_meter_tx_header_set(false);
    else if (*args) { usb_send_str("usage: meter hdr [on|off]\r\n"); return; }

    bool aa55 = fpga_meter_tx_header_get();
    usb_debug_printf(
        "meter TX header: %s  (frame[0]=%02X frame[1]=%02X)\r\n"
        "  tx_count=%u rx_bytes=%u data_frames=%u\r\n"
        "  echo_start=%u echo_hdr=%u echo_valid=%u echo_bad=%u echo_frames=%u\r\n"
        "  transition: wake_ms=%u tries=%u ok=%u | unconfirmed=%u wake_timeouts=%u\r\n",
        aa55 ? "AA 55 (default)" : "00 00 (legacy, negative control)",
        aa55 ? 0xAA : 0x00, aa55 ? 0x55 : 0x00,
        (unsigned)fpga.tx_count, (unsigned)fpga.rx_byte_count,
        (unsigned)fpga.frame_count,
        (unsigned)fpga.rx_sync_echo_start_count,
        (unsigned)fpga.rx_sync_echo_header_count,
        (unsigned)fpga.rx_echo_valid_count,
        (unsigned)fpga.rx_echo_bad_count,
        (unsigned)fpga.echo_count,
        (unsigned)fpga.meter_soc_wake_ms,
        (unsigned)fpga.meter_selector_attempts,
        (unsigned)fpga.meter_selector_confirmed,
        (unsigned)fpga.meter_selector_unconfirmed_total,
        (unsigned)fpga.meter_soc_wake_timeouts);
}

void cmd_meter_fast(const char *args)
{
    bool on;
    if (shell_onoff_arg(args, &on)) fpga_meter_fast_switch_set(on);
    else if (*args) { usb_send_str("usage: meter fast [on|off]\r\n"); return; }
    usb_debug_printf("meter fast switching: %s (soc_live=%u, fast=%u fallbacks=%u)\r\n",
                     onoff(fpga_meter_fast_switch_get()), fpga_meter_soc_live() ? 1U : 0U,
                     (unsigned)fpga.meter_fast_switch_count,
                     (unsigned)fpga.meter_fast_fallback_count);
}

void cmd_meter_keepalive(const char *args)
{
    bool on;
    if (shell_onoff_arg(args, &on)) fpga_meter_keepalive_set(on);
    else if (*args) { usb_send_str("usage: meter keepalive [on|off]\r\n"); return; }
    usb_debug_printf("meter keepalive: %s  data_frames=%u tx_count=%u (read twice, 2 s apart,"
                     " to see whether frames keep coming)\r\n",
                     onoff(fpga_meter_keepalive_get()),
                     (unsigned)fpga.frame_count, (unsigned)fpga.tx_count);
}

/* The meter's debug overlay. Short SAVE used to toggle it; since UI round 1
 * SAVE is the screenshot in every mode (owner decision D1) and the overlay is
 * here only. Toggles; the meter repaints on its own when it changes. */
void cmd_meter_overlay(const char *args)
{
    (void)args;
    meter_toggle_debug_overlay();
    usb_send_str("meter debug overlay toggled\r\n");
}

void cmd_meter_beep(const char *args)
{
    bool on, m_on, active;
    int8_t port;
    uint32_t edges, pulses, gated, tick;

    while (*args == ' ') args++;
    if (shell_onoff_arg(args, &on)) {
        continuity_buzzer_soc_edge_mode_set(on);
    } else if (strncmp(args, "tick", 4) == 0) {
        uint32_t ms;
        if (parse_int(args + 4 + (args[4] == ' '), &ms) != 0 || ms < 1 || ms > 1000) {
            usb_send_str("usage: meter beep tick <1-1000 ms per housekeeping tick>\r\n");
            return;
        }
        continuity_buzzer_soc_tick_ms_set(ms);
    } else if (strncmp(args, "src", 3) == 0) {
        /* meter beep src <c|d|e|off> [rise|fall|both] [pu|pd] */
        const char *p = args + 3;
        int8_t prt;
        uint8_t edge = 2;
        bool pu = true;
        while (*p == ' ') p++;
        if (strncmp(p, "off", 3) == 0) prt = -1;
        else if (*p >= 'c' && *p <= 'e') prt = (int8_t)(*p - 'a');
        else if (*p >= 'C' && *p <= 'E') prt = (int8_t)(*p - 'A');
        else { usb_send_str("usage: meter beep src <c|d|e|off> [rise|fall|both] [pu|pd]  (pin 3 of the port = EXTI3; A and B carry USART2/SPI3)\r\n"); return; }
        if (strstr(p, "rise")) edge = 0;
        else if (strstr(p, "fall")) edge = 1;
        if (strstr(p, "pd")) pu = false;
        if (!continuity_buzzer_soc_edge_config(prt, edge, pu)) {
            usb_send_str("meter beep src: refused\r\n");
            return;
        }
    } else if (*args) {
        usb_send_str("usage: meter beep [on|off | src <c|d|e|off> [rise|fall|both] [pu|pd] | tick <ms>]\r\n");
        return;
    }
    continuity_buzzer_soc_status(&m_on, &port, &edges, &pulses, &gated, &active, &tick);
    usb_debug_printf(
        "meter beep: soc_edge_mode=%s exti3_port=%c edges=%lu pulses=%lu gated_out=%lu active=%u"
        " tick_ms=%lu pulse_ticks=%u | gate(now: submode %u)=%u (only Auto and Continuity)\r\n",
        onoff(m_on), port < 0 ? '-' : (char)('A' + port), edges, pulses, gated,
        active ? 1U : 0U, tick, (unsigned)METER_BEEP_PULSE_TICKS, (unsigned)meter_submode,
        meter_stock_beep_gate_for_submode(meter_submode) ? 1U : 0U);
}

static int32_t scaled_i100(float value)
{
    float scaled = value * 100.0f;
    if (scaled >= 0.0f) return (int32_t)(scaled + 0.5f);
    return (int32_t)(scaled - 0.5f);
}

static int32_t scaled_i10000(float value)
{
    float scaled = value * 10000.0f;
    if (scaled >= 0.0f) return (int32_t)(scaled + 0.5f);
    return (int32_t)(scaled - 0.5f);
}

static void print_i100(const char *label, float value, const char *suffix)
{
    int32_t scaled = scaled_i100(value);
    int32_t abs_scaled = scaled < 0 ? -scaled : scaled;
    usb_debug_printf("%s%s%ld.%02ld%s\r\n",
                     label,
                     scaled < 0 ? "-" : "",
                     abs_scaled / 100,
                     abs_scaled % 100,
                     suffix ? suffix : "");
}
#define usb_meter_wave_snap (*(meter_voltage_wave_snapshot_t *)(void *)shell_bus_scratch)

void cmd_meter_dump(const char *args)
{
    {
        uint8_t f[12];
        const uint32_t n = meter_data_acv_l1_get(99u, f);
        usb_debug_printf("acv leading-1 frames: %lu logged\r\n", (unsigned long)n);
        for (unsigned k = 0; k < 8u && k < n; k++) {
            (void)meter_data_acv_l1_get(k, f);
            usb_debug_printf("  L1#%u", k);
            for (unsigned b = 0; b < 12u; b++) usb_debug_printf(" %02X", f[b]);
            usb_debug_printf("\r\n");
        }
    }
    uint32_t delay = 0;
    meter_autoselect_status_t auto_st;
    meter_reading_t snap;
    bool have_snap;
    bool live;

    if (args && *args) {
        if (parse_int(args, &delay) != 0 || delay > 5000) {
            usb_send_str("Usage: meter dump [delay_ms<=5000]\r\n");
            return;
        }
    }
    if (delay > 0) vTaskDelay(pdMS_TO_TICKS(delay));

    memset(&snap, 0, sizeof(snap));
    snap.display_str[0] = '-';
    snap.display_str[1] = '-';
    snap.display_str[2] = '-';
    snap.display_str[3] = '\0';
    snap.unit_suffix = "";
    have_snap = meter_data_snapshot(&snap);
    live = have_snap && snap.valid && snap.submode == meter_submode;
    meter_autoselect_get_status(&auto_st);

    usb_send_str("=== DMM State ===\r\n");
    usb_debug_printf("mode=%lu startup=%s meter_submode=%u (%s) layout=%u (%s)\r\n",
                     (uint32_t)current_mode,
                     startup_mode_name(startup_mode),
                     (unsigned)meter_submode,
                     meter_submode_name(meter_submode),
                     (unsigned)meter_layout,
                     meter_layout_name(meter_layout));
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
    usb_debug_printf("ui_draws=%lu full_clears=%lu partial_clears=%lu draw_us=%lu max_draw_us=%lu over_budget=%lu rendered_update=%lu last_full=%u ui_live=%u continuity_flash=%u live_same_submode=%u\r\n",
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
                     live ? 1U : 0U);
    usb_debug_printf("valid=%u reading_submode=%u class=%u updates=%lu display=%s unit=%s\r\n",
                     snap.valid ? 1U : 0U,
                     (unsigned)snap.submode,
                     (unsigned)snap.result_class,
                     snap.update_count,
                     live ? snap.display_str : "---",
                     (live && snap.unit_suffix) ? snap.unit_suffix : "");
    usb_debug_printf("transition_discard_remaining=%u transition_frame_skips=%lu\r\n",
                     (unsigned)meter_frame_discard_count,
                     meter_transition_frame_skip_count);
    usb_debug_printf("bcd_value=%d decimal_pos=%u negative=%u unit_variant=%u bar_i100=%ld aux_freq_i10=%ld\r\n",
                     snap.bcd_value,
                     (unsigned)snap.decimal_pos,
                     snap.negative ? 1U : 0U,
                     (unsigned)snap.unit_variant,
                     (long)scaled_i100(snap.bar_fraction),
                     (long)scaled_i100(snap.aux_freq_hz) / 10L);
    usb_debug_printf("flags ac=%u auto=%u hold=%u probe=%u range_ind=%u range_cmd=%u beep=%u\r\n",
                     snap.is_ac ? 1U : 0U,
                     snap.is_auto_range ? 1U : 0U,
                     snap.is_hold ? 1U : 0U,
                     (unsigned)snap.probe_type,
                     (unsigned)snap.range_indicator,
                     (unsigned)snap.range_cmd,
                     snap.continuity_beep ? 1U : 0U);
    usb_debug_printf("stock_fsm mode=%u variant=%u format=%u dc_state=%u display_cmd=%u unit_index=%u composite=%u\r\n",
                     (unsigned)snap.stock_mode,
                     (unsigned)snap.stock_variant,
                     (unsigned)snap.stock_format,
                     (unsigned)snap.stock_dc_state,
                     (unsigned)snap.stock_display_cmd,
                     (unsigned)snap.stock_unit_index,
                     (unsigned)snap.stock_composite_index);
    usb_debug_printf("frame_family expected=%u observed=%u reject=%u\r\n",
                     (unsigned)snap.expected_frame_family,
                     (unsigned)snap.observed_frame_family,
                     (unsigned)snap.reject_reason);
    usb_send_str("frame=");
    for (int i = 0; i < 12; i++) usb_debug_printf("%02X%s", snap.dbg_frame[i], i == 11 ? "" : " ");
    usb_send_str("\r\nnibbles=");
    for (int i = 0; i < 4; i++) usb_debug_printf("%02X%s", snap.dbg_nibbles[i], i == 3 ? "" : " ");
    usb_send_str(" raw_digits=");
    for (int i = 0; i < 4; i++) usb_debug_printf("%02X%s", snap.dbg_raw_digits[i], i == 3 ? "" : " ");
    usb_send_str("\r\nf6_history=");
    uint8_t f6_count = meter_f6_history_count;
    if (f6_count > METER_F6_HISTORY_LEN) f6_count = METER_F6_HISTORY_LEN;
    for (int i = 0; i < f6_count; i++) usb_debug_printf("%02X%s", meter_f6_history[i], i + 1 == f6_count ? "" : " ");
    usb_send_str("\r\n");
    usb_debug_printf("wave_samples=%lu\r\n", meter_voltage_wave_sample_count());

    usb_send_str("history newest_first:\r\n");
    uint8_t count = meter_frame_history_count;
    if (count > METER_FRAME_HISTORY_LEN) count = METER_FRAME_HISTORY_LEN;
    for (uint8_t n = 0; n < count; n++) {
        uint8_t idx = (uint8_t)((meter_frame_history_head + METER_FRAME_HISTORY_LEN - 1U - n)
                                % METER_FRAME_HISTORY_LEN);
        const meter_frame_history_t *h = &meter_frame_history[idx];
        usb_debug_printf("#%u upd=%lu sub=%u cls=%u raw=%d dp=%u unit=%s disp=%s "
                         "family=%u/%u reject=%u "
                         "f6=%02X f7=%02X f8=%02X f9=%02X extra=%04X aux_freq_i10=%u digits=%02X,%02X,%02X,%02X\r\n",
                         (unsigned)n,
                         h->update_count,
                         (unsigned)h->submode,
                         (unsigned)h->result_class,
                         h->bcd_value,
                         (unsigned)h->decimal_pos,
                         h->unit_suffix ? h->unit_suffix : "",
                         h->display_str,
                         (unsigned)h->expected_frame_family,
                         (unsigned)h->observed_frame_family,
                         (unsigned)h->reject_reason,
                         (unsigned)h->flags,
                         (unsigned)h->status,
                         (unsigned)h->meas_flags,
                         (unsigned)h->additional_status,
                         (unsigned)h->extra,
                         (unsigned)h->aux_freq_hz_i10,
                         (unsigned)h->raw_digits[0],
                         (unsigned)h->raw_digits[1],
                         (unsigned)h->raw_digits[2],
                         (unsigned)h->raw_digits[3]);
    }
}

void cmd_meter_autoscan(const char *args)
{
    uint32_t settle_ms = 2500;
    uint32_t wait_budget_ms;
    uint8_t best_mode = meter_submode;
    uint8_t best_score = 0;
    size_t candidate_count = 0;
    const uint8_t *candidates = meter_auto_candidates(&candidate_count);

    if (args && *args) {
        if (parse_int(args, &settle_ms) != 0 || settle_ms > 3000) {
            usb_send_str("Usage: meter autoscan [settle_ms<=3000]\r\n");
            return;
        }
    }
    wait_budget_ms = (settle_ms < 2500U) ? 2500U : settle_ms;

    current_mode = MODE_MULTIMETER;
    meter_layout = METER_LAYOUT_FULL;
    usb_debug_printf("autoscan settle_ms=%lu wait_budget_ms=%lu candidates=%u\r\n",
                     settle_ms,
                     wait_budget_ms,
                     (unsigned)candidate_count);

    for (uint8_t i = 0; i < candidate_count; i++) {
        uint8_t submode = candidates[i];
        uint8_t score;

        meter_submode = submode;
        meter_reset_minmaxavg();
        meter_voltage_wave_reset();
        /*
         * Autoscan is a user-visible mode walk, not a transport wake diagnostic.
         * Use the same no-wake transition as button left/right so the scan does
         * not do a DCV preamble before every candidate or double-click relays.
         */
        fpga_set_meter_mode(submode);

        score = 0;
        for (uint32_t waited = 0; waited < wait_budget_ms; waited += 100U) {
            vTaskDelay(pdMS_TO_TICKS(100));
            score = meter_auto_score(submode, (const meter_reading_t *)&meter_reading);
            if (score > 0) break;
        }
        usb_debug_printf("auto candidate sub=%u (%s) score=%u valid=%u cls=%u "
                         "family=%u/%u reject=%u "
                         "disp=%s unit=%s raw=%d dp=%u f8=%02X seq=%u word=%04X apply=%04X\r\n",
                         (unsigned)submode,
                         meter_submode_name(submode),
                         (unsigned)score,
                         meter_reading.valid ? 1U : 0U,
                         (unsigned)meter_reading.result_class,
                         (unsigned)meter_reading.expected_frame_family,
                         (unsigned)meter_reading.observed_frame_family,
                         (unsigned)meter_reading.reject_reason,
                         (meter_reading.valid && meter_reading.submode == submode)
                            ? meter_reading.display_str : "---",
                         (meter_reading.valid && meter_reading.submode == submode &&
                          meter_reading.unit_suffix) ? meter_reading.unit_suffix : "",
                         meter_reading.bcd_value,
                         (unsigned)meter_reading.decimal_pos,
                         (unsigned)meter_reading.dbg_frame[8],
                         (unsigned)fpga.meter_mode_sequence_count,
                         (unsigned)fpga.meter_mode_selector_word,
                         (unsigned)fpga.meter_mode_apply_word);
        if (score > best_score) {
            best_score = score;
            best_mode = submode;
        }
    }

    meter_submode = best_mode;
    meter_reset_minmaxavg();
    meter_voltage_wave_reset();
    fpga_set_meter_mode(best_mode);
    usb_debug_printf("autoscan selected submode=%u (%s) score=%u\r\n",
                     (unsigned)best_mode,
                     meter_submode_name(best_mode),
                     (unsigned)best_score);
}

static uint8_t gpio_level(gpio_type *port, uint16_t pin)
{
    return (port->idt & (1U << pin)) ? 1U : 0U;
}

static int parse_stream_args(const char *args, uint32_t *count, uint32_t *delay_ms,
                             const char *usage)
{
    char buf[32];
    char *tok;
    char *saveptr = NULL;

    if (args == NULL || *args == '\0') return 0;
    if (strlen(args) >= sizeof(buf)) {
        usb_send_str(usage);
        return -1;
    }

    strcpy(buf, args);
    tok = strtok_r(buf, " \t", &saveptr);
    if (tok && (parse_int(tok, count) != 0 || *count > 200)) {
        usb_send_str(usage);
        return -1;
    }
    tok = strtok_r(NULL, " \t", &saveptr);
    if (tok && (parse_int(tok, delay_ms) != 0 || *delay_ms > 5000)) {
        usb_send_str(usage);
        return -1;
    }
    tok = strtok_r(NULL, " \t", &saveptr);
    if (tok) {
        usb_send_str(usage);
        return -1;
    }
    return 0;
}

static uint16_t meter_dbg_extra(void)
{
    return ((uint16_t)meter_reading.dbg_frame[10] << 8) |
           meter_reading.dbg_frame[11];
}

static int parse_mux_arm_args(const char *args, uint32_t *portc_porte,
                              uint32_t *porta_portb, uint32_t *settle_ms,
                              const char *usage)
{
    char buf[48];
    char *tok;
    char *saveptr = NULL;

    if (args == NULL || *args == '\0' || strlen(args) >= sizeof(buf)) {
        usb_send_str(usage);
        return -1;
    }

    strcpy(buf, args);
    tok = strtok_r(buf, " \t", &saveptr);
    if (tok == NULL || parse_int(tok, portc_porte) != 0 ||
        *portc_porte > 9) {
        usb_send_str(usage);
        return -1;
    }
    tok = strtok_r(NULL, " \t", &saveptr);
    if (tok == NULL || parse_int(tok, porta_portb) != 0 ||
        *porta_portb > 9) {
        usb_send_str(usage);
        return -1;
    }
    tok = strtok_r(NULL, " \t", &saveptr);
    if (tok && (parse_int(tok, settle_ms) != 0 || *settle_ms > 5000)) {
        usb_send_str(usage);
        return -1;
    }
    tok = strtok_r(NULL, " \t", &saveptr);
    if (tok) {
        usb_send_str(usage);
        return -1;
    }
    return 0;
}

static void print_frame_hex(const char *label, const uint8_t frame[12])
{
    usb_send_str(label);
    for (int i = 0; i < 12; i++) {
        usb_debug_printf("%02X%s", (unsigned)frame[i], i == 11 ? "" : " ");
    }
    usb_send_str("\r\n");
}

static void print_volatile_frame_inline(const volatile uint8_t frame[12])
{
    for (int i = 0; i < 12; i++) {
        usb_debug_printf("%s%02X", i == 0 ? "" : " ", (unsigned)frame[i]);
    }
}

static void print_tx_frame_inline(const uint8_t frame[FPGA_TX_FRAME_SIZE])
{
    for (uint8_t i = 0; i < FPGA_TX_FRAME_SIZE; i++) {
        usb_debug_printf("%s%02X", i == 0 ? "" : " ",
                         (unsigned)frame[i]);
    }
}

void cmd_meter_trace(void)
{
    meter_reading_t snap;
    bool have_snap = meter_data_snapshot(&snap);
    bool live = have_snap && (current_mode == MODE_MULTIMETER || fpga_meter_link_on()) &&
                snap.valid && snap.submode == meter_submode;
    fpga_meter_selector_t selectors = fpga_meter_expected_selectors(meter_submode);
    fpga_meter_transition_plan_t plan =
        fpga_meter_transition_plan_for_submode(meter_submode);
    uint16_t plan_probe_word = plan.has_probe_detect ?
        (uint16_t)(gpio_level(GPIOC, 7) ? 0x0507U : 0x050AU) : 0U;
    uint16_t extra = have_snap ?
        (((uint16_t)snap.dbg_frame[10] << 8) | snap.dbg_frame[11]) : 0;
    uint8_t rxh_count;
    uint8_t rxh_frames[FPGA_RX_FRAME_HISTORY][FPGA_RX_FRAME_SIZE];
    uint16_t rxh_data[FPGA_RX_FRAME_HISTORY];
    uint16_t rxh_tx[FPGA_RX_FRAME_HISTORY];
    uint16_t rxh_echo[FPGA_RX_FRAME_HISTORY];
    uint16_t rxh_seq[FPGA_RX_FRAME_HISTORY];
    uint8_t rxh_seq_sub[FPGA_RX_FRAME_HISTORY];
    uint8_t rxh_busy[FPGA_RX_FRAME_HISTORY];
    uint8_t rxh_discard[FPGA_RX_FRAME_HISTORY];
    uint8_t txh_count;
    uint8_t txh_frames[FPGA_TX_FRAME_HISTORY][FPGA_TX_FRAME_SIZE];
    uint16_t txh_tx[FPGA_TX_FRAME_HISTORY];
    uint8_t txc_count;
    uint8_t txc_frames[FPGA_TX_FRAME_HISTORY][FPGA_TX_FRAME_SIZE];
    uint16_t txc_tx[FPGA_TX_FRAME_HISTORY];
    uint8_t mth_count;
    uint8_t mth_submode[FPGA_METER_TRANSITION_HISTORY];
    uint16_t mth_config[FPGA_METER_TRANSITION_HISTORY];
    uint16_t mth_selector[FPGA_METER_TRANSITION_HISTORY];
    uint16_t mth_apply[FPGA_METER_TRANSITION_HISTORY];
    uint8_t mth_bank[FPGA_METER_TRANSITION_HISTORY];
    uint8_t mth_bank_first[FPGA_METER_TRANSITION_HISTORY];
    uint8_t mth_bank_second[FPGA_METER_TRANSITION_HISTORY];
    uint16_t mth_probe[FPGA_METER_TRANSITION_HISTORY];
    uint16_t mth_start[FPGA_METER_TRANSITION_HISTORY];
    uint16_t mth_seq[FPGA_METER_TRANSITION_HISTORY];
    uint16_t mth_tx_before[FPGA_METER_TRANSITION_HISTORY];
    uint16_t mth_tx_after[FPGA_METER_TRANSITION_HISTORY];
    uint16_t mth_frame_before[FPGA_METER_TRANSITION_HISTORY];
    uint16_t mth_frame_after[FPGA_METER_TRANSITION_HISTORY];
    uint16_t mth_planned_gpio[FPGA_METER_TRANSITION_HISTORY];
    uint16_t mth_actual_gpio[FPGA_METER_TRANSITION_HISTORY];
    uint8_t first_rx_valid;
    uint8_t first_rx_armed;
    uint8_t first_rx_submode;
    uint16_t first_rx_seq;
    uint16_t first_rx_config;
    uint16_t first_rx_selector;
    uint16_t first_rx_apply;
    uint16_t first_rx_probe;
    uint16_t first_rx_start;
    uint16_t first_rx_planned_gpio;
    uint16_t first_rx_actual_gpio;
    uint16_t first_rx_data;
    uint16_t first_rx_tx;
    uint16_t first_rx_echo;
    uint8_t first_rx_busy;
    uint8_t first_rx_discard;
    uint8_t first_rx_frame[FPGA_RX_FRAME_SIZE];
    uint32_t first_rx_h2_bytes;
    uint8_t first_rx_h2_done;
    uint8_t first_rx_h2_post_run;
    uint8_t first_rx_h2_post_mask;
    uint8_t producer_frame[FPGA_RX_FRAME_SIZE];
    uint16_t rx_sync_data_start;
    uint16_t rx_sync_echo_start;
    uint16_t rx_sync_data_header;
    uint16_t rx_sync_echo_header;
    uint16_t rx_sync_bad_second;
    uint16_t rx_sync_stray;
    uint8_t last_echo_frame[FPGA_RX_ECHO_FRAME_SIZE];
    uint8_t rxraw_count;
    uint8_t rxraw_byte[FPGA_RX_RAW_HISTORY];
    uint16_t rxraw_tx[FPGA_RX_RAW_HISTORY];
    uint8_t rxraw_tx_index[FPGA_RX_RAW_HISTORY];
    uint8_t rxraw_rx_index[FPGA_RX_RAW_HISTORY];
    uint8_t post_h2_trigger[FPGA_POST_H2_TRIGGER_HISTORY];
    uint8_t post_h2_rx_len[FPGA_POST_H2_TRIGGER_HISTORY];
    uint8_t post_h2_rx[FPGA_POST_H2_TRIGGER_HISTORY][FPGA_POST_H2_RX_HISTORY];
    uint32_t h2_rx_00_count;
    uint32_t h2_rx_ff_count;
    uint32_t h2_rx_other_count;
    uint8_t h2_close_rx_len;
    uint8_t h2_close_rx[sizeof(fpga.h2_close_rx)];
    const factory_cal_t *factory_cal = flash_fs_factory_cal();

    taskENTER_CRITICAL();
    memcpy(producer_frame, (const void *)fpga.rx_frame, FPGA_RX_FRAME_SIZE);
    rx_sync_data_start = fpga.rx_sync_data_start_count;
    rx_sync_echo_start = fpga.rx_sync_echo_start_count;
    rx_sync_data_header = fpga.rx_sync_data_header_count;
    rx_sync_echo_header = fpga.rx_sync_echo_header_count;
    rx_sync_bad_second = fpga.rx_sync_bad_second_count;
    rx_sync_stray = fpga.rx_sync_stray_count;
    memcpy(last_echo_frame, (const void *)fpga.last_rx_echo_frame,
           sizeof(last_echo_frame));
    rxraw_count = fpga.rx_raw_history_count;
    if (rxraw_count > FPGA_RX_RAW_HISTORY) {
        rxraw_count = FPGA_RX_RAW_HISTORY;
    }
    for (uint8_t n = 0; n < rxraw_count; n++) {
        uint8_t idx = (uint8_t)((fpga.rx_raw_history_head +
                                 FPGA_RX_RAW_HISTORY - 1U - n) %
                                FPGA_RX_RAW_HISTORY);
        rxraw_byte[n] = fpga.rx_raw_history[idx];
        rxraw_tx[n] = fpga.rx_raw_history_tx_count[idx];
        rxraw_tx_index[n] = fpga.rx_raw_history_tx_index[idx];
        rxraw_rx_index[n] = fpga.rx_raw_history_rx_index[idx];
    }
    memcpy(post_h2_trigger, (const void *)fpga.post_h2_spi3_trigger,
           sizeof(post_h2_trigger));
    memcpy(post_h2_rx_len, (const void *)fpga.post_h2_spi3_rx_len,
           sizeof(post_h2_rx_len));
    memcpy(post_h2_rx, (const void *)fpga.post_h2_spi3_rx,
           sizeof(post_h2_rx));
    h2_rx_00_count = fpga.h2_rx_00_count;
    h2_rx_ff_count = fpga.h2_rx_ff_count;
    h2_rx_other_count = fpga.h2_rx_other_count;
    h2_close_rx_len = fpga.h2_close_rx_len;
    memcpy(h2_close_rx, (const void *)fpga.h2_close_rx, sizeof(h2_close_rx));
    first_rx_valid = fpga.meter_first_rx_after_transition_valid;
    first_rx_armed = fpga.meter_first_rx_after_transition_armed;
    first_rx_submode = fpga.meter_first_rx_after_transition_submode;
    first_rx_seq = fpga.meter_first_rx_after_transition_seq;
    first_rx_config = fpga.meter_first_rx_after_transition_config;
    first_rx_selector = fpga.meter_first_rx_after_transition_selector;
    first_rx_apply = fpga.meter_first_rx_after_transition_apply;
    first_rx_probe = fpga.meter_first_rx_after_transition_probe;
    first_rx_start = fpga.meter_first_rx_after_transition_start;
    first_rx_planned_gpio =
        fpga.meter_first_rx_after_transition_planned_gpio;
    first_rx_actual_gpio = fpga.meter_first_rx_after_transition_actual_gpio;
    first_rx_data = fpga.meter_first_rx_after_transition_data;
    first_rx_tx = fpga.meter_first_rx_after_transition_tx;
    first_rx_echo = fpga.meter_first_rx_after_transition_echo;
    first_rx_busy = fpga.meter_first_rx_after_transition_busy;
    first_rx_discard = fpga.meter_first_rx_after_transition_discard;
    memcpy(first_rx_frame,
           (const void *)fpga.meter_first_rx_after_transition_frame,
           FPGA_RX_FRAME_SIZE);
    first_rx_h2_bytes = fpga.meter_first_rx_after_transition_h2_bytes;
    first_rx_h2_done = fpga.meter_first_rx_after_transition_h2_done;
    first_rx_h2_post_run = fpga.meter_first_rx_after_transition_h2_post_run_count;
    first_rx_h2_post_mask = fpga.meter_first_rx_after_transition_h2_post_mask;
    rxh_count = fpga.rx_frame_history_count;
    if (rxh_count > FPGA_RX_FRAME_HISTORY) rxh_count = FPGA_RX_FRAME_HISTORY;
    for (uint8_t n = 0; n < rxh_count; n++) {
        uint8_t idx = (uint8_t)((fpga.rx_frame_history_head +
                                 FPGA_RX_FRAME_HISTORY - 1U - n) %
                                FPGA_RX_FRAME_HISTORY);
        memcpy(rxh_frames[n], (const void *)fpga.rx_frame_history[idx],
               FPGA_RX_FRAME_SIZE);
        rxh_data[n] = fpga.rx_history_frame_count[idx];
        rxh_tx[n] = fpga.rx_history_tx_count[idx];
        rxh_echo[n] = fpga.rx_history_echo_count[idx];
        rxh_seq[n] = fpga.rx_history_sequence_count[idx];
        rxh_seq_sub[n] = fpga.rx_history_sequence_submode[idx];
        rxh_busy[n] = fpga.rx_history_transition_busy[idx];
        rxh_discard[n] = fpga.rx_history_discard_remaining[idx];
    }
    txh_count = fpga.tx_frame_history_count;
    if (txh_count > FPGA_TX_FRAME_HISTORY) {
        txh_count = FPGA_TX_FRAME_HISTORY;
    }
    for (uint8_t n = 0; n < txh_count; n++) {
        uint8_t idx = (uint8_t)((fpga.tx_frame_history_head +
                                 FPGA_TX_FRAME_HISTORY - 1U - n) %
                                FPGA_TX_FRAME_HISTORY);
        memcpy(txh_frames[n], (const void *)fpga.tx_frame_history[idx],
               FPGA_TX_FRAME_SIZE);
        txh_tx[n] = fpga.tx_frame_history_tx_count[idx];
    }
    txc_count = fpga.tx_control_frame_history_count;
    if (txc_count > FPGA_TX_FRAME_HISTORY) {
        txc_count = FPGA_TX_FRAME_HISTORY;
    }
    for (uint8_t n = 0; n < txc_count; n++) {
        uint8_t idx = (uint8_t)((fpga.tx_control_frame_history_head +
                                 FPGA_TX_FRAME_HISTORY - 1U - n) %
                                FPGA_TX_FRAME_HISTORY);
        memcpy(txc_frames[n], (const void *)fpga.tx_control_frame_history[idx],
               FPGA_TX_FRAME_SIZE);
        txc_tx[n] = fpga.tx_control_frame_history_tx_count[idx];
    }
    mth_count = fpga.meter_transition_history_count;
    if (mth_count > FPGA_METER_TRANSITION_HISTORY) {
        mth_count = FPGA_METER_TRANSITION_HISTORY;
    }
    for (uint8_t n = 0; n < mth_count; n++) {
        uint8_t idx = (uint8_t)((fpga.meter_transition_history_head +
                                 FPGA_METER_TRANSITION_HISTORY - 1U - n) %
                                FPGA_METER_TRANSITION_HISTORY);
        mth_submode[n] = fpga.meter_transition_history_submode[idx];
        mth_config[n] = fpga.meter_transition_history_config[idx];
        mth_selector[n] = fpga.meter_transition_history_selector[idx];
        mth_apply[n] = fpga.meter_transition_history_apply[idx];
        mth_bank[n] = fpga.meter_transition_history_bank[idx];
        mth_bank_first[n] = fpga.meter_transition_history_bank_first[idx];
        mth_bank_second[n] = fpga.meter_transition_history_bank_second[idx];
        mth_probe[n] = fpga.meter_transition_history_probe[idx];
        mth_start[n] = fpga.meter_transition_history_start[idx];
        mth_seq[n] = fpga.meter_transition_history_sequence_count[idx];
        mth_tx_before[n] = fpga.meter_transition_history_tx_before[idx];
        mth_tx_after[n] = fpga.meter_transition_history_tx_after[idx];
        mth_frame_before[n] = fpga.meter_transition_history_frame_before[idx];
        mth_frame_after[n] = fpga.meter_transition_history_frame_after[idx];
        mth_planned_gpio[n] = fpga.meter_transition_history_planned_gpio[idx];
        mth_actual_gpio[n] = fpga.meter_transition_history_actual_gpio[idx];
    }
    taskEXIT_CRITICAL();

    usb_send_str("=== DMM Trace ===\r\n");
    usb_debug_printf("trace v=1 snapshot=%u\r\n", have_snap ? 1U : 0U);
    if (!have_snap) return;

    usb_debug_printf("context mode=%lu startup=%s ui_sub=%u "
                     "reading_sub=%u live=%u valid=%u updates=%lu\r\n",
                     (uint32_t)current_mode,
                     startup_mode_name(startup_mode),
                     (unsigned)meter_submode,
                     (unsigned)snap.submode,
                     live ? 1U : 0U,
                     snap.valid ? 1U : 0U,
                     snap.update_count);
    usb_debug_printf("producer counts tx=%u rx_bytes=%u data=%u echo=%u "
                     "rx_valid=%u\r\n",
                     fpga.tx_count,
                     fpga.rx_byte_count,
                     fpga.frame_count,
                     fpga.echo_count,
                     fpga.rx_frame_valid ? 1U : 0U);
    usb_debug_printf("producer_last_rx data=%u tx=%u echo=%u seq=%u "
                     "seq_sub=%u busy=%u discard=%u\r\n",
                     fpga.last_rx_frame_count,
                     fpga.last_rx_tx_count,
                     fpga.last_rx_echo_count,
                     fpga.last_rx_mode_sequence_count,
                     (unsigned)fpga.last_rx_mode_sequence_submode,
                     (unsigned)fpga.last_rx_transition_busy,
                     (unsigned)fpga.last_rx_discard_remaining);
    usb_debug_printf("rx_sync data_start=%u echo_start=%u data_hdr=%u "
                     "echo_hdr=%u bad_second=%u stray=%u "
                     "data_tx_busy_drop=%u echo_valid=%u echo_bad=%u\r\n",
                     rx_sync_data_start,
                     rx_sync_echo_start,
                     rx_sync_data_header,
                     rx_sync_echo_header,
                     rx_sync_bad_second,
                     rx_sync_stray,
                     fpga.rx_data_tx_busy_drop_count,
                     fpga.rx_echo_valid_count,
                     fpga.rx_echo_bad_count);
    usb_debug_printf("plan stock_mode=%u raw_low=%02X family=%u mux=%u "
                     "portc_porte=%u porta_portb=%u settle_ms=%u discard=%u "
                     "bank=%u/%02X/%02X\r\n",
                     (unsigned)selectors.function_selector,
                     (unsigned)selectors.range_selector,
                     (unsigned)plan.frame_family,
                     (unsigned)plan.mux_index,
                     (unsigned)plan.portc_porte_mux,
                     (unsigned)plan.porta_portb_mux,
                     (unsigned)plan.settle_ms,
                     (unsigned)plan.discard_frames,
                     plan.has_command_bank_prefix ? 1U : 0U,
                     (unsigned)plan.command_bank_first,
                     (unsigned)plan.command_bank_second);
    usb_debug_printf("wire config=%04X has_config=%u selector=%04X "
                     "apply=%04X has_apply=%u probe=%04X start=%04X "
                     "seq_count=%u seq_sub=%u\r\n",
                     (unsigned)plan.config_word,
                     plan.has_config_word ? 1U : 0U,
                     (unsigned)plan.selector_word,
                     (unsigned)plan.apply_word,
                     plan.has_apply_word ? 1U : 0U,
                     (unsigned)plan_probe_word,
                     (unsigned)plan.start_word,
                     (unsigned)fpga.meter_mode_sequence_count,
                     (unsigned)fpga.meter_mode_sequence_submode);
    usb_debug_printf("last_sequence config=%04X selector=%04X apply=%04X "
                     "probe=%04X start=%04X\r\n",
                     (unsigned)fpga.meter_mode_config_word,
                     (unsigned)fpga.meter_mode_selector_word,
                     (unsigned)fpga.meter_mode_apply_word,
                     (unsigned)fpga.meter_mode_probe_word,
                     (unsigned)fpga.meter_mode_start_word);
    usb_debug_printf("decoded display=%s unit=%s value_i10000=%ld raw=%d "
                     "dp=%u class=%u reject=%u family=%u/%u extra=%04X\r\n",
                     snap.valid ? snap.display_str : "---",
                     (snap.valid && snap.unit_suffix) ? snap.unit_suffix : "",
                     (long)scaled_i10000(snap.value),
                     snap.bcd_value,
                     (unsigned)snap.decimal_pos,
                     (unsigned)snap.result_class,
                     (unsigned)snap.reject_reason,
                     (unsigned)snap.expected_frame_family,
                     (unsigned)snap.observed_frame_family,
                     (unsigned)extra);
    usb_debug_printf("stock_fsm mode=%u variant=%u format=%u dc_state=%u "
                     "display_cmd=%u unit_index=%u composite=%u\r\n",
                     (unsigned)snap.stock_mode,
                     (unsigned)snap.stock_variant,
                     (unsigned)snap.stock_format,
                     (unsigned)snap.stock_dc_state,
                     (unsigned)snap.stock_display_cmd,
                     (unsigned)snap.stock_unit_index,
                     (unsigned)snap.stock_composite_index);
    usb_debug_printf("transition busy=%u discard_now=%u skip_count=%lu\r\n",
                     fpga_meter_transition_busy() ? 1U : 0U,
                     (unsigned)meter_frame_discard_count,
                     meter_transition_frame_skip_count);
    print_frame_hex("producer_frame=", producer_frame);
    print_frame_hex("parsed_frame=", snap.dbg_frame);
    usb_debug_printf("first_transition_rx valid=%u armed=%u sub=%u seq=%u "
                     "config=%04X selector=%04X apply=%04X probe=%04X "
                     "start=%04X "
                     "planned_gpio=%03X actual_gpio=%03X data=%u tx=%u "
                     "echo=%u busy=%u discard=%u h2_bytes=%lu h2_done=%u "
                     "h2_post_run=%u h2_post_mask=%02X frame=",
                     (unsigned)first_rx_valid,
                     (unsigned)first_rx_armed,
                     (unsigned)first_rx_submode,
                     first_rx_seq,
                     first_rx_config,
                     first_rx_selector,
                     first_rx_apply,
                     first_rx_probe,
                     first_rx_start,
                     first_rx_planned_gpio,
                     first_rx_actual_gpio,
                     first_rx_data,
                     first_rx_tx,
                     first_rx_echo,
                     (unsigned)first_rx_busy,
                     (unsigned)first_rx_discard,
                     first_rx_h2_bytes,
                     (unsigned)first_rx_h2_done,
                     (unsigned)first_rx_h2_post_run,
                     (unsigned)first_rx_h2_post_mask);
    print_volatile_frame_inline(first_rx_frame);
    usb_send_str("\r\n");
    usb_send_str("last_echo_frame=");
    for (uint8_t i = 0; i < FPGA_RX_ECHO_FRAME_SIZE; i++) {
        usb_debug_printf("%s%02X", i == 0 ? "" : " ",
                         (unsigned)last_echo_frame[i]);
    }
    usb_send_str("\r\n");
    usb_send_str("rx_raw newest_first:\r\n");
    for (uint8_t n = 0; n < rxraw_count; n++) {
        usb_debug_printf("rxraw n=%u tx=%u txi=%u rxi=%u byte=%02X\r\n",
                         (unsigned)n,
                         rxraw_tx[n],
                         (unsigned)rxraw_tx_index[n],
                         (unsigned)rxraw_rx_index[n],
                         (unsigned)rxraw_byte[n]);
    }
    usb_send_str("transition_history newest_first:\r\n");
    for (uint8_t n = 0; n < mth_count; n++) {
        usb_debug_printf("mth n=%u sub=%u seq=%u config=%04X selector=%04X "
                         "apply=%04X bank=%u/%02X/%02X probe=%04X "
                         "start=%04X tx=%u..%u data=%u..%u planned_gpio=%03X "
                         "actual_gpio=%03X\r\n",
                         (unsigned)n,
                         (unsigned)mth_submode[n],
                         mth_seq[n],
                         mth_config[n],
                         mth_selector[n],
                         mth_apply[n],
                         (unsigned)mth_bank[n],
                         (unsigned)mth_bank_first[n],
                         (unsigned)mth_bank_second[n],
                         mth_probe[n],
                         mth_start[n],
                         mth_tx_before[n],
                         mth_tx_after[n],
                         mth_frame_before[n],
                         mth_frame_after[n],
                         mth_planned_gpio[n],
                         mth_actual_gpio[n]);
    }
    usb_send_str("producer_history newest_first:\r\n");
    for (uint8_t n = 0; n < rxh_count; n++) {
        usb_debug_printf("rxh n=%u data=%u tx=%u echo=%u seq=%u seq_sub=%u "
                         "busy=%u discard=%u frame=",
                         (unsigned)n,
                         rxh_data[n],
                         rxh_tx[n],
                         rxh_echo[n],
                         rxh_seq[n],
                         (unsigned)rxh_seq_sub[n],
                         (unsigned)rxh_busy[n],
                         (unsigned)rxh_discard[n]);
        print_volatile_frame_inline(rxh_frames[n]);
        usb_send_str("\r\n");
    }
    usb_send_str("tx_history newest_first:\r\n");
    for (uint8_t n = 0; n < txh_count; n++) {
        usb_debug_printf("txh n=%u tx=%u frame=", (unsigned)n, txh_tx[n]);
        print_tx_frame_inline(txh_frames[n]);
        usb_send_str("\r\n");
    }
    usb_send_str("tx_control_history newest_first:\r\n");
    for (uint8_t n = 0; n < txc_count; n++) {
        usb_debug_printf("txc n=%u tx=%u frame=", (unsigned)n, txc_tx[n]);
        print_tx_frame_inline(txc_frames[n]);
        usb_send_str("\r\n");
    }
    usb_debug_printf("gpio control PC6=%u PB11=%u PC11=%u PC7=%u PC0=%u\r\n",
                     gpio_level(GPIOC, 6),
                     gpio_level(GPIOB, 11),
                     gpio_level(GPIOC, 11),
                     gpio_level(GPIOC, 7),
                     gpio_level(GPIOC, 0));
    usb_debug_printf("gpio_frontend PC12=%u PE4=%u PE5=%u PE6=%u PA15=%u "
                     "PA10=%u PB10=%u PB9=%u PA6=%u\r\n",
                     gpio_level(GPIOC, 12),
                     gpio_level(GPIOE, 4),
                     gpio_level(GPIOE, 5),
                     gpio_level(GPIOE, 6),
                     gpio_level(GPIOA, 15),
                     gpio_level(GPIOA, 10),
                     gpio_level(GPIOB, 10),
                     gpio_level(GPIOB, 9),
                     gpio_level(GPIOA, 6));
    uint16_t h2_close_nonff = count_non_ff_bytes(h2_close_rx, h2_close_rx_len);
    uint16_t post_h2_nonff =
        count_post_h2_non_ff_snapshot(post_h2_rx_len, post_h2_rx);

    usb_debug_printf("h2 bytes=%lu done=%u post_enq=%u post_run=%u "
                     "post_drop=%u post_mask=%02X post_rx_nonff=%u "
                     "spi_ok=%u spi_to=%u rx00=%lu rxff=%lu rxother=%lu "
                     "rx_nonff=%lu close_len=%u close_nonff=%u\r\n",
                     fpga.h2_bytes_sent,
                     fpga.h2_upload_done ? 1U : 0U,
                     (unsigned)fpga.post_h2_spi3_boot_enqueued,
                     (unsigned)fpga.post_h2_spi3_boot_run_count,
                     (unsigned)fpga.post_h2_spi3_boot_dropped,
                     (unsigned)fpga.post_h2_spi3_boot_mask,
                     (unsigned)post_h2_nonff,
                     fpga.spi3_ok_count,
                     fpga.spi3_total_timeouts,
                     h2_rx_00_count,
                     h2_rx_ff_count,
                     h2_rx_other_count,
                     h2_rx_00_count + h2_rx_other_count,
                     (unsigned)h2_close_rx_len,
                     (unsigned)h2_close_nonff);
    usb_debug_printf("factory_cal loaded=%u ch_size=%u channels=%u\r\n",
                     (factory_cal != NULL && factory_cal->loaded) ? 1U : 0U,
                     (unsigned)FACTORY_CAL_CHANNEL_SIZE,
                     (unsigned)FACTORY_CAL_NUM_CHANNELS);
    usb_send_str("h2_close_rx bytes=");
    for (uint8_t i = 0; i < h2_close_rx_len && i < sizeof(h2_close_rx); i++) {
        usb_debug_printf("%s%02X", i == 0 ? "" : " ",
                         (unsigned)h2_close_rx[i]);
    }
    usb_send_str("\r\n");
    for (uint8_t i = 0; i < FPGA_POST_H2_TRIGGER_HISTORY; i++) {
        uint8_t len = post_h2_rx_len[i];
        uint8_t shown = len;
        if (shown > FPGA_POST_H2_RX_HISTORY) shown = FPGA_POST_H2_RX_HISTORY;
        usb_debug_printf("h2_post_rx n=%u trigger=%02X len=%u bytes=",
                         (unsigned)i,
                         (unsigned)post_h2_trigger[i],
                         (unsigned)len);
        for (uint8_t j = 0; j < shown; j++) {
            usb_debug_printf("%s%02X", j == 0 ? "" : " ",
                             (unsigned)post_h2_rx[i][j]);
        }
        if (len > FPGA_POST_H2_RX_HISTORY) usb_send_str(" ...");
        usb_send_str("\r\n");
    }
}

void cmd_meter_frontend(void)
{
    uint16_t extra = meter_dbg_extra();
    fpga_meter_selector_t selectors = fpga_meter_expected_selectors(meter_submode);
    fpga_meter_transition_plan_t plan =
        fpga_meter_transition_plan_for_submode(meter_submode);
    uint8_t tx_count = fpga.tx_cmd_history_count;

    usb_send_str("=== DMM Frontend ===\r\n");
    usb_debug_printf("mode=%lu startup=%s meter_submode=%u (%s) reading_submode=%u valid=%u class=%u updates=%lu\r\n",
                     (uint32_t)current_mode,
                     startup_mode_name(startup_mode),
                     (unsigned)meter_submode,
                     meter_submode_name(meter_submode),
                     (unsigned)meter_reading.submode,
                     meter_reading.valid ? 1U : 0U,
                     (unsigned)meter_reading.result_class,
                     meter_reading.update_count);
    usb_debug_printf("expected_selector stock_mode=%u raw_low=%02X voltage_function_axis=%u\r\n",
                     (unsigned)selectors.function_selector,
                     (unsigned)selectors.range_selector,
                     selectors.voltage_function_axis ? 1U : 0U);
    usb_debug_printf("transition_plan family=%u mux=%u portc_porte_mux=%u porta_portb_mux=%u discard=%u settle_ms=%u selector=%04X apply=%04X has_apply=%u\r\n",
                     (unsigned)plan.frame_family,
                     (unsigned)plan.mux_index,
                     (unsigned)plan.portc_porte_mux,
                     (unsigned)plan.porta_portb_mux,
                     (unsigned)plan.discard_frames,
                     (unsigned)plan.settle_ms,
                     (unsigned)plan.selector_word,
                     (unsigned)plan.apply_word,
                     plan.has_apply_word ? 1U : 0U);
    usb_debug_printf("mode_sequence count=%u submode=%u selector=%04X apply=%04X probe=%04X start=%04X\r\n",
                     (unsigned)fpga.meter_mode_sequence_count,
                     (unsigned)fpga.meter_mode_sequence_submode,
                     (unsigned)fpga.meter_mode_selector_word,
                     (unsigned)fpga.meter_mode_apply_word,
                     (unsigned)fpga.meter_mode_probe_word,
                     (unsigned)fpga.meter_mode_start_word);
    usb_debug_printf("display=%s unit=%s bcd_value=%d dp=%u f6=%02X f7=%02X f8=%02X f9=%02X extra=%04X aux_freq_i10=%ld beep=%u discard=%u trans_skips=%lu\r\n",
                     meter_reading.valid ? meter_reading.display_str : "---",
                     (meter_reading.valid && meter_reading.unit_suffix) ? meter_reading.unit_suffix : "",
                     meter_reading.bcd_value,
                     (unsigned)meter_reading.decimal_pos,
                     (unsigned)meter_reading.dbg_frame[6],
                     (unsigned)meter_reading.dbg_frame[7],
                     (unsigned)meter_reading.dbg_frame[8],
                     (unsigned)meter_reading.dbg_frame[9],
                     (unsigned)extra,
                     (long)scaled_i100(meter_reading.aux_freq_hz) / 10L,
                     meter_reading.continuity_beep ? 1U : 0U,
                     (unsigned)meter_frame_discard_count,
                     meter_transition_frame_skip_count);
    usb_debug_printf("frame_family expected=%u observed=%u reject=%u\r\n",
                     (unsigned)meter_reading.expected_frame_family,
                     (unsigned)meter_reading.observed_frame_family,
                     (unsigned)meter_reading.reject_reason);
    usb_send_str("tx_recent:");
    for (uint8_t i = 0; i < tx_count; i++) {
        uint8_t idx = (uint8_t)((fpga.tx_cmd_history_head + 16U - tx_count + i) & 0x0FU);
        usb_debug_printf(" %02X%02X",
                         (unsigned)fpga.tx_cmd_hi_history[idx],
                         (unsigned)fpga.tx_cmd_lo_history[idx]);
    }
    usb_send_str("\r\n");
    usb_print_last_tx_frame();
    usb_print_recent_tx_frames();
    usb_debug_printf("control PC6_spi=%u PB11_active=%u PC11_meter_mux=%u PC7_probe=%u PC0_ready=%u probe_tail_override=%d\r\n",
                     gpio_level(GPIOC, 6),
                     gpio_level(GPIOB, 11),
                     gpio_level(GPIOC, 11),
                     gpio_level(GPIOC, 7),
                     gpio_level(GPIOC, 0),
                     (int)fpga_meter_probe_tail_override);
    usb_debug_printf("port_c_e PC12_route=%u PE4=%u PE5=%u PE6=%u\r\n",
                     gpio_level(GPIOC, 12),
                     gpio_level(GPIOE, 4),
                     gpio_level(GPIOE, 5),
                     gpio_level(GPIOE, 6));
    usb_debug_printf("port_a_b PA15=%u PA10=%u PB10=%u PB9=%u PA6=%u\r\n",
                     gpio_level(GPIOA, 15),
                     gpio_level(GPIOA, 10),
                     gpio_level(GPIOB, 10),
                     gpio_level(GPIOB, 9),
                     gpio_level(GPIOA, 6));
}

void cmd_meter_probe_tail(const char *args)
{
    while (args != NULL && (*args == ' ' || *args == '\t')) args++;

    if (args == NULL || *args == '\0') {
        if (fpga_meter_probe_tail_override < 0) {
            usb_debug_printf("meter probe-tail=auto PC7=%u live=%02X\r\n",
                             gpio_level(GPIOC, 7),
                             gpio_level(GPIOC, 7) ? 0x07U : FPGA_CMD_METER_NOPROBE);
        } else {
            usb_debug_printf("meter probe-tail=%02X override=1 PC7=%u\r\n",
                             (unsigned)fpga_meter_probe_tail_override,
                             gpio_level(GPIOC, 7));
        }
        return;
    }

    if (strcmp(args, "auto") == 0) {
        fpga_meter_probe_tail_override = -1;
        usb_send_str("meter probe-tail=auto\r\n");
    } else if (strcmp(args, "07") == 0 || strcmp(args, "7") == 0) {
        fpga_meter_probe_tail_override = 0x07;
        usb_send_str("meter probe-tail=07\r\n");
    } else if (strcmp(args, "0a") == 0 || strcmp(args, "0A") == 0 ||
               strcmp(args, "10") == 0) {
        fpga_meter_probe_tail_override = FPGA_CMD_METER_NOPROBE;
        usb_send_str("meter probe-tail=0A\r\n");
    } else {
        usb_send_str("Usage: meter probe-tail [auto|07|0a]\r\n");
    }
}

static void print_meter_mux_stream_line(uint32_t index)
{
    meter_reading_t snap;
    bool have_snap = meter_data_snapshot(&snap);
    bool live = (current_mode == MODE_MULTIMETER || fpga_meter_link_on()) &&
                have_snap &&
                snap.valid &&
                snap.submode == meter_submode;
    fpga_meter_selector_t selectors = fpga_meter_expected_selectors(meter_submode);
    fpga_meter_transition_plan_t plan =
        fpga_meter_transition_plan_for_submode(meter_submode);

    usb_debug_printf("t=%lu upd=%lu ui_sub=%u rd_sub=%u live=%u cls=%u "
                     "stock_mode=%u raw_low=%02X family=%u mux=%u "
                     "portc_porte=%u porta_portb=%u settle=%u ",
                     index,
                     have_snap ? snap.update_count : 0UL,
                     (unsigned)meter_submode,
                     have_snap ? (unsigned)snap.submode : 0U,
                     live ? 1U : 0U,
                     have_snap ? (unsigned)snap.result_class : 0U,
                     (unsigned)selectors.function_selector,
                     (unsigned)selectors.range_selector,
                     (unsigned)plan.frame_family,
                     (unsigned)plan.mux_index,
                     (unsigned)plan.portc_porte_mux,
                     (unsigned)plan.porta_portb_mux,
                     (unsigned)plan.settle_ms);
    usb_debug_printf("obs_family=%u reject=%u seq=%u seq_sub=%u "
                     "seq_word=%04X seq_apply=%04X tx=%u data=%u echo=%u ",
                     have_snap ? (unsigned)snap.observed_frame_family : 0U,
                     have_snap ? (unsigned)snap.reject_reason : 0U,
                     (unsigned)fpga.meter_mode_sequence_count,
                     (unsigned)fpga.meter_mode_sequence_submode,
                     (unsigned)fpga.meter_mode_selector_word,
                     (unsigned)fpga.meter_mode_apply_word,
                     fpga.tx_count,
                     fpga.frame_count,
                     fpga.echo_count);
    usb_debug_printf("disp=%s unit=%s raw=%d dp=%u f6=%02X f7=%02X "
                     "f8=%02X f9=%02X extra=%04X discard=%u ",
                     (have_snap && snap.valid) ? snap.display_str : "---",
                     (have_snap && snap.valid && snap.unit_suffix) ? snap.unit_suffix : "",
                     have_snap ? snap.bcd_value : 0,
                     have_snap ? (unsigned)snap.decimal_pos : 0U,
                     have_snap ? (unsigned)snap.dbg_frame[6] : 0U,
                     have_snap ? (unsigned)snap.dbg_frame[7] : 0U,
                     have_snap ? (unsigned)snap.dbg_frame[8] : 0U,
                     have_snap ? (unsigned)snap.dbg_frame[9] : 0U,
                     (unsigned)meter_dbg_extra(),
                     (unsigned)meter_frame_discard_count);
    usb_debug_printf("PC6=%u PB11=%u PC11=%u PC12=%u PE4=%u PE5=%u PE6=%u "
                     "PA15=%u PA10=%u PB10=%u PB9=%u PA6=%u frame=",
                     gpio_level(GPIOC, 6),
                     gpio_level(GPIOB, 11),
                     gpio_level(GPIOC, 11),
                     gpio_level(GPIOC, 12),
                     gpio_level(GPIOE, 4),
                     gpio_level(GPIOE, 5),
                     gpio_level(GPIOE, 6),
                     gpio_level(GPIOA, 15),
                     gpio_level(GPIOA, 10),
                     gpio_level(GPIOB, 10),
                     gpio_level(GPIOB, 9),
                     gpio_level(GPIOA, 6));
    if (have_snap) {
        print_volatile_frame_inline(snap.dbg_frame);
    }
    usb_send_str("\r\n");
}

void cmd_meter_mux_stream(const char *args)
{
    uint32_t count = 16;
    uint32_t delay_ms = 250;
    uint32_t last_update = 0xFFFFFFFFu;
    const char *usage = "Usage: meter mux-stream [count<=200] [delay_ms<=5000]\r\n";

    if (parse_stream_args(args, &count, &delay_ms, usage) != 0) return;

    usb_debug_printf("mux-stream count=%lu delay_ms=%lu\r\n", count, delay_ms);
    for (uint32_t i = 0; i < count; i++) {
        if (meter_reading.update_count != last_update) {
            last_update = meter_reading.update_count;
            print_meter_mux_stream_line(i);
        }
        if (delay_ms > 0) vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

void cmd_meter_mux_arms(const char *args)
{
    uint32_t portc_porte = 0;
    uint32_t porta_portb = 0;
    uint32_t settle_ms = 300;
    uint16_t planned_gpio = 0;
    uint16_t actual_gpio = 0;
    const char *usage =
        "Usage: meter mux-arms <portc_porte 0-9> <porta_portb 0-9> [settle_ms<=5000]\r\n";

    if (parse_mux_arm_args(args, &portc_porte, &porta_portb,
                           &settle_ms, usage) != 0) {
        return;
    }
    if (current_mode != MODE_MULTIMETER) {
        usb_send_str("ERR: switch to meter mode before applying DMM mux arms\r\n");
        return;
    }
    if (!fpga_debug_apply_meter_mux_arms((uint8_t)portc_porte,
                                         (uint8_t)porta_portb,
                                         &planned_gpio,
                                         &actual_gpio)) {
        usb_send_str(usage);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(settle_ms));
    /* UNOBEYED: an obeyed 0x0509 moves the SoC's display state, and this
     * trace is about the relays, not about commanding the meter.
     * Reported, not discarded: this only ENQUEUES, and if the TX queue is
     * full the START frame never goes out, so the trace below would read
     * as "the hardware did not answer" rather than "we never asked". */
    if (fpga_send_cmd_unobeyed(0x05, FPGA_CMD_METER_START) != pdTRUE)
        usb_send_str("WARN: METER_START not queued (TX queue full) — the trace"
                     " below is NOT a response to it\r\n");
    vTaskDelay(pdMS_TO_TICKS(350));

    usb_send_str("=== DMM Mux Arms Trace ===\r\n");
    usb_debug_printf("mux_arms portc_porte=%lu porta_portb=%lu settle_ms=%lu "
                     "planned_gpio=%03X actual_gpio=%03X\r\n",
                     portc_porte,
                     porta_portb,
                     settle_ms,
                     (unsigned)planned_gpio,
                     (unsigned)actual_gpio);
    cmd_meter_trace();
}

void cmd_meter_boot_sequence(const char *args)
{
    uint32_t settle_ms = 300;
    uint16_t planned_gpio = 0;
    uint16_t actual_gpio = 0;
    const char *usage = "Usage: meter boot-sequence [settle_ms<=5000]\r\n";

    if (args != NULL && *args != '\0') {
        if (parse_int(args, &settle_ms) != 0 || settle_ms > 5000U) {
            usb_send_str(usage);
            return;
        }
    }
    if (current_mode != MODE_MULTIMETER) {
        usb_send_str("ERR: switch to meter mode before replaying DMM boot order\r\n");
        return;
    }
    if (!fpga_debug_send_meter_boot_order(meter_submode,
                                          settle_ms,
                                          &planned_gpio,
                                          &actual_gpio)) {
        usb_send_str(usage);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(settle_ms));
    /* UNOBEYED: an obeyed 0x0509 moves the SoC's display state, and this
     * trace is about the relays, not about commanding the meter.
     * Reported, not discarded: this only ENQUEUES, and if the TX queue is
     * full the START frame never goes out, so the trace below would read
     * as "the hardware did not answer" rather than "we never asked". */
    if (fpga_send_cmd_unobeyed(0x05, FPGA_CMD_METER_START) != pdTRUE)
        usb_send_str("WARN: METER_START not queued (TX queue full) — the trace"
                     " below is NOT a response to it\r\n");
    vTaskDelay(pdMS_TO_TICKS(350));

    usb_send_str("=== DMM Boot-Order Trace ===\r\n");
    usb_debug_printf("boot_sequence submode=%u (%s) settle_ms=%lu "
                     "order=0508,0509,probe,selector planned_gpio=%03X actual_gpio=%03X\r\n",
                     (unsigned)meter_submode,
                     meter_submode_name(meter_submode),
                     settle_ms,
                     (unsigned)planned_gpio,
                     (unsigned)actual_gpio);
    cmd_meter_trace();
}

void cmd_meter_pc11_timing(const char *args)
{
    uint32_t low_ms = 250;
    uint32_t high_ms = 250;
    uint16_t planned_gpio = 0;
    uint16_t actual_gpio = 0;
    const char *usage = "Usage: meter pc11-timing [low_ms<=5000] [high_ms<=5000]\r\n";

    if (args != NULL && *args != '\0') {
        char buf[48];
        char *saveptr = NULL;
        char *tok;

        /* Guard the copy. strncpy() truncates silently, so an over-long
         * argument lost its tail BEFORE the trailing-token rejection below
         * could see it — the guard existed but could be walked around. Both
         * sibling parsers (parse_stream_args, parse_mux_arm_args) already
         * check this. */
        if (strlen(args) >= sizeof(buf)) {
            usb_send_str(usage);
            return;
        }
        strcpy(buf, args);
        tok = strtok_r(buf, " \t", &saveptr);
        if (tok != NULL &&
            (parse_int(tok, &low_ms) != 0 || low_ms > 5000U)) {
            usb_send_str(usage);
            return;
        }
        tok = strtok_r(NULL, " \t", &saveptr);
        if (tok != NULL &&
            (parse_int(tok, &high_ms) != 0 || high_ms > 5000U)) {
            usb_send_str(usage);
            return;
        }
        if (strtok_r(NULL, " \t", &saveptr) != NULL) {
            usb_send_str(usage);
            return;
        }
    }
    if (current_mode != MODE_MULTIMETER) {
        usb_send_str("ERR: switch to meter mode before probing DMM PC11 timing\r\n");
        return;
    }
    if (!fpga_debug_send_meter_pc11_timing(meter_submode,
                                           low_ms,
                                           high_ms,
                                           &planned_gpio,
                                           &actual_gpio)) {
        usb_send_str(usage);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(high_ms));
    /* UNOBEYED: an obeyed 0x0509 moves the SoC's display state, and this
     * trace is about the relays, not about commanding the meter.
     * Reported, not discarded: this only ENQUEUES, and if the TX queue is
     * full the START frame never goes out, so the trace below would read
     * as "the hardware did not answer" rather than "we never asked". */
    if (fpga_send_cmd_unobeyed(0x05, FPGA_CMD_METER_START) != pdTRUE)
        usb_send_str("WARN: METER_START not queued (TX queue full) — the trace"
                     " below is NOT a response to it\r\n");
    vTaskDelay(pdMS_TO_TICKS(350));

    usb_send_str("=== DMM PC11 Timing Trace ===\r\n");
    usb_debug_printf("pc11_timing submode=%u (%s) low_ms=%lu high_ms=%lu "
                     "planned_gpio=%03X actual_gpio=%03X\r\n",
                     (unsigned)meter_submode,
                     meter_submode_name(meter_submode),
                     low_ms,
                     high_ms,
                     (unsigned)planned_gpio,
                     (unsigned)actual_gpio);
    cmd_meter_trace();
}

void cmd_meter_stream(const char *args)
{
    uint32_t count = 16;
    uint32_t delay_ms = 250;
    uint32_t last_update = 0xFFFFFFFFu;
    const char *usage = "Usage: meter stream [count<=200] [delay_ms<=5000]\r\n";

    if (parse_stream_args(args, &count, &delay_ms, usage) != 0) return;

    usb_debug_printf("stream count=%lu delay_ms=%lu\r\n", count, delay_ms);
    for (uint32_t i = 0; i < count; i++) {
        meter_reading_t snap;

        if (meter_data_snapshot(&snap) &&
            snap.update_count != last_update) {
            last_update = snap.update_count;
            uint16_t extra = ((uint16_t)snap.dbg_frame[10] << 8) |
                             snap.dbg_frame[11];
            usb_debug_printf("%lu upd=%lu sub=%u cls=%u family=%u/%u reject=%u "
                             "raw=%d dp=%u unit=%s disp=%s "
                             "f6=%02X f7=%02X f8=%02X f9=%02X extra=%04X aux_freq_i10=%ld wave=%lu beep=%u\r\n",
                             i,
                             snap.update_count,
                             (unsigned)snap.submode,
                             (unsigned)snap.result_class,
                             (unsigned)snap.expected_frame_family,
                             (unsigned)snap.observed_frame_family,
                             (unsigned)snap.reject_reason,
                             snap.bcd_value,
                             (unsigned)snap.decimal_pos,
                             snap.unit_suffix ? snap.unit_suffix : "",
                             snap.display_str,
                             (unsigned)snap.dbg_frame[6],
                             (unsigned)snap.dbg_frame[7],
                             (unsigned)snap.dbg_frame[8],
                             (unsigned)snap.dbg_frame[9],
                             (unsigned)extra,
                             (long)scaled_i100(snap.aux_freq_hz) / 10L,
                             meter_voltage_wave_sample_count(),
                             snap.continuity_beep ? 1U : 0U);
        }
        if (delay_ms > 0) vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

void cmd_meter_adc_snapshot(void)
{
    meter_voltage_wave_snapshot(&usb_meter_wave_snap, METER_VOLTAGE_WAVE_RENDER_POINTS,
                                meter_reading.aux_freq_hz);

    usb_send_str("=== DMM ADC Snapshot ===\r\n");
    usb_debug_printf("mode=%lu meter_submode=%u (%s) wave_samples=%lu\r\n",
                     (uint32_t)current_mode,
                     (unsigned)meter_submode,
                     meter_submode_name(meter_submode),
                     meter_voltage_wave_sample_count());
    usb_debug_printf("sampler=%s spi_path=%s selector_override=%d last_preacq=%02X last_selector=%02X "
                     "preacq_override=%d last_preacq_rx=%02X last_sample=%02X min_sample=%02X max_sample=%02X samples=%lu ff_samples=%lu "
                     "zero_samples=%lu enq_attempts=%lu enq_success=%lu enq_drops=%lu\r\n",
                     fpga_meter_adc_sampler_enabled ? "on" : "off",
                     fpga_meter_adc_use_preacq ? "preacq" : "direct",
                     (int)fpga_meter_adc_selector_override,
                     (unsigned)fpga_meter_adc_last_preacq,
                     (unsigned)fpga_meter_adc_last_selector,
                     (int)fpga_meter_adc_preacq_override,
                     (unsigned)fpga_meter_adc_last_preacq_rx,
                     (unsigned)fpga_meter_adc_last_sample,
                     (unsigned)fpga_meter_adc_min_sample,
                     (unsigned)fpga_meter_adc_max_sample,
                     fpga_meter_adc_samples,
                     fpga_meter_adc_ff_samples,
                     fpga_meter_adc_zero_samples,
                     fpga_meter_adc_enqueue_attempts,
                     fpga_meter_adc_enqueue_success,
                     fpga_meter_adc_enqueue_drops);
    usb_debug_printf("adc_diag trans=%lu not_v=%lu gen=%lu last_gen=%lu first=%02X busy=%u discard=%u probing=%u to=%lu total_to=%lu ch=%u\r\n",
                     fpga_meter_adc_transition_skips,
                     fpga_meter_adc_not_voltage_skips,
                     fpga_meter_adc_reset_generation,
                     fpga_meter_adc_last_reset_generation,
                     (unsigned)fpga_meter_adc_first_sample_after_reset,
                     fpga_meter_transition_busy() ? 1U : 0U,
                     (unsigned)meter_frame_discard_count,
                     fpga.spi3_probing ? 1U : 0U,
                     (unsigned long)fpga.spi3_timeout_count,
                     (unsigned long)fpga.spi3_total_timeouts,
                     (unsigned)active_channel);
    usb_debug_printf("snapshot_count=%u raw_last=%u raw_min=%u raw_max=%u p2p=%u synced=%u\r\n",
                     (unsigned)usb_meter_wave_snap.count,
                     (unsigned)usb_meter_wave_snap.raw_last,
                     (unsigned)usb_meter_wave_snap.raw_min,
                     (unsigned)usb_meter_wave_snap.raw_max,
                     (unsigned)usb_meter_wave_snap.peak_to_peak_raw,
                     usb_meter_wave_snap.synced ? 1U : 0U);
    print_i100("mean_raw=", usb_meter_wave_snap.mean_raw, "");
    print_i100("rms_raw=", usb_meter_wave_snap.rms_raw, "");
    print_i100("freq=", usb_meter_wave_snap.freq_hz, " Hz");
    usb_debug_printf("dmm_display=%s dmm_unit=%s dmm_class=%u dmm_updates=%lu dmm_valid=%u\r\n",
                     meter_reading.valid ? meter_reading.display_str : "---",
                     (meter_reading.valid && meter_reading.unit_suffix) ? meter_reading.unit_suffix : "",
                     (unsigned)meter_reading.result_class,
                     meter_reading.update_count,
                     meter_reading.valid ? 1U : 0U);
}

static void cmd_meter_wave(void)
{
    extern volatile device_mode_t current_mode;
    extern volatile uint8_t meter_submode;

    uint32_t before = meter_voltage_wave_sample_count();
    vTaskDelay(pdMS_TO_TICKS(250));
    uint32_t after = meter_voltage_wave_sample_count();

    meter_voltage_wave_snapshot(&usb_meter_wave_snap, METER_VOLTAGE_WAVE_RENDER_POINTS, 0.0f);

    usb_send_str("=== SPI3 Meter ADC Probe ===\r\n");
    usb_debug_printf("mode=%lu meter_submode=%u (%s)\r\n",
                     (uint32_t)current_mode,
                     (unsigned)meter_submode,
                     meter_submode_name(meter_submode));
    usb_debug_printf("samples_total=%lu delta_250ms=%lu approx_rate=%lu Hz\r\n",
                     after, after - before, (after - before) * 4U);
    usb_debug_printf("sampler=%s spi_path=%s enq=%lu ok=%lu drop=%lu samples=%lu ff=%lu zero=%lu "
                     "selector_mode=%d preacq_mode=%d last_pre=%02X pre_rx=%02X selector=%02X last=%02X min=%02X max=%02X\r\n",
                     fpga_meter_adc_sampler_enabled ? "on" : "off",
                     fpga_meter_adc_use_preacq ? "preacq" : "direct",
                     fpga_meter_adc_enqueue_attempts,
                     fpga_meter_adc_enqueue_success,
                     fpga_meter_adc_enqueue_drops,
                     fpga_meter_adc_samples,
                     fpga_meter_adc_ff_samples,
                     fpga_meter_adc_zero_samples,
                     (int)fpga_meter_adc_selector_override,
                     (int)fpga_meter_adc_preacq_override,
                     (unsigned)fpga_meter_adc_last_preacq,
                     (unsigned)fpga_meter_adc_last_preacq_rx,
                     (unsigned)fpga_meter_adc_last_selector,
                     (unsigned)fpga_meter_adc_last_sample,
                     (unsigned)fpga_meter_adc_min_sample,
                     (unsigned)fpga_meter_adc_max_sample);
    usb_debug_printf("adc_diag trans=%lu not_v=%lu gen=%lu last_gen=%lu first=%02X busy=%u discard=%u probing=%u to=%lu total_to=%lu ch=%u\r\n",
                     fpga_meter_adc_transition_skips,
                     fpga_meter_adc_not_voltage_skips,
                     fpga_meter_adc_reset_generation,
                     fpga_meter_adc_last_reset_generation,
                     (unsigned)fpga_meter_adc_first_sample_after_reset,
                     fpga_meter_transition_busy() ? 1U : 0U,
                     (unsigned)meter_frame_discard_count,
                     fpga.spi3_probing ? 1U : 0U,
                     (unsigned long)fpga.spi3_timeout_count,
                     (unsigned long)fpga.spi3_total_timeouts,
                     (unsigned)active_channel);
    usb_debug_printf("snapshot_count=%u raw_last=%u raw_min=%u raw_max=%u p2p=%u synced=%u\r\n",
                     (unsigned)usb_meter_wave_snap.count,
                     (unsigned)usb_meter_wave_snap.raw_last,
                     (unsigned)usb_meter_wave_snap.raw_min,
                     (unsigned)usb_meter_wave_snap.raw_max,
                     (unsigned)usb_meter_wave_snap.peak_to_peak_raw,
                     usb_meter_wave_snap.synced ? 1U : 0U);
    print_i100("mean_raw=", usb_meter_wave_snap.mean_raw, "");
    print_i100("rms_raw=", usb_meter_wave_snap.rms_raw, "");
    print_i100("freq=", usb_meter_wave_snap.freq_hz, " Hz");
    usb_debug_printf("dmm=%s %s class=%u updates=%lu valid=%u\r\n",
                     meter_reading.display_str,
                     meter_reading.unit_suffix ? meter_reading.unit_suffix : "",
                     (unsigned)meter_reading.result_class,
                     meter_reading.update_count,
                     meter_reading.valid ? 1U : 0U);
    usb_send_str("Experimental SPI3 meter-ADC probe; all-FF means this path is not armed/valid, not a DMM waveform.\r\n");
}

void cmd_meter_wave_args(const char *args)
{
    if (args == NULL || *args == '\0') {
        cmd_meter_wave();
        return;
    }

    if (strcmp(args, "reset") == 0) {
        meter_voltage_wave_reset();
        fpga_meter_adc_diag_reset();
        usb_send_str("meter wave diagnostics reset\r\n");
        return;
    }

    if (strncmp(args, "sampler", 7) == 0 &&
        (args[7] == '\0' || args[7] == ' ' || args[7] == '\t')) {
        const char *value = args + 7;
        while (*value == ' ' || *value == '\t') value++;
        if (*value == '\0') {
            usb_debug_printf("meter wave sampler=%s\r\n",
                             fpga_meter_adc_sampler_enabled ? "on" : "off");
        } else if (strcmp(value, "on") == 0) {
            fpga_meter_adc_sampler_enabled = true;
            meter_voltage_wave_reset();
            fpga_meter_adc_diag_reset();
            usb_send_str("meter wave sampler=on\r\n");
        } else if (strcmp(value, "off") == 0) {
            fpga_meter_adc_sampler_enabled = false;
            meter_voltage_wave_reset();
            fpga_meter_adc_diag_reset();
            usb_send_str("meter wave sampler=off\r\n");
        } else {
            usb_send_str("Usage: meter wave sampler [on|off]\r\n");
        }
        return;
    }

    if (strncmp(args, "path", 4) == 0 &&
        (args[4] == '\0' || args[4] == ' ' || args[4] == '\t')) {
        const char *value = args + 4;
        while (*value == ' ' || *value == '\t') value++;

        if (*value == '\0') {
            usb_debug_printf("meter wave path=%s\r\n",
                             fpga_meter_adc_use_preacq ? "preacq" : "direct");
        } else if (strcmp(value, "direct") == 0) {
            fpga_meter_adc_use_preacq = false;
            meter_voltage_wave_reset();
            fpga_meter_adc_diag_reset();
            usb_send_str("meter wave path=direct\r\n");
        } else if (strcmp(value, "preacq") == 0) {
            fpga_meter_adc_use_preacq = true;
            meter_voltage_wave_reset();
            fpga_meter_adc_diag_reset();
            usb_send_str("meter wave path=preacq\r\n");
        } else {
            usb_send_str("Usage: meter wave path [direct|preacq]\r\n");
        }
        return;
    }

    if (strncmp(args, "selector", 8) == 0 &&
        (args[8] == '\0' || args[8] == ' ' || args[8] == '\t')) {
        const char *value = args + 8;
        uint32_t selector;
        while (*value == ' ' || *value == '\t') value++;

        if (*value == '\0') {
            usb_debug_printf("meter wave selector=%d\r\n",
                             (int)fpga_meter_adc_selector_override);
        } else if (strcmp(value, "auto") == 0) {
            fpga_meter_adc_selector_override = -1;
            meter_voltage_wave_reset();
            fpga_meter_adc_diag_reset();
            usb_send_str("meter wave selector=auto\r\n");
        } else if (parse_int(value, &selector) == 0 && selector <= 255U) {
            fpga_meter_adc_selector_override = (int16_t)selector;
            meter_voltage_wave_reset();
            fpga_meter_adc_diag_reset();
            usb_debug_printf("meter wave selector=%d\r\n",
                             (int)fpga_meter_adc_selector_override);
        } else {
            usb_send_str("Usage: meter wave selector [auto|0..255]\r\n");
        }
        return;
    }

    if (strncmp(args, "preacq", 6) == 0 &&
        (args[6] == '\0' || args[6] == ' ' || args[6] == '\t')) {
        const char *value = args + 6;
        uint32_t preacq;
        while (*value == ' ' || *value == '\t') value++;

        if (*value == '\0') {
            usb_debug_printf("meter wave preacq=%d\r\n",
                             (int)fpga_meter_adc_preacq_override);
        } else if (strcmp(value, "auto") == 0) {
            fpga_meter_adc_preacq_override = -1;
            meter_voltage_wave_reset();
            fpga_meter_adc_diag_reset();
            usb_send_str("meter wave preacq=auto\r\n");
        } else if (parse_int(value, &preacq) == 0 && preacq <= 255U) {
            fpga_meter_adc_preacq_override = (int16_t)preacq;
            meter_voltage_wave_reset();
            fpga_meter_adc_diag_reset();
            usb_debug_printf("meter wave preacq=%d\r\n",
                             (int)fpga_meter_adc_preacq_override);
        } else {
            usb_send_str("Usage: meter wave preacq [auto|0..255]\r\n");
        }
        return;
    }

    usb_send_str("Usage: meter wave [reset|sampler|path|selector|preacq]\r\n");
}
