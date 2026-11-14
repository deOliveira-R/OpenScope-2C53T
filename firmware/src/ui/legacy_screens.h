/*
 * OpenScope 2C53T - legacy screens: the screen router (screen.h) wired over
 * the pre-router key handlers (ARCH-1 B1)
 *
 * One screen per device mode (the bases), one per Settings page (children,
 * mirroring settings_depth), the Oscilloscope Settings page and the gallery
 * (layers). Each key callback forwards to the handler input_handler.c ran for
 * that key before the router existed, in the same order, so every key keeps
 * its meaning; the router only decides who is asked first:
 *
 *   gallery (GRAB) -> self-cal lock -> globals (key_map_global) ->
 *   osc page (OPAQUE) -> settings page / base
 *
 * LEGACY STATE IS AUTHORITATIVE. current_mode, settings_depth,
 * osc_menu_is_open() and gallery_ui_is_open() still decide what is up (the
 * shell writes current_mode and closes the gallery from its own task), and
 * the router's stack MIRRORS them: legacy_screens_sync() matches the stack to
 * them on the input task, before and after every key and on every pass of the
 * input loop. The screens' open()/close() are therefore empty: entering a mode
 * is still enter_mode() (input_handler.c), which posts the mode's hardware
 * posture and so applies that instrument's stored state (model_hw_fw.c's
 * posture table). The long mode keys reach it as a sys.mode set (lgc_mode_set),
 * the shell's `set sys.mode` / `set sys.instrument` path. sys.instrument reads current_mode, so it and the router's
 * base are the same value one sync apart.
 *
 * MIGRATED SCREENS (ARCH-1 B2 on). A base that is a real screen (it has its
 * own file and a draw(): home_screen.c) is registered here in place of its
 * legacy row. The mirror above still decides when it is up; what changes is
 * who handles its keys and paints it: the screen's key() acts on the model
 * and answers what to repaint, and this file posts that paint and runs the
 * settings hook once per key (lgc_native_after), where each legacy handler
 * does both itself. Its open()/close() run from the mirror's screen_switch.
 *
 * This file is pure (no FreeRTOS, no driver header): the router order is
 * host-tested against the pre-router dispatch (tests/test_screen_router.c).
 * Its environment is the lgc_* functions below, implemented by
 * input_handler.c in the firmware and by fakes in the host test.
 */
#ifndef LEGACY_SCREENS_H
#define LEGACY_SCREENS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ui.h"
#include "key_model.h"
#include "screen.h"

/* ── the router (input task) ───────────────────────────────────────── */

/* Register the screens and hooks and start the stack on the current mode. */
void    legacy_screens_init(void);
/* Match the stack to the legacy state (input task only). Cheap when equal. */
void    legacy_screens_sync(void);
/* DCMD_DRAW_ACTIVE for the screen on top now (DCMD_DRAW_ACTIVE + its id).
 * Input task, when a handler repaints the screen it is on. */
uint8_t legacy_screens_active_cmd(void);
/* The legacy draw command that repaints screen `top` in place (its
 * DCMD_DRAW_<mode>, or DCMD_DRAW_OSC_MENU); DCMD_NONE_MODAL_DROP for a
 * screen with no in-place paint (the gallery) or an id out of range. */
uint8_t legacy_screens_update_cmd(screen_id_t top);
/* Display task: a received command; a DCMD_DRAW_ACTIVE + id becomes
 * legacy_screens_update_cmd(id), anything else is returned as it is. */
uint8_t legacy_screens_resolve_cmd(uint8_t cmd);
/* Settings page shown at settings_depth d (SCR_SETTINGS for 0). */
screen_id_t legacy_settings_page(int8_t depth);

/* ── the environment (input_handler.c; fakes in the host test) ─────── */

typedef struct {
    device_mode_t mode;           /* current_mode                          */
    int8_t        settings_depth;
    bool          osc_open;       /* osc_menu_is_open(), any mode          */
    bool          gallery_open;   /* gallery_ui_is_open()                  */
} lgc_state_t;

void      lgc_state(lgc_state_t *s);
key_ctx_t lgc_ctx(void);                          /* key_ctx_now()                  */
void      lgc_exec(key_cmd_t k, button_id_t btn); /* exec_key_cmd + settings hook   */
bool      lgc_meter_softkey(button_id_t btn);     /* true: a softkey took it        */
bool      lgc_siggen_edit(button_id_t btn);       /* true: the digit editor took it */
void      lgc_mode_key(button_id_t btn);          /* the per-key switch (+ POWER)   */
/* The long mode keys: set sys.mode through the model (prop_set_local,
 * ORIGIN_KEY), the path `set sys.mode` / `set sys.instrument` take, then the
 * settings hook as for every key. */
void      lgc_mode_set(device_mode_t m);
bool      lgc_gallery_key(uint8_t raw);           /* gallery_ui_key; false: not consumed
                                                   * (it was closed, or POWER)      */
bool      lgc_selfcal_busy(void);
void      lgc_selfcal_cancel(void);               /* AUTO / MENU while locked       */
void      lgc_selfcal_refuse(void);               /* "Busy: AUTO cancels"           */
void      lgc_touch(void);                        /* auto-off restart               */
void      lgc_click(void);                        /* key click                      */
/* After a key a migrated screen handled: `res` is its answer. SCR_DRAW posts
 * an in-place paint of screen `scr` (DCMD_DRAW_ACTIVE + scr), SCR_DRAW_FULL
 * a DCMD_REDRAW_ALL, SCR_HANDLED nothing (the key changed nothing on screen,
 * or it switched the mode and the switch repaints); then the settings hook,
 * as for every key. */
void      lgc_native_after(screen_id_t scr, scr_res_t res);
void      lgc_lock(void);                         /* guards the stack arrays        */
void      lgc_unlock(void);

/* `ui screen` describe hooks (src/model/ui_shell.c; fakes in the host test):
 * one key=value fragment each. */
void ui_describe_osc_menu(char *out, size_t n);   /* "row=ch1 pane=pane item=2" */
void ui_describe_settings(char *out, size_t n);   /* "settings=1/0/3"           */

#endif /* LEGACY_SCREENS_H */
