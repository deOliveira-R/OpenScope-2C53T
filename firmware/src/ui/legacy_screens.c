/*
 * OpenScope 2C53T - legacy screens. See legacy_screens.h.
 *
 * The order the pre-router input_handle_button() asked its handlers in, and
 * where each step lives now:
 *
 *   1. auto-off restart, key click              router step 1 (touch/click hooks)
 *   2. gallery open: it takes every key but     SCR_GALLERY (gallery_screen.c, a
 *      POWER, which it hands back closed         migrated screen), GRAB; POWER:
 *                                               the POWER global closes it first
 *   3. self-cal running: AUTO/MENU cancel,      router step 3 (locked hook)
 *      the rest "Busy", POWER passes
 *   4. long keys: key_map(long)                 globals (key_map_global; the
 *                                               mode keys as a sys.mode set),
 *                                               then the top screen's key_map(long)
 *   5. short keys key_map claims (SAVE; the     globals (SAVE), then the osc page
 *      scope's keys and its Settings page)      (SCR_OSC_MENU, osc_menu_screen.c,
 *                                               a migrated screen: its keys of
 *                                               steps 4 and 5) / SCR_SCOPE
 *                                               (scope_screen.c, a migrated
 *                                               screen: the plot's keys of
 *                                               steps 4, 5 and 9)
 *   6. home page: every key but POWER           SCR_HOME (home_screen.c, a
 *                                               migrated screen: see below)
 *   7. meter softkeys (MOVE/SELECT/TRIGGER/PRM) SCR_METER (meter_screen.c, a
 *                                               migrated screen: the softkeys
 *                                               and its keys of step 9)
 *   8. generator digit editor                   SCR_SIGGEN (siggen_screen.c, a
 *                                               migrated screen: its long OK /
 *                                               long AUTO of step 4, the editor
 *                                               and its keys of step 9)
 *   9. the per-key switch (POWER's countdown)   every base (POWER: the global;
 *                                               the countdown's box is a layer,
 *                                               SCR_POWER_PROMPT, with no keys)
 *
 * The osc page is OPAQUE but not GRAB: a grabbing layer is asked before the
 * self-cal lock, and today the lock comes first (a long MOVE during autoset
 * says "Busy", it does not switch mode).
 *
 * No stack_changed hook, and after_key acts for migrated screens only: each
 * forwarded handler keeps its own settings hook (the lock never had one)
 * and sends its own display command, so for a key a legacy handler took the
 * router adds no repaint and no settings write.
 *
 * Migrated screens (their own files; legacy_screens.h). For a key one of
 * them handles, the order is still the one above: the context is taken
 * before the key (ls_globals), the screen's key() runs, and ls_after_key
 * posts the paint it answered and runs the settings hook
 * (lgc_native_after). ls_legacy_done says a legacy path (a legacy screen,
 * the lock, a global) handled the key and has done both itself.
 */
#include "legacy_screens.h"
#include "home_screen.h"
#include "settings_screen.h"
#include "siggen_screen.h"
#include "meter_screen.h"
#include "scope_screen.h"
#include "osc_menu_screen.h"
#include "gallery_screen.h"
#include "overlay_screens.h"
#include "model.h"     /* PG_x: the groups each screen shows (pure header) */

_Static_assert((unsigned)SCR_COUNT <= (unsigned)(DCMD_DRAW_ACTIVE_END - DCMD_DRAW_ACTIVE),
               "every screen id fits the DCMD_DRAW_ACTIVE range");
_Static_assert(MODE_OSCILLOSCOPE == 0 && MODE_MULTIMETER == 1 && MODE_SIGNAL_GEN == 2,
               "a base screen's instrument id is its mode value (sys.instrument 0..2)");

/* ── migrated screens ──────────────────────────────────────────────── */

/* Per key (input task): reset by ls_touch (router step 1, every real key). */
static bool        ls_legacy_done;   /* a legacy path handled it: repaint and settings hook done */

static bool ls_is_native(screen_id_t id)
{
    const screen_t *s = screen_get(id);
    return s != NULL && s->draw != NULL;
}

/* ── the screens ───────────────────────────────────────────────────── */

screen_id_t legacy_settings_page(int8_t depth)
{
    switch ((settings_page_t)depth) {
    case SET_PAGE_ABOUT: return SCR_SET_ABOUT;
    case SET_PAGE_SOUND: return SCR_SET_SOUND;
    default:             return SCR_SETTINGS;     /* the list (and any value that is no page) */
    }
}

uint8_t legacy_screens_update_cmd(screen_id_t top)
{
    switch (top) {
    case SCR_SCOPE:    return DCMD_DRAW_SCOPE;
    case SCR_METER:    return DCMD_DRAW_METER;
    case SCR_SIGGEN:   return DCMD_DRAW_SIGGEN;
    case SCR_HOME:     return DCMD_DRAW_HOME;
    case SCR_OSC_MENU: return DCMD_DRAW_OSC_MENU;
    case SCR_SETTINGS: case SCR_SET_ABOUT: case SCR_SET_SOUND:
        return DCMD_DRAW_SETTINGS;
    default:           return DCMD_NONE_MODAL_DROP;
    }
}

uint8_t legacy_screens_active_cmd(void)
{
    const screen_id_t top = screen_top();
    return (unsigned)top < (unsigned)SCR_COUNT ? (uint8_t)(DCMD_DRAW_ACTIVE + (uint8_t)top)
                                               : (uint8_t)DCMD_NONE_MODAL_DROP;
}

uint8_t legacy_screens_resolve_cmd(uint8_t cmd)
{
    if (cmd < (uint8_t)DCMD_DRAW_ACTIVE || cmd >= (uint8_t)DCMD_DRAW_ACTIVE_END) return cmd;
    return legacy_screens_update_cmd((screen_id_t)(cmd - (uint8_t)DCMD_DRAW_ACTIVE));
}

/* ── hooks ─────────────────────────────────────────────────────────── */

static bool ls_locked(button_id_t b, bool is_long)
{
    (void)is_long;
    if (!lgc_selfcal_busy()) return false;
    ls_legacy_done = true;
    if (b == BTN_AUTO || b == BTN_MENU) lgc_selfcal_cancel();
    else lgc_selfcal_refuse();
    return true;
}

/* The globals do not depend on the context (key_map_global), but the context
 * is still taken before a global runs, where the old dispatch took it for
 * every key past the lock: lgc_ctx() drops an arrow stage that no longer
 * exists (cursors switched off, mask cleared from the shell). */
/* The long mode keys' target, or MODE_COUNT for any other action. */
static device_mode_t ls_mode_of(key_action_t a)
{
    switch (a) {
    case KA_ENTER_HOME:   return MODE_HOME;
    case KA_ENTER_METER:  return MODE_MULTIMETER;
    case KA_ENTER_SCOPE:  return MODE_OSCILLOSCOPE;
    case KA_ENTER_SIGGEN: return MODE_SIGNAL_GEN;
    default:              return MODE_COUNT;
    }
}

static bool ls_globals(button_id_t b, bool is_long)
{
    const key_cmd_t g = key_map_global(NULL, b, is_long);
    if (g.act == KA_LEGACY) {
        /* A migrated screen takes the key next (the base, or the page on
         * top of the scope): the context is still taken first, where the
         * legacy screens take it in ls_mapped. */
        if (ls_is_native(screen_base()) || ls_is_native(screen_top())) (void)lgc_ctx();
        return false;
    }
    ls_legacy_done = true;
    const device_mode_t m = ls_mode_of((key_action_t)g.act);
    if (g.act == KA_POWER_PROMPT) {
        /* POWER is never a layer's key: the gallery is closed first, so
         * power-off always works. */
        lgc_state_t s;
        lgc_state(&s);
        if (s.gallery_open) {
            gallery_ui_close();
            (void)screen_close(SCR_GALLERY);
        }
        (void)lgc_ctx();
        lgc_mode_key(BTN_POWER);
    } else if (m != MODE_COUNT) {
        /* Long MENU/SELECT/MOVE/TRIGGER: a sys.mode set, as the shell's. */
        (void)lgc_ctx();
        lgc_mode_set(m);
    } else {
        (void)lgc_ctx();
        lgc_exec(g, b);
    }
    return true;
}

static void ls_touch(void)
{
    ls_legacy_done = false;
    lgc_touch();
}

/* Router step 7. A key a migrated screen handled: post what it answered and
 * run the settings hook. The screen is the one whose key() took it (the
 * router knows), not the one on top now: the key may have switched the mode
 * or closed the layer. */
static void ls_after_key(scr_res_t res)
{
    if (ls_legacy_done) return;
    const screen_id_t who = screen_last_handler();
    if (!ls_is_native(who)) return;
    lgc_native_after(who, res);
}

static const screen_hooks_t ls_hooks = {
    .lock = lgc_lock, .unlock = lgc_unlock, .touch = ls_touch, .click = lgc_click,
    .locked = ls_locked, .globals = ls_globals, .after_key = ls_after_key, .stack_changed = NULL,
};

/* ── the mirror ────────────────────────────────────────────────────── */

void legacy_screens_sync(void)
{
    lgc_state_t s;
    lgc_state(&s);
    const screen_id_t base = screen_base_for_mode(s.mode);
    if (base == SCR_COUNT) return;                 /* not a mode: leave the stack */

    screen_id_t want[SCR_STACK_MAX];
    uint8_t n = 0;
    want[n++] = base;
    if (s.mode == MODE_SETTINGS && legacy_settings_page(s.settings_depth) != SCR_SETTINGS)
        want[n++] = legacy_settings_page(s.settings_depth);
    if (s.mode == MODE_OSCILLOSCOPE && s.osc_open) want[n++] = SCR_OSC_MENU;
    if (s.gallery_open) want[n++] = SCR_GALLERY;
    /* The small layers, on top of whatever is up; with all of them the stack
     * is full and the last is not pushed (the display task reads the flags,
     * not the stack). */
    if (s.logo_up && n < SCR_STACK_MAX) want[n++] = SCR_LOGO;
    if (s.prompt_up && n < SCR_STACK_MAX) want[n++] = SCR_POWER_PROMPT;

    screen_stack_t cur = screen_snapshot();
    if (cur.n == n) {
        bool same = true;
        for (uint8_t i = 0; i < n; i++) if (cur.id[i] != want[i]) same = false;
        if (same) return;
    }
    if (cur.n == 0 || cur.id[0] != base) {
        screen_switch(base);                       /* pops every layer */
        cur = screen_snapshot();
    }
    uint8_t keep = 1;
    while (keep < cur.n && keep < n && cur.id[keep] == want[keep]) keep++;
    if (keep < cur.n) (void)screen_close(cur.id[keep]);
    for (uint8_t i = keep; i < n; i++) (void)screen_push(want[i]);
}

void legacy_screens_init(void)
{
    screen_reset();
    (void)screen_register(&settings_screen);
    (void)screen_register(&settings_sound_screen);
    (void)screen_register(&settings_about_screen);
    (void)screen_register(&home_screen);
    (void)screen_register(&siggen_screen);
    (void)screen_register(&meter_screen);
    (void)screen_register(&scope_screen);
    (void)screen_register(&osc_menu_screen);
    (void)screen_register(&gallery_screen);
    (void)screen_register(&logo_screen);
    (void)screen_register(&power_prompt_screen);
    screen_set_hooks(&ls_hooks);
    lgc_state_t s;
    lgc_state(&s);
    const screen_id_t base = screen_base_for_mode(s.mode);
    (void)screen_init(base != SCR_COUNT ? base : SCR_SCOPE);
    legacy_screens_sync();
}
