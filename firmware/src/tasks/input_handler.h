/*
 * OpenScope 2C53T - Input Handler
 *
 * Processes debounced button presses and dispatches actions
 * based on current device mode. Extracted from main.c to keep
 * the input task thin and the button logic testable.
 */

#ifndef INPUT_HANDLER_H
#define INPUT_HANDLER_H

#include "ui.h"
#include "key_model.h"
#include "FreeRTOS.h"
#include "queue.h"
#include <stdbool.h>
#include "settings_store.h"

/* Handle a debounced key (it may carry BTN_LONG_FLAG): MENU + POWER resets
 * into the bootloader; everything else goes to the screen router
 * (ui/ui_router.h). Input task. */
void input_handle_button(button_id_t button, QueueHandle_t display_queue);

/* Auto power-off timer (parity C15): call every pass of the input task. */
void input_autooff_poll(void);
/* Ask the input task to write pending settings now (shell-side changes).
 * Asynchronous; the shell dispatcher calls it after every command, so a
 * persisted setting changed by ANY command is captured without a key press. */
void input_request_settings_flush(void);
/* Same, but wait (<= timeout_ms) for the input task to act and return what the
 * write really did. Returns false if the input task did not answer in time. */
bool input_settings_flush_sync(uint32_t timeout_ms, settings_flush_outcome_t *out);

/* "Reset to defaults" from the shell (`reset settings CONFIRM`): ask the
 * input task to run settings_store_reset_to_defaults() and wait
 * (<= timeout_ms) for its result. true: it ran, *out is what it did. false:
 * no answer in time; the request is withdrawn and will not run, unless
 * *in_progress says the reset had already started (it then finishes). */
bool input_settings_reset_sync(uint32_t timeout_ms, settings_reset_result_t *out, bool *in_progress);

/* Restart the auto-off timer with a new period (0 = off). Input task only:
 * the sys.autooff setter's hardware op (model_hw_fw.c). */
void input_autooff_apply(uint8_t mins);

/* True while the input task is inside a key's handler. The handler posts
 * its own repaint when it returns; a setter it called may block meanwhile
 * (an acquisition park), and the display task, which then sees the change
 * first, must not paint the whole screen for it a second time (main.c
 * scope_view_tick: another kind of view). Read by the display task. */
extern volatile bool input_key_in_flight;

/* Run fn(arg) on the input task (the owner of the screen stack and of every
 * setter) and wait: for a shell handler that needs a step made there
 * (util/task_call.h). One caller at a time (the shell task). true: it ran.
 * false: the input task did not take it up in timeout_ms (a gallery grab,
 * the POWER countdown, a long setter) and it will not run. */
bool input_call_sync(void (*fn)(void *), void *arg, uint32_t timeout_ms);
/* Input task, every pass: the call that is waiting, if any. */
void input_call_serve(void);
/* Post a key into the input queue (main.c; BTN_NONE = a wake, no key). */
bool input_inject_button(button_id_t b);

/* scope_view_select() (the view with the FFT pool lifecycle) moved to
 * scope_apply.h at the UI round 1 merge, with the other shared glue. */
#include "scope_apply.h"

#endif /* INPUT_HANDLER_H */
