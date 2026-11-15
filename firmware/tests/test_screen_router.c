/*
 * Host test for the screen core (src/ui/screen.c): the router order, overlays
 * over a base, modal capture, close, switch and the snapshot. In the first
 * part the screens are fakes that log what they were asked (the core's
 * mechanics need answers a real screen does not give on demand); the router
 * code is the firmware's. The second part is the firmware's own table
 * (src/ui/ui_router.c) with every real screen on it: see there.
 *
 * Build: see `make test-screen-router`.
 */

#define SCREEN_TEST_ENV_REAL_FOCUS   /* the scope screen is linked: it owns the arrow stage */
#include "screen_test_env.h"     /* part 2: the real model under the real screens; CHECK, current_mode */

/* ---- event log ---------------------------------------------------------- */

static char     g_log[512];
static int      g_lock_depth, g_lock_calls, g_unlock_calls, g_lock_held_in_cb;
static int      g_touch, g_click, g_after, g_after_last, g_stack_posts;
static int      g_locked_calls, g_globals_calls;

static void logf_(const char *s)
{
    if (g_lock_depth) g_lock_held_in_cb++;
    if (strlen(g_log) + strlen(s) + 2 < sizeof g_log) { strcat(g_log, s); strcat(g_log, " "); }
}
static void logn(const char *what, const char *who, int v)
{
    char b[48];
    snprintf(b, sizeof b, "%s:%s%d", what, who, v);
    logf_(b);
}
#define LOGIS(str) (strcmp(g_log, str " ") == 0)

static void clear_log(void) { g_log[0] = '\0'; }

/* ---- fake screens: one set of callbacks, behaviour set per screen ------- */

typedef struct {
    scr_res_t   key_res;       /* what key() answers */
    redraw_action_t tick_res;
    uint32_t    epoch;
    int         keys;
    scr_key_t   last;
    int         opens, closes;
    screen_id_t last_open_from, last_close_to;
    char        name_tag;
} fake_t;

static fake_t F[SCR_COUNT];

#define CB(id)                                                                          \
    static void open_##id(screen_id_t from)  { F[id].opens++;  F[id].last_open_from = from;                  \
                                               logn("open", #id, (int)from); } \
    static void close_##id(screen_id_t to)   { F[id].closes++; F[id].last_close_to = to; logn("close", #id, (int)to); } \
    static scr_res_t key_##id(scr_key_t k)   { F[id].keys++; F[id].last = k; logn("key", #id, 0); return F[id].key_res; } \
    static void draw_##id(scr_paint_t h, uint32_t fr) { (void)fr; logn("draw", #id, (int)h); } \
    static redraw_action_t tick_##id(uint32_t fr) { (void)fr; logn("tick", #id, 0); return F[id].tick_res; } \
    static uint32_t epoch_##id(void)         { return F[id].epoch; }
CB(SCR_HOME) CB(SCR_SCOPE) CB(SCR_METER) CB(SCR_SETTINGS) CB(SCR_SET_SOUND)
CB(SCR_OSC_MENU) CB(SCR_GALLERY) CB(SCR_LOGO) CB(SCR_POWER_PROMPT) CB(SCR_SET_ABOUT)

static void describe_osc(char *out, size_t n) { snprintf(out, n, "row 2 pane 1"); }

#define SCREEN(id, nm, md, ins, fl, grp, desc) \
    { nm, id, md, ins, fl, open_##id, close_##id, key_##id, draw_##id, tick_##id, epoch_##id, desc, grp }

static const screen_t S_HOME     = SCREEN(SCR_HOME,     "home",     MODE_HOME,         0xFF, 0, 0x01, NULL);
static const screen_t S_SCOPE    = SCREEN(SCR_SCOPE,    "scope",    MODE_OSCILLOSCOPE, 3,    0, 0x02, NULL);
static const screen_t S_METER    = SCREEN(SCR_METER,    "meter",    MODE_MULTIMETER,   0xFF, 0, 0x04, NULL);
static const screen_t S_SETTINGS = SCREEN(SCR_SETTINGS, "settings", MODE_SETTINGS,     0xFF, 0, 0x08, NULL);
static const screen_t S_SET_SOUND  = SCREEN(SCR_SET_SOUND,  "set_sound",  MODE_COUNT,        0xFF, SCR_F_CHILD, 0x10, NULL);
static const screen_t S_OSC_MENU = SCREEN(SCR_OSC_MENU, "osc_menu", MODE_COUNT,        0xFF, SCR_F_OPAQUE | SCR_F_GRAB, 0x20, describe_osc);
static const screen_t S_GALLERY  = SCREEN(SCR_GALLERY,  "gallery",  MODE_COUNT,        0xFF, SCR_F_OPAQUE | SCR_F_GRAB | SCR_F_NO_BARS, 0x40, NULL);
static const screen_t S_LOGO     = SCREEN(SCR_LOGO,     "logo",     MODE_COUNT,        0xFF, 0, 0x80, NULL);
static const screen_t S_POWER    = SCREEN(SCR_POWER_PROMPT, "power", MODE_COUNT,       0xFF, SCR_F_OPAQUE, 0x100, NULL);
static const screen_t S_ABOUT    = SCREEN(SCR_SET_ABOUT, "about",    MODE_COUNT,       0xFF, SCR_F_CHILD, 0, NULL);

/* ---- hooks -------------------------------------------------------------- */

static bool g_lock_busy, g_lock_pass_power_probe;
static bool g_global_take;          /* globals() takes these */
static button_id_t g_global_btn; static bool g_global_long, g_global_any;

static void h_lock(void)   { g_lock_depth++; g_lock_calls++; }
static void h_unlock(void) { g_lock_depth--; g_unlock_calls++; }
static void h_touch(void)  { g_touch++;  logf_("touch"); }
static void h_click(void)  { g_click++;  logf_("click"); }
static bool h_locked(button_id_t b, bool l)
{
    (void)l; g_locked_calls++; logf_("locked");
    if (b == BTN_POWER) g_lock_pass_power_probe = true;
    return g_lock_busy;
}
static bool h_globals(button_id_t b, bool l)
{
    g_globals_calls++; logf_("globals");
    g_global_btn = b; g_global_long = l;
    return g_global_any || (g_global_take && b == g_global_btn);
}
static void h_after(scr_res_t r) { g_after++; g_after_last = (int)r; logf_("after"); }
static void h_posted(void)       { g_stack_posts++; }

static const screen_hooks_t HOOKS = { h_lock, h_unlock, h_touch, h_click, h_locked, h_globals, h_after, h_posted };

static void setup(screen_id_t base)
{
    screen_reset();
    memset(F, 0, sizeof F);
    memset(g_log, 0, sizeof g_log);
    g_lock_depth = g_lock_calls = g_unlock_calls = g_lock_held_in_cb = 0;
    g_touch = g_click = g_after = g_after_last = g_stack_posts = 0;
    g_locked_calls = g_globals_calls = 0;
    g_lock_busy = g_lock_pass_power_probe = false;
    g_global_take = g_global_any = false;
    g_global_btn = BTN_NONE; g_global_long = false;
    for (int i = 0; i < SCR_COUNT; i++) { F[i].key_res = SCR_UNHANDLED; F[i].tick_res = REDRAW_SKIP; }
    const screen_t *all[] = { &S_HOME, &S_SCOPE, &S_METER, &S_SETTINGS, &S_SET_SOUND, &S_OSC_MENU,
                              &S_GALLERY, &S_LOGO, &S_POWER, &S_ABOUT };
    for (unsigned i = 0; i < sizeof all / sizeof all[0]; i++) screen_register(all[i]);
    screen_set_hooks(&HOOKS);
    screen_init(base);
    clear_log();
    g_stack_posts = 0;
}

/* ---- tests -------------------------------------------------------------- */

static void test_registry_and_init(void)
{
    printf("registry and init\n");
    setup(SCR_SCOPE);
    CHECK("init: base is scope, depth 1", screen_base() == SCR_SCOPE && screen_top() == SCR_SCOPE &&
                                           screen_snapshot().n == 1);
    CHECK("init: base open(SCR_COUNT) was called once", F[SCR_SCOPE].opens == 1 && F[SCR_SCOPE].last_open_from == SCR_COUNT);
    CHECK("register: duplicate id refused, NULL refused", !screen_register(&S_SCOPE) && !screen_register(NULL));
    CHECK("init: an overlay is not a base", !screen_init(SCR_OSC_MENU));
    CHECK("init: an unregistered id is refused", !screen_init(SCR_SIGGEN));
    CHECK("lookup by mode", screen_base_for_mode(MODE_MULTIMETER) == SCR_METER &&
                            screen_base_for_mode(MODE_SIGNAL_GEN) == SCR_COUNT);
    CHECK("lookup by instrument id (scope owns 3)", screen_base_for_instrument(3) == SCR_SCOPE &&
                            screen_base_for_instrument(9) == SCR_COUNT &&
                            screen_base_for_instrument(SCR_INSTRUMENT_NONE) == SCR_COUNT);
}

static void test_none_and_activity(void)
{
    printf("step 1: activity and BTN_NONE\n");
    setup(SCR_SCOPE);
    CHECK("BTN_NONE: UNHANDLED", screen_route_key(BTN_NONE) == SCR_UNHANDLED);
    CHECK("BTN_NONE: long flag alone is also none", screen_route_key((button_id_t)BTN_LONG_FLAG) == SCR_UNHANDLED);
    CHECK("BTN_NONE routes nowhere: no hook, no key, no log", g_log[0] == '\0');
    CHECK("BTN_NONE: touch/click/after not called", g_touch == 0 && g_click == 0 && g_after == 0 && F[SCR_SCOPE].keys == 0);
    screen_route_key(BTN_UP);
    CHECK("real key: touch and click once each, before anything else", g_touch == 1 && g_click == 1 &&
          strncmp(g_log, "touch click ", 12) == 0);
    CHECK("an unhandled key: no after_key", g_after == 0);
}

static void test_order_base(void)
{
    printf("router order on a base\n");
    setup(SCR_SCOPE);
    F[SCR_SCOPE].key_res = SCR_DRAW;
    CHECK("base handles: result passes through", screen_route_key(BTN_UP) == SCR_DRAW);
    CHECK("order: touch click locked globals key after",
          LOGIS("touch click locked globals key:SCR_SCOPE0 after"));
    CHECK("after_key got the result", g_after == 1 && g_after_last == SCR_DRAW);

    clear_log(); g_global_any = true;
    CHECK("globals take the key: HANDLED", screen_route_key(BTN_MENU | BTN_LONG_FLAG) == SCR_HANDLED);
    CHECK("globals first: the base never saw it", F[SCR_SCOPE].keys == 1 && g_global_long && g_global_btn == BTN_MENU);
    CHECK("globals-handled key still runs after_key", g_after == 2 && g_after_last == SCR_HANDLED);

    clear_log(); g_global_any = false; g_lock_busy = true;
    int before = F[SCR_SCOPE].keys;
    CHECK("self-cal lock consumes the key before globals and the base",
          screen_route_key(BTN_AUTO) == SCR_HANDLED && F[SCR_SCOPE].keys == before &&
          LOGIS("touch click locked after"));
    g_lock_pass_power_probe = false;
    int lc = g_locked_calls;
    screen_route_key(BTN_POWER);
    CHECK("POWER is never offered to the lock", !g_lock_pass_power_probe && g_locked_calls == lc);
    CHECK("POWER reaches the globals under the lock", g_global_btn == BTN_POWER);

    setup(SCR_SCOPE);
    F[SCR_SCOPE].key_res = SCR_HANDLED;
    screen_route_key(BTN_OK | BTN_LONG_FLAG);
    CHECK("long flag is stripped and passed as is_long", F[SCR_SCOPE].last.btn == BTN_OK && F[SCR_SCOPE].last.is_long);
    screen_route_key(BTN_OK);
    CHECK("a short key arrives with is_long false", F[SCR_SCOPE].last.btn == BTN_OK && !F[SCR_SCOPE].last.is_long);
}

static void test_overlay_over_base(void)
{
    printf("overlays over the base\n");
    setup(SCR_SETTINGS);
    screen_push(SCR_SET_SOUND);
    F[SCR_SET_SOUND].key_res = SCR_DRAW;
    screen_route_key(BTN_DOWN);
    CHECK("child on top gets the key, the base does not", F[SCR_SET_SOUND].keys == 1 && F[SCR_SETTINGS].keys == 0);

    F[SCR_SET_SOUND].key_res = SCR_UNHANDLED; F[SCR_SETTINGS].key_res = SCR_HANDLED;
    CHECK("a non-opaque layer that leaves the key passes it down", screen_route_key(BTN_DOWN) == SCR_HANDLED &&
          F[SCR_SET_SOUND].keys == 2 && F[SCR_SETTINGS].keys == 1);

    setup(SCR_SCOPE);
    screen_push(SCR_POWER_PROMPT);              /* opaque, not a grab */
    F[SCR_POWER_PROMPT].key_res = SCR_UNHANDLED; F[SCR_SCOPE].key_res = SCR_HANDLED;
    CHECK("an opaque layer ends the walk: nothing reaches the base",
          screen_route_key(BTN_UP) == SCR_UNHANDLED && F[SCR_SCOPE].keys == 0);
    CHECK("all unhandled: no after_key", g_after == 0);

    setup(SCR_SCOPE);
    screen_push(SCR_LOGO); screen_push(SCR_POWER_PROMPT);
    F[SCR_LOGO].key_res = SCR_HANDLED;
    screen_route_key(BTN_UP);
    CHECK("stack of two: the top is asked first and only it when it answers", F[SCR_POWER_PROMPT].keys == 1);
    CHECK("opaque top above a plain layer: the plain layer is not asked", F[SCR_LOGO].keys == 0);
}

static void test_grab(void)
{
    printf("modal capture (GRAB)\n");
    setup(SCR_SCOPE);
    screen_push(SCR_OSC_MENU);
    F[SCR_OSC_MENU].key_res = SCR_DRAW;
    CHECK("grab layer handles: result passes through", screen_route_key(BTN_LEFT) == SCR_DRAW);
    CHECK("the handler of that key is the grab layer", screen_last_handler() == SCR_OSC_MENU);
    CHECK("grab: no lock check, no globals when it handled",
          g_locked_calls == 0 && g_globals_calls == 0 && F[SCR_SCOPE].keys == 0);

    clear_log();
    F[SCR_OSC_MENU].key_res = SCR_UNHANDLED; g_global_any = true;
    CHECK("grab leaves the key: globals only", screen_route_key(BTN_MENU | BTN_LONG_FLAG) == SCR_HANDLED &&
          g_globals_calls == 1 && g_locked_calls == 0 && F[SCR_SCOPE].keys == 0);
    CHECK("grab then globals order", LOGIS("touch click key:SCR_OSC_MENU0 globals after"));
    CHECK("a key the globals took has no handler screen", screen_last_handler() == SCR_COUNT);

    g_global_any = false;
    CHECK("grab leaves the key, globals decline: nothing, never the base",
          screen_route_key(BTN_UP) == SCR_UNHANDLED && F[SCR_SCOPE].keys == 0);

    int k = F[SCR_OSC_MENU].keys;
    g_global_any = true;
    screen_route_key(BTN_POWER);
    CHECK("POWER is never grabbed: globals get it, the layer does not",
          F[SCR_OSC_MENU].keys == k && g_global_btn == BTN_POWER);
    g_global_any = false;
    screen_route_key(BTN_POWER);
    CHECK("POWER the globals decline reaches neither the grab layer nor the base",
          F[SCR_OSC_MENU].keys == k && F[SCR_SCOPE].keys == 0);

    F[SCR_OSC_MENU].key_res = SCR_CLOSE;
    clear_log();
    CHECK("CLOSE pops the grab layer and asks for a full redraw",
          screen_route_key(BTN_MENU) == SCR_DRAW_FULL && screen_top() == SCR_SCOPE && F[SCR_OSC_MENU].closes == 1);
    CHECK("close(to) names the screen below", F[SCR_OSC_MENU].last_close_to == SCR_SCOPE);
    CHECK("the handler is the layer that closed itself, though it is gone",
          screen_last_handler() == SCR_OSC_MENU && screen_top() == SCR_SCOPE);
    CHECK("after_key ran for the close", g_after >= 1 && g_after_last == SCR_DRAW_FULL);
    F[SCR_SCOPE].key_res = SCR_HANDLED;
    CHECK("after the pop the base gets keys again", screen_route_key(BTN_UP) == SCR_HANDLED && F[SCR_SCOPE].keys == 1);
    CHECK("... and is the handler; a key nobody takes, or BTN_NONE, leaves none or the last",
          screen_last_handler() == SCR_SCOPE &&
          (F[SCR_SCOPE].key_res = SCR_UNHANDLED, screen_route_key(BTN_UP) == SCR_UNHANDLED) &&
          screen_last_handler() == SCR_COUNT);
}

static void test_layer_close(void)
{
    printf("CLOSE from an ordinary layer and from a base\n");
    setup(SCR_SCOPE);
    screen_push(SCR_LOGO);                      /* not a grab: reached in the layer walk */
    F[SCR_LOGO].key_res = SCR_CLOSE;
    clear_log();
    CHECK("a plain layer's CLOSE pops it and asks for a full redraw",
          screen_route_key(BTN_OK) == SCR_DRAW_FULL && screen_top() == SCR_SCOPE &&
          screen_snapshot().n == 1 && F[SCR_LOGO].closes == 1);
    CHECK("its close(to) names the base", F[SCR_LOGO].last_close_to == SCR_SCOPE);
    CHECK("the base never saw the key", F[SCR_SCOPE].keys == 0);
    CHECK("after_key got DRAW_FULL", g_after_last == SCR_DRAW_FULL);

    setup(SCR_SCOPE);
    screen_push(SCR_SET_SOUND); screen_push(SCR_LOGO);
    F[SCR_LOGO].key_res = SCR_UNHANDLED; F[SCR_SET_SOUND].key_res = SCR_CLOSE;
    CHECK("a lower layer's CLOSE (key passed down) pops it and the one above",
          screen_route_key(BTN_OK) == SCR_DRAW_FULL && screen_top() == SCR_SCOPE &&
          F[SCR_SET_SOUND].closes == 1 && F[SCR_LOGO].closes == 1);
    CHECK("the handler is the lower layer, not the one that passed the key down",
          screen_last_handler() == SCR_SET_SOUND);

    setup(SCR_SCOPE);
    F[SCR_SCOPE].key_res = SCR_CLOSE;
    uint32_t gen = screen_snapshot().gen;
    CHECK("a base cannot close: CLOSE reads as HANDLED",
          screen_route_key(BTN_OK) == SCR_HANDLED && g_after_last == SCR_HANDLED);
    CHECK("the base stays, nothing closed, the stack unchanged",
          screen_top() == SCR_SCOPE && screen_snapshot().n == 1 && F[SCR_SCOPE].closes == 0 &&
          screen_snapshot().gen == gen && g_stack_posts == 0);
}

static void test_close_and_pop(void)
{
    printf("close, pop, push\n");
    setup(SCR_SCOPE);
    CHECK("pop on a base alone does nothing", (screen_pop(), screen_snapshot().n == 1) && g_stack_posts == 0);
    CHECK("close of a layer that is not open: false, no callback", !screen_close(SCR_GALLERY) && F[SCR_GALLERY].closes == 0);
    CHECK("close of the base is refused", !screen_close(SCR_SCOPE) && screen_base() == SCR_SCOPE);

    CHECK("push logo", screen_push(SCR_LOGO) && F[SCR_LOGO].opens == 1 && F[SCR_LOGO].last_open_from == SCR_SCOPE);
    CHECK("push posts a stack change", g_stack_posts == 1);
    CHECK("push twice: refused", !screen_push(SCR_LOGO) && F[SCR_LOGO].opens == 1);
    CHECK("push a base as a layer: refused", !screen_push(SCR_METER));
    CHECK("push an unregistered screen: refused", !screen_push(SCR_SIGGEN));
    CHECK("close is idempotent: first closes, second is a no-op",
          screen_close(SCR_LOGO) && !screen_close(SCR_LOGO) && F[SCR_LOGO].closes == 1);
    CHECK("close posts once", g_stack_posts == 2);

    screen_push(SCR_SET_SOUND); screen_push(SCR_SET_ABOUT); screen_push(SCR_LOGO); screen_push(SCR_GALLERY);
    CHECK("depth cap: the stack is full at SCR_STACK_MAX (5: one of every kind of layer)",
          SCR_STACK_MAX == 5 && screen_snapshot().n == SCR_STACK_MAX && !screen_push(SCR_POWER_PROMPT));
    clear_log();
    CHECK("close of a lower layer closes the ones above it, top first",
          screen_close(SCR_SET_SOUND) && screen_snapshot().n == 1);
    CHECK("pop order was top-down (logo, about, osc)", strstr(g_log, "SCR_LOGO") < strstr(g_log, "SCR_SET_ABOUT") &&
          strstr(g_log, "SCR_SET_ABOUT") < strstr(g_log, "SCR_SET_SOUND"));

    screen_push(SCR_LOGO);
    screen_pop();
    CHECK("pop closes the top and names the one below", F[SCR_LOGO].last_close_to == SCR_SCOPE && screen_snapshot().n == 1);
}

static void test_switch(void)
{
    printf("switch\n");
    setup(SCR_SETTINGS);
    screen_push(SCR_SET_SOUND); screen_push(SCR_LOGO);
    clear_log(); g_stack_posts = 0;
    screen_switch(SCR_METER);
    CHECK("switch: layers close top-down, then the old base closes, then the new opens",
          strstr(g_log, "close:SCR_LOGO") && strstr(g_log, "close:SCR_LOGO") < strstr(g_log, "close:SCR_SET_SOUND") &&
          strstr(g_log, "close:SCR_SET_SOUND") < strstr(g_log, "close:SCR_SETTINGS") &&
          strstr(g_log, "close:SCR_SETTINGS") < strstr(g_log, "open:SCR_METER"));
    CHECK("switch: old base told where it goes, new base told where it came from",
          F[SCR_SETTINGS].last_close_to == SCR_METER && F[SCR_METER].last_open_from == SCR_SETTINGS);
    CHECK("switch: stack is the new base alone", screen_snapshot().n == 1 && screen_base() == SCR_METER);
    CHECK("switch posts one stack change", g_stack_posts == 1);

    screen_push(SCR_LOGO);
    int opens = F[SCR_METER].opens, closes = F[SCR_METER].closes;
    screen_switch(SCR_METER);
    CHECK("switch to the same base: overlays close, the base is not reopened",
          screen_snapshot().n == 1 && F[SCR_METER].opens == opens && F[SCR_METER].closes == closes);
    screen_switch(SCR_LOGO);
    screen_switch(SCR_SIGGEN);
    CHECK("switch to a non-base or unregistered id: ignored", screen_base() == SCR_METER);
}

static void test_opaque_ticks_draw(void)
{
    printf("OPAQUE, ticks, draw, epoch\n");
    setup(SCR_SCOPE);
    F[SCR_SCOPE].tick_res = REDRAW_INCREMENTAL; F[SCR_LOGO].tick_res = REDRAW_SKIP;
    screen_push(SCR_LOGO);
    clear_log();
    CHECK("tick: base and plain layer both tick, max wins", screen_tick(1) == REDRAW_INCREMENTAL &&
          strstr(g_log, "tick:SCR_SCOPE") && strstr(g_log, "tick:SCR_LOGO"));
    CHECK("popups allowed without an opaque layer", screen_popups_allowed());

    screen_push(SCR_POWER_PROMPT);
    F[SCR_POWER_PROMPT].tick_res = REDRAW_FULL;
    clear_log();
    CHECK("OPAQUE layer: the base and the plain layer below do not tick",
          screen_tick(2) == REDRAW_FULL && !strstr(g_log, "tick:SCR_SCOPE") && !strstr(g_log, "tick:SCR_LOGO"));
    F[SCR_POWER_PROMPT].tick_res = REDRAW_SKIP;
    CHECK("opaque layer quiet: the result is its own, not the base's", screen_tick(3) == REDRAW_SKIP);
    CHECK("OPAQUE suppresses popups", !screen_popups_allowed());

    clear_log();
    screen_draw(SCR_PAINT_FULL, 0);
    CHECK("full draw starts at the highest OPAQUE layer", LOGIS("draw:SCR_POWER_PROMPT0"));
    setup(SCR_SCOPE); screen_push(SCR_LOGO); clear_log();
    screen_draw(SCR_PAINT_FULL, 0);
    CHECK("full draw without opaque: bottom-up", LOGIS("draw:SCR_SCOPE0 draw:SCR_LOGO0"));
    clear_log();
    screen_draw(SCR_PAINT_UPDATE, 0);
    CHECK("update draw: the top only", LOGIS("draw:SCR_LOGO1"));

    setup(SCR_SCOPE);
    CHECK("bars wanted on a plain stack", screen_bars_wanted());
    screen_push(SCR_GALLERY);
    CHECK("NO_BARS layer: bars not wanted", !screen_bars_wanted());
    screen_pop();
    CHECK("bars back after it closes", screen_bars_wanted());

    uint32_t e0 = screen_epoch();
    F[SCR_SCOPE].epoch = 7;
    uint32_t e1 = screen_epoch();
    CHECK("epoch follows the visible layers' epochs", e0 != e1 && screen_epoch() == e1);
    screen_push(SCR_OSC_MENU);
    F[SCR_SCOPE].epoch = 8;
    uint32_t e2 = screen_epoch(), e3;
    F[SCR_SCOPE].epoch = 9;
    e3 = screen_epoch();
    CHECK("epoch ignores a layer hidden under an OPAQUE one", e2 == e3);
    CHECK("groups: union of the visible layers", screen_groups() == 0x20);
    screen_pop();
    CHECK("groups: base only after the pop", screen_groups() == 0x02);
}

static void test_snapshot(void)
{
    printf("snapshot\n");
    setup(SCR_SETTINGS);
    screen_stack_t s0 = screen_snapshot();
    CHECK("snapshot: base only, ids and generation",
          s0.n == 1 && s0.id[0] == SCR_SETTINGS && s0.gen == 1);
    screen_push(SCR_SET_SOUND); screen_push(SCR_OSC_MENU);
    screen_stack_t s = screen_snapshot();
    CHECK("snapshot: ids bottom-up", s.n == 3 && s.id[0] == SCR_SETTINGS && s.id[1] == SCR_SET_SOUND && s.id[2] == SCR_OSC_MENU);
    CHECK("snapshot: generation moves on every change", s.gen == s0.gen + 2);
    screen_close(SCR_OSC_MENU); screen_close(SCR_OSC_MENU);
    CHECK("an idempotent close does not move the generation", screen_snapshot().gen == s.gen + 1);

    char buf[128];
    screen_snapshot_text(buf, sizeof buf);
    CHECK("text: names and generation", strcmp(buf, "settings > set_sound gen=4") == 0);
    screen_push(SCR_OSC_MENU);
    screen_snapshot_text(buf, sizeof buf);
    CHECK("text: the top's describe() is appended", strcmp(buf, "settings > set_sound > osc_menu gen=5 | row 2 pane 1") == 0);
    char small[12];
    size_t n = screen_snapshot_text(small, sizeof small);
    CHECK("text: a small buffer is cut, terminated, and the length is right", n == strlen(small) && n < sizeof small);
    CHECK("text: zero-size buffer is safe", screen_snapshot_text(small, 0) == 0 && screen_snapshot_text(NULL, 5) == 0);
}

/* A key handler that changes the stack itself (push from key, close from key). */
static scr_res_t reenter_key(scr_key_t k)
{
    (void)k;
    logf_("reenter");
    screen_push(SCR_LOGO);
    return SCR_HANDLED;
}
static const screen_t S_REENTER = { "scope", SCR_SCOPE, MODE_OSCILLOSCOPE, 3, 0, NULL, NULL, reenter_key, NULL, NULL, NULL, NULL, 0 };

static void test_locking_and_reentry(void)
{
    printf("locking and re-entry\n");
    setup(SCR_SCOPE);
    screen_push(SCR_LOGO); screen_route_key(BTN_UP); screen_tick(0); screen_draw(SCR_PAINT_FULL, 0);
    screen_pop(); screen_snapshot();
    CHECK("lock and unlock balanced", g_lock_calls == g_unlock_calls && g_lock_calls > 0 && g_lock_depth == 0);
    CHECK("the lock was never held while a screen callback ran", g_lock_held_in_cb == 0);

    screen_reset();
    screen_register(&S_REENTER); screen_register(&S_LOGO); screen_set_hooks(&HOOKS);
    memset(F, 0, sizeof F); g_lock_held_in_cb = 0;
    screen_init(SCR_SCOPE);
    CHECK("a key handler may push (no deadlock on the injected lock)",
          screen_route_key(BTN_OK) == SCR_HANDLED && screen_top() == SCR_LOGO && g_lock_held_in_cb == 0);

    screen_reset();
    screen_hooks_t none = {0};
    screen_set_hooks(&none);
    screen_register(&S_SCOPE); screen_register(&S_LOGO);
    memset(F, 0, sizeof F);
    CHECK("all hooks NULL: router and stack still work", screen_init(SCR_SCOPE) && screen_push(SCR_LOGO) &&
          screen_route_key(BTN_UP) == SCR_UNHANDLED && (screen_pop(), screen_top() == SCR_SCOPE));
}

/* ═══ the firmware's screens on the router (ARCH-1 B8) ═══════════════════════
 *
 * From here on nothing is a stand-in: src/ui/ui_router.c registers the REAL
 * screens (home, scope, meter, generator, the Settings list and its pages,
 * the Oscilloscope Settings page, the gallery, the logo, the POWER prompt),
 * on the real property model. Faked are only what a screen may not do
 * itself: the paints (display task), the hardware ops, and the router's
 * environment (ui_env_*: the click, the display queue, the settings
 * service, the screenshot, the POWER countdown), all of which log.
 *
 * What is checked: the stack is the authority for what is shown (every
 * layer comes up by a push and goes by a pop, the mode is the base's and
 * current_mode mirrors it); the mode switch; who is asked for a key, in
 * which order, on every screen and under every layer (the global keys: POWER
 * never grabbed, SAVE and the long mode keys from everywhere; the self-cal
 * lock; the gallery's grab); the repaint posted and the settings service
 * after a key; which layer the display task paints. */

#include "fw_screens_env.h"

/* ── tests ─────────────────────────────────────────────────────────── */

static void test_fw_table(void)
{
    printf("the firmware's screens: the table\n");
    fw_boot(ST_SCOPE, OV_NONE, false);
    int ok = 1;
    for (int i = 0; i < SCR_COUNT; i++) {
        const screen_t *s = screen_get((screen_id_t)i);
        ok &= s != NULL && s->id == (screen_id_t)i && s->name != NULL && s->draw != NULL;
    }
    CHECK("every screen id is registered, with a name and a draw of its own (no forwarding rows)", ok);
    CHECK("the five bases by mode: home, scope, meter, generator, settings",
          screen_base_for_mode(MODE_HOME) == SCR_HOME && screen_base_for_mode(MODE_OSCILLOSCOPE) == SCR_SCOPE &&
          screen_base_for_mode(MODE_MULTIMETER) == SCR_METER && screen_base_for_mode(MODE_SIGNAL_GEN) == SCR_SIGGEN &&
          screen_base_for_mode(MODE_SETTINGS) == SCR_SETTINGS);
    CHECK("the layers: pages keep the bars, the gallery / logo / prompt own the LCD; only the gallery grabs",
          settings_sound_screen.flags == (SCR_F_CHILD | SCR_F_OPAQUE) && settings_about_screen.flags == (SCR_F_CHILD | SCR_F_OPAQUE) &&
          osc_menu_screen.flags == SCR_F_OPAQUE && gallery_screen.flags == (SCR_F_OPAQUE | SCR_F_GRAB | SCR_F_NO_BARS) &&
          logo_screen.flags == (SCR_F_OPAQUE | SCR_F_NO_BARS) && power_prompt_screen.flags == (SCR_F_OPAQUE | SCR_F_NO_BARS));
    ok = 1;
    static const device_mode_t modes[] = { MODE_OSCILLOSCOPE, MODE_MULTIMETER, MODE_SIGNAL_GEN, MODE_SETTINGS, MODE_HOME };
    for (unsigned i = 0; i < 5; i++) {
        current_mode = modes[i];                     /* main()'s boot target */
        ui_router_init(current_mode);
        ok &= screen_base() == screen_base_for_mode(modes[i]) && screen_snapshot().n == 1 && current_mode == modes[i];
    }
    CHECK("boot: the stack starts on the screen of the boot mode (every Startup choice), nothing over it", ok);
    ui_router_init(MODE_COUNT);
    CHECK("not a mode: the scope, and current_mode says so", screen_base() == SCR_SCOPE && current_mode == MODE_OSCILLOSCOPE);
}

static void test_fw_stack_authority(void)
{
    printf("the stack is what is shown\n");
    fw_boot(ST_SCOPE, OV_NONE, false);
    fw_key(BTN_MENU);
    CHECK("MENU on the scope: the page's layer is pushed by the key (scope>osc_menu)",
          stack_is("scope>osc_menu") && osc_menu_is_open());
    fw_key(LONG(BTN_SAVE));
    CHECK("long SAVE: the gallery over it (scope>osc_menu>gallery)", stack_is("scope>osc_menu>gallery") && gallery_ui_is_open());
    CHECK("logo show: over that (4 deep)", logo_screen_show(false) && stack_is("scope>osc_menu>gallery>logo") && logo_screen_up());
    power_prompt_show(POWER_PROMPT_AUTOOFF, 0);
    CHECK("the POWER prompt still fits on top (every kind of layer at once: 5)",
          stack_is("scope>osc_menu>gallery>logo>power_prompt") && power_prompt_up() && screen_modal() == &power_prompt_screen);
    power_prompt_hide();
    logo_screen_hide();
    CHECK("each goes by closing its layer: prompt, logo", stack_is("scope>osc_menu>gallery") && !power_prompt_up() && !logo_screen_up());
    fw_key(BTN_MENU);
    CHECK("MENU closes the gallery: the page is still under it", stack_is("scope>osc_menu") && !gallery_ui_is_open());
    fw_key(BTN_MENU);
    CHECK("MENU closes the page", stack_is("scope") && !osc_menu_is_open());

    fw_boot(ST_SET, OV_NONE, false);
    fw_key(BTN_OK);
    CHECK("OK on Sound and Light: settings>set_sound", stack_is("settings>set_sound") && settings_screen_view().page == SET_PAGE_SOUND);
    fw_key(LONG(BTN_SAVE));
    CHECK("the gallery over a Settings page keeps the page under it (settings=7/..)",
          stack_is("settings>set_sound>gallery") && settings_screen_view().page == SET_PAGE_SOUND);
    fw_key(BTN_MENU); fw_key(BTN_MENU);
    CHECK("gallery closed, page closed: the list", stack_is("settings") && settings_screen_view().page == SET_PAGE_LIST);

    int ok = 1;
    for (int st = 0; st < ST_COUNT; st++)
        for (int ov = 0; ov < OV_COUNT; ov++) {
            fw_boot((fw_state_t)st, (fw_over_t)ov, false);
            ok &= mirror_ok() && current_mode == st_mode[st];
            ok &= (ov == OV_GALLERY) == gallery_ui_is_open() && (ov == OV_LOGO) == logo_screen_up();
            ok &= (st == ST_SCOPE_PAGE || st == ST_SCOPE_PANE) == screen_is_open(SCR_OSC_MENU);
        }
    CHECK("in every state: current_mode is the base's mode; a layer is up exactly while it is on the stack", ok);
}

static void test_fw_mode_switch(void)
{
    printf("the mode switch\n");
    fw_boot(ST_SCOPE_PAGE, OV_NONE, false);
    fw_key(LONG(BTN_SELECT));
    CHECK("long SELECT (-> scope) on the scope with its page up: the same mode, nothing closes",
          stack_is("scope>osc_menu") && r_has("mode:0=") && osc_menu_is_open());
    fw_boot(ST_SCOPE_PAGE, OV_GALLERY, false);
    (void)prop_set_local(P(SYS_MODE), MODE_OSCILLOSCOPE, ORIGIN_SHELL, NULL);
    CHECK("`set sys.mode scope` there with the gallery up: the gallery closes, the page stays",
          stack_is("scope>osc_menu") && r_has("mode:0="));
    fw_boot(ST_SCOPE_PAGE, OV_GALLERY, false);
    (void)prop_set_local(P(SYS_MODE), MODE_MULTIMETER, ORIGIN_SHELL, NULL);
    CHECK("`set sys.mode meter`: every layer closes, the page's model with its layer, the meter is the base",
          stack_is("meter") && !osc_menu_is_open() && osc_menu_screen_opener() == BTN_NONE && !gallery_ui_is_open() &&
          current_mode == MODE_MULTIMETER && r_has("mode:1"));
    (void)prop_set_local(P(SYS_MODE), MODE_OSCILLOSCOPE, ORIGIN_SHELL, NULL);
    CHECK("back on the scope the page does not reappear", stack_is("scope"));
    fw_boot(ST_GEN_EDIT, OV_NONE, false);
    fw_key(LONG(BTN_MENU));
    CHECK("long MENU from the generator's editor: Home, the editor dropped, Home preselects the generator",
          stack_is("home") && !siggen_ui_editor()->active && home_screen_selected() == home_item_for_mode(MODE_SIGNAL_GEN));
    fw_boot(ST_SET_QUESTION, OV_NONE, false);
    fw_key(LONG(BTN_MOVE));
    (void)prop_set_local(P(SYS_MODE), MODE_SETTINGS, ORIGIN_SHELL, NULL);
    CHECK("leaving Settings with the question up and coming back: the list, row 0",
          stack_is("settings") && settings_screen_view().page == SET_PAGE_LIST && settings_screen_view().row == 0);
    int ok = 1;
    static const struct { uint8_t key; device_mode_t to; } K[] = {
        { LONG(BTN_MENU), MODE_HOME }, { LONG(BTN_MOVE), MODE_MULTIMETER },
        { LONG(BTN_SELECT), MODE_OSCILLOSCOPE }, { LONG(BTN_TRIGGER), MODE_SIGNAL_GEN } };
    for (int st = 0; st < ST_COUNT; st++)
        for (int ov = 0; ov < OV_COUNT; ov++) {
            if (ov == OV_GALLERY) continue;                      /* the gallery grabs them: below */
            for (unsigned k = 0; k < 4; k++) {
                fw_boot((fw_state_t)st, (fw_over_t)ov, false);
                const screen_stack_t before = screen_snapshot();
                fw_key(K[k].key);
                const bool same = (K[k].to == st_mode[st]);
                char tok[16];
                snprintf(tok, sizeof tok, same ? "mode:%d=" : "mode:%d", (int)K[k].to);
                bool good = r_count("mode:") == 1 && r_has(tok) && mirror_ok() && current_mode == K[k].to &&
                            screen_last_handler() == SCR_COUNT && r_count("svc") == 1 && !r_has("shot") && !r_has("power");
                if (same) good = good && screen_snapshot().n == before.n;      /* nothing closes */
                else      good = good && screen_snapshot().n == 1;             /* every layer closed */
                if (!good) printf("    %s, overlay %d, key %u: [%s] stack %s\n", st_name[st], ov, K[k].key, R, fw_stack());
                ok &= good;
            }
        }
    CHECK("the long mode keys from every screen, page and under the logo: one sys.mode set, no screen asked", ok);
}

/* The expectation table of the router as it was wired over the handlers
 * (the pre-B8 test's rows), on the real screens. Log: the environment's
 * calls in order, then who took the key and its answer (1 handled, 2 paint
 * in place, 3 repaint all), then the stack. */
static void test_fw_key_table(void)
{
    printf("who takes a key: the expectation table\n");
    static const struct {
        const char *what; fw_state_t st; fw_over_t ov; bool busy; uint8_t raw; const char *want; const char *stack;
    } T[] = {
        { "long MOVE in the scope -> meter (global)",       ST_SCOPE,      OV_NONE, 0, LONG(BTN_MOVE),    "touch click mode:1 svc", "meter" },
        { "long SELECT in the meter -> scope (global)",     ST_METER,      OV_NONE, 0, LONG(BTN_SELECT),  "touch click mode:0 svc", "scope" },
        { "long TRIGGER on home -> generator (global)",     ST_HOME,       OV_NONE, 0, LONG(BTN_TRIGGER), "touch click mode:2 svc", "siggen" },
        { "long MENU on a settings page -> home",           ST_SET_SOUND,  OV_NONE, 0, LONG(BTN_MENU),    "touch click mode:4 svc", "home" },
        { "long MOVE under the osc page -> meter",          ST_SCOPE_PAGE, OV_NONE, 0, LONG(BTN_MOVE),    "touch click mode:1 svc", "meter" },
        { "long SAVE in the generator -> gallery",          ST_GEN,        OV_NONE, 0, LONG(BTN_SAVE),    "touch click svc", "siggen>gallery" },
        { "short SAVE on home -> screenshot",               ST_HOME,       OV_NONE, 0, BTN_SAVE,          "touch click shot svc", "home" },
        { "long OK in the scope: its screen (50 % centre)", ST_SCOPE,      OV_NONE, 0, LONG(BTN_OK),      "touch click center50 svc who:1:1", "scope" },
        { "long AUTO in the scope: its screen (baseline)",  ST_SCOPE,      OV_NONE, 0, LONG(BTN_AUTO),    "touch click basecal svc who:1:1", "scope" },
        { "long CH2 in the scope: its screen (CH2 on/off)", ST_SCOPE,      OV_NONE, 0, LONG(BTN_CH2),     "touch click post:all svc who:1:3", "scope" },
        { "long CH1 under the osc page: the page (nothing)", ST_SCOPE_PAGE, OV_NONE, 0, LONG(BTN_CH1),    "touch click svc who:7:1", "scope>osc_menu" },
        { "long OK in the generator: its screen",           ST_GEN,        OV_NONE, 0, LONG(BTN_OK),      "touch click post:3 svc who:3:2", "siggen" },
        { "long AUTO in the generator: its screen",         ST_GEN,        OV_NONE, 0, LONG(BTN_AUTO),    "touch click post:3 svc who:3:2", "siggen" },
        { "CH1 in the generator: screen, full repaint",     ST_GEN,        OV_NONE, 0, BTN_CH1,           "touch click post:all svc who:3:3", "siggen" },
        { "MENU in the generator: its screen (Home)",       ST_GEN,        OV_NONE, 0, BTN_MENU,          "touch click mode:4 svc who:3:1", "home" },
        { "POWER in the generator -> the countdown",        ST_GEN,        OV_NONE, 0, BTN_POWER,         "touch click power svc", "siggen" },
        { "long TRIGGER in the generator: global, itself",  ST_GEN,        OV_NONE, 0, LONG(BTN_TRIGGER), "touch click mode:2= svc", "siggen" },
        { "UP in the generator during autoset: Busy",       ST_GEN,        OV_NONE, 1, BTN_UP,            "touch click busy post:all", "siggen" },
        { "POWER in the meter -> the countdown",            ST_METER,      OV_NONE, 0, BTN_POWER,         "touch click power svc", "meter" },
        { "POWER on home -> the countdown",                 ST_HOME,       OV_NONE, 0, BTN_POWER,         "touch click power svc", "home" },
        { "POWER with the gallery up: closes it, then",     ST_SCOPE,      OV_GALLERY, 0, BTN_POWER,      "touch click power svc", "scope" },
        { "long MOVE with the gallery up: the gallery's",   ST_SCOPE,      OV_GALLERY, 0, LONG(BTN_MOVE), "touch click svc who:8:1", "scope>gallery" },
        { "long MOVE during autoset: Busy",                 ST_SCOPE_PAGE, OV_NONE, 1, LONG(BTN_MOVE),    "touch click busy post:all", "scope>osc_menu" },
        { "long MENU during autoset: cancel",               ST_SCOPE,      OV_NONE, 1, LONG(BTN_MENU),    "touch click cancel", "scope" },
        { "POWER during autoset passes",                    ST_SCOPE,      OV_NONE, 1, BTN_POWER,         "touch click power svc", "scope" },
        { "UP under the osc page: the page",                ST_SCOPE_PAGE, OV_NONE, 0, BTN_UP,            "touch click post:7 svc who:7:2", "scope>osc_menu" },
        { "MENU in the scope: its screen (opens the page)", ST_SCOPE,      OV_NONE, 0, BTN_MENU,          "touch click post:1 svc who:1:2", "scope>osc_menu" },
        { "UP in the scope: its screen",                    ST_SCOPE,      OV_NONE, 0, BTN_UP,            "touch click post:all svc who:1:3", "scope" },
        { "OK in the scope: its screen",                    ST_SCOPE,      OV_NONE, 0, BTN_OK,            "touch click post:all svc who:1:3", "scope" },
        { "SAVE in the scope stays a global",               ST_SCOPE,      OV_NONE, 0, BTN_SAVE,          "touch click shot svc", "scope" },
        { "MOVE in the meter: softkey 0",                   ST_METER,      OV_NONE, 0, BTN_MOVE,          "touch click post:2 svc who:2:2", "meter" },
        { "LEFT in the meter: its screen",                  ST_METER,      OV_NONE, 0, BTN_LEFT,          "touch click post:all svc who:2:3", "meter" },
        { "PRM in the generator: the editor",               ST_GEN,        OV_NONE, 0, BTN_PRM,           "touch click post:3 svc who:3:2", "siggen" },
        { "UP in the generator, editor closed: screen",     ST_GEN,        OV_NONE, 0, BTN_UP,            "touch click post:3 svc who:3:2", "siggen" },
        { "UP in the generator, editor open: screen",       ST_GEN_EDIT,   OV_NONE, 0, BTN_UP,            "touch click post:3 svc who:3:2", "siggen" },
        { "OK on home: home (enters the scope)",            ST_HOME,       OV_NONE, 0, BTN_OK,            "touch click mode:0 svc who:0:1", "scope" },
        { "RIGHT on home: home, and its selection paint",   ST_HOME,       OV_NONE, 0, BTN_RIGHT,         "touch click post:0 svc who:0:2", "home" },
        { "long OK on home: home (nothing)",                ST_HOME,       OV_NONE, 0, LONG(BTN_OK),      "touch click svc who:0:1", "home" },
        { "MENU on home with the gallery up: gallery's",    ST_HOME,       OV_GALLERY, 0, BTN_MENU,       "touch click post:all svc who:8:3", "home" },
        { "MENU on the Settings list: its screen (Home)",   ST_SET,        OV_NONE, 0, BTN_MENU,          "touch click mode:4 svc who:4:1", "home" },
        { "MENU on a Settings page: the page (back)",       ST_SET_ABOUT,  OV_NONE, 0, BTN_MENU,          "touch click pageleft post:all svc who:5:3", "settings" },
        { "short MOVE in settings: its screen, a repaint",  ST_SET,        OV_NONE, 0, BTN_MOVE,          "touch click post:all svc who:4:3", "settings" },
        { "UP on Sound and Light: the page, in place",      ST_SET_SOUND,  OV_NONE, 0, BTN_UP,            "touch click post:6 svc who:6:2", "settings>set_sound" },
        { "OK on the question: the reset, the list",        ST_SET_QUESTION, OV_NONE, 0, BTN_OK,          "touch click reset post:all svc who:4:3", "settings" },
        { "SAVE on a Settings page: the screenshot global", ST_SET_ABOUT,  OV_NONE, 0, BTN_SAVE,          "touch click shot svc", "settings>set_about" },
        { "SAVE under the logo: still the global",          ST_METER,      OV_LOGO, 0, BTN_SAVE,          "touch click shot svc", "meter>logo" },
        { "UP under the logo: nobody's",                    ST_METER,      OV_LOGO, 0, BTN_UP,            "touch click", "meter>logo" },
        { "SAVE with the gallery up: the gallery's (arm delete)", ST_SCOPE, OV_GALLERY, 0, BTN_SAVE,     "touch click post:8 svc who:8:2", "scope>gallery" },
        { "an arrow in the gallery during autoset: the gallery first", ST_SCOPE, OV_GALLERY, 1, BTN_RIGHT, "touch click post:8 svc who:8:2", "scope>gallery" },
    };
    _Static_assert(SCR_HOME == 0 && SCR_SCOPE == 1 && SCR_METER == 2 && SCR_SIGGEN == 3 && SCR_SETTINGS == 4 &&
                   SCR_SET_ABOUT == 5 && SCR_SET_SOUND == 6 && SCR_OSC_MENU == 7 && SCR_GALLERY == 8,
                   "the screen numbers the table spells");
    int bad = 0;
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
        fw_boot(T[i].st, T[i].ov, T[i].busy);
        fw_key(T[i].raw);
        if (screen_last_handler() != SCR_COUNT) r("who:%d:%d", (int)screen_last_handler(), 0);
        /* the answer is what the router returned: read it back from the posts */
        char got[sizeof R];
        strcpy(got, R);
        char *w = strstr(got, "who:");
        if (w) {
            const int res = r_has("post:all") ? 3 : (r_count("post:") ? 2 : 1);
            snprintf(w, sizeof got - (size_t)(w - got), "who:%d:%d ", (int)screen_last_handler(), res);
        }
        char want[160];
        snprintf(want, sizeof want, "%s ", T[i].want);
        if (strcmp(got, want) != 0 || !stack_is(T[i].stack) || !mirror_ok()) {
            bad++;
            printf("    %s: want [%s] %s, got [%s] %s\n", T[i].what, T[i].want, T[i].stack, got, fw_stack());
        }
    }
    CHECK("each row: the logged calls, the screen asked, its repaint, the stack after", bad == 0);
}

/* Every key, short and long, in every state: the order of the router. */
static void test_fw_global_order(void)
{
    printf("the global keys and the order, every key in every state\n");
    int cases = 0, bad = 0;
    char why[160] = "";
    for (int st = 0; st < ST_COUNT; st++)
    for (int ov = 0; ov < OV_COUNT; ov++)
    for (int busy = 0; busy < 2; busy++)
    for (int b = BTN_CH1; b <= BTN_POWER; b++)
    for (int lng = 0; lng < 2; lng++) {
        if (lng && b == BTN_POWER) continue;                     /* POWER has no long press */
        const uint8_t raw = (uint8_t)(b | (lng ? BTN_LONG_FLAG : 0));
        fw_boot((fw_state_t)st, (fw_over_t)ov, busy != 0);
        const screen_stack_t before = screen_snapshot();
        const screen_id_t top0 = before.id[before.n - 1];
        const env_epochs_t e0 = env_epochs();
        fw_key(raw);
        const env_epochs_t e1 = env_epochs();
        const bool untouched = env_epochs_moved(&e0, &e1) == 0 && env_log_is("");   /* no property, no owner call */
        const screen_id_t who = screen_last_handler();
        const char *err = NULL;
        cases++;

        if (r_count("touch") != 1 || r_count("click") != 1) err = "touch / click not once";
        else if (!mirror_ok()) err = "current_mode is not the base's mode";
        else if (fw_lock_depth != 0) err = "lock not released";
        else if (b == BTN_POWER) {
            /* POWER is never grabbed, never locked, never a screen's. */
            if (r_count("power") != 1 || who != SCR_COUNT) err = "POWER did not reach the countdown alone";
            else if (fw_power_saw_gallery || gallery_ui_is_open()) err = "the gallery was not closed before the countdown";
            else if (!fw_power_saw_prompt) err = "the prompt was not on top during the countdown";
            else if (power_prompt_up()) err = "the prompt stayed up";
            else if (r_has("busy") || r_has("cancel") || r_has("shot") || r_has("mode:")) err = "POWER did something else";
            else if (!untouched) err = "POWER set a property";
        } else if (ov == OV_GALLERY) {
            /* The gallery grabs every other key, before the lock and the globals. */
            if (who != SCR_GALLERY) err = "the gallery did not take the key";
            else if (r_has("busy") || r_has("cancel") || r_has("shot") || r_has("mode:") || r_has("power")) err = "something else acted under the gallery";
            else if (screen_base() != before.id[0]) err = "the base changed under the gallery";
        } else if (busy) {
            /* The self-cal lock: AUTO / MENU cancel, the rest Busy; no service. */
            const bool cancels = (b == BTN_AUTO || b == BTN_MENU);
            if (who != SCR_COUNT) err = "a screen was asked during self-cal";
            else if (cancels ? !r_is("touch click cancel") : !r_is("touch click busy post:all")) err = "the lock's answer";
            else if (screen_snapshot().n != before.n || screen_base() != before.id[0]) err = "the stack moved during self-cal";
            else if (!untouched) err = "a property was set during self-cal";
        } else if (lng && (b == BTN_MENU || b == BTN_MOVE || b == BTN_SELECT || b == BTN_TRIGGER)) {
            if (r_count("mode:") != 1 || who != SCR_COUNT || r_count("svc") != 1) err = "a long mode key was not one sys.mode set";
        } else if (b == BTN_SAVE && !lng) {
            if (!r_is("touch click shot svc") || who != SCR_COUNT) err = "short SAVE was not the screenshot";
            else if (screen_snapshot().n != before.n) err = "short SAVE moved the stack";
            else if (!untouched) err = "short SAVE set a property";
        } else if (b == BTN_SAVE && lng) {
            if (screen_top() != SCR_GALLERY || who != SCR_COUNT || !r_is("touch click svc")) err = "long SAVE did not open the gallery";
            else if (screen_snapshot().n != before.n + 1) err = "long SAVE did not push one layer";
        } else if (ov == OV_LOGO) {
            /* The logo has no keys and hides what is under it. */
            if (who != SCR_COUNT || !r_is("touch click")) err = "a key got past the logo";
            else if (!untouched) err = "a property was set under the logo";
        } else {
            /* A screen's key: the top layer if it is a page, else the base. */
            if (who != top0) err = "not the top screen's key";
            else if (r_count("svc") != 1) err = "settings service not once";
            else if (r_has("shot") || r_has("power") || r_has("busy") || r_has("cancel")) err = "a global acted on a screen's key";
        }
        if (err) {
            if (!bad) snprintf(why, sizeof why, "%s, overlay %d, busy %d, key %u: %s [%s]", st_name[st], ov, busy, raw, err, R);
            bad++;
        }
    }
    printf("    %d cases\n", cases);
    if (bad) printf("    first: %s\n", why);
    CHECK("POWER never grabbed or locked; the gallery first; the lock; the globals; then the top screen", bad == 0);
    CHECK("... over 11 screens and pages x (nothing, gallery, logo) x self-cal x 29 keys", cases == 11 * 3 * 2 * 29);
}

static void test_fw_lock_and_service(void)
{
    printf("the self-cal lock, the repaint, the settings service\n");
    fw_boot(ST_SCOPE, OV_NONE, true);
    fw_key(BTN_UP);
    CHECK("a key during self-cal: the popup \"Busy: AUTO cancels\", one repaint, no settings service",
          strcmp(g_fw_popup, "Busy: AUTO cancels") == 0 && r_is("touch click busy post:all"));
    fw_boot(ST_SCOPE, OV_NONE, true);
    fw_key(BTN_AUTO);
    CHECK("AUTO during self-cal cancels, nothing posted", r_is("touch click cancel"));
    fw_boot(ST_HOME, OV_NONE, false);
    CHECK("a key a screen answers SCR_DRAW: that screen in place (DCMD_DRAW_ACTIVE + its id), then the service",
          env_key(BTN_LEFT) == SCR_DRAW && r_is("touch click post:0 svc"));
    fw_boot(ST_HOME, OV_NONE, false);
    fw_key(BTN_UP);
    CHECK("a key that changes nothing on screen: no post, the service once", r_is("touch click svc"));
    fw_boot(ST_SCOPE_PAGE, OV_NONE, false);
    fw_key(BTN_MENU);                                            /* the page closes itself */
    CHECK("a layer that closes under its key: everything repainted, posted for the layer that took the key",
          r_is("touch click post:all svc") && screen_last_handler() == SCR_OSC_MENU && stack_is("scope"));
    fw_boot(ST_SCOPE, OV_NONE, false);
    (void)prop_set_local(P(VIEW_CURSOR), 1, ORIGIN_SHELL, NULL);
    scope_key_focus = KF_CURSOR;
    (void)prop_set_local(P(VIEW_CURSOR), 0, ORIGIN_SHELL, NULL);
    fw_key(BTN_SAVE);
    CHECK("an arrow stage that no longer exists (cursors switched off from the shell) is dropped by any key, a global too",
          scope_key_focus != KF_CURSOR);
}

static void test_fw_display_layers(void)
{
    printf("which layer the display task paints\n");
    static const struct { fw_state_t st; fw_over_t ov; const screen_t *page, *modal; } T[] = {
        { ST_HOME,         OV_NONE,    NULL,                   NULL },
        { ST_SCOPE,        OV_NONE,    NULL,                   NULL },
        { ST_SCOPE_PAGE,   OV_NONE,    &osc_menu_screen,       NULL },
        { ST_SCOPE_PANE,   OV_NONE,    &osc_menu_screen,       NULL },
        { ST_SET,          OV_NONE,    NULL,                   NULL },
        { ST_SET_QUESTION, OV_NONE,    NULL,                   NULL },
        { ST_SET_SOUND,    OV_NONE,    &settings_sound_screen, NULL },
        { ST_SET_ABOUT,    OV_NONE,    &settings_about_screen, NULL },
        { ST_METER,        OV_GALLERY, NULL,                   &gallery_screen },
        { ST_SCOPE_PAGE,   OV_GALLERY, &osc_menu_screen,       &gallery_screen },
        { ST_SET_SOUND,    OV_LOGO,    &settings_sound_screen, &logo_screen },
        { ST_GEN,          OV_LOGO,    NULL,                   &logo_screen },
    };
    int ok = 1;
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
        fw_boot(T[i].st, T[i].ov, false);
        ok &= screen_page() == T[i].page && screen_modal() == T[i].modal;
        ok &= screen_bars_wanted() == (T[i].modal == NULL);
    }
    CHECK("the page between the bars and the layer that owns the LCD, from the stack, in every state", ok);
    fw_boot(ST_SCOPE_PAGE, OV_GALLERY, false);
    power_prompt_show(POWER_PROMPT_AUTOOFF, 0);
    CHECK("the POWER prompt over the gallery over the page: the prompt owns the LCD, the page is still the page",
          screen_modal() == &power_prompt_screen && screen_page() == &osc_menu_screen);
    fw_boot(ST_SCOPE, OV_LOGO, false);
    fw_key(LONG(BTN_SAVE));
    CHECK("the gallery opened under `logo show` goes over the logo, and is what is shown",
          stack_is("scope>logo>gallery") && screen_modal() == &gallery_screen);
    logo_screen_hide();
    CHECK("... and the logo closing takes it along (a layer cannot outlive the one under it)", stack_is("scope"));
}

int main(void)
{
    printf("screen router\n");
    test_registry_and_init();
    test_none_and_activity();
    test_order_base();
    test_overlay_over_base();
    test_grab();
    test_layer_close();
    test_close_and_pop();
    test_switch();
    test_opaque_ticks_draw();
    test_snapshot();
    test_locking_and_reentry();
    test_fw_table();
    test_fw_stack_authority();
    test_fw_mode_switch();
    test_fw_key_table();
    test_fw_global_order();
    test_fw_lock_and_service();
    test_fw_display_layers();
    printf("\n%d/%d passed\n", passed, run);
    return passed == run ? 0 : 1;
}
