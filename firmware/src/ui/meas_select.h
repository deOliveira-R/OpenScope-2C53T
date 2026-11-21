/*
 * OpenScope 2C53T - which measurements the scope shows
 * (UI round 1, R1-A; stock's "Parameter Measurement" page)
 *
 * One bitmask per measurement set. A set is what can be measured on one
 * source: each channel's time record has stock's twelve (bit i =
 * scope_prm_id_t i, scope_measure.h: Freq, Peri, Duty+, Duty-, Wid+, Wid-,
 * Vmax, Vmin, Vp-p, Amp, Vrms, Avg), and the spectrum has its own (bit i =
 * fft_meas_id_t i, dsp/fft_live.h: Peak, Level, H2, H3).
 *
 * Defaults: both channels the five badges CH1 showed before the page existed,
 * SCOPE_PRM_DEFAULT_MASK (Freq, Peri, Duty+, Vp-p, Vrms); CH2 had only its
 * "CH2pp" badge (Vp-p) until the owner asked for CH1's set (2026-11-12).
 * config_init_defaults() carries the same pair; a saved record keeps its own.
 * The spectrum: its peak and the peak's level.
 *
 * STORAGE. CH1's mask IS scope_state_t.prm_mask (the field the C6 badges, the
 * redraw epoch in main.c and the shell's `fpga scope prm` already use), so
 * there is one copy and nothing to keep in sync. The others have no field
 * there and live here. The channels' are persisted through the settings
 * record (config.h v5, settings_store.c). Pure: no LCD, no FPGA.
 */
#ifndef MEAS_SELECT_H
#define MEAS_SELECT_H

#include <stdint.h>
#include "scope_measure.h"

typedef enum { MEAS_SET_CH1 = 0, MEAS_SET_CH2, MEAS_SET_FFT, MEAS_SET_COUNT } meas_set_t;

/* The spectrum's set, counted here so this file needs no DSP header;
 * scope_furniture.c asserts both against dsp/fft_live.h. */
#define MEAS_SELECT_FFT_COUNT    4u
#define MEAS_SELECT_FFT_DEFAULT  ((uint16_t)0x0003u)     /* Peak, Level */

#define MEAS_SELECT_CH1_DEFAULT  ((uint16_t)SCOPE_PRM_DEFAULT_MASK)
#define MEAS_SELECT_CH2_DEFAULT  ((uint16_t)SCOPE_PRM_DEFAULT_MASK)   /* = CH1's */

unsigned meas_select_size(uint8_t set);      /* measurements the set has; 0 for no such set */
/* The name of measurement id of a set, as the strip and the Measure pane
 * show it ("Freq", "Vp-p"; "Peak", "Level", "H2", "H3"); "?" out of range. */
const char *meas_select_label(uint8_t set, unsigned id);
uint16_t meas_select_all(uint8_t set);       /* the mask with every one of them on */
uint16_t meas_select_get(uint8_t set);       /* bit i = measurement i shown */
void     meas_select_set(uint8_t set, uint16_t mask);  /* bits above the set's dropped; no such set: ignored */
void     meas_select_toggle(uint8_t set, uint8_t id);  /* flips one; an id the set has not: ignored */
unsigned meas_select_count(uint8_t set);     /* how many are on */
void     meas_select_defaults(void);         /* every set back to the defaults above */

#endif /* MEAS_SELECT_H */
