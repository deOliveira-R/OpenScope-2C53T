/*
 * OpenScope 2C53T - FPGA driver: bring-up and tasks
 *
 * The FPGA runs our own capture core, loaded at every boot; the meter chip
 * is a separate part on USART2.
 *
 * fpga_init(), in order:
 *   1. remap to free PB3/PB4/PB5 from JTAG
 *   2. USART2 set to 9600 baud and left disabled (the meter link enables it)
 *   3. SPI3 init and enable (mode 3, /2 = 60 MHz), PC6 (FPGA supply) high
 *   4. the design is uploaded on the same pins as GPIO (fpga_boot_upload),
 *      then the core's registers are written and the core is identified
 *   5. PC0 (record ready) on EXINT0, the two offset references, the
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
 * Initialization
 * ═══════════════════════════════════════════════════════════════════ */

void fpga_init(void)
{
    /* ---------------------------------------------------------------
     * Step 1: turn JTAG off, keep SWD. At reset PB3 is JTDO, PB4 NJTRST
     * and PA15 JTDI; this frees them for SPI3 and the relays.
     *
     * The AT32F403A has two remap systems and both are set:
     *   - IOMUX->remap (offset 0x04, the STM32-compatible one):
     *     SWJ_CFG bits [26:24] = 010. Bit 28 (SPI3 remap) is not touched;
     *     its reset value 0 keeps SPI3 on PB3/PB4/PB5.
     *   - the AT32 GMUX registers: SWJTAG_GMUX (remap7, offset 0x30) = 010,
     *     and SPI3_GMUX (remap5, offset 0x28) = 0010, which routes SPI3
     *     SCK/MISO/MOSI to PB3/PB4/PB5 (vendor header).
     * --------------------------------------------------------------- */
    crm_periph_clock_enable(CRM_IOMUX_PERIPH_CLOCK, TRUE);

    /* Change only SWJ_CFG [26:24] */
    {
        uint32_t remap = IOMUX->remap;
        remap &= ~(0x7u << 24);   /* Clear SWJ_CFG bits */
        remap |= (0x2u << 24);    /* Set SWJ_CFG = 010 (JTAG off, SWD on) */
        IOMUX->remap = remap;
    }

    /* GMUX remap (AT32-specific). Stock does not write SPI3_GMUX; setting it
     * selects the PB3/PB4/PB5 route explicitly. */
    gpio_pin_remap_config(SWJTAG_GMUX_010, TRUE);
    gpio_pin_remap_config(SPI3_GMUX_0010, TRUE);

    /* ---------------------------------------------------------------
     * Step 2: USART2 pins and 9600 baud. The USART is left disabled; the
     * meter link (meter_link.c) enables it with its interrupts.
     * --------------------------------------------------------------- */
    crm_periph_clock_enable(CRM_USART2_PERIPH_CLOCK, TRUE);

    /* PA2 = USART2_TX: AF push-pull */
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

    /* 9600 baud from APB1 = HCLK/2. CTRL1 = 0: USART disabled, as in stock's
     * scope boot, so PA2/PA3 stay quiet until the meter link starts. */
    USART2->baudr = system_core_clock / 2 / FPGA_USART_BAUD;  /* APB1 = HCLK/2 */
    USART2->ctrl1 = 0;

    /* ---------------------------------------------------------------
     * Step 3: SPI3 and PC6.
     *
     * First a 50 ms settle. main() drove PC6, PB11 and PC11 at its very
     * start; whether the upload still needs this wait is not measured.
     * --------------------------------------------------------------- */
    systick_delay_ms(50);

    /* The SPI3 pins and the peripheral (fpga_boot.c owns their states). */
    spi3_pins_peripheral();


    /* PC6 = FPGA supply enable, push-pull HIGH. main() already drove it HIGH
     * at its start; this sets it up again through the driver. */
    gpio_cfg.gpio_pins = GPIO_PINS_6;
    gpio_cfg.gpio_mode = GPIO_MODE_OUTPUT;
    gpio_cfg.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(GPIOC, &gpio_cfg);
    GPIOC->scr = PC6_MASK;

    /* Stock waits about 100 ms after enabling SPI3 before it talks to the
     * FPGA. */
    systick_delay_ms(100);

    /* ---------------------------------------------------------------
     * Step 4: upload the design and arm the core (fpga_boot.c).
     *
     * PB11 (CH2 range relay bit 1) is set up as an output and driven HIGH
     * first, the level main() already gave it; the CH2 range table sets
     * it at the end of this function.
     * --------------------------------------------------------------- */
    gpio_cfg.gpio_pins = GPIO_PINS_11;
    gpio_cfg.gpio_mode = GPIO_MODE_OUTPUT;
    gpio_cfg.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(GPIOB, &gpio_cfg);
    GPIOB->scr = PB11_MASK;   /* PB11 HIGH */

    fpga_boot_upload();

    /* ---------------------------------------------------------------
     * Step 5: record-ready line, offset references, relays, coupling.
     * --------------------------------------------------------------- */

    /* PC0 = record ready from the FPGA, active LOW. Input with pull-up, so
     * an FPGA that does not drive the line reads "not ready"; a driving FPGA
     * overrides the weak pull. */
    gpio_cfg.gpio_pins = GPIO_PINS_0;
    gpio_cfg.gpio_mode = GPIO_MODE_INPUT;
    gpio_cfg.gpio_pull = GPIO_PULL_UP;
    gpio_init(GPIOC, &gpio_cfg);
    gpio_cfg.gpio_pull = GPIO_PULL_NONE;   /* restore shared struct default */

    /* EXINT0 counts falling edges of PC0 (EXINT0_IRQHandler in fpga_acq.c).
     * The acquisition task waits for a new edge, which a level read could
     * miss when the pulse is short; one record pair per edge, as stock. The
     * count is shown by `status`. */
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

    /* The analog offset references of the two channels (the functions keep
     * an older "trigger" name; the trigger level is a core register). An MCU
     * reset zeroes them, and a channel without its reference reads flat
     * zeros. These are fixed boot values; main() later applies the unit's
     * calibration page when it is usable (scope_vpos).
     *
     * CH1: DAC channel 1 on PA4, set to mid-scale 2048. It is initialised
     * here, right before its first write (dac.h). The signal generator uses
     * DAC channel 2 (PA5); dac.c changes only one channel's half of the
     * shared DAC_CTRL, so the two cannot disturb each other. */
    scope_trigger_dac_init();
    scope_trigger_dac_raw(2048);

    /* CH2: TMR13 channel 1 as a PWM DAC on PA6, through an RC filter, so
     * its centre code is not 2048. Measured on unit #1, range 5
     * (2026-08-27): code 2048 gave a mean of 65 and 3072 a mean of 195,
     * which interpolates to the ADC mid-point 128 at code 2544.
     * `fpga scope center ch2` refines it per range and per unit. */
    scope_trigger_ch2_init();
    scope_trigger_ch2_raw(2544);

    /* Range relays: an MCU reset leaves these pins floating, the relay
     * coils drop and the input is disconnected. Make them outputs (PC12,
     * PE4, PE5, PE6, PA15, PA10, PB10; PB11 was done in step 4);
     * fpga_set_scope_frontend_ranges() below drives them. PA15 is free
     * because JTAG was turned off in step 1. */
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

    /* AC/DC coupling relays: PD12 (CH1), PD13 (CH2), HIGH = DC (decoded from
     * stock; on the bench PD12 HIGH passed a DC step). They must be plain
     * GPIO outputs, as in stock: as an EXMC alternate-function pin PD12
     * ignores output writes and stays LOW, which leaves CH1 AC-coupled.
     * Boot sets DC; fpga_apply_coupling() changes them later. */
    gpio_cfg.gpio_pins = GPIO_PINS_12 | GPIO_PINS_13;
    gpio_init(GPIOD, &gpio_cfg);
    GPIOD->scr = (1U << 12) | (1U << 13);   /* both channels DC-coupled */

    fpga_set_scope_frontend_ranges();

    fpga.initialized = true;
}

/* ═══════════════════════════════════════════════════════════════════
 * Task Creation
 * ═══════════════════════════════════════════════════════════════════ */

/* Create the queues, the record staging buffers and the four tasks. Called
 * by main() before the scheduler starts. */
void fpga_create_tasks(void)
{
    /* No tasks if fpga_init() did not run (fpga.initialized is set only at
     * its end); the rest of the firmware works without scope or meter data. */
    if (!fpga.initialized) {
        return;
    }

    /* Staging buffers: a record is read into these and copied to the public
     * buffers in one step (fpga_acq_frames_commit). On the heap, not in BSS,
     * because the boot stack has little room above BSS. If an allocation
     * fails, the task writes the public buffers directly. */
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
    xTaskCreate(fpga_acq_task, "fpga", FPGA_ACQ_STACK, NULL, 3, &acq_task_handle);
}
