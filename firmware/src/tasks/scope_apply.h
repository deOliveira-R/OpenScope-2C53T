/*
 * OpenScope 2C53T - the hardware side of a scope setting change
 *
 * One copy of the glue that the keys (input_handler.c: the trigger panel, the
 * legacy Settings list) and the Oscilloscope Settings page (osc_menu_ui.c)
 * both need, so the two can never disagree on what a change does: the
 * coupling relay with readback, a trigger change's register write / SINGLE
 * re-arm, Roll through the FPGA with revert, the scope view with the shared
 * memory pool (FFT and persistence take turns on it), and persistence on.
 * UI round 1 merge (each side had its own static copy before).
 *
 * Input task only: these write scope state the display task reads, as the
 * key handlers always have. Not host-tested (fpga_* calls); the decisions
 * they apply are (trig_menu.c, scope_state.c, osc_menu_model.c).
 */
#ifndef SCOPE_APPLY_H
#define SCOPE_APPLY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ui.h"            /* scope_view_t */
#include "persistence.h"   /* persist_mode_t */

/* CH1 (ch 0) / CH2 (ch 1): DC <-> AC on the relay. The label changes only if
 * the relay did (fpga_apply_coupling verifies by readback). GND is out of the
 * cycle: no hardware ground path is known. Writes "CH1 AC" or "CH1 AC NOT SET"
 * to pb; returns true if the relay switched. */
bool scope_apply_coupling_toggle(uint8_t ch, char *pb, size_t n);

/* After trig_menu_change() returned `what` (TM_* flags): reg 0x06 for the
 * source, the SINGLE arm for the mode (stock: selecting Single arms the
 * one-shot, "Await Trigger...", 0x0801C8C2). The edge is the MCU filter and
 * the soft trigger only, no register. Writes the popup text to pb ("" if
 * nothing changed). Returns true when the text is more than an echo of the
 * new value: a refusal, a NOT SET, or the SINGLE arm. */
bool scope_apply_trigger_change(uint8_t what, char *pb, size_t n);

/* Roll (C11) on/off through the FPGA. On refusal the old mode is kept and
 * false returned. True if the scope is now in the mode asked for. */
bool scope_apply_roll(bool want);

#ifdef FEATURE_FFT
/* The views that need the FFT's half of the shared pool (FFT, split,
 * waterfall). X-Y does not: its phosphor trail borrows persistence's. */
bool scope_view_uses_fft(scope_view_t v);

/* Switch the scope's view (time / FFT / split / waterfall / X-Y) with the
 * pool lifecycle: claimed on the way into a spectrum view (persistence, which
 * holds the same pool, is switched off first), released on the way to a view
 * that does not use it. The view is switched off before the pool goes, so the
 * display task never draws a spectrum from a released pool. */
void scope_view_select(scope_view_t v);
#endif

/* Persistence on in mode m (or just the mode, if it is on already). The pool
 * is shared: a spectrum view goes back to the time view first. False (and
 * persistence off) if the pool could not be had. */
bool scope_apply_persist_on(persist_mode_t m);
/* Persistence off; the pool is released. */
void scope_apply_persist_off(void);

#endif /* SCOPE_APPLY_H */
