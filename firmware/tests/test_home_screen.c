/*
 * Host tests for the home screen as a screen of the router (src/ui/
 * home_screen.c; ARCH-1 B2): the real screen, the real router (screen.c),
 * the real home model and the real property model with fake hardware ops.
 *
 * What is checked: every key on the page; that OK and MENU become ONE
 * property set (sys.instrument for an instrument, sys.mode for Settings)
 * whose mode op is the only thing that moves the mode; that LEFT/RIGHT wrap
 * over the whole entry table and touch nothing but the selection; open()'s
 * preselection; the paints; `ui screen`'s home= token; and that the entry
 * table agrees with the model's instrument table.
 *
 * Build: make test-home-screen.
 */
#include "screen_test_env.h"
#include "home_screen.h"
#include "home_model.h"

/* home_ui.c's two paints (display task in the firmware). */
static char g_paint[64];
void home_ui_draw(uint8_t sel)           { snprintf(g_paint, sizeof g_paint, "full:%u", sel); }
void home_ui_draw_selection(uint8_t sel) { snprintf(g_paint, sizeof g_paint, "sel:%u", sel); }

static const screen_t S_SCOPE    = ENV_BASE("scope",    SCR_SCOPE,    MODE_OSCILLOSCOPE, 0);
static const screen_t S_METER    = ENV_BASE("meter",    SCR_METER,    MODE_MULTIMETER,   1);
static const screen_t S_SIGGEN   = ENV_BASE("siggen",   SCR_SIGGEN,   MODE_SIGNAL_GEN,   2);
static const screen_t S_SETTINGS = ENV_BASE("settings", SCR_SETTINGS, MODE_SETTINGS,     SCR_INSTRUMENT_NONE);

static int g_after; static scr_res_t g_after_res;
static void h_after(scr_res_t r) { g_after++; g_after_res = r; }
static const screen_hooks_t HOOKS = { .after_key = h_after };

/* A device showing `mode`'s screen, the stack on it. */
static void boot(device_mode_t mode)
{
    env_fresh(mode);
    screen_reset();
    screen_register(&S_SCOPE); screen_register(&S_METER); screen_register(&S_SIGGEN);
    screen_register(&S_SETTINGS); screen_register(&home_screen);
    screen_set_hooks(&HOOKS);
    screen_init(screen_base_for_mode(mode));
    g_after = 0;
}

/* What the long MENU global does: a sys.mode set (legacy_screens.c). */
static void go_home(void)
{
    (void)prop_set_local(P(SYS_MODE), MODE_HOME, ORIGIN_KEY, NULL);
    env_log_clear();
}

static const char *described(void)
{
    static char d[32];
    d[0] = '\0';
    home_screen.describe(d, sizeof d);
    return d;
}

static void test_screen_entry(void)
{
    printf("the screen\n");
    CHECK("name home, id SCR_HOME, MODE_HOME, no instrument, no flags",
          strcmp(home_screen.name, "home") == 0 && home_screen.id == SCR_HOME &&
          home_screen.mode == MODE_HOME && home_screen.instrument == SCR_INSTRUMENT_NONE &&
          home_screen.flags == 0);
    CHECK("it has open, key, draw and describe; no epoch (an update always paints)",
          home_screen.open && home_screen.key && home_screen.draw && home_screen.describe &&
          home_screen.epoch == NULL);
    CHECK("it shows the bars' groups only (gen, sys, ui)",
          home_screen.groups == (uint16_t)((1u << PG_GEN) | (1u << PG_SYS) | (1u << PG_UI)));
}

static void test_table(void)
{
    printf("the entry table against the model\n");
    int ok = 1, inst = 0;
    for (unsigned i = 0; i < HOME_ITEM_COUNT; i++) {
        const home_tile_t *t = home_tile((home_item_t)i);
        ok &= t != NULL && t->token && t->token[0] && t->label && t->label[0] &&
              t->icon < HOME_ICON_COUNT && t->mode < MODE_COUNT && t->mode != MODE_HOME;
        if (t->instrument != HOME_NO_INSTRUMENT) {
            const instrument_desc_t *in = model_instrument_at(t->instrument);
            ok &= in != NULL && in->value == t->instrument && in->mode == t->mode;
            inst++;
        } else {
            for (uint8_t k = 0; k < model_instrument_count(); k++)
                ok &= model_instrument_at(k)->mode != t->mode;
        }
        for (unsigned j = 0; j < i; j++)
            ok &= strcmp(home_tile((home_item_t)j)->token, t->token) != 0 &&
                  home_tile((home_item_t)j)->mode != t->mode;
    }
    CHECK("every row: token, label, icon, a mode that is not Home, all distinct", ok);
    CHECK("an instrument row carries the model's sys.instrument value of its mode", ok && inst == 3);
    CHECK("out of range: no row", home_tile(HOME_ITEM_COUNT) == NULL);
    CHECK("tokens are the names `ui screen` gave before (meter scope siggen settings)",
          strcmp(home_tile(HOME_ITEM_METER)->token, "meter") == 0 &&
          strcmp(home_tile(HOME_ITEM_SCOPE)->token, "scope") == 0 &&
          strcmp(home_tile(HOME_ITEM_SIGGEN)->token, "siggen") == 0 &&
          strcmp(home_tile(HOME_ITEM_SETTINGS)->token, "settings") == 0);
}

static void test_boot_on_home(void)
{
    printf("boot on Home (Startup on Boot = Home)\n");
    boot(MODE_HOME);
    CHECK("Oscilloscope preselected", home_screen_selected() == HOME_ITEM_SCOPE &&
          strcmp(described(), "home=scope") == 0);
    CHECK("MENU: nothing to go back to, no set", env_key(BTN_MENU) == SCR_HANDLED && env_log_is("") &&
          current_mode == MODE_HOME);
    CHECK("OK enters the scope: one mode op, the base follows",
          env_key(BTN_OK) == SCR_HANDLED && env_log_is("mode(0);") &&
          current_mode == MODE_OSCILLOSCOPE && screen_base() == SCR_SCOPE);
}

static void test_left_right(void)
{
    printf("LEFT / RIGHT: the selection, with wrap, and nothing else\n");
    boot(MODE_OSCILLOSCOPE);
    go_home();
    const env_epochs_t e0 = env_epochs();
    int ok = 1;
    uint8_t want = HOME_ITEM_SCOPE;
    for (unsigned i = 0; i < HOME_ITEM_COUNT; i++) {
        want = (uint8_t)((want + 1u) % HOME_ITEM_COUNT);
        ok &= env_key(BTN_RIGHT) == SCR_DRAW && home_screen_selected() == want;
    }
    CHECK("RIGHT x count: every entry once, back where it started (wrap)", ok && want == HOME_ITEM_SCOPE);
    ok = 1;
    for (unsigned i = 0; i < HOME_ITEM_COUNT; i++) {
        want = (uint8_t)((want + HOME_ITEM_COUNT - 1u) % HOME_ITEM_COUNT);
        ok &= env_key(BTN_LEFT) == SCR_DRAW && home_screen_selected() == want;
    }
    CHECK("LEFT x count: the other way round, with wrap", ok && want == HOME_ITEM_SCOPE);
    (void)env_key(BTN_LEFT);
    CHECK("LEFT from Oscilloscope: Multimeter; again: wraps to the last entry",
          home_screen_selected() == HOME_ITEM_METER && env_key(BTN_LEFT) == SCR_DRAW &&
          home_screen_selected() == HOME_ITEM_COUNT - 1);
    CHECK("`ui screen` follows the selection", strcmp(described(), "home=settings") == 0);
    const env_epochs_t e1 = env_epochs();
    CHECK("no owner call, no mode change, no settings note, no property epoch moved",
          env_log_is("") && current_mode == MODE_HOME && screen_base() == SCR_HOME &&
          env_notes == 0 && env_epochs_moved(&e0, &e1) == 0);
    CHECK("each of them answered through the router's after-key hook", g_after == 2 * HOME_ITEM_COUNT + 2 &&
          g_after_res == SCR_DRAW);
}

static void test_other_keys(void)
{
    printf("the keys with no meaning on Home\n");
    boot(MODE_MULTIMETER);
    go_home();
    static const uint8_t shorts[] = { BTN_UP, BTN_DOWN, BTN_CH1, BTN_CH2, BTN_MOVE, BTN_SELECT,
                                      BTN_TRIGGER, BTN_PRM, BTN_AUTO, BTN_SAVE, BTN_POWER };
    static const uint8_t longs[]  = { BTN_AUTO, BTN_CH1, BTN_MOVE, BTN_SELECT, BTN_TRIGGER, BTN_PRM,
                                      BTN_CH2, BTN_SAVE, BTN_MENU, BTN_OK };
    const env_epochs_t e0 = env_epochs();
    int ok = 1;
    for (unsigned i = 0; i < sizeof shorts; i++)
        ok &= env_key(shorts[i]) == SCR_HANDLED;
    for (unsigned i = 0; i < sizeof longs; i++)
        ok &= env_key((uint8_t)(longs[i] | BTN_LONG_FLAG)) == SCR_HANDLED;
    const env_epochs_t e1 = env_epochs();
    CHECK("UP, DOWN, the front keys and every long key: handled, nothing drawn",
          ok);
    CHECK("... selection kept, no owner call, no mode change, no epoch, no note",
          home_screen_selected() == HOME_ITEM_METER && env_log_is("") && current_mode == MODE_HOME &&
          env_epochs_moved(&e0, &e1) == 0 && env_notes == 0);
    /* In the firmware SAVE, POWER and the long mode keys never reach the
     * screen: they are the router's globals (test-screen-router). */
}

static void test_ok_each_entry(void)
{
    printf("OK: one property set per entry\n");
    int ok = 1, one_op = 1, closed = 1;
    for (unsigned i = 0; i < HOME_ITEM_COUNT; i++) {
        boot(MODE_OSCILLOSCOPE);
        go_home();
        while (home_screen_selected() != i) (void)env_key(BTN_RIGHT);
        env_log_clear();
        const home_tile_t *t = home_tile((home_item_t)i);
        char want[24];
        snprintf(want, sizeof want, "mode(%u);", t->mode);
        ok &= env_key(BTN_OK) == SCR_HANDLED;
        one_op &= env_log_is(want);
        closed &= current_mode == (device_mode_t)t->mode && screen_base() == screen_base_for_mode(t->mode) &&
                  prop_get(P(SYS_MODE)) == t->mode &&
                  prop_get(P(SYS_INSTRUMENT)) == (t->instrument != HOME_NO_INSTRUMENT ? t->instrument : 3);
    }
    CHECK("OK answers handled (the mode switch repaints, the screen asks for none)", ok);
    CHECK("each entry: exactly one owner call, the mode op with its mode", one_op);
    CHECK("each entry: the mode, the router's base, sys.mode and sys.instrument agree", closed);

    /* The mode op as a pure log: nothing but the model's owner moves the mode. */
    boot(MODE_OSCILLOSCOPE);
    go_home();
    (void)env_key(BTN_LEFT);                               /* Multimeter */
    env_mode_applies = false;
    env_log_clear();
    (void)env_key(BTN_OK);
    CHECK("with the owner only logging: the request is made and the screen wrote no mode itself",
          env_log_is("mode(1);") && current_mode == MODE_HOME && screen_base() == SCR_HOME &&
          home_screen_selected() == HOME_ITEM_METER);

    /* Self-cal: the router's lock answers "Busy" before the screen is asked;
     * if a key did get here, the model refuses the same way. */
    boot(MODE_OSCILLOSCOPE);
    go_home();
    env_busy = true;
    CHECK("self-cal running: the set is refused by the model, no mode op",
          env_key(BTN_OK) == SCR_HANDLED && env_log_is("") && current_mode == MODE_HOME);
}

static void test_open_and_back(void)
{
    printf("open(): preselect where Home was opened from; MENU goes back there\n");
    static const struct { device_mode_t from; home_item_t item; const char *back; } T[] = {
        { MODE_OSCILLOSCOPE, HOME_ITEM_SCOPE,    "mode(0);" },
        { MODE_MULTIMETER,   HOME_ITEM_METER,    "mode(1);" },
        { MODE_SIGNAL_GEN,   HOME_ITEM_SIGGEN,   "mode(2);" },
        { MODE_SETTINGS,     HOME_ITEM_SETTINGS, "mode(3);" },
    };
    int pre = 1, back = 1;
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
        boot(T[i].from);
        go_home();
        pre &= home_screen_selected() == T[i].item && current_mode == MODE_HOME && screen_base() == SCR_HOME;
        (void)env_key(BTN_RIGHT); (void)env_key(BTN_RIGHT);      /* the peek moves the selection */
        env_log_clear();
        back &= env_key(BTN_MENU) == SCR_HANDLED && env_log_is(T[i].back) && current_mode == T[i].from &&
                screen_base() == screen_base_for_mode(T[i].from);
    }
    CHECK("from each screen: its entry is preselected", pre);
    CHECK("short MENU returns to that screen whatever is selected: one mode op", back);

    boot(MODE_MULTIMETER);
    go_home();
    (void)env_key(BTN_RIGHT);                                    /* Oscilloscope */
    (void)env_key(BTN_OK);
    go_home();
    CHECK("re-entering from the scope preselects the scope again", home_screen_selected() == HOME_ITEM_SCOPE);
}

static void test_draw(void)
{
    printf("draw (display task)\n");
    boot(MODE_SIGNAL_GEN);
    go_home();
    g_paint[0] = '\0';
    screen_draw(SCR_PAINT_FULL, 0);
    CHECK("FULL: the whole page with the selection", strcmp(g_paint, "full:2") == 0);
    (void)env_key(BTN_RIGHT);
    screen_draw(SCR_PAINT_UPDATE, 0);
    CHECK("UPDATE: the selection's parts only", strcmp(g_paint, "sel:3") == 0);
    CHECK("no tick: the page is static", screen_tick(0) == REDRAW_SKIP);
    char t[96];
    screen_snapshot_text(t, sizeof t);
    CHECK("the router's snapshot text: home ... | home=settings",
          strncmp(t, "home gen=", 9) == 0 && strstr(t, "| home=settings") != NULL);
}

int main(void)
{
    printf("home screen\n");
    test_screen_entry();
    test_table();
    test_boot_on_home();
    test_left_right();
    test_other_keys();
    test_ok_each_entry();
    test_open_and_back();
    test_draw();
    printf("\n%d/%d passed\n", passed, run);
    return passed == run ? 0 : 1;
}
