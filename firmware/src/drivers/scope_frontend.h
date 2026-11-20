/*
 * OpenScope 2C53T - scope front end (scope_frontend.c)
 *
 * What sits between the two BNC inputs and the converters: the range relays
 * of each channel, the AC/DC coupling relays and the routing of the channels
 * to the two converters. All of it is MCU pins, not the FPGA.
 */
#ifndef SCOPE_FRONTEND_H
#define SCOPE_FRONTEND_H

#include <stdint.h>
#include <stdbool.h>

/* Set channel ch's (1 or 2) range relays for volts/div index idx. The caller
 * updates the stored setting. false = bad argument, relays untouched. */
bool    fpga_apply_vdiv(uint8_t ch, uint8_t idx);

/* Drive one channel's AC/DC coupling relay (PD12 CH1, PD13 CH2, high = DC)
 * and read it back. false = not applied; the caller must not relabel. */
bool    fpga_apply_coupling(uint8_t ch, uint8_t coupling);   /* a coupling_t */

/* Which channels reach the converters: a 2-bit code on PC1/PC2 (1 = CH1,
 * 2 = CH2, 3 = both). With one channel routed, both converters sample it. */
void    fpga_set_channel_mask(uint8_t mask);
/* The same from the stored channel enables; false if both channels are off. */
bool    fpga_apply_channel_enables(void);

/* Apply the stored ranges and coupling. main() calls it at boot once the
 * settings are loaded. A stored GND coupling becomes DC. */
void    fpga_reconcile_frontend_after_arm(void);

/* For calibration from the shell: set the range relays for index range_idx
 * (clamped to VDIV_COUNT-1) on both channels, or on one (ch 0 = CH1,
 * 1 = CH2, anything else = both). */
void fpga_scope_set_range_diag(uint8_t range_idx);
void fpga_scope_set_range_diag_ch(uint8_t ch, uint8_t range_idx);

/* Moves each time the scope's front-end settings are put back after the
 * meter held the front end (meter_link.h, fpga_meter_holds_fe). The model
 * watches it to re-send the scope registers stored meanwhile. Read only. */
extern volatile uint8_t fpga_scope_posture_gen;

#endif /* SCOPE_FRONTEND_H */
