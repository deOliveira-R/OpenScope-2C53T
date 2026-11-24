/*
 * OpenScope 2C53T - Battery Voltage Monitor
 * PB1 / ADC1 Channel 9, 239.5-cycle sample time
 */

#ifndef BATTERY_H
#define BATTERY_H

#include <stdint.h>
#include <stdbool.h>

/* Initialize ADC1 for battery voltage reading on PB1 */
void battery_adc_init(void);

/* Call once per second to take a reading and update state */
void battery_update(void);

/* Averaged estimate of the resting cell voltage in millivolts (cached; the
 * charging step is taken off while on external power) */
uint16_t battery_read_mv(void);

/* Last cell voltage as measured, millivolts (cached) */
uint16_t battery_raw_mv(void);

/* Get battery percentage estimate (0-100) */
uint8_t battery_percent(void);

/* Returns 1 while on external (USB) power: PC7 low, debounced 3 s */
uint8_t battery_is_charging(void);

/* Returns 1 if voltage critically low (< 3.3V) */
uint8_t battery_is_critical(void);

/* Critical for long enough: the input task must power off (any task may ask). */
bool battery_shutdown_due(void);

/* Bench: gauge a cell below the critical level, on battery, until turned off. */
void battery_test_low(bool on);

#endif
