/*
 * Host tests for the key model: the press state machine (long presses) and
 * the key -> action map. Both are what the firmware runs: key_scan_tick() is
 * the body of the TMR3 ISR's debounce, key_map() is what the input task asks.
 *
 * Build: see `make test-key-model`.
 */

#include <stdio.h>
#include <string.h>
#include "key_model.h"
#include "scope_state.h"
#include "trig_menu.h"

static int run, passed;
#define CHECK(name, cond) do { run++; if (cond) { passed++; printf("  %-62s[PASS]\n", name); } \
                               else { printf("  %-62s[FAIL]\n", name); } } while (0)

/* ── scan driver: hold `raw` for `ticks`, collect the events ───────── */
typedef struct { int shorts, longs; int last_bit; int at_tick_short, at_tick_long; } tally_t;

static void hold(key_scan_t *s, uint16_t raw, int ticks, int *tick, tally_t *t)
{
    key_scan_event_t ev[KEY_NUM];
    for (int i = 0; i < ticks; i++) {
        (*tick)++;
        uint8_t n = key_scan_tick(s, raw, ev);
        for (uint8_t k = 0; k < n; k++) {
            t->last_bit = ev[k].bit;
            if (ev[k].is_long) { t->longs++;  t->at_tick_long  = *tick; }
            else               { t->shorts++; t->at_tick_short = *tick; }
        }
    }
}

/* One full press: down for `ticks`, then released for a few ticks. */
static tally_t press(uint16_t raw, int ticks)
{
    key_scan_t s; key_scan_init(&s);
    tally_t t; memset(&t, 0, sizeof t);
    int tick = 0;
    hold(&s, raw, ticks, &tick, &t);
    hold(&s, 0, 5, &tick, &t);
    return t;
}

#define BIT(b) ((uint16_t)(1u << (b)))

static void test_scan(void)
{
    printf("press state machine\n");

    /* A key with a long action: tap, short-on-release, long-at-threshold. */
    tally_t t = press(BIT(KEYBIT_MENU), KEY_CONFIRM_TICKS - 1);
    CHECK("long-capable key: contact under 70 ticks = bounce, nothing", t.shorts == 0 && t.longs == 0);

    t = press(BIT(KEYBIT_MENU), KEY_CONFIRM_TICKS);
    CHECK("long-capable key: exactly 70 ticks = ONE short, on release",
          t.shorts == 1 && t.longs == 0 && t.at_tick_short == KEY_CONFIRM_TICKS + 1);

    t = press(BIT(KEYBIT_MENU), KEY_LONG_TICKS - 1);
    CHECK("long-capable key: one tick short of LONG = one short, no long",
          t.shorts == 1 && t.longs == 0);

    t = press(BIT(KEYBIT_MENU), KEY_LONG_TICKS);
    CHECK("long-capable key: held to LONG = one long, at that tick, no short",
          t.longs == 1 && t.shorts == 0 && t.at_tick_long == KEY_LONG_TICKS &&
          t.last_bit == KEYBIT_MENU);

    t = press(BIT(KEYBIT_MENU), KEY_LONG_TICKS * 6);
    CHECK("long-capable key: holding on does not re-fire (one long total)",
          t.longs == 1 && t.shorts == 0);

    /* Every key in the mask behaves so, and the rest do not. */
    int bad = 0;
    for (int b = 0; b < KEY_NUM; b++) {
        tally_t k = press(BIT(b), KEY_LONG_TICKS + 20);
        const bool has_long = (KEY_LONG_MASK >> b) & 1u;
        if (has_long ? !(k.longs == 1 && k.shorts == 0)
                     : (k.longs != 0 || k.shorts < 1))
            bad++;
    }
    CHECK("every key: long exactly when in KEY_LONG_MASK", bad == 0);
    CHECK("POWER is never long-capable (its handler polls the pin)",
          (KEY_LONG_MASK & BIT(KEYBIT_POWER)) == 0);

    /* Keys without a long action fire at the confirm, as before. */
    t = press(BIT(KEYBIT_UP), KEY_CONFIRM_TICKS);
    CHECK("no-long key: short fires AT the confirm, not on release",
          t.shorts == 1 && t.at_tick_short == KEY_CONFIRM_TICKS);

    /* Arrows repeat: 70, then 70+250 = 320, 395, 470 ... */
    key_scan_t s; key_scan_init(&s);
    t = (tally_t){0}; int tick = 0;
    hold(&s, BIT(KEYBIT_RIGHT), KEY_CONFIRM_TICKS + KEY_REPEAT_INITIAL + 2 * KEY_REPEAT_INTERVAL,
         &tick, &t);
    CHECK("arrow held: confirm + first repeat + two more at the interval",
          t.shorts == 4 && t.longs == 0);
    CHECK("arrow held: the last repeat lands on the interval grid",
          t.at_tick_short == KEY_CONFIRM_TICKS + KEY_REPEAT_INITIAL + 2 * KEY_REPEAT_INTERVAL);

    /* Past 255 ticks nothing wraps (the old byte counter saturated at 510 ms). */
    t = press(BIT(KEYBIT_SELECT), 300);
    CHECK("counter is wide: a 600 ms hold is still one short on release", t.shorts == 1 && t.longs == 0);

    /* Two keys at once are independent. */
    key_scan_init(&s); t = (tally_t){0}; tick = 0;
    hold(&s, BIT(KEYBIT_UP) | BIT(KEYBIT_MENU), KEY_CONFIRM_TICKS, &tick, &t);
    CHECK("two keys: UP fires at the confirm while MENU waits for release",
          t.shorts == 1 && t.last_bit == KEYBIT_UP);
}

/* ── mapping ───────────────────────────────────────────────────────── */
static key_ctx_t ctx(device_mode_t m, key_focus_t f)
{
    key_ctx_t c;
    memset(&c, 0, sizeof c);
    c.mode = m; c.focus = f;
    c.scope_time_view = (m == MODE_OSCILLOSCOPE);
    return c;
}

static int is(key_cmd_t k, key_action_t a, int arg)
{
    return k.act == a && k.arg == arg;
}

static void test_long_map(void)
{
    printf("long-press map\n");
    const device_mode_t modes[] = { MODE_OSCILLOSCOPE, MODE_MULTIMETER, MODE_SIGNAL_GEN, MODE_SETTINGS,
                                    MODE_HOME };
    int ok = 1;
    for (unsigned i = 0; i < sizeof modes / sizeof modes[0]; i++) {
        key_ctx_t c = ctx(modes[i], KF_ZOOM);
        ok &= is(key_map(&c, BTN_MENU, true),    KA_ENTER_HOME, 0);
        ok &= is(key_map(&c, BTN_MOVE, true),    KA_ENTER_METER, 0);
        ok &= is(key_map(&c, BTN_SELECT, true),  KA_ENTER_SCOPE, 0);
        ok &= is(key_map(&c, BTN_TRIGGER, true), KA_ENTER_SIGGEN, 0);
    }
    CHECK("long BACK/SELECT/MOVE/TRIGGER = Home/Meter/Scope/Siggen from ANY mode", ok);

    key_ctx_t sc = ctx(MODE_OSCILLOSCOPE, KF_ZOOM);
    CHECK("long PRM in the scope time view = hide measurements",
          is(key_map(&sc, BTN_PRM, true), KA_MEAS_HIDE_TOGGLE, 0));
    key_ctx_t me = ctx(MODE_MULTIMETER, KF_ZOOM);
    CHECK("long PRM in the meter does nothing", is(key_map(&me, BTN_PRM, true), KA_NONE, 0));
    key_ctx_t fv = sc; fv.scope_time_view = false;
    CHECK("long PRM in an FFT view does nothing", is(key_map(&fv, BTN_PRM, true), KA_NONE, 0));

    CHECK("long CH1 / CH2 in the scope = channel enable toggle",
          is(key_map(&sc, BTN_CH1, true), KA_CH_ENABLE_TOGGLE, 0) &&
          is(key_map(&sc, BTN_CH2, true), KA_CH_ENABLE_TOGGLE, 1));
    CHECK("long CH1 outside the scope does nothing", is(key_map(&me, BTN_CH1, true), KA_NONE, 0));

    CHECK("long AUTO in the scope time view = baseline correction (stock 0x0800DF58)",
          is(key_map(&sc, BTN_AUTO, true), KA_BASECAL, 0));
    CHECK("long OK in the scope time view = 50 % centre (stock 0x0800DA3A)",
          is(key_map(&sc, BTN_OK, true), KA_CENTER50, 0));
    CHECK("long AUTO/OK in an FFT view or the meter do nothing",
          is(key_map(&fv, BTN_AUTO, true), KA_NONE, 0) && is(key_map(&fv, BTN_OK, true), KA_NONE, 0) &&
          is(key_map(&me, BTN_AUTO, true), KA_NONE, 0) && is(key_map(&me, BTN_OK, true), KA_NONE, 0));
    {
        int g = 1;
        for (unsigned i = 0; i < sizeof modes / sizeof modes[0]; i++) {
            key_ctx_t c = ctx(modes[i], KF_ZOOM);
            g &= is(key_map(&c, BTN_SAVE, true), KA_GALLERY_OPEN, 0);
        }
        key_ctx_t pg = sc; pg.osc_menu_open = true;
        g &= is(key_map(&pg, BTN_SAVE, true), KA_GALLERY_OPEN, 0);
        CHECK("long SAVE opens the gallery from EVERY mode and the page (D1)", g);
    }
    CHECK("long UP is not a thing", is(key_map(&sc, BTN_UP, true), KA_NONE, 0));

    /* Parity G5: long OK in the generator is Run/Stop (manual 2.1); long AUTO
     * there is the output-at-boot policy (ours). The other modes are unchanged. */
    {
        key_ctx_t sg = ctx(MODE_SIGNAL_GEN, KF_ZOOM);
        CHECK("long OK in the generator is Run/Stop",
              is(key_map(&sg, BTN_OK, true), KA_SIGGEN_RUNSTOP, 0));
        CHECK("long AUTO in the generator is the boot policy toggle",
              is(key_map(&sg, BTN_AUTO, true), KA_SIGGEN_BOOT_TOGGLE, 0));
        CHECK("long OK in the meter does nothing; in the scope it is the 50 % centre",
              is(key_map(&me, BTN_OK, true), KA_NONE, 0) &&
              key_map(&sc, BTN_OK, true).act == KA_CENTER50);
    }
}

static void test_short_map(void)
{
    printf("short-press map (scope time view)\n");
    key_ctx_t z = ctx(MODE_OSCILLOSCOPE, KF_ZOOM);
    CHECK("zoom: LEFT/RIGHT = timebase, UP/DOWN = V/div",
          is(key_map(&z, BTN_LEFT, false),  KA_TB_STEP, -1) &&
          is(key_map(&z, BTN_RIGHT, false), KA_TB_STEP, +1) &&
          is(key_map(&z, BTN_UP, false),    KA_VDIV_STEP, +1) &&
          is(key_map(&z, BTN_DOWN, false),  KA_VDIV_STEP, -1));
    key_ctx_t cm = ctx(MODE_OSCILLOSCOPE, KF_CHMOVE);
    CHECK("channel move: UP/DOWN = position, LEFT/RIGHT = trigger column",
          is(key_map(&cm, BTN_UP, false),    KA_CHPOS_STEP, +1) &&
          is(key_map(&cm, BTN_DOWN, false),  KA_CHPOS_STEP, -1) &&
          is(key_map(&cm, BTN_LEFT, false),  KA_TRIGCOL_STEP, -1) &&
          is(key_map(&cm, BTN_RIGHT, false), KA_TRIGCOL_STEP, +1));
    key_ctx_t tm = ctx(MODE_OSCILLOSCOPE, KF_TRIGMOVE);
    CHECK("trigger move: UP/DOWN = trigger level, LEFT/RIGHT = trigger column",
          is(key_map(&tm, BTN_UP, false),    KA_TRIGLVL_STEP, +1) &&
          is(key_map(&tm, BTN_DOWN, false),  KA_TRIGLVL_STEP, -1) &&
          is(key_map(&tm, BTN_LEFT, false),  KA_TRIGCOL_STEP, -1) &&
          is(key_map(&tm, BTN_RIGHT, false), KA_TRIGCOL_STEP, +1));
    key_ctx_t cu = ctx(MODE_OSCILLOSCOPE, KF_CURSOR);
    CHECK("cursor: UP/DOWN move, LEFT/RIGHT pick",
          is(key_map(&cu, BTN_UP, false),   KA_CURSOR_MOVE, -1) &&
          is(key_map(&cu, BTN_DOWN, false), KA_CURSOR_MOVE, +1) &&
          key_map(&cu, BTN_LEFT, false).act == KA_CURSOR_NEXT &&
          key_map(&cu, BTN_RIGHT, false).act == KA_CURSOR_NEXT);
    key_ctx_t mk = ctx(MODE_OSCILLOSCOPE, KF_MASK); mk.mask_present = true;
    CHECK("mask stage: tolerances, and MOVE = stop-on-fail (no channel to switch)",
          is(key_map(&mk, BTN_UP, false),    KA_MASK_TOL_V, +1) &&
          is(key_map(&mk, BTN_RIGHT, false), KA_MASK_TOL_H, +1) &&
          key_map(&mk, BTN_MOVE, false).act == KA_MASK_STOPFAIL &&
          key_map(&mk, BTN_SELECT, false).act == KA_FOCUS_NEXT);
    key_ctx_t mg = mk; mg.mask_present = false;
    CHECK("mask stage without a mask: MOVE is the channel switch again",
          key_map(&mg, BTN_MOVE, false).act == KA_ADJ_CH_SWITCH);

    /* Stock manual m.p30: MOVE = "Switch CH1/CH2 Adjustment", SELECT =
     * "Select Function for Directional Keys" (UI round 1 swapped them). */
    int sw = 1;
    for (int f = KF_ZOOM; f <= KF_CURSOR; f++) {
        key_ctx_t c = ctx(MODE_OSCILLOSCOPE, (key_focus_t)f); c.cursors_on = true;
        sw &= key_map(&c, BTN_MOVE, false).act == KA_ADJ_CH_SWITCH;
        sw &= key_map(&c, BTN_SELECT, false).act == KA_FOCUS_NEXT;
    }
    CHECK("MOVE short = switch CH1/CH2 adjustment, in every non-mask stage", sw);
    CHECK("SELECT short = next arrow stage (zoom, cursor ...)", sw &&
          key_map(&z, BTN_SELECT, false).act == KA_FOCUS_NEXT &&
          key_map(&cu, BTN_SELECT, false).act == KA_FOCUS_NEXT);
    CHECK("the focus popup names MOVE, not SEL, as the mask stop key",
          strstr(key_focus_popup(KF_MASK), "MOVE:stop") != NULL &&
          strstr(key_focus_popup(KF_MASK), "SEL") == NULL);

    printf("short-press map (FFT views, other modes)\n");
    key_ctx_t fv = z; fv.scope_time_view = false;
    CHECK("FFT view: arrows and SELECT (window) stay legacy; MOVE switches the channel",
          key_map(&fv, BTN_UP, false).act == KA_LEGACY &&
          key_map(&fv, BTN_LEFT, false).act == KA_LEGACY &&
          key_map(&fv, BTN_SELECT, false).act == KA_LEGACY &&
          key_map(&fv, BTN_MOVE, false).act == KA_ADJ_CH_SWITCH);
    CHECK("FFT view: PRM no longer cycles the views, it opens the page at Measure",
          is(key_map(&fv, BTN_PRM, false), KA_OSCMENU_OPEN, OSC_ROW_PARAM));
    int leg = 1;
    const device_mode_t others[] = { MODE_MULTIMETER, MODE_SIGNAL_GEN, MODE_SETTINGS, MODE_HOME };
    const button_id_t keys[] = { BTN_MOVE, BTN_SELECT, BTN_PRM, BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT,
                                 BTN_TRIGGER, BTN_CH1, BTN_OK, BTN_AUTO, BTN_MENU };
    for (unsigned m = 0; m < sizeof others / sizeof others[0]; m++)
        for (unsigned k = 0; k < sizeof keys / sizeof keys[0]; k++) {
            key_ctx_t c = ctx(others[m], KF_ZOOM);
            c.osc_menu_open = true;   /* a stale flag outside the scope changes nothing */
            leg &= key_map(&c, keys[k], false).act == KA_LEGACY;
        }
    CHECK("meter / siggen / settings / home: every short press but SAVE is legacy", leg);
    CHECK("scope CH1/OK/AUTO short stay legacy",
          key_map(&z, BTN_CH1, false).act == KA_LEGACY &&
          key_map(&z, BTN_OK, false).act == KA_LEGACY &&
          key_map(&z, BTN_AUTO, false).act == KA_LEGACY);
}

static void test_save(void)
{
    printf("SAVE everywhere (owner decision D1)\n");
    const device_mode_t modes[] = { MODE_OSCILLOSCOPE, MODE_MULTIMETER, MODE_SIGNAL_GEN, MODE_SETTINGS,
                                    MODE_HOME };
    int ok = 1;
    for (unsigned i = 0; i < sizeof modes / sizeof modes[0]; i++) {
        key_ctx_t c = ctx(modes[i], KF_ZOOM);
        ok &= is(key_map(&c, BTN_SAVE, false), KA_SCREENSHOT, 0);
    }
    CHECK("short SAVE = screenshot in every mode (the meter's overlay is the shell's now)", ok);
    key_ctx_t fv = ctx(MODE_OSCILLOSCOPE, KF_ZOOM); fv.scope_time_view = false;
    key_ctx_t tp = ctx(MODE_OSCILLOSCOPE, KF_ZOOM); tp.trig_menu_open = true;
    key_ctx_t pg = ctx(MODE_OSCILLOSCOPE, KF_ZOOM); pg.osc_menu_open = true; pg.osc_menu_opener = BTN_MENU;
    CHECK("short SAVE = screenshot in the FFT views, over the trigger panel and on the page",
          is(key_map(&fv, BTN_SAVE, false), KA_SCREENSHOT, 0) &&
          is(key_map(&tp, BTN_SAVE, false), KA_SCREENSHOT, 0) &&
          is(key_map(&pg, BTN_SAVE, false), KA_SCREENSHOT, 0));
}

static void test_osc_menu(void)
{
    printf("Oscilloscope Settings page (UI round 1)\n");
    key_ctx_t z = ctx(MODE_OSCILLOSCOPE, KF_ZOOM);
    CHECK("scope short MENU opens the page at Display (stock BACK)",
          is(key_map(&z, BTN_MENU, false), KA_OSCMENU_OPEN, OSC_ROW_DISPLAY));
    CHECK("scope short PRM opens the page at Parameter Measurement",
          is(key_map(&z, BTN_PRM, false), KA_OSCMENU_OPEN, OSC_ROW_PARAM));
    key_ctx_t fv = z; fv.scope_time_view = false;
    CHECK("FFT views: MENU opens the page too (the way back to the time view)",
          is(key_map(&fv, BTN_MENU, false), KA_OSCMENU_OPEN, OSC_ROW_DISPLAY));
    CHECK("TRIGGER short still opens the inline trigger panel (D3)",
          is(key_map(&z, BTN_TRIGGER, false), KA_TRIGMENU_OPEN, 0));
    key_ctx_t me = ctx(MODE_MULTIMETER, KF_ZOOM);
    CHECK("meter short MENU / PRM are not the page (home peek, softkey: D2)",
          key_map(&me, BTN_MENU, false).act == KA_LEGACY &&
          key_map(&me, BTN_PRM, false).act == KA_LEGACY);

    key_ctx_t o = z; o.osc_menu_open = true; o.osc_menu_opener = BTN_PRM;
    o.focus = KF_CURSOR; o.cursors_on = true;   /* the page wins over any stage */
    CHECK("open: arrows and OK go to the page",
          is(key_map(&o, BTN_UP, false),    KA_OSCMENU_KEY, OSC_KEY_UP) &&
          is(key_map(&o, BTN_DOWN, false),  KA_OSCMENU_KEY, OSC_KEY_DOWN) &&
          is(key_map(&o, BTN_LEFT, false),  KA_OSCMENU_KEY, OSC_KEY_LEFT) &&
          is(key_map(&o, BTN_RIGHT, false), KA_OSCMENU_KEY, OSC_KEY_RIGHT) &&
          is(key_map(&o, BTN_OK, false),    KA_OSCMENU_KEY, OSC_KEY_OK));
    CHECK("open: MENU is BACK on the page (up a level, or close)",
          is(key_map(&o, BTN_MENU, false), KA_OSCMENU_KEY, OSC_KEY_BACK));
    CHECK("open from PRM: PRM again closes it",
          is(key_map(&o, BTN_PRM, false), KA_OSCMENU_CLOSE, 0));
    key_ctx_t om = o; om.osc_menu_opener = BTN_MENU;
    CHECK("open from MENU: PRM jumps to Measure instead",
          is(key_map(&om, BTN_PRM, false), KA_OSCMENU_OPEN, OSC_ROW_PARAM));
    CHECK("open: TRIGGER / CH1 / CH2 jump to their rows (ours) ...",
          is(key_map(&om, BTN_TRIGGER, false), KA_OSCMENU_OPEN, OSC_ROW_TRIGGER) &&
          is(key_map(&om, BTN_CH1, false), KA_OSCMENU_OPEN, OSC_ROW_CH1) &&
          is(key_map(&om, BTN_CH2, false), KA_OSCMENU_OPEN, OSC_ROW_CH2));
    key_ctx_t ot = o; ot.osc_menu_opener = BTN_TRIGGER;
    key_ctx_t o1 = o; o1.osc_menu_opener = BTN_CH1;
    CHECK("... and then, being the opener, close it",
          is(key_map(&ot, BTN_TRIGGER, false), KA_OSCMENU_CLOSE, 0) &&
          is(key_map(&o1, BTN_CH1, false), KA_OSCMENU_CLOSE, 0) &&
          is(key_map(&o1, BTN_CH2, false), KA_OSCMENU_OPEN, OSC_ROW_CH2));
    CHECK("open: MOVE / SELECT / AUTO do nothing (their plot is hidden)",
          is(key_map(&o, BTN_MOVE, false), KA_NONE, 0) &&
          is(key_map(&o, BTN_SELECT, false), KA_NONE, 0) &&
          is(key_map(&o, BTN_AUTO, false), KA_NONE, 0));
    CHECK("open: POWER stays with its own handler",
          key_map(&o, BTN_POWER, false).act == KA_LEGACY);
    key_ctx_t ofv = o; ofv.scope_time_view = false;
    CHECK("open over an FFT view: the page still owns the arrows",
          is(key_map(&ofv, BTN_LEFT, false), KA_OSCMENU_KEY, OSC_KEY_LEFT));

    CHECK("open: long MENU/MOVE/SELECT/TRIGGER keep their global meaning",
          is(key_map(&o, BTN_MENU, true), KA_ENTER_HOME, 0) &&
          is(key_map(&o, BTN_MOVE, true), KA_ENTER_METER, 0) &&
          is(key_map(&o, BTN_SELECT, true), KA_ENTER_SCOPE, 0) &&
          is(key_map(&o, BTN_TRIGGER, true), KA_ENTER_SIGGEN, 0));
    CHECK("open: the scope's own long presses are ignored",
          is(key_map(&o, BTN_PRM, true), KA_NONE, 0) &&
          is(key_map(&o, BTN_CH1, true), KA_NONE, 0) &&
          is(key_map(&o, BTN_CH2, true), KA_NONE, 0) &&
          is(key_map(&o, BTN_AUTO, true), KA_NONE, 0) &&
          is(key_map(&o, BTN_OK, true), KA_NONE, 0));
    key_ctx_t sg = ctx(MODE_SIGNAL_GEN, KF_ZOOM); sg.osc_menu_open = true;
    CHECK("a stale page flag does not reach the generator's long OK / AUTO",
          is(key_map(&sg, BTN_OK, true), KA_SIGGEN_RUNSTOP, 0) &&
          is(key_map(&sg, BTN_AUTO, true), KA_SIGGEN_BOOT_TOGGLE, 0));
}

static void test_trig_menu(void)
{
    printf("trigger panel (C3)\n");
    key_ctx_t z = ctx(MODE_OSCILLOSCOPE, KF_ZOOM);
    CHECK("time view: TRIGGER short opens the trigger panel (stock Trigger Settings)",
          is(key_map(&z, BTN_TRIGGER, false), KA_TRIGMENU_OPEN, 0));
    key_ctx_t fv = z; fv.scope_time_view = false;
    CHECK("FFT views: TRIGGER short stays legacy", key_map(&fv, BTN_TRIGGER, false).act == KA_LEGACY);
    CHECK("TRIGGER long is still the signal generator", is(key_map(&z, BTN_TRIGGER, true), KA_ENTER_SIGGEN, 0));

    key_ctx_t o = z; o.trig_menu_open = true;
    o.focus = KF_CURSOR; o.cursors_on = true;     /* the panel wins over any stage */
    CHECK("open: UP/DOWN pick the row, LEFT/RIGHT/OK change it",
          is(key_map(&o, BTN_UP, false), KA_TRIGMENU_ROW, -1) &&
          is(key_map(&o, BTN_DOWN, false), KA_TRIGMENU_ROW, +1) &&
          is(key_map(&o, BTN_LEFT, false), KA_TRIGMENU_CHANGE, -1) &&
          is(key_map(&o, BTN_RIGHT, false), KA_TRIGMENU_CHANGE, +1) &&
          is(key_map(&o, BTN_OK, false), KA_TRIGMENU_CHANGE, +1));
    CHECK("open: TRIGGER/MENU/MOVE/SELECT/PRM close it",
          is(key_map(&o, BTN_TRIGGER, false), KA_TRIGMENU_CLOSE, 0) &&
          is(key_map(&o, BTN_MENU, false), KA_TRIGMENU_CLOSE, 0) &&
          is(key_map(&o, BTN_MOVE, false), KA_TRIGMENU_CLOSE, 0) &&
          is(key_map(&o, BTN_SELECT, false), KA_TRIGMENU_CLOSE, 0) &&
          is(key_map(&o, BTN_PRM, false), KA_TRIGMENU_CLOSE, 0));
    CHECK("open: CH1/AUTO keep their meaning; long presses still switch modes",
          key_map(&o, BTN_CH1, false).act == KA_LEGACY &&
          key_map(&o, BTN_AUTO, false).act == KA_LEGACY &&
          is(key_map(&o, BTN_MOVE, true), KA_ENTER_METER, 0));

    scope_state_t s; scope_state_init(&s);
    trig_menu_t m; trig_menu_open(&m);
    int a = m.open && m.row == TM_ROW_MODE && strcmp(trig_menu_row_label(m.row), "Mode:") == 0;
    trig_menu_row_step(&m, -1);
    int b = m.row == TM_ROW_SOURCE;
    trig_menu_row_step(&m, +1);
    CHECK("opens on Mode:, rows wrap", a && b && m.row == TM_ROW_MODE);

    uint8_t r = trig_menu_change(&m, &s, +1);
    int c1 = (r == TM_CHANGED_MODE) && s.trigger.mode == TRIG_NORMAL &&
             strcmp(trig_menu_row_value(&s, TM_ROW_MODE), "Normal") == 0;
    trig_menu_change(&m, &s, +1);
    int c2 = s.trigger.mode == TRIG_SINGLE;
    trig_menu_change(&m, &s, +1);
    int c3 = s.trigger.mode == TRIG_AUTO;
    trig_menu_change(&m, &s, -1);
    CHECK("Mode: Auto -> Normal -> Single -> Auto, and back", c1 && c2 && c3 && s.trigger.mode == TRIG_SINGLE);

    m.row = TM_ROW_EDGE;
    r = trig_menu_change(&m, &s, +1);
    CHECK("Edge: Rising <-> Falling", r == TM_CHANGED_EDGE && s.trigger.edge == TRIG_FALLING &&
          strcmp(trig_menu_row_value(&s, TM_ROW_EDGE), "Falling") == 0);

    m.row = TM_ROW_SOURCE;
    r = trig_menu_change(&m, &s, +1);
    int d1 = (r == TM_CHANGED_SOURCE) && s.trigger.source == TRIG_SRC_CH2 &&
             strcmp(trig_menu_row_value(&s, TM_ROW_SOURCE), "CH2") == 0 &&
             strcmp(trig_menu_row_label(TM_ROW_SOURCE), "Channel:") == 0;
    trig_menu_change(&m, &s, +1);
    s.ch2.enabled = false;
    r = trig_menu_change(&m, &s, +1);
    CHECK("Channel: CH1 <-> CH2; refused onto a channel that is off",
          d1 && r == TM_REFUSED_OFF && s.trigger.source == TRIG_SRC_CH1);
    trig_menu_close(&m);
    CHECK("closes", !m.open);
}

static void test_focus(void)
{
    printf("arrow stage cycle\n");
    key_focus_t f = KF_ZOOM;
    f = key_focus_next(f, false, false); int a = f == KF_CHMOVE;
    f = key_focus_next(f, false, false); int b = f == KF_TRIGMOVE;
    f = key_focus_next(f, false, false); int c = f == KF_ZOOM;
    CHECK("no cursors, no mask: zoom -> channel move -> trigger move -> zoom", a && b && c);

    f = KF_TRIGMOVE;
    f = key_focus_next(f, false, true); a = f == KF_MASK;
    f = key_focus_next(f, false, true); b = f == KF_ZOOM;
    CHECK("mask present: trigger move -> mask -> zoom", a && b);

    f = key_focus_next(KF_TRIGMOVE, true, true); a = f == KF_CURSOR;
    f = key_focus_next(f, true, true);           b = f == KF_MASK;
    CHECK("cursors and mask: ... trigger move -> cursor -> mask", a && b);

    CHECK("cursor stage drops when cursors go off, mask stage when the mask goes",
          key_focus_sanitize(KF_CURSOR, false, false) == KF_ZOOM &&
          key_focus_sanitize(KF_MASK, true, false) == KF_ZOOM &&
          key_focus_sanitize(KF_CURSOR, true, false) == KF_CURSOR &&
          key_focus_sanitize((key_focus_t)99, true, true) == KF_ZOOM);
    CHECK("MOVE from a vanished mask stage restarts at channel move, not mask",
          key_focus_next(KF_MASK, false, false) == KF_CHMOVE);

    CHECK("cursors switched on select the cursor stage; off returns to zoom",
          key_focus_after_cursor_change(KF_ZOOM, true) == KF_CURSOR &&
          key_focus_after_cursor_change(KF_CURSOR, false) == KF_ZOOM &&
          key_focus_after_cursor_change(KF_TRIGMOVE, false) == KF_TRIGMOVE);

    /* Every stage is reachable and the walk is a cycle. */
    int seen[KF_COUNT] = {0}; f = KF_ZOOM;
    for (int i = 0; i < 10; i++) { seen[f] = 1; f = key_focus_next(f, true, true); }
    CHECK("with everything on, all five stages are visited and it wraps",
          seen[KF_ZOOM] && seen[KF_CHMOVE] && seen[KF_TRIGMOVE] && seen[KF_CURSOR] && seen[KF_MASK] &&
          f == KF_ZOOM);
}

static void test_state(void)
{
    printf("scope state helpers\n");
    channel_state_t c; memset(&c, 0, sizeof c);
    scope_adjust_position(&c, +1);
    int a = c.position == SCOPE_POSITION_STEP;
    for (int i = 0; i < 100; i++) scope_adjust_position(&c, +1);
    int b = c.position == SCOPE_POSITION_MAX;
    for (int i = 0; i < 100; i++) scope_adjust_position(&c, -1);
    int d = c.position == -SCOPE_POSITION_MAX;
    CHECK("position steps by 5 and clamps at +-100", a && b && d);

    scope_state_t s; scope_state_init(&s);
    char buf[12];
    scope_trigger_summary(&s, buf, sizeof buf);
    int e = strcmp(buf, "Auto R C1") == 0;
    s.trigger.mode = TRIG_NORMAL; s.trigger.edge = TRIG_FALLING; s.trigger.source = TRIG_SRC_CH2;
    scope_trigger_summary(&s, buf, sizeof buf);
    int f = strcmp(buf, "Norm F C2") == 0;
    s.trigger.mode = TRIG_SINGLE;
    scope_trigger_summary(&s, buf, sizeof buf);
    int g = strcmp(buf, "Sngl F C2") == 0;
    char tiny[4];
    scope_trigger_summary(&s, tiny, sizeof tiny);
    int h = strlen(tiny) == 3;
    CHECK("trigger summary: Auto R C1 / Norm F C2 / Sngl F C2, truncates safely", e && f && g && h);
    CHECK("measurements start shown", s.prm_hidden == false);
}

int main(void)
{
    test_scan();
    test_long_map();
    test_short_map();
    test_trig_menu();
    test_save();
    test_osc_menu();
    test_focus();
    test_state();
    printf("\n%d/%d passed\n", passed, run);
    return passed == run ? 0 : 1;
}
