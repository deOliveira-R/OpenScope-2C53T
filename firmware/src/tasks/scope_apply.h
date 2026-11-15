/*
 * OpenScope 2C53T - the hardware side of a scope setting change
 *
 * The helper library of the scope's property ops (model_hw_fw.c: the only
 * caller since ARCH-1 B7; every key and the shell reach it through a
 * property set): the scope view with the shared memory pool (FFT and
 * persistence take turns on it; X-Y borrows persistence's buffer for its
 * trail and is brought up here), and persistence on / off. The coupling
 * relay, a trigger change and Roll are in their setters (props_scope.c).
 *
 * Input task only: these write scope state the display task reads. Not host-tested (fpga_* calls); the decisions
 * they apply are (trig_menu.c, scope_state.c, osc_menu_model.c).
 */
#ifndef SCOPE_APPLY_H
#define SCOPE_APPLY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ui.h"            /* scope_view_t */
#include "persistence.h"   /* persist_mode_t */

#if FEATURE_FFT
/* The views that need the FFT's half of the shared pool (FFT, split,
 * waterfall). X-Y does not: its phosphor trail borrows persistence's. */
bool scope_view_uses_fft(scope_view_t v);

/* Switch the scope's view (time / FFT / split / waterfall / X-Y) with the
 * pool lifecycle: claimed on the way into a spectrum view (persistence, which
 * holds the same pool, is switched off first), released on the way to a view
 * that does not use it. The view is switched off before the pool goes, so the
 * display task never draws a spectrum from a released pool. Entering X-Y
 * claims the persistence buffer for its trail (cleared, short tail) and,
 * without the calibration page, brings the offset outputs up. */
void scope_view_select(scope_view_t v);
#endif

/* Persistence on in mode m (or just the mode, if it is on already). The pool
 * is shared: a spectrum view goes back to the time view first. False (and
 * persistence off) if the pool could not be had. */
bool scope_apply_persist_on(persist_mode_t m);
/* Persistence off; the pool is released. */
void scope_apply_persist_off(void);

#endif /* SCOPE_APPLY_H */
