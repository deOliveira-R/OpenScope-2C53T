/*
 * OpenScope 2C53T - the signal generator screen as a screen of the router
 * (screen.h; ARCH-1 B3), on the gen.* properties
 *
 * signal_gen.c owns the generator (its state, TMR7/DMA2/DAC2 through
 * dac_output.c); the gen.* properties are its setters. This screen is the
 * view: every key that changes the generator is one property set on the
 * input task (prop_set_local / prop_step_local), so a key edit is what
 * `get gen.*` answers at once, and a `set gen.*` from the shell while the
 * screen is shown is what the screen paints (the set posts the repaint; the
 * editor steps from the live value at its next key).
 *
 *   key                    editor closed            editor open
 *   SELECT                 gen.wave +1 (wraps)      the same
 *   UP / DOWN              gen.amp +/- 0.1 V        gen.freq +/- 10^digit
 *   LEFT / RIGHT           gen.duty -/+ 5 % (square and sawtooth only)
 *                                                   digit cursor
 *   OK                     gen.out toggle           close, keep the value
 *   PRM                    open the editor          close, keep the value
 *   MENU                   Home (sys.mode)          close, gen.freq = the
 *                                                   value the edit began with
 *   long OK                gen.out toggle (Run/Stop, manual 2.1)
 *   long AUTO              gen.bootout toggle (output at boot, ours)
 *   CH1 CH2 TRIGGER MOVE   nothing but a full repaint (as before B3)
 *   AUTO, other long keys  nothing
 *   (SAVE, POWER and the long mode keys are the router's globals.)
 *
 * The screen keeps no generator state: the editor's cursor (active, digit)
 * and its undo value are UI state; siggen_edit_t.value is loaded from
 * gen.freq before every key. draw() and epoch() read the generator through
 * its owner and the properties, on the display task, and set nothing.
 *
 * Pure: no FreeRTOS, no driver header. Host test: make test-siggen-screen.
 */
#ifndef SIGGEN_SCREEN_H
#define SIGGEN_SCREEN_H

#include <stdint.h>
#include "screen.h"

extern const screen_t siggen_screen;

/* siggen_ui.c (display task): the whole page (the preview, the parameters,
 * the digit field while the editor is open). */
void siggen_ui_draw(void);

#endif /* SIGGEN_SCREEN_H */
