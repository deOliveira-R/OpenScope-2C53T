/*
 * OpenScope 2C53T - shell commands: system
 *
 * status, SPI flash, CAN analyser, meter, settings and persistence, bootloader
 * reboot, battery, signal generator, firmware loader.
 */

#include "shell_parts.h"

/* Print the last command frame sent to the FPGA, in hex. */
static void usb_print_last_tx_frame(void)
{
    usb_send_str("last_tx_frame:");
    for (int i = 0; i < FPGA_TX_FRAME_SIZE; i++) {
        usb_debug_printf(" %02X", (unsigned)fpga.last_tx_frame[i]);
    }
    usb_send_str("\r\n");
}

/* Exported for the feature shell files (declared in shell_internal.h). */
void usb_debug_sh_print_last_tx_frame(void)
{
    usb_print_last_tx_frame();
}

/* Print the TX frame history ring, oldest first, one bracketed frame each. */
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

/* Exported for the feature shell files (declared in shell_internal.h). */
void usb_debug_sh_print_recent_tx_frames(void)
{
    usb_print_recent_tx_frames();
}

/* What the last design upload did, in words. */
static const char *upload_result_name(uint8_t r)
{
    switch ((fpga_upload_result_t)r) {
    case FPGA_UPLOAD_SENT:          return "sent";
    case FPGA_UPLOAD_NOT_EMPTY:     return "nothing sent: the FPGA already ran a design";
    case FPGA_UPLOAD_SOURCE_FAILED: return "source failed part-way: the FPGA is empty";
    default:                        return "not run";
    }
}

/* The configuration port's replies around the last upload. Only an upload
 * that got as far as sending reads them, so nothing is printed otherwise. */
static void status_print_upload(void)
{
    const uint32_t sr = fpga_cfg_status();

    usb_debug_printf("Upload: %s (%lu of %lu bytes)\r\n",
                     upload_result_name(fpga.upload_result),
                     (unsigned long)fpga.h2_bytes_sent, (unsigned long)fpga.upload_len);
    if (fpga.upload_result != FPGA_UPLOAD_SENT &&
        fpga.upload_result != FPGA_UPLOAD_SOURCE_FAILED)
        return;
    usb_debug_printf("STATUS (0x41) after upload: %08lX  DONE (bit %u): %s\r\n",
                     (unsigned long)sr, (unsigned)GOWIN_STATUS_DONE_BIT,
                     ((sr >> GOWIN_STATUS_DONE_BIT) & 1u) ? "set (the design started)" : "clear");
}

/* `status`: uptime and clock, then one section per part of the driver: the
 * FPGA and its last upload, the acquisition, the meter link. Each printf
 * stays below usb_debug_printf's buffer. */
void cmd_status(void)
{
    extern volatile uint32_t uptime_seconds;

    usb_debug_printf(
        "=== System ===\r\n"
        "Uptime: %lus\r\n"
        "SYSCLK: %uMHz\r\n"
        "\r\n=== FPGA ===\r\n"
        "Initialized: %s\r\n"
        "SPI3 pins released: %s\r\n"
        "SPI3 CTRL1: 0x%04lX  PB4 (MISO): %d  PC6 (supply): %d  PB6 (CS): %d\r\n",
        (unsigned long)uptime_seconds,
        system_core_clock / 1000000,
        fpga.initialized ? "YES" : "NO",
        fpga.bus_released ? "YES" : "NO",
        (unsigned long)SPI3->ctrl1,
        (GPIOB->idt & (1 << 4)) ? 1 : 0,
        (GPIOC->idt & (1 << 6)) ? 1 : 0,
        (GPIOB->idt & (1 << 6)) ? 1 : 0
    );
    status_print_upload();
    usb_debug_printf("Core status (op 03) after the load: %02X %02X %02X %02X\r\n",
                     fpga.scope_status[0], fpga.scope_status[1],
                     fpga.scope_status[2], fpga.scope_status[3]);

    usb_debug_printf(
        "\r\n=== Acquisition ===\r\n"
        "SPI3 OK: %u\r\n"
        "SPI3 records refused: %u (transfers timed out: %u)\r\n",
        fpga.spi3_ok_count,
        fpga.spi3_total_timeouts,
        fpga.spi3_hw_timeouts
    );
    /* PC0 edges: record pairs started by the core (read twice for a rate).
     * acq hdr: the 3 bytes before the samples of the last op 04 and 05 reply. */
    usb_debug_printf(
        "PC0 edges: %lu\r\n"
        "acq latency: %lu ms (strobe -> commit, last cycle); poll reads before it: %lu (total %lu)\r\n"
        "acq rotation: %ld samples (seam of the last strobed record; -1 = none)\r\n"
        "acq hdr CH1: %02X %02X %02X  CH2: %02X %02X %02X\r\n",
        (unsigned long)fpga.pc0_edges,
        (unsigned long)fpga.acq_last_latency_ms, (unsigned long)fpga.acq_polls_last, (unsigned long)fpga.acq_poll_reads,
        (long)fpga.acq_last_rot,
        fpga.acq_hdr_ch1[0], fpga.acq_hdr_ch1[1], fpga.acq_hdr_ch1[2],
        fpga.acq_hdr_ch2[0], fpga.acq_hdr_ch2[1], fpga.acq_hdr_ch2[2]
    );
    /* The timebase in force: register 0x01, sample rate, s/div. */
    timebase_print_status(false);

    usb_debug_printf(
        "\r\n=== Meter link ===\r\n"
        "TX count: %u\r\n"
        "RX bytes: %u\r\n"
        "Data frames: %u\r\n"
        "Echo frames: %u\r\n"
        "RX sync: data_start=%u echo_start=%u data_hdr=%u echo_hdr=%u bad2=%u stray=%u\r\n",
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
    usb_print_last_tx_frame();
    usb_print_recent_tx_frames();
    if (fpga.rx_frame_valid) {
        usb_debug_printf("Last RX frame:");
        for (int i = 0; i < FPGA_RX_FRAME_SIZE; i++)
            usb_debug_printf(" %02X", fpga.rx_frame[i]);
        usb_send_str("\r\n");
    }
}


void cmd_flash_jedec(void)
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

/* flash read <addr> <len>: hex dump of up to 256 bytes of the W25Q, 16 per line. */
void cmd_flash_read(const char *args)
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

/* flash dump <addr> <len>: up to 4096 raw W25Q bytes after a "FLASHDUMP <len>"
 * header line, read and sent in 256-byte chunks. */
void cmd_flash_dump(const char *args)
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

/* CAN bus analyser (CH1 = CAN_H, CH2 = CAN_L).
 *   can decode              decode the current record, frame by frame
 *   can stats [n]           n fresh records (default 20): per-ID counts, ACK/NAK/CRC
 *   can screen [on|off]     readout on badge row 2, refreshed 5x per second
 * Best timebase for decoding a 500 kbit/s bus: 0x0A (whole frame + ACK). */

/* The one flag a frame is reported by, in priority order. */
static const char *can_flags_str(uint8_t f)
{
    if (f & DECODE_FLAG_ERROR) return "ERR";
    if (f & DECODE_FLAG_TRUNC) return "TRUNC";
    if (f & DECODE_FLAG_CRC)   return "CRC-FAIL";
    if (f & DECODE_FLAG_ACK)   return "ACK";
    if (f & DECODE_FLAG_NAK)   return "NAK";
    return "?";
}

/* One line per record: generation, timebase and sample rate, rotation,
 * samples per bit, bit rate and the two bus levels (CH1-CH2 counts). */
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

void cmd_can_decode(void)
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

/* Collect n records with distinct generations (default 20) into the stats
 * table, giving up after 1 s per record plus 2 s. Prints the summary line,
 * the totals, one row per ID, and the last record that held a CAN signal. */
void cmd_can_stats(const char *args)
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

void cmd_can_screen(const char *args)
{
    while (args && *args == ' ') args++;
    if (args && *args) {
        if      (strncmp(args, "on",  2) == 0) can_scope_screen_set(true);
        else if (strncmp(args, "off", 3) == 0) can_scope_screen_set(false);
        else { usb_send_str("Usage: can screen [on|off]\r\n"); return; }
    }
    usb_debug_printf("can screen %s\r\n", can_scope_screen_on() ? "ON (badge row 2)" : "OFF");
}

/* meter relays [on|off]: whether entering the meter moves the scope's range
 * relays. With it on, the meter link follows the screen only. */
void cmd_meter_relays(const char *args)
{
    bool on;
    if (shell_onoff_arg(args, &on)) fpga_meter_relays_set(on);
    else if (*args) { usb_send_str("usage: meter relays [on|off]\r\n"); return; }
    usb_debug_printf("meter relays: %s (on = meter entry moves the scope's range relays, the old"
                     " behaviour, and the meter link follows the screen only; applies at the next"
                     " meter entry or function change)\r\n",
                     onoff(fpga_meter_relays_get()));
}

/* meter state: mode and submode with the stock selector words, the meter link
 * (policy, PC11, fast switch and wake counters), the last decoded reading,
 * range fields, the first 12 frame bytes, the continuity beep and the
 * cal-word guard. Read only. */
void cmd_meter_state(void)
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

/* reset settings CONFIRM: "Reset to defaults" on the direct route. The
 * built-in defaults (the property table's) go over the user settings (the
 * saved record) through the same function as the Settings row: the input
 * task runs every setter, so the hardware follows, and writes the record
 * (input_handler.c settings_reset_now -> settings_store_reset_to_defaults).
 * The word CONFIRM is the confirm step. Never touched: calibration of any
 * kind, the gallery, the logo, the firmware cache. */
static void reset_emit(const char *line) { (void)usb_debug_printf("%s\r\n", line); }

void cmd_reset_settings(const char *args)
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

/* settings: the settings store's status (storage bound, boot load result,
 * writes, write failures, changes seen) and the config layer's save counters.
 * Together they tell a write that never reached flash from a load that never
 * came back, and a failed save from one refused because writes are disabled. */
void cmd_settings(void)
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

    /* The store's "write failures" above counts a save refused because writes
     * are disabled at build time the same as a real failure. The config
     * layer's counters (config_persist_stats_t, config.h) separate the two. */
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

/* Reply first, give it time to leave, then reboot into the DFU bootloader. */
void cmd_reboot_bootloader(void)
{
    usb_send_str("Rebooting to bootloader...\r\n");
    usb_delay_ms(20);
    dfu_request_reboot();
}


const char *meter_layout_name(uint8_t layout)
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

/* Exported for the feature shell files (declared in shell_internal.h). */
const char *usb_debug_sh_meter_layout_name(uint8_t layout)
{
    return meter_layout_name(layout);
}

/* persist [on|off]: whether shell commands save the settings they change.
 * Off is for bench sweeps (one settings record per changed command otherwise);
 * RAM only, on at every boot. Key presses save as usual either way. */
void cmd_persist(const char *args)
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

/* batt: the gauge's inputs and outputs in one line. */
void cmd_batt(const char *args)
{
    (void)args;
    usb_debug_printf("batt: raw %u mV, rest %u mV, %u%%, external %u (PC7=%u), critical %u\r\n",
                     (unsigned)battery_raw_mv(), (unsigned)battery_read_mv(), (unsigned)battery_percent(),
                     (unsigned)battery_is_charging(), (unsigned)((GPIOC->idt >> 7) & 1u),
                     (unsigned)battery_is_critical());
}

/* meter auto [start [settle_ms]|status|cancel]: start (default settle 700 ms,
 * at most 3000), query or cancel the background submode autoselect. Every
 * accepted form ends with the status line. */
void cmd_meter_auto_async(const char *args)
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

/* siggen: the signal generator state and the registers behind it.
 * Prints the requested settings, our timer timing next to the stock formula's,
 * then TMR7 (CTRL1/CTRL2/DIV/PR/CNT), DAC CTRL (both halves) and data
 * registers, DMA2 channel 4 and its flex source select, the PA5 mode nibble,
 * and samples of the cycle in the DMA buffer. A GEN-lead -> CH2 capture can
 * then be tied to the programming. The settings are the gen.* properties
 * (set gen.wave=3 gen.amp=1.5 gen.freq=2000 gen.out=on); `siggen dump`
 * prints the whole cycle. */
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

void cmd_siggen(const char *args)
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

/* fwload / fwstat / fwswap / fwapply: firmware image loading over this shell.
 * The state machine and every flash access live in fw_loader.c; these
 * handlers only parse and print. The RX rerouting that feeds the transfer is
 * in vUsbDebugTask (shell.c). */

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

/* Loader state and progress, both W25Q cache slots (size/CRC), and the last
 * install breadcrumb (AT32 backup registers, captured once at boot; it
 * survives a reset while the board stays powered). */
void fwl_print_status(void)
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

/* fwload <size> <crc32 hex> [a|b]: arm the raw-byte intake for an image of
 * <size> bytes into a W25Q cache slot (default b). Once armed, the next bytes
 * from the host go into the image, so every argument is checked first and
 * anything malformed is refused rather than defaulted. */
void cmd_fwload(const char *args)
{
    char *end = NULL;
    unsigned long size = strtoul(args, &end, 10);
    char *end2 = NULL;
    bool crc_bad = false;
    unsigned long crc = 0;
    if (end && *end) {
        crc = strtoul(end, &end2, 16);
        /* strtoul() leaves endptr == nptr when it converted nothing, so this
         * separates "crc32 token is not hex" from "crc32 is 0" and from "no
         * crc32 given", each with its own message. */
        if (end2 == end) crc_bad = true;
    }
    uint8_t slot = 1; /* default: slot b, by convention the other firmware */
    bool slot_bad = false;
    if (end2 != NULL) {
        while (*end2 == ' ') end2++;
        if (*end2 == 'a' || *end2 == 'A') slot = 0;
        else if (*end2 == 'b' || *end2 == 'B') slot = 1;
        else if (*end2 != '\0') slot_bad = true;
        /* The letter must end the argument: `fwload 8192 DEADBEEF apple`
         * is refused, not taken as slot a. */
        if (!slot_bad && *end2 != '\0') {
            const char *tail = end2 + 1;
            while (*tail == ' ') tail++;
            if (*tail != '\0') slot_bad = true;
        }
    }
    /* A slot argument that is present but is neither a nor b is refused, not
     * defaulted: arming the intake would make the next line typed part of
     * the image. scripts/cdc_flash.py only ever sends a or b. */
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

void cmd_fwstat(void)
{
    fwl_print_status();
}

/* fwswap a|b: install an image already cached in a W25Q slot (no transfer).
 * The loader verifies the slot, then erases, programs and verifies MCU flash
 * from RAM and resets into the image. Returns only if the install is refused. */
void cmd_fwswap(const char *args)
{
    uint8_t slot;
    while (*args == ' ') args++;
    /* The token must be exactly one letter (`fwswap apple` is refused): this
     * command erases the application and resets. */
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

/* fwapply: install the staged image (erase, program, verify from RAM) and
 * reset into it. Returns only when the apply is refused. */
void cmd_fwapply(void)
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
