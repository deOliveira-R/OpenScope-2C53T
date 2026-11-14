/*
 * Host test for the screen core (src/ui/screen.c): the router order, overlays
 * over a base, modal capture, close, switch and the snapshot. The screens are
 * fakes that log what they were asked; the router code is the firmware's.
 *
 * Build: see `make test-screen-router`.
 */

#include <stdio.h>
#include <string.h>
#include "screen.h"

static int run, passed;
#define CHECK(name, cond) do { run++; if (cond) { passed++; printf("  %-62s[PASS]\n", name); } \
                               else { printf("  %-62s[FAIL]\n", name); } } while (0)

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
CB(SCR_HOME) CB(SCR_SCOPE) CB(SCR_METER) CB(SCR_SETTINGS) CB(SCR_SET_OSC)
CB(SCR_OSC_MENU) CB(SCR_GALLERY) CB(SCR_LOGO) CB(SCR_POWER_PROMPT) CB(SCR_SET_ABOUT)

static void describe_osc(char *out, size_t n) { snprintf(out, n, "row 2 pane 1"); }

#define SCREEN(id, nm, md, ins, fl, grp, desc) \
    { nm, id, md, ins, fl, open_##id, close_##id, key_##id, draw_##id, tick_##id, epoch_##id, desc, grp }

static const screen_t S_HOME     = SCREEN(SCR_HOME,     "home",     MODE_HOME,         0xFF, 0, 0x01, NULL);
static const screen_t S_SCOPE    = SCREEN(SCR_SCOPE,    "scope",    MODE_OSCILLOSCOPE, 3,    0, 0x02, NULL);
static const screen_t S_METER    = SCREEN(SCR_METER,    "meter",    MODE_MULTIMETER,   0xFF, 0, 0x04, NULL);
static const screen_t S_SETTINGS = SCREEN(SCR_SETTINGS, "settings", MODE_SETTINGS,     0xFF, 0, 0x08, NULL);
static const screen_t S_SET_OSC  = SCREEN(SCR_SET_OSC,  "set_osc",  MODE_COUNT,        0xFF, SCR_F_CHILD, 0x10, NULL);
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
    const screen_t *all[] = { &S_HOME, &S_SCOPE, &S_METER, &S_SETTINGS, &S_SET_OSC, &S_OSC_MENU,
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
    CHECK("init: an unregistered id is refused", !screen_init(SCR_SET_SOUND));
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
    screen_push(SCR_SET_OSC);
    F[SCR_SET_OSC].key_res = SCR_DRAW;
    screen_route_key(BTN_DOWN);
    CHECK("child on top gets the key, the base does not", F[SCR_SET_OSC].keys == 1 && F[SCR_SETTINGS].keys == 0);

    F[SCR_SET_OSC].key_res = SCR_UNHANDLED; F[SCR_SETTINGS].key_res = SCR_HANDLED;
    CHECK("a non-opaque layer that leaves the key passes it down", screen_route_key(BTN_DOWN) == SCR_HANDLED &&
          F[SCR_SET_OSC].keys == 2 && F[SCR_SETTINGS].keys == 1);

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
    CHECK("grab: no lock check, no globals when it handled",
          g_locked_calls == 0 && g_globals_calls == 0 && F[SCR_SCOPE].keys == 0);

    clear_log();
    F[SCR_OSC_MENU].key_res = SCR_UNHANDLED; g_global_any = true;
    CHECK("grab leaves the key: globals only", screen_route_key(BTN_MENU | BTN_LONG_FLAG) == SCR_HANDLED &&
          g_globals_calls == 1 && g_locked_calls == 0 && F[SCR_SCOPE].keys == 0);
    CHECK("grab then globals order", LOGIS("touch click key:SCR_OSC_MENU0 globals after"));

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
    CHECK("after_key ran for the close", g_after >= 1 && g_after_last == SCR_DRAW_FULL);
    F[SCR_SCOPE].key_res = SCR_HANDLED;
    CHECK("after the pop the base gets keys again", screen_route_key(BTN_UP) == SCR_HANDLED && F[SCR_SCOPE].keys == 1);
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
    screen_push(SCR_SET_OSC); screen_push(SCR_LOGO);
    F[SCR_LOGO].key_res = SCR_UNHANDLED; F[SCR_SET_OSC].key_res = SCR_CLOSE;
    CHECK("a lower layer's CLOSE (key passed down) pops it and the one above",
          screen_route_key(BTN_OK) == SCR_DRAW_FULL && screen_top() == SCR_SCOPE &&
          F[SCR_SET_OSC].closes == 1 && F[SCR_LOGO].closes == 1);

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
    CHECK("push an unregistered screen: refused", !screen_push(SCR_SET_SOUND));
    CHECK("close is idempotent: first closes, second is a no-op",
          screen_close(SCR_LOGO) && !screen_close(SCR_LOGO) && F[SCR_LOGO].closes == 1);
    CHECK("close posts once", g_stack_posts == 2);

    screen_push(SCR_SET_OSC); screen_push(SCR_SET_ABOUT); screen_push(SCR_LOGO);
    CHECK("depth cap: the stack is full at SCR_STACK_MAX", screen_snapshot().n == SCR_STACK_MAX && !screen_push(SCR_POWER_PROMPT));
    clear_log();
    CHECK("close of a lower layer closes the ones above it, top first",
          screen_close(SCR_SET_OSC) && screen_snapshot().n == 1);
    CHECK("pop order was top-down (logo, about, osc)", strstr(g_log, "SCR_LOGO") < strstr(g_log, "SCR_SET_ABOUT") &&
          strstr(g_log, "SCR_SET_ABOUT") < strstr(g_log, "SCR_SET_OSC"));

    screen_push(SCR_LOGO);
    screen_pop();
    CHECK("pop closes the top and names the one below", F[SCR_LOGO].last_close_to == SCR_SCOPE && screen_snapshot().n == 1);
}

static void test_switch(void)
{
    printf("switch\n");
    setup(SCR_SETTINGS);
    screen_push(SCR_SET_OSC); screen_push(SCR_LOGO);
    clear_log(); g_stack_posts = 0;
    screen_switch(SCR_METER);
    CHECK("switch: layers close top-down, then the old base closes, then the new opens",
          strstr(g_log, "close:SCR_LOGO") && strstr(g_log, "close:SCR_LOGO") < strstr(g_log, "close:SCR_SET_OSC") &&
          strstr(g_log, "close:SCR_SET_OSC") < strstr(g_log, "close:SCR_SETTINGS") &&
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
    screen_switch(SCR_SET_SOUND);
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
    screen_push(SCR_SET_OSC); screen_push(SCR_OSC_MENU);
    screen_stack_t s = screen_snapshot();
    CHECK("snapshot: ids bottom-up", s.n == 3 && s.id[0] == SCR_SETTINGS && s.id[1] == SCR_SET_OSC && s.id[2] == SCR_OSC_MENU);
    CHECK("snapshot: generation moves on every change", s.gen == s0.gen + 2);
    screen_close(SCR_OSC_MENU); screen_close(SCR_OSC_MENU);
    CHECK("an idempotent close does not move the generation", screen_snapshot().gen == s.gen + 1);

    char buf[128];
    screen_snapshot_text(buf, sizeof buf);
    CHECK("text: names and generation", strcmp(buf, "settings > set_osc gen=4") == 0);
    screen_push(SCR_OSC_MENU);
    screen_snapshot_text(buf, sizeof buf);
    CHECK("text: the top's describe() is appended", strcmp(buf, "settings > set_osc > osc_menu gen=5 | row 2 pane 1") == 0);
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

/* ═══ the legacy screens (ARCH-1 B1): the firmware's table and hooks ═══════
 *
 * src/ui/legacy_screens.c and the key model are compiled into this test as
 * they are; their environment (the lgc_* handlers input_handler.c provides)
 * is faked below and logs which pre-router handler a key reached. The
 * expectation is old_route(): the dispatch order of input_handle_button() as
 * it was before the router (base 6a8c2ba), transcribed step by step. Every key,
 * short and long, in every mode, settings page and overlay must reach the same
 * handler with the same action in both. */

#include "../src/ui/key_model.c"
#include "../src/ui/legacy_screens.c"

typedef struct {
    device_mode_t mode;
    int8_t        depth;
    bool          osc_open, osc_locked;
    uint8_t       opener;
    bool          gallery, busy, editor, tv, cursors, mask;
    key_focus_t   focus;
    uint8_t       ach;
} lw_t;

static lw_t W;
static char LG[256];
static int  lg_lock;
static uint8_t gallery_close_key = 0xFF;   /* raw key the fake gallery closes on */

static void lg(const char *fmt, int a, int b, int c)
{
    char t[40];
    snprintf(t, sizeof t, fmt, a, b, c);
    if (strlen(LG) + strlen(t) + 2 < sizeof LG) { strcat(LG, t); strcat(LG, " "); }
}

/* Two handler-side facts the order depends on, the same for both paths:
 * softkey_slot_for_button() (softkey.c) and siggen_edit_input()'s claim. */
static bool softkey_takes(button_id_t b)
{
    return b == BTN_MOVE || b == BTN_SELECT || b == BTN_TRIGGER || b == BTN_PRM;
}
static bool editor_takes(button_id_t b)
{
    if (W.mode != MODE_SIGNAL_GEN) return false;
    if (!W.editor) return b == BTN_PRM;
    switch (b) {
    case BTN_LEFT: case BTN_RIGHT: case BTN_UP: case BTN_DOWN:
    case BTN_OK: case BTN_PRM: case BTN_MENU: return true;
    default: return false;
    }
}

static key_ctx_t w_ctx(void)
{
    key_ctx_t c;
    memset(&c, 0, sizeof c);
    c.mode = W.mode; c.settings_depth = W.depth;
    c.scope_time_view = (W.mode == MODE_OSCILLOSCOPE) && W.tv;
    c.cursors_on = W.cursors; c.mask_present = W.mask;
    c.focus = key_focus_sanitize(W.focus, W.cursors, W.mask);
    c.active_channel = W.ach;
    c.osc_menu_open = (W.mode == MODE_OSCILLOSCOPE) && W.osc_open;
    c.osc_menu_locked = c.osc_menu_open && W.osc_locked;
    c.osc_menu_opener = W.opener;
    return c;
}

void lgc_state(lgc_state_t *s)
{
    s->mode = W.mode; s->settings_depth = W.depth; s->osc_open = W.osc_open; s->gallery_open = W.gallery;
}
key_ctx_t lgc_ctx(void)                      { lg("ctx", 0, 0, 0); return w_ctx(); }   /* key_ctx_now */
void lgc_exec(key_cmd_t k, button_id_t btn)  { lg("exec:%d:%d:%d", k.act, k.arg, btn); }
void lgc_home(button_id_t b)                 { lg("home:%d", b, 0, 0); }
bool lgc_meter_softkey(button_id_t b)        { if (!softkey_takes(b)) return false; lg("softkey:%d", b, 0, 0); return true; }
bool lgc_siggen_edit(button_id_t b)          { if (!editor_takes(b)) return false; lg("edit:%d", b, 0, 0); return true; }
void lgc_mode_key(button_id_t b)             { lg("switch:%d", b, 0, 0); }
void lgc_mode_set(device_mode_t m)           { lg("mode:%d", m, 0, 0); }   /* sys.mode, ORIGIN_KEY */
void lgc_gallery_key(uint8_t raw)
{
    lg("gallery:%d", raw, 0, 0);
    if ((raw & (uint8_t)~BTN_LONG_FLAG) == BTN_POWER || raw == gallery_close_key) W.gallery = false;
}
bool lgc_selfcal_busy(void)   { return W.busy; }
void lgc_selfcal_cancel(void) { lg("cancel", 0, 0, 0); }
void lgc_selfcal_refuse(void) { lg("busy", 0, 0, 0); }
void lgc_touch(void)          { lg("touch", 0, 0, 0); }
void lgc_click(void)          { lg("click", 0, 0, 0); }
void lgc_lock(void)           { lg_lock++; }
void lgc_unlock(void)         { lg_lock--; }
void ui_describe_osc_menu(char *out, size_t n) { snprintf(out, n, "row=ch1 pane=rail item=0"); }
void ui_describe_settings(char *out, size_t n) { snprintf(out, n, "settings=%d/0/0", (int)W.depth); }
void ui_describe_home(char *out, size_t n)     { snprintf(out, n, "home=scope"); }

/* input_handle_button() before the router (base 6a8c2ba), step by step. */
static void old_route(uint8_t raw)
{
    const button_id_t b = (button_id_t)(raw & (uint8_t)~BTN_LONG_FLAG);
    const bool lng = (raw & BTN_LONG_FLAG) != 0;
    lg("touch", 0, 0, 0); lg("click", 0, 0, 0);                 /* autooff_touch, sys_pwm_click */
    if (W.gallery) {                                             /* gallery_ui_key: all but POWER */
        lgc_gallery_key(raw);
        if (b != BTN_POWER) return;
    }
    if (W.busy) {                                                /* scope_selfcal_busy() */
        if (b == BTN_AUTO || b == BTN_MENU) { lg("cancel", 0, 0, 0); return; }
        if (b != BTN_POWER) { lg("busy", 0, 0, 0); return; }
    }
    const key_ctx_t c = lgc_ctx();                               /* key_ctx_now(), once */
    if (lng) {                                                   /* every long key */
        const key_cmd_t k = key_map(&c, b, true);
        /* exec_key_cmd's KA_ENTER_* cases: enter_mode then, a sys.mode set
         * now (the same input_mode_switch -> enter_mode underneath). */
        switch (k.act) {
        case KA_ENTER_HOME:   lg("mode:%d", MODE_HOME, 0, 0);         return;
        case KA_ENTER_METER:  lg("mode:%d", MODE_MULTIMETER, 0, 0);   return;
        case KA_ENTER_SCOPE:  lg("mode:%d", MODE_OSCILLOSCOPE, 0, 0); return;
        case KA_ENTER_SIGGEN: lg("mode:%d", MODE_SIGNAL_GEN, 0, 0);   return;
        default:              lgc_exec(k, b);                         return;
        }
    }
    const key_cmd_t k = key_map(&c, b, false);
    if (k.act != KA_LEGACY) { lgc_exec(k, b); return; }          /* key_map's short claims */
    if (W.mode == MODE_HOME && b != BTN_POWER) { lgc_home(b); return; }
    if (W.mode == MODE_MULTIMETER && softkey_takes(b)) { lg("softkey:%d", b, 0, 0); return; }
    if (editor_takes(b)) { lg("edit:%d", b, 0, 0); return; }    /* siggen_edit_input */
    lgc_mode_key(b);                                             /* switch (button) */
}

static void new_route(uint8_t raw)
{
    legacy_screens_sync();
    (void)screen_route_key((button_id_t)raw);
    legacy_screens_sync();
}

/* Does the stack mirror W? */
static bool stack_mirrors(void)
{
    screen_stack_t st = screen_snapshot();
    screen_id_t want[SCR_STACK_MAX];
    uint8_t n = 0;
    want[n++] = screen_base_for_mode(W.mode);
    if (W.mode == MODE_SETTINGS && W.depth > 0) want[n++] = legacy_settings_page(W.depth);
    if (W.mode == MODE_OSCILLOSCOPE && W.osc_open) want[n++] = SCR_OSC_MENU;
    if (W.gallery) want[n++] = SCR_GALLERY;
    if (st.n != n) return false;
    for (uint8_t i = 0; i < n; i++) if (st.id[i] != want[i]) return false;
    return true;
}

static int compare_one(uint8_t raw, char *why, size_t nwhy)
{
    const lw_t saved = W;
    LG[0] = '\0';
    old_route(raw);
    char want[sizeof LG];
    strcpy(want, LG);
    const lw_t after_old = W;
    W = saved;
    legacy_screens_sync();
    LG[0] = '\0';
    new_route(raw);
    const bool ok = strcmp(want, LG) == 0 && W.gallery == after_old.gallery && stack_mirrors();
    if (!ok && why[0] == '\0')
        snprintf(why, nwhy, "mode %d depth %d osc %d/%d op %d gal %d busy %d ed %d tv %d key %u: want [%s] got [%s]",
                 saved.mode, saved.depth, saved.osc_open, saved.osc_locked, saved.opener, saved.gallery,
                 saved.busy, saved.editor, saved.tv, raw, want, LG);
    W = saved;
    return ok ? 0 : 1;
}

/* Every key: 15 short, and long for the keys the scan gives a long to. */
static int all_keys(int *cases, char *why, size_t nwhy)
{
    int bad = 0;
    for (int b = BTN_CH1; b <= BTN_POWER; b++) {
        bad += compare_one((uint8_t)b, why, nwhy); (*cases)++;
        static const uint8_t long_keys[] = { BTN_AUTO, BTN_CH1, BTN_MOVE, BTN_SELECT, BTN_TRIGGER,
                                             BTN_PRM, BTN_CH2, BTN_SAVE, BTN_MENU, BTN_OK };
        for (unsigned i = 0; i < sizeof long_keys; i++)
            if (long_keys[i] == b) { bad += compare_one((uint8_t)(b | BTN_LONG_FLAG), why, nwhy); (*cases)++; }
    }
    return bad;
}

static void test_legacy_equivalence(void)
{
    printf("legacy screens: every key routed as before the router\n");
    screen_reset();
    memset(&W, 0, sizeof W);
    W.mode = MODE_OSCILLOSCOPE; W.tv = true; W.opener = BTN_NONE;
    legacy_screens_init();
    CHECK("init: the base of the current mode", screen_base() == SCR_SCOPE && screen_top() == SCR_SCOPE);

    /* The page as the keys can leave it (opener and lock come in pairs). */
    static const struct { bool open, locked; uint8_t opener; } osc[] = {
        { false, false, BTN_NONE }, { true, false, BTN_MENU }, { true, false, BTN_CH1 },
        { true, true, BTN_PRM }, { true, true, BTN_TRIGGER },
    };
    static const struct { key_focus_t f; bool cursors, mask; } foc[] = {
        { KF_ZOOM, false, false }, { KF_TRIGMOVE, false, false }, { KF_CURSOR, true, false },
        { KF_MASK, false, true },
    };
    int bad = 0, cases = 0;
    char why[400] = "";
    for (int mode = 0; mode < MODE_COUNT; mode++)
    for (int depth = 0; depth < (mode == MODE_SETTINGS ? 8 : 1); depth++)
    for (unsigned o = 0; o < sizeof osc / sizeof osc[0]; o++)
    for (int gal = 0; gal < 2; gal++)
    for (int busy = 0; busy < 2; busy++)
    for (int ed = 0; ed < (mode == MODE_SIGNAL_GEN ? 2 : 1); ed++)
    for (int tv = 0; tv < (mode == MODE_OSCILLOSCOPE ? 2 : 1); tv++)
    for (unsigned f = 0; f < (mode == MODE_OSCILLOSCOPE ? sizeof foc / sizeof foc[0] : 1u); f++) {
        /* Outside the scope only "closed" and a page left open by a shell
         * mode switch (it is not the scope's, so not on the stack). */
        if (mode != MODE_OSCILLOSCOPE && o > 1) continue;
        memset(&W, 0, sizeof W);
        W.mode = (device_mode_t)mode; W.depth = (int8_t)depth;
        W.osc_open = osc[o].open; W.osc_locked = osc[o].locked; W.opener = osc[o].opener;
        W.gallery = gal; W.busy = busy; W.editor = ed; W.tv = tv;
        W.focus = foc[f].f; W.cursors = foc[f].cursors; W.mask = foc[f].mask;
        bad += all_keys(&cases, why, sizeof why);
    }
    printf("    %d cases\n", cases);
    if (bad) printf("    first difference: %s\n", why);
    CHECK("every key, short and long, every mode/page/overlay: same handler, same action",
          bad == 0 && cases > 5000);
    CHECK("the injected lock is balanced", lg_lock == 0);
}

/* A table to read: one row per meaning that moved in the router (globals,
 * grab, lock), with the handler log it must produce. */
static void test_legacy_table(void)
{
    printf("legacy screens: the expectation table\n");
    static const struct {
        const char *what; device_mode_t mode; int8_t depth; bool osc, locked, gal, busy, ed;
        uint8_t raw; const char *want;
    } T[] = {
        { "long MOVE in the scope -> meter (global)",     MODE_OSCILLOSCOPE, 0, 0, 0, 0, 0, 0, BTN_MOVE | BTN_LONG_FLAG,    "touch click ctx mode:1" },
        { "long SELECT in the meter -> scope (global)",   MODE_MULTIMETER,   0, 0, 0, 0, 0, 0, BTN_SELECT | BTN_LONG_FLAG,  "touch click ctx mode:0" },
        { "long TRIGGER on home -> generator (global)",   MODE_HOME,         0, 0, 0, 0, 0, 0, BTN_TRIGGER | BTN_LONG_FLAG, "touch click ctx mode:2" },
        { "long MENU on a settings page -> home",         MODE_SETTINGS,     3, 0, 0, 0, 0, 0, BTN_MENU | BTN_LONG_FLAG,    "touch click ctx mode:4" },
        { "long MOVE under the osc page -> meter",        MODE_OSCILLOSCOPE, 0, 1, 0, 0, 0, 0, BTN_MOVE | BTN_LONG_FLAG,    "touch click ctx mode:1" },
        { "long SAVE in the generator -> gallery",        MODE_SIGNAL_GEN,   0, 0, 0, 0, 0, 0, BTN_SAVE | BTN_LONG_FLAG,    "touch click ctx exec:10:0:8" },
        { "short SAVE on home -> screenshot",             MODE_HOME,         0, 0, 0, 0, 0, 0, BTN_SAVE,                    "touch click ctx exec:11:0:8" },
        { "long OK in the scope -> 50 % centre",          MODE_OSCILLOSCOPE, 0, 0, 0, 0, 0, 0, BTN_OK | BTN_LONG_FLAG,      "touch click ctx exec:13:0:14" },
        { "long AUTO in the scope -> baseline",           MODE_OSCILLOSCOPE, 0, 0, 0, 0, 0, 0, BTN_AUTO | BTN_LONG_FLAG,    "touch click ctx exec:12:0:7" },
        { "long CH2 in the scope -> CH2 on/off",          MODE_OSCILLOSCOPE, 0, 0, 0, 0, 0, 0, BTN_CH2 | BTN_LONG_FLAG,     "touch click ctx exec:7:1:2" },
        { "long CH1 under the osc page: nothing",         MODE_OSCILLOSCOPE, 0, 1, 0, 0, 0, 0, BTN_CH1 | BTN_LONG_FLAG,     "touch click ctx exec:1:0:1" },
        { "long OK in the generator -> run/stop",         MODE_SIGNAL_GEN,   0, 0, 0, 0, 0, 0, BTN_OK | BTN_LONG_FLAG,      "touch click ctx exec:9:0:14" },
        { "POWER in the meter -> the countdown",          MODE_MULTIMETER,   0, 0, 0, 0, 0, 0, BTN_POWER,                   "touch click ctx switch:15" },
        { "POWER on home -> the countdown",               MODE_HOME,         0, 0, 0, 0, 0, 0, BTN_POWER,                   "touch click ctx switch:15" },
        { "POWER with the gallery up: closes it, then",   MODE_OSCILLOSCOPE, 0, 0, 0, 1, 0, 0, BTN_POWER,                   "touch click gallery:15 ctx switch:15" },
        { "long MOVE with the gallery up: the gallery's", MODE_OSCILLOSCOPE, 0, 0, 0, 1, 0, 0, BTN_MOVE | BTN_LONG_FLAG,    "touch click gallery:131" },
        { "long MOVE during autoset: Busy",               MODE_OSCILLOSCOPE, 0, 1, 0, 0, 1, 0, BTN_MOVE | BTN_LONG_FLAG,    "touch click busy" },
        { "long MENU during autoset: cancel",             MODE_OSCILLOSCOPE, 0, 0, 0, 0, 1, 0, BTN_MENU | BTN_LONG_FLAG,    "touch click cancel" },
        { "POWER during autoset passes",                  MODE_OSCILLOSCOPE, 0, 0, 0, 0, 1, 0, BTN_POWER,                   "touch click ctx switch:15" },
        { "UP under the osc page: the page",              MODE_OSCILLOSCOPE, 0, 1, 0, 0, 0, 0, BTN_UP,                      "touch click ctx exec:28:0:10" },
        { "MENU in the scope opens the page",             MODE_OSCILLOSCOPE, 0, 0, 0, 0, 0, 0, BTN_MENU,                    "touch click ctx exec:26:0:9" },
        { "MOVE in the meter: softkey 0",                 MODE_MULTIMETER,   0, 0, 0, 0, 0, 0, BTN_MOVE,                    "touch click ctx softkey:3" },
        { "LEFT in the meter: the switch",                MODE_MULTIMETER,   0, 0, 0, 0, 0, 0, BTN_LEFT,                    "touch click ctx switch:12" },
        { "PRM in the generator: the editor",             MODE_SIGNAL_GEN,   0, 0, 0, 0, 0, 0, BTN_PRM,                     "touch click ctx edit:6" },
        { "UP in the generator, editor closed: switch",   MODE_SIGNAL_GEN,   0, 0, 0, 0, 0, 0, BTN_UP,                      "touch click ctx switch:10" },
        { "UP in the generator, editor open: editor",     MODE_SIGNAL_GEN,   0, 0, 0, 0, 0, 1, BTN_UP,                      "touch click ctx edit:10" },
        { "OK on home: home",                             MODE_HOME,         0, 0, 0, 0, 0, 0, BTN_OK,                      "touch click ctx home:14" },
        { "MENU on Settings > Oscilloscope: switch",      MODE_SETTINGS,     1, 0, 0, 0, 0, 0, BTN_MENU,                    "touch click ctx switch:9" },
        { "short MOVE in settings: switch",               MODE_SETTINGS,     0, 0, 0, 0, 0, 0, BTN_MOVE,                    "touch click ctx switch:3" },
    };
    _Static_assert(KA_ENTER_HOME == 2 && KA_ENTER_METER == 3 && KA_ENTER_SCOPE == 4 && KA_ENTER_SIGGEN == 5 &&
                   KA_CH_ENABLE_TOGGLE == 7 && KA_SIGGEN_RUNSTOP == 9 && KA_GALLERY_OPEN == 10 &&
                   KA_SCREENSHOT == 11 && KA_BASECAL == 12 && KA_CENTER50 == 13 && KA_OSCMENU_OPEN == 26 &&
                   KA_OSCMENU_KEY == 28 && KA_NONE == 1, "the action numbers the table spells");
    int bad = 0;
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
        memset(&W, 0, sizeof W);
        W.mode = T[i].mode; W.depth = T[i].depth; W.osc_open = T[i].osc; W.osc_locked = T[i].locked;
        W.opener = T[i].osc ? BTN_MENU : BTN_NONE; W.gallery = T[i].gal; W.busy = T[i].busy;
        W.editor = T[i].ed; W.tv = true;
        const lw_t saved = W;
        LG[0] = '\0'; old_route(T[i].raw);
        char o[sizeof LG]; strcpy(o, LG);
        W = saved; legacy_screens_sync();
        LG[0] = '\0'; new_route(T[i].raw);
        char want[80]; snprintf(want, sizeof want, "%s ", T[i].want);
        if (strcmp(o, want) != 0 || strcmp(LG, want) != 0) {
            bad++;
            printf("    %s: want [%s] old [%s] new [%s]\n", T[i].what, T[i].want, o, LG);
        }
    }
    CHECK("each row: the old order and the router give the logged handler", bad == 0);
}

static void test_legacy_mirror(void)
{
    printf("legacy screens: the stack mirrors the legacy state\n");
    memset(&W, 0, sizeof W);
    W.mode = MODE_OSCILLOSCOPE; W.tv = true;
    legacy_screens_init();
    const uint32_t g0 = screen_snapshot().gen;
    legacy_screens_sync();
    CHECK("sync with nothing changed leaves the stack (gen) alone", screen_snapshot().gen == g0);

    W.osc_open = true; W.opener = BTN_MENU; legacy_screens_sync();
    CHECK("page open -> scope > osc_menu", stack_mirrors() && screen_top() == SCR_OSC_MENU);
    W.gallery = true; legacy_screens_sync();
    CHECK("gallery over the page -> scope > osc_menu > gallery", stack_mirrors() && screen_top() == SCR_GALLERY);
    W.mode = MODE_MULTIMETER; W.gallery = false; legacy_screens_sync();   /* shell `mode meter` */
    CHECK("shell mode switch + gallery close -> meter alone (stale page not shown)",
          stack_mirrors() && screen_base() == SCR_METER && screen_top() == SCR_METER);
    W.mode = MODE_SETTINGS; W.depth = 3; W.osc_open = false; legacy_screens_sync();
    CHECK("settings depth 3 -> settings > set_math", stack_mirrors() && screen_top() == SCR_SET_MATH);
    W.depth = 7; legacy_screens_sync();
    CHECK("depth 3 -> 7 swaps the page", stack_mirrors() && screen_top() == SCR_SET_SOUND);
    W.depth = 0; legacy_screens_sync();
    CHECK("depth 0 -> the settings base", stack_mirrors() && screen_top() == SCR_SETTINGS);
    W.depth = 5;
    CHECK("depth 4 and 5 are both the component page",
          legacy_settings_page(4) == SCR_SET_COMPONENT && legacy_settings_page(5) == SCR_SET_COMPONENT);

    /* The gallery closing itself pops its layer within the same key. */
    W.mode = MODE_OSCILLOSCOPE; W.depth = 0; W.gallery = true; legacy_screens_sync();
    gallery_close_key = BTN_MENU;
    (void)screen_route_key(BTN_MENU);
    CHECK("gallery closes on its key -> its layer is popped by the router",
          !W.gallery && screen_top() == SCR_SCOPE);
    gallery_close_key = 0xFF;

    /* Instruments: a base's instrument id is the sys.instrument value. */
    CHECK("bases by instrument: 0 scope, 1 meter, 2 generator; home/settings none",
          screen_base_for_instrument(0) == SCR_SCOPE && screen_base_for_instrument(1) == SCR_METER &&
          screen_base_for_instrument(2) == SCR_SIGGEN && screen_base_for_instrument(3) == SCR_COUNT &&
          screen_get(SCR_HOME)->instrument == SCR_INSTRUMENT_NONE &&
          screen_get(SCR_SETTINGS)->instrument == SCR_INSTRUMENT_NONE);
    CHECK("names are the sys.mode labels", strcmp(screen_get(SCR_SCOPE)->name, "scope") == 0 &&
          strcmp(screen_get(SCR_SIGGEN)->name, "siggen") == 0 && strcmp(screen_get(SCR_HOME)->name, "home") == 0);

    /* DCMD_DRAW_ACTIVE: each screen's in-place paint. */
    W.mode = MODE_MULTIMETER; W.depth = 0; W.gallery = false; W.osc_open = false; legacy_screens_sync();
    const uint8_t act = legacy_screens_active_cmd();
    CHECK("DRAW_ACTIVE carries the sender's screen and resolves to its command",
          act == DCMD_DRAW_ACTIVE + SCR_METER && legacy_screens_resolve_cmd(act) == DCMD_DRAW_METER &&
          legacy_screens_resolve_cmd(DCMD_DRAW_ACTIVE + SCR_HOME) == DCMD_DRAW_HOME &&
          legacy_screens_resolve_cmd(DCMD_REDRAW_ALL) == DCMD_REDRAW_ALL &&
          legacy_screens_resolve_cmd(DCMD_NONE_MODAL_DROP) == DCMD_NONE_MODAL_DROP &&
          legacy_screens_resolve_cmd(DCMD_DRAW_ACTIVE + SCR_COUNT) == DCMD_NONE_MODAL_DROP);
    W.mode = MODE_OSCILLOSCOPE; legacy_screens_sync();
    CHECK("DRAW_ACTIVE -> the screen's own draw command",
          legacy_screens_update_cmd(SCR_SCOPE) == DCMD_DRAW_SCOPE &&
          legacy_screens_update_cmd(SCR_METER) == DCMD_DRAW_METER &&
          legacy_screens_update_cmd(SCR_SIGGEN) == DCMD_DRAW_SIGGEN &&
          legacy_screens_update_cmd(SCR_HOME) == DCMD_DRAW_HOME &&
          legacy_screens_update_cmd(SCR_OSC_MENU) == DCMD_DRAW_OSC_MENU &&
          legacy_screens_update_cmd(SCR_SET_SOUND) == DCMD_DRAW_SETTINGS &&
          legacy_screens_update_cmd(SCR_SETTINGS) == DCMD_DRAW_SETTINGS &&
          legacy_screens_update_cmd(SCR_GALLERY) == DCMD_NONE_MODAL_DROP);

    char t[160];
    W.osc_open = true; W.gallery = false; legacy_screens_sync();
    screen_snapshot_text(t, sizeof t);
    CHECK("snapshot text: base > page, and the page's describe", strstr(t, "scope > osc_menu gen=") == t &&
          strstr(t, "| row=ch1 pane=rail item=0") != NULL);
    screen_reset();
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
    test_legacy_equivalence();
    test_legacy_table();
    test_legacy_mirror();
    printf("\n%d/%d passed\n", passed, run);
    return passed == run ? 0 : 1;
}
