/*
 * OpenScope 2C53T - battery gauge logic (pure, host-tested)
 *
 * The gauge has two inputs: the cell voltage at PB1 and the external-power
 * line PC7 (low while USB power is present; unit #2, 2026-11-13). Measured
 * on unit #2 with the scope running:
 *   - the charger holds the cell at 4.185 V; it rests at about 4.13 V once
 *     the cable is out, so "full" is 4.12 V of resting voltage, not 4.20 V;
 *   - plugging the cable in lifts the reading by about 50 mV at once (the
 *     charge current through the cell's resistance). The gauge takes that
 *     step off while on external power, so the percentage does not jump.
 */
#ifndef BATTERY_GAUGE_H
#define BATTERY_GAUGE_H

#include <stdint.h>
#include <stdbool.h>

#define BATT_EXT_STEP_MV     50u   /* reading lift while on external power */
#define BATT_EXT_DEBOUNCE    3u    /* seconds before the symbol follows PC7 */
#define BATT_CRITICAL_MV     3300u
#define BATT_CRITICAL_COUNT  10u
#define BATT_AVG_SIZE        16u

typedef struct {
    uint16_t avg_buf[BATT_AVG_SIZE];   /* resting-voltage estimates, mV */
    uint8_t  avg_idx, avg_count;
    uint8_t  ext_count;                /* consecutive seconds PC7 disagreed with `external` */
    uint8_t  critical_count;
    bool     external;                 /* debounced: on external power */
    bool     critical;
    uint16_t raw_mv;                   /* last reading as measured */
    uint16_t rest_mv;                  /* averaged resting estimate */
    uint8_t  percent;
} batt_gauge_t;

/* Resting cell voltage to percent (unit #2 curve, 100 % at 4.12 V). */
uint8_t batt_gauge_percent(uint16_t rest_mv);

/* One reading per second: the measured cell voltage and PC7's meaning. */
void batt_gauge_step(batt_gauge_t *g, uint16_t raw_mv, bool ext_now);

#endif
