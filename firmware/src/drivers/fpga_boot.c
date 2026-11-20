/*
 * OpenScope 2C53T - FPGA design upload
 *
 * The configuration sequence on the SPI3 pins as GPIO, the design slot,
 * reload, and the release of the bus. Split out of fpga.c; shared items:
 * fpga_internal.h.
 */

#include "fpga_internal.h"
#include "fpga_bitstream_core.h"

/* One mode-3 byte: CLK falls, drive MOSI, CLK rises, sample MISO. MSB first.
 * No explicit delay — the GPIO write latency IS the slow/gapped clock that
 * distinguishes this from hardware SPI3 (the whole point of the A/B). */
#ifndef FPGA_BB_HALF_DELAY

#define FPGA_BB_HALF_DELAY 0   /* nops per SCK half-period; 0 = as fast as GPIO writes go */

#endif

/* Logic-analyzer builds (`make guest-coldtrace-la`, issue #18 2026-08-15):
 * maksidze's Saleae tops out at 24 MS/s and the undelayed loop clocks faster
 * than that resolves (his 1508.zip bit-bang captures came back aliased).
 * FPGA_BB_HALF_DELAY=60 gives a ~1-2 MHz SCK — trivially resolvable — at the
 * cost of ~1 s extra config time. Config entry is not timing-sensitive
 * (Exp B2 / the whole bit-bang-vs-AF story), so this should not change the
 * outcome; if it does, that is itself a finding. */
static inline void bb_half_delay(void)
{
#if FPGA_BB_HALF_DELAY > 0
    for (volatile int d = 0; d < FPGA_BB_HALF_DELAY; d++) { __asm__ volatile("nop"); }
#endif
}

static uint8_t bb_xfer(uint8_t value)
{
    uint8_t result = 0;
    for (uint8_t i = 0; i < 8u; ++i) {
        GPIOB->clr = BB_SCK;
        if (value & 0x80u) GPIOB->scr = BB_MOSI;
        else               GPIOB->clr = BB_MOSI;
        bb_half_delay();
        GPIOB->scr = BB_SCK;
        bb_half_delay();
        result = (uint8_t)(result << 1);
        value  = (uint8_t)(value << 1);
        if (GPIOB->idt & BB_MISO) result |= 1u;
    }
    return result;
}

/* 32-bit register read: dummy byte (CS high), CS low, opcode + 3 pad, read 4,
 * CS high. Stores the 4 bytes MSB-first into out4 (out4[0] = first byte). */
static void bb_read_reg32(uint8_t opcode, uint8_t *out4)
{
    (void)bb_xfer(0);
    GPIOB->clr = BB_CS;
    (void)bb_xfer(opcode);
    (void)bb_xfer(0);
    (void)bb_xfer(0);
    (void)bb_xfer(0);
    for (uint8_t i = 0; i < 4u; ++i) out4[i] = bb_xfer(0);
    GPIOB->scr = BB_CS;
}

/* 2-byte command: dummy byte (CS high), CS low, 2 bytes, CS high. */
void bb_cmd16(uint8_t hi, uint8_t lo)
{
    (void)bb_xfer(0);
    GPIOB->clr = BB_CS;
    (void)bb_xfer(hi);
    (void)bb_xfer(lo);
    GPIOB->scr = BB_CS;
}

/* fpga_reload() support (2026-11-15). The bit-bang upload below normally sends
 * the compiled-in design, exactly as before. fpga_reload() can hand it another
 * payload source (the flash slot) and ask it to send NOTHING unless the FPGA
 * first answers its IDCODE on the configuration port, i.e. is really empty.
 * At boot all three are zero/false and the path is the unchanged one. */
static fpga_payload_fn bb_src_fn;        /* NULL: the compiled-in design */

static void           *bb_src_ctx;

static uint32_t        bb_src_len;

static bool            bb_require_empty;

static uint8_t         bb_outcome;       /* 0 ok, 1 not empty (nothing sent), 2 source failed mid-upload */

static bool            bb_arm_live;      /* outcome 1: still run the arm writes (a design is running and stays) */

uint8_t fpga_bitbang_config_sequence(void)
{
    gpio_init_type gpio_cfg;
    gpio_default_para_init(&gpio_cfg);

    fpga.probe_id_bit       = -1;
    fpga.probe_id_bit_close = -1;
    fpga.diag_spi_ctrl1     = 0xBBBBu;   /* S1 marker: this was the bit-bang build */

    int sched_running = (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING);
    if (sched_running && acq_task_handle) vTaskSuspend(acq_task_handle);

    /* Switch PB3(SCK)/PB5(MOSI)/PB6(CS) to GPIO push-pull outputs, PB4(MISO)
     * to floating input — exactly Stlkv's setup. Pre-load idle levels first
     * (CLK HIGH, CS HIGH, MOSI LOW = mode-3 idle) to avoid a config glitch. */
    GPIOB->scr = BB_SCK | BB_CS;
    GPIOB->clr = BB_MOSI;
    gpio_cfg.gpio_pins = BB_SCK | BB_MOSI | BB_CS;
    gpio_cfg.gpio_mode = GPIO_MODE_OUTPUT;
    gpio_cfg.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(GPIOB, &gpio_cfg);
    gpio_cfg.gpio_pins = BB_MISO;
    gpio_cfg.gpio_mode = GPIO_MODE_INPUT;
    gpio_cfg.gpio_pull = GPIO_PULL_NONE;   /* floating, as Stlkv's transplant */
    gpio_init(GPIOB, &gpio_cfg);

    /* Prelude reads (populate the anchor + overlay). */
    bb_read_reg32(0x11, (uint8_t *)fpga.probe_idcode);
    bb_read_reg32(0x13, (uint8_t *)fpga.probe_user);
    bb_read_reg32(0x41, (uint8_t *)fpga.probe_status);
    /* Cleanly-framed 4-byte read: IDCODE is aligned at bit 0 when it matches. */
    if (fpga.probe_idcode[0] == 0x01 && fpga.probe_idcode[1] == 0x20 &&
        fpga.probe_idcode[2] == 0x68 && fpga.probe_idcode[3] == 0x1B)
        fpga.probe_id_bit = 0;

    /* fpga_reload(): a part that does not answer its IDCODE here still runs a
     * design (its SSPI pins are user I/O). Sending the upload would only feed
     * 115 KB of stray bytes to that design's own SPI port: send nothing. */
    bb_outcome = 0;
    if (bb_require_empty && fpga.probe_id_bit != 0) {
        bb_outcome = 1;
        goto bb_restore_pins;
    }

    bb_cmd16(0x12, 0x00);   /* INIT_ADDR */
    bb_cmd16(0x15, 0x00);   /* CONFIG_ENABLE */

    /* 0x3B + full payload in one CS-LOW frame. */
    (void)bb_xfer(0);
    GPIOB->clr = BB_CS;
    (void)bb_xfer(0x3B);
    if (bb_src_fn == NULL) {
        for (uint32_t i = 0; i < FPGA_BITSTREAM_CORE_SIZE; ++i)
            (void)bb_xfer(fpga_bitstream_core[i]);
        fpga.h2_bytes_sent = FPGA_BITSTREAM_CORE_SIZE;
    } else {
        /* Payload from a source (the flash slot), 256 bytes at a time. The
         * clock simply stops between chunks; the frame stays open (CS low). */
        uint8_t  chunk[256];
        uint32_t off = 0;
        while (off < bb_src_len) {
            uint32_t n = bb_src_len - off;
            if (n > sizeof chunk) n = sizeof chunk;
            if (!bb_src_fn(bb_src_ctx, off, chunk, n)) { bb_outcome = 2; break; }
            for (uint32_t i = 0; i < n; ++i) (void)bb_xfer(chunk[i]);
            off += n;
        }
        fpga.h2_bytes_sent = off;
    }
    GPIOB->scr = BB_CS;
    fpga.h2_upload_done = (bb_outcome == 0) ? 1 : 0;

    /* Post-upload STATUS — this drives CFG + the D (DONE_FINAL) overlay flag. */
    bb_read_reg32(0x41, (uint8_t *)fpga.cfg_status_reg);
    /* Post-config IDCODE anchor (Exp L: a configured part stops answering, so
     * an all-zero / mismatched read here is the SUCCESS signature). */
    bb_read_reg32(0x11, (uint8_t *)fpga.probe_idcode_close);
    if (fpga.probe_idcode_close[0] == 0x01 && fpga.probe_idcode_close[1] == 0x20 &&
        fpga.probe_idcode_close[2] == 0x68 && fpga.probe_idcode_close[3] == 0x1B)
        fpga.probe_id_bit_close = 0;

    bb_cmd16(0x3A, 0x00);   /* CONFIG_DISABLE */
    fpga_scope_delay_ms(100);

bb_restore_pins:
    /* Restore PB3/4/5 to SPI3 AF so the acquisition task can read 0x04/0x05
     * over hardware SPI3 if config took. PB6 stays GPIO (software CS). */
    gpio_cfg.gpio_pins = BB_SCK | BB_MOSI;
    gpio_cfg.gpio_mode = GPIO_MODE_MUX;
    gpio_cfg.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(GPIOB, &gpio_cfg);
    gpio_cfg.gpio_pins = BB_MISO;
    gpio_cfg.gpio_mode = GPIO_MODE_INPUT;
    gpio_cfg.gpio_pull = GPIO_PULL_NONE;
    gpio_init(GPIOB, &gpio_cfg);
    SPI3_CS_DEASSERT();

    /* fpga_reload() refused or lost its source: no design was loaded, so there
     * is nothing to arm. The boot path asks for the arm writes even so when a
     * running design could not be removed (bb_arm_live): that design stays. */
    if (bb_outcome != 0 && !(bb_outcome == 1 && bb_arm_live)) {
        if (bb_outcome == 2) {               /* the FPGA is empty now */
            acq_design_ours = false;
            acq_core_ok_set(false);
            acq_edge_reg = 0x00;
        }
        if (sched_running && acq_task_handle) vTaskResume(acq_task_handle);
        return 0;
    }

    /* Engine-arm attempt (bench plan item 4) — Build B breaks the config wall
     * (DONE_FINAL set) but the capture engine starts UNARMED (one buffer then
     * halt). Replicate stock's post-config shape: ~600 ms gap, then the five
     * runtime control writes, then a 0x03 status read. Per the #18 netlist
     * answer the arm bit is committed by one of these addresses (candidate
     * 0x08→0xAD); watch SS byte 1 for 01 = armed, and the demo trace for
     * disappearance. Runs over hardware SPI3 (AF restored just above): a
     * configured part's SSPI pins are now the user design's runtime control SPI.
     * SSPI reads are valid only at a slow clock, so read 0x03 at /256. */
    fpga_scope_delay_ms(600);
    spi3_set_br(7);                      /* /256 */
    {
        static const uint8_t arm_cfg[][2] = {
            { FPGA_CORE_OP_TIMEBASE, 0x08 }, { FPGA_CORE_OP_LEGACY02, 0x03 }, { FPGA_CORE_OP_SOURCE, 0x00 },
            { FPGA_CORE_OP_EDGE, FPGA_CORE_EDGE_EITHER }, { FPGA_CORE_OP_LEVEL, 0xAD },
        };
        for (unsigned i = 0; i < 5; i++) {
            SPI3_CS_ASSERT();
            spi3_xfer(arm_cfg[i][0]);
            spi3_xfer(arm_cfg[i][1]);
            SPI3_CS_DEASSERT();
            fpga_scope_delay_ms(2);
        }
        SPI3_CS_ASSERT();
        spi3_xfer(FPGA_CORE_OP_LIVE);
        for (unsigned i = 0; i < 4; i++)
            fpga.scope_status[i] = spi3_xfer(0xFF);
        SPI3_CS_DEASSERT();
        /* The burst wrote 07 00. Whether the edge register may be used from
         * here on: only on our own core, and only when this upload was ours
         * and ended with DONE (a design that stayed is unknown). */
        acq_edge_reg = 0x00;
        acq_design_ours = trig_edge_reg_loaded_by_us(bb_outcome == 0,
                                                    (fpga.cfg_status_reg[2] & 0x20u) != 0u);
        /* Ask our core which build it is (op E2; the E-range is never sent to
         * a design that is not ours). A core older than D5 answers zeros. */
        for (unsigned i = 0; i < CORE_IDENT_REPLY_LEN; i++) fpga.core_ident[i] = 0u;
        if (acq_design_ours) {
            SPI3_CS_ASSERT();
            fpga.core_ident[0] = spi3_xfer(FPGA_CORE_OP_IDENT);
            for (unsigned i = 1; i < CORE_IDENT_REPLY_LEN; i++)
                fpga.core_ident[i] = spi3_xfer(0xFF);
            SPI3_CS_DEASSERT();
        }
        {   /* only our core, identified and complete, is acquired from */
            uint8_t r[CORE_IDENT_REPLY_LEN];
            for (unsigned i = 0; i < CORE_IDENT_REPLY_LEN; i++) r[i] = fpga.core_ident[i];
            acq_core_ok_set(core_ident_usable(acq_design_ours, r));
        }
    }
    fpga_reconcile_timebase_after_arm();
    spi3_set_br(0);                      /* restore /2 for normal acquisition */

    if (sched_running && acq_task_handle) vTaskResume(acq_task_handle);
    return fpga.cfg_status_reg[3];
}

/*
 * Reload the FPGA at runtime (bench 2026-11-15, unit #2).
 *
 * PC6 switches the FPGA's supply: low for a second, the part comes back empty
 * and answers its IDCODE on the configuration port; the bit-bang upload then
 * loads a design as at boot. Proven sequence: SPI lines released (Hi-Z), PC6
 * low 1 s, PC6 high, lines back, upload.
 *
 * A design whose inputs have their clamp diodes on (the stock design; the
 * vendor tool's default) keeps the FPGA alive through the ADC outputs with PC6
 * low. Such a part never answers the IDCODE, so nothing is sent and the status
 * says so: only a power cycle of the unit replaces that design.
 *
 * fn == NULL reloads the compiled-in design. The caller validates its source
 * BEFORE calling (a bad slot must never leave the FPGA empty).
 */
fpga_reload_status_t fpga_reload(fpga_payload_fn fn, void *ctx, uint32_t len,
                                 fpga_reload_report_t *rep)
{
    if (fn != NULL && len == 0u) return FPGA_RELOAD_BAD_ARG;

    int sched_running = (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING);
    if (sched_running && acq_task_handle) vTaskSuspend(acq_task_handle);

    /* fpga_bus_release() also drives PB11 high (an old guess at its role). On
     * this firmware PB11 is a CH2 front-end line: put its level back after. */
    const bool pb11_was_high = (GPIOB->odt & PB11_MASK) != 0u;
    fpga_bus_release();                  /* PB3/PB5/PB6 Hi-Z, SPI3 off, acq gated */
    if (!pb11_was_high) GPIOB->clr = PB11_MASK;
    GPIOC->clr = PC6_MASK;               /* FPGA supply off */
    fpga_scope_delay_ms(FPGA_RELOAD_OFF_MS);
    GPIOC->scr = PC6_MASK;               /* and on again */
    fpga_scope_delay_ms(FPGA_RELOAD_ON_MS);
    fpga_spi3_bus_reacquire();

    bb_src_fn = fn; bb_src_ctx = ctx; bb_src_len = len;
    bb_require_empty = true;
    (void)fpga_bitbang_config_sequence();    /* resumes the acquisition task itself */
    const uint8_t outcome = bb_outcome;
    bb_src_fn = NULL; bb_src_ctx = NULL; bb_src_len = 0;
    bb_require_empty = false;
    bb_outcome = 0;

    const uint32_t status = ((uint32_t)fpga.cfg_status_reg[0] << 24) | ((uint32_t)fpga.cfg_status_reg[1] << 16) |
                            ((uint32_t)fpga.cfg_status_reg[2] << 8)  |  (uint32_t)fpga.cfg_status_reg[3];
    if (rep) {
        rep->idcode_before = ((uint32_t)fpga.probe_idcode[0] << 24) | ((uint32_t)fpga.probe_idcode[1] << 16) |
                             ((uint32_t)fpga.probe_idcode[2] << 8)  |  (uint32_t)fpga.probe_idcode[3];
        rep->status = (outcome == 1) ? 0u : status;
        rep->sent   = (outcome == 1) ? 0u : fpga.h2_bytes_sent;
    }
    if (outcome == 1) return FPGA_RELOAD_NOT_EMPTY;
    if (outcome == 2) return FPGA_RELOAD_SRC_FAIL;
    if (!(status & (1u << 13))) return FPGA_RELOAD_NOT_DONE;
    /* The arm burst left reg 0x08 at 0xAD and reg 0x06 at CH1; the timebase
     * was put back above. Hand the trigger level and source back to the scope
     * state too (at boot main() does this after the settings are restored),
     * or the design fires at a level and on a channel the UI does not show. */
    if (sched_running) {
        (void)fpga_apply_trigger_level(scope_state_get()->trigger.level);
        (void)fpga_apply_trigger_source();
    }
    return FPGA_RELOAD_OK;
}

/*
 * Boot upload. Normally the compiled-in design. When the flash slot holds a
 * VALID design with its boot flag set (`fpga slot boot on`) and its payload
 * CRC holds, that design is uploaded instead: a development design then
 * survives a power cycle of the unit. The region layer is not bound this early
 * in boot, so the slot is READ through the raw flash primitive at the region's
 * address (reads only). Anything wrong with the slot: the compiled-in design.
 */
static uint8_t  fpga_boot_from_slot;

static char     fpga_boot_slot_name[FSLOT_NAME_LEN + 1];

static bool boot_slot_payload(void *ctx, uint32_t off, uint8_t *buf, uint32_t n)
{
    (void)ctx;
    return flash_fs_raw_read_bytes(flash_region_table[FSLOT_REGION].start + FSLOT_DATA_OFFSET + off,
                                   buf, n) == FLASH_FS_OK;
}

/* What the boot found: 0 = an empty FPGA (cold boot), 1 = a running design that
 * PC6 removed, 2 = a running design that stayed (nothing was uploaded). */
static uint8_t  fpga_boot_live_state;

/* One upload attempt of the boot design (slot if allowed and good, else the
 * compiled-in one), sending nothing unless the FPGA answers its IDCODE.
 * Returns the sequence's outcome (0 sent, 1 not empty). */
static uint8_t fpga_boot_upload_once(bool slot_ok, const fslot_info_t *info, bool arm_live)
{
    uint8_t outcome;

    fpga_boot_from_slot = 0;
    bb_require_empty = true;
    bb_arm_live = arm_live;
    if (slot_ok) {
        bb_src_fn = boot_slot_payload; bb_src_ctx = NULL; bb_src_len = info->len;
        (void)fpga_bitbang_config_sequence();
        outcome = bb_outcome;
        bb_src_fn = NULL; bb_src_len = 0; bb_outcome = 0;
        if (outcome == 0) {
            fpga_boot_from_slot = 1;
            memcpy(fpga_boot_slot_name, info->name, sizeof fpga_boot_slot_name);
        }
        if (outcome != 2) goto done;
        /* the flash read failed mid-upload: the FPGA is still empty */
    }
    (void)fpga_bitbang_config_sequence();
    outcome = bb_outcome;
    bb_outcome = 0;
done:
    bb_require_empty = false;
    bb_arm_live = false;
    return outcome;
}

void fpga_boot_upload(void)
{
    uint8_t      hdr[FSLOT_HDR_LEN];
    fslot_info_t info;
    bool         slot_ok = false;

    fpga_boot_live_state = 0;
    /* Only in images that carry the bench shell: `fpga slot boot off` lives
     * there, and the flag is on the W25Q, where a reflash does not clear it. An
     * image without the shell must never be steered by a flag it cannot reset. */
#if defined(FEATURE_BENCHSHELL) && FEATURE_BENCHSHELL
    const bool slot_allowed = true;
#else
    const bool slot_allowed = false;
#endif
    if (slot_allowed &&
        flash_fs_raw_read_bytes(flash_region_table[FSLOT_REGION].start, hdr, sizeof hdr) == FLASH_FS_OK) {
        fslot_classify_header(hdr, &info);
        if (info.state == FSLOT_VALID && info.boot) {
            uint8_t  buf[256];
            uint32_t c = CAL_BACKUP_CRC32_INIT;
            bool     ok = true;
            for (uint32_t off = 0; ok && off < info.len; off += sizeof buf) {
                uint32_t n = info.len - off;
                if (n > sizeof buf) n = sizeof buf;
                ok = boot_slot_payload(NULL, off, buf, n);
                if (ok) c = cal_backup_crc32_update(c, buf, n);
            }
            slot_ok = ok && cal_backup_crc32_final(c) == info.crc;
        }
    }

    /* An MCU reset that kept the unit powered (a firmware flash) finds the FPGA
     * still running its design. The upload would then be 115 KB of stray bytes
     * into that design's own SPI port. So: nothing is sent unless the FPGA
     * answers its IDCODE. If it does not, switch its supply off through PC6
     * (SPI lines released, as fpga_reload does) and try again. A design that
     * survives that (input clamps on: the stock design) stays and is armed. */
    if (fpga_boot_upload_once(slot_ok, &info, false) == 1) {
        gpio_init_type g;
        gpio_default_para_init(&g);
        g.gpio_pins = BB_SCK | BB_MOSI | BB_CS;
        g.gpio_mode = GPIO_MODE_INPUT;
        g.gpio_pull = GPIO_PULL_NONE;
        gpio_init(GPIOB, &g);            /* Hi-Z: no back-feed through the SPI lines */
        GPIOC->clr = PC6_MASK;           /* FPGA supply off */
        fpga_scope_delay_ms(FPGA_RELOAD_OFF_MS);
        GPIOC->scr = PC6_MASK;
        fpga_scope_delay_ms(FPGA_RELOAD_ON_MS);
        fpga_boot_live_state = (fpga_boot_upload_once(slot_ok, &info, true) == 1) ? 2 : 1;
    }
}

uint8_t fpga_boot_live(void)
{
    return fpga_boot_live_state;
}

const char *fpga_boot_design(void)
{
    return fpga_boot_from_slot ? fpga_boot_slot_name : NULL;
}

/* ═══════════════════════════════════════════════════════════════════
 * SPI3 bus release — hand the bus to an external SSPI master (ESP32)
 * ═══════════════════════════════════════════════════════════════════
 *
 * EXPERIMENTAL — UNTESTED ON HARDWARE (2026-06-14). See the experimental/
 * esp32-bringup branch README (tools/esp32_sspi_bringup/).
 *
 * Purpose: make the MCU let go of the SPI3 lines (PB3 SCK, PB5 MOSI, PB6 CS)
 * so an external 3.3 V SPI master soldered to the back-side SPI3 test pads
 * (maksidze's #18 pad map) can drive the Gowin SSPI config interface itself —
 * at a controlled slow clock — WITHOUT bus contention, and without the
 * build → flash → pinhole-reset loop. The MCU keeps the board alive (PC9
 * power hold is untouched) and stages the FPGA enables, then yields the bus.
 *
 * Why this lets the ESP32 win the bus:
 *   - SPI3 is point-to-point (one master). Two masters driving SCK/MOSI/CS at
 *     once = contention = garbage. So we MUST tri-state our driven lines.
 *   - PB4 (MISO) is FPGA→MCU; it's already an input — the ESP32 also only
 *     reads it, so no contention there. Left as-is.
 *   - PB3 (SCK) and PB5 (MOSI) were AF push-pull outputs; PB6 (CS) was a GPIO
 *     output. All three become floating inputs (true Hi-Z) here.
 *
 * Staging held for the FPGA (not on the ESP32's 4 pads, so the MCU owns them):
 *   - PC6 = HIGH  (FPGA SPI enable)
 *   - PB11 = HIGH (FPGA active mode)
 *
 * The acquisition task checks fpga.bus_released and stays off the bus while
 * this is set (see fpga_acquisition_task). Re-flash to undo (no un-release
 * command on purpose — the bench operator power-cycles or reflashes). */
void fpga_bus_release(void)
{
    gpio_init_type gpio_cfg;

    /* Latch the flag first so the acq task bails before re-touching SPI3. */
    fpga.bus_released = true;
    fpga.spi3_active = false;

    /* Disable the SPI3 peripheral so it stops driving SCK/MOSI. */
    FPGA_SPI->ctrl1 &= ~(1u << 6);   /* SPE = 0 */

    /* Tri-state the MCU-driven SPI3 lines: PB3 (SCK), PB5 (MOSI), PB6 (CS).
     * Floating input = Hi-Z, so the ESP32 owns these nets. */
    gpio_default_para_init(&gpio_cfg);
    gpio_cfg.gpio_mode = GPIO_MODE_INPUT;
    gpio_cfg.gpio_pull = GPIO_PULL_NONE;
    gpio_cfg.gpio_pins = GPIO_PINS_3 | GPIO_PINS_5 | GPIO_PINS_6;
    gpio_init(GPIOB, &gpio_cfg);

    /* PB4 (MISO) is already a floating input — leave it; both we and the
     * ESP32 only ever read it, the FPGA drives it. */

    /* Stage the FPGA enables the MCU still owns (not on the ESP32 pads). */
    gpio_default_para_init(&gpio_cfg);
    gpio_cfg.gpio_mode = GPIO_MODE_OUTPUT;
    gpio_cfg.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_cfg.gpio_pins = GPIO_PINS_11;          /* PB11 = active mode */
    gpio_init(GPIOB, &gpio_cfg);
    GPIOB->scr = PB11_MASK;                      /* PB11 HIGH */

    gpio_cfg.gpio_pins = GPIO_PINS_6;            /* PC6 = SPI enable */
    gpio_init(GPIOC, &gpio_cfg);
    GPIOC->scr = PC6_MASK;                       /* PC6 HIGH */
}

/* Re-acquire the SPI3 bus the MCU released in fpga_bus_release() — a faithful
 * mirror of the fpga_init() SPI3 pin+peripheral block (fpga.c ~4740-4966), so a
 * guest-bringup boot (config port left pristine, no acq flooding, CDC alive) can
 * hand the bus back and let `fpga reinit` drive the failing hardware-SPI prelude
 * on demand for an LA capture (H7). No-op if the bus was never released. */
void fpga_spi3_bus_reacquire(void)
{
    gpio_init_type gpio_cfg;
    gpio_default_para_init(&gpio_cfg);

    crm_periph_clock_enable(CRM_SPI3_PERIPH_CLOCK, TRUE);

    /* PB3 = SPI3_SCK: AF push-pull, 10MHz slew (matches stock, H6). */
    gpio_cfg.gpio_pins = GPIO_PINS_3;
    gpio_cfg.gpio_mode = GPIO_MODE_MUX;
    gpio_cfg.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(GPIOB, &gpio_cfg);

    /* PB4 = SPI3_MISO: input PULL-UP — stock's defined idle level (Exp E), so an
     * undriven line reads a clean 0xFF instead of floating noise on the capture. */
    gpio_cfg.gpio_pins = GPIO_PINS_4;
    gpio_cfg.gpio_mode = GPIO_MODE_INPUT;
    gpio_cfg.gpio_pull = GPIO_PULL_UP;
    gpio_init(GPIOB, &gpio_cfg);
    gpio_cfg.gpio_pull = GPIO_PULL_NONE;   /* restore shared struct default */

    /* PB5 = SPI3_MOSI: AF push-pull. */
    gpio_cfg.gpio_pins = GPIO_PINS_5;
    gpio_cfg.gpio_mode = GPIO_MODE_MUX;
    gpio_cfg.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(GPIOB, &gpio_cfg);

    /* PB6 = SPI3_CS: GPIO output push-pull, idle HIGH. */
    gpio_cfg.gpio_pins = GPIO_PINS_6;
    gpio_cfg.gpio_mode = GPIO_MODE_OUTPUT;
    gpio_cfg.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(GPIOB, &gpio_cfg);
    SPI3_CS_DEASSERT();

    /* PC6 = FPGA SPI enable HIGH (bus_release already staged it, re-assert). */
    gpio_cfg.gpio_pins = GPIO_PINS_6;
    gpio_cfg.gpio_mode = GPIO_MODE_OUTPUT;
    gpio_cfg.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(GPIOC, &gpio_cfg);
    GPIOC->scr = PC6_MASK;

    /* SPI3 CTRL1/CTRL2/SPE — identical to fpga_init (mode 3, master, /2, SW CS). */
    FPGA_SPI->ctrl1 = (1 << 0) | (1 << 1) | (1 << 2) | (1 << 8) | (1 << 9);
    FPGA_SPI->ctrl2 = 0x03;
    FPGA_SPI->ctrl1 |= (1 << 6);   /* SPE = 1 */

    fpga.bus_released = false;
    fpga.spi3_active  = true;
}
