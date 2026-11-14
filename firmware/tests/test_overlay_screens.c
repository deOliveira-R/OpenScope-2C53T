/*
 * Host tests for the small layers as screens of the router (src/ui/
 * overlay_screens.c; ARCH-1 B5): the POWER countdown prompt, the logo, and
 * the display task's bookkeeping of the layers that own the LCD.
 *
 * What is checked: the prompt's states as the input task's POWER loop and
 * auto power-off drive them, each show painted exactly once by a display
 * task that paints from the epoch; that the prompt and the logo have no
 * keys, take none from what is under them, and that POWER always reaches
 * the globals; the logo painted once per show (not at all when main() drew
 * it); and what is repainted when a layer goes: the base without a clear
 * under the prompt's box where the base can, everything otherwise.
 *
 * Build: make test-overlay-screens.
 */
#include <stdio.h>
#include <string.h>

#include "overlay_screens.h"
#include "theme.h"

static int run, passed;
#define CHECK(name, cond) do { run++; if (cond) { passed++; printf("  %-72s[PASS]\n", name); } \
                               else { printf("  %-72s[FAIL]\n", name); } } while (0)

/* ── the paints' stand-ins (display task in the firmware) ──────────── */
static char g_log[128];
static int  g_logo_paints;
void power_prompt_ui_paint(power_prompt_view_t v)
{
    char t[16];
    snprintf(t, sizeof t, "%s%u;", v.kind == POWER_PROMPT_HOLD ? "hold" : v.kind == POWER_PROMPT_GOODBYE ? "bye"
                                   : v.kind == POWER_PROMPT_AUTOOFF ? "auto" : "?", (unsigned)v.count);
    strncat(g_log, t, sizeof g_log - strlen(g_log) - 1);
}
void logo_ui_paint(void) { g_logo_paints++; logo_screen_mark_painted(); }

/* The display task's gate for a layer (main.c layer_paint_update). */
static uint32_t g_gate; static const screen_t *g_gated;
static void frames(const screen_t *s, int n)
{
    for (int i = 0; i < n; i++) {
        const uint32_t e = s->epoch();
        if (g_gated == s && e == g_gate) continue;
        s->draw(SCR_PAINT_UPDATE, 0);
        g_gate = e; g_gated = s;
    }
}

/* ── the router around them ────────────────────────────────────────── */
static int g_base_keys, g_globals_asked; static button_id_t g_global_btn;
static scr_res_t base_key(scr_key_t k) { (void)k; g_base_keys++; return SCR_HANDLED; }
static void base_draw(scr_paint_t how, uint32_t frame) { (void)how; (void)frame; }
static const screen_t S_METER = { .name = "meter", .id = SCR_METER, .mode = MODE_MULTIMETER, .instrument = 1,
                                  .key = base_key, .draw = base_draw };
static bool h_globals(button_id_t b, bool l) { (void)l; g_globals_asked++; g_global_btn = b; return b == BTN_POWER; }
static const screen_hooks_t HOOKS = { .globals = h_globals };

static void boot(void)
{
    theme_init(THEME_DARK_BLUE);
    power_prompt_hide();
    logo_screen_hide();
    screen_reset();
    screen_register(&S_METER); screen_register(&power_prompt_screen); screen_register(&logo_screen);
    screen_set_hooks(&HOOKS);
    screen_init(SCR_METER);
    g_log[0] = '\0'; g_logo_paints = 0; g_gated = NULL;
    g_base_keys = g_globals_asked = 0; g_global_btn = BTN_NONE;
}

static void test_entries(void)
{
    printf("the screens\n");
    CHECK("power_prompt: SCR_POWER_PROMPT, OPAQUE | NO_BARS, not GRAB, no key, a draw and an epoch",
          strcmp(power_prompt_screen.name, "power_prompt") == 0 && power_prompt_screen.id == SCR_POWER_PROMPT &&
          power_prompt_screen.mode == MODE_COUNT && power_prompt_screen.flags == (SCR_F_OPAQUE | SCR_F_NO_BARS) &&
          power_prompt_screen.key == NULL && power_prompt_screen.draw && power_prompt_screen.epoch &&
          power_prompt_screen.groups == 0);
    CHECK("logo: SCR_LOGO, OPAQUE | NO_BARS, not GRAB, no key, a draw and an epoch",
          strcmp(logo_screen.name, "logo") == 0 && logo_screen.id == SCR_LOGO && logo_screen.mode == MODE_COUNT &&
          logo_screen.flags == (SCR_F_OPAQUE | SCR_F_NO_BARS) && logo_screen.key == NULL && logo_screen.draw &&
          logo_screen.epoch && logo_screen.groups == 0);
}

/* The input task's POWER loop: show 3 (the claim), then 3, 2, 1 a second
 * apart (20 display frames each), then Goodbye. */
static void test_power_hold(void)
{
    printf("the POWER countdown, held to the end\n");
    boot();
    CHECK("nothing shown: not up", !power_prompt_up() && power_prompt_view().kind == POWER_PROMPT_NONE);
    power_prompt_show(POWER_PROMPT_HOLD, 3);
    CHECK("the claim: up at once", power_prompt_up() && power_prompt_view().count == 3);
    frames(&power_prompt_screen, 2);                 /* the 120 ms before the count */
    for (int c = 3; c > 0; c--) {
        power_prompt_show(POWER_PROMPT_HOLD, (uint8_t)c);
        frames(&power_prompt_screen, 20);
    }
    power_prompt_show(POWER_PROMPT_GOODBYE, 0);
    frames(&power_prompt_screen, 10);                /* the 500 ms before the power-off */
    CHECK("painted: the box at the claim, again at each second (3 2 1), then Goodbye; nothing in between",
          strcmp(g_log, "hold3;hold3;hold2;hold1;bye0;") == 0);
    CHECK("still up at the power-off", power_prompt_up());
}

static void test_power_release(void)
{
    printf("the POWER countdown, released\n");
    boot();
    power_prompt_show(POWER_PROMPT_HOLD, 3);
    frames(&power_prompt_screen, 2);
    power_prompt_show(POWER_PROMPT_HOLD, 3);
    frames(&power_prompt_screen, 7);
    power_prompt_hide();
    CHECK("released: down at once, two paints were made", !power_prompt_up() && strcmp(g_log, "hold3;hold3;") == 0);
    g_log[0] = '\0'; g_gated = NULL;                 /* main.c: a layer coming up starts the gate over */
    power_prompt_show(POWER_PROMPT_HOLD, 3);
    frames(&power_prompt_screen, 2);
    CHECK("pressed again: the box is painted again (the same text is a new show)", strcmp(g_log, "hold3;") == 0);
    const uint32_t e = power_prompt_screen.epoch();
    theme_init(THEME_CLASSIC_GREEN);
    CHECK("its epoch follows the theme", power_prompt_screen.epoch() != e);
}

static void test_autooff(void)
{
    printf("auto power-off\n");
    boot();
    power_prompt_show(POWER_PROMPT_AUTOOFF, 0);
    frames(&power_prompt_screen, 18);                /* the 120 + 800 ms before the power-off */
    CHECK("\"Auto power off / Goodbye!\" painted once", strcmp(g_log, "auto0;") == 0 && power_prompt_up());
}

static void test_router(void)
{
    printf("the router: no keys for them, none for what is under them, POWER to the globals\n");
    boot();
    power_prompt_show(POWER_PROMPT_HOLD, 3);
    (void)screen_push(SCR_POWER_PROMPT);
    CHECK("the prompt on top: meter > power_prompt, no bars, no popups",
          screen_top() == SCR_POWER_PROMPT && !screen_bars_wanted() && !screen_popups_allowed());
    CHECK("an arrow: nobody's (the box has no keys and hides the meter's)",
          screen_route_key(BTN_UP) == SCR_UNHANDLED && g_base_keys == 0 && screen_last_handler() == SCR_COUNT);
    CHECK("POWER: the globals', as on every screen",
          screen_route_key(BTN_POWER) == SCR_HANDLED && g_global_btn == BTN_POWER && g_base_keys == 0);
    power_prompt_hide();
    screen_pop();
    CHECK("the prompt gone: the meter has its keys back", screen_route_key(BTN_UP) == SCR_HANDLED && g_base_keys == 1);
    logo_screen_show(false);
    (void)screen_push(SCR_LOGO);
    g_base_keys = 0;
    CHECK("the logo on top: the same", screen_route_key(BTN_OK) == SCR_UNHANDLED && g_base_keys == 0 &&
          screen_route_key(BTN_POWER) == SCR_HANDLED && !screen_bars_wanted());
}

static void test_logo(void)
{
    printf("the logo\n");
    boot();
    logo_screen_show(true);                          /* boot: main() painted it */
    CHECK("boot: up, and counted as painted", logo_screen_up() && logo_screen_painted());
    frames(&logo_screen, 20);                        /* the ~1 s hold */
    CHECK("the display task does not paint it again", g_logo_paints == 0);
    logo_screen_hide();
    CHECK("the hold over: down", !logo_screen_up());

    g_gated = NULL;
    logo_screen_show(false);                         /* `logo show` */
    CHECK("preview: up, not painted yet", logo_screen_up() && !logo_screen_painted());
    frames(&logo_screen, 40);
    CHECK("the display task paints it once, and the shell sees it painted",
          g_logo_paints == 1 && logo_screen_painted());
    logo_screen.draw(SCR_PAINT_FULL, 0);
    CHECK("a FULL paint (a box over it went) paints it again", g_logo_paints == 2);
    logo_screen_hide();
    g_gated = NULL;
    logo_screen_show(false);
    frames(&logo_screen, 3);
    CHECK("a second preview is painted again", g_logo_paints == 3);
    logo_screen_hide();
    logo_screen.draw(SCR_PAINT_FULL, 0);
    CHECK("down: nothing is painted", g_logo_paints == 3);
}

/* What the display task repaints as layers come and go. */
static const screen_t S_GAL = { .name = "gallery", .id = SCR_GALLERY, .mode = MODE_COUNT,
                                .flags = SCR_F_OPAQUE | SCR_F_GRAB | SCR_F_NO_BARS };
static void test_layer_track(void)
{
    printf("when a layer goes: what is repainted\n");
    layer_track_t t = { 0 };
    CHECK("no layer, none before: nothing", !layer_track_step(&t, NULL) &&
          layer_track_take(&t, true) == LAYER_REPAINT_NONE);

    CHECK("the prompt comes up: the gate starts over, no whole paint owed (the box paints itself)",
          layer_track_step(&t, &power_prompt_screen) && !layer_track_take_whole(&t) &&
          !layer_track_step(&t, &power_prompt_screen));
    CHECK("it goes over a base that is a real screen: that screen, without a clear (RESTORE)",
          layer_track_step(&t, NULL) && layer_track_take(&t, true) == LAYER_REPAINT_RESTORE);
    CHECK("... once", layer_track_take(&t, true) == LAYER_REPAINT_NONE);

    (void)layer_track_step(&t, &power_prompt_screen);
    (void)layer_track_step(&t, NULL);
    CHECK("it goes over the scope, the settings or the menu page (no restore paint): everything",
          layer_track_take(&t, false) == LAYER_REPAINT_ALL && layer_track_take(&t, false) == LAYER_REPAINT_NONE);

    (void)layer_track_step(&t, &S_GAL);
    CHECK("the gallery comes up: no whole paint owed (it paints from its own state)", !layer_track_take_whole(&t));
    (void)layer_track_step(&t, NULL);
    CHECK("the gallery goes: everything (it covered the bars), whatever the base",
          layer_track_take(&t, true) == LAYER_REPAINT_ALL);

    (void)layer_track_step(&t, &logo_screen);
    (void)layer_track_step(&t, NULL);
    CHECK("the logo goes: everything", layer_track_take(&t, true) == LAYER_REPAINT_ALL);

    (void)layer_track_step(&t, &S_GAL);
    CHECK("POWER in the gallery (it closes, the box comes up over its picture): the box, whole",
          layer_track_step(&t, &power_prompt_screen) && layer_track_take_whole(&t) && !layer_track_take_whole(&t));
    (void)layer_track_step(&t, NULL);
    CHECK("... released: everything, though the base could restore (the gallery's picture is under the box)",
          layer_track_take(&t, true) == LAYER_REPAINT_ALL);

    (void)layer_track_step(&t, &logo_screen);
    (void)layer_track_step(&t, &power_prompt_screen);
    (void)layer_track_take_whole(&t);
    CHECK("POWER over the logo, released while the logo is still up: the logo again, whole; nothing else yet",
          layer_track_step(&t, &logo_screen) && layer_track_take_whole(&t));
    (void)layer_track_step(&t, NULL);
    CHECK("... then everything when the logo goes", layer_track_take(&t, true) == LAYER_REPAINT_ALL);

    (void)layer_track_step(&t, &S_GAL);
    (void)layer_track_step(&t, NULL);
    layer_track_redrawn(&t);
    CHECK("a full redraw made in between (DCMD_REDRAW_ALL): nothing more is owed",
          layer_track_take(&t, true) == LAYER_REPAINT_NONE);
}

int main(void)
{
    test_entries();
    test_power_hold();
    test_power_release();
    test_autooff();
    test_router();
    test_logo();
    test_layer_track();
    printf("\n%d/%d passed\n", passed, run);
    return passed == run ? 0 : 1;
}
