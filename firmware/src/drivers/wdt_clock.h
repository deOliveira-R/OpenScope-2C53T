/*
 * OpenScope 2C53T - measure the watchdog's own clock (LICK, nominally 40 kHz).
 *
 * The chip routes LICK to TMR5 channel 4 for this purpose. The timer runs from
 * the crystal, so the period it captures gives the real watchdog timeout of
 * this unit. Nothing here starts the watchdog.
 */
#ifndef WDT_CLOCK_H
#define WDT_CLOCK_H

#include <stdint.h>

/* The measured LICK rate in Hz, or 0 if it could not be measured. Blocks for
 * about a second; call from a task. */
uint32_t wdt_clock_measure_hz(void);

#endif /* WDT_CLOCK_H */
