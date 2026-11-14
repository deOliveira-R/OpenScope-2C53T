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
 *   4. long keys: key_map(long)                 globals (key_map_global), then
 *                                               the top screen's key_map(long)
 *   5. short keys key_map claims (SAVE; the     globals (SAVE), then the osc page
 *      scope's keys and its Settings page)      layer / the scope base
 *   6. home page: every key but POWER           SCR_HOME
 *   7. meter softkeys (MOVE/SELECT/TRIGGER/PRM) SCR_METER
 *   8. generator digit editor                   SCR_SIGGEN
 *   9. the per-key switch (POWER's countdown)   every base (POWER: the global)
 *
 * The osc page is OPAQUE but not GRAB: a grabbing layer is asked before the
 * self-cal lock, and today the lock comes first (a long MOVE during autoset
 * says "Busy", it does not switch mode).
 *
 * No after_key and no stack_changed hook: each forwarded handler keeps its own
 * settings hook (the gallery and the lock never had one) and sends its own
 * display command, so the router adds no repaint and no settings write.
 */
#include "legacy_screens.h"

_Static_assert(MODE_OSCILLOSCOPE == 0 && MODE_MULTIMETER == 1 && MODE_SIGNAL_GEN == 2,
               "a base screen's instrument id is its mode value (sys.instrument 0..2)");

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
    if (!ls_mapped(k)) lgc_mode_key(k.btn);
    return SCR_HANDLED;
}

static scr_res_t ls_key_meter(scr_key_t k)
{
    if (!ls_mapped(k) && !lgc_meter_softkey(k.btn)) lgc_mode_key(k.btn);
    return SCR_HANDLED;
}

static scr_res_t ls_key_siggen(scr_key_t k)
{
    if (!ls_mapped(k) && !lgc_siggen_edit(k.btn)) lgc_mode_key(k.btn);
    return SCR_HANDLED;
}

/* The settings base and every settings page: settings_depth decides inside. */
static scr_res_t ls_key_settings(scr_key_t k)
{
    if (!ls_mapped(k)) lgc_mode_key(k.btn);
    return SCR_HANDLED;
}

static scr_res_t ls_key_home(scr_key_t k)
{
    if (ls_mapped(k)) return SCR_HANDLED;
    if (k.btn != BTN_POWER) lgc_home(k.btn);      /* POWER is a global: never here */
    else lgc_mode_key(k.btn);
    return SCR_HANDLED;
}

/* The Oscilloscope Settings page: key_map() answers for it while it is open
 * (it owns the arrows, OK and MENU; the plot's own long keys do nothing). */
static scr_res_t ls_key_osc_menu(scr_key_t k)
{
    const key_ctx_t c = lgc_ctx();
    const key_cmd_t m = key_map(&c, k.btn, k.is_long);
    if (m.act == KA_LEGACY) return SCR_UNHANDLED;   /* POWER only; the globals took it */
    lgc_exec(m, k.btn);
    return SCR_HANDLED;
}

/* The gallery closes itself (MENU, ...): then its layer goes too. */
static scr_res_t ls_key_gallery(scr_key_t k)
{
    lgc_gallery_key((uint8_t)((uint8_t)k.btn | (k.is_long ? BTN_LONG_FLAG : 0u)));
    lgc_state_t s;
    lgc_state(&s);
    return s.gallery_open ? SCR_HANDLED : SCR_CLOSE;
}

/* ── the screens ───────────────────────────────────────────────────── */

#define LBASE(id, nm, md, ins, keyfn, desc) \
    { nm, id, md, ins, 0, NULL, NULL, keyfn, NULL, NULL, NULL, desc, 0 }
#define LPAGE(id, nm, fl, keyfn, desc) \
    { nm, id, MODE_COUNT, SCR_INSTRUMENT_NONE, fl, NULL, NULL, keyfn, NULL, NULL, NULL, desc, 0 }

static const screen_t ls_table[] = {
    LBASE(SCR_SCOPE,    "scope",    MODE_OSCILLOSCOPE, MODE_OSCILLOSCOPE,   ls_key_scope,    NULL),
    LBASE(SCR_METER,    "meter",    MODE_MULTIMETER,   MODE_MULTIMETER,     ls_key_meter,    NULL),
    LBASE(SCR_SIGGEN,   "siggen",   MODE_SIGNAL_GEN,   MODE_SIGNAL_GEN,     ls_key_siggen,   NULL),
    LBASE(SCR_SETTINGS, "settings", MODE_SETTINGS,     SCR_INSTRUMENT_NONE, ls_key_settings, ui_describe_settings),
    LBASE(SCR_HOME,     "home",     MODE_HOME,         SCR_INSTRUMENT_NONE, ls_key_home,     ui_describe_home),
    LPAGE(SCR_SET_OSC,       "set_osc",       SCR_F_CHILD, ls_key_settings, ui_describe_settings),
    LPAGE(SCR_SET_ABOUT,     "set_about",     SCR_F_CHILD, ls_key_settings, ui_describe_settings),
    LPAGE(SCR_SET_MATH,      "set_math",      SCR_F_CHILD, ls_key_settings, ui_describe_settings),
    LPAGE(SCR_SET_COMPONENT, "set_component", SCR_F_CHILD, ls_key_settings, ui_describe_settings),
    LPAGE(SCR_SET_BODE,      "set_bode",      SCR_F_CHILD, ls_key_settings, ui_describe_settings),
    LPAGE(SCR_SET_SOUND,     "set_sound",     SCR_F_CHILD, ls_key_settings, ui_describe_settings),
    LPAGE(SCR_OSC_MENU, "osc_menu", SCR_F_OPAQUE,                                ls_key_osc_menu, ui_describe_osc_menu),
    LPAGE(SCR_GALLERY,  "gallery",  SCR_F_OPAQUE | SCR_F_GRAB | SCR_F_NO_BARS, ls_key_gallery,  NULL),
};

screen_id_t legacy_settings_page(int8_t depth)
{
    switch (depth) {
    case 1:  return SCR_SET_OSC;
    case 2:  return SCR_SET_ABOUT;
    case 3:  return SCR_SET_MATH;
    case 4:                                       /* component tester        */
    case 5:  return SCR_SET_COMPONENT;            /* ... its resistor calculator */
    case 6:  return SCR_SET_BODE;
    case 7:  return SCR_SET_SOUND;
    default: return SCR_SETTINGS;
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
    case SCR_SETTINGS: case SCR_SET_OSC: case SCR_SET_ABOUT: case SCR_SET_MATH:
    case SCR_SET_COMPONENT: case SCR_SET_BODE: case SCR_SET_SOUND:
        return DCMD_DRAW_SETTINGS;
    default:           return DCMD_NONE_MODAL_DROP;
    }
}

/* ── hooks ─────────────────────────────────────────────────────────── */

static bool ls_locked(button_id_t b, bool is_long)
{
    (void)is_long;
    if (!lgc_selfcal_busy()) return false;
    if (b == BTN_AUTO || b == BTN_MENU) lgc_selfcal_cancel();
    else lgc_selfcal_refuse();
    return true;
}

static bool ls_globals(button_id_t b, bool is_long)
{
    const key_cmd_t g = key_map_global(NULL, b, is_long);
    if (g.act == KA_LEGACY) return false;
    if (g.act == KA_POWER_PROMPT) {
        /* The gallery hands POWER back after closing itself. */
        lgc_state_t s;
        lgc_state(&s);
        if (s.gallery_open) {
            lgc_gallery_key((uint8_t)BTN_POWER);
            (void)screen_close(SCR_GALLERY);
        }
        lgc_mode_key(BTN_POWER);
    } else {
        lgc_exec(g, b);
    }
    return true;
}

static const screen_hooks_t ls_hooks = {
    .lock = lgc_lock, .unlock = lgc_unlock, .touch = lgc_touch, .click = lgc_click,
    .locked = ls_locked, .globals = ls_globals, .after_key = NULL, .stack_changed = NULL,
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
    screen_set_hooks(&ls_hooks);
    lgc_state_t s;
    lgc_state(&s);
    const screen_id_t base = screen_base_for_mode(s.mode);
    (void)screen_init(base != SCR_COUNT ? base : SCR_SCOPE);
    legacy_screens_sync();
}
