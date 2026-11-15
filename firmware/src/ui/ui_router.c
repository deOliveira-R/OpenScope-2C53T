/*
 * OpenScope 2C53T - the firmware's screens on the router. See ui_router.h.
 *
 * The order a key is asked in (screen.c screen_route_key), with what this
 * file hooks in:
 *
 *   1. every key: auto-off restart, key click            ur_touch, ui_env_click
 *   2. a grabbing top layer (the gallery): every key
 *      but POWER
 *   3. self-cal running: AUTO/MENU cancel, the rest      ur_locked
 *      "Busy"; POWER passes
 *   4. the global keys                                   ur_globals
 *   5. the layers top-down, then the base                their own files
 *   7. the repaint the screen answered, the settings     ur_after_key
 *      service
 *
 * The Oscilloscope Settings page is OPAQUE but not GRAB: a grabbing layer is
 * asked before the self-cal lock, and the lock comes first there (a long
 * MOVE during autoset says "Busy", it does not switch the mode).
 */
#include "ui_router.h"
#include "key_model.h"
#include "home_screen.h"
#include "settings_screen.h"
#include "siggen_screen.h"
#include "meter_screen.h"
#include "scope_screen.h"
#include "osc_menu_screen.h"
#include "gallery_screen.h"
#include "overlay_screens.h"
#include "model.h"

_Static_assert((unsigned)SCR_COUNT <= (unsigned)(DCMD_DRAW_ACTIVE_END - DCMD_DRAW_ACTIVE),
               "every screen id fits the DCMD_DRAW_ACTIVE range");
_Static_assert(MODE_OSCILLOSCOPE == 0 && MODE_MULTIMETER == 1 && MODE_SIGNAL_GEN == 2,
               "a base screen's instrument id is its mode value (sys.instrument 0..2)");

/* Per key (input task): set by the lock, which never ran the settings
 * service (a run notes its own changes when it ends). */
static bool ur_no_service;

/* ── step 1 ────────────────────────────────────────────────────────── */

static void ur_touch(void)
{
    ur_no_service = false;
    ui_env_touch();
}

/* ── step 3: the self-cal lock ─────────────────────────────────────── */

/* While autoset or the baseline correction runs the keys are locked, as
 * stock locks them while ms[0x2F]/[0x30] are set (0x08040074): a range or
 * timebase press would pull the front end out from under the servo. AUTO
 * and MENU (short or long) cancel and restore; POWER still works (the
 * router never asks about it). */
static bool ur_locked(button_id_t b, bool is_long)
{
    (void)is_long;
    if (!scope_selfcal_busy()) return false;
    ur_no_service = true;
    if (b == BTN_AUTO || b == BTN_MENU) {
        scope_selfcal_request_cancel();
    } else {
        scope_show_popup("Busy: AUTO cancels");
        ui_env_post(DCMD_REDRAW_ALL);
    }
    return true;
}

/* ── step 4: the global keys ───────────────────────────────────────── */

/* The long mode keys' target, or MODE_COUNT for any other action. */
static device_mode_t ur_mode_of(key_action_t a)
{
    switch (a) {
    case KA_ENTER_HOME:   return MODE_HOME;
    case KA_ENTER_METER:  return MODE_MULTIMETER;
    case KA_ENTER_SCOPE:  return MODE_OSCILLOSCOPE;
    case KA_ENTER_SIGGEN: return MODE_SIGNAL_GEN;
    default:              return MODE_COUNT;
    }
}

static bool ur_globals(button_id_t b, bool is_long)
{
    /* Before any key acts: an arrow stage of the scope that no longer
     * exists (the cursors switched off or the mask cleared from the shell)
     * is dropped, as the key dispatch always did for every key past the
     * lock. */
    (void)scope_screen_ctx();

    const key_cmd_t g = key_map_global(NULL, b, is_long);
    const device_mode_t m = ur_mode_of(g.act);
    if (m != MODE_COUNT) {
        /* Long MENU / MOVE / SELECT / TRIGGER: a sys.mode set, as the
         * shell's; the setter switches, applies the posture and repaints
         * (nothing if it is the mode already). */
        (void)prop_set_local(P(SYS_MODE), (int32_t)m, ORIGIN_KEY, NULL);
        return true;
    }
    switch (g.act) {
    case KA_GALLERY_OPEN:
        /* Long SAVE (parity C16): stock's 3x3 screenshot gallery, from
         * every screen (D1). Its layer owns the LCD and every key but POWER
         * until it closes; the display task then repaints what was under
         * it. */
        gallery_screen_open();
        return true;
    case KA_SCREENSHOT:
        ui_env_screenshot();
        return true;
    case KA_POWER_PROMPT:
        /* POWER is never a layer's key: the gallery is closed first, so
         * power-off always works and a release shows the screen that was
         * under the gallery. */
        (void)screen_close(SCR_GALLERY);
        ui_env_power_key();
        return true;
    default:
        return false;           /* KA_LEGACY: a screen's key */
    }
}

/* ── step 7 ────────────────────────────────────────────────────────── */

/* The screen is the one whose key() took the key (the router knows), not
 * the one on top now: the key may have switched the mode or closed the
 * layer. A global or the lock: nothing to post here (they posted their
 * own, or the display task sees the layer). */
static void ur_after_key(scr_res_t res)
{
    const screen_id_t who = screen_last_handler();
    if ((unsigned)who < (unsigned)SCR_COUNT) {
        if (res == SCR_DRAW_FULL)  ui_env_post(DCMD_REDRAW_ALL);
        else if (res == SCR_DRAW)  ui_env_post((uint8_t)(DCMD_DRAW_ACTIVE + (uint8_t)who));
    }
    if (!ur_no_service) ui_env_settings_service();
}

static const screen_hooks_t ur_hooks = {
    .lock = ui_env_lock, .unlock = ui_env_unlock, .touch = ur_touch, .click = ui_env_click,
    .locked = ur_locked, .globals = ur_globals, .after_key = ur_after_key, .stack_changed = NULL,
};

/* ── the table ─────────────────────────────────────────────────────── */

static const screen_t *const ur_screens[] = {
    &home_screen, &scope_screen, &meter_screen, &siggen_screen,
    &settings_screen, &settings_sound_screen, &settings_about_screen,
    &osc_menu_screen, &gallery_screen, &logo_screen, &power_prompt_screen,
};
_Static_assert(sizeof ur_screens / sizeof ur_screens[0] == SCR_COUNT, "one screen per id");

void ui_router_init(device_mode_t boot_mode)
{
    screen_reset();
    for (unsigned i = 0; i < sizeof ur_screens / sizeof ur_screens[0]; i++)
        (void)screen_register(ur_screens[i]);
    screen_set_hooks(&ur_hooks);
    const screen_id_t base = screen_base_for_mode(boot_mode);
    (void)screen_init(base != SCR_COUNT ? base : SCR_SCOPE);
}

void ui_router_key(button_id_t raw)
{
    (void)screen_route_key(raw);
}

bool ui_router_switch_mode(device_mode_t m)
{
    const screen_id_t base = screen_base_for_mode(m);
    if (base == SCR_COUNT) return false;
    (void)screen_close(SCR_GALLERY);
    if (screen_base() == base) return false;
    screen_switch(base);
    return true;
}
