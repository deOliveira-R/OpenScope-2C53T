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
#include "scope_state_pre_b7.h"

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
static key_ctx_t vctx(device_mode_t m, uint8_t view, key_focus_t f)
{
    key_ctx_t c;
    memset(&c, 0, sizeof c);
    c.mode = m; c.view = view; c.focus = f;
    return c;
}
/* Y-T, the view a scope boots in. */
static key_ctx_t ctx(device_mode_t m, key_focus_t f) { return vctx(m, VIEW_MODE_YT, f); }

static int is(key_cmd_t k, key_action_t a, int arg)
{
    return k.act == a && k.arg == arg;
}

/* What each view is, written out here (the expectations, not view_table[]):
 * the jobs short SELECT steps through, in order; whether it draws the
 * measurement strip (long PRM); whether it shows the time record (long OK,
 * long AUTO). */
static const struct {
    uint8_t view; uint8_t n; key_focus_t ring[KF_COUNT]; bool strip, time;
} k_views[] = {
    { VIEW_MODE_YT,        5, { KF_ZOOM, KF_CHMOVE, KF_TRIGMOVE, KF_CURSOR, KF_MASK }, true,  true  },
    { VIEW_MODE_ROLL,      5, { KF_ZOOM, KF_CHMOVE, KF_TRIGMOVE, KF_CURSOR, KF_MASK }, true,  true  },
    { VIEW_MODE_XY,        3, { KF_ZOOM, KF_CHMOVE, KF_TRIGMOVE },                     false, true  },
    { VIEW_MODE_FFT,       3, { KF_SPECTRUM, KF_ZOOM, KF_SPECTRUM_CURSOR },            true,  false },
    { VIEW_MODE_SPLIT,     4, { KF_ZOOM, KF_CHMOVE, KF_TRIGMOVE, KF_SPECTRUM },        false, true  },
    { VIEW_MODE_WATERFALL, 2, { KF_SPECTRUM, KF_ZOOM },                                false, false },
};
#define N_VIEWS (sizeof k_views / sizeof k_views[0])

/* Short AUTO: the spectrum's fit on the spectrum, the autoset on the time
 * record; split goes by the half the job is on. */
static key_action_t want_auto(uint8_t view, key_focus_t f)
{
    switch (view) {
    case VIEW_MODE_FFT:
    case VIEW_MODE_WATERFALL: return KA_FFT_AUTO;
    case VIEW_MODE_SPLIT:     return (f == KF_SPECTRUM || f == KF_SPECTRUM_CURSOR) ? KA_FFT_AUTO : KA_AUTOSET;
    default:                  return KA_AUTOSET;
    }
}

/* The arrows of each job (key_model.h), in every view alike. */
static const struct { key_focus_t f; key_cmd_t up, down, left, right; } k_arrows[KF_COUNT] = {
    { KF_ZOOM,     { KA_VDIV_STEP, +1 },   { KA_VDIV_STEP, -1 },   { KA_TB_STEP, -1 },      { KA_TB_STEP, +1 } },
    { KF_CHMOVE,   { KA_CHPOS_STEP, +1 },  { KA_CHPOS_STEP, -1 },  { KA_TRIGCOL_STEP, -1 }, { KA_TRIGCOL_STEP, +1 } },
    { KF_TRIGMOVE, { KA_TRIGLVL_STEP, +1 },{ KA_TRIGLVL_STEP, -1 },{ KA_TRIGCOL_STEP, -1 }, { KA_TRIGCOL_STEP, +1 } },
    { KF_CURSOR,   { KA_CURSOR_MOVE, -1 }, { KA_CURSOR_MOVE, +1 }, { KA_CURSOR_NEXT, 0 },   { KA_CURSOR_NEXT, 0 } },
    { KF_MASK,     { KA_MASK_TOL_V, +1 },  { KA_MASK_TOL_V, -1 },  { KA_MASK_TOL_H, -1 },   { KA_MASK_TOL_H, +1 } },
    { KF_SPECTRUM, { KA_FFT_REF, +1 },     { KA_FFT_REF, -1 },     { KA_FFT_ZOOM, -1 },     { KA_FFT_ZOOM, +1 } },
    { KF_SPECTRUM_CURSOR, { KA_SPECTRUM_CURSOR_MOVE, -1 }, { KA_SPECTRUM_CURSOR_MOVE, +1 },
                          { KA_CURSOR_NEXT, 0 },           { KA_CURSOR_NEXT, 0 } },
};

static int same(key_cmd_t a, key_cmd_t b) { return a.act == b.act && a.arg == b.arg; }

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
    CHECK("long PRM in the scope Y-T view = hide measurements",
          is(key_map(&sc, BTN_PRM, true), KA_MEAS_HIDE_TOGGLE, 0));
    key_ctx_t me = ctx(MODE_MULTIMETER, KF_ZOOM);
    CHECK("long PRM in the meter does nothing", is(key_map(&me, BTN_PRM, true), KA_NONE, 0));
    key_ctx_t fv = sc; fv.view = VIEW_MODE_FFT;
    CHECK("long PRM in the FFT view hides its measurement strip",
          is(key_map(&fv, BTN_PRM, true), KA_MEAS_HIDE_TOGGLE, 0));
    key_ctx_t wv = sc; wv.view = VIEW_MODE_WATERFALL;
    CHECK("long PRM in the waterfall does nothing (no strip)", is(key_map(&wv, BTN_PRM, true), KA_NONE, 0));
    {
        /* Long PRM where a strip is drawn; long OK / AUTO where the time
         * record is shown; nothing elsewhere. Every view, every job. */
        int ok = 1;
        for (unsigned v = 0; v < N_VIEWS; v++)
            for (int f = KF_ZOOM; f < KF_COUNT; f++) {
                key_ctx_t c = vctx(MODE_OSCILLOSCOPE, k_views[v].view, (key_focus_t)f);
                c.cursors_on = true; c.mask_present = true;
                const bool t = k_views[v].time;
                ok &= is(key_map(&c, BTN_PRM, true), k_views[v].strip ? KA_MEAS_HIDE_TOGGLE : KA_NONE, 0);
                ok &= is(key_map(&c, BTN_OK, true),  t ? KA_CENTER50 : KA_NONE, 0);
                ok &= is(key_map(&c, BTN_AUTO, true), t ? KA_BASECAL : KA_NONE, 0);
                ok &= is(key_map(&c, BTN_CH1, true), KA_CH_ENABLE_TOGGLE, 0) &&
                      is(key_map(&c, BTN_CH2, true), KA_CH_ENABLE_TOGGLE, 1);
            }
        CHECK("long PRM: Y-T, roll and FFT (the strip); long OK / AUTO: Y-T, roll, X-Y, split; long CH1/CH2 everywhere", ok);
    }
    key_ctx_t xy = vctx(MODE_OSCILLOSCOPE, VIEW_MODE_XY, KF_ZOOM);
    CHECK("X-Y is a view of the time record: long OK = 50 %, long AUTO = baseline, long PRM nothing (no strip)",
          is(key_map(&xy, BTN_OK, true), KA_CENTER50, 0) && is(key_map(&xy, BTN_AUTO, true), KA_BASECAL, 0) &&
          is(key_map(&xy, BTN_PRM, true), KA_NONE, 0));

    CHECK("long CH1 / CH2 in the scope = channel enable toggle",
          is(key_map(&sc, BTN_CH1, true), KA_CH_ENABLE_TOGGLE, 0) &&
          is(key_map(&sc, BTN_CH2, true), KA_CH_ENABLE_TOGGLE, 1));
    CHECK("long CH1 outside the scope does nothing", is(key_map(&me, BTN_CH1, true), KA_NONE, 0));

    CHECK("long AUTO in the scope Y-T view = baseline correction (stock 0x0800DF58)",
          is(key_map(&sc, BTN_AUTO, true), KA_BASECAL, 0));
    CHECK("long OK in the scope Y-T view = 50 % centre (stock 0x0800DA3A)",
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
    printf("short-press map (scope Y-T view)\n");
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

    printf("short-press map (every view, other modes)\n");
    key_ctx_t fv = vctx(MODE_OSCILLOSCOPE, VIEW_MODE_FFT, KF_SPECTRUM);
    CHECK("FFT view, spectrum job: UP/DOWN = reference level, LEFT = zoom in, RIGHT = zoom out",
          is(key_map(&fv, BTN_UP, false), KA_FFT_REF, +1) &&
          is(key_map(&fv, BTN_DOWN, false), KA_FFT_REF, -1) &&
          is(key_map(&fv, BTN_LEFT, false), KA_FFT_ZOOM, -1) &&
          is(key_map(&fv, BTN_RIGHT, false), KA_FFT_ZOOM, +1));
    key_ctx_t fz = fv; fz.focus = KF_ZOOM;
    CHECK("FFT view, zoom job: UP/DOWN = V/div, LEFT/RIGHT = timebase (the input's scales)",
          is(key_map(&fz, BTN_UP, false), KA_VDIV_STEP, +1) &&
          is(key_map(&fz, BTN_DOWN, false), KA_VDIV_STEP, -1) &&
          is(key_map(&fz, BTN_LEFT, false), KA_TB_STEP, -1) &&
          is(key_map(&fz, BTN_RIGHT, false), KA_TB_STEP, +1));
    CHECK("FFT view: SELECT = next job, AUTO = fit the span in both jobs; MOVE switches the channel",
          is(key_map(&fv, BTN_SELECT, false), KA_FOCUS_NEXT, 0) &&
          is(key_map(&fz, BTN_SELECT, false), KA_FOCUS_NEXT, 0) &&
          is(key_map(&fv, BTN_AUTO, false), KA_FFT_AUTO, 0) &&
          is(key_map(&fz, BTN_AUTO, false), KA_FFT_AUTO, 0) &&
          key_map(&fv, BTN_MOVE, false).act == KA_ADJ_CH_SWITCH);
    {
        /* Every view and job: the arrows are the job's, SELECT the next job,
         * AUTO by the domain, OK and CH1/CH2 as in Y-T. */
        int arrows = 1, rest = 1, aut = 1;
        for (unsigned v = 0; v < N_VIEWS; v++)
            for (int f = KF_ZOOM; f < KF_COUNT; f++) {
                key_ctx_t c = vctx(MODE_OSCILLOSCOPE, k_views[v].view, (key_focus_t)f);
                c.cursors_on = true; c.mask_present = true;
                arrows &= k_arrows[f].f == (key_focus_t)f &&
                          same(key_map(&c, BTN_UP, false),    k_arrows[f].up) &&
                          same(key_map(&c, BTN_DOWN, false),  k_arrows[f].down) &&
                          same(key_map(&c, BTN_LEFT, false),  k_arrows[f].left) &&
                          same(key_map(&c, BTN_RIGHT, false), k_arrows[f].right);
                rest &= is(key_map(&c, BTN_SELECT, false), KA_FOCUS_NEXT, 0) &&
                        is(key_map(&c, BTN_OK, false), KA_RUNSTOP, 0) &&
                        is(key_map(&c, BTN_CH1, false), KA_COUPLING_TOGGLE, 0) &&
                        is(key_map(&c, BTN_CH2, false), KA_COUPLING_TOGGLE, 1);
                aut &= is(key_map(&c, BTN_AUTO, false), want_auto(k_views[v].view, (key_focus_t)f), 0);
            }
        CHECK("every view: the arrows go by the job (zoom, chmove, trigmove, cursor, mask, spectrum)", arrows);
        CHECK("every view: SELECT = next job, OK = RUN/STOP, CH1/CH2 = coupling", rest);
        CHECK("AUTO: autoset in Y-T, roll, X-Y; fit the span in FFT and waterfall; split by the job", aut);
    }
    {
        key_ctx_t sp = vctx(MODE_OSCILLOSCOPE, VIEW_MODE_SPLIT, KF_SPECTRUM);
        key_ctx_t st = sp; st.focus = KF_TRIGMOVE;
        key_ctx_t x = vctx(MODE_OSCILLOSCOPE, VIEW_MODE_XY, KF_ZOOM);
        CHECK("split: AUTO fits the span in the spectrum job, autosets in a time job",
              is(key_map(&sp, BTN_AUTO, false), KA_FFT_AUTO, 0) && is(key_map(&st, BTN_AUTO, false), KA_AUTOSET, 0));
        CHECK("X-Y: AUTO is the autoset, the arrows scale V/div and time as in Y-T",
              is(key_map(&x, BTN_AUTO, false), KA_AUTOSET, 0) && is(key_map(&x, BTN_UP, false), KA_VDIV_STEP, +1) &&
              is(key_map(&x, BTN_RIGHT, false), KA_TB_STEP, +1));
    }
    {
        /* The mask job's MOVE asks only for the job and a mask. */
        int sf = 1;
        for (unsigned v = 0; v < N_VIEWS; v++) {
            key_ctx_t c = vctx(MODE_OSCILLOSCOPE, k_views[v].view, KF_MASK);
            c.mask_present = true;
            sf &= is(key_map(&c, BTN_MOVE, false), KA_MASK_STOPFAIL, 0);
            c.mask_present = false;
            sf &= is(key_map(&c, BTN_MOVE, false), KA_ADJ_CH_SWITCH, 0);
        }
        CHECK("MOVE in the mask job with a mask = stop-on-fail in every view; without one the channel switch", sf);
    }
    CHECK("FFT view: PRM no longer cycles the views, it opens the Measure pane",
          is(key_map(&fv, BTN_PRM, false), KA_OSCMENU_PANE, OSC_ROW_PARAM));
    CHECK("FFT view: TRIGGER opens the Trigger pane (no more trigger-mode cycle)",
          is(key_map(&fv, BTN_TRIGGER, false), KA_OSCMENU_PANE, OSC_ROW_TRIGGER));
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
    {
        int ok = 1;
        for (int f = KF_ZOOM; f < KF_COUNT; f++) {
            key_ctx_t c = ctx(MODE_OSCILLOSCOPE, (key_focus_t)f); c.cursors_on = true; c.mask_present = true;
            ok &= is(key_map(&c, BTN_CH1, false), KA_COUPLING_TOGGLE, 0) &&
                  is(key_map(&c, BTN_CH2, false), KA_COUPLING_TOGGLE, 1) &&
                  is(key_map(&c, BTN_OK, false), KA_RUNSTOP, 0) &&
                  is(key_map(&c, BTN_AUTO, false), KA_AUTOSET, 0);
        }
        CHECK("scope short CH1/CH2 = coupling of that channel, OK = RUN/STOP, AUTO = autoset: every stage", ok);
    }
    {
        /* B7: the scope's table is complete. With the page closed every key
         * but POWER has an action of its own in every view and stage. */
        static const button_id_t all[] = { BTN_CH1, BTN_CH2, BTN_MOVE, BTN_SELECT, BTN_TRIGGER, BTN_PRM,
                                           BTN_AUTO, BTN_SAVE, BTN_MENU, BTN_UP, BTN_DOWN, BTN_LEFT,
                                           BTN_RIGHT, BTN_OK };
        int claimed = 1, n = 0;
        for (unsigned v = 0; v < N_VIEWS; v++)
            for (int f = KF_ZOOM; f < KF_COUNT; f++)
                for (int cur = 0; cur < 2; cur++)
                    for (int mk = 0; mk < 2; mk++)
                        for (unsigned k = 0; k < sizeof all / sizeof all[0]; k++)
                            for (int lng = 0; lng < 2; lng++) {
                                const uint8_t view = k_views[v].view;
                                key_ctx_t c = vctx(MODE_OSCILLOSCOPE, view,
                                                   key_focus_sanitize(view, (key_focus_t)f, cur, mk));
                                c.cursors_on = cur; c.mask_present = mk;
                                claimed &= key_map(&c, all[k], lng).act != KA_LEGACY;
                                n++;
                            }
        CHECK("scope, page closed: no key but POWER is KA_LEGACY (every view x job x 14 keys x short/long)",
              claimed && n == (int)(N_VIEWS * KF_COUNT * 2 * 2 * (sizeof all / sizeof all[0]) * 2) &&
              N_VIEWS == VIEW_MODE_COUNT);
        CHECK("scope POWER short stays with the router's global",
              key_map(&z, BTN_POWER, false).act == KA_LEGACY && key_map(&fv, BTN_POWER, false).act == KA_LEGACY);
    }
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
    key_ctx_t fv = vctx(MODE_OSCILLOSCOPE, VIEW_MODE_FFT, KF_SPECTRUM);
    key_ctx_t tp = ctx(MODE_OSCILLOSCOPE, KF_ZOOM);
    tp.osc_menu_open = true; tp.osc_menu_locked = true; tp.osc_menu_opener = BTN_TRIGGER;
    key_ctx_t pg = ctx(MODE_OSCILLOSCOPE, KF_ZOOM); pg.osc_menu_open = true; pg.osc_menu_opener = BTN_MENU;
    CHECK("short SAVE = screenshot in the FFT views, in a locked pane and on the page",
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
    CHECK("scope short PRM opens the page locked inside Parameter Measurement",
          is(key_map(&z, BTN_PRM, false), KA_OSCMENU_PANE, OSC_ROW_PARAM));
    key_ctx_t fv = z; fv.view = VIEW_MODE_FFT; fv.focus = KF_SPECTRUM;
    CHECK("FFT views: MENU opens the page too (the way back to the time view)",
          is(key_map(&fv, BTN_MENU, false), KA_OSCMENU_OPEN, OSC_ROW_DISPLAY));
    CHECK("scope short TRIGGER opens the page locked inside Trigger Settings (no inline panel)",
          is(key_map(&z, BTN_TRIGGER, false), KA_OSCMENU_PANE, OSC_ROW_TRIGGER));
    key_ctx_t zc = z; zc.focus = KF_CURSOR; zc.cursors_on = true;
    CHECK("... whatever the arrows adjust", is(key_map(&zc, BTN_TRIGGER, false), KA_OSCMENU_PANE, OSC_ROW_TRIGGER) &&
          is(key_map(&zc, BTN_PRM, false), KA_OSCMENU_PANE, OSC_ROW_PARAM));
    key_ctx_t mt = ctx(MODE_MULTIMETER, KF_ZOOM);
    CHECK("TRIGGER outside the scope stays legacy (meter softkey)",
          key_map(&mt, BTN_TRIGGER, false).act == KA_LEGACY);

    /* Locked (stock V1.2.0, owner on unit #2, 2026-11-12). */
    key_ctx_t lp = z; lp.osc_menu_open = true; lp.osc_menu_locked = true; lp.osc_menu_opener = BTN_PRM;
    lp.focus = KF_CURSOR; lp.cursors_on = true;
    CHECK("locked: arrows and OK go to the pane",
          is(key_map(&lp, BTN_UP, false),    KA_OSCMENU_KEY, OSC_KEY_UP) &&
          is(key_map(&lp, BTN_DOWN, false),  KA_OSCMENU_KEY, OSC_KEY_DOWN) &&
          is(key_map(&lp, BTN_LEFT, false),  KA_OSCMENU_KEY, OSC_KEY_LEFT) &&
          is(key_map(&lp, BTN_RIGHT, false), KA_OSCMENU_KEY, OSC_KEY_RIGHT) &&
          is(key_map(&lp, BTN_OK, false),    KA_OSCMENU_KEY, OSC_KEY_OK));
    CHECK("locked: MENU closes the page (not BACK to the rail)",
          is(key_map(&lp, BTN_MENU, false), KA_OSCMENU_CLOSE, 0));
    CHECK("locked from PRM: PRM again closes it",
          is(key_map(&lp, BTN_PRM, false), KA_OSCMENU_CLOSE, 0));
    CHECK("locked from PRM: TRIGGER moves the lock to the Trigger pane",
          is(key_map(&lp, BTN_TRIGGER, false), KA_OSCMENU_PANE, OSC_ROW_TRIGGER));
    key_ctx_t lt = lp; lt.osc_menu_opener = BTN_TRIGGER;
    CHECK("locked from TRIGGER: TRIGGER closes, PRM moves the lock to Measure",
          is(key_map(&lt, BTN_TRIGGER, false), KA_OSCMENU_CLOSE, 0) &&
          is(key_map(&lt, BTN_PRM, false), KA_OSCMENU_PANE, OSC_ROW_PARAM) &&
          is(key_map(&lt, BTN_MENU, false), KA_OSCMENU_CLOSE, 0));
    CHECK("locked: CH1 / CH2 / MOVE / SELECT / AUTO lead nowhere (no other row)",
          is(key_map(&lp, BTN_CH1, false), KA_NONE, 0) &&
          is(key_map(&lp, BTN_CH2, false), KA_NONE, 0) &&
          is(key_map(&lp, BTN_MOVE, false), KA_NONE, 0) &&
          is(key_map(&lp, BTN_SELECT, false), KA_NONE, 0) &&
          is(key_map(&lp, BTN_AUTO, false), KA_NONE, 0));
    CHECK("locked: POWER stays with its own handler; SAVE is a screenshot; long keys global",
          key_map(&lp, BTN_POWER, false).act == KA_LEGACY &&
          is(key_map(&lp, BTN_SAVE, false), KA_SCREENSHOT, 0) &&
          is(key_map(&lp, BTN_MENU, true), KA_ENTER_HOME, 0) &&
          is(key_map(&lp, BTN_PRM, true), KA_NONE, 0));
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
    key_ctx_t ofv = o; ofv.view = VIEW_MODE_FFT; ofv.focus = KF_SPECTRUM;
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

static void test_trigger_key(void)
{
    printf("TRIGGER\n");
    key_ctx_t z = ctx(MODE_OSCILLOSCOPE, KF_ZOOM);
    CHECK("TRIGGER long is still the signal generator", is(key_map(&z, BTN_TRIGGER, true), KA_ENTER_SIGGEN, 0));
    /* (The trigger settings are the Trigger pane's rows on trig.mode / edge /
     * source: make test-osc-menu-screen. trig_menu.c, the model of the inline
     * panel, had no caller left and is gone.) */
}

static void test_focus(void)
{
    printf("arrow stage cycle (Y-T)\n");
    const uint8_t yt = VIEW_MODE_YT;
    key_focus_t f = KF_ZOOM;
    f = key_focus_next(yt, f, false, false); int a = f == KF_CHMOVE;
    f = key_focus_next(yt, f, false, false); int b = f == KF_TRIGMOVE;
    f = key_focus_next(yt, f, false, false); int c = f == KF_ZOOM;
    CHECK("no cursors, no mask: zoom -> channel move -> trigger move -> zoom", a && b && c);

    f = KF_TRIGMOVE;
    f = key_focus_next(yt, f, false, true); a = f == KF_MASK;
    f = key_focus_next(yt, f, false, true); b = f == KF_ZOOM;
    CHECK("mask present: trigger move -> mask -> zoom", a && b);

    f = key_focus_next(yt, KF_TRIGMOVE, true, true); a = f == KF_CURSOR;
    f = key_focus_next(yt, f, true, true);           b = f == KF_MASK;
    CHECK("cursors and mask: ... trigger move -> cursor -> mask", a && b);

    CHECK("cursor stage drops when cursors go off, mask stage when the mask goes",
          key_focus_sanitize(yt, KF_CURSOR, false, false) == KF_ZOOM &&
          key_focus_sanitize(yt, KF_MASK, true, false) == KF_ZOOM &&
          key_focus_sanitize(yt, KF_CURSOR, true, false) == KF_CURSOR &&
          key_focus_sanitize(yt, KF_COUNT, true, true) == KF_ZOOM);
    CHECK("MOVE from a vanished mask stage restarts at channel move, not mask",
          key_focus_next(yt, KF_MASK, false, false) == KF_CHMOVE);

    CHECK("cursors switched on select the cursor stage; off returns to zoom",
          key_focus_after_cursor_change(yt, KF_ZOOM, true) == KF_CURSOR &&
          key_focus_after_cursor_change(yt, KF_CURSOR, false) == KF_ZOOM &&
          key_focus_after_cursor_change(yt, KF_TRIGMOVE, false) == KF_TRIGMOVE);

    printf("the jobs of every view\n");
    {
        /* With cursors on and a mask, SELECT walks exactly the view's ring
         * from its first job and wraps; without them it skips the cursor and
         * mask jobs; a job the view does not offer gives way to its first. */
        int ring = 1, skip = 1, san = 1;
        for (unsigned v = 0; v < N_VIEWS; v++) {
            const uint8_t view = k_views[v].view;
            const key_focus_t *r = k_views[v].ring;
            const unsigned n = k_views[v].n;
            for (unsigned i = 0; i < n; i++)
                ring &= key_focus_next(view, r[i], true, true) == r[(i + 1) % n];
            /* the same ring with the cursor and mask jobs taken out */
            key_focus_t bare[KF_COUNT]; unsigned m = 0;
            for (unsigned i = 0; i < n; i++)
                if (r[i] != KF_CURSOR && r[i] != KF_SPECTRUM_CURSOR && r[i] != KF_MASK) bare[m++] = r[i];
            for (unsigned i = 0; i < m; i++)
                skip &= key_focus_next(view, bare[i], false, false) == bare[(i + 1) % m];
            for (int j = KF_ZOOM; j <= KF_COUNT; j++) {
                bool listed = false;
                for (unsigned i = 0; i < n; i++) listed |= r[i] == (key_focus_t)j;
                san &= key_focus_sanitize(view, (key_focus_t)j, true, true) == (listed ? (key_focus_t)j : r[0]);
            }
            san &= key_focus_sanitize(view, KF_CURSOR, false, false) == r[0] &&
                   key_focus_sanitize(view, KF_SPECTRUM_CURSOR, false, false) == r[0] &&
                   key_focus_sanitize(view, KF_MASK, false, false) == r[0];
        }
        CHECK("every view: SELECT walks its jobs in order and wraps (cursors on, a mask)", ring);
        CHECK("every view: without cursors or a mask those jobs are skipped", skip);
        CHECK("every view: a job it does not offer (or no longer) falls back to its first job", san);
    }
    const uint8_t xy = VIEW_MODE_XY, fft = VIEW_MODE_FFT, sp = VIEW_MODE_SPLIT, wf = VIEW_MODE_WATERFALL;
    CHECK("X-Y: zoom -> channel move -> trigger move -> zoom, cursors and mask not offered",
          key_focus_next(xy, KF_TRIGMOVE, true, true) == KF_ZOOM &&
          key_focus_sanitize(xy, KF_CURSOR, true, true) == KF_ZOOM &&
          key_focus_sanitize(xy, KF_MASK, true, true) == KF_ZOOM &&
          key_focus_sanitize(xy, KF_SPECTRUM, true, true) == KF_ZOOM);
    CHECK("FFT without cursors and waterfall always: spectrum <-> zoom, starting on the spectrum",
          key_focus_next(fft, KF_SPECTRUM, false, false) == KF_ZOOM && key_focus_next(fft, KF_ZOOM, false, false) == KF_SPECTRUM &&
          key_focus_next(wf, KF_SPECTRUM, true, true) == KF_ZOOM && key_focus_next(wf, KF_ZOOM, true, true) == KF_SPECTRUM &&
          key_focus_sanitize(fft, KF_TRIGMOVE, true, true) == KF_SPECTRUM &&
          key_focus_sanitize(wf, KF_CURSOR, true, true) == KF_SPECTRUM);
    CHECK("FFT with cursors: spectrum -> zoom -> spectrum cursor -> spectrum",
          key_focus_next(fft, KF_SPECTRUM, true, false) == KF_ZOOM &&
          key_focus_next(fft, KF_ZOOM, true, false) == KF_SPECTRUM_CURSOR &&
          key_focus_next(fft, KF_SPECTRUM_CURSOR, true, false) == KF_SPECTRUM);
    CHECK("the spectrum cursor job: FFT's only; the time cursor job is not FFT's",
          key_focus_sanitize(fft, KF_SPECTRUM_CURSOR, true, true) == KF_SPECTRUM_CURSOR &&
          key_focus_sanitize(fft, KF_SPECTRUM_CURSOR, false, true) == KF_SPECTRUM &&
          key_focus_sanitize(fft, KF_CURSOR, true, true) == KF_SPECTRUM &&
          key_focus_sanitize(yt, KF_SPECTRUM_CURSOR, true, true) == KF_ZOOM &&
          key_focus_sanitize(wf, KF_SPECTRUM_CURSOR, true, true) == KF_SPECTRUM &&
          key_focus_sanitize(sp, KF_SPECTRUM_CURSOR, true, true) == KF_ZOOM);
    {
        /* Cursors switched on: the view's cursor job (none: the job stays);
         * off: a cursor job gives way to the view's first, another stays. */
        static const struct { uint8_t view; key_focus_t cursor_job, first; } C[] = {
            { VIEW_MODE_YT, KF_CURSOR, KF_ZOOM },             { VIEW_MODE_ROLL, KF_CURSOR, KF_ZOOM },
            { VIEW_MODE_XY, KF_COUNT, KF_ZOOM },              { VIEW_MODE_FFT, KF_SPECTRUM_CURSOR, KF_SPECTRUM },
            { VIEW_MODE_SPLIT, KF_COUNT, KF_ZOOM },           { VIEW_MODE_WATERFALL, KF_COUNT, KF_SPECTRUM },
        };
        int ok = 1;
        for (unsigned i = 0; i < sizeof C / sizeof C[0]; i++) {
            const uint8_t v = C[i].view;
            const key_focus_t on = C[i].cursor_job == KF_COUNT ? KF_TRIGMOVE : C[i].cursor_job;
            ok &= key_focus_after_cursor_change(v, KF_TRIGMOVE, true) == on;
            ok &= key_focus_after_cursor_change(v, KF_CURSOR, false) == C[i].first;
            ok &= key_focus_after_cursor_change(v, KF_SPECTRUM_CURSOR, false) == C[i].first;
            ok &= key_focus_after_cursor_change(v, KF_ZOOM, false) == KF_ZOOM;
            ok &= key_focus_after_cursor_change(v, KF_SPECTRUM, false) == KF_SPECTRUM;
        }
        CHECK("after_cursor_change per view: on -> Y-T/roll cursor, FFT spectrum cursor, else the job stays; "
              "off -> a cursor job goes to the view's first", ok);
    }
    {
        key_ctx_t c = vctx(MODE_OSCILLOSCOPE, VIEW_MODE_FFT, KF_SPECTRUM_CURSOR);
        c.cursors_on = true;
        CHECK("FFT, spectrum cursor job: UP = line -1, DOWN = +1 (spectrum_cursor_move), LEFT/RIGHT = next line",
              is(key_map(&c, BTN_UP, false), KA_SPECTRUM_CURSOR_MOVE, -1) &&
              is(key_map(&c, BTN_DOWN, false), KA_SPECTRUM_CURSOR_MOVE, +1) &&
              is(key_map(&c, BTN_LEFT, false), KA_CURSOR_NEXT, 0) &&
              is(key_map(&c, BTN_RIGHT, false), KA_CURSOR_NEXT, 0) &&
              is(key_map(&c, BTN_AUTO, false), KA_FFT_AUTO, 0));
    }
    CHECK("split: zoom -> channel move -> trigger move -> spectrum -> zoom",
          key_focus_next(sp, KF_TRIGMOVE, true, true) == KF_SPECTRUM &&
          key_focus_next(sp, KF_SPECTRUM, true, true) == KF_ZOOM &&
          key_focus_sanitize(sp, KF_MASK, true, true) == KF_ZOOM);
    CHECK("a view.mode out of range is Y-T",
          key_focus_next(VIEW_MODE_COUNT, KF_TRIGMOVE, true, true) == KF_CURSOR &&
          key_focus_sanitize(VIEW_MODE_COUNT, KF_SPECTRUM, true, true) == KF_ZOOM);
    {
        int named = 1;
        for (int j = KF_ZOOM; j < KF_COUNT; j++) {
            named &= strcmp(key_focus_name((key_focus_t)j), "?") != 0 && key_focus_popup((key_focus_t)j)[0] != '\0';
            for (int k = KF_ZOOM; k < j; k++)
                named &= strcmp(key_focus_name((key_focus_t)j), key_focus_name((key_focus_t)k)) != 0;
        }
        CHECK("every job has its own shell word and a popup; spectrum's names its keys",
              named && strcmp(key_focus_name(KF_SPECTRUM), "spectrum") == 0 &&
              strstr(key_focus_popup(KF_SPECTRUM), "Ref dB") && strstr(key_focus_popup(KF_SPECTRUM), "Span") &&
              strcmp(key_focus_name(KF_COUNT), "?") == 0);
    }
    CHECK("the spectrum job works on the spectrum, every other on the time record",
          key_focus_domain(KF_SPECTRUM) == VIEW_DOM_FREQ && key_focus_domain(KF_ZOOM) == VIEW_DOM_TIME &&
          key_focus_domain(KF_MASK) == VIEW_DOM_TIME && key_focus_domain(KF_CURSOR) == VIEW_DOM_TIME &&
          key_focus_domain(KF_SPECTRUM_CURSOR) == VIEW_DOM_FREQ);
    CHECK("the cursor jobs are the time cursor and the spectrum cursor; scursor's shell word",
          key_focus_is_cursor(KF_CURSOR) && key_focus_is_cursor(KF_SPECTRUM_CURSOR) &&
          !key_focus_is_cursor(KF_ZOOM) && !key_focus_is_cursor(KF_SPECTRUM) && !key_focus_is_cursor(KF_MASK) &&
          strcmp(key_focus_name(KF_SPECTRUM_CURSOR), "scursor") == 0 &&
          strcmp(key_focus_popup(KF_SPECTRUM_CURSOR), "Arrows: Cursor") == 0);
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

/* ── key_map_global (ARCH-1 B1): the keys every screen shares ──────── */

/* The grid below covers the views of the time record the pre-B1 key map
 * knew (its "time view") and the jobs it knew: the time-domain jobs, which
 * come first in key_focus_t. */
static const uint8_t k_grid_views[] = { VIEW_MODE_YT, VIEW_MODE_ROLL };
#define KM_GRID_TIME_JOBS  KF_SPECTRUM      /* KF_ZOOM .. KF_MASK */

/* The actions as numbered at 6a8c2ba: KA_SPECTRUM_CURSOR_MOVE was added
 * after KA_CURSOR_NEXT since, so the ones after it move down by one. It is
 * never an answer in this grid (no spectrum job; test_global checks). */
static uint8_t pre_b1_code(key_action_t a)
{
    return (uint8_t)(a > KA_SPECTRUM_CURSOR_MOVE ? a - 1 : a);
}

/* key_map over every context key_ctx_t can express in those views (mode,
 * settings depth 0/3/6, view, job, cursors, mask, channel, page
 * open/locked, opener) x 15 keys x short/long: an FNV-1a hash of the
 * (act, arg) answers in a fixed order. */
static uint32_t km_grid(key_cmd_t (*fn)(const key_ctx_t *, button_id_t, bool), unsigned *count)
{
    static const uint8_t openers[] = { BTN_NONE, BTN_PRM, BTN_TRIGGER, BTN_CH1, BTN_CH2, BTN_MENU };
    uint32_t h = 2166136261u;
    unsigned n = 0;
    for (int mode = 0; mode < MODE_COUNT; mode++)
    for (int depth = 0; depth < 8; depth += 3)
    for (unsigned v = 0; v < sizeof k_grid_views; v++)
    for (int f = 0; f < KM_GRID_TIME_JOBS; f++)
    for (int cur = 0; cur < 2; cur++)
    for (int mk = 0; mk < 2; mk++)
    for (int ach = 0; ach < 2; ach++)
    for (int op = 0; op < 2; op++)
    for (int lk = 0; lk < 2; lk++)
    for (unsigned o = 0; o < sizeof openers; o++)
    for (int b = BTN_CH1; b <= BTN_POWER; b++)
    for (int lng = 0; lng < 2; lng++) {
        key_ctx_t c;
        memset(&c, 0, sizeof c);
        c.mode = (device_mode_t)mode; c.view = k_grid_views[v];
        c.focus = (key_focus_t)f; c.cursors_on = cur; c.mask_present = mk; c.active_channel = (uint8_t)ach;
        c.osc_menu_open = op; c.osc_menu_locked = lk; c.osc_menu_opener = openers[o];
        const key_cmd_t k = fn(&c, (button_id_t)b, lng != 0);
        const uint8_t bytes[2] = { pre_b1_code(k.act), (uint8_t)k.arg };
        for (int i = 0; i < 2; i++) { h ^= bytes[i]; h *= 16777619u; }
        n++;
    }
    *count = n;
    return h;
}

/* The same grid run over key_map() as it was before key_map_global was split
 * out of it (base 6a8c2ba, the tree the router was wired on): compiled from
 * that commit's key_model.c with this very km_grid(), each of the two views
 * here read as its scope_time_view ("scope mode and the time-domain view":
 * true in the scope, false in every other mode). key_map's answers in the
 * views of the time record must not move. (The spectrum views' keys are the
 * job's since the view table: tested by name in test_short_map and
 * test_long_map.) */
#define KM_GRID_PRE_B1_HASH   0xC772DF85u
#define KM_GRID_PRE_B1_COUNT  864000u

/* B7 gave the scope's last per-key cases names (KA_COUPLING_TOGGLE ..
 * KA_FFT_ZOOM), where key_map answered KA_LEGACY. key_map with those answers
 * read as KA_LEGACY again is the pre-B1 function over the grid: so nothing
 * else moved, and a B7 action appears only where a per-key case was. */
static bool is_b7_action(key_action_t a)
{
    return a == KA_COUPLING_TOGGLE || a == KA_RUNSTOP || a == KA_AUTOSET || a == KA_FFT_AUTO ||
           a == KA_FFT_REF || a == KA_FFT_ZOOM;
}
static bool g_grid_saw_added;      /* an action added since 6a8c2ba answered in the grid */
static key_cmd_t key_map_pre_b7(const key_ctx_t *c, button_id_t b, bool lng)
{
    key_cmd_t k = key_map(c, b, lng);
    g_grid_saw_added |= k.act == KA_SPECTRUM_CURSOR_MOVE;
    if (is_b7_action(k.act)) { k.act = KA_LEGACY; k.arg = 0; }
    return k;
}

static void test_global(void)
{
    printf("key_map_global (router step 4)\n");

    unsigned n = 0;
    const uint32_t h = km_grid(key_map_pre_b7, &n);
    int time_jobs = 1;
    for (int f = 0; f < KM_GRID_TIME_JOBS; f++) time_jobs &= key_focus_domain((key_focus_t)f) == VIEW_DOM_TIME;
    CHECK("the grid's jobs are the time-domain ones", time_jobs && key_focus_domain(KM_GRID_TIME_JOBS) != VIEW_DOM_TIME);
    CHECK("key_map: every answer over the Y-T / roll grid as before the split (B7 names read as legacy)",
          h == KM_GRID_PRE_B1_HASH && n == KM_GRID_PRE_B1_COUNT && !g_grid_saw_added);
    {
        /* ... and a B7 action is only ever answered for a short key of the
         * scope with the page closed, in every view and job. */
        static const uint8_t openers[] = { BTN_NONE, BTN_PRM };
        int stray = 0, seen = 0;
        for (int mode = 0; mode < MODE_COUNT; mode++)
        for (unsigned v = 0; v < N_VIEWS; v++)
        for (int f = 0; f < KF_COUNT; f++)
        for (int op = 0; op < 2; op++)
        for (int lk = 0; lk < 2; lk++)
        for (unsigned o = 0; o < sizeof openers; o++)
        for (int b = BTN_CH1; b <= BTN_POWER; b++)
        for (int lng = 0; lng < 2; lng++) {
            key_ctx_t c = vctx((device_mode_t)mode, k_views[v].view, (key_focus_t)f);
            c.cursors_on = true; c.mask_present = true;
            c.osc_menu_open = op; c.osc_menu_locked = lk; c.osc_menu_opener = openers[o];
            if (!is_b7_action(key_map(&c, (button_id_t)b, lng != 0).act)) continue;
            seen++;
            if (mode != MODE_OSCILLOSCOPE || op || lng) stray++;
        }
        CHECK("the B7 actions: scope only, short only, never with the page open", stray == 0 && seen > 0);
    }

    /* Exactly these are global, with these actions, in every context. */
    static const struct { button_id_t b; bool lng; key_action_t a; } glob[] = {
        { BTN_MENU, true, KA_ENTER_HOME },     { BTN_MOVE, true, KA_ENTER_METER },
        { BTN_SELECT, true, KA_ENTER_SCOPE },  { BTN_TRIGGER, true, KA_ENTER_SIGGEN },
        { BTN_SAVE, true, KA_GALLERY_OPEN },   { BTN_SAVE, false, KA_SCREENSHOT },
        { BTN_POWER, false, KA_POWER_PROMPT },
    };
    int bad = 0, agree = 0;
    for (int mode = 0; mode < MODE_COUNT; mode++)
    for (int op = 0; op < 2; op++)
    for (int lk = 0; lk < 2; lk++)
    for (unsigned v = 0; v < N_VIEWS; v++)
    for (int b = BTN_CH1; b <= BTN_POWER; b++)
    for (int lng = 0; lng < 2; lng++) {
        key_ctx_t c = vctx((device_mode_t)mode, k_views[v].view, KF_ZOOM);
        c.osc_menu_open = op; c.osc_menu_locked = lk;
        c.osc_menu_opener = BTN_PRM;
        key_action_t want = KA_LEGACY;
        for (unsigned i = 0; i < sizeof glob / sizeof glob[0]; i++)
            if (glob[i].b == (button_id_t)b && glob[i].lng == (lng != 0)) want = glob[i].a;
        const key_cmd_t g = key_map_global(&c, (button_id_t)b, lng != 0);
        if (g.act != want || g.arg != 0) bad++;
        if (key_map_global(NULL, (button_id_t)b, lng != 0).act != g.act) bad++;
        /* key_map gives the global answer first, POWER aside (KA_LEGACY). */
        const key_cmd_t m = key_map(&c, (button_id_t)b, lng != 0);
        if (want != KA_LEGACY && b != BTN_POWER) agree += (m.act == want);
        if (b == BTN_POWER && !lng && m.act != KA_LEGACY) bad++;
    }
    CHECK("globals: long MENU/MOVE/SELECT/TRIGGER/SAVE, short SAVE/POWER, any context", bad == 0);
    CHECK("key_map returns the global answer for every global key (POWER stays LEGACY)",
          agree == (int)(MODE_COUNT * 2 * 2 * N_VIEWS * 6));
    CHECK("no other key is global (arrows, OK, CH1/CH2, PRM, AUTO, short mode keys)",
          key_map_global(NULL, BTN_UP, false).act == KA_LEGACY &&
          key_map_global(NULL, BTN_OK, true).act == KA_LEGACY &&
          key_map_global(NULL, BTN_PRM, true).act == KA_LEGACY &&
          key_map_global(NULL, BTN_CH1, true).act == KA_LEGACY &&
          key_map_global(NULL, BTN_AUTO, true).act == KA_LEGACY &&
          key_map_global(NULL, BTN_MENU, false).act == KA_LEGACY &&
          key_map_global(NULL, BTN_MOVE, false).act == KA_LEGACY);
}

int main(void)
{
    test_scan();
    test_long_map();
    test_short_map();
    test_trigger_key();
    test_save();
    test_osc_menu();
    test_focus();
    test_state();
    test_global();
    printf("\n%d/%d passed\n", passed, run);
    return passed == run ? 0 : 1;
}
