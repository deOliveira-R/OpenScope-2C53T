#ifndef CAN_SCOPE_H
#define CAN_SCOPE_H

/*
 * CAN bus analyser on the live scope: CH1 = CAN_H, CH2 = CAN_L (either way
 * round). Glue between the acquisition buffers and src/decode/can_bus.c.
 *
 * Memory: the static RAM is full, so the working buffers (difference signal,
 * decode result, statistics: ~5.6 KB) come from the FreeRTOS heap, allocated
 * on first use and freed when the screen readout is switched off.
 *
 * Timebase: decoding needs >= 2 samples per bit. On a 500 kbit/s bus:
 *   0x08  5 MS/s    10 samples/bit  waveform detail; an 8-byte frame does not fit
 *   0x09  2.5 MS/s   5 samples/bit  frame up to its CRC
 *   0x0A  1.25 MS/s  2.5 samples/bit whole frame incl. ACK (best for decoding)
 * The 2C53T records 512 samples after the trigger, so the frame must fit there.
 */

#include <stdbool.h>
#include <stdint.h>
#include "can_bus.h"

/* Sample rate (S/s) for a reg-0x01 code: OpenScope's measured table, else the
 * rates measured against a 500 kbit/s bus for the fast codes; 0 if unknown. */
float can_scope_sample_rate(uint8_t code);

/* Analyse the current record (one coherent CH1+CH2 capture). Returns false on
 * no memory or no capture. *gen gets the record's generation. */
bool can_scope_capture(can_bus_report_t *rep, const decode_result_t **res, uint32_t *gen);

/* Screen readout (badge row 2). */
void can_scope_screen_set(bool on);
bool can_scope_screen_on(void);
/* Analyse the newest record if it is new, add it to the running statistics,
 * and format the one-line readout. Returns 1 when the bus looks healthy. */
int  can_scope_screen_line(char *out, uint32_t len);

/* Statistics shared by the screen and `can stats`. */
can_bus_stats_t *can_scope_stats(void);

#endif /* CAN_SCOPE_H */
