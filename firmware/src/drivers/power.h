/*
 * OpenScope 2C53T - the soft power latch
 *
 * The supply stays on while PC9 is high or the POWER key (PC8, active low) is
 * down. Any chip reset returns PC9 to an input, which releases the latch.
 *
 * These are inline register stores on purpose: boot uses them first thing, and
 * a fault handler uses them with nothing else trusted. The firmware installer
 * (fw_loader.c) runs from RAM while this code is being erased, so it keeps its
 * own store to the same pin.
 */
#ifndef POWER_H
#define POWER_H

#include <stdbool.h>
#include "at32f403a_407.h"

#define POWER_HOLD_PIN_MASK  (1u << 9)     /* PC9 */
#define POWER_KEY_PIN_MASK   (1u << 8)     /* PC8 */

static inline void power_hold_set(void)     { GPIOC->scr = POWER_HOLD_PIN_MASK; }

/* The unit goes off now, or when POWER is let go if it is still held. */
static inline void power_hold_release(void) { GPIOC->clr = POWER_HOLD_PIN_MASK; }

static inline bool power_key_down(void)     { return (GPIOC->idt & POWER_KEY_PIN_MASK) == 0u; }

#endif /* POWER_H */
