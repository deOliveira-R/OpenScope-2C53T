/*
 * Host tests for the multimeter screen (src/ui/meter_screen.c; ARCH-1 B6):
 * the real screen, the real router (screen.c), the real softkey tables
 * (softkey.c), the real property model with the meter's setters
 * (props_meter.c) and the real fuse tables, with fake hardware ops and a
 * logging paint module (the meter_ui.c side of meter_screen.h).
 *
 * What is checked:
 *   - the key table of meter_screen.h: every key, short and long, on every
 *     softkey bar: the screen's answer, the owner calls, the popup;
 *   - every change of meter state is a property set (the op log and the
 *     paint module's log are the only ways anything moved), is what
 *     prop_get answers at once, bumps the meter epoch and notes the
 *     settings when the property is a saved one;
 *   - the side effects of meter.layout and meter.fusetype are the setters':
 *     the View softkey and `set meter.layout`, the type keys and
 *     `set meter.fusetype` leave the same state and make the same owner
 *     calls, and both match what the key handlers did before B6;
 *   - a set from elsewhere while the screen is up (the model's route, and
 *     the old commands' direct write) is painted by the next tick, in
 *     place, once; a steady reading costs no paint;
 *   - the tick's rule: readings at most every 5 frames, the animation
 *     heartbeat, the autoselect holding the page.
 *
 * Build: make test-meter-screen.
 */
#include "screen_test_env.h"
#include "meter_screen.h"
#include "fuse_model.h"
#include "fuse_table.h"

/* ── the paint module's side (meter_ui.c in the firmware) ──────────── */
static int      g_draws[3];            /* by scr_paint_t */
static uint32_t g_reading_seq, g_rendered_seq, g_gen;
static bool     g_periodic, g_autosel;
static int      g_cancels;
static char     g_popup[64];
static bool     g_lim_valid;
static uint8_t  g_lim_active;

void     meter_ui_draw(scr_paint_t how) { g_draws[how]++; g_rendered_seq = g_reading_seq; }
uint32_t meter_ui_reading_seq(void)     { return g_reading_seq; }
uint32_t meter_ui_rendered_seq(void)    { return g_rendered_seq; }
bool     meter_ui_periodic(void)        { return g_periodic; }
bool     meter_ui_autoselect_running(void) { return g_autosel; }
uint32_t meter_ui_gen(void)             { return g_gen; }
void meter_ui_stats_reset(void)         { env_logf("stats_reset;"); g_gen++; scope_show_popup("Min/Max reset"); }
void meter_ui_limit_press(uint8_t w)    { env_logf("limit_press(%u);", w); g_gen++; g_lim_valid = true; g_lim_active = w; }
void meter_ui_limits_nudge(int8_t d)    { env_logf("limits_nudge(%d);", d); g_gen++; }
void meter_ui_fuse_cal(void)            { env_logf("fuse_cal;"); g_gen++; }
void meter_ui_autoselect_cancel(void)   { g_cancels++; }
void meter_ui_limit_text(uint8_t w, char *b, uint8_t n) { snprintf(b, n, g_lim_valid ? (w ? "1.10" : "0.90") : "--"); }
bool meter_ui_limit_active(uint8_t w)   { return g_lim_valid && g_lim_active == w; }
void meter_ui_cal_text(char *b, uint8_t n) { snprintf(b, n, "not set"); }
bool meter_ui_cal_set(void)             { return false; }
void scope_show_popup(const char *t)    { snprintf(g_popup, sizeof g_popup, "%s", t); }

static const screen_t S_SCOPE = ENV_BASE("scope", SCR_SCOPE, MODE_OSCILLOSCOPE, 0);
static const screen_t S_HOME  = ENV_BASE("home",  SCR_HOME,  MODE_HOME, SCR_INSTRUMENT_NONE);

#define LONG(b) ((uint8_t)((b) | BTN_LONG_FLAG))
#define AUTO_F  (METER_SUBMODE_COUNT - 1)

static int32_t G(const prop_desc_t *p) { return prop_get(p); }

static void boot(void)
{
    env_fresh(MODE_MULTIMETER);
    screen_reset();
    screen_register(&S_SCOPE); screen_register(&S_HOME); screen_register(&meter_screen);
    screen_init(SCR_METER);
    g_draws[0] = g_draws[1] = g_draws[2] = 0;
    g_reading_seq = g_rendered_seq = 7; g_gen = 0; g_periodic = g_autosel = false; g_cancels = 0;
    g_popup[0] = '\0'; g_lim_valid = false; g_lim_active = 0;
    env_log_clear();
}

/* A state set the way the shell does it, then a clean log. */
static void given(const prop_desc_t *p, int32_t v)
{
    (void)prop_set_local(p, v, ORIGIN_SHELL, NULL);
    env_log_clear();
    g_popup[0] = '\0';
}

typedef struct { uint8_t func, layout, fview, ftype, frating; } mstate_t;
static void set_state(const mstate_t *m)
{
    boot();
    /* The globals directly, as a loaded record would: no setter side effect. */
    meter_submode = m->func; meter_layout = m->layout; fuse_view = m->fview;
    fuse_type = m->ftype; fuse_rating_idx = m->frating;
    env_log_clear();
}

/* One key from a state: answer, owner calls, popup ("" = none). */
static bool one(const mstate_t *m, uint8_t raw, scr_res_t want_res, const char *want_log, const char *want_popup)
{
    set_state(m);
    const scr_res_t r = env_key(raw);
    const bool ok = r == want_res && env_log_is(want_log) && strcmp(g_popup, want_popup) == 0;
    if (!ok) printf("    key %u func %u layout %u page %u: answer %d (want %d), log [%s] (want [%s]), popup [%s] (want [%s])\n",
                    raw, m->func, m->layout, m->fview, r, want_res, env_log, want_log, g_popup, want_popup);
    return ok;
}

static const mstate_t DCV      = { 0, METER_LAYOUT_BIG, 0, 0, 6 };
static const mstate_t OHM      = { 6, METER_LAYOUT_BIG, 0, 0, 6 };
static const mstate_t CONT     = { 7, METER_LAYOUT_BIG, 0, 0, 6 };
static const mstate_t TEMP     = { 10, METER_LAYOUT_BIG, 0, 0, 6 };
static const mstate_t STATS    = { 6, METER_LAYOUT_STATS, 0, 0, 6 };
static const mstate_t LIMITS   = { 6, METER_LAYOUT_LIMITS, 0, 0, 6 };
static const mstate_t FUSE     = { 0, METER_LAYOUT_FUSE, FUSE_VIEW_DETAIL, 0, 6 };
static const mstate_t FUSE_SC  = { 0, METER_LAYOUT_FUSE, FUSE_VIEW_SCAN, 0, 6 };
static const mstate_t FUSE_TY  = { 0, METER_LAYOUT_FUSE, FUSE_VIEW_TYPES, 0, 6 };

static void test_screen_entry(void)
{
    printf("the screen\n");
    CHECK("name meter, id SCR_METER, MODE_MULTIMETER, instrument 1 (meter), no flags",
          strcmp(meter_screen.name, "meter") == 0 && meter_screen.id == SCR_METER &&
          meter_screen.mode == MODE_MULTIMETER && meter_screen.instrument == 1 && meter_screen.flags == 0);
    const instrument_desc_t *in = model_instrument_at(meter_screen.instrument);
    CHECK("its instrument is the model's meter", in && in->mode == MODE_MULTIMETER);
    CHECK("it has key, draw, tick, epoch and describe",
          meter_screen.key && meter_screen.draw && meter_screen.tick && meter_screen.epoch && meter_screen.describe);
    CHECK("it shows meter, gen, sys and ui",
          meter_screen.groups == (uint16_t)((1u << PG_METER) | (1u << PG_GEN) | (1u << PG_SYS) | (1u << PG_UI)));
    boot();
    CHECK("defaults: DC V, Big, hold / relative / degF off, beep below 30 Ohm, ATO 10 A detail",
          G(P(METER_FUNC)) == 0 && G(P(METER_LAYOUT)) == METER_LAYOUT_BIG && !G(P(METER_HOLD)) &&
          !G(P(METER_REL)) && !G(P(METER_DEGF)) && G(P(METER_BEEP)) == 1 && meter_beep_ohms() == 30 &&
          G(P(METER_FUSETYPE)) == 0 && G(P(METER_FUSERATING)) == 6 && G(P(METER_FUSEVIEW)) == 0);

    int ok = 1, bars = 0;
    for (uint8_t l = 0; l < METER_LAYOUT_COUNT; l++)
    for (uint8_t v = 0; v < FUSE_VIEW_COUNT; v++)
    for (uint8_t f = 0; f < METER_SUBMODE_COUNT; f++) {
        const softkey_bar_t *b = meter_softkey_bar(l, v, f);
        ok &= b != NULL && softkey_bar_valid(b);
        /* PRM is always View: the way out of every bar */
        ok &= b->key[3].label && strcmp(b->key[3].label, "View") == 0;
        bars++;
    }
    CHECK("every bar (layout x fuse page x function) is valid and keeps View on PRM", ok && bars == 240);
    CHECK("the bars: main, continuity (Beep), temperature (Unit), limits (Low/High), fuse, fuse scan, fuse types",
          strcmp(meter_softkey_bar(0, 0, 0)->key[2].label, "Relative") == 0 &&
          strcmp(meter_softkey_bar(0, 0, 7)->key[2].label, "Beep") == 0 &&
          strcmp(meter_softkey_bar(0, 0, 10)->key[0].label, "Unit") == 0 &&
          strcmp(meter_softkey_bar(METER_LAYOUT_LIMITS, 0, 7)->key[1].label, "Low") == 0 &&
          strcmp(meter_softkey_bar(METER_LAYOUT_FUSE, 0, 0)->key[0].label, "Cal leads") == 0 &&
          strcmp(meter_softkey_bar(METER_LAYOUT_FUSE, FUSE_VIEW_SCAN, 0)->key[1].label, "Draw if >") == 0 &&
          strcmp(meter_softkey_bar(METER_LAYOUT_FUSE, FUSE_VIEW_TYPES, 0)->key[0].label, "Fuse type") == 0);
}

static void test_keys(void)
{
    printf("every key: answer, owner calls, popup\n");
    /* softkeys, main bar */
    CHECK("MOVE (Function): meter.func +1",              one(&DCV, BTN_MOVE, SCR_DRAW, "meter_func(1);", ""));
    CHECK("MOVE on Auto wraps to DC V",                  one(&(mstate_t){ AUTO_F, 0, 0, 0, 6 }, BTN_MOVE, SCR_DRAW, "meter_func(0);", ""));
    CHECK("SELECT (Reset): the statistics, no property", one(&DCV, BTN_SELECT, SCR_DRAW, "stats_reset;", "Min/Max reset"));
    CHECK("TRIGGER (Relative): meter.rel on",            one(&DCV, BTN_TRIGGER, SCR_DRAW, "meter_flag(1,1);", "Relative: On"));
    CHECK("PRM (View): Big -> Graph",                    one(&DCV, BTN_PRM, SCR_DRAW, "meter_layout(1);", ""));
    CHECK("PRM: Graph -> Stats",                         one(&(mstate_t){ 6, METER_LAYOUT_CHART, 0, 0, 6 }, BTN_PRM, SCR_DRAW, "meter_layout(2);", ""));
    CHECK("PRM: Stats -> Limits (the setter's hook takes limits from the reading)",
                                                         one(&STATS, BTN_PRM, SCR_DRAW, "meter_layout(4);", ""));
    CHECK("PRM: Limits -> Fuse on ohms: DC V forced first, and said",
                                                         one(&LIMITS, BTN_PRM, SCR_DRAW, "meter_func(0);meter_layout(3);", "Fuse tester: DC V"));
    CHECK("PRM: Limits -> Fuse on DC V: no function change, no popup",
                                                         one(&(mstate_t){ 0, METER_LAYOUT_LIMITS, 0, 0, 6 }, BTN_PRM, SCR_DRAW, "meter_layout(3);", ""));
    CHECK("PRM: Fuse -> Big",                            one(&FUSE, BTN_PRM, SCR_DRAW, "meter_layout(0);", ""));
    /* continuity and temperature bars */
    CHECK("TRIGGER in Continuity (Beep): meter.beep +1", one(&CONT, BTN_TRIGGER, SCR_DRAW, "", "Beep below 50 Ohm"));
    CHECK("MOVE in Temperature (Unit): meter.degf on",   one(&TEMP, BTN_MOVE, SCR_DRAW, "meter_flag(2,1);", "Temperature: degF"));
    CHECK("TRIGGER in Temperature: still Relative",      one(&TEMP, BTN_TRIGGER, SCR_DRAW, "meter_flag(1,1);", "Relative: On"));
    /* limits bar */
    CHECK("MOVE in Limits: still Function",              one(&LIMITS, BTN_MOVE, SCR_DRAW, "meter_func(7);", ""));
    CHECK("SELECT in Limits: Low",                       one(&LIMITS, BTN_SELECT, SCR_DRAW, "limit_press(0);", ""));
    CHECK("TRIGGER in Limits: High",                     one(&LIMITS, BTN_TRIGGER, SCR_DRAW, "limit_press(1);", ""));
    CHECK("UP in Limits: the active limit up",           one(&LIMITS, BTN_UP, SCR_DRAW, "limits_nudge(1);", ""));
    CHECK("DOWN in Limits: down",                        one(&LIMITS, BTN_DOWN, SCR_DRAW, "limits_nudge(-1);", ""));
    /* fuse bars */
    CHECK("MOVE in Fuse: Cal leads",                     one(&FUSE, BTN_MOVE, SCR_DRAW, "fuse_cal;", ""));
    CHECK("MOVE on the Scan page: Cal leads",            one(&FUSE_SC, BTN_MOVE, SCR_DRAW, "fuse_cal;", ""));
    CHECK("MOVE on the Types page: no owner call (meter.fusetype)", one(&FUSE_TY, BTN_MOVE, SCR_DRAW, "", ""));
    CHECK("SELECT in Fuse (Rating): no owner call (meter.fuserating)", one(&FUSE, BTN_SELECT, SCR_DRAW, "", ""));
    CHECK("SELECT on the Scan page (Draw if >): no owner call", one(&FUSE_SC, BTN_SELECT, SCR_DRAW, "", ""));
    CHECK("TRIGGER in Fuse (Show): no owner call (meter.fuseview)", one(&FUSE, BTN_TRIGGER, SCR_DRAW, "", ""));
    CHECK("UP / DOWN / LEFT / RIGHT in Fuse: a full repaint, no owner call",
          one(&FUSE, BTN_UP, SCR_DRAW_FULL, "", "") && one(&FUSE, BTN_DOWN, SCR_DRAW_FULL, "", "") &&
          one(&FUSE, BTN_LEFT, SCR_DRAW_FULL, "", "") && one(&FUSE, BTN_RIGHT, SCR_DRAW_FULL, "", ""));
    /* the other keys */
    CHECK("RIGHT: meter.func +1, a full repaint",        one(&OHM, BTN_RIGHT, SCR_DRAW_FULL, "meter_func(7);", ""));
    CHECK("LEFT: meter.func -1",                         one(&OHM, BTN_LEFT, SCR_DRAW_FULL, "meter_func(5);", ""));
    CHECK("LEFT on DC V wraps to Auto",                  one(&DCV, BTN_LEFT, SCR_DRAW_FULL, "meter_func(11);", ""));
    CHECK("LEFT / RIGHT in Limits: the function too",    one(&LIMITS, BTN_RIGHT, SCR_DRAW_FULL, "meter_func(7);", ""));
    CHECK("UP / DOWN outside Limits and Fuse: nothing",
          one(&OHM, BTN_UP, SCR_HANDLED, "", "") && one(&STATS, BTN_DOWN, SCR_HANDLED, "", ""));
    CHECK("OK: meter.hold on",                           one(&OHM, BTN_OK, SCR_DRAW, "meter_flag(0,1);", "HOLD"));
    CHECK("AUTO: meter.func = auto",                     one(&OHM, BTN_AUTO, SCR_DRAW, "meter_func(11);", "AUTO"));
    CHECK("AUTO on Auto: set again (re-arms the SoC)",   one(&(mstate_t){ AUTO_F, 0, 0, 0, 6 }, BTN_AUTO, SCR_DRAW, "meter_func(11);", "AUTO"));
    CHECK("MENU: Home, as one sys.mode set",             one(&OHM, BTN_MENU, SCR_HANDLED, "mode(4);", ""));
    CHECK("CH1, CH2: a full repaint and nothing else",
          one(&OHM, BTN_CH1, SCR_DRAW_FULL, "", "") && one(&FUSE, BTN_CH2, SCR_DRAW_FULL, "", ""));
    int ok = 1;
    static const uint8_t none[] = { BTN_SAVE, BTN_POWER, LONG(BTN_CH1), LONG(BTN_CH2), LONG(BTN_PRM), LONG(BTN_MOVE),
                                    LONG(BTN_SELECT), LONG(BTN_TRIGGER), LONG(BTN_SAVE), LONG(BTN_MENU),
                                    LONG(BTN_OK), LONG(BTN_AUTO), LONG(BTN_UP), LONG(BTN_LEFT) };
    static const mstate_t *const where[] = { &OHM, &LIMITS, &FUSE, &FUSE_SC, &TEMP };
    for (unsigned w = 0; w < 5; w++)
        for (unsigned i = 0; i < sizeof none; i++) ok &= one(where[w], none[i], SCR_HANDLED, "", "");
    CHECK("SAVE, POWER (the router's) and every long key: handled, nothing", ok);

    /* toggles the other way, and the refusals */
    boot();
    (void)env_key(BTN_OK);
    env_log_clear();
    CHECK("OK again: hold off, \"Live\"", env_key(BTN_OK) == SCR_DRAW && env_log_is("meter_flag(0,0);") &&
          strcmp(g_popup, "Live") == 0 && !G(P(METER_HOLD)));
    boot();
    env_meter_reading = false;
    CHECK("OK with no reading: refused, said, hold stays off",
          env_key(BTN_OK) == SCR_DRAW && env_log_is("meter_flag(0,1);") &&
          strcmp(g_popup, "HOLD: no reading") == 0 && !G(P(METER_HOLD)));
    boot();
    (void)env_key(BTN_TRIGGER);
    CHECK("TRIGGER again: relative off", env_key(BTN_TRIGGER) == SCR_DRAW && strcmp(g_popup, "Relative: Off") == 0 &&
          !G(P(METER_REL)));
    set_state(&TEMP);
    (void)env_key(BTN_MOVE);
    CHECK("MOVE again in Temperature: degC", env_key(BTN_MOVE) == SCR_DRAW &&
          strcmp(g_popup, "Temperature: degC") == 0 && !G(P(METER_DEGF)));
    set_state(&LIMITS);
    env_meter_func_ok = false;
    CHECK("View into Fuse with the function change refused: the view stays, and says so",
          env_key(BTN_PRM) == SCR_DRAW && G(P(METER_LAYOUT)) == METER_LAYOUT_LIMITS && G(P(METER_FUNC)) == 6 &&
          env_log_is("meter_func(0);") && strcmp(g_popup, "DC V NOT set") == 0);

    /* the autoselect: a softkey ends it, the other keys do not ask */
    boot();
    (void)env_key(BTN_MOVE); (void)env_key(BTN_SELECT); (void)env_key(BTN_TRIGGER); (void)env_key(BTN_PRM);
    const int after_soft = g_cancels;
    (void)env_key(BTN_OK); (void)env_key(BTN_UP); (void)env_key(BTN_CH1); (void)env_key(LONG(BTN_OK));
    CHECK("each softkey asks the running autoselect to stop; OK, the arrows and the long keys do not",
          after_soft == 4 && g_cancels == 4);

    boot();
    (void)env_key(BTN_MENU);
    CHECK("MENU: the mode and the router's base are Home; the meter is untouched",
          current_mode == MODE_HOME && screen_base() == SCR_HOME && G(P(METER_FUNC)) == 0);
    boot();
    env_mode_applies = false;
    (void)env_key(BTN_MENU);
    CHECK("... with the owner only logging: the screen wrote no mode itself",
          env_log_is("mode(4);") && current_mode == MODE_MULTIMETER && screen_base() == SCR_METER);
}

static void test_fuse_keys(void)
{
    printf("the fuse tester's keys, through the properties\n");
    set_state(&FUSE);                                  /* ATO, index 6 */
    const uint8_t ato_n = fuse_tables[0].count;
    (void)env_key(BTN_UP);
    CHECK("UP: the next larger rating", G(P(METER_FUSERATING)) == 7);
    (void)env_key(BTN_DOWN); (void)env_key(BTN_DOWN);
    CHECK("DOWN: smaller", G(P(METER_FUSERATING)) == 5);
    for (int i = 0; i < 20; i++) (void)env_key(BTN_DOWN);
    CHECK("DOWN stops at the smallest", G(P(METER_FUSERATING)) == 0);
    for (int i = 0; i < 20; i++) (void)env_key(BTN_UP);
    CHECK("UP stops at this type's largest", G(P(METER_FUSERATING)) == ato_n - 1);
    (void)env_key(BTN_SELECT);
    CHECK("SELECT (Rating) wraps from the largest to the smallest", G(P(METER_FUSERATING)) == 0);
    (void)env_key(BTN_SELECT);
    CHECK("SELECT: the next", G(P(METER_FUSERATING)) == 1);
    (void)env_key(BTN_TRIGGER);
    CHECK("TRIGGER (Show): detail -> table", G(P(METER_FUSEVIEW)) == FUSE_VIEW_MULTI);
    (void)env_key(BTN_TRIGGER);
    CHECK("... -> scan: the bar's SELECT is the threshold now", G(P(METER_FUSEVIEW)) == FUSE_VIEW_SCAN &&
          strcmp(meter_screen_bar()->key[1].label, "Draw if >") == 0);
    const int32_t r0 = G(P(METER_FUSERATING));
    (void)env_key(BTN_UP);
    CHECK("UP on the Scan page: the threshold +0.1 mV, the rating stays",
          G(P(METER_FUSETHRESH)) == 2 && G(P(METER_FUSERATING)) == r0);
    (void)env_key(BTN_DOWN); (void)env_key(BTN_DOWN);
    CHECK("DOWN stops at 0.1 mV", G(P(METER_FUSETHRESH)) == 1);
    given(P(METER_FUSETHRESH), 50);
    (void)env_key(BTN_UP);
    CHECK("UP stops at 5.0 mV", G(P(METER_FUSETHRESH)) == 50);
    /* the presets, against the model's own function */
    given(P(METER_FUSETHRESH), 1);
    int ok = 1;
    for (int i = 0; i < 8; i++) {
        const float want = fuse_scan_threshold_next_preset((float)G(P(METER_FUSETHRESH)) / 10.0f);
        (void)env_key(BTN_SELECT);
        ok &= G(P(METER_FUSETHRESH)) == (int32_t)(want * 10.0f + 0.5f);
    }
    CHECK("SELECT on the Scan page: the next preset, eight times round", ok);
    (void)env_key(BTN_TRIGGER);
    CHECK("TRIGGER: scan -> types: MOVE is the fuse type", G(P(METER_FUSEVIEW)) == FUSE_VIEW_TYPES &&
          strcmp(meter_screen_bar()->key[0].label, "Fuse type") == 0);
    (void)env_key(BTN_MOVE);
    CHECK("MOVE on the Types page: the next type", G(P(METER_FUSETYPE)) == 1);
    (void)env_key(BTN_TRIGGER);
    CHECK("TRIGGER: types -> detail (wraps)", G(P(METER_FUSEVIEW)) == FUSE_VIEW_DETAIL);
    (void)env_key(BTN_LEFT); (void)env_key(BTN_LEFT);
    CHECK("LEFT twice: the type wraps to the last", G(P(METER_FUSETYPE)) == FUSE_TYPE_COUNT - 1);
    (void)env_key(BTN_RIGHT);
    CHECK("RIGHT: wraps to the first", G(P(METER_FUSETYPE)) == 0);
    CHECK("none of it called a hardware owner", env_log_is(""));
    boot();
    given(P(METER_FUNC), 7);
    (void)env_key(BTN_TRIGGER); (void)env_key(BTN_TRIGGER); (void)env_key(BTN_TRIGGER);
    CHECK("Beep: 30 -> 50 -> 100 -> 10 Ohm", G(P(METER_BEEP)) == 0 && meter_beep_ohms() == 10 &&
          strcmp(g_popup, "Beep below 10 Ohm") == 0);
}

static void test_through_the_model(void)
{
    printf("every change is a property set: get, epoch, settings note\n");
    static const struct { const char *what; const mstate_t *from; uint8_t raw; const prop_desc_t *p; int32_t want; unsigned notes; } T[] = {
        { "MOVE -> meter.func",              &DCV,     BTN_MOVE,    P(METER_FUNC),       1, 1 },
        { "RIGHT -> meter.func",             &DCV,     BTN_RIGHT,   P(METER_FUNC),       1, 1 },
        { "AUTO -> meter.func",              &DCV,     BTN_AUTO,    P(METER_FUNC),       AUTO_F, 1 },
        { "PRM -> meter.layout",             &DCV,     BTN_PRM,     P(METER_LAYOUT),     METER_LAYOUT_CHART, 1 },
        { "OK -> meter.hold",                &DCV,     BTN_OK,      P(METER_HOLD),       1, 0 },
        { "TRIGGER -> meter.rel",            &DCV,     BTN_TRIGGER, P(METER_REL),        1, 0 },
        { "TRIGGER (continuity) -> meter.beep", &CONT, BTN_TRIGGER, P(METER_BEEP),       2, 0 },
        { "MOVE (temperature) -> meter.degf", &TEMP,   BTN_MOVE,    P(METER_DEGF),       1, 0 },
        { "RIGHT (fuse) -> meter.fusetype",  &FUSE,    BTN_RIGHT,   P(METER_FUSETYPE),   1, 0 },
        { "UP (fuse) -> meter.fuserating",   &FUSE,    BTN_UP,      P(METER_FUSERATING), 7, 0 },
        { "SELECT (fuse) -> meter.fuserating", &FUSE,  BTN_SELECT,  P(METER_FUSERATING), 7, 0 },
        { "UP (scan) -> meter.fusethresh",   &FUSE_SC, BTN_UP,      P(METER_FUSETHRESH), 2, 0 },
        { "TRIGGER (fuse) -> meter.fuseview", &FUSE,   BTN_TRIGGER, P(METER_FUSEVIEW),   1, 0 },
        { "MOVE (types) -> meter.fusetype",  &FUSE_TY, BTN_MOVE,    P(METER_FUSETYPE),   1, 0 },
    };
    int ok = 1;
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
        set_state(T[i].from);
        env_notes = 0;
        const env_epochs_t e0 = env_epochs();
        (void)env_key(T[i].raw);
        const env_epochs_t e1 = env_epochs();
        const bool good = G(T[i].p) == T[i].want && env_epochs_moved(&e0, &e1) == (1u << PG_METER) &&
                          env_notes == T[i].notes && current_mode == MODE_MULTIMETER;
        if (!good) printf("    %s: value %ld, groups 0x%X, notes %u\n", T[i].what, (long)G(T[i].p),
                          env_epochs_moved(&e0, &e1), env_notes);
        ok &= good;
    }
    CHECK("each: the property reads the new value, only the meter epoch moved, a settings note "
          "for the saved ones (function, layout)", ok);

    boot();
    env_notes = 0;
    const env_epochs_t c0 = env_epochs();
    (void)env_key(BTN_UP); (void)env_key(BTN_CH1); (void)env_key(LONG(BTN_OK)); (void)env_key(BTN_SELECT);
    const env_epochs_t c1 = env_epochs();
    CHECK("keys that change no property: no epoch, no note (Reset is the paint module's)",
          env_log_is("stats_reset;") && env_epochs_moved(&c0, &c1) == 0 && env_notes == 0);

    /* A function change releases hold and relative: the owner's op. */
    boot();
    (void)env_key(BTN_OK); (void)env_key(BTN_TRIGGER);
    CHECK("hold and relative on", G(P(METER_HOLD)) && G(P(METER_REL)));
    (void)env_key(BTN_RIGHT);
    CHECK("RIGHT: the function change released both", !G(P(METER_HOLD)) && !G(P(METER_REL)));
    (void)env_key(BTN_OK);
    (void)prop_set_local(P(METER_FUNC), 3, ORIGIN_AUTO, NULL);
    CHECK("the autoselect's own candidate (ORIGIN_AUTO) keeps hold, and the op is told",
          G(P(METER_HOLD)) && G(P(METER_FUNC)) == 3 && strstr(env_log, "meter_func(3,auto);") != NULL);
}

/* ── the two routes, and the handlers before B6 ────────────────────── */

/* fuse_ui.c's keep_rating before B6: a new type keeps the same rating in
 * amps when it has one, else the nearest below (or the smallest). */
static uint8_t old_keep_rating(uint8_t old_type, uint8_t old_idx, uint8_t new_type)
{
    const fuse_table_t *ot = &fuse_tables[old_type];
    const uint8_t amps = ot->entries[old_idx < ot->count ? old_idx : 0].rating_amps;
    const fuse_table_t *nt = &fuse_tables[new_type];
    uint8_t best = 0;
    for (uint8_t i = 0; i < nt->count; i++)
        if (nt->entries[i].rating_amps <= amps) best = i;
    return best;
}

static void test_both_routes(void)
{
    printf("meter.layout and meter.fusetype: the key, the shell and the old handlers agree\n");
    /* The View softkey before B6 (meter_ui.c skp_view): the next view in the
     * softkey's order; Fuse off DC V selects DC V (cancel autoselect, reset,
     * release hold / relative, post) and says so; Limits takes limits. */
    static const uint8_t order[METER_LAYOUT_COUNT] = { METER_LAYOUT_BIG, METER_LAYOUT_CHART, METER_LAYOUT_STATS,
                                                       METER_LAYOUT_LIMITS, METER_LAYOUT_FUSE };
    int cases = 0, bad = 0;
    char first[200] = "";
    for (uint8_t f = 0; f < METER_SUBMODE_COUNT; f++)
    for (uint8_t i = 0; i < METER_LAYOUT_COUNT; i++)
    for (int held = 0; held < 2; held++) {
        const uint8_t from = order[i], to = order[(i + 1) % METER_LAYOUT_COUNT];
        const bool force = (to == METER_LAYOUT_FUSE && f != 0);
        char want_log[64], key_log[64], sh_log[64];
        snprintf(want_log, sizeof want_log, "%smeter_layout(%u);", force ? "meter_func(0);" : "", to);
        const mstate_t m = { f, from, 0, 0, 6 };

        set_state(&m);
        env_meter_flag[METER_FLAG_HOLD] = env_meter_flag[METER_FLAG_REL] = held;
        const scr_res_t r = env_key(BTN_PRM);
        snprintf(key_log, sizeof key_log, "%s", env_log);
        const uint8_t k_func = meter_submode, k_lay = meter_layout;
        const bool k_hold = G(P(METER_HOLD)), k_rel = G(P(METER_REL));
        const bool k_popup = strcmp(g_popup, force ? "Fuse tester: DC V" : "") == 0;

        set_state(&m);
        env_meter_flag[METER_FLAG_HOLD] = env_meter_flag[METER_FLAG_REL] = held;
        prop_note_t note;
        const prop_status_t s = prop_set_local(P(METER_LAYOUT), to, ORIGIN_SHELL, &note);
        snprintf(sh_log, sizeof sh_log, "%s", env_log);

        cases++;
        const bool good =
            r == SCR_DRAW && s == PS_OK && k_popup &&
            strcmp(note.text, force ? "Fuse tester: DC V" : "") == 0 &&
            strcmp(key_log, want_log) == 0 && strcmp(sh_log, want_log) == 0 &&
            k_lay == to && meter_layout == to &&
            k_func == (force ? 0 : f) && meter_submode == k_func &&
            /* the forced function change releases hold and relative, as any */
            k_hold == (force ? false : (bool)held) && k_rel == k_hold &&
            (bool)G(P(METER_HOLD)) == k_hold && (bool)G(P(METER_REL)) == k_rel;
        if (!good) {
            bad++;
            if (!first[0]) snprintf(first, sizeof first, "func %u %u->%u held %d: key [%s] shell [%s] want [%s] note [%s]",
                                    f, from, to, held, key_log, sh_log, want_log, note.text);
        }
    }
    if (first[0]) printf("    first difference: %s\n", first);
    CHECK("View (PRM) and `set meter.layout`, 12 functions x 5 views x hold/relative: the same owner calls "
          "(as skp_view's before B6), the same state, the same text", bad == 0 && cases == 120);

    boot();
    given(P(METER_FUNC), 6);
    given(P(METER_LAYOUT), METER_LAYOUT_FUSE);
    given(P(METER_FUNC), 6);                        /* the shell puts ohms back under the fuse tester */
    (void)prop_set_local(P(METER_LAYOUT), METER_LAYOUT_FUSE, ORIGIN_SHELL, NULL);
    CHECK("`set meter.layout fuse` in the fuse tester: no change, nothing forced (as before: only entering does)",
          env_log_is("meter_layout(3);") && meter_submode == 6);
    given(P(METER_LAYOUT), METER_LAYOUT_LIMITS);
    env_meter_func_ok = false;
    prop_note_t n;
    CHECK("`set meter.layout fuse` with the function change refused: NOT_SET, the layout stays",
          prop_set_local(P(METER_LAYOUT), METER_LAYOUT_FUSE, ORIGIN_SHELL, &n) == PS_NOT_SET &&
          meter_layout == METER_LAYOUT_LIMITS && meter_submode == 6 && strcmp(n.text, "DC V NOT set") == 0);

    /* The fuse type: RIGHT / LEFT in the fuse tester, MOVE on the Types
     * page, `set meter.fusetype`, against fuse_next_type / fuse_prev_type. */
    cases = 0; bad = 0;
    for (uint8_t t = 0; t < FUSE_TYPE_COUNT; t++)
    for (uint8_t i = 0; i < fuse_tables[t].count; i++)
    for (int way = 0; way < 3; way++) {
        const uint8_t to = (way == 1) ? (t == 0 ? FUSE_TYPE_COUNT - 1 : (uint8_t)(t - 1))
                                      : (uint8_t)((t + 1) % FUSE_TYPE_COUNT);
        const uint8_t want_idx = old_keep_rating(t, i, to);
        const mstate_t m = { 0, METER_LAYOUT_FUSE, way == 2 ? FUSE_VIEW_TYPES : FUSE_VIEW_DETAIL, t, i };
        set_state(&m);
        (void)env_key(way == 0 ? BTN_RIGHT : way == 1 ? BTN_LEFT : BTN_MOVE);
        const bool key_ok = fuse_type == to && fuse_rating_idx == want_idx && env_log_is("");
        set_state(&m);
        const bool sh_ok = prop_set_local(P(METER_FUSETYPE), to, ORIGIN_SHELL, NULL) == PS_OK &&
                           fuse_type == to && fuse_rating_idx == want_idx && env_log_is("");
        cases++;
        if (!key_ok || !sh_ok) bad++;
    }
    CHECK("the type keys and `set meter.fusetype`, every type x rating x direction: the rating kept in amps "
          "as fuse_next_type / fuse_prev_type did", bad == 0 && cases > 100);
    printf("    %d fuse cases\n", cases);
}

/* ── the display side ──────────────────────────────────────────────── */

/* The display task's loop for a screen with a tick (main.c): frames
 * [from, to), painting in place when the tick says so. Paints made. */
static int run_frames(uint32_t from, uint32_t to)
{
    int n = 0;
    for (uint32_t f = from; f < to; f++)
        if (screen_tick(f) != REDRAW_SKIP) { screen_draw(SCR_PAINT_UPDATE, f); n++; }
    return n;
}

static void test_display(void)
{
    printf("the tick, the epoch and the paint\n");
    boot();
    CHECK("entered and not painted yet: the first tick paints", run_frames(0, 1) == 1 && g_draws[SCR_PAINT_UPDATE] == 1);
    CHECK("a steady reading: 200 frames, no paint", run_frames(1, 201) == 0);

    /* a set from the shell while the screen is shown */
    const uint32_t e0 = meter_screen.epoch();
    (void)prop_set_local(P(METER_FUNC), 6, ORIGIN_SHELL, NULL);
    CHECK("`set meter.func ohm`: the epoch moves", meter_screen.epoch() != e0);
    CHECK("... the next tick paints it in place, once (no full paint, no wait for a reading)",
          run_frames(201, 202) == 1 && run_frames(202, 300) == 0 &&
          g_draws[SCR_PAINT_FULL] == 0 && g_draws[SCR_PAINT_RESTORE] == 0);
    int ok = 1;
    uint32_t fr = 300;
    static const struct { const prop_desc_t *p; int32_t v; } sets[] = {
        { P(METER_LAYOUT), METER_LAYOUT_STATS }, { P(METER_HOLD), 1 }, { P(METER_REL), 1 }, { P(METER_DEGF), 1 },
        { P(METER_BEEP), 3 }, { P(METER_LAYOUT), METER_LAYOUT_FUSE }, { P(METER_FUSETYPE), 2 },
        { P(METER_FUSERATING), 2 }, { P(METER_FUSEVIEW), 2 }, { P(METER_FUSETHRESH), 25 }, { P(METER_FUNC), 11 },
    };
    for (unsigned i = 0; i < sizeof sets / sizeof sets[0]; i++) {
        const uint32_t before = meter_screen.epoch();
        (void)prop_set_local(sets[i].p, sets[i].v, ORIGIN_SHELL, NULL);
        const bool good = meter_screen.epoch() != before && run_frames(fr, fr + 1) == 1 && run_frames(fr + 1, fr + 50) == 0;
        if (!good) printf("    set %s: not painted once\n", sets[i].p->name);
        ok &= good;
        fr += 50;
    }
    CHECK("every meter property the page shows, set from the shell: one in-place paint each", ok);
    CHECK("still no full paint", g_draws[SCR_PAINT_FULL] == 0);
    /* the old commands write the state directly */
    meter_submode = 2;
    CHECK("`meter mode` (a direct write of meter_submode): painted by the next tick", run_frames(fr, fr + 1) == 1);
    theme_init(THEME_CLASSIC_GREEN);
    CHECK("the theme: painted", run_frames(fr + 1, fr + 2) == 1);
    g_gen++;
    CHECK("view state with no property (statistics, limits, lead offset): painted", run_frames(fr + 2, fr + 3) == 1);
    (void)prop_set_local(P(METER_LAYOUT), METER_LAYOUT_LIMITS, ORIGIN_SHELL, NULL);
    (void)run_frames(fr + 3, fr + 4);
    g_lim_valid = true;
    CHECK("a softkey value (the limits' text): painted", run_frames(fr + 4, fr + 5) == 1);
    CHECK("and then nothing", run_frames(fr + 5, fr + 100) == 0);

    /* readings */
    boot();
    (void)run_frames(0, 1);
    g_reading_seq++;
    CHECK("a new reading within 5 frames of the last tick paint waits", run_frames(1, 5) == 0);
    CHECK("... and is painted at the fifth", run_frames(5, 6) == 1);
    int paints = 0;
    for (uint32_t f = 6; f < 206; f++) {            /* a reading every frame: 20 Hz */
        g_reading_seq++;
        paints += run_frames(f, f + 1);
    }
    CHECK("readings at 20 Hz: painted at 4 Hz (40 in 200 frames)", paints == 40);
    g_reading_seq = g_rendered_seq;
    CHECK("the reading settles: no paint", run_frames(206, 400) == 0);
    g_periodic = true;
    CHECK("an animating page (the debug strip): once a second", run_frames(400, 500) == 5);
    g_periodic = false;
    g_autosel = true;
    g_reading_seq++;
    (void)prop_set_local(P(METER_FUNC), 3, ORIGIN_AUTO, NULL);
    CHECK("the autoselect running: the page is held still", run_frames(500, 600) == 0);
    g_autosel = false;
    CHECK("... and painted when it ends", run_frames(600, 601) == 1);

    /* a key's paint is the same gate */
    boot();
    (void)run_frames(0, 1);
    (void)env_key(BTN_MOVE);
    screen_draw(SCR_PAINT_UPDATE, 1);                /* the key's DCMD_DRAW_ACTIVE */
    CHECK("after a key's own paint the tick has nothing to add", run_frames(1, 100) == 0);
    const uint32_t e1 = meter_screen.epoch();
    (void)env_key(BTN_UP); (void)env_key(BTN_CH1); (void)env_key(LONG(BTN_OK));
    CHECK("keys that changed nothing leave the epoch (their in-place paint is dropped)", meter_screen.epoch() == e1);

    g_draws[0] = g_draws[1] = g_draws[2] = 0;
    screen_draw(SCR_PAINT_FULL, 0); screen_draw(SCR_PAINT_RESTORE, 0); screen_draw(SCR_PAINT_UPDATE, 0);
    CHECK("FULL, RESTORE and UPDATE reach the paint module as such",
          g_draws[SCR_PAINT_FULL] == 1 && g_draws[SCR_PAINT_RESTORE] == 1 && g_draws[SCR_PAINT_UPDATE] == 1);

    char d[32], t[96];
    boot();
    meter_screen.describe(d, sizeof d);
    CHECK("describe: mview=big", strcmp(d, "mview=big") == 0);
    given(P(METER_LAYOUT), METER_LAYOUT_FUSE);
    given(P(METER_FUSEVIEW), FUSE_VIEW_SCAN);
    meter_screen.describe(d, sizeof d);
    CHECK("describe in the fuse tester: mview=fuse/scan", strcmp(d, "mview=fuse/scan") == 0);
    screen_snapshot_text(t, sizeof t);
    CHECK("the router's snapshot text: meter ... | mview=fuse/scan",
          strncmp(t, "meter gen=", 10) == 0 && strstr(t, "| mview=fuse/scan") != NULL);
}

static void test_leaving(void)
{
    printf("leaving and entering\n");
    boot();
    (void)env_key(BTN_RIGHT); (void)env_key(BTN_PRM);
    (void)prop_set_local(P(SYS_MODE), MODE_OSCILLOSCOPE, ORIGIN_KEY, NULL);   /* a long mode key */
    env_log_clear();
    (void)prop_set_local(P(METER_FUNC), 6, ORIGIN_SHELL, NULL);
    CHECK("a `set meter.func` with the scope shown: stored, the owner called",
          G(P(METER_FUNC)) == 6 && env_log_is("meter_func(6);") && screen_base() == SCR_SCOPE);
    (void)prop_set_local(P(SYS_MODE), MODE_MULTIMETER, ORIGIN_KEY, NULL);
    env_log_clear();
    CHECK("back on the meter: the view and the function are as they were left; the keys work",
          G(P(METER_LAYOUT)) == METER_LAYOUT_CHART && screen_base() == SCR_METER &&
          env_key(BTN_MOVE) == SCR_DRAW && env_log_is("meter_func(7);"));
    CHECK("entering makes no owner call by itself (the link and the posture are not the screen's)",
          meter_screen.close == NULL);
}

int main(void)
{
    printf("multimeter screen\n");
    test_screen_entry();
    test_keys();
    test_fuse_keys();
    test_through_the_model();
    test_both_routes();
    test_display();
    test_leaving();
    printf("\n%d/%d passed\n", passed, run);
    return passed == run ? 0 : 1;
}
