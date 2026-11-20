/*
 * OpenScope 2C53T - FPGA Communication Driver
 *
 * The FPGA runs our own capture core, loaded at every boot; the meter chip
 * is a separate part on USART2.
 *
 * fpga_init(), in order:
 *   1. remap to free PB3/PB4/PB5 from JTAG
 *   2. USART2 set to 9600 baud and left disabled (the meter link enables it)
 *   3. SPI3 init and enable (mode 3, /2 = 60 MHz), then PC6 high
 *   4. the design is uploaded on the same pins as GPIO (fpga_boot_upload),
 *      then the core is identified and its registers are written
 *   5. PC0 (record ready) on EXINT0, the two trigger references, the
 *      front-end relays and coupling
 *
 * Tasks (fpga_create_tasks):
 *   - fpga_acq_task:        reads records from the core over SPI3
 *   - fpga_usart_tx_task:   sends 10-byte command frames to the meter chip
 *   - fpga_usart_rx_task:   handles the meter chip's frames
 *   - fpga_meter_poll_task: meter function changes and the keepalive
 *
 * The parts: fpga_boot.c (design upload), fpga_acq.c (records from the
 * capture core), scope_frontend.c (relays and coupling), meter_link.c (the
 * meter chip on USART2); shared items: fpga_internal.h.
 */

#include "fpga_internal.h"

/* ═══════════════════════════════════════════════════════════════════
 * Global State
 * ═══════════════════════════════════════════════════════════════════ */

fpga_state_t fpga;

/* ═══════════════════════════════════════════════════════════════════
 * SPI3 IRQ Handler (stub)
 *
 * Stock firmware enables SPI3 IRQ #51 (NVIC_ISER1 bit 19).
 * We use polled SPI3, but enable the interrupt to match stock config.
 * This stub just clears any pending flags to prevent IRQ storms.
 * Compliance audit (2026-04-06): added to match stock init.
 * ═══════════════════════════════════════════════════════════════════ */

void SPI3_I2S3EXT_IRQHandler(void)
{
    /* Read STS and DR to clear any pending RXNE/TXE/OVR flags */
    volatile uint32_t sts = FPGA_SPI->sts;
    volatile uint32_t dr  = FPGA_SPI->dt;
    (void)sts;
    (void)dr;
}

/* ═══════════════════════════════════════════════════════════════════
 * Initialization
 * ═══════════════════════════════════════════════════════════════════ */

void fpga_init(void)
{
    /* Clear state */
    memset(&fpga, 0, sizeof(fpga));

    /* ---------------------------------------------------------------
     * Step 1: AFIO remap — disable JTAG-DP, keep SW-DP
     * This frees PB3/PB4/PB5 for SPI3 use.
     * Stock firmware: AFIO_PCF0 = (AFIO_PCF0 & ~0xF000) | 0x2000
     * AT32 equivalent: IOMUX_REMAP6 SWJ_JTAG remap
     * --------------------------------------------------------------- */
    /* Enable IOMUX clock (should already be enabled from main.c) */
    crm_periph_clock_enable(CRM_IOMUX_PERIPH_CLOCK, TRUE);

    /* Disable JTAG and configure SPI3 pin mapping.
     *
     * AT32F403A has TWO remap systems:
     *   1. STM32-compatible: IOMUX->remap (offset 0x04) — same as GD32 AFIO_PCF0
     *      - Bits [26:24] swjtag_mux: 010 = disable JTAG, keep SWD
     *      - Bit [28] spi3_mux: 0 = PB3/PB4/PB5 (DEFAULT), 1 = PC10/PC11/PC12
     *   2. Extended GMUX: IOMUX->remap5/remap7 — AT32-specific
     *
     * The stock GD32 firmware uses system 1: writes AFIO_PCF0 bits [26:24]=010
     * and leaves bit 28=0 (SPI3 on PB3/PB4/PB5 by default). We must use the
     * same compatible register so both remap systems agree.
     */
    /* Read-modify-write the compatible remap register:
     * - Set bits [26:24] = 010 (JTAG off, SWD on)
     * - Clear bit [28] = 0 (SPI3 on PB3/PB4/PB5)
     * Stock firmware: (reg & ~0xF000) | 0x2000 at AFIO+0x08 per CLAUDE.md,
     * but the actual SWJ_CFG is at bits [26:24] of offset 0x04. */
    /* AT32F403A pin remapping: keep legacy SWJ_CFG and SWJTAG_GMUX aligned.
     *
     * The legacy SWJ_CFG in IOMUX->remap (offset 0x04) defaults to 000
     * (full JTAG enabled) on reset. PB3=JTDO, PB4=NJTRST in that state.
     * The GMUX SWJTAG in remap7 (offset 0x30) is a SEPARATE register.
     * Both must be set to free PB3/PB4 for SPI3 use.
     *
     * Legacy: SWJ_CFG bits [26:24] = 010 → JTAG off, SWD on
     *         Do NOT touch bit 28 (SPI3_MUX), stock leaves SPI3 on PB3/PB4/PB5.
     * GMUX:  SWJTAG = 010 only; leave SPI3_GMUX cleared like stock/scopediag.
     */
    /* Legacy JTAG disable — write-only bits, only modify SWJ_CFG [26:24] */
    {
        uint32_t remap = IOMUX->remap;
        remap &= ~(0x7u << 24);   /* Clear SWJ_CFG bits */
        remap |= (0x2u << 24);    /* Set SWJ_CFG = 010 (JTAG off, SWD on) */
        IOMUX->remap = remap;
    }

    /*
     * GMUX remap (AT32-specific).
     *
     * Stock V1.2.0 proves only the JTAG/SWD remap plus PB3/PB4/PB5 GPIO
     * configuration. It does not write SPI3_GMUX/remap5. B1 fix intentionally
     * keeps SPI3_GMUX_0010 set TRUE to preserve the default AT32 pin route.
     * This does not prove DMM calibration/range semantics by itself; it is a
     * routing-level correction only.
     */
    gpio_pin_remap_config(SWJTAG_GMUX_010, TRUE);
    gpio_pin_remap_config(SPI3_GMUX_0010, TRUE);

    /* ---------------------------------------------------------------
     * Step 2: USART2 init — 9600 baud, 8N1, TX+RX with interrupts
     * --------------------------------------------------------------- */
    crm_periph_clock_enable(CRM_USART2_PERIPH_CLOCK, TRUE);

    /* PA2 = USART2_TX: AF push-pull, 50MHz */
    gpio_init_type gpio_cfg;
    gpio_default_para_init(&gpio_cfg);
    gpio_cfg.gpio_pins = GPIO_PINS_2;
    gpio_cfg.gpio_mode = GPIO_MODE_MUX;
    gpio_cfg.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(GPIOA, &gpio_cfg);

    /* PA3 = USART2_RX: Input floating */
    gpio_cfg.gpio_pins = GPIO_PINS_3;
    gpio_cfg.gpio_mode = GPIO_MODE_INPUT;
    gpio_cfg.gpio_pull = GPIO_PULL_NONE;
    gpio_init(GPIOA, &gpio_cfg);

    /* USART2 config: 9600 baud, 8N1 */
    USART2->baudr = system_core_clock / 2 / FPGA_USART_BAUD;  /* APB1 = HCLK/2 */
    USART2->ctrl1 = 0;
    /* Experiment: leave USART2 fully DISABLED (UEN clear) like stock's scope boot.
     * No RE/TE/RDBFIEN/UEN, no NVIC — zero USART2 traffic on PA2/PA3. */
    (void)0;

    /* ---------------------------------------------------------------
     * Step 3: a short settle before SPI3 is set up. PC6/PB11/PC11 were
     * driven at the very top of main(). The 50 ms dates from the builds
     * that took over an FPGA the factory firmware had loaded; whether the
     * upload in Step 6 still needs it has not been measured.
     * --------------------------------------------------------------- */
    systick_delay_ms(50);

    /* ---------------------------------------------------------------
     * Step 4: SPI3 peripheral init — Mode 3, Master, /2 prescaler
     *
     * AT32 SPI4 peripheral is at 0x40003C00 (same address as GD32 SPI3).
     * The AT32 HAL calls this SPI4, but we use direct register access
     * to match the stock firmware exactly.
     * --------------------------------------------------------------- */
    /* Enable SPI3 clock (bit 15 of APB1EN).
     * BUG FIX: was CRM_SPI4_PERIPH_CLOCK (bit 16) — wrong peripheral!
     * On AT32F403A, SPI3 (0x40003C00) and SPI4 (0x40004000) are separate.
     * PB3/PB4/PB5 map to SPI3, not SPI4. */
    crm_periph_clock_enable(CRM_SPI3_PERIPH_CLOCK, TRUE);

    /* PB3 = SPI3_SCK: AF push-pull. GPIO_DRIVE_STRENGTH_STRONGER (=0x01) yields
     * CRL nibble 0x9 = MODE[1:0]=01 = 10 MHz slew — matches stock's PB3 exactly
     * (Exp E/R SWD dump; H6, 2026-08-25). NOT 50 MHz as an earlier comment said. */
    gpio_cfg.gpio_pins = GPIO_PINS_3;
    gpio_cfg.gpio_mode = GPIO_MODE_MUX;
    gpio_cfg.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(GPIOB, &gpio_cfg);

    /* PB4 = SPI3_MISO.
     * Exp E (2026-07-28) measured stock as input PULL-UP here (GPIOB CRL pin4
     * nibble 8 = CNF 10, with ODR bit4 set) while ours was input FLOATING
     * (nibble 4 = CNF 01). Stock's pull-up is exactly why an undriven MISO
     * reads 0xFF in the issue-#18 capture; a floating input has no defined
     * idle level, which makes every status byte we sample suspect. */
    gpio_cfg.gpio_pins = GPIO_PINS_4;
    gpio_cfg.gpio_mode = GPIO_MODE_INPUT;
    /* Warm-handoff also wants stock's defined idle level: the acquisition
     * loop samples MISO with no config-port anchor available (Exp L), so a
     * floating input would make idle bytes unreadable noise. */
    gpio_cfg.gpio_pull = GPIO_PULL_UP;
    gpio_init(GPIOB, &gpio_cfg);
    gpio_cfg.gpio_pull = GPIO_PULL_NONE;   /* restore shared struct default */

    /* PB5 = SPI3_MOSI: AF push-pull, 50MHz */
    gpio_cfg.gpio_pins = GPIO_PINS_5;
    gpio_cfg.gpio_mode = GPIO_MODE_MUX;
    gpio_cfg.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(GPIOB, &gpio_cfg);

    /* PB6 = SPI3_CS: GPIO output push-pull (software CS) */
    gpio_cfg.gpio_pins = GPIO_PINS_6;
    gpio_cfg.gpio_mode = GPIO_MODE_OUTPUT;
    gpio_cfg.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(GPIOB, &gpio_cfg);

    /* CS deassert (idle HIGH) */
    SPI3_CS_DEASSERT();

    /* PC6 = FPGA SPI enable: output push-pull, set HIGH */

    /* PB11 = FPGA active mode — DO NOT configure as output yet!
     *
     * Stock firmware sets PB11 HIGH in "step 52" (just before
     * vTaskStartScheduler), but critically does NOT configure PB11
     * as a GPIO output before SPI3 init. On reset, PB11 defaults
     * to floating input. If the FPGA has an internal pull-up on its
     * PB11-connected pin, floating = HIGH = active mode.
     *
     * Previously we configured PB11 as output push-pull here, which
     * drives it LOW (GPIO output default). If the FPGA gates its
     * SPI slave interface on PB11, this would explain MISO stuck at
     * 0xFF and zero USART echo frames.
     *
     * PB11 gpio_init + set HIGH is deferred until the post-H2 meter
     * frontend projection, before the meter activation command block. */

    /*
     * SPI3 register configuration (direct, matching stock firmware):
     *   CTRL1: Master, CPOL=1, CPHA=1, 8-bit, MSB first, SSM=1, SSI=1, /2
     *
     * Bit layout of SPI_CTRL1:
     *   [0]   CPHA   = 1 (Mode 3)
     *   [1]   CPOL   = 1 (Mode 3)
     *   [2]   MSTEN  = 1 (Master)
     *   [5:3] MDIV   = 000 (/2 prescaler → 60MHz from 120MHz APB1)
     *   [6]   SPIEN  = 0 (enable later)
     *   [7]   LTF    = 0 (MSB first)
     *   [8]   SWCSIN = 1 (SSI high)
     *   [9]   SWCSEN = 1 (SSM enable)
     *   [10]  RONLY  = 0 (full duplex)
     *   [11]  FBN    = 0 (8-bit)
     */
    FPGA_SPI->ctrl1 = (1 << 0)   /* CPHA = 1 */
               | (1 << 1)   /* CPOL = 1 */
               | (1 << 2)   /* MSTEN = 1 */
               /* BR[2:0] = 000 → /2 prescaler = 60MHz from 120MHz APB1.
                * Compliance audit (2026-04-06): stock uses /2 (60MHz),
                * confirmed by fpga_task_annotated.c, FPGA_TASK_ANALYSIS.md,
                * and remaining_unknowns.md. We had (1<<3) = /4 = 30MHz
                * which was WRONG — half the expected clock rate. */
               | (1 << 8)   /* SWCSIN (SSI) = 1 */
               | (1 << 9);  /* SWCSEN (SSM) = 1 */

    /* Stock firmware sets CTL1 |= 0x03 (RXDMAEN + TXDMAEN).
     * Compliance audit (2026-04-06): our previous comment that "DMA must
     * be DISABLED or the data register won't work" was WRONG. On AT32/STM32F1,
     * setting DMA enable bits without configuring DMA channels just causes
     * ignored DMA requests — polled DR access still works fine. The FPGA
     * may depend on seeing these DMA request signals as part of its SPI
     * slave handshake. Match stock exactly. */
    FPGA_SPI->ctrl2 = 0x03;  /* RXDMAEN + TXDMAEN (match stock) */

    /* Enable SPI */
    FPGA_SPI->ctrl1 |= (1 << 6) /* SPE */;

    /* Enable SPI3 interrupt in NVIC (stock enables IRQ #51).
     * We use polled transfers, but the stock firmware enables this and the
     * FPGA may expect it. Stub handler below clears any pending flags. */
    NVIC_EnableIRQ(SPI3_I2S3EXT_IRQn);

    /*
     * PC6 = FPGA SPI enable.
     *
     * Stock V1.2.0 enables/configures SPI3 first, then configures GPIOC.6 and
     * drives it high before the 100 ms pre-handshake delay. The previous open
     * firmware raised PC6 before SPI3 CTRL1/CTRL2/SPE were programmed, which
     * could expose an unready SPI master to the FPGA. Keep this ordering close
     * to the stock boot trace instead of treating PC6 as a harmless early GPIO.
     * This is still not H2 acceptance proof; live low-DCV validation decides
     * whether the FPGA actually applied the table.
     */
    gpio_cfg.gpio_pins = GPIO_PINS_6;
    gpio_cfg.gpio_mode = GPIO_MODE_OUTPUT;
    gpio_cfg.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(GPIOC, &gpio_cfg);
    GPIOC->scr = PC6_MASK;

    /* Capture register state for diagnostics */
    fpga.diag_remap5 = IOMUX->remap;   /* STM32-compatible remap */
    fpga.diag_remap7 = IOMUX->remap5;  /* AT32 GMUX remap5: spi3_gmux lives here */
    fpga.diag_spi_ctrl1 = FPGA_SPI->ctrl1;
    fpga.diag_spi_sts = FPGA_SPI->sts;

    /* ---------------------------------------------------------------
     * Step 5: SysTick delay
     * Stock firmware has ~100ms delay after SPI3 enable before
     * handshake. Previous value was 20ms total — too short.
     * --------------------------------------------------------------- */
    systick_delay_ms(100);

    /* ---------------------------------------------------------------
     * Step 6: SPI3 FPGA handshake — CS-framed commands (stock-faithful)
     *
     * GROUND-TRUTHED 2026-06-10 from arm-none-eabi-objdump of the raw
     * stock V1.2.0 binary. The sequence lives in master init
     * (FUN_08023A50) at 0x0802676E–0x08026D8x; the bulk-upload loop at
     * 0x08026B28 occurs exactly once in the image. Register tracking:
     *   r4 = 0x40011000 (GPIOC base), lr = 0xFFFFFC10, ip = 0xFFFFFC14
     *   [r4,lr] = 0x40010C10 = GPIOB_BSRR → PB6 HIGH = CS DEASSERT
     *   [r4,ip] = 0x40010C14 = GPIOB_BRR  → PB6 LOW  = CS ASSERT
     *
     * This INVERTS the CS polarity claimed by
     * SPI3_HANDSHAKE_BYTE_ACCURATE.md (which also mis-attributed the
     * code to FUN_08027a50 — that doc's warmup bursts and held-low CS
     * are not in the stock image). Stock frames EACH command in its own
     * CS-LOW window and clocks one dummy byte with CS HIGH in between:
     *
     *   CS↑ 00 | CS↓ 05 00 CS↑ 00 | ~100ms
     *          | CS↓ 12 00 CS↑ 00 | ~100ms
     *          | CS↓ 15 00 CS↑ 00 |
     *          | CS↓ 3B <115,638-byte bitstream> CS↑ 00
     *          | CS↓ 3A 00 CS↑ 00 | CS↓ 00 CS↑ 00
     *
     * The H2 blob is the Gowin FPGA bitstream, not a cal table — same
     * 0x3B/0x3A bracket, size ±1 byte, and 160-byte frame structure as
     * the working rosenrot00/OpenScope-2C23T HW4 loader. See
     * analysis_v120/h2_extracted/h2_is_gowin_bitstream_2c23t_evidence.md
     * and docs/fpga_bitstream_replay_plan.md.
     *
     * Every stock byte is a full-duplex polled exchange (wait TXE →
     * write → wait RXNE → read); spi3_xfer matches that exactly, and
     * spi3_pump_h2_record is the gap-free equivalent for the bulk stream.
     * --------------------------------------------------------------- */

    /* The full PB11-arm → prelude → bitstream upload → close → scope-config
     * handshake now lives in fpga_spi3_config_sequence() so the debug shell
     * (`fpga reinit`) can replay it on demand for fast iteration without a
     * reflash. Parameters let us sweep the variables under investigation. */
    /* Warm-handoff test: the FPGA was already configured (scope) by stock
     * before the no-power-loss handoff. Do NOT run the SSPI config sequence —
     * the running design's config port is closed anyway, and reading it
     * desynchronises acquisition (Exp L). Arm PB11 (active mode) to match
     * stock's scope-run posture (PC6 is already HIGH from above), then set up
     * the two things the acquisition task needs, mark init done, and bail
     * before the meter-frontend routing + meter USART commands. */
    gpio_cfg.gpio_pins = GPIO_PINS_11;
    gpio_cfg.gpio_mode = GPIO_MODE_OUTPUT;
    gpio_cfg.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(GPIOB, &gpio_cfg);
    GPIOB->scr = PB11_MASK;   /* PB11 HIGH — FPGA active (match stock scope) */

    /* Cold-config + live-readout rig (2026-08-13): instead of relying on a stock
     * pre-config (the plain warm handoff), DO the bit-bang cold config HERE, then
     * fall through to the warmtest read/render pipeline. This is the reliable
     * arm-test bench: real config (Build B breaks the wall, DONE_FINAL set) plus
     * the fpga_acq_task 0x04/0x05 readout that latches the demo trace off
     * on real data. Runs AFTER PB11 (IOR1B) and with PC6 (IOB7B) already HIGH from
     * Step 4, so the engine-arm co-enables are asserted when the FPGA_CONFIG_B_ARM
     * writes fire inside the sequence. `make guest-coldtrace`. */
    fpga_boot_upload();

    /* PC0 = FPGA data-ready, active LOW. Nothing else in the image ever
     * configures this pin — every prior read relied on the reset-default
     * floating input. Make it an explicit input with PULL-UP: undriven ⇒
     * HIGH ⇒ "not ready", so a floating line on an unconfigured FPGA cannot
     * fake a data-ready and poison the cold-boot negative control. A
     * configured FPGA actively driving the line wins over the weak pull. */
    gpio_cfg.gpio_pins = GPIO_PINS_0;
    gpio_cfg.gpio_mode = GPIO_MODE_INPUT;
    gpio_cfg.gpio_pull = GPIO_PULL_UP;
    gpio_init(GPIOC, &gpio_cfg);
    gpio_cfg.gpio_pull = GPIO_PULL_NONE;   /* restore shared struct default */

    /* PC0 falling-edge counter on EXINT0 (2026-08-14) — the data-ready EDGE
     * instrument. Counts fresh ready events regardless of pulse width (a
     * polled level read is blind to short deasserts, which kept every prior
     * PC0 observation regime-ambiguous) and paces the acquisition task the
     * way stock's runtime is paced (June capture: one 04/05 pair per ready
     * event, never a level re-read). Read the count in `status`. */
    IOMUX->exintc1_bit.exint0 = 2;         /* EXINT line 0 <- port C */
    {
        exint_init_type ei;
        exint_default_para_init(&ei);
        ei.line_select   = EXINT_LINE_0;
        ei.line_mode     = EXINT_LINE_INTERRUPT;
        ei.line_polarity = EXINT_TRIGGER_FALLING_EDGE;
        ei.line_enable   = TRUE;
        exint_init(&ei);
    }
    nvic_irq_enable(EXINT0_IRQn, 6, 0);

    /* Trigger DAC — THE missing link (Stlkv, issue #18, 2026-08-12): the MCU
     * reset zeroes DAC1 (PA4, DHR12R1 @ 0x40007408), the FPGA's trigger
     * comparator loses its reference, and every capture reads flat zeros
     * until it is restored. Arm mid-scale. Touches only MCU-internal state
     * (CRM clocks, PA4 analog, DAC registers) — cannot disturb the FPGA.
     * The signal generator is on DAC channel 2 (PA5); both channels share
     * DAC_CTRL, and the DAC owner (dac.c) changes it only by a masked
     * read-modify-write of one channel's half, so the generator cannot
     * disturb this. Channel 1 is initialised here, at boot, right before its
     * first write (dac.h); a write no longer initialises it. */
    scope_trigger_dac_init();
    scope_trigger_dac_raw(2048);

    /* CH2 offset reference — TMR13 CH1 as a PWM-DAC on PA6, the CH2 analog of
     * the DAC1 arm above (the factory firmware's TMR13 setup, decoded from its
     * master_init). Without it CH2 reads all zeros. Confirmed on the bench:
     * EXP-07 (2026-08-17) moved CH2's mean with TMR13 alone and not with DAC1;
     * EXP-21 (2026-08-21) read 1 kHz on CH1 and 2 kHz on CH2 independently.
     * Like the DAC1 write this is MCU-internal (CRM/PA6/TMR13). */
    scope_trigger_ch2_init();
    /* Arm at the code that CENTERS CH2, not raw mid-scale. DAC1 (CH1) centers at
     * 2048 because it is a true 12-bit DAC — 2048 is mid voltage. TMR13 is a
     * PWM-DAC through an RC filter, so its centering code is NOT 2048: bench
     * unit #1, range 5 (2026-08-27, JDS6600 on CH2) measured 2048->mean 65,
     * 3072->mean 195, which interpolates to ADC 128 at code 2544. Arming 2048
     * left CH2 at mean ~65 (low, looked like "railed noise" with no signal);
     * 2544 boots CH2 centered, mirroring CH1's fixed-arm pattern. The runtime
     * `fpga scope center ch2` servo refines this per-range / per-unit. */
    scope_trigger_ch2_raw(2544);

    /* Frontend relays — bench run 3 (2026-08-12, first live capture): the
     * engine free-runs after a pinhole handoff, but the trace sat at the
     * baseline and ignored a 4.2 V battery, because the MCU reset floats
     * the relay bank (PC12/PE4/PE5/PE6/PA15/PA10/PB10) and the coils drop —
     * input path disconnected. Configure the bank as outputs (Step 9
     * normally does this but warmtest returns before it) and drive stock's
     * scope posture for the UI's default range. Pure GPIO relay writes:
     * Exp C proved these are signal relays, not config straps, so this
     * cannot disturb the FPGA. (PA15 is free — SWJ_CFG remap ran above.) */
    gpio_cfg.gpio_mode = GPIO_MODE_OUTPUT;
    gpio_cfg.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_cfg.gpio_pins = GPIO_PINS_12;
    gpio_init(GPIOC, &gpio_cfg);
    gpio_cfg.gpio_pins = GPIO_PINS_4 | GPIO_PINS_5 | GPIO_PINS_6;
    gpio_init(GPIOE, &gpio_cfg);
    gpio_cfg.gpio_pins = GPIO_PINS_15 | GPIO_PINS_10;
    gpio_init(GPIOA, &gpio_cfg);
    gpio_cfg.gpio_pins = GPIO_PINS_10;
    gpio_init(GPIOB, &gpio_cfg);

    /* AC/DC COUPLING RELAYS — PD12 (CH1), PD13 (CH2), HIGH = DC.
     * Found 2026-08-15 in stock's scope settings handler (0x0800BD60 item 2 ->
     * GPIOD BSRR/BRR at 0x0800C846..0x0800C93E; boot restore at 0x0802C58E;
     * state bytes ms[0x00]/ms[0x01], 0=AC 1=DC), and BENCH-CONFIRMED the same
     * evening: with PD12 driven HIGH a 300 mV -> 3 V DC step moved the captured
     * mean 218 -> railed, where before it did not move at all.
     *
     * ⚠ WHY THIS WAS INVISIBLE: our reset default leaves PD12 in EXMC ALTERNATE
     * FUNCTION (address line A17) — GPIOD CRH read 0xBB4BBBBB on the bench, and
     * an AF pin ignores ODR, so writing BSRR did nothing. None of our LCD
     * accesses assert A17 (the LCD's RS is A16/PD11 — 0x6001FFFE vs 0x60020000
     * differ in HADDR bit 17 = A16, despite the comment in lcd.c), so EXMC held
     * PD12 LOW forever = permanently AC-coupled, with a ~9 Hz high-pass that we
     * spent a morning characterising as an analog property of the front end.
     * Taking the pins as plain GPIO outputs is what stock does (its GPIOD CRH
     * nibbles for PD12/PD13 are 1 = output push-pull in the Exp E SWD dump).
     *
     * DC is the default for scope work; a coupling menu should drive these. */
    gpio_cfg.gpio_pins = GPIO_PINS_12 | GPIO_PINS_13;
    gpio_init(GPIOD, &gpio_cfg);
    GPIOD->scr = (1U << 12) | (1U << 13);   /* both channels DC-coupled */

    fpga_set_scope_frontend_ranges(scope_state_get());
    /* (The PC12-HIGH override that used to sit here is gone: PC12 is the input
     * PATH select, not the DC-coupling select — the range table now sets it
     * per range, and coupling is PD12/PD13 above. Bench 2026-08-15.) */

    fpga.initialized = true;
}

/* ═══════════════════════════════════════════════════════════════════
 * Task Creation
 * ═══════════════════════════════════════════════════════════════════ */

void fpga_create_tasks(void)
{
    /* Only create FPGA tasks if init succeeded.
     * If fpga_init() failed or was skipped, the rest of the
     * firmware still works — just no scope/meter data. */
    if (!fpga.initialized) {
        return;
    }

    /* Create queues */
    usart_tx_queue = xQueueCreate(10, sizeof(uint32_t));
    meter_rx_queue = xQueueCreate(8, sizeof(fpga_meter_rx_event_t));

    /* P0.2 frame staging (see fpga_acq_frames_commit). Heap, not BSS — the
     * boot-stack headroom over BSS is ~2 KB (flash_fs.c has the same note).
     * A failed alloc falls back to the pre-2026-08-20 in-place writes. */
    acq_stage_ch1 = (uint8_t *)pvPortMalloc(FPGA_ADC_BUF_SIZE);
    acq_stage_ch2 = (uint8_t *)pvPortMalloc(FPGA_ADC_BUF_SIZE);
    if (acq_stage_ch1 == NULL || acq_stage_ch2 == NULL) {
        if (acq_stage_ch1) { vPortFree(acq_stage_ch1); }
        if (acq_stage_ch2) { vPortFree(acq_stage_ch2); }
        acq_stage_ch1 = acq_stage_ch2 = NULL;
    } else {
        memset(acq_stage_ch1, 128, FPGA_ADC_BUF_SIZE);   /* mid-scale */
        memset(acq_stage_ch2, 128, FPGA_ADC_BUF_SIZE);
    }

    /* The acquisition task: reads records from the core over SPI3. */
    xTaskCreate(fpga_acq_task, "fpga", 256, NULL, 3, &acq_task_handle);
    /* The meter tasks. Each is gated on the meter link (meter_link_serving),
     * so with the link off USART2 is quiet, and with it on only the meter's
     * own words go out. */
    xTaskCreate(fpga_usart_tx_task,    "dvom_TX",   FPGA_DVOM_TX_STACK, NULL, 2, &tx_task_handle);
    xTaskCreate(fpga_usart_rx_task,    "dvom_RX",   FPGA_DVOM_RX_STACK, NULL, 3, &rx_task_handle);
    /* 192, not 64: function changes (fpga_set_meter_mode) now run here. */
    xTaskCreate(fpga_meter_poll_task,  "meter_poll", 192, NULL, 2, &meter_poll_handle);
}
