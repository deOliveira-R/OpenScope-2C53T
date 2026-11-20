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

    /* PB3 (SCK), PB5 (MOSI), PB6 (CS) become GPIO push-pull outputs, PB4
     * (MISO) a floating input. The idle levels (SCK HIGH, CS HIGH, MOSI LOW:
     * mode-3 idle) are loaded first, so the pins do not glitch. */
    GPIOB->scr = BB_SCK | BB_CS;
    GPIOB->clr = BB_MOSI;
    gpio_cfg.gpio_pins = BB_SCK | BB_MOSI | BB_CS;
    gpio_cfg.gpio_mode = GPIO_MODE_OUTPUT;
    gpio_cfg.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(GPIOB, &gpio_cfg);
    gpio_cfg.gpio_pins = BB_MISO;
    gpio_cfg.gpio_mode = GPIO_MODE_INPUT;
    gpio_cfg.gpio_pull = GPIO_PULL_NONE;   /* floating */
    gpio_init(GPIOB, &gpio_cfg);

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
    /* PB3 and PB5 back to the SPI3 alternate function, so the acquisition
     * task reads records over hardware SPI3. PB4 stays a floating input and
     * PB6 a GPIO (software CS). */
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

    /* fpga_bus_release() also drives PB11 high. PB11 is a CH2 range relay
     * line, so put its level back afterwards. */
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
 * SPI3 bus release and re-acquisition
 * ═══════════════════════════════════════════════════════════════════
 *
 * fpga_bus_release() lets go of the lines the MCU drives (PB3 SCK, PB5 MOSI,
 * PB6 CS): SPI3 off, all three floating inputs (Hi-Z). Two uses: fpga_reload()
 * before it switches the FPGA's supply off (no back-feed through the lines),
 * and the toolkit's `fpga busrelease`, which hands the bus to an external SPI
 * master on the SPI3 test pads. PB4 (MISO) is driven by the FPGA and is
 * already an input, so it is left alone. The MCU keeps PC6 HIGH (FPGA
 * supply) and also drives PB11 HIGH.
 *
 * While fpga.bus_released is set, fpga_acq_task stays off the bus.
 * fpga_spi3_bus_reacquire() takes the bus back. */
void fpga_bus_release(void)
{
    gpio_init_type gpio_cfg;

    /* Set the flag first so the acquisition task stops touching SPI3. */
    fpga.bus_released = true;
    fpga.spi3_active = false;

    /* Disable the SPI3 peripheral so it stops driving SCK/MOSI. */
    FPGA_SPI->ctrl1 &= ~(1u << 6);   /* SPE = 0 */

    /* PB3 (SCK), PB5 (MOSI), PB6 (CS) to floating inputs (Hi-Z). */
    gpio_default_para_init(&gpio_cfg);
    gpio_cfg.gpio_mode = GPIO_MODE_INPUT;
    gpio_cfg.gpio_pull = GPIO_PULL_NONE;
    gpio_cfg.gpio_pins = GPIO_PINS_3 | GPIO_PINS_5 | GPIO_PINS_6;
    gpio_init(GPIOB, &gpio_cfg);

    /* PB11 and PC6 as push-pull outputs, both HIGH. */
    gpio_default_para_init(&gpio_cfg);
    gpio_cfg.gpio_mode = GPIO_MODE_OUTPUT;
    gpio_cfg.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_cfg.gpio_pins = GPIO_PINS_11;          /* PB11 = CH2 range relay bit 1 */
    gpio_init(GPIOB, &gpio_cfg);
    GPIOB->scr = PB11_MASK;                      /* PB11 HIGH */

    gpio_cfg.gpio_pins = GPIO_PINS_6;            /* PC6 = FPGA supply enable */
    gpio_init(GPIOC, &gpio_cfg);
    GPIOC->scr = PC6_MASK;                       /* PC6 HIGH */
}

/* Take the SPI3 bus back after fpga_bus_release(): the same pin and
 * peripheral setup as fpga_init(), then clear fpga.bus_released. It always
 * reconfigures, whether or not the bus was released. */
void fpga_spi3_bus_reacquire(void)
{
    gpio_init_type gpio_cfg;
    gpio_default_para_init(&gpio_cfg);

    crm_periph_clock_enable(CRM_SPI3_PERIPH_CLOCK, TRUE);

    /* PB3 = SPI3_SCK: AF push-pull, the same drive setting as stock. */
    gpio_cfg.gpio_pins = GPIO_PINS_3;
    gpio_cfg.gpio_mode = GPIO_MODE_MUX;
    gpio_cfg.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(GPIOB, &gpio_cfg);

    /* PB4 = SPI3_MISO: input with pull-up, as stock, so an undriven line
     * reads 0xFF. */
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

    /* PC6 = FPGA supply enable, HIGH (fpga_bus_release left it HIGH). */
    gpio_cfg.gpio_pins = GPIO_PINS_6;
    gpio_cfg.gpio_mode = GPIO_MODE_OUTPUT;
    gpio_cfg.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(GPIOC, &gpio_cfg);
    GPIOC->scr = PC6_MASK;

    /* SPI3 CTRL1, CTRL2 and enable, as in fpga_init (mode 3, master, /2, software CS). */
    FPGA_SPI->ctrl1 = (1 << 0) | (1 << 1) | (1 << 2) | (1 << 8) | (1 << 9);
    FPGA_SPI->ctrl2 = 0x03;
    FPGA_SPI->ctrl1 |= (1 << 6);   /* SPE = 1 */

    fpga.bus_released = false;
    fpga.spi3_active  = true;
}
