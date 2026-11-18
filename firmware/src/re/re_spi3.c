/*
 * OpenScope 2C53T - reverse-engineering toolkit: the SPI bus to the FPGA
 * (FEATURE_RETOOLS)
 *
 * What does the FPGA answer on its SPI port, as a configuration port and as
 * a running design?
 *   spi3 probe     does the FPGA drive MISO at all?
 *   spi3 gowin     the configuration port's IDCODE / USERCODE / STATUS
 *   spi3 edgecap   the same frame by the SPI peripheral and by GPIO, for a
 *                  logic analyser to compare
 *   spi3 opsweep   which opcodes of the running design return data?
 * The handlers were moved here unchanged from the bench shell
 * (src/drivers/shell_bench.c), which keeps the helpers its own commands also
 * use (spi3_raw_xfer, spi3_gowin_read_reg, spi3_set_baud, spi3_opread_window).
 * Rows: shell_re.h. Guide: docs/re/README.md.
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

/* SPI3 claim/release is handled by the dispatcher via SC_SPI3. */
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

/* spi3 gowin — read the Gowin SSPI ID / USERCODE / STATUS registers
 * (spi3_gowin_read_reg, shell_bench.c, has the framing) and decode the status
 * bits.
 *
 * Why this matters: our `spi3 xfer 41 ...` returned 0x00000000 on a *running*
 * NV-booted FPGA. A configured GW1N must report DONE_FINAL / GOWIN_VLD / READY
 * in its status register — zero means either our READ FRAMING is wrong (this
 * command tests that by also reading IDCODE, which must equal 0x0120681B for
 * the GW1N-2) or the part is genuinely unconfigured. Either answer unblocks us.
 *
 * Gowin status-register bit names from openFPGALoader src/gowin.cpp. */
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
}

/* Full proven-shape 0x04 read; returns the sample span. Used as the wedge
 * canary between sweep steps. Full 1026-byte window on purpose — do NOT
 * shorten it (frame shape is part of the protocol; see the 0x05 hazard note
 * on fpga_acq_read_channel). Consumes one capture frame. */
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
