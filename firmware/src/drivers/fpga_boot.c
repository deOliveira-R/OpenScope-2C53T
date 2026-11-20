/*
 * OpenScope 2C53T - FPGA design upload
 *
 * The design upload: the FPGA configuration sequence bit-banged on the SPI3
 * pins, the boot choice between the compiled-in design and the flash slot,
 * runtime reload, and the release and re-acquisition of the SPI3 bus.
 * Shared items: fpga_internal.h.
 */

#include "fpga_internal.h"
#include "fpga_bitstream_core.h"

#ifndef FPGA_BB_HALF_DELAY

#define FPGA_BB_HALF_DELAY 0   /* nops per SCK half-period; 0 = as fast as GPIO writes go */

#endif

/* Optional slowdown of the bit-bang clock. `make guest-coldtrace-la` sets
 * FPGA_BB_HALF_DELAY=60, about 1-2 MHz SCK, so a 24 MS/s logic analyzer can
 * resolve it; the upload then takes about 1 s longer. */
static inline void bb_half_delay(void)
{
#if FPGA_BB_HALF_DELAY > 0
    for (volatile int d = 0; d < FPGA_BB_HALF_DELAY; d++) { __asm__ volatile("nop"); }
#endif
}

/* One mode-3 byte, MSB first: SCK falls, MOSI is driven, SCK rises, MISO is
 * sampled. Returns the byte read. */
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

/* Options of the next fpga_bitbang_config_sequence() call, set by its callers
 * (fpga_boot_upload_once, fpga_reload) and cleared after it. By default it
 * sends the compiled-in design. A caller can give another payload source
 * (the flash slot), and can ask it to send nothing unless the FPGA first
 * answers its IDCODE on the configuration port, which means it is empty. */
static fpga_payload_fn bb_src_fn;        /* NULL: the compiled-in design */

static void           *bb_src_ctx;

static uint32_t        bb_src_len;

static bool            bb_require_empty;

static uint8_t         bb_outcome;       /* 0 ok, 1 not empty (nothing sent), 2 source failed mid-upload */

static bool            bb_arm_live;      /* outcome 1: still run the arm writes (a design is running and stays) */

/* Upload a design over the configuration port, then send the core's control
 * writes and identify the core. Suspends the acquisition task while it runs.
 * Returns the last byte of the post-upload STATUS, or 0 when it stopped
 * before the control writes; the outcome is left in bb_outcome. */
uint8_t fpga_bitbang_config_sequence(void)
{
    gpio_init_type gpio_cfg;
    gpio_default_para_init(&gpio_cfg);

    fpga.probe_id_bit       = -1;
    fpga.probe_id_bit_close = -1;
    fpga.diag_spi_ctrl1     = 0xBBBBu;   /* not a register value: tells `status` the bit-bang path ran */

    int sched_running = (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING);
    if (sched_running && acq_task_handle) vTaskSuspend(acq_task_handle);

    /* The pins become GPIO for the configuration port. */
    spi3_pins_gpio();

    /* Read IDCODE, USERCODE and STATUS before the upload (shown by `status`). */
    bb_read_reg32(0x11, (uint8_t *)fpga.probe_idcode);
    bb_read_reg32(0x13, (uint8_t *)fpga.probe_user);
    bb_read_reg32(0x41, (uint8_t *)fpga.probe_status);
    /* 0x0120681B is the FPGA's IDCODE: only an empty (unconfigured) part
     * answers it on this port. */
    if (fpga.probe_idcode[0] == 0x01 && fpga.probe_idcode[1] == 0x20 &&
        fpga.probe_idcode[2] == 0x68 && fpga.probe_idcode[3] == 0x1B)
        fpga.probe_id_bit = 0;

    /* A part that does not answer its IDCODE still runs a design, and these
     * pins are that design's own SPI port. An upload would only feed it about
     * 115 KB of stray bytes, so send nothing when asked to require empty. */
    bb_outcome = 0;
    if (bb_require_empty && fpga.probe_id_bit != 0) {
        bb_outcome = 1;
        goto bb_restore_pins;
    }

    bb_cmd16(0x12, 0x00);   /* INIT_ADDR */
    bb_cmd16(0x15, 0x00);   /* CONFIG_ENABLE */

    /* 0x3B and the whole design in one CS-low frame. */
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

    /* STATUS after the upload; bit 13 is DONE (the design started). */
    bb_read_reg32(0x41, (uint8_t *)fpga.cfg_status_reg);
    /* IDCODE again: a configured part stops answering, so a mismatch here is
     * the expected result of a good upload. */
    bb_read_reg32(0x11, (uint8_t *)fpga.probe_idcode_close);
    if (fpga.probe_idcode_close[0] == 0x01 && fpga.probe_idcode_close[1] == 0x20 &&
        fpga.probe_idcode_close[2] == 0x68 && fpga.probe_idcode_close[3] == 0x1B)
        fpga.probe_id_bit_close = 0;

    bb_cmd16(0x3A, 0x00);   /* CONFIG_DISABLE */
    fpga_scope_delay_ms(100);

bb_restore_pins:
    /* The pins go back to SPI3, so the acquisition task reads records over
     * the peripheral. */
    spi3_pins_peripheral();

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

    /* Arm writes: after a 600 ms wait (stock's gap after its upload), five
     * register writes to the core (timebase, reg 0x02, trigger source CH1,
     * edge, trigger level 0xAD), then a read of the live status (0x03).
     * Over hardware SPI3 at /256; on a configured part these pins are the
     * design's own control port. The timebase is put back right after; the
     * trigger level and source are put back by main() at boot and by
     * fpga_reload() at runtime. */
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
        /* The burst wrote 07 00 (edge: either direction). The edge register
         * may be used from here on only on our own core: this upload must
         * have been ours and must have ended with DONE (a design that stayed
         * is unknown). */
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
 * Reload the FPGA at runtime.
 *
 * PC6 switches the FPGA's supply: after 1 s low the part comes back empty and
 * answers its IDCODE on the configuration port, and the bit-bang upload loads
 * a design as at boot. Sequence (measured on unit #2, 2026-11-15): SPI lines
 * released (Hi-Z), PC6 low 1 s, PC6 high, lines back, upload.
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

    fpga_bus_release();                  /* no line into the FPGA is driven while its supply is off */
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
    /* The arm writes left reg 0x08 at 0xAD and reg 0x06 at CH1; the timebase
     * was already put back. Put the trigger level and source back from the
     * scope state too (at boot main() does this after the settings are
     * restored), or the core triggers at a level and on a channel the UI does
     * not show. */
    if (sched_running) {
        (void)fpga_apply_trigger_level(scope_state_get()->trigger.level);
        (void)fpga_apply_trigger_source();
    }
    return FPGA_RELOAD_OK;
}

/*
 * Boot upload. Normally the compiled-in design. When the flash slot holds a
 * valid design with its boot flag set (`fpga slot boot on`) and its payload
 * CRC holds, that design is uploaded instead, so a development design
 * survives a power cycle. The flash region layer is not bound this early in
 * boot, so the slot is read through the raw flash primitive at the region's
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

/* Name of the slot design uploaded at boot, or NULL for the compiled-in one. */
const char *fpga_boot_design(void)
{
    return fpga_boot_from_slot ? fpga_boot_slot_name : NULL;
}

/* ═══════════════════════════════════════════════════════════════════
 * The SPI3 pins: PB3 SCK, PB4 MISO, PB5 MOSI, PB6 chip select
 *
 * They have three states, and the three functions below are the only code
 * that sets them. PB4 is an input with pull-up in all three, so a line the
 * FPGA does not drive reads 0xFF. No other pin is touched here: PC6 (the
 * FPGA's supply) belongs to fpga_init and fpga_reload, PB11 to the front end.
 * ═══════════════════════════════════════════════════════════════════ */

static void spi3_pin_miso_input(void)
{
    gpio_init_type g;
    gpio_default_para_init(&g);
    g.gpio_pins = BB_MISO;
    g.gpio_mode = GPIO_MODE_INPUT;
    g.gpio_pull = GPIO_PULL_UP;
    gpio_init(GPIOB, &g);
}

/* The peripheral drives the bus: PB3 and PB5 alternate function, PB6 a GPIO
 * output (software chip select, idle high), SPI3 master in mode 3 at /2
 * (60 MHz from the 120 MHz APB1), 8 bits, MSB first. */
void spi3_pins_peripheral(void)
{
    gpio_init_type g;
    gpio_default_para_init(&g);

    crm_periph_clock_enable(CRM_SPI3_PERIPH_CLOCK, TRUE);

    g.gpio_pins = BB_SCK | BB_MOSI;
    g.gpio_mode = GPIO_MODE_MUX;
    g.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    g.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(GPIOB, &g);
    spi3_pin_miso_input();
    SPI3_CS_DEASSERT();
    g.gpio_pins = BB_CS;
    g.gpio_mode = GPIO_MODE_OUTPUT;
    gpio_init(GPIOB, &g);

    FPGA_SPI->ctrl1 = SPI3_CTRL1_MODE3_MASTER;
    FPGA_SPI->ctrl2 = 0;
    FPGA_SPI->ctrl1 |= SPI3_CTRL1_ENABLE;
}

/* The MCU drives the bus as GPIO, for the FPGA's configuration port: PB3,
 * PB5 and PB6 push-pull outputs. The idle levels of mode 3 (SCK high, chip
 * select high, MOSI low) are loaded first, so the pins do not glitch. */
void spi3_pins_gpio(void)
{
    gpio_init_type g;
    gpio_default_para_init(&g);

    GPIOB->scr = BB_SCK | BB_CS;
    GPIOB->clr = BB_MOSI;
    g.gpio_pins = BB_SCK | BB_MOSI | BB_CS;
    g.gpio_mode = GPIO_MODE_OUTPUT;
    g.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    g.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(GPIOB, &g);
    spi3_pin_miso_input();
}

/* Nothing drives the bus from the MCU: SPI3 off, PB3, PB5 and PB6 floating
 * inputs. */
void spi3_pins_released(void)
{
    gpio_init_type g;
    gpio_default_para_init(&g);

    FPGA_SPI->ctrl1 &= ~SPI3_CTRL1_ENABLE;
    g.gpio_pins = BB_SCK | BB_MOSI | BB_CS;
    g.gpio_mode = GPIO_MODE_INPUT;
    g.gpio_pull = GPIO_PULL_NONE;
    gpio_init(GPIOB, &g);
    spi3_pin_miso_input();
}

/* Give the bus up and take it back. Two users: fpga_reload(), so that no
 * line feeds the FPGA while its supply is off, and the toolkit's
 * `fpga busrelease`, for an external SPI master on the test pads. While
 * fpga.bus_released is set, the acquisition task stays off the bus. */
void fpga_bus_release(void)
{
    fpga.bus_released = true;    /* first, so the acquisition task stops using SPI3 */
    spi3_pins_released();
}

void fpga_spi3_bus_reacquire(void)
{
    spi3_pins_peripheral();
    fpga.bus_released = false;
}
