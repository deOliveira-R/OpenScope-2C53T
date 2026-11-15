/*
 * Host tests for the Settings screens (src/ui/settings_screen.c; ARCH-1 B4 /
 * B8): the real screens, the real router (screen.c) and the real property
 * model, with fake hardware ops and a fake environment (the click, the
 * reset, the USB action, the firmware update, the page-left flush).
 *
 * What is checked:
 *   - the key table of settings_screen.h: every key, short and long, on the
 *     list, the question, Sound and Light and About: the screen's answer,
 *     what it changed and which environment function it called;
 *   - every value changes through its property (sys.autooff, sys.startup,
 *     meter.link, sys.theme, sys.volume, sys.brightness): prop_get answers
 *     it at once, the group's epoch moves and the settings are noted; a key
 *     that changes nothing notes nothing and moves no epoch;
 *   - which page is shown is the router's stack; `ui screen`'s
 *     settings=<page>/<row>/<sub> keeps its numbers (0, 2, 7, 8);
 *   - the epochs the display task gates the paints on;
 *   - 6000 random keys against a transcription of the handlers these
 *     screens replace (input_handler.c mode_key / input_handle_settings_ok
 *     on settings_depth / settings_selected / settings_sub_selected): the
 *     same page, rows, values, environment calls and repaint after each.
 *
 * Build: make test-settings-screen.
 */
#include "screen_test_env.h"
#include "settings_screen.h"
#include "startup_policy.h"

/* ── the paint and the environment: counted ────────────────────────── */
static int g_paint_list, g_paint_question, g_paint_sound, g_paint_about;
static uint8_t g_paint_arg;
void settings_ui_draw_list(uint8_t row)           { g_paint_list++; g_paint_arg = row; }
void settings_ui_draw_reset_question(uint8_t row) { g_paint_question++; g_paint_arg = row; }
void settings_ui_draw_sound(uint8_t sub)          { g_paint_sound++; g_paint_arg = sub; }
void settings_ui_draw_about(void)                 { g_paint_about++; }

typedef struct { int click, left, reset, usb, fw; } envc_t;
static envc_t E;
void settings_env_click(void)            { E.click++; }
void settings_env_page_left(void)        { E.left++; }
void settings_env_reset(void)            { E.reset++; }
void settings_env_usb_share_toggle(void) { E.usb++; }
void settings_env_fw_update(void)        { E.fw++; }

static const screen_t S_SCOPE = ENV_BASE("scope", SCR_SCOPE, MODE_OSCILLOSCOPE, 0);
static const screen_t S_HOME  = ENV_BASE("home",  SCR_HOME,  MODE_HOME, SCR_INSTRUMENT_NONE);

#define LONG(b) ((uint8_t)((b) | BTN_LONG_FLAG))

static void boot(void)
{
    env_fresh(MODE_SETTINGS);
    screen_reset();
    screen_register(&S_SCOPE); screen_register(&S_HOME);
    screen_register(&settings_screen); screen_register(&settings_sound_screen);
    screen_register(&settings_about_screen);
    screen_init(SCR_SETTINGS);
    memset(&E, 0, sizeof E);
    g_paint_list = g_paint_question = g_paint_sound = g_paint_about = 0;
    env_log_sys = true;
    env_notes = 0;
    env_log_clear();
}

static int32_t G(const prop_desc_t *p) { return prop_get(p); }
static settings_view_t V(void) { return settings_screen_view(); }
static bool view_is(unsigned page, unsigned row, unsigned sub)
{
    const settings_view_t v = V();
    return v.page == page && v.row == row && v.sub == sub;
}
static const char *described(void)
{
    static char d[32];
    d[0] = '\0';
    settings_screen.describe(d, sizeof d);
    return d;
}
static void keys(uint8_t raw, int n) { while (n-- > 0) (void)env_key(raw); }
static void to_row(settings_item_t r) { boot(); keys(BTN_DOWN, (int)r); }
static void to_sound(void) { boot(); (void)env_key(BTN_OK); }                        /* row 0 */
static void to_about(void) { to_row(SET_ITEM_ABOUT); (void)env_key(BTN_OK); }
static void to_question(void) { to_row(SET_ITEM_RESET_DEFAULTS); (void)env_key(BTN_OK); }

static void test_entries(void)
{
    printf("the screens\n");
    CHECK("settings: SCR_SETTINGS, MODE_SETTINGS, no instrument, a base",
          strcmp(settings_screen.name, "settings") == 0 && settings_screen.id == SCR_SETTINGS &&
          settings_screen.mode == MODE_SETTINGS && settings_screen.instrument == SCR_INSTRUMENT_NONE &&
          settings_screen.flags == 0);
    CHECK("set_sound, set_about: pages (CHILD | OPAQUE), no mode",
          strcmp(settings_sound_screen.name, "set_sound") == 0 && settings_sound_screen.id == SCR_SET_SOUND &&
          strcmp(settings_about_screen.name, "set_about") == 0 && settings_about_screen.id == SCR_SET_ABOUT &&
          settings_sound_screen.mode == MODE_COUNT && settings_about_screen.mode == MODE_COUNT &&
          settings_sound_screen.flags == (SCR_F_CHILD | SCR_F_OPAQUE) &&
          settings_about_screen.flags == (SCR_F_CHILD | SCR_F_OPAQUE));
    CHECK("each has key, draw, epoch and describe; the list has open and close",
          settings_screen.key && settings_screen.draw && settings_screen.epoch && settings_screen.describe &&
          settings_screen.open && settings_screen.close &&
          settings_sound_screen.key && settings_sound_screen.draw && settings_sound_screen.epoch &&
          settings_about_screen.key && settings_about_screen.draw && settings_about_screen.epoch);
    CHECK("every property group repaints them", settings_screen.groups == (uint16_t)((1u << PG_COUNT) - 1u) &&
          settings_sound_screen.groups == settings_screen.groups && settings_about_screen.groups == settings_screen.groups);
    boot();
    CHECK("entered: the list, row 0, settings=0/0/0, stack settings",
          view_is(SET_PAGE_LIST, 0, 0) && strcmp(described(), "settings=0/0/0") == 0 &&
          screen_snapshot().n == 1 && screen_top() == SCR_SETTINGS);
}

static void test_list_rows(void)
{
    printf("the list: the rows\n");
    boot();
    CHECK("UP on row 0 stays (asked to paint; the epoch did not move)",
          env_key(BTN_UP) == SCR_DRAW && view_is(0, 0, 0));
    CHECK("DOWN: row 1", env_key(BTN_DOWN) == SCR_DRAW && view_is(0, 1, 0));
    keys(BTN_DOWN, 20);
    CHECK("DOWN stops on the last row (About)", view_is(0, SET_ITEM_ABOUT, 0));
    CHECK("UP: one row up", env_key(BTN_UP) == SCR_DRAW && view_is(0, SET_ITEM_ABOUT - 1, 0));
    CHECK("moving the row sets nothing and notes nothing", env_notes == 0 && env_log_is(""));
}

static void test_list_values(void)
{
    printf("the list: every value is its property\n");
    /* Auto Shutdown */
    to_row(SET_ITEM_AUTOOFF);
    env_epochs_t e0 = env_epochs();
    CHECK("Auto Shutdown RIGHT: sys.autooff off -> 15 min, the timer re-armed",
          env_key(BTN_RIGHT) == SCR_DRAW && G(P(SYS_AUTOOFF)) == 1 && env_log_is("autooff(15);"));
    env_epochs_t e1 = env_epochs();
    CHECK("... the sys group's epoch moved and the settings were noted once",
          env_epochs_moved(&e0, &e1) == (1u << PG_SYS) && env_notes == 1);
    keys(BTN_RIGHT, 5);
    CHECK("RIGHT stops at 60 min", G(P(SYS_AUTOOFF)) == 3);
    const unsigned n3 = env_notes;
    e0 = env_epochs();
    env_log_clear();
    CHECK("RIGHT at the end: nothing set, nothing noted, no epoch moved",
          env_key(BTN_RIGHT) == SCR_DRAW && G(P(SYS_AUTOOFF)) == 3 && env_notes == n3 && env_log_is("") &&
          ((e1 = env_epochs()), env_epochs_moved(&e0, &e1) == 0));
    CHECK("OK at the end goes round to off, a full repaint",
          env_key(BTN_OK) == SCR_DRAW_FULL && G(P(SYS_AUTOOFF)) == 0 && env_log_is("autooff(0);"));
    CHECK("LEFT at off stays", env_key(BTN_LEFT) == SCR_DRAW && G(P(SYS_AUTOOFF)) == 0);
    CHECK("OK: off -> 15 min", env_key(BTN_OK) == SCR_DRAW_FULL && G(P(SYS_AUTOOFF)) == 1);
    CHECK("LEFT: back to off", env_key(BTN_LEFT) == SCR_DRAW && G(P(SYS_AUTOOFF)) == 0);

    /* Startup on Boot: stock's row order home, meter, scope, generator, then Restore; round */
    to_row(SET_ITEM_STARTUP);
    int ok = G(P(SYS_STARTUP)) == STARTUP_RESTORE;
    static const startup_mode_t fwd[] = { STARTUP_HOME, STARTUP_METER, STARTUP_SCOPE, STARTUP_SIGGEN, STARTUP_RESTORE };
    for (unsigned i = 0; i < 5; i++) { ok &= env_key(BTN_RIGHT) == SCR_DRAW; ok &= G(P(SYS_STARTUP)) == (int32_t)fwd[i]; }
    CHECK("Startup RIGHT x5 from Last Used: home, meter, scope, generator, Last Used", ok);
    ok = 1;
    static const startup_mode_t back[] = { STARTUP_SIGGEN, STARTUP_SCOPE, STARTUP_METER, STARTUP_HOME, STARTUP_RESTORE };
    for (unsigned i = 0; i < 5; i++) { ok &= env_key(BTN_LEFT) == SCR_DRAW; ok &= G(P(SYS_STARTUP)) == (int32_t)back[i]; }
    CHECK("Startup LEFT x5: the same order backwards, round", ok);
    CHECK("Startup OK: one step forward", env_key(BTN_OK) == SCR_DRAW_FULL && G(P(SYS_STARTUP)) == STARTUP_HOME &&
                                          startup_mode == STARTUP_HOME);

    /* Meter on Battery */
    to_row(SET_ITEM_METER_LINK);
    const int32_t link0 = G(P(METER_LINK));
    e0 = env_epochs();
    CHECK("Meter on Battery RIGHT: meter.link flips", env_key(BTN_RIGHT) == SCR_DRAW && G(P(METER_LINK)) == !link0);
    e1 = env_epochs();
    CHECK("... the meter group's epoch moved, noted", env_epochs_moved(&e0, &e1) == (1u << PG_METER) && env_notes == 1);
    CHECK("LEFT flips it back, OK flips again",
          env_key(BTN_LEFT) == SCR_DRAW && G(P(METER_LINK)) == link0 &&
          env_key(BTN_OK) == SCR_DRAW_FULL && G(P(METER_LINK)) == !link0);

    /* Theme */
    to_row(SET_ITEM_THEME);
    CHECK("Theme RIGHT: sys.theme +1, the theme in force follows",
          env_key(BTN_RIGHT) == SCR_DRAW && G(P(SYS_THEME)) == 1 && theme_get_id() == 1);
    keys(BTN_RIGHT, 3);
    CHECK("Theme RIGHT goes round", G(P(SYS_THEME)) == 0);
    CHECK("Theme LEFT goes round the other way", env_key(BTN_LEFT) == SCR_DRAW && G(P(SYS_THEME)) == 3);
    CHECK("Theme OK: +1", env_key(BTN_OK) == SCR_DRAW_FULL && G(P(SYS_THEME)) == 0);

    /* a `set` from elsewhere is what the row steps from */
    to_row(SET_ITEM_AUTOOFF);
    (void)prop_set_local(P(SYS_AUTOOFF), 2, ORIGIN_SHELL, NULL);
    CHECK("after `set sys.autooff 30`: RIGHT steps from it to 60", env_key(BTN_RIGHT) == SCR_DRAW && G(P(SYS_AUTOOFF)) == 3);

    /* rows without a value */
    static const settings_item_t none[] = { SET_ITEM_SOUND_LIGHT, SET_ITEM_USB_SHARE, SET_ITEM_FW_UPDATE,
                                            SET_ITEM_RESET_DEFAULTS, SET_ITEM_ABOUT };
    ok = 1;
    for (unsigned i = 0; i < sizeof none / sizeof none[0]; i++) {
        to_row(none[i]);
        e0 = env_epochs();
        ok &= env_key(BTN_LEFT) == SCR_HANDLED && env_key(BTN_RIGHT) == SCR_HANDLED;
        e1 = env_epochs();
        ok &= env_epochs_moved(&e0, &e1) == 0 && env_notes == 0 && env_log_is("") && view_is(0, none[i], 0);
        ok &= E.click == 0 && E.left == 0 && E.reset == 0 && E.usb == 0 && E.fw == 0;
    }
    CHECK("LEFT / RIGHT on a page or action row: handled, nothing changes, no repaint asked", ok);
}

static void test_list_actions(void)
{
    printf("the list: pages and actions\n");
    boot();
    CHECK("OK on Sound and Light: the page is pushed (stack settings > set_sound), settings=7/0/0",
          env_key(BTN_OK) == SCR_DRAW_FULL && screen_top() == SCR_SET_SOUND && screen_snapshot().n == 2 &&
          strcmp(described(), "settings=7/0/0") == 0);
    to_row(SET_ITEM_ABOUT);
    CHECK("OK on About: stack settings > set_about, settings=2/8/0",
          env_key(BTN_OK) == SCR_DRAW_FULL && screen_top() == SCR_SET_ABOUT && strcmp(described(), "settings=2/8/0") == 0);
    to_row(SET_ITEM_USB_SHARE);
    CHECK("OK on USB Sharing: the one action, no property, no note",
          env_key(BTN_OK) == SCR_DRAW_FULL && E.usb == 1 && env_notes == 0 && env_log_is("") && view_is(0, SET_ITEM_USB_SHARE, 0));
    to_row(SET_ITEM_FW_UPDATE);
    CHECK("OK on Firmware Update: settings_env_fw_update, once",
          env_key(BTN_OK) == SCR_DRAW_FULL && E.fw == 1 && E.usb == 0 && E.reset == 0);
    boot();
    CHECK("MENU on the list: Home, as one sys.mode set", env_key(BTN_MENU) == SCR_HANDLED && env_log_is("mode(4);") &&
                                                         screen_base() == SCR_HOME && E.left == 0);
    boot();
    env_mode_applies = false;
    (void)env_key(BTN_MENU);
    CHECK("... with the owner only logging: the screen moved no mode itself",
          current_mode == MODE_SETTINGS && screen_base() == SCR_SETTINGS);
}

static void test_question(void)
{
    printf("Reset to Defaults: the question\n");
    to_row(SET_ITEM_RESET_DEFAULTS);
    CHECK("OK on the row only asks: page 8 over the list, stack still settings, nothing reset",
          env_key(BTN_OK) == SCR_DRAW_FULL && view_is(SET_PAGE_RESET, SET_ITEM_RESET_DEFAULTS, 0) &&
          screen_snapshot().n == 1 && E.reset == 0 && strcmp(described(), "settings=8/7/0") == 0);
    CHECK("UP / DOWN on the question: the row does not move",
          env_key(BTN_UP) == SCR_DRAW && env_key(BTN_DOWN) == SCR_DRAW && view_is(SET_PAGE_RESET, SET_ITEM_RESET_DEFAULTS, 0));
    CHECK("LEFT / RIGHT: nothing", env_key(BTN_LEFT) == SCR_HANDLED && env_key(BTN_RIGHT) == SCR_HANDLED && E.reset == 0);
    int ok = 1;
    static const uint8_t other[] = { BTN_AUTO, BTN_PRM, BTN_SELECT, LONG(BTN_OK), LONG(BTN_AUTO), LONG(BTN_PRM),
                                     LONG(BTN_CH1), LONG(BTN_CH2) };
    for (unsigned i = 0; i < sizeof other; i++) ok &= env_key(other[i]) == SCR_HANDLED;
    CHECK("AUTO, PRM, SELECT and the long keys: nothing, the question stays",
          ok && E.reset == 0 && V().page == SET_PAGE_RESET);
    CHECK("MENU = no: the list again, nothing reset, the boundary flush",
          env_key(BTN_MENU) == SCR_DRAW_FULL && view_is(0, SET_ITEM_RESET_DEFAULTS, 0) && E.reset == 0 && E.left == 1 &&
          env_log_is(""));
    to_question();
    CHECK("OK = yes: settings_env_reset once, the list again, still in Settings",
          env_key(BTN_OK) == SCR_DRAW_FULL && E.reset == 1 && view_is(0, SET_ITEM_RESET_DEFAULTS, 0) &&
          screen_base() == SCR_SETTINGS);
    CHECK("OK again asks again (it never resets on one press)",
          env_key(BTN_OK) == SCR_DRAW_FULL && E.reset == 1 && V().page == SET_PAGE_RESET);
    to_question();
    (void)prop_set_local(P(SYS_MODE), MODE_HOME, ORIGIN_SHELL, NULL);
    (void)prop_set_local(P(SYS_MODE), MODE_SETTINGS, ORIGIN_SHELL, NULL);
    CHECK("leaving Settings drops the question; entering again: the list, row 0", view_is(0, 0, 0));
}

static void test_sound(void)
{
    printf("Sound and Light\n");
    to_sound();
    CHECK("UP on the first row stays; DOWN: Brightness (settings=7/0/1); DOWN stays",
          env_key(BTN_UP) == SCR_DRAW && view_is(7, 0, 0) && env_key(BTN_DOWN) == SCR_DRAW && view_is(7, 0, 1) &&
          strcmp(described(), "settings=7/0/1") == 0 && env_key(BTN_DOWN) == SCR_DRAW && view_is(7, 0, 1));
    to_sound();
    env_epochs_t e0 = env_epochs();
    CHECK("Sound RIGHT: sys.volume 1 -> 2, a click at the new level",
          env_key(BTN_RIGHT) == SCR_DRAW && G(P(SYS_VOLUME)) == 2 && sys_prefs_volume() == 2 && E.click == 1);
    env_epochs_t e1 = env_epochs();
    CHECK("... sys epoch moved, noted once", env_epochs_moved(&e0, &e1) == (1u << PG_SYS) && env_notes == 1);
    keys(BTN_RIGHT, 12);
    CHECK("RIGHT stops at 10 (a click each time, as before)", G(P(SYS_VOLUME)) == 10 && E.click == 13);
    const unsigned n = env_notes;
    (void)env_key(BTN_RIGHT);
    CHECK("RIGHT at 10: nothing noted", G(P(SYS_VOLUME)) == 10 && env_notes == n);
    CHECK("OK at 10 goes round to 0 (off), a full repaint", env_key(BTN_OK) == SCR_DRAW_FULL && G(P(SYS_VOLUME)) == 0);
    CHECK("LEFT at 0 stays", env_key(BTN_LEFT) == SCR_DRAW && G(P(SYS_VOLUME)) == 0);
    to_sound();
    (void)env_key(BTN_DOWN);
    env_log_clear();
    CHECK("Brightness LEFT: sys.brightness 100 -> 95 %, the backlight applied, no click",
          env_key(BTN_LEFT) == SCR_DRAW && G(P(SYS_BRIGHTNESS)) == 95 && env_log_is("backlight(95);") && E.click == 0);
    keys(BTN_LEFT, 20);
    CHECK("LEFT stops at 25 %", G(P(SYS_BRIGHTNESS)) == 25);
    CHECK("RIGHT: 30 %", env_key(BTN_RIGHT) == SCR_DRAW && G(P(SYS_BRIGHTNESS)) == 30);
    (void)prop_set_local(P(SYS_BRIGHTNESS), 100, ORIGIN_SHELL, NULL);
    CHECK("OK at 100 % goes round to 25 %", env_key(BTN_OK) == SCR_DRAW_FULL && G(P(SYS_BRIGHTNESS)) == 25);
    CHECK("MENU: back to the list, row 0, the page popped, the boundary flush",
          env_key(BTN_MENU) == SCR_DRAW_FULL && screen_top() == SCR_SETTINGS && view_is(0, 0, 0) && E.left == 1);
    CHECK("OK again: the page opens on its first row", env_key(BTN_OK) == SCR_DRAW_FULL && view_is(7, 0, 0));
    (void)prop_set_local(P(SYS_MODE), MODE_HOME, ORIGIN_SHELL, NULL);
    CHECK("a mode change with the page up pops it", screen_snapshot().n == 1 && screen_base() == SCR_HOME);
}

static void test_about(void)
{
    printf("About\n");
    to_about();
    const uint32_t ep = settings_about_screen.epoch();
    CHECK("UP / DOWN: painted again (the live numbers), the epoch moves",
          env_key(BTN_UP) == SCR_DRAW && settings_about_screen.epoch() != ep && view_is(2, SET_ITEM_ABOUT, 0));
    CHECK("LEFT / RIGHT: nothing", env_key(BTN_LEFT) == SCR_HANDLED && env_key(BTN_RIGHT) == SCR_HANDLED);
    CHECK("OK goes back (no flush)", env_key(BTN_OK) == SCR_DRAW_FULL && screen_top() == SCR_SETTINGS &&
                                     view_is(0, SET_ITEM_ABOUT, 0) && E.left == 0);
    (void)env_key(BTN_OK);
    CHECK("MENU goes back, the boundary flush", env_key(BTN_MENU) == SCR_DRAW_FULL && screen_top() == SCR_SETTINGS &&
                                                E.left == 1 && current_mode == MODE_SETTINGS);
    CHECK("nothing on About set a property", env_notes == 0 && env_log_is(""));
}

static void test_common_keys(void)
{
    printf("keys with no meaning here\n");
    static void (*const where[])(void) = { boot, to_question, to_sound, to_about };
    static const char *const name[] = { "the list", "the question", "Sound and Light", "About" };
    static const uint8_t full[] = { BTN_CH1, BTN_CH2, BTN_TRIGGER, BTN_MOVE };
    static const uint8_t none[] = { BTN_AUTO, BTN_PRM, BTN_SELECT, LONG(BTN_AUTO), LONG(BTN_OK), LONG(BTN_PRM),
                                    LONG(BTN_CH1), LONG(BTN_CH2), LONG(BTN_MENU), LONG(BTN_MOVE), LONG(BTN_SELECT),
                                    LONG(BTN_TRIGGER), LONG(BTN_SAVE), BTN_SAVE, BTN_POWER };
    for (unsigned w = 0; w < 4; w++) {
        int ok = 1;
        char label[80];
        for (unsigned i = 0; i < sizeof full + sizeof none; i++) {
            where[w]();
            const settings_view_t v0 = V();
            const screen_stack_t s0 = screen_snapshot();
            const env_epochs_t e0 = env_epochs();
            const envc_t c0 = E;
            const uint8_t k = i < sizeof full ? full[i] : none[i - sizeof full];
            ok &= env_key(k) == (i < sizeof full ? SCR_DRAW_FULL : SCR_HANDLED);
            const env_epochs_t e1 = env_epochs();
            const settings_view_t v1 = V();
            ok &= env_epochs_moved(&e0, &e1) == 0 && env_notes == 0 && env_log_is("");
            ok &= v0.page == v1.page && v0.row == v1.row && v0.sub == v1.sub && screen_snapshot().n == s0.n;
            ok &= memcmp(&c0, &E, sizeof E) == 0;
        }
        snprintf(label, sizeof label, "%s: CH1 CH2 TRIGGER MOVE repaint, the rest nothing; no change", name[w]);
        CHECK(label, ok);
    }
}

static void test_draw_and_epochs(void)
{
    printf("paint and epochs\n");
    boot();
    settings_screen.draw(SCR_PAINT_FULL, 0);
    CHECK("the list paints the list with its row", g_paint_list == 1 && g_paint_question == 0 && g_paint_arg == 0);
    uint32_t e = settings_screen.epoch();
    (void)env_key(BTN_UP);
    CHECK("a key that changed nothing: the list's epoch is the same (no LCD write)", settings_screen.epoch() == e);
    (void)env_key(BTN_DOWN);
    CHECK("a row move changes it", settings_screen.epoch() != e);
    e = settings_screen.epoch();
    (void)prop_set_local(P(SYS_AUTOOFF), 2, ORIGIN_SHELL, NULL);
    CHECK("a `set sys.autooff` changes it", settings_screen.epoch() != e);
    e = settings_screen.epoch();
    (void)prop_set_local(P(SYS_STARTUP), STARTUP_METER, ORIGIN_SHELL, NULL);
    const uint32_t e2 = settings_screen.epoch();
    (void)prop_set_local(P(METER_LINK), !G(P(METER_LINK)), ORIGIN_SHELL, NULL);
    const uint32_t e3 = settings_screen.epoch();
    (void)prop_set_local(P(SYS_THEME), 2, ORIGIN_SHELL, NULL);
    CHECK("... and sys.startup, meter.link, sys.theme each", e2 != e && e3 != e2 && settings_screen.epoch() != e3);
    to_question();
    settings_screen.draw(SCR_PAINT_UPDATE, 0);
    CHECK("with the question up the list paints the question", g_paint_question == 1 && g_paint_list == 0 &&
                                                               g_paint_arg == SET_ITEM_RESET_DEFAULTS);
    e = settings_screen.epoch();
    (void)env_key(BTN_MENU);
    CHECK("the question going changes the epoch", settings_screen.epoch() != e);
    to_sound();
    settings_sound_screen.draw(SCR_PAINT_FULL, 0);
    e = settings_sound_screen.epoch();
    (void)env_key(BTN_DOWN);
    const uint32_t s1 = settings_sound_screen.epoch();
    (void)env_key(BTN_LEFT);
    const uint32_t s2 = settings_sound_screen.epoch();
    (void)env_key(BTN_DOWN);
    CHECK("Sound and Light paints its row; its epoch follows the row and the values, not a clamped key",
          g_paint_sound == 1 && s1 != e && s2 != s1 && settings_sound_screen.epoch() == s2);
    (void)prop_set_local(P(SYS_VOLUME), 7, ORIGIN_SHELL, NULL);
    CHECK("a `set sys.volume` with the page up changes its epoch", settings_sound_screen.epoch() != s2);
    to_about();
    settings_about_screen.draw(SCR_PAINT_RESTORE, 0);
    e = settings_about_screen.epoch();
    CHECK("About paints; its epoch is still while nothing is pressed", g_paint_about == 1 && settings_about_screen.epoch() == e);
}

/* ── the handlers these screens replace, transcribed ───────────────── */
/* input_handler.c before B8: mode_key()'s Settings arms and
 * input_handle_settings_ok(), on the three globals. What it sent to the
 * display task is its answer: REDRAW_ALL = FULL, DCMD_DRAW_ACTIVE = DRAW,
 * nothing = HANDLED; MENU at the top entered Home. */
typedef struct { int8_t depth, sel, sub; } old_t;
static old_t O;
static envc_t OE;

static void old_step(const prop_desc_t *p, int dir, bool wrap)
{
    const int32_t v = prop_get(p);
    const bool at_end = (dir > 0) ? (v >= p->max) : (v <= p->min);
    if (!at_end) (void)prop_step_local(p, dir, ORIGIN_KEY, NULL);
    else if (wrap) (void)prop_set_local(p, (dir > 0) ? p->min : p->max, ORIGIN_KEY, NULL);
}
static void old_sound(int dir, bool wrap)
{
    if (O.sub == SET_SOUND_ROW_VOLUME) { old_step(P(SYS_VOLUME), dir, wrap); OE.click++; }
    else old_step(P(SYS_BRIGHTNESS), dir, wrap);
}
static bool old_value(settings_item_t item, int dir, bool wrap)
{
    switch (item) {
    case SET_ITEM_AUTOOFF: old_step(P(SYS_AUTOOFF), dir, wrap); return true;
    case SET_ITEM_STARTUP:
        (void)prop_set_local(P(SYS_STARTUP),
                             (int32_t)startup_mode_step((startup_mode_t)prop_get(P(SYS_STARTUP)), dir), ORIGIN_KEY, NULL);
        return true;
    case SET_ITEM_METER_LINK: (void)prop_step_local(P(METER_LINK), dir, ORIGIN_KEY, NULL); return true;
    case SET_ITEM_THEME:      (void)prop_step_local(P(SYS_THEME), dir, ORIGIN_KEY, NULL); return true;
    default: return false;
    }
}
static void old_ok(void)
{
    if (O.depth == SET_PAGE_RESET) { OE.reset++; O.depth = SET_PAGE_LIST; return; }
    if (O.depth == SET_PAGE_LIST && O.sel == SET_ITEM_RESET_DEFAULTS) { O.depth = SET_PAGE_RESET; return; }
    if (O.depth == SET_PAGE_SOUND) { old_sound(+1, true); return; }
    if (O.depth == SET_PAGE_ABOUT) { O.depth = SET_PAGE_LIST; return; }
    const settings_item_t item = (settings_item_t)O.sel;
    if (old_value(item, +1, true)) return;
    switch (item) {
    case SET_ITEM_SOUND_LIGHT: O.depth = SET_PAGE_SOUND; O.sub = 0; break;
    case SET_ITEM_ABOUT:       O.depth = SET_PAGE_ABOUT; break;
    case SET_ITEM_USB_SHARE:   OE.usb++; break;
    case SET_ITEM_FW_UPDATE:   OE.fw++; break;
    default: break;
    }
}
enum { OLD_NONE = SCR_HANDLED, OLD_DRAW = SCR_DRAW, OLD_FULL = SCR_DRAW_FULL, OLD_HOME = 100 };
static int old_key(uint8_t raw)
{
    if (raw & BTN_LONG_FLAG) return OLD_NONE;          /* key_map: KA_NONE, nothing sent */
    switch (raw) {
    case BTN_MENU:
        if (O.depth > 0) { O.depth = 0; O.sub = 0; OE.left++; return OLD_FULL; }
        return OLD_HOME;                               /* enter_mode(MODE_HOME) */
    case BTN_CH1: case BTN_CH2: case BTN_TRIGGER: case BTN_MOVE:
        return OLD_FULL;
    case BTN_UP:
        if (O.depth == SET_PAGE_LIST) { if (O.sel > 0) O.sel--; }
        else if (O.depth == SET_PAGE_SOUND) { if (O.sub > 0) O.sub--; }
        return OLD_DRAW;
    case BTN_DOWN:
        if (O.depth == SET_PAGE_LIST) { if (O.sel < SETTINGS_ITEM_COUNT - 1) O.sel++; }
        else if (O.depth == SET_PAGE_SOUND) { if (O.sub < SET_SOUND_ROWS - 1) O.sub++; }
        return OLD_DRAW;
    case BTN_LEFT:
    case BTN_RIGHT: {
        const int dir = (raw == BTN_RIGHT) ? +1 : -1;
        if (O.depth == SET_PAGE_SOUND) { old_sound(dir, false); return OLD_DRAW; }
        if (O.depth == SET_PAGE_LIST && old_value((settings_item_t)O.sel, dir, false)) return OLD_DRAW;
        return OLD_NONE;
    }
    case BTN_OK:
        old_ok();
        return OLD_FULL;
    default:
        return OLD_NONE;
    }
}

typedef struct { int res; int8_t depth, sel, sub; int32_t v[6]; envc_t e; unsigned notes; } snap_t;
static void snap_values(snap_t *s)
{
    s->v[0] = G(P(SYS_AUTOOFF)); s->v[1] = G(P(SYS_STARTUP)); s->v[2] = G(P(METER_LINK));
    s->v[3] = G(P(SYS_THEME));   s->v[4] = G(P(SYS_VOLUME));  s->v[5] = G(P(SYS_BRIGHTNESS));
    s->notes = env_notes;
}

#define SEQ_LEN 6000
static uint8_t  seq[SEQ_LEN];
static snap_t   trace_old[SEQ_LEN];

static void test_against_old_handlers(void)
{
    printf("against the handlers before B8\n");
    static const uint8_t pool[] = { BTN_UP, BTN_DOWN, BTN_DOWN, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_RIGHT, BTN_OK, BTN_OK,
                                    BTN_MENU, BTN_CH1, BTN_CH2, BTN_TRIGGER, BTN_MOVE, BTN_AUTO, BTN_PRM, BTN_SELECT,
                                    LONG(BTN_OK), LONG(BTN_AUTO), LONG(BTN_PRM), LONG(BTN_CH1) };
    uint32_t lcg = 12345u;
    for (int i = 0; i < SEQ_LEN; i++) {
        lcg = lcg * 1664525u + 1013904223u;
        seq[i] = pool[(lcg >> 16) % sizeof pool];
    }
    /* the old handlers */
    boot();
    memset(&O, 0, sizeof O); memset(&OE, 0, sizeof OE);
    for (int i = 0; i < SEQ_LEN; i++) {
        snap_t *s = &trace_old[i];
        s->res = old_key(seq[i]);
        if (s->res == OLD_HOME) { O.sel = 0; O.depth = 0; }     /* Home, and Settings entered again */
        s->depth = O.depth; s->sel = O.sel; s->sub = O.sub; s->e = OE;
        snap_values(s);
    }
    /* the screens */
    boot();
    int bad = -1;
    for (int i = 0; i < SEQ_LEN && bad < 0; i++) {
        const snap_t *w = &trace_old[i];
        snap_t n;
        n.res = (int)env_key(seq[i]);
        if (w->res == OLD_HOME) {
            if (n.res != SCR_HANDLED || screen_base() != SCR_HOME) bad = i;
            n.res = OLD_HOME;
            (void)prop_set_local(P(SYS_MODE), MODE_SETTINGS, ORIGIN_SHELL, NULL);
        }
        const settings_view_t v = V();
        n.depth = (int8_t)v.page; n.sel = (int8_t)v.row; n.sub = (int8_t)v.sub; n.e = E;
        snap_values(&n);
        if (n.res != w->res || n.depth != w->depth || n.sel != w->sel || n.sub != w->sub ||
            memcmp(n.v, w->v, sizeof n.v) != 0 || memcmp(&n.e, &w->e, sizeof n.e) != 0 || n.notes != w->notes) {
            bad = i;
            printf("    key #%d (%u): old res %d page %d/%d/%d, new res %d page %d/%d/%d\n", i, seq[i],
                   w->res, w->depth, w->sel, w->sub, n.res, n.depth, n.sel, n.sub);
        }
        /* the stack says the same as the page */
        const screen_id_t top = screen_top();
        if ((v.page == SET_PAGE_SOUND) != (top == SCR_SET_SOUND) || (v.page == SET_PAGE_ABOUT) != (top == SCR_SET_ABOUT))
            bad = i;
    }
    CHECK("6000 random keys: the same answer, page, rows, values, calls and notes after each", bad < 0);
    CHECK("... and the walk visited the question, both pages, the reset and every action",
          OE.reset > 0 && OE.usb > 0 && OE.fw > 0 && OE.left > 0 && OE.click > 0);
}

int main(void)
{
    printf("settings screens\n");
    test_entries();
    test_list_rows();
    test_list_values();
    test_list_actions();
    test_question();
    test_sound();
    test_about();
    test_common_keys();
    test_draw_and_epochs();
    test_against_old_handlers();
    printf("\n%d/%d passed\n", passed, run);
    return passed == run ? 0 : 1;
}
