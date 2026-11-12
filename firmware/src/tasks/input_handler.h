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

/*
 * Handle a debounced button press.
 *
 * Reads the current device mode, scope state, and settings state
 * to determine the appropriate action. Sends display commands
 * via the provided queue.
 *
 * Returns the display command sent (or 0 if none).
 */
uint8_t input_handle_button(button_id_t button, QueueHandle_t display_queue);

/* Auto power-off timer (parity C15): call every pass of the input task. */
void input_autooff_poll(void);
/* Ask the input task to write pending settings now (shell-side changes).
 * Asynchronous; the shell dispatcher calls it after every command, so a
 * persisted setting changed by ANY command is captured without a key press. */
void input_request_settings_flush(void);
/* Same, but wait (<= timeout_ms) for the input task to act and return what the
 * write really did. Returns false if the input task did not answer in time. */
bool input_settings_flush_sync(uint32_t timeout_ms, settings_flush_outcome_t *out);

/* What the scope time view's arrow keys adjust (short SELECT cycles it). */
extern volatile key_focus_t scope_key_focus;

#ifdef FEATURE_FFT
/*
 * Switch the scope's view (time / FFT / split / waterfall / X-Y) with the FFT
 * pool lifecycle: the pool is claimed on leaving the time-domain view and
 * released on returning to it. Short PRM used to cycle the views through
 * this; since UI round 1 the views are chosen on the Oscilloscope Settings
 * page (CH panes "FFT: ON/OFF", Display), which calls this. Input task only.
 */
void scope_view_select(scope_view_t v);
#endif

/*
 * Handle OK press in settings menu (top-level and sub-menus).
 * Called internally by input_handle_button, but exposed for testing.
 */
void input_handle_settings_ok(void);

#endif /* INPUT_HANDLER_H */
