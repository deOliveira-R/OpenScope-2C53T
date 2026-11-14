/*
 * OpenScope 2C53T - signal generator output driver.
 *
 * Stock V1.2.0's hardware path (init 0x0802C8E8..0x0802CAF6, apply
 * 0x08029D40):
 *
 *   dac_buffer[100] -> DMA2 ch4 (flex request 6 = DAC2, circular)
 *                   -> DAC channel 2 (D2DTH12R, output buffer ON) -> PA5
 *                         ^
 *                    TMR7 TRGO (update), 100 updates per output cycle
 *
 * DAC channel 1 (PA4) is the scope's CH1 offset / trigger reference DAC
 * (scope_trigger.c). The DAC registers are written only by the DAC owner
 * (dac.h); this driver asks it for channel-2 changes, which never touch
 * channel 1, and it never touches DMA2 ch3, so the generator can run in
 * every mode, as it does in stock.
 */

#ifndef DAC_OUTPUT_H
#define DAC_OUTPUT_H

#include <stdint.h>
#include <stdbool.h>

#include "siggen_wave.h"

#define DAC_BUFFER_SIZE  SIGGEN_CYCLE_SAMPLES

/*
 * PA5 analog, DAC channel 2 armed on TMR7 TRGO with DMA, DMA2 ch4
 * running circular over the buffer, TMR7 programmed but stopped: the
 * output sits at 0 V until dac_output_apply() starts the timer. Stock
 * does the same at init (EN2 set, CEN left clear). Call once at boot.
 */
void dac_output_init(void);

/*
 * Start or retune: program TMR7 DIV/PR from *t, fire an update event and
 * start the counter (stock 0x08029D9E..0x08029DD2). The buffer must hold
 * the cycle already.
 */
void dac_output_apply(const siggen_timing_t *t);

/*
 * Stock's off state (0x08029DD6..0x08029E0A): stop TMR7, then drive code 0
 * through a software trigger and re-select TMR7. DAC2 stays enabled, so
 * the GEN lead is held at 0 V rather than left floating, and the DMA
 * stays armed for the next start.
 */
void dac_output_stop(void);

/* The 100-sample DMA source buffer (12-bit codes, right-aligned). */
uint16_t *dac_output_get_buffer(void);

bool dac_output_is_running(void);

/* TMR7 input clock: APB1 x2 when APB1 is divided (240 MHz on this board,
 * measured live: stock's DIV 1199 / PR 1 gave 998 Hz for 1 kHz). */
uint32_t dac_output_timer_clock_hz(void);

#endif /* DAC_OUTPUT_H */
