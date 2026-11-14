/*
 * OpenScope 2C53T - DAC owner (see dac.h).
 *
 * Register values are stock V1.2.0's: channel 1 from master_init
 * (0x0802C23C..0x0802C26A, net CTRL low half 0x3D), channel 2 from the
 * generator init and off paths (0x0802CA28..0x0802CAF6, 0x08029DEA).
 *
 * Built with -DHOST_REGS for tests/test_dac.c: DAC is then a plain struct,
 * and every store, clock, pin and mask call is logged in order.
 */
#include "dac.h"
#include "siggen_wave.h"   /* siggen_dac_ctrl_ch2, SIGGEN_DAC_* (pure) */

#ifdef HOST_REGS
#define DAC            (&dac_host_regs)
#define HOST_LOG(w, v) dac_host_log((w), (v))
#else
#include "at32f403a_407.h"
#define HOST_LOG(w, v) ((void)0)
#endif

/* DAC_CTRL channel-1 half (bits 15:0). */
#define DAC_D1EN       (1u << 0)
#define DAC_D1TEN      (1u << 2)    /* trigger enable */
#define DAC_D1TSEL_SW  (7u << 3)    /* trigger = software (0b111) */
/* D1OBDIS (bit 1) stays clear: output buffer on, as stock. */
#define DAC_CH1_RUN    (DAC_D1TSEL_SW | DAC_D1TEN | DAC_D1EN)   /* 0x3D */
#define DAC_D1SWTRG    (1u << 0)
#define DAC_D2SWTRG    (1u << 1)

static volatile bool ch1_ready;

static uint32_t mask_irq(void)
{
#ifdef HOST_REGS
    HOST_LOG(DAC_HOST_MASK, 0);
    return 0;
#else
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    return primask;
#endif
}

static void unmask_irq(uint32_t primask)
{
#ifdef HOST_REGS
    (void)primask;
    HOST_LOG(DAC_HOST_UNMASK, 0);
#else
    __set_PRIMASK(primask);
#endif
}

/* The one DAC_CTRL store. Caller holds the mask. */
static void ctrl_store(uint32_t v)
{
    DAC->ctrl = v;
    HOST_LOG(DAC_HOST_CTRL, v);
}

/* Every DAC_CTRL read-modify-write: clear `clr`, set `set`, one masked store. */
static void dac_ctrl_modify(uint32_t clr, uint32_t set)
{
    const uint32_t pm = mask_irq();
    ctrl_store((DAC->ctrl & ~clr) | set);
    unmask_irq(pm);
}

uint32_t dac_ctrl_get(void)
{
    return DAC->ctrl;
}

void dac_ch1_init(void)
{
    if (ch1_ready) return;
    /* Clocks and PA4 are idempotent; the CTRL stores are made once, the
     * ready check repeated inside their mask section. */
#ifdef HOST_REGS
    HOST_LOG(DAC_HOST_CLK_DAC, 1);
    HOST_LOG(DAC_HOST_CLK_GPIOA, 1);
    HOST_LOG(DAC_HOST_PA4_ANALOG, 0);
#else
    crm_periph_clock_enable(CRM_DAC_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
    gpio_init_type gpio_cfg;                 /* PA4 = DAC channel 1, analog */
    gpio_default_para_init(&gpio_cfg);
    gpio_cfg.gpio_pins = GPIO_PINS_4;
    gpio_cfg.gpio_mode = GPIO_MODE_ANALOG;
    gpio_init(GPIOA, &gpio_cfg);
#endif
    /* Trigger select and trigger enable first (0x3C), D1EN last in its own
     * store (0x3D), so the first software trigger cannot latch against an
     * unconfigured trigger select. Channel 2's half is kept. */
    const uint32_t pm = mask_irq();
    if (!ch1_ready) {
        const uint32_t v = (DAC->ctrl & ~SIGGEN_DAC_CH1_MASK) | DAC_D1TSEL_SW | DAC_D1TEN;
        ctrl_store(v);
        ctrl_store(v | DAC_D1EN);
        ch1_ready = true;
    }
    unmask_irq(pm);
}

bool dac_ch1_ready(void)
{
    return ch1_ready;
}

#ifdef HOST_REGS
void dac_host_reset(void)
{
    ch1_ready = false;
}
#endif

void dac_ch1_write(uint16_t code)
{
    /* Boot initialises channel 1 before its first write in every image
     * (dac.h); a write before that is dropped, never made to an unclocked
     * peripheral nor turned into an init on whichever task wrote first. */
    if (!ch1_ready) return;
    const uint32_t pm = mask_irq();
    /* Stock keeps DHR12R1's upper bits; the trigger bits self-clear, so a
     * plain store of D1SWTRG leaves channel 2's trigger bit alone. */
    const uint32_t d = (DAC->d1dth12r & 0xFFFFF000u) | (code & 0x0FFFu);
    DAC->d1dth12r = d;
    HOST_LOG(DAC_HOST_D1DTH12R, d);
    DAC->swtrg = DAC_D1SWTRG;
    HOST_LOG(DAC_HOST_SWTRG, DAC_D1SWTRG);
    unmask_irq(pm);
}

void dac_ch2_config(uint8_t tsel, bool en)
{
    dac_ctrl_modify(~SIGGEN_DAC_CH1_MASK, siggen_dac_ctrl_ch2(0u, tsel, en));
}

void dac_ch2_data_clear(void)
{
    DAC->d2dth12r = 0u;
    HOST_LOG(DAC_HOST_D2DTH12R, 0u);
}

void dac_ch2_output_zero(void)
{
    const uint32_t d = DAC->d2dth12r & ~0xFFFu;
    DAC->d2dth12r = d;
    HOST_LOG(DAC_HOST_D2DTH12R, d);
    dac_ch2_config(SIGGEN_DAC_TSEL_SW, true);
    DAC->swtrg = DAC_D2SWTRG;               /* channel 1's trigger bit 0 */
    HOST_LOG(DAC_HOST_SWTRG, DAC_D2SWTRG);
    dac_ch2_config(SIGGEN_DAC_TSEL_TMR7, true);
}
