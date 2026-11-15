/*
 * OpenScope 2C53T - the firmware's screens on the router, and the keys that
 * belong to no screen (ARCH-1 B8)
 *
 * screen.c is the router: the stack, and the order a key is offered in.
 * This file is what the firmware puts on it:
 *
 *   - the screen table: every base (home, scope, meter, generator,
 *     settings), the Settings pages and the layers (the Oscilloscope
 *     Settings page, the gallery, the logo, the POWER prompt), each from its
 *     own file;
 *   - the self-cal lock (router step 3): while autoset or the baseline
 *     correction runs, AUTO and MENU cancel it and every other key but POWER
 *     says "Busy: AUTO cancels";
 *   - THE GLOBAL KEYS (router step 4; key_model.h key_map_global says which
 *     they are), handled here before any screen is asked, from every screen
 *     and over every layer but a grabbing one:
 *       long MENU / MOVE / SELECT / TRIGGER   home / meter / scope / generator: one
 *                                             set of sys.mode (ORIGIN_KEY), the
 *                                             setter `set sys.mode` ends in
 *       long SAVE                             the gallery (its layer is pushed)
 *       short SAVE                            a screenshot (ui_env_screenshot)
 *       POWER                                 the gallery is closed, then the
 *                                             hold-to-power-off countdown
 *                                             (ui_env_power_key). POWER is never
 *                                             a layer's key: the router does not
 *                                             offer it to a grabbing layer.
 *     The gallery grabs the keyboard (SCR_F_GRAB), so with it up SAVE and the
 *     long mode keys are its own (long SAVE closes it); POWER still comes
 *     here.
 *   - after a key (router step 7): the repaint the screen that took it
 *     answered (SCR_DRAW: that screen in place; SCR_DRAW_FULL: everything),
 *     then the settings service.
 *
 * Which screen is shown is the router's stack and nothing else: no mirror,
 * no per-layer flags. The mode is switched by ui_router_switch_mode() only
 * (the sys.mode / sys.instrument op).
 *
 * Pure: no FreeRTOS, no driver header. Its environment is the ui_env_*
 * functions (tasks/input_handler.c in the firmware, fakes in the host
 * test). Host test: make test-screen-router (the real screens).
 */
#ifndef UI_ROUTER_H
#define UI_ROUTER_H

#include <stdbool.h>
#include <stdint.h>
#include "ui.h"
#include "screen.h"

/* Register the screens and the hooks and start the stack on the screen of
 * `boot_mode` (the scope if it has none). Input task, before its first key. */
void ui_router_init(device_mode_t boot_mode);

/* One key (raw may carry BTN_LONG_FLAG): routed, repainted, serviced. */
void ui_router_key(button_id_t raw);

/* The mode switch, for the sys.mode / sys.instrument op (input task). The
 * gallery is closed first, also when the mode is already the one asked for:
 * a mode change hands the screen to the mode. Then, if the mode differs,
 * every layer is closed top-down, the old base closed and the new one
 * opened (screen_switch: current_mode follows there). true: the base
 * changed, and the caller applies the mode's posture and repaints. */
bool ui_router_switch_mode(device_mode_t m);

/* ── the environment (tasks/input_handler.c; fakes in the host test) ── */
void ui_env_lock(void);                 /* guards the router's stack arrays              */
void ui_env_unlock(void);
void ui_env_touch(void);                /* every key: the auto power-off count restarts   */
void ui_env_click(void);                /* every key: the key click                       */
void ui_env_post(uint8_t dcmd);         /* a display command; never waits                 */
void ui_env_settings_service(void);     /* after a key: the settings store's turn         */
void ui_env_screenshot(void);           /* short SAVE: capture, its popup, its repaint    */
void ui_env_power_key(void);            /* POWER: the countdown, and the power-off itself */
/* The self-cal run (drivers/scope_selfcal_run.h). */
bool scope_selfcal_busy(void);
void scope_selfcal_request_cancel(void);

#endif /* UI_ROUTER_H */
