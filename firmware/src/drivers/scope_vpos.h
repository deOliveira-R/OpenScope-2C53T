/*
 * scope_vpos.h — per-channel vertical position and the offset DACs, driven
 * from this unit's stock calibration page (parity item C4).
 *
 * The decode/validation/formula is src/util/calpage.c (pure, host-tested).
 * This is the device half: it copies the live page at 0x08006000 into RAM
 * once at boot (read-only, like cal_backup.c), hands the per-channel gain to
 * scope_cal, and writes the offset references — DAC1 (PA4) for CH1 and the
 * TMR13 C1DT PWM-DAC (PA6) for CH2, the same two outputs stock drives at
 * 0x08008A48 and 0x08008C5A — from the bank, range and position in force.
 *
 * Every input of stock's formula changes the code, so it is re-applied on
 * each of them: the range (fpga_apply_vdiv), the timebase and the channel
 * mask (they pick the bank: fpga_apply_timebase, fpga_apply_channel_enables)
 * and the position (scope_vpos_set). The dry-spell recovery in the
 * acquisition task re-applies too, instead of dropping CH1 to mid-scale.
 *
 * A unit without a usable page (or `cal page off`) keeps the behaviour from
 * before this existed: fixed arm DAC1 2048 / TMR13 2544 and the `fpga scope
 * center` servo. Nothing unit-specific is compiled in.
 *
 * Position is stock's: int, -100..+100 per channel, the same units as stock's
 * ms[4]/ms[5]. pos = -100 puts the base endpoint on the DAC, +100 the upper
 * one; on a quiet input the record sits at about raw 128 + pos (stock's
 * zero reference, ADDENDUM_A S7) — 100 + pos in OpenScope's records, which
 * subtract FPGA_ADC_OFFSET (28) from every raw byte.
 */
#ifndef SCOPE_VPOS_H
#define SCOPE_VPOS_H

#include <stdint.h>
#include <stdbool.h>

#include "calpage.h"

/* Read and decode the live page, publish the gains, apply both channels.
 * Call once at boot after settings_store_init() and the frontend reconcile
 * (range and timebase are then the restored ones). Read-only on flash. */
void scope_vpos_init(void);

/* Page decoded and usable, and not switched off from the shell. */
bool scope_vpos_active(void);

/* `cal page on|off`. Off restores the pre-C4 behaviour (compiled gain table,
 * fixed DAC arm) without touching the page. On does nothing on an unusable
 * page and returns false. */
bool scope_vpos_enable(bool on);

/* Decode status of the live page (NONE before init). The full decode is not
 * kept in RAM; the shell re-parses the page for its table view. */
calpage_status_t scope_vpos_status(void);

/* Re-apply both channels from the current state. No-op when inactive. */
void scope_vpos_apply(void);

/* Re-apply one channel (1 or 2) for `range`, which callers pass explicitly
 * because fpga_apply_vdiv() runs before the caller stores vdiv_idx. */
void scope_vpos_apply_ch(uint8_t ch, uint8_t range);

/* Set a channel's vertical position (clamped to +-100) and apply it.
 * Returns false for a bad channel. The setter for the UI keys. */
bool scope_vpos_set(uint8_t ch, int pos);
int  scope_vpos_get(uint8_t ch);

/* Last write made by this module for ch (1/2): bank, code and whether it came
 * from the page. False if nothing was written yet. */
bool scope_vpos_last(uint8_t ch, uint8_t *bank, uint8_t *range, uint16_t *code);

/* CH1 offset-DAC endpoints (pos -100 and +100) for the bank and range in
 * force, from the page. False when inactive or no clean pair exists. For
 * the 250 MS/s self-calibration sweep (C2). A pure page read: the generator
 * (DAC channel 2, PA5) has no say in it. */
bool scope_vpos_ch1_endpoints(uint16_t *base, uint16_t *upper);

/* Write a raw CH1 offset-DAC code (self-calibration sweep); undo with
 * scope_vpos_apply_ch(1, range) or scope_vpos_apply(). False when the page
 * is inactive or the code is out of range. A running generator does not
 * refuse it: it is on DAC channel 2, and the DAC owner (dac.c) keeps the
 * two channels' registers apart. */
bool scope_vpos_ch1_raw(uint16_t code);

/* Acquisition dry-spell recovery (fpga.c): re-apply the calibrated codes, or
 * the legacy DAC1 mid-scale arm when inactive. */
void scope_vpos_recover(void);

/* ── Baseline correction (parity C10, src/drivers/scope_selfcal_run.c) ──
 * RAM-only deltas on the page's endpoint pair per (ch 1/2, bank, range),
 * applied by every write above. Nothing here touches flash; persistence
 * (on request only) is src/util/usercal.c. */

/* The page pair (after substitution, before any correction) the offset path
 * uses for (ch, bank, range). False when inactive or no bank is clean. */
bool scope_vpos_page_pair(uint8_t ch, uint8_t bank, uint8_t range,
                          uint16_t *base, uint16_t *upper);
/* Store a correction; refused (false) when it would leave 0..4095. The
 * caller re-applies (scope_vpos_apply) when it is done storing. */
bool scope_vpos_corr_set(uint8_t ch, uint8_t bank, uint8_t range, int16_t d_base, int16_t d_upper);
bool scope_vpos_corr_get(uint8_t ch, uint8_t bank, uint8_t range, int16_t *d_base, int16_t *d_upper);
void scope_vpos_corr_clear(void);   /* and re-apply */
unsigned scope_vpos_corr_count(void);

/* The servo's hold on a channel's reference: while on, every apply writes
 * `code` (still never DAC1 while the generator owns it). Off re-applies the
 * page code. */
void scope_vpos_override(uint8_t ch, bool on, uint16_t code);

#endif /* SCOPE_VPOS_H */
