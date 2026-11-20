/*
 * OpenScope 2C53T - reverse-engineering toolkit: who drives the FPGA's
 * configuration port, and with which handshake (FEATURE_RETOOLS)
 *
 *   fpga busrelease     take the MCU off the SPI bus so that an external
 *                       master can try a configuration (tools/re/fpga-config/)
 *   fpga busreacquire   take the bus back
 *   fpga reinit         replay the configuration handshake through the SPI
 *                       peripheral with one parameter changed per run
 * The handlers were moved here unchanged from the bench shell
 * (src/drivers/shell_bench.c). Rows: shell_re.h. Guide: docs/re/README.md.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "at32f403a_407.h"
#include "FreeRTOS.h"
#include "task.h"

#include "usb_debug.h"
#include "fpga.h"
#define SHELL_FEATURE_FILE   /* shell_internal.h: the shell core's helper names */
#include "shell_internal.h"
#include "shell_bench.h"     /* the bench shell's bus helpers */
#include "shell_re.h"
#include "re_fpga_driver.h"

/* Give the SPI3 pins up so that an external SPI master on the test pads can
 * drive the FPGA's configuration port; `fpga busreacquire` takes them back. */
void cmd_fpga_bus_release(void)
{
    fpga_bus_release();
    usb_send_str("SPI3 pins released: PB3 (SCK), PB5 (MOSI), PB6 (CS) are inputs; PB4 (MISO) input with pull-up.\r\n");
    usb_send_str("  The FPGA stays powered (PC6). Take the pins back with `fpga busreacquire`.\r\n");
}

void cmd_fpga_bus_reacquire(void)
{
    fpga_spi3_bus_reacquire();
    usb_send_str("SPI3 bus REACQUIRED by MCU.\r\n");
    usb_send_str("  PB3(SCK)/PB5(MOSI) -> SPI3 AF, PB6(CS) -> GPIO out HIGH.\r\n");
    usb_send_str("  PB4(MISO) input pull-up (stock idle), PC6=HIGH, SPE=1.\r\n");
    usb_send_str("  Config port left untouched. `fpga reinit` now drives it.\r\n");
}

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
    if (fboot.bus_released) {
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
                     fboot.scope_status[0], fboot.scope_status[1],
                     fboot.scope_status[2], fboot.scope_status[3]);

    /* Gowin STATUS_REGISTER (0x41) — the authoritative config status. Decode the
     * named bits so one reinit run tells us WHICH side of the wall we're on:
     *   all-FF  -> FPGA not driving MISO (never entered config-receive)
     *   CRC/IDV -> bytes reached the config engine (wire/content problem)
     *   DONE    -> config actually took. */
    uint32_t sr = fpga_cfg_status();
    usb_debug_printf("0x41 STATUS: %02X %02X %02X %02X (raw=%08lX)\r\n",
                     fboot.cfg_status_reg[0], fboot.cfg_status_reg[1],
                     fboot.cfg_status_reg[2], fboot.cfg_status_reg[3], sr);
    if (sr == 0xFFFFFFFFu || sr == 0x00000000u) {
        usb_debug_printf("  -> %s (no valid status; FPGA not in SSPI config-receive)\r\n",
                         sr == 0xFFFFFFFFu ? "all-FF" : "all-00");
    } else {
        usb_debug_printf("  flags:%s%s%s%s%s%s%s\r\n",
                         (sr & (1u << 0))  ? " CRC_ERR"   : "",
                         (sr & (1u << 1))  ? " BAD_CMD"   : "",
                         (sr & (1u << 2))  ? " ID_FAIL"   : "",
                         (sr & (1u << 12)) ? " GWVLD"     : "",
                         (sr & (1u << GOWIN_STATUS_DONE_BIT)) ? " DONE"      : "",
                         (sr & (1u << 15)) ? " READY"     : "",
                         (sr & (1u << 16)) ? " POR"       : "");
    }
    /* Post-CONFIG_ENABLE STATUS (probe_edit): did 0x15 engage SYSTEM_EDIT_MODE
     * (bit7)? Read at /256 right after 0x15, before the bitstream — the precise
     * wall test (openFPGALoader's enableCfg() polls exactly this bit). */
    if (opt.reload_3c)
        usb_send_str("reinit: sent 0x3C RELOAD before prelude\r\n");
    if (opt.probe_edit) {
        uint32_t es = ((uint32_t)re_fpga.edit_mode_status[0] << 24) |
                      ((uint32_t)re_fpga.edit_mode_status[1] << 16) |
                      ((uint32_t)re_fpga.edit_mode_status[2] << 8) |
                      (uint32_t)re_fpga.edit_mode_status[3];
        usb_debug_printf("post-0x15 STATUS@/256: %02X %02X %02X %02X (raw=%08lX)\r\n",
                         re_fpga.edit_mode_status[0], re_fpga.edit_mode_status[1],
                         re_fpga.edit_mode_status[2], re_fpga.edit_mode_status[3], es);
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
