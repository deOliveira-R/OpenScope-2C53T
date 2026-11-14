/*
 * OpenScope 2C53T - legacy screens. See legacy_screens.h.
 *
 * The order the pre-router input_handle_button() asked its handlers in, and
 * where each step lives now:
 *
 *   1. auto-off restart, key click              router step 1 (touch/click hooks)
 *   2. gallery open: it takes every key but     SCR_GALLERY, GRAB; POWER: the
 *      POWER, which it hands back closed         POWER global closes it first
 *   3. self-cal running: AUTO/MENU cancel,      router step 3 (locked hook)
 *      the rest "Busy", POWER passes
 *   4. long keys: key_map(long)                 globals (key_map_global; the
 *                                               mode keys as a sys.mode set),
 *                                               then the top screen's key_map(long)
 *   5. short keys key_map claims (SAVE; the     globals (SAVE), then the osc page
 *      scope's keys and its Settings page)      layer / the scope base
 *   6. home page: every key but POWER           SCR_HOME (home_screen.c, a
 *                                               migrated screen: see below)
 *   7. meter softkeys (MOVE/SELECT/TRIGGER/PRM) SCR_METER
 *   8. generator digit editor                   SCR_SIGGEN
 *   9. the per-key switch (POWER's countdown)   every base (POWER: the global)
 *
 * The osc page is OPAQUE but not GRAB: a grabbing layer is asked before the
 * self-cal lock, and today the lock comes first (a long MOVE during autoset
 * says "Busy", it does not switch mode).
 *
 * No stack_changed hook, and after_key acts for migrated screens only: each
 * forwarded handler keeps its own settings hook (the gallery and the lock
 * never had one) and sends its own display command, so for a key a legacy
 * handler took the router adds no repaint and no settings write.
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
#include "model.h"     /* PG_x: the groups each screen shows (pure header) */

_Static_assert((unsigned)SCR_COUNT <= (unsigned)(DCMD_DRAW_ACTIVE_END - DCMD_DRAW_ACTIVE),
               "every screen id fits the DCMD_DRAW_ACTIVE range");
_Static_assert(MODE_OSCILLOSCOPE == 0 && MODE_MULTIMETER == 1 && MODE_SIGNAL_GEN == 2,
               "a base screen's instrument id is its mode value (sys.instrument 0..2)");

/* ── migrated screens ──────────────────────────────────────────────── */

/* Per key (input task): reset by ls_touch (router step 1, every real key). */
static bool        ls_legacy_done;   /* a legacy path handled it: repaint and settings hook done */
static screen_id_t ls_key_base;      /* the base when the key arrived (it may switch under it)   */

static bool ls_is_native(screen_id_t id)
{
    const screen_t *s = screen_get(id);
    return s != NULL && s->draw != NULL;
}

/* ── key callbacks ─────────────────────────────────────────────────── */

/* The key_map() part every base shares: long keys always (the globals were
 * taken in router step 4), short keys it claims. false: the screen's own
 * handlers decide. */
static bool ls_mapped(scr_key_t k)
{
    const key_ctx_t c = lgc_ctx();
    const key_cmd_t m = key_map(&c, k.btn, k.is_long);
    if (!k.is_long && m.act == KA_LEGACY) return false;
    lgc_exec(m, k.btn);
    return true;
}

static scr_res_t ls_key_scope(scr_key_t k)
{
    ls_legacy_done = true;
    if (!ls_mapped(k)) lgc_mode_key(k.btn);
    return SCR_HANDLED;
}

static scr_res_t ls_key_meter(scr_key_t k)
{
    ls_legacy_done = true;
    if (!ls_mapped(k) && !lgc_meter_softkey(k.btn)) lgc_mode_key(k.btn);
    return SCR_HANDLED;
}

static scr_res_t ls_key_siggen(scr_key_t k)
{
    ls_legacy_done = true;
    if (!ls_mapped(k) && !lgc_siggen_edit(k.btn)) lgc_mode_key(k.btn);
    return SCR_HANDLED;
}

/* The settings base and every settings page: settings_depth decides inside. */
static scr_res_t ls_key_settings(scr_key_t k)
{
    ls_legacy_done = true;
    if (!ls_mapped(k)) lgc_mode_key(k.btn);
    return SCR_HANDLED;
}

/* The Oscilloscope Settings page: key_map() answers for it while it is open
 * (it owns the arrows, OK and MENU; the plot's own long keys do nothing). */
static scr_res_t ls_key_osc_menu(scr_key_t k)
{
    const key_ctx_t c = lgc_ctx();
    const key_cmd_t m = key_map(&c, k.btn, k.is_long);
    if (m.act == KA_LEGACY) return SCR_UNHANDLED;   /* POWER only; the globals took it */
    ls_legacy_done = true;
    lgc_exec(m, k.btn);
    return SCR_HANDLED;
}

/* The gallery closes itself (MENU, ...): then its layer goes too. A key it
 * did not consume means it was already closed (the shell's `mode`, or
 * another task, between the pre-key sync and here): its layer goes and the
 * key falls through to the normal handlers, as before the router
 * (`gallery_ui_is_open() && gallery_ui_key()`); the router routes it from
 * the self-cal lock on (screen.c step 2). */
static scr_res_t ls_key_gallery(scr_key_t k)
{
    if (!lgc_gallery_key((uint8_t)((uint8_t)k.btn | (k.is_long ? BTN_LONG_FLAG : 0u)))) {
        (void)screen_close(SCR_GALLERY);
        return SCR_UNHANDLED;
    }
    ls_legacy_done = true;
    lgc_state_t s;
    lgc_state(&s);
    return s.gallery_open ? SCR_HANDLED : SCR_CLOSE;
}

/* ── the screens ───────────────────────────────────────────────────── */

/* The property groups each screen shows (screen_t.groups, bit = PG_x): a
 * shell set repaints the screen only when it changed one of them
 * (model_hw_fw.c port_changed). Every screen with the bars shows the
 * generator (the status bar's generator slot) and sys/ui (theme, mode,
 * focus). The settings base and its pages keep every group (device settings
 * are sys.* and meter.link today; a repaint there is cheap): all of them. The
 * gallery shows none of them. */
#define G(g)        ((uint16_t)(1u << (g)))
#define G_BARS      (G(PG_GEN) | G(PG_SYS) | G(PG_UI))
#define G_SCOPE     (G_BARS | G(PG_CH1) | G(PG_CH2) | G(PG_TB) | G(PG_TRIG) | G(PG_VIEW) | \
                     G(PG_FFT) | G(PG_IL) | G(PG_MASK))
#define G_METER     (G_BARS | G(PG_METER))
#define G_SIGGEN    G_BARS
#define G_ALL       ((uint16_t)((1u << PG_COUNT) - 1u))

#define LBASE(id, nm, md, ins, keyfn, desc, grp) \
    { nm, id, md, ins, 0, NULL, NULL, keyfn, NULL, NULL, NULL, desc, grp }
#define LPAGE(id, nm, fl, keyfn, desc, grp) \
    { nm, id, MODE_COUNT, SCR_INSTRUMENT_NONE, fl, NULL, NULL, keyfn, NULL, NULL, NULL, desc, grp }

/* The legacy rows. SCR_HOME is not one: home_screen (home_screen.c). */
static const screen_t ls_table[] = {
    LBASE(SCR_SCOPE,    "scope",    MODE_OSCILLOSCOPE, MODE_OSCILLOSCOPE,   ls_key_scope,    NULL,                 G_SCOPE),
    LBASE(SCR_METER,    "meter",    MODE_MULTIMETER,   MODE_MULTIMETER,     ls_key_meter,    NULL,                 G_METER),
    LBASE(SCR_SIGGEN,   "siggen",   MODE_SIGNAL_GEN,   MODE_SIGNAL_GEN,     ls_key_siggen,   NULL,                 G_SIGGEN),
    LBASE(SCR_SETTINGS, "settings", MODE_SETTINGS,     SCR_INSTRUMENT_NONE, ls_key_settings, ui_describe_settings, G_ALL),
    LPAGE(SCR_SET_ABOUT,     "set_about",     SCR_F_CHILD, ls_key_settings, ui_describe_settings, G_ALL),
    LPAGE(SCR_SET_SOUND,     "set_sound",     SCR_F_CHILD, ls_key_settings, ui_describe_settings, G_ALL),
    LPAGE(SCR_OSC_MENU, "osc_menu", SCR_F_OPAQUE,                                ls_key_osc_menu, ui_describe_osc_menu, G_SCOPE),
    LPAGE(SCR_GALLERY,  "gallery",  SCR_F_OPAQUE | SCR_F_GRAB | SCR_F_NO_BARS, ls_key_gallery,  NULL,                 0),
};
_Static_assert(PG_COUNT <= 16, "screen_t.groups holds one bit per property group");

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
        /* A migrated base takes the key next: the context is still taken
         * first, where the legacy screens take it in ls_mapped. */
        if (ls_is_native(screen_base())) (void)lgc_ctx();
        return false;
    }
    ls_legacy_done = true;
    const device_mode_t m = ls_mode_of((key_action_t)g.act);
    if (g.act == KA_POWER_PROMPT) {
        /* The gallery hands POWER back after closing itself. */
        lgc_state_t s;
        lgc_state(&s);
        if (s.gallery_open) {
            (void)lgc_gallery_key((uint8_t)BTN_POWER);
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
    ls_key_base = screen_base();
    lgc_touch();
}

/* Router step 7. A key a migrated screen handled: post what it answered and
 * run the settings hook. The screen is the base the key arrived on, not
 * the one on top now (the key may have switched the mode). */
static void ls_after_key(scr_res_t res)
{
    if (ls_legacy_done || !ls_is_native(ls_key_base)) return;
    lgc_native_after(ls_key_base, res);
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
    for (unsigned i = 0; i < sizeof ls_table / sizeof ls_table[0]; i++)
        (void)screen_register(&ls_table[i]);
    (void)screen_register(&home_screen);
    screen_set_hooks(&ls_hooks);
    lgc_state_t s;
    lgc_state(&s);
    const screen_id_t base = screen_base_for_mode(s.mode);
    (void)screen_init(base != SCR_COUNT ? base : SCR_SCOPE);
    legacy_screens_sync();
}
