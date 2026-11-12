/*
 * OpenScope 2C53T - which of stock's 12 measurements each channel shows
 * (UI round 1, R1-A; stock's "Parameter Measurement" page)
 *
 * A bitmask per channel: bit i = scope_prm_id_t i (scope_measure.h: Freq,
 * Peri, Duty+, Duty-, Wid+, Wid-, Vmax, Vmin, Vp-p, Amp, Vrms, Avg).
 *
 * Defaults are what the instrument showed before the page existed: CH1 the
 * five badges of SCOPE_PRM_DEFAULT_MASK (Freq, Peri, Duty+, Vp-p, Vrms), CH2
 * its one "CH2pp" badge (Vp-p). config_init_defaults() carries the same pair.
 *
 * STORAGE. CH1's mask IS scope_state_t.prm_mask (the field the C6 badges, the
 * redraw epoch in main.c and the shell's `fpga scope prm` already use), so
 * there is one copy and nothing to keep in sync. CH2's has no field there and
 * lives here. Persisted through the settings record (config.h v5,
 * settings_store.c). Pure: no LCD, no FPGA.
 */
#ifndef MEAS_SELECT_H
#define MEAS_SELECT_H

#include <stdint.h>
#include "scope_measure.h"

#define MEAS_SELECT_CH1_DEFAULT  ((uint16_t)SCOPE_PRM_DEFAULT_MASK)
#define MEAS_SELECT_CH2_DEFAULT  ((uint16_t)(1u << SCOPE_PRM_VPP))

uint16_t meas_select_get(uint8_t ch);        /* bit i = measurement i shown, ch 0/1 */
void     meas_select_set(uint8_t ch, uint16_t mask);   /* bits above the 12 dropped; ch > 1 ignored */
void     meas_select_toggle(uint8_t ch, uint8_t prm);  /* flips one; prm >= 12 ignored */
unsigned meas_select_count(uint8_t ch);      /* how many are on */
void     meas_select_defaults(void);         /* both channels back to the defaults above */

#endif /* MEAS_SELECT_H */
