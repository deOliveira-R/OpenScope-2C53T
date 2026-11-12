/*
 * Host tests for the home screen model (stock's mode-selection page) and the
 * Startup-on-Boot policy that can land on it. Both are what the firmware
 * runs: home_key() is what the input task asks on home, startup_policy.h is
 * what main() asks at boot.
 *
 * Build: see `make test-home-model`.
 */

#include <stdio.h>
#include <string.h>
#include "home_model.h"
#include "startup_policy.h"

static int run, passed;
#define CHECK(name, cond) do { run++; if (cond) { passed++; printf("  %-62s[PASS]\n", name); } \
                               else { printf("  %-62s[FAIL]\n", name); } } while (0)

/* home_ui.c owns the live instance in the firmware; the model never calls it. */
home_t *home_ui_model(void) { static home_t h = HOME_INIT; return &h; }

static void test_order_and_labels(void)
{
    printf("items (stock 0x0804C924 order)\n");
    CHECK("0 Multimeter, 1 Oscilloscope, 2 Signal Generator, 3 Settings",
          strcmp(home_item_label(HOME_ITEM_METER), "Multimeter") == 0 &&
          strcmp(home_item_label(HOME_ITEM_SCOPE), "Oscilloscope") == 0 &&
          strcmp(home_item_label(HOME_ITEM_SIGGEN), "Signal Generator") == 0 &&
          strcmp(home_item_label(HOME_ITEM_SETTINGS), "Settings") == 0 &&
          HOME_ITEM_COUNT == 4);
    int ok = 1;
    for (int i = 0; i < (int)HOME_ITEM_COUNT; i++)
        ok &= home_item_for_mode(home_item_mode((home_item_t)i)) == (home_item_t)i;
    CHECK("item -> mode -> item round-trips for all four", ok);
    CHECK("MODE_HOME is not an item", home_item_for_mode(MODE_HOME) == HOME_ITEM_COUNT);
    CHECK("labels fit stock's 170 px box at ~9 px/char (<= 18 chars)",
          strlen(home_item_label(HOME_ITEM_SIGGEN)) <= 18);
}

static void test_boot_state(void)
{
    printf("boot state\n");
    home_t h = HOME_INIT;
    home_t g; memset(&g, 0x5A, sizeof g); home_init(&g);
    CHECK("HOME_INIT and home_init(): Oscilloscope preselected (0x0802DF80)",
          h.sel == HOME_ITEM_SCOPE && g.sel == HOME_ITEM_SCOPE);
    home_result_t r = home_key(&h, HOME_KEY_BACK);
    CHECK("BACK right after boot: nothing to go back to", r.act == HOME_IGNORED);
    r = home_key(&h, HOME_KEY_OK);
    CHECK("OK at boot enters the scope", r.act == HOME_ENTER && r.mode == MODE_OSCILLOSCOPE);
}

static void test_left_right_wrap(void)
{
    printf("LEFT/RIGHT (0x0800B226 / 0x0800B7FA)\n");
    home_t h = HOME_INIT;
    h.sel = HOME_ITEM_METER;
    home_result_t r = home_key(&h, HOME_KEY_LEFT);
    CHECK("LEFT from Multimeter wraps to Settings", r.act == HOME_MOVED && h.sel == HOME_ITEM_SETTINGS);
    r = home_key(&h, HOME_KEY_RIGHT);
    CHECK("RIGHT from Settings wraps to Multimeter", r.act == HOME_MOVED && h.sel == HOME_ITEM_METER);
    int ok = 1;
    for (int i = 0; i < 4; i++) { home_key(&h, HOME_KEY_RIGHT); ok &= h.sel == (uint8_t)((i + 1) % 4); }
    CHECK("four RIGHTs visit 1,2,3,0", ok);
    ok = 1;
    for (int i = 0; i < 8; i++) home_key(&h, HOME_KEY_LEFT);
    ok &= h.sel == HOME_ITEM_METER;
    CHECK("eight LEFTs come back to the start", ok);
    CHECK("neighbours of Multimeter: Settings left, Oscilloscope right",
          home_neighbour(HOME_ITEM_METER, -1) == HOME_ITEM_SETTINGS &&
          home_neighbour(HOME_ITEM_METER, +1) == HOME_ITEM_SCOPE);
    CHECK("neighbours of Settings: Signal Generator left, Multimeter right",
          home_neighbour(HOME_ITEM_SETTINGS, -1) == HOME_ITEM_SIGGEN &&
          home_neighbour(HOME_ITEM_SETTINGS, +1) == HOME_ITEM_METER);
}

static void test_ok_enters(void)
{
    printf("OK (0x0800BD90: F68 = sel + 1)\n");
    const device_mode_t want[4] = { MODE_MULTIMETER, MODE_OSCILLOSCOPE, MODE_SIGNAL_GEN, MODE_SETTINGS };
    int ok = 1;
    for (int i = 0; i < 4; i++) {
        home_t h = HOME_INIT; h.sel = (uint8_t)i;
        home_result_t r = home_key(&h, HOME_KEY_OK);
        ok &= r.act == HOME_ENTER && r.mode == want[i] && h.sel == (uint8_t)i;
    }
    CHECK("OK enters meter / scope / siggen / settings and keeps the selection", ok);
}

static void test_ignored_keys(void)
{
    printf("keys stock ignores on home\n");
    home_t h = HOME_INIT; h.sel = HOME_ITEM_SIGGEN;
    home_result_t a = home_key(&h, HOME_KEY_UP);
    home_result_t b = home_key(&h, HOME_KEY_DOWN);
    home_result_t c = home_key(&h, HOME_KEY_OTHER);
    CHECK("UP/DOWN/other: nothing, selection unchanged",
          a.act == HOME_IGNORED && b.act == HOME_IGNORED && c.act == HOME_IGNORED &&
          h.sel == HOME_ITEM_SIGGEN);
}

static void test_enter_preselects(void)
{
    printf("entering home (long BACK, 0x0800D834: F69 = item of the mode left)\n");
    const device_mode_t from[4] = { MODE_MULTIMETER, MODE_OSCILLOSCOPE, MODE_SIGNAL_GEN, MODE_SETTINGS };
    int ok = 1;
    for (int i = 0; i < 4; i++) {
        home_t h = HOME_INIT; h.sel = (uint8_t)((i + 2) % 4);
        home_enter(&h, from[i]);
        ok &= h.sel == (uint8_t)i && h.back == (uint8_t)from[i];
    }
    CHECK("from meter/scope/siggen/settings preselects its own item", ok);

    home_t h = HOME_INIT;
    home_enter(&h, MODE_SIGNAL_GEN);
    home_key(&h, HOME_KEY_LEFT);
    home_enter(&h, MODE_HOME);
    CHECK("re-entering from home keeps selection and back target",
          h.sel == HOME_ITEM_SCOPE && h.back == MODE_SIGNAL_GEN);

    home_result_t r = home_key(&h, HOME_KEY_BACK);
    CHECK("BACK (ours) returns to the mode home was opened from, even after moving",
          r.act == HOME_ENTER && r.mode == MODE_SIGNAL_GEN);
}

static void test_robust(void)
{
    printf("robustness\n");
    home_t h = { 200, 77 };
    home_result_t r = home_key(&h, HOME_KEY_OK);
    CHECK("corrupt selection is sanitized to Oscilloscope", r.mode == MODE_OSCILLOSCOPE && h.sel == HOME_ITEM_SCOPE);
    r = home_key(&h, HOME_KEY_BACK);
    CHECK("corrupt back target is ignored", r.act == HOME_IGNORED);
    h.sel = 9;
    CHECK("neighbour of a corrupt selection stays in range",
          home_neighbour(h.sel, -1) < HOME_ITEM_COUNT && home_neighbour(h.sel, +1) < HOME_ITEM_COUNT);
    h.back = (uint8_t)MODE_HOME;
    r = home_key(&h, HOME_KEY_BACK);
    CHECK("back target MODE_HOME is ignored (no loop)", r.act == HOME_IGNORED);
}

static void test_startup_policy(void)
{
    printf("Startup on Boot (stock 0x0804C208: None, Multimeter, Oscilloscope, Signal Generator)\n");
    CHECK("None -> home, siggen -> generator, meter, scope",
          startup_boot_mode(STARTUP_HOME, false, false) == MODE_HOME &&
          startup_boot_mode(STARTUP_SIGGEN, false, false) == MODE_SIGNAL_GEN &&
          startup_boot_mode(STARTUP_METER, false, false) == MODE_MULTIMETER &&
          startup_boot_mode(STARTUP_SCOPE, false, false) == MODE_OSCILLOSCOPE);
    CHECK("warm-handoff image: home/siggen stay scope",
          startup_boot_mode(STARTUP_HOME, true, true) == MODE_OSCILLOSCOPE &&
          startup_boot_mode(STARTUP_SIGGEN, true, false) == MODE_OSCILLOSCOPE);
    int ok = 1;
    startup_mode_t m = STARTUP_HOME;
    const startup_mode_t seq[4] = { STARTUP_METER, STARTUP_SCOPE, STARTUP_SIGGEN, STARTUP_HOME };
    for (int i = 0; i < 4; i++) { m = startup_mode_step(m, +1); ok &= m == seq[i]; }
    CHECK("RIGHT steps in stock's row order and wraps", ok);
    CHECK("LEFT from Home wraps to Signal Gen", startup_mode_step(STARTUP_HOME, -1) == STARTUP_SIGGEN);
    CHECK("an unknown value steps from the default",
          startup_mode_step((startup_mode_t)42, +1) == STARTUP_SCOPE);
    ok = 1;
    for (int i = 0; i < (int)STARTUP_COUNT; i++)
        ok &= startup_mode_from_config(startup_mode_to_config((startup_mode_t)i)) == (startup_mode_t)i;
    CHECK("every startup choice round-trips through the record", ok);
    CHECK("settings is not a startup choice (-> default meter)",
          startup_mode_from_config(CONFIG_MODE_SETTINGS) == STARTUP_METER);
}

int main(void)
{
    test_order_and_labels();
    test_boot_state();
    test_left_right_wrap();
    test_ok_enters();
    test_ignored_keys();
    test_enter_preselects();
    test_robust();
    test_startup_policy();
    printf("\n%d/%d passed\n", passed, run);
    return passed == run ? 0 : 1;
}
