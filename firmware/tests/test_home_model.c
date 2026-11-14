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
    printf("Startup on Boot (stock 0x0804C208: None, Multimeter, Oscilloscope, Signal Generator; ours: Restore)\n");
    /* The whole table: (choice, last screen, warm handoff, meter submodes).
     * want[choice][last screen] is the boot screen of every image but the
     * scope-only warm one. Last screen 4 and 255 are garbage. */
    static const uint8_t last[6] = { STARTUP_SCREEN_NONE, STARTUP_SCREEN_SCOPE, STARTUP_SCREEN_METER,
                                     STARTUP_SCREEN_SIGGEN, 4, 255 };
#define ALL6(m) { m, m, m, m, m, m }
    static const device_mode_t want[STARTUP_COUNT][6] = {
        [STARTUP_SCOPE]   = ALL6(MODE_OSCILLOSCOPE),
        [STARTUP_METER]   = ALL6(MODE_MULTIMETER),
        [STARTUP_HOME]    = ALL6(MODE_HOME),
        [STARTUP_SIGGEN]  = ALL6(MODE_SIGNAL_GEN),
        [STARTUP_RESTORE] = { MODE_HOME, MODE_OSCILLOSCOPE, MODE_MULTIMETER, MODE_SIGNAL_GEN,
                              MODE_HOME, MODE_HOME },
    };
#undef ALL6
    int plain = 1, bench = 1, scope_only = 1, sub_only = 1;
    for (int m = 0; m < (int)STARTUP_COUNT; m++) {
        for (int l = 0; l < 6; l++) {
            const startup_mode_t sm = (startup_mode_t)m;
            plain      &= startup_boot_mode(sm, last[l], false, false) == want[m][l];
            /* submodes without a warm handoff is not a build we make; it is the plain table */
            sub_only &= startup_boot_mode(sm, last[l], false, true) == want[m][l];
            bench      &= startup_boot_mode(sm, last[l], true, true) == want[m][l];
            scope_only &= startup_boot_mode(sm, last[l], true, false) == MODE_OSCILLOSCOPE;
        }
    }
    CHECK("plain image: every choice honoured; Restore = the last instrument, else Home", plain);
    CHECK("submodes alone change nothing", sub_only);
    CHECK("bench image (warm handoff + meter submodes): the same table", bench);
    CHECK("warm handoff without meter submodes: always the scope", scope_only);
    CHECK("the bug: Startup = Home in the bench image booted the scope",
          startup_boot_mode(STARTUP_HOME, STARTUP_SCREEN_NONE, true, true) == MODE_HOME &&
          startup_boot_mode(STARTUP_SIGGEN, STARTUP_SCREEN_NONE, true, true) == MODE_SIGNAL_GEN);
    CHECK("an unknown choice boots the scope",
          startup_boot_mode((startup_mode_t)42, STARTUP_SCREEN_METER, false, false) == MODE_OSCILLOSCOPE);

    /* The last screen in the record: mode + 1 for the instruments, 0 otherwise. */
    CHECK("a capture records scope 1, meter 2, generator 3",
          startup_screen_of_mode(MODE_OSCILLOSCOPE) == 1 && startup_screen_of_mode(MODE_MULTIMETER) == 2 &&
          startup_screen_of_mode(MODE_SIGNAL_GEN) == 3);
    CHECK("home, settings and garbage record none (0)",
          startup_screen_of_mode(MODE_HOME) == 0 && startup_screen_of_mode(MODE_SETTINGS) == 0 &&
          startup_screen_of_mode(MODE_COUNT) == 0 && startup_screen_of_mode((device_mode_t)200) == 0);
    int ok = 1;
    for (int v = 0; v < 256; v++)
        ok &= startup_screen_clamp((uint8_t)v) == (v <= 3 ? v : 0);
    CHECK("a load keeps 0..3 and reads anything else as none", ok);
    ok = 1;
    for (int m = 0; m <= (int)MODE_SIGNAL_GEN; m++)
        ok &= startup_boot_mode(STARTUP_RESTORE, startup_screen_of_mode((device_mode_t)m), false, false)
              == (device_mode_t)m;
    CHECK("Restore after a save on an instrument boots that instrument", ok);

    /* The Settings row. */
    ok = 1;
    startup_mode_t m = STARTUP_HOME;
    const startup_mode_t seq[5] = { STARTUP_METER, STARTUP_SCOPE, STARTUP_SIGGEN, STARTUP_RESTORE, STARTUP_HOME };
    for (int i = 0; i < 5; i++) { m = startup_mode_step(m, +1); ok &= m == seq[i]; }
    CHECK("RIGHT steps Home, Multimeter, Oscilloscope, Signal Gen, Restore and wraps", ok);
    ok = 1;
    m = STARTUP_HOME;
    const startup_mode_t rev[5] = { STARTUP_RESTORE, STARTUP_SIGGEN, STARTUP_SCOPE, STARTUP_METER, STARTUP_HOME };
    for (int i = 0; i < 5; i++) { m = startup_mode_step(m, -1); ok &= m == rev[i]; }
    CHECK("LEFT from Home wraps to Restore and walks back", ok);
    CHECK("an unknown value steps from the default (Restore)",
          startup_mode_step((startup_mode_t)42, +1) == STARTUP_HOME &&
          startup_mode_step((startup_mode_t)42, -1) == STARTUP_SIGGEN);
    ok = 1;
    for (int i = 0; i < (int)STARTUP_COUNT; i++)
        ok &= startup_mode_from_config(startup_mode_to_config((startup_mode_t)i)) == (startup_mode_t)i;
    CHECK("every startup choice round-trips through the record", ok);
    CHECK("the stored values did not move: 0 scope, 1 meter, 2 siggen, 4 home; 5 is restore",
          startup_mode_to_config(STARTUP_SCOPE) == 0 && startup_mode_to_config(STARTUP_METER) == 1 &&
          startup_mode_to_config(STARTUP_SIGGEN) == 2 && startup_mode_to_config(STARTUP_HOME) == 4 &&
          startup_mode_to_config(STARTUP_RESTORE) == 5);
    CHECK("settings (3) and garbage are not startup choices: they read as the meter, as before",
          startup_mode_from_config(CONFIG_MODE_SETTINGS) == STARTUP_METER &&
          startup_mode_from_config(6) == STARTUP_METER && startup_mode_from_config(255) == STARTUP_METER);
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
