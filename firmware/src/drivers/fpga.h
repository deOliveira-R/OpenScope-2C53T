/*
 * OpenScope 2C53T - FPGA driver: bring-up, tasks and shared state
 *
 * The FPGA (Gowin GW1N-UV2) runs our capture core, uploaded at every boot
 * and read over SPI3 (PB3 SCK, PB4 MISO, PB5 MOSI, PB6 chip select; PC6 is
 * the FPGA's supply enable). The parts of the driver have their own headers,
 * all included here:
 *
 *   fpga_boot.h       design upload and reload
 *   fpga_acq.h        records from the core, its registers, interleave, roll
 *   scope_frontend.h  range relays, coupling, converter routing
 *   meter_link.h      the meter chip on USART2 (not part of the FPGA)
 */

#ifndef FPGA_H
#define FPGA_H

#include <stdint.h>
#include <stdbool.h>
#include "FreeRTOS.h"
#include "queue.h"
#include "fpga_boot.h"
#include "fpga_acq.h"
#include "scope_frontend.h"
#include "meter_link.h"

/* State of the driver as a whole. */
typedef struct {
    volatile bool     initialized;  /* fpga_init finished */
    volatile uint8_t  channel_mask;  /* last PC1/PC2 code applied: 1 = CH1, 2 = CH2, 3 = both */
} fpga_state_t;

extern fpga_state_t fpga;   /* defined in fpga.c */

/* Bring up SPI3, USART2 and the control pins, upload the design, identify
 * the core and write its registers. Call after the clocks are set and before
 * the scheduler starts. */
void fpga_init(void);

/* Create the driver's tasks: acquisition (fpga), the meter link's transmit
 * (dvom_TX), receive (dvom_RX) and poll (meter_poll). Call after fpga_init()
 * and before the scheduler starts. */
void fpga_create_tasks(void);

#endif /* FPGA_H */
