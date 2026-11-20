/*
 * OpenScope 2C53T - driver-level functions that only the toolkit calls
 *
 * The configuration sequence on the SPI3 peripheral with its probes, the
 * edge-capture rig and a raw meter frame. Split out of fpga.c; built only
 * with the toolkit (RETOOLS).
 */

#include "../drivers/fpga_internal.h"
#include "re_fpga_driver.h"
#include "../drivers/fpga_bitstream_core.h"

static void fpga_h2_record_body_rx(uint8_t rx_byte)
{
    if (rx_byte == 0x00U) {
        fboot.h2_rx_00_count++;
    } else if (rx_byte == 0xFFU) {
        fboot.h2_rx_ff_count++;
    } else {
        fboot.h2_rx_other_count++;
    }
}

static void fpga_h2_record_close_rx(uint8_t rx_byte)
{
    uint8_t len = fboot.h2_close_rx_len;
    if (len < sizeof(fboot.h2_close_rx)) {
        fboot.h2_close_rx[len] = rx_byte;
        fboot.h2_close_rx_len = (uint8_t)(len + 1U);
    }
}

/* Double-buffered SPI3 pump for the H2 upload (per GitHub issue #11, Lanchon).
 *
 * spi3_xfer()'s order — wait TDBE, write, wait RDBF, read — only queues
 * the NEXT tx byte after the current byte's RX has landed, by which point
 * the shift register has already drained. That underruns the transmitter
 * and clock-stretches the SPI bus between every byte; an interrupt landing
 * in the RDBF busy-wait widens the gap arbitrarily. Over the 115KB H2
 * upload that is tens of thousands of stalls.
 *
 * This pump primes the tx buffer one byte ahead: the moment TDBE frees it
 * writes tx[i], THEN blocks on RDBF for tx[i-1]. The shift register is
 * reloaded back-to-back so the clock runs continuously, and the pump
 * tolerates any interrupt shorter than one byte-time.
 *
 *   tx: bytes to send. n: byte count. Caller manages CS.
 *
 * RX is not returned to the caller — it is folded into the h2 diagnostic
 * counters via fpga_h2_record_body_rx() as it arrives, since the upload has
 * no per-byte reply worth buffering (stock's own upload reads all-FF; see
 * the issue-#18 capture).
 *
 * Timeout-guarded like spi3_xfer so a misconfigured peripheral can't hang
 * the boot; on timeout the byte is treated as 0xFF and we keep going.
 */
static void spi3_pump_h2_record(const uint8_t *tx, uint32_t n)
{
    if (n == 0)
        return;

    volatile uint32_t timeout;

    uint32_t i = 0;

    timeout = 100000;
    while (!(FPGA_SPI->sts & SPI_I2S_TDBE_FLAG)) {
        if (--timeout == 0) break;
    }
    FPGA_SPI->dt = tx[0];

    while (++i < n) {
        timeout = 100000;
        while (!(FPGA_SPI->sts & SPI_I2S_TDBE_FLAG)) {
            if (--timeout == 0) break;
        }
        FPGA_SPI->dt = tx[i];

        timeout = 100000;
        while (!(FPGA_SPI->sts & SPI_I2S_RDBF_FLAG)) {
            if (--timeout == 0) break;
        }
        fpga_h2_record_body_rx((timeout == 0) ? 0xFF : (uint8_t)FPGA_SPI->dt);
    }

    timeout = 100000;
    while (!(FPGA_SPI->sts & SPI_I2S_RDBF_FLAG)) {
        if (--timeout == 0) break;
    }
    fpga_h2_record_body_rx((timeout == 0) ? 0xFF : (uint8_t)FPGA_SPI->dt);
}

/* ═══════════════════════════════════════════════════════════════════
 * SPI3 FPGA config handshake (shared by fpga_init and `fpga reinit`)
 *
 * Sequence is the stock-captured order (issue-#18 Saleae capture):
 *   [PB11 HIGH, 1ms] CS↑00 | CS↓ 05 00 CS↑00 | gap | CS↓ 12 00 CS↑00 | gap
 *   | CS↓ 15 00 CS↑00 | CS↓ 3B <115638-byte bitstream> CS↑00
 *   | CS↓ 3A <close> CS↑00 | CS↓00 CS↑00 | [post_close delay]
 *   | CS↓ 01 08 CS↑ | 02 03 | 06 00 | 07 00 | 08 AD | CS↓ 03 <status×4> CS↑
 * ═══════════════════════════════════════════════════════════════════ */
/* ── Exp J helpers: the anchored opcode-discrimination probe ───────────────
 * Read a Gowin register in the sibling's framing (openFPGALoader
 * read_register32): a bare clock with CS HIGH to frame, then CS LOW, the 4-byte
 * command word <opcode 00 00 00>, then clock the reply out.
 *
 * We clock EIGHT bytes, not four. A Gowin register is 32 bits, so the second
 * four are "extra" — and that is the point: if the FPGA is free-running a fixed
 * 4-byte pattern instead of answering, the read shows it directly as
 * <abcd abcd> rather than leaving us to infer it. Exp I established that every
 * status number this project has recorded was such a pattern; four bytes cannot
 * tell the two cases apart, eight can.
 *
 * CALLER MUST already be at /256 — SSPI reads are garbage at /2 (fpga.c:1564). */
static void spi3_read_reg8(uint8_t opcode, volatile uint8_t *out8)
{
    spi3_xfer(0x00);                      /* bare clock, CS HIGH (frame) */
    SPI3_CS_ASSERT();
    spi3_xfer(opcode);
    spi3_xfer(0x00); spi3_xfer(0x00); spi3_xfer(0x00);
    for (unsigned i = 0; i < 8; i++)
        out8[i] = spi3_xfer(0x00);
    SPI3_CS_DEASSERT();
}

/* Slide a 32-bit window across all 33 bit alignments of the 64-bit reply and
 * look for the known IDCODE 0x0120681B (GW1N-2 family; independently confirmed
 * in the Gowin .fs preamble at file offset 0x4AD19).
 *
 * The bit-offset search is deliberate, not defensive. Every measurement artifact
 * this project has hit — garbage reads at /2, floating MISO, the SWD script's
 * byte rotation — manifests as a PHASE SHIFT of otherwise-correct data. An
 * exact-match test would report "IDCODE absent" when the IDCODE is present but
 * misaligned, which is precisely the mistake that kept 0x8001C810 alive for six
 * weeks. Returns the offset (0..32), or -1 if the IDCODE is genuinely not there. */
static int8_t spi3_find_idcode(const volatile uint8_t *b8)
{
    uint64_t w = 0;
    for (unsigned i = 0; i < 8; i++)
        w = (w << 8) | b8[i];
    for (unsigned s = 0; s <= 32; s++) {
        if ((uint32_t)((w >> (32 - s)) & 0xFFFFFFFFu) == 0x0120681Bu)
            return (int8_t)s;
    }
    return -1;
}

uint8_t fpga_spi3_config_sequence(const fpga_cfg_seq_opts_t *opt)
{
    gpio_init_type gpio_cfg;
    gpio_default_para_init(&gpio_cfg);

    /* -1 = "IDCODE not found". Must be set explicitly: fpga is zero-initialised,
     * and 0 is a VALID bit offset (perfectly aligned), so leaving these at 0
     * would report a successful match on a probe that never ran. */
    fboot.probe_id_bit       = -1;
    fboot.probe_id_bit_close = -1;

    /* Keep the acquisition task off the SPI3 bus during the handshake.
     * No-op pre-RTOS; essential when replayed live via `fpga reinit`. */
    int sched_running = (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING);
    if (sched_running && acq_task_handle) vTaskSuspend(acq_task_handle);

    (void)FPGA_SPI->dt;                  /* Discard any stale RX data */

    /* Optional FPGA reset pulse (rosenrot00's working 2C23T loader does this;
     * our 2C53T sequence lacks it). Configure the chosen pin as push-pull
     * output, drive LOW for reset_low_ms, then HIGH 1ms before the handshake. */
    if (opt->reset_port >= 1 && opt->reset_port <= 5) {
        gpio_type *rport = (gpio_type *)((const gpio_type *[]){
            GPIOA, GPIOB, GPIOC, GPIOD, GPIOE }[opt->reset_port - 1]);
        uint32_t rmask = (1u << opt->reset_pin);
        gpio_cfg.gpio_pins = rmask;
        gpio_cfg.gpio_mode = GPIO_MODE_OUTPUT;
        gpio_cfg.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
        gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
        gpio_init(rport, &gpio_cfg);
        rport->clr = rmask;                 /* RESET LOW */
        fpga_scope_delay_ms(opt->reset_low_ms ? opt->reset_low_ms : 10);
        rport->scr = rmask;                 /* RESET HIGH */
        fpga_scope_delay_ms(1);
    }

    /* Strap-hold (2026-06-13 GPIO-audit lead): drive Port-D pins that stock
     * asserts on scope-mode entry but our firmware never touches, HELD through
     * the entire handshake (prelude→0x3B→0x3A→status). NOT a pulse — a held
     * level, matching stock. PD2 is the prime config-entry-lever candidate.
     * See unmapped_mcu_fpga_pin_candidates.md §4a. */
    if (opt->strap_pd2) {
        gpio_cfg.gpio_pins = (1u << 2);                    /* PD2 */
        gpio_cfg.gpio_mode = GPIO_MODE_OUTPUT;
        gpio_cfg.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
        gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
        gpio_init(GPIOD, &gpio_cfg);
        if (opt->strap_pd2 == 1) GPIOD->scr = (1u << 2);   /* hold HIGH (stock) */
        else                     GPIOD->clr = (1u << 2);   /* hold LOW */
    }
    if (opt->strap_pd1213) {
        gpio_cfg.gpio_pins = (1u << 12) | (1u << 13);      /* PD12, PD13 */
        gpio_cfg.gpio_mode = GPIO_MODE_OUTPUT;
        gpio_cfg.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
        gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
        gpio_init(GPIOD, &gpio_cfg);
        if (opt->strap_pd1213 == 1) GPIOD->scr = (3u << 12);  /* hold HIGH */
        else                        GPIOD->clr = (3u << 12);  /* hold LOW */
    }

    if (opt->arm_pb11) {
        /* PB11 HIGH ~1ms before the CS pulse — stock raises it 1.0ms before
         * the bare CS pulse and holds it HIGH through the upload (capture). */
        gpio_cfg.gpio_pins = GPIO_PINS_11;
        gpio_cfg.gpio_mode = GPIO_MODE_OUTPUT;
        gpio_cfg.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
        gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
        gpio_init(GPIOB, &gpio_cfg);
        GPIOB->scr = PB11_MASK;
        fpga_scope_delay_ms(1);
    }

    /* WIRE-EXACT to the issue-#18 stock capture. The capture clocks NOTHING
     * while CS is high; every byte sits inside a CS-LOW frame. Our previous
     * version clocked flush 0x00 bytes with CS HIGH between frames — 8 stray
     * SCK edges per gap that, if the GW1N SSPI shift register isn't strictly
     * CS-gated, desync the command parser so CONFIG_ENABLE (0x15) never lands.
     * Removed entirely below. See SPI3_STOCK_BOOT_CAPTURE_ANALYSIS.md. */

    /* Command-phase clock. The SSPI read path is clock-limited (IDCODE reads
     * garbage at /2, clean at /256), so the prelude/close/status all run at
     * opt->cmd_br here; only the bulk 0x3B payload switches to opt->upload_br
     * and then returns to cmd_br for the close/status reads. Restored to /2 at
     * function exit.
     *
     * single_br (EXP-34): skip this entirely. spi3_set_br() toggles SPE off/on,
     * and this call fires BEFORE the 05/12/15 prelude — the one pre-0x15 SPE
     * glitch stock never produces (stock sets BR once at SPI3 init and leaves
     * it). Leaving it out runs the whole transaction at the caller's divider
     * (fpga_init leaves /2 = stock's rate) with SPE untouched through 0x15. */
    if (!opt->single_br) spi3_set_br(opt->cmd_br);


    /* [0-] Exp J (probe_idcode): ANCHORED opcode-discrimination probe, run on a
     * pristine bus BEFORE the prelude — the FPGA is running its NV design and we
     * have not yet touched it.
     *
     * This is the first test in the investigation with a known correct answer.
     * Read four opcodes at /256: 0x11 READ_IDCODE (answer: 0x0120681B),
     * 0x13 USERCODE, 0x41 STATUS, and 0x00 no-op as the control.
     *
     *   IDCODE found, and 0x11 != 0x00  => the FPGA DOES decode SSPI opcodes.
     *                                      The 2026-06-13 "not in config-receive
     *                                      mode" conclusion — the one that sent
     *                                      this project toward JTAG — collapses.
     *   all four identical, repeating   => the FPGA free-runs a fixed pattern and
     *                                      ignores MOSI. June's conclusion finally
     *                                      rests on a valid measurement, and the
     *                                      FT232H JTAG route is the answer.
     *
     * Note this ADDS CS frames ahead of the config attempt, so it is off by
     * default; a stock-faithful run must leave probe_idcode = 0. */
    if (opt->probe_idcode) {
        spi3_set_br(7);                      /* /256 — the only valid read clock */
        spi3_read_reg8(0x11, fboot.probe_idcode);
        spi3_read_reg8(0x00, fboot.probe_noop);
        spi3_read_reg8(0x41, fboot.probe_status);
        spi3_read_reg8(0x13, fboot.probe_user);

        fboot.probe_id_bit = spi3_find_idcode(fboot.probe_idcode);

        spi3_set_br(opt->cmd_br);
    }

    /* [0a] DIAGNOSTIC (reload_3c): Gowin SSPI RELOAD (0x3C) — software reconfig
     * trigger, sent at /256 in its own CS frame before the prelude, with a settle
     * delay. Tests whether it knocks the running NV design back toward config
     * (clears GWVLD/FLASH_LOCK so CONFIG_ENABLE can engage SYSTEM_EDIT_MODE). */
    if (opt->reload_3c) {
        spi3_set_br(7);                  /* /256 */
        SPI3_CS_ASSERT();
        spi3_xfer(0x3C);
        spi3_xfer(0x00);
        SPI3_CS_DEASSERT();
        spi3_set_br(opt->cmd_br);
        fpga_scope_delay_ms(50);
    }

    /* [0] bare CS pulse — CS low→high with ZERO clocks (stock t=3.6082).
     * A CS assertion is the SSPI frame-sync that resets the command FSM. */
    SPI3_CS_DEASSERT();
    (void)FPGA_SPI->dt;                  /* drain stale RX without clocking */
    SPI3_CS_ASSERT();
    for (volatile int d = 0; d < 50; d++) { __asm__ volatile("nop"); }
    SPI3_CS_DEASSERT();
    fpga_scope_delay_ms(opt->prelude_gap_ms);   /* stock waits ~100ms → 0x05 */


    /* [1-3] CONFIG_ENABLE prelude — 05 00 / 12 00 / 15 00.
     * Framing per opt->prelude_frame_mode (sweep knob; 0 = stock-faithful):
     *   0 split    : CS↓05 00↑ | CS↓12 00↑ | CS↓15 00↑   (then 3B in its own frame)
     *   1 combined : CS↓05 00 12 00 15 00↑                (then 3B in its own frame)
     *   2 merge    : CS↓05 00↑ | CS↓12 00↑ | CS↓15 00 3B <table>↑  (15 shares upload frame)
     * init_hs[] capture indices are identical across all three. */
    if (opt->prelude_frame_mode == 1) {
        SPI3_CS_ASSERT();
        if (!opt->omit_05) {
            fboot.init_hs[1] = spi3_xfer(0x05);
            fboot.init_hs[2] = spi3_xfer(0x00);
        }
        fboot.init_hs[4] = spi3_xfer(0x12);
        fboot.init_hs[5] = spi3_xfer(0x00);
        fboot.init_hs[7] = spi3_xfer(0x15);
        fboot.init_hs[8] = spi3_xfer(0x00);
        SPI3_CS_DEASSERT();
    } else {
        /* [1] 05 00 — skipped entirely when omit_05 is set. init_hs[1]/[2] stay
         * at their zero-init value, which is how the bench tells the frame was
         * not sent (a real 0x05 frame records the MISO bytes here). */
        if (!opt->omit_05) {
            SPI3_CS_ASSERT();
            fboot.init_hs[1] = spi3_xfer(0x05);
            fboot.init_hs[2] = spi3_xfer(0x00);
            SPI3_CS_DEASSERT();
        }
        fpga_scope_delay_ms(opt->prelude_gap_ms);

        /* [1b] Build A (2026-08-13): the maksidze/Stlkv V0.4 "richer prelude"
         * reads, positioned HERE — between ERASE_SRAM and INIT_ADDR, immediately
         * before CONFIG_ENABLE, matching the order Stlkv's working cold-start
         * loader uses (0x11 → 0x13 → 0x41 → 0x12 → 0x15). Our probe_idcode reads
         * (if also on) sit far earlier, before the bare CS pulse; this tests
         * whether reads *adjacent* to config-enable are the load-bearing bit.
         * Forced to /256 (SSPI reads are garbage at /2); restores opt->cmd_br. */
        if (opt->prelude_reads) {
            spi3_set_br(7);
            spi3_read_reg8(0x11, fboot.probe_idcode);
            spi3_read_reg8(0x13, fboot.probe_user);
            spi3_read_reg8(0x41, fboot.probe_status);
            fboot.probe_id_bit = spi3_find_idcode(fboot.probe_idcode);
            spi3_set_br(opt->cmd_br);
        }

        /* [2] 12 00 */
        SPI3_CS_ASSERT();
        fboot.init_hs[4] = spi3_xfer(0x12);
        fboot.init_hs[5] = spi3_xfer(0x00);
        SPI3_CS_DEASSERT();
        fpga_scope_delay_ms(opt->prelude_gap_ms);

        /* [3] 15 00 — own frame (mode 0) or held LOW into the upload (mode 2) */
        SPI3_CS_ASSERT();
        fboot.init_hs[7] = spi3_xfer(0x15);
        fboot.init_hs[8] = spi3_xfer(0x00);
        if (opt->prelude_frame_mode != 2) {
            SPI3_CS_DEASSERT();
        }
    }

    /* [3b] DIAGNOSTIC (probe_edit): read STATUS(0x41) at /256 IMMEDIATELY after
     * CONFIG_ENABLE — does 0x15 engage SYSTEM_EDIT_MODE (bit7)? Reads are only
     * valid at slow clock. Skipped in merge mode (CS held LOW into the upload).
     * Default off so the real attempt stays byte-unchanged. */
    if (opt->probe_edit && opt->prelude_frame_mode != 2) {
        spi3_set_br(7);                  /* /256 — valid SSPI read clock */
        spi3_xfer(0x00);                 /* bare clock, CS HIGH (frame) */
        SPI3_CS_ASSERT();
        spi3_xfer(0x41);
        spi3_xfer(0x00); spi3_xfer(0x00); spi3_xfer(0x00);
        for (unsigned i = 0; i < 4; i++)
            fboot.edit_mode_status[i] = spi3_xfer(0x00);
        SPI3_CS_DEASSERT();
        spi3_set_br(opt->cmd_br);        /* restore command clock */
    }

    /* [3c] Exp J, second half: read IDCODE again AFTER CONFIG_ENABLE. The
     * pre-prelude probe asks "does the FPGA answer at all?"; this asks "did 0x15
     * change that?" A part that ignores 0x11 before but answers it after would
     * mean CONFIG_ENABLE is landing and only the upload is broken — the opposite
     * of the current working theory, and worth one CS frame to rule out. */
    if (opt->probe_idcode && opt->prelude_frame_mode != 2) {
        spi3_set_br(7);                  /* /256 */
        spi3_read_reg8(0x11, fboot.probe_idcode_post);
        spi3_set_br(opt->cmd_br);
    }

    /* Optional digest gap between CONFIG_ENABLE and the data stream (stock ~8µs
     * → default 0). Skipped in merge mode, where CS stays LOW into the upload. */
    if (opt->prelude_frame_mode != 2)
        fpga_scope_delay_ms(opt->pre_upload_gap_ms);

    /* [4] bitstream upload — 0x3B + full table. mode 2 continues the CS frame
     * opened by 15 00; modes 0/1 open a fresh CS frame here. */
    if (opt->prelude_frame_mode != 2) SPI3_CS_ASSERT();
    fboot.init_hs[10] = spi3_xfer(0x3B);  /* open upload */
    if (!opt->single_br) spi3_set_br(opt->upload_br);  /* EXP-34: keep SPE untouched */
    fboot.h2_rx_00_count = 0;
    fboot.h2_rx_ff_count = 0;
    fboot.h2_rx_other_count = 0;
    fboot.h2_close_rx_len = 0;
    memset((void *)fboot.h2_close_rx, 0, sizeof(fboot.h2_close_rx));
    spi3_pump_h2_record(fpga_bitstream_core, FPGA_BITSTREAM_CORE_SIZE);
    if (!opt->single_br) spi3_set_br(opt->cmd_br);  /* EXP-34: keep SPE untouched */
    /* Trailing clocks: Gowin runs the CRC-check / DONE / wakeup on CCLK cycles
     * AFTER the last config byte. Our sequence sent none; rosenrot00's working
     * 2C23T SPI loader clocks ~200 dummy 0x00 here. Stay inside the upload CS
     * frame. Default 0 = stock-faithful; sweep via `fpga reinit tcN`. */
    for (uint32_t i = 0; i < opt->trailing_clocks; i++)
        spi3_xfer(0x00);
    SPI3_CS_DEASSERT();
    fboot.h2_bytes_sent = FPGA_BITSTREAM_CORE_SIZE;
    fboot.h2_upload_done = 1;
    fboot.upload_len = FPGA_BITSTREAM_CORE_SIZE;
    fboot.upload_result = FPGA_UPLOAD_SENT;

    /* [5] 3A 00 — close/commit in its own CS-LOW frame. Stock → 0xF8. */
    SPI3_CS_ASSERT();
    fpga_h2_record_close_rx(spi3_xfer(0x3A));
    fboot.h2_close_status = spi3_xfer(0x00);
    fpga_h2_record_close_rx(fboot.h2_close_status);
    SPI3_CS_DEASSERT();

    /* [6] single 0x00 byte, CS LOW (stock flush frame at t=4.4484). */
    SPI3_CS_ASSERT();
    fpga_h2_record_close_rx(spi3_xfer(0x00));
    SPI3_CS_DEASSERT();

    /* [6b] Gowin STATUS_REGISTER read (opcode 0x41) — the authoritative config
     * status, which our sequence never read. Framed like rosenrot00's working
     * read_register32(0x41000000): a bare dummy byte (CS HIGH), then CS LOW,
     * opcode 0x41 + 3 pad bytes, then clock 4 bytes back. Decoded by the shell.
     * All-0xFF = FPGA not driving MISO (never entered config-receive) → config-
     * entry wall; CRC_ERROR/ID_VERIFY_FAILED set = bytes reached the engine →
     * wire/content problem. See sibling_loader_config_diff.md.
     *
     * CLOCK: forced to /256 here, NOT opt->cmd_br. SSPI reads are only valid at
     * a slow clock (fpga.c:1564); at /2 the MISO data arrives after the sampling
     * edge and we latch the PREVIOUS bit. That is not hypothetical — it is what
     * this very read has been doing since it was added. Every historical value
     * of this register ("stable 80 01 C8 10 across repeats", decoded as
     * "READY POR" in sibling_loader_config_diff.md:85-86) was taken at /2, and
     * 0x8001C810 is bit-for-bit 0x00039020 sampled one bit early:
     *
     *   0x00039020 = 0000 0000 0000 0011 1001 0000 0010 0000
     *   prepend the trailing 1 of the preceding word, shift right one:
     *              = 1000 0000 0000 0001 1100 1000 0001 0000 = 0x8001C810
     *
     * 0x8001C810 sets bits 4/11/31, which are not defined bits in the Gowin map
     * at all — the giveaway that it was never a real register value. Same bug
     * family as the /2 status reads and the floating-MISO reads. */
    spi3_set_br(7);                           /* /256 — the only valid SSPI read clock */

    /* ANCHOR FIRST. Read the IDCODE in the same window and at the same clock as
     * the status below, so cfg_status_reg is only believed on a read path proven
     * against a known answer (0x0120681B). This runs on every build, not just the
     * probe build: an unanchored status read is precisely what produced
     * "80 01 C8 10 = READY POR" and kept it alive for six weeks.
     *
     * It doubles as the configured/not-configured test. Exp L (2026-07-28): once
     * stock has configured the FPGA, the SSPI config port CLOSES and 0x11 returns
     * zeros, because the port then belongs to the user design carrying ADC data.
     * So an IDCODE that still answers HERE — after our full 115,638-byte upload
     * and the 0x3A close — means the config port never closed and we are
     * definitively not configured. */
    spi3_read_reg8(0x11, fboot.probe_idcode_close);
    fboot.probe_id_bit_close = spi3_find_idcode(fboot.probe_idcode_close);

    spi3_xfer(0x00);                          /* bare clock, CS HIGH (frame) */
    SPI3_CS_ASSERT();
    spi3_xfer(0x41);
    spi3_xfer(0x00); spi3_xfer(0x00); spi3_xfer(0x00);
    for (unsigned i = 0; i < 4; i++)
        fboot.cfg_status_reg[i] = spi3_xfer(0x00);
    SPI3_CS_DEASSERT();
    spi3_set_br(opt->cmd_br);                 /* restore the command clock */

    /* Step 7c: post-upload scope config (5 register writes + status read). */
    fpga_scope_delay_ms(opt->post_close_ms);

    static const uint8_t scope_cfg[][2] = {
        { FPGA_CORE_OP_TIMEBASE, 0x08 }, { FPGA_CORE_OP_LEGACY02, 0x03 }, { FPGA_CORE_OP_SOURCE, 0x00 },
        { FPGA_CORE_OP_EDGE, FPGA_CORE_EDGE_EITHER }, { FPGA_CORE_OP_LEVEL, 0xAD },
    };
    for (unsigned i = 0; i < sizeof(scope_cfg) / sizeof(scope_cfg[0]); i++) {
        SPI3_CS_ASSERT();
        spi3_xfer(scope_cfg[i][0]);
        spi3_xfer(scope_cfg[i][1]);
        SPI3_CS_DEASSERT();
    }

    SPI3_CS_ASSERT();
    spi3_xfer(FPGA_CORE_OP_LIVE);                          /* status read */
    for (unsigned i = 0; i < 4; i++)
        fboot.scope_status[i] = spi3_xfer(0xFF);
    SPI3_CS_DEASSERT();
    acq_edge_reg = 0x00;                 /* the burst wrote 07 00 */
    acq_design_ours = false;             /* this path does not vouch for the design */
    acq_core_ok_set(false);

    fpga_reconcile_timebase_after_arm();

    spi3_set_br(0);                      /* restore /2 (60MHz) for normal acq */

    if (sched_running && acq_task_handle) vTaskResume(acq_task_handle);

    return fboot.h2_close_status;
}

/* EXP-37 (2026-08-26) — LA edge-capture rig. Emits the SAME logical CONFIG_ENABLE
 * frame (CS-high dummy 0x00, CS-low 0x15 0x00) N times over AF hardware-SPI3 at
 * /256, then a gap, then N times over GPIO bit-bang at ~470kHz. Same pins, same
 * rate, same frame — drive type (AF vs GPIO push-pull) is the ONLY variable.
 * Arm sigrok (D0=SCK/D1=MISO/D2=MOSI/D3=CS) and diff the two bursts.
 *
 * NOTE: this fires ISOLATED 0x15 frames — no 05/12 prelude — so it does NOT
 * configure anything; it exists purely to photograph the SCK/MOSI/CS waveform of
 * a 0x15 frame under each drive. `reps` frames per group, ~2ms apart; 60ms gap
 * between the AF and GPIO bursts as a marker.
 *
 * Bit-bang half only exists under FPGA_CONFIG_B (BB_* pin masks, bb_cmd16), so
 * the rig is built only for those images — `make guest-bringup-bb`. */
void fpga_edgecap_15(uint8_t reps)
{
    gpio_init_type gpio_cfg;
    gpio_default_para_init(&gpio_cfg);
    if (reps == 0) reps = 16;

    if (fboot.bus_released) fpga_spi3_bus_reacquire();

    /* --- Group 1: AF-driven 0x15 frames at /256 (~470kHz, decodable) --- */
    spi3_set_br(7);
    for (uint8_t i = 0; i < reps; i++) {
        (void)spi3_xfer(0x00);          /* CS-HIGH dummy (mirrors bb_cmd16) */
        SPI3_CS_ASSERT();
        (void)spi3_xfer(0x15);
        (void)spi3_xfer(0x00);
        SPI3_CS_DEASSERT();
        fpga_scope_delay_ms(2);
    }

    fpga_scope_delay_ms(60);            /* gap marker between the two bursts */

    /* --- Switch PB3(SCK)/PB5(MOSI)/PB6(CS) to GPIO push-pull, PB4 floating in,
     *     mode-3 idle (SCK/CS high, MOSI low) — exactly the bit-bang posture. */
    FPGA_SPI->ctrl1 &= ~(1u << 6);      /* SPE off: peripheral releases the pins */
    GPIOB->scr = BB_SCK | BB_CS;
    GPIOB->clr = BB_MOSI;
    gpio_cfg.gpio_pins = BB_SCK | BB_MOSI | BB_CS;
    gpio_cfg.gpio_mode = GPIO_MODE_OUTPUT;
    gpio_cfg.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(GPIOB, &gpio_cfg);
    gpio_cfg.gpio_pins = BB_MISO;
    gpio_cfg.gpio_mode = GPIO_MODE_INPUT;
    gpio_cfg.gpio_pull = GPIO_PULL_NONE;
    gpio_init(GPIOB, &gpio_cfg);

    /* --- Group 2: GPIO bit-bang 0x15 frames (rate = FPGA_BB_HALF_DELAY) --- */
    for (uint8_t i = 0; i < reps; i++) {
        bb_cmd16(0x15, 0x00);           /* CS-HIGH dummy + CS↓ 15 00 CS↑ */
        fpga_scope_delay_ms(2);
    }

    /* Restore AF bus so later shell commands work. */
    fpga_spi3_bus_reacquire();
}

void fpga_send_raw_frame(const uint8_t *frame)
{
    (void)meter_tx_raw_frame(frame);
}
