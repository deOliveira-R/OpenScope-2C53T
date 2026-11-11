/*
 * OpenScope 2C53T - signal generator output driver (DAC2 / PA5).
 *
 * Mirrors stock V1.2.0 register for register (see dac_output.h). The
 * previous driver used DAC1/PA4 + TMR6 + DMA2 ch3; DAC1 is the scope's
 * CH1 offset DAC, so the generator could never run beside the scope, and
 * stopping it cleared D1EN and killed capture (signal_gen.c used to keep
 * the warm-handoff build inert for that reason).
 *
 * DAC_CTRL is shared with scope_trigger.c, which rewrites the lower half
 * from another task. Every write here is a read-modify-write of the upper
 * half only (siggen_dac_ctrl_ch2), inside a PRIMASK section so the two
 * read-modify-writes cannot interleave and undo each other.
 */

#include "dac_output.h"
#include "at32f403a_407.h"

#include <stddef.h>

static uint16_t dac_buffer[DAC_BUFFER_SIZE];
static volatile bool running = false;

static uint32_t irq_save(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    return primask;
}

static void irq_restore(uint32_t primask)
{
    __set_PRIMASK(primask);
}

/* Replace channel 2's half of DAC_CTRL, keep channel 1's. */
static void dac_ctrl_ch2_write(uint8_t tsel, bool en)
{
    uint32_t pm = irq_save();
    DAC->ctrl = siggen_dac_ctrl_ch2(DAC->ctrl, tsel, en);
    irq_restore(pm);
}

uint32_t dac_output_timer_clock_hz(void)
{
    /* TMR7 sits on APB1; like every timer here its clock is doubled when
     * APB1 is divided from AHB (240 MHz sysclk, APB1 = 120 MHz -> 240 MHz,
     * which the stock bench run confirmed). */
    crm_clocks_freq_type clk;
    crm_clocks_freq_get(&clk);
    return (clk.apb1_freq == clk.ahb_freq) ? clk.apb1_freq : clk.apb1_freq * 2u;
}

void dac_output_init(void)
{
    crm_periph_clock_enable(CRM_DMA2_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_DAC_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_TMR7_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);

    for (unsigned i = 0; i < DAC_BUFFER_SIZE; i++) dac_buffer[i] = 0;

    /* PA5 analog: stock 0x0802C95E..0x0802C97A, gpio_init(GPIOA, pin 5,
     * mode ANALOG). PA4 is left to scope_trigger_dac_init(). */
    gpio_init_type gpio_cfg;
    gpio_default_para_init(&gpio_cfg);
    gpio_cfg.gpio_pins = GPIO_PINS_5;
    gpio_cfg.gpio_mode = GPIO_MODE_ANALOG;
    gpio_init(GPIOA, &gpio_cfg);

    /* TMR7: stopped, TRGO = update event (CTRL2 PTOS = 2, 0x0802CA22). */
    TMR7->ctrl1_bit.tmren = 0;
    TMR7->ctrl2_bit.ptos = 2;
    TMR7->div = 1199;           /* stock's 1 kHz; retuned by apply */
    TMR7->pr = 1;
    TMR7->swevt_bit.ovfswtr = 1;

    /* DAC channel 2 configured, not yet enabled: TMR7 trigger, buffer ON
     * (stock clears BOFF2, 0x0802CA48), no wave generation, DMA on. */
    dac_ctrl_ch2_write(SIGGEN_DAC_TSEL_TMR7, false);
    DAC->d2dth12r = 0;

    /* DMA2 ch4 (0x0802CA5A..0x0802CAEA): mem -> periph, circular, memory
     * increment, 32-bit peripheral / 16-bit memory size as stock, low
     * priority, 100 items, to D2DTH12R. */
    dma_channel_type *ch = DMA2_CHANNEL4;
    ch->ctrl_bit.chen = 0;
    DMA2->clr = 0xFu << 12;     /* channel 4 flags, as stock [r4 - 0x40] */
    ch->ctrl = (1u << 4)        /* DTD: memory -> peripheral */
             | (1u << 5)        /* LM: circular */
             | (1u << 7)        /* MINCM */
             | (2u << 8)        /* PWIDTH 32-bit */
             | (1u << 10);      /* MWIDTH 16-bit; CHPL 0 = low */
    ch->dtcnt = DAC_BUFFER_SIZE;
    ch->paddr = (uint32_t)&DAC->d2dth12r;
    ch->maddr = (uint32_t)dac_buffer;
    /* Flexible request: DMA2 ch4 <- DAC2 (id 6). Stock sets SRC_SEL1
     * DMA_FLEX_EN (bit 24, [r4+0x60]) then SRC_SEL0 bits 31:24 = 6
     * ([r4+0x5C]), the same two fields dma_flexible_config() writes; the
     * lib's DMA module is not linked, so write them here. */
    DMA2->src_sel1_bit.dma_flex_en = 1;
    DMA2->src_sel0_bit.ch4_src = DMA_FLEXIBLE_DAC2;
    ch->ctrl_bit.chen = 1;

    /* EN2 last, in its own write, as stock (0x0802CAF2). Output = 0 V. */
    dac_ctrl_ch2_write(SIGGEN_DAC_TSEL_TMR7, true);
    running = false;
}

void dac_output_apply(const siggen_timing_t *t)
{
    if (t == NULL) { dac_output_stop(); return; }

    /* Self-heal if anything rewrote channel 2 (a full-register DAC write
     * elsewhere would also have to be fixed, this just keeps us running). */
    if ((DAC->ctrl & ~SIGGEN_DAC_CH1_MASK) !=
        (siggen_dac_ctrl_ch2(0, SIGGEN_DAC_TSEL_TMR7, true) & ~SIGGEN_DAC_CH1_MASK))
        dac_ctrl_ch2_write(SIGGEN_DAC_TSEL_TMR7, true);
    if (!DMA2_CHANNEL4->ctrl_bit.chen) DMA2_CHANNEL4->ctrl_bit.chen = 1;

    TMR7->div = t->div;
    TMR7->pr = t->pr;
    TMR7->swevt_bit.ovfswtr = 1;    /* load DIV now; stock EGR |= 1 */
    TMR7->ctrl1_bit.tmren = 1;
    running = true;
}

void dac_output_stop(void)
{
    TMR7->ctrl1_bit.tmren = 0;
    /* 0x08029DEA: D2DTH12R &= ~0xFFF, then a software trigger to move it
     * to the output, then back to the TMR7 trigger. */
    DAC->d2dth12r &= ~0xFFFu;
    dac_ctrl_ch2_write(SIGGEN_DAC_TSEL_SW, true);
    DAC->swtrg = 1u << 1;           /* D2SWTRG; bit 0 (channel 1) left 0 */
    dac_ctrl_ch2_write(SIGGEN_DAC_TSEL_TMR7, true);
    running = false;
}

uint16_t *dac_output_get_buffer(void)
{
    return dac_buffer;
}

bool dac_output_is_running(void)
{
    return running;
}
