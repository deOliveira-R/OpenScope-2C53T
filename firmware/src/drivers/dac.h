/*
 * OpenScope 2C53T - DAC owner: the only writer of the DAC peripheral.
 *
 * Both channels share DAC_CTRL:
 *   - channel 1, PA4: the scope's CH1 offset / trigger reference
 *     (scope_trigger.c, scope_vpos.c). Software-triggered, one code at a time.
 *   - channel 2, PA5: the signal generator (dac_output.c). TMR7-triggered,
 *     fed by DMA2 channel 4 from the cycle buffer.
 *
 * Every DAC_CTRL change is a masked read-modify-write of one channel's half
 * (dac_ctrl_modify), so a channel-1 write can never put back a stale
 * channel-2 half or the other way round, whichever task or interrupt runs.
 * The code-plus-trigger pair of channel 1 is one masked section too.
 *
 * Init is done once, at boot, in every image, before the channel's first
 * write: channel 2 from dac_output_init() (main.c), channel 1 from
 * scope_trigger_dac_init() -> dac_ch1_init(), in fpga_init() just before
 * the warm images' mid-scale arm and in main.c just before
 * scope_vpos_init() (the first CH1 write of the other images). Later init
 * calls are no-ops; a write never initialises.
 */
#ifndef DAC_H
#define DAC_H

#include <stdint.h>
#include <stdbool.h>

/* Channel 1 (PA4). Clocks, PA4 analog, CTRL low half 0x3C then 0x3D
 * (software trigger selected and enabled, buffer on, D1EN last), as stock's
 * master_init. Once only: a second call does nothing. */
void dac_ch1_init(void);
bool dac_ch1_ready(void);

/* Channel 1: DHR12R1[11:0] = code, then the channel-1 software trigger, in
 * one masked section. Before dac_ch1_init() the write is dropped. */
void dac_ch1_write(uint16_t code);

/* Channel 2 (PA5). Replace channel 2's half of DAC_CTRL with the generator
 * setup (siggen_dac_ctrl_ch2: trigger tsel, DMA on, buffer on, EN2 = en);
 * channel 1's half is kept. tsel: SIGGEN_DAC_TSEL_TMR7 / _SW. */
void dac_ch2_config(uint8_t tsel, bool en);

/* Channel 2 data register D2DTH12R: the DMA's destination. */
void dac_ch2_data_clear(void);              /* D2DTH12R = 0 (init) */

/* Stock's generator off state (0x08029DEA): D2DTH12R &= ~0xFFF, software
 * trigger selected, D2SWTRG to move code 0 to the output, back to TMR7.
 * Channel 1's bits, data and trigger are not touched. */
void dac_ch2_output_zero(void);

/* DAC_CTRL as it is now (read only). */
uint32_t dac_ctrl_get(void);

#ifdef HOST_REGS
/* Host write-log test (tests/test_dac.c): the fake register block and the
 * hooks dac.c calls instead of the hardware. */
typedef struct {
    uint32_t ctrl, swtrg, d1dth12r, d2dth12r;
} dac_host_regs_t;
extern dac_host_regs_t dac_host_regs;
enum { DAC_HOST_CTRL, DAC_HOST_SWTRG, DAC_HOST_D1DTH12R, DAC_HOST_D2DTH12R,
       DAC_HOST_CLK_DAC, DAC_HOST_CLK_GPIOA, DAC_HOST_PA4_ANALOG,
       DAC_HOST_MASK, DAC_HOST_UNMASK };
void dac_host_log(int what, uint32_t value);
void dac_host_reset(void);              /* channel 1 not initialised again (one boot per test) */
#endif

#endif /* DAC_H */
