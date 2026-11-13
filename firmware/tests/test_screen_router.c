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
    printf("\n%d/%d passed\n", passed, run);
    return passed == run ? 0 : 1;
}
