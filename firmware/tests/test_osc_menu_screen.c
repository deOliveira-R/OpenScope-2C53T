/*
 * Host tests for the Oscilloscope Settings page as a screen of the router
 * (src/ui/osc_menu_screen.c; ARCH-1 B5): the real screen, the real router
 * (screen.c), the real page model (osc_menu_model.c), the real key map
 * (key_model.c) and the real property model with fake hardware ops.
 *
 * What is checked: the binding table (every pane and row names a settable
 * property; a RADIO item is a value of it); that every row and item sets its
 * property through ONE set, in both directions, with the owner call the
 * setter makes; the rules that stay in the setters (keep one channel on, a
 * refused relay, a channel that is off, Single); the footer notes; that a
 * set from the shell while the page is open moves the page's epoch (so it
 * is repainted in place) and an idle page's does not; the router order (the
 * self-cal lock and the globals before the page, POWER never to it, nothing
 * below it); closing; the paints; `ui screen`'s row/pane/item.
 *
 * Build: make test-osc-menu-screen.
 */
#include "screen_test_env.h"
#include "osc_menu_screen.h"
#include "osc_menu_model.h"
#include "key_model.h"
#include "persistence.h"
#include "math_channel.h"

/* ── osc_menu_ui.c's paint (display task in the firmware) ──────────── */
static int g_draws, g_invalidates;
void osc_menu_draw(void)       { g_draws++; }
void osc_menu_invalidate(void) { g_invalidates++; }

/* ── input_handler.c's part: the arrows' focus follows the cursors ─── */
static int g_cursors_calls, g_cursors_on;
void osc_menu_env_cursors(bool on) { g_cursors_calls++; g_cursors_on = on; }

/* ── the scope's owners (fake ops; the rules are the setters') ─────── */
static bool t_il, t_coupling_ok, t_trigsrc_ok, t_persist_ok, t_view_ok;
static uint8_t t_persist_mode;
static bool f_channel_enables(void)
{
    const scope_state_t *ss = scope_state_get();
    env_logf("enables(%d,%d);", ss->ch1.enabled, ss->ch2.enabled);
    return ss->ch1.enabled || ss->ch2.enabled;
}
static bool f_interleave(void) { return t_il; }
static bool f_coupling(uint8_t ch, uint8_t c) { env_logf("coupling(%u,%u);", ch, c); return t_coupling_ok; }
static bool f_trigsrc(void) { env_logf("trigsrc(%d);", scope_state_get()->trigger.source); return t_trigsrc_ok; }
static void f_rearm(void) { env_logf("rearm;"); }
static bool f_view(uint8_t v)
{
    env_logf("view(%u);", v);
    if (t_view_ok) scope_view = (scope_view_t)v;
    return t_view_ok;
}
static bool f_persist_on(uint8_t m)
{
    env_logf("persist_on(%u);", m);
    if (!t_persist_ok) return false;
    persist_enabled = true; t_persist_mode = m;
    return true;
}
static void f_persist_off(void) { env_logf("persist_off;"); persist_enabled = false; }
static uint8_t f_persist_mode(void) { return t_persist_mode; }
static bool f_roll(void) { env_logf("roll(%d);", scope_state_get()->roll); return true; }

static model_hw_ops_t t_ops;

static const screen_t S_SCOPE = ENV_BASE("scope", SCR_SCOPE, MODE_OSCILLOSCOPE, 0);
static const screen_t S_METER = ENV_BASE("meter", SCR_METER, MODE_MULTIMETER, 1);

static int g_after; static scr_res_t g_after_res;
static int g_locked_asked, g_globals_asked; static bool g_lock_on;
static void h_after(scr_res_t r) { g_after++; g_after_res = r; }
static bool h_locked(button_id_t b, bool l) { (void)b; (void)l; g_locked_asked++; return g_lock_on; }
/* The firmware's globals: SAVE, POWER and the long mode keys (key_map_global). */
static bool h_globals(button_id_t b, bool l)
{
    g_globals_asked++;
    return key_map_global(NULL, b, l).act != KA_LEGACY;
}
static const screen_hooks_t HOOKS = { .locked = h_locked, .globals = h_globals, .after_key = h_after };

static int g_scope_keys;
static scr_res_t scope_key(scr_key_t k) { (void)k; g_scope_keys++; return SCR_HANDLED; }
static screen_t S_SCOPE_K;

/* A scope with the page closed. */
static void boot(void)
{
    env_fresh(MODE_OSCILLOSCOPE);
    t_ops = env_ops;
    t_ops.channel_enables = f_channel_enables; t_ops.interleave_active = f_interleave;
    t_ops.coupling = f_coupling; t_ops.trigger_source = f_trigsrc; t_ops.trigger_source_follow = f_trigsrc; t_ops.single_rearm = f_rearm;
    t_ops.view_select = f_view; t_ops.persist_on = f_persist_on; t_ops.persist_off = f_persist_off;
    t_ops.persist_mode = f_persist_mode; t_ops.roll = f_roll;
    model_bind_hw(&t_ops);
    t_il = false; t_coupling_ok = t_trigsrc_ok = t_persist_ok = t_view_ok = true; t_persist_mode = 0;
    osc_menu_close();
    S_SCOPE_K = S_SCOPE; S_SCOPE_K.key = scope_key;
    screen_reset();
    screen_register(&S_SCOPE_K); screen_register(&S_METER); screen_register(&osc_menu_screen);
    screen_set_hooks(&HOOKS);
    screen_init(SCR_SCOPE);
    g_after = g_locked_asked = g_globals_asked = g_scope_keys = 0; g_lock_on = false;
    g_draws = g_invalidates = g_cursors_calls = 0; g_cursors_on = -1;
    env_log_clear();
}

/* What the scope's key and the stack mirror do: open the page, push its layer. */
static void open_page(osc_row_t row, bool locked, uint8_t opener)
{
    osc_menu_screen_open(row, locked, opener);      /* pushes its layer */
}
/* The full page, focus inside `row`'s pane (as OK on the rail leaves it). */
static void open_in(osc_row_t row)
{
    boot();
    open_page(row, false, BTN_MENU);
    (void)env_key(BTN_OK);
    env_log_clear();
}
static const osc_menu_state_t *S(void) { return osc_menu_state(); }
static const char *note(void) { return osc_menu_screen_note(); }
static int32_t G(const prop_desc_t *p) { return prop_get(p); }
static void down(int n) { while (n-- > 0) (void)env_key(BTN_DOWN); }

/* ── the screen and its table ──────────────────────────────────────── */

static void test_screen_entry(void)
{
    printf("the screen\n");
    CHECK("name osc_menu, id SCR_OSC_MENU, no mode, no instrument",
          strcmp(osc_menu_screen.name, "osc_menu") == 0 && osc_menu_screen.id == SCR_OSC_MENU &&
          osc_menu_screen.mode == MODE_COUNT && osc_menu_screen.instrument == SCR_INSTRUMENT_NONE);
    CHECK("OPAQUE, not GRAB (the self-cal lock comes before it), bars kept",
          osc_menu_screen.flags == SCR_F_OPAQUE);
    CHECK("it has key, close, draw, epoch and describe; no tick (the epoch is the gate)",
          osc_menu_screen.key && osc_menu_screen.close && osc_menu_screen.draw && osc_menu_screen.epoch &&
          osc_menu_screen.describe && osc_menu_screen.tick == NULL);
    const uint16_t g = osc_menu_screen.groups;
    CHECK("it shows the scope's groups and the bars', not the meter's",
          (g & (1u << PG_CH1)) && (g & (1u << PG_CH2)) && (g & (1u << PG_TRIG)) && (g & (1u << PG_VIEW)) &&
          (g & (1u << PG_SYS)) && (g & (1u << PG_GEN)) && !(g & (1u << PG_METER)));
}

static void test_table(void)
{
    printf("the binding table\n");
    boot();
    static const struct { osc_row_t row; uint8_t item; const char *name; } B[] = {
        { OSC_ROW_DISPLAY, 0, "view.mode" },     { OSC_ROW_CURSOR, 0, "view.cursor" },
        { OSC_ROW_PERSIST, 0, "view.persist" },  { OSC_ROW_MATH, 0, "view.math" },
        { OSC_ROW_PARAM, 0, "ch1.meas" },
        { OSC_ROW_CH1, OSC_CH_ENABLE, "ch1.enabled" }, { OSC_ROW_CH1, OSC_CH_PROBE, "ch1.probe" },
        { OSC_ROW_CH1, OSC_CH_COUPLING, "ch1.coupling" },
        { OSC_ROW_CH2, OSC_CH_ENABLE, "ch2.enabled" }, { OSC_ROW_CH2, OSC_CH_PROBE, "ch2.probe" },
        { OSC_ROW_CH2, OSC_CH_COUPLING, "ch2.coupling" },
        { OSC_ROW_TRIGGER, OSC_TRIG_MODE, "trig.mode" }, { OSC_ROW_TRIGGER, OSC_TRIG_EDGE, "trig.edge" },
        { OSC_ROW_TRIGGER, OSC_TRIG_SOURCE, "trig.source" },
    };
    int ok = 1;
    for (unsigned i = 0; i < sizeof B / sizeof B[0]; i++) {
        const prop_desc_t *p = osc_menu_bind_prop(B[i].row, B[i].item);
        ok &= p != NULL && strcmp(p->name, B[i].name) == 0 && prop_is_settable(p) && p == prop_find(B[i].name);
    }
    CHECK("every pane and row names its property, and it is settable", ok);

    int rows = 0; ok = 1;
    for (int r = 0; r < OSC_ROW_COUNT; r++) {
        const osc_pane_layout_t *l = osc_menu_layout((osc_row_t)r);
        if (l->kind != OSC_PANE_VALUES) continue;
        for (uint8_t i = 0; i < l->n_items; i++, rows++) {
            const prop_desc_t *p = osc_menu_bind_prop((osc_row_t)r, i);
            ok &= p != NULL && (p->flags & PF_WRAP) && p->step == 1 &&
                  osc_menu_bind_value((osc_row_t)r, i) == -1;
        }
        ok &= osc_menu_bind_prop((osc_row_t)r, l->n_items) == NULL;
    }
    CHECK("VALUES rows: 9, each a wrapping property stepped by 1; no row past the pane", ok && rows == 9);

    ok = 1;
    static const osc_row_t radio[] = { OSC_ROW_DISPLAY, OSC_ROW_CURSOR, OSC_ROW_PERSIST, OSC_ROW_MATH };
    for (unsigned k = 0; k < 4; k++) {
        const prop_desc_t *p = osc_menu_bind_prop(radio[k], 0);
        const uint8_t n = osc_menu_layout(radio[k])->n_items;
        uint32_t seen = 0;
        for (uint8_t i = 0; i < n; i++) {
            const int32_t v = osc_menu_bind_value(radio[k], i);
            ok &= v >= p->min && v <= p->max && !(seen & (1u << v));
            seen |= 1u << v;
        }
        /* every value of the property has an item: the pane can show any state */
        ok &= (int32_t)n == p->max - p->min + 1 && osc_menu_bind_value(radio[k], n) == -1;
    }
    CHECK("RADIO / Math: item -> a value of the property, one to one and onto", ok);

    char t[24];
    ok = 1;
    for (uint8_t i = 0; i < OSC_DISP_COUNT; i++) {
        static const char *const tok[] = { "yt", "roll", "xy", "fft", "split", "waterfall" };
        prop_format_value(P(VIEW_MODE), osc_menu_bind_value(OSC_ROW_DISPLAY, i), t, sizeof t);
        ok &= strcmp(t, tok[i]) == 0;
    }
    CHECK("Display items in order are view.mode yt roll xy fft split waterfall", ok);
    ok = 1;
    for (uint8_t i = 1; i < OSC_MATH_COUNT; i++)
        ok &= osc_menu_bind_value(OSC_ROW_MATH, i) == 1 + (int32_t)osc_menu_math_op(i);
    CHECK("Math: Off = 0, a chip = 1 + its math_op_t", ok && osc_menu_bind_value(OSC_ROW_MATH, 0) == 0);
    CHECK("no binding out of range",
          osc_menu_bind_prop(OSC_ROW_COUNT, 0) == NULL && osc_menu_bind_value(OSC_ROW_COUNT, 0) == -1 &&
          osc_menu_bind_value(OSC_ROW_CH1, 0) == -1 && osc_menu_bind_current(OSC_ROW_COUNT) == 0);
}

/* The text a VALUES row shows is the one the page always showed. */
static void test_value_text(void)
{
    printf("VALUES rows: the text of every value\n");
    boot();
    scope_state_t *ss = scope_state_get();
    char t[16];
    int ok = 1;
    for (int ch = 0; ch < 2; ch++) {
        channel_state_t *c = ch ? &ss->ch2 : &ss->ch1;
        const osc_row_t row = ch ? OSC_ROW_CH2 : OSC_ROW_CH1;
        for (int e = 0; e < 2; e++) {
            c->enabled = e;
            osc_menu_bind_text(row, OSC_CH_ENABLE, t, sizeof t);
            ok &= strcmp(t, e ? "ON" : "OFF") == 0;
        }
        for (int p = 0; p < PROBE_COUNT; p++) {
            c->probe = (probe_t)p;
            osc_menu_bind_text(row, OSC_CH_PROBE, t, sizeof t);
            ok &= strcmp(t, probe_labels[p]) == 0;
        }
        for (int k = 0; k <= COUPLING_AC; k++) {
            c->coupling = (coupling_t)k;
            osc_menu_bind_text(row, OSC_CH_COUPLING, t, sizeof t);
            ok &= strcmp(t, coupling_labels[k]) == 0;
        }
    }
    CHECK("CH1 / CH2: ON OFF, 1X 10X, DC AC", ok && PROBE_COUNT == 2);
    ok = 1;
    for (int m = 0; m < TRIG_COUNT; m++) {
        ss->trigger.mode = (trigger_mode_t)m;
        osc_menu_bind_text(OSC_ROW_TRIGGER, OSC_TRIG_MODE, t, sizeof t);
        ok &= strcmp(t, trigger_mode_labels[m]) == 0;
    }
    for (int e = 0; e < TRIG_EDGE_COUNT; e++) {
        ss->trigger.edge = (trigger_edge_t)e;
        osc_menu_bind_text(OSC_ROW_TRIGGER, OSC_TRIG_EDGE, t, sizeof t);
        ok &= strcmp(t, trigger_edge_labels[e]) == 0;
    }
    for (int s = 0; s < TRIG_SRC_COUNT; s++) {
        ss->trigger.source = (trigger_source_t)s;
        osc_menu_bind_text(OSC_ROW_TRIGGER, OSC_TRIG_SOURCE, t, sizeof t);
        ok &= strcmp(t, trigger_source_labels[s]) == 0;
    }
    CHECK("Trigger: Auto Normal Single, Rising Falling, CH1 CH2", ok && TRIG_COUNT == 3);
    osc_menu_bind_text(OSC_ROW_DISPLAY, 0, t, sizeof t);
    CHECK("a RADIO pane has no value text", t[0] == '\0');
}

/* ── every row sets its property, both directions ──────────────────── */

/* One key on VALUES row `item` of `row`: the property moved to `want`,
 * `get` says so, its group's epoch moved, and the owner was called `log`. */
static int values_step(osc_row_t row, uint8_t item, uint8_t key, int32_t want, const char *log)
{
    const prop_desc_t *p = osc_menu_bind_prop(row, item);
    while (S()->item != item) (void)env_key(BTN_DOWN);
    env_log_clear();
    const env_epochs_t e0 = env_epochs();
    const int32_t was = G(p);
    const scr_res_t r = env_key(key);
    const env_epochs_t e1 = env_epochs();
    char t[16], w[16];
    osc_menu_bind_text(row, item, t, sizeof t);
    if (p->type == PT_BOOL) snprintf(w, sizeof w, "%s", want ? "ON" : "OFF");
    else prop_format(p, want, w, sizeof w);
    const int ok = r == SCR_DRAW && G(p) == want && want != was && strcmp(t, w) == 0 &&
                   (env_epochs_moved(&e0, &e1) & (1u << p->group)) && env_log_is(log);
    if (!ok) printf("    %s item %u key %u: got %ld want %ld log [%s] text %s res %d\n", p->name, item, key,
                    (long)G(p), (long)want, env_log, t, r);
    return ok;
}

static void test_values_rows(void)
{
    printf("VALUES rows: LEFT / RIGHT / OK are one step of the row's property\n");
    for (int ch = 0; ch < 2; ch++) {
        const osc_row_t row = ch ? OSC_ROW_CH2 : OSC_ROW_CH1;
        char nm[64];
        open_in(row);
        /* The trigger is on CH1: CH1 going off moves it to CH2 first (the
         * setter's rule: reg 0x06, then the routing); it does not come back
         * with the channel, so the second time nothing moves. */
        int ok = values_step(row, OSC_CH_ENABLE, BTN_RIGHT, 0, ch ? "enables(1,0);" : "trigsrc(1);enables(0,1);");
        ok &= values_step(row, OSC_CH_ENABLE, BTN_LEFT, 1, "enables(1,1);");
        ok &= values_step(row, OSC_CH_ENABLE, BTN_OK, 0, ch ? "enables(1,0);" : "enables(0,1);");
        ok &= values_step(row, OSC_CH_ENABLE, BTN_OK, 1, "enables(1,1);");
        snprintf(nm, sizeof nm, "CH%d Channel: chN.enabled off and on, each way; the trigger leaves a channel that goes off", ch + 1);
        CHECK(nm, ok);
        ok  = values_step(row, OSC_CH_PROBE, BTN_RIGHT, PROBE_10X, "");
        ok &= values_step(row, OSC_CH_PROBE, BTN_RIGHT, PROBE_1X, "");      /* wraps */
        ok &= values_step(row, OSC_CH_PROBE, BTN_LEFT, PROBE_10X, "");      /* wraps the other way */
        ok &= values_step(row, OSC_CH_PROBE, BTN_OK, PROBE_1X, "");
        snprintf(nm, sizeof nm, "CH%d Probe: chN.probe 1X <-> 10X, wrapping, no hardware", ch + 1);
        CHECK(nm, ok);
        ok  = values_step(row, OSC_CH_COUPLING, BTN_RIGHT, COUPLING_AC, ch ? "coupling(1,1);" : "coupling(0,1);");
        ok &= values_step(row, OSC_CH_COUPLING, BTN_LEFT, COUPLING_DC, ch ? "coupling(1,0);" : "coupling(0,0);");
        ok &= values_step(row, OSC_CH_COUPLING, BTN_OK, COUPLING_AC, ch ? "coupling(1,1);" : "coupling(0,1);");
        snprintf(nm, sizeof nm, "CH%d Coupling: chN.coupling DC <-> AC through the relay (GND never)", ch + 1);
        CHECK(nm, ok && G(osc_menu_bind_prop(row, OSC_CH_COUPLING)) <= COUPLING_AC);
    }
    open_in(OSC_ROW_TRIGGER);
    int ok = values_step(OSC_ROW_TRIGGER, OSC_TRIG_MODE, BTN_RIGHT, TRIG_NORMAL, "");
    ok &= values_step(OSC_ROW_TRIGGER, OSC_TRIG_MODE, BTN_RIGHT, TRIG_SINGLE, "rearm;");
    ok &= values_step(OSC_ROW_TRIGGER, OSC_TRIG_MODE, BTN_RIGHT, TRIG_AUTO, "");       /* wraps */
    ok &= values_step(OSC_ROW_TRIGGER, OSC_TRIG_MODE, BTN_LEFT, TRIG_SINGLE, "rearm;"); /* wraps back */
    ok &= values_step(OSC_ROW_TRIGGER, OSC_TRIG_MODE, BTN_LEFT, TRIG_NORMAL, "");
    ok &= values_step(OSC_ROW_TRIGGER, OSC_TRIG_MODE, BTN_OK, TRIG_SINGLE, "rearm;");
    CHECK("Trigger Mode: trig.mode Auto Normal Single, wrapping; Single arms the one-shot", ok);
    ok  = values_step(OSC_ROW_TRIGGER, OSC_TRIG_EDGE, BTN_RIGHT, TRIG_FALLING, "");
    ok &= values_step(OSC_ROW_TRIGGER, OSC_TRIG_EDGE, BTN_LEFT, TRIG_RISING, "");
    ok &= values_step(OSC_ROW_TRIGGER, OSC_TRIG_EDGE, BTN_OK, TRIG_FALLING, "");
    CHECK("Trigger Edge: trig.edge Rising <-> Falling, no register", ok);
    ok  = values_step(OSC_ROW_TRIGGER, OSC_TRIG_SOURCE, BTN_RIGHT, TRIG_SRC_CH2, "trigsrc(1);");
    ok &= values_step(OSC_ROW_TRIGGER, OSC_TRIG_SOURCE, BTN_LEFT, TRIG_SRC_CH1, "trigsrc(0);");
    ok &= values_step(OSC_ROW_TRIGGER, OSC_TRIG_SOURCE, BTN_OK, TRIG_SRC_CH2, "trigsrc(1);");
    CHECK("Trigger Channel: trig.source CH1 <-> CH2 through reg 0x06", ok);
}

/* Pick item `item` of a RADIO / Math pane by its keys; the cursor starts on
 * the value in force. */
static scr_res_t pick(osc_row_t row, uint8_t item)
{
    const osc_pane_layout_t *l = osc_menu_layout(row);
    for (int guard = 0; guard < 40 && S()->item != item; guard++) {
        if (l->kind == OSC_PANE_RADIO) { (void)env_key(BTN_DOWN); continue; }
        uint8_t r, c, wr, wc;                  /* the Math grid: by row, then by column */
        osc_menu_chip_pos(row, S()->item, &r, &c);
        osc_menu_chip_pos(row, item, &wr, &wc);
        if (r != wr) (void)env_key(r < wr ? BTN_DOWN : BTN_UP);
        else         (void)env_key(c < wc ? BTN_RIGHT : BTN_LEFT);
    }
    env_log_clear();
    return env_key(BTN_OK);
}

static void test_radio_panes(void)
{
    printf("RADIO panes and Math: OK on an item is one set of the pane's property\n");
    int ok = 1;
    for (uint8_t from = 0; from < OSC_DISP_COUNT; from++)
    for (uint8_t to = 0; to < OSC_DISP_COUNT; to++) {
        boot();
        (void)prop_set_local(P(VIEW_MODE), from, ORIGIN_SHELL, NULL);
        open_page(OSC_ROW_DISPLAY, false, BTN_MENU);
        (void)env_key(BTN_OK);
        ok &= S()->item == from && osc_menu_bind_current(OSC_ROW_DISPLAY) == from;   /* ENTER = get */
        const scr_res_t r = pick(OSC_ROW_DISPLAY, to);
        ok &= r == SCR_DRAW && G(P(VIEW_MODE)) == to && osc_menu_bind_current(OSC_ROW_DISPLAY) == to &&
              note()[0] == '\0' && osc_menu_is_open();
        if (!ok) { printf("    display %u -> %u: view.mode %ld\n", from, to, (long)G(P(VIEW_MODE))); break; }
    }
    CHECK("Display: every view from every view (36), the cursor entering on the view in force", ok);

    ok = 1;
    for (uint8_t from = 0; from < OSC_PERS_COUNT; from++)
    for (uint8_t to = 0; to < OSC_PERS_COUNT; to++) {
        boot();
        (void)prop_set_local(P(VIEW_PERSIST), from, ORIGIN_SHELL, NULL);
        open_page(OSC_ROW_PERSIST, false, BTN_MENU);
        (void)env_key(BTN_OK);
        ok &= S()->item == from;
        const scr_res_t r = pick(OSC_ROW_PERSIST, to);
        ok &= r == SCR_DRAW && G(P(VIEW_PERSIST)) == to &&
              env_log_is(to == 0 ? "persist_off;" : to == 1 ? "persist_on(1);" : to == 2 ? "persist_on(2);"
                         : to == 3 ? "persist_on(3);" : "persist_on(4);");
    }
    CHECK("Persist: every mode from every mode (25), through the pool's owner", ok);

    ok = 1;
    for (uint8_t from = 0; from < OSC_MATH_COUNT; from++)
    for (uint8_t to = 0; to < OSC_MATH_COUNT; to++) {
        boot();
        (void)prop_set_local(P(VIEW_MATH), osc_menu_bind_value(OSC_ROW_MATH, from), ORIGIN_SHELL, NULL);
        open_page(OSC_ROW_MATH, false, BTN_MENU);
        (void)env_key(BTN_OK);
        ok &= S()->item == from;
        const scr_res_t r = pick(OSC_ROW_MATH, to);
        ok &= r == SCR_DRAW && G(P(VIEW_MATH)) == osc_menu_bind_value(OSC_ROW_MATH, to) &&
              osc_menu_bind_current(OSC_ROW_MATH) == to &&
              math_enabled == (to != 0) && (to == 0 || math_op == osc_menu_math_op(to));
    }
    CHECK("Math: every chip from every chip (81): view.math, math_enabled and math_op follow", ok);

    ok = 1;
    for (uint8_t from = 0; from < OSC_CUR_COUNT; from++)
    for (uint8_t to = 0; to < OSC_CUR_COUNT; to++) {
        boot();
        (void)prop_set_local(P(VIEW_CURSOR), from, ORIGIN_SHELL, NULL);
        open_page(OSC_ROW_CURSOR, false, BTN_MENU);
        (void)env_key(BTN_OK);
        ok &= S()->item == from;
        g_cursors_calls = 0;
        const scr_res_t r = pick(OSC_ROW_CURSOR, to);
        const cursor_state_t *c = &scope_state_get()->cursor;
        ok &= G(P(VIEW_CURSOR)) == to && (int)c->mode == to &&
              c->active == (to == CURSOR_HORIZONTAL ? CURSOR_SEL_H1 : CURSOR_SEL_V1);
        /* a cursor mode closes the page (the arrows move the cursors at once); Off keeps it */
        ok &= (to == OSC_CUR_OFF) ? (r == SCR_DRAW && osc_menu_is_open() && screen_top() == SCR_OSC_MENU)
                                  : (r == SCR_DRAW_FULL && !osc_menu_is_open() && screen_top() == SCR_SCOPE);
        /* the focus hook: only when the mode changed, with "some cursor is on" */
        ok &= (to == from) ? g_cursors_calls == 0 : (g_cursors_calls == 1 && g_cursors_on == (to != 0));
    }
    CHECK("Cursors: every mode from every mode (16): view.cursor; a mode closes the page; the focus hook", ok);
}

/* ── the rules stay in the setters; the footer says what the row cannot ── */

static void test_rules_and_notes(void)
{
    printf("the setters' rules, and the footer note\n");
    open_in(OSC_ROW_CH2);
    (void)env_key(BTN_RIGHT);                                   /* CH2 off */
    CHECK("CH2 off: no note (the row says OFF)", G(P(CH2_ENABLED)) == 0 && note()[0] == '\0');
    open_page(OSC_ROW_CH1, false, BTN_CH1); (void)env_key(BTN_OK); env_log_clear();
    const scr_res_t r = env_key(BTN_RIGHT);                      /* CH1 off too: refused */
    CHECK("the last channel cannot go off: refused by the setter, the value kept, \"Keep one CH on\"",
          r == SCR_DRAW && G(P(CH1_ENABLED)) == 1 && strcmp(note(), "Keep one CH on") == 0 &&
          env_log_is(""));                                       /* before any owner call */
    CHECK("a key that redraws nothing keeps the note", env_key(BTN_MOVE) == SCR_HANDLED &&
          strcmp(note(), "Keep one CH on") == 0);
    (void)env_key(BTN_DOWN);
    CHECK("the next key that moves something drops it", note()[0] == '\0');

    open_in(OSC_ROW_CH2);
    t_il = true;
    (void)env_key(BTN_RIGHT);
    CHECK("CH2 off with the interleave in force: the setter's note \"CH1 alone: 250 MS/s\"",
          G(P(CH2_ENABLED)) == 0 && strcmp(note(), "CH1 alone: 250 MS/s") == 0);
    CHECK("the active channel follows the live one (ui.ach)",
          (active_channel = 1, open_in(OSC_ROW_CH2), env_key(BTN_RIGHT), active_channel == 0));

    open_in(OSC_ROW_CH1);
    down(2);
    t_coupling_ok = false;
    (void)env_key(BTN_RIGHT);
    CHECK("a relay that does not answer: coupling kept, \"CH1 AC NOT SET\"",
          G(P(CH1_COUPLING)) == COUPLING_DC && strcmp(note(), "CH1 AC NOT SET") == 0 &&
          env_log_is("coupling(0,1);"));
    open_in(OSC_ROW_CH2);
    down(2);
    (void)prop_set_local(P(CH2_COUPLING), COUPLING_AC, ORIGIN_SHELL, NULL);
    t_coupling_ok = false;
    (void)env_key(BTN_LEFT);
    CHECK("... CH2, from AC: \"CH2 DC NOT SET\"",
          G(P(CH2_COUPLING)) == COUPLING_AC && strcmp(note(), "CH2 DC NOT SET") == 0);

    open_in(OSC_ROW_TRIGGER);
    down(2);
    scope_state_get()->ch2.enabled = false;
    (void)env_key(BTN_RIGHT);
    CHECK("trigger on a channel that is off: refused, \"CH2 is off\", no register write",
          G(P(TRIG_SOURCE)) == TRIG_SRC_CH1 && strcmp(note(), "CH2 is off") == 0 && env_log_is(""));
    scope_state_get()->ch2.enabled = true;
    t_trigsrc_ok = false;
    (void)env_key(BTN_RIGHT);
    CHECK("reg 0x06 refused: \"Trig CH2 NOT SET\", the source put back",
          G(P(TRIG_SOURCE)) == TRIG_SRC_CH1 && strcmp(note(), "Trig CH2 NOT SET") == 0);

    open_in(OSC_ROW_TRIGGER);
    scope_state_get()->running = false;
    (void)env_key(BTN_RIGHT); (void)env_key(BTN_RIGHT);          /* Normal, Single */
    CHECK("Single: armed, running, \"Await Trigger...\"",
          G(P(TRIG_MODE)) == TRIG_SINGLE && scope_state_get()->running && strcmp(note(), "Await Trigger...") == 0);
    (void)env_key(BTN_RIGHT);
    CHECK("Auto again: no note", G(P(TRIG_MODE)) == TRIG_AUTO && note()[0] == '\0');

    boot();
    open_page(OSC_ROW_PERSIST, false, BTN_MENU); (void)env_key(BTN_OK);
    t_persist_ok = false;
    (void)pick(OSC_ROW_PERSIST, OSC_PERS_1S);
    CHECK("no pool for persistence: off kept, the whole text \"No memory for persistence\"",
          G(P(VIEW_PERSIST)) == 0 && strcmp(note(), "No memory for persistence") == 0);

    boot();
    open_page(OSC_ROW_DISPLAY, false, BTN_MENU); (void)env_key(BTN_OK);
    t_view_ok = false;
    (void)pick(OSC_ROW_DISPLAY, OSC_DISP_FFT);
    CHECK("a view its owner refuses: the old view kept, \"view NOT SET\"",
          G(P(VIEW_MODE)) == 0 && strcmp(note(), "view NOT SET") == 0);

    open_in(OSC_ROW_CH1);
    (void)env_key(BTN_RIGHT);                                    /* a note-less change */
    open_page(OSC_ROW_CH2, false, BTN_CH2);                      /* a jump: a new visit */
    CHECK("a note does not survive into the next visit",
          (open_in(OSC_ROW_CH2), env_key(BTN_RIGHT), open_page(OSC_ROW_CH1, false, BTN_CH1),
           (void)env_key(BTN_OK), (void)env_key(BTN_RIGHT), strcmp(note(), "Keep one CH on") == 0) &&
          (osc_menu_screen_open(OSC_ROW_DISPLAY, false, BTN_MENU), note()[0] == '\0'));

    open_in(OSC_ROW_CH1);
    env_busy = true;
    (void)env_key(BTN_RIGHT);
    CHECK("self-cal running (a shell autoset): the set is BUSY, nothing changes, the footer says why",
          G(P(CH1_ENABLED)) == 1 && strcmp(note(), "self-cal running") == 0 && env_log_is(""));
    env_busy = false;
}

/* ── Measure: the explicit actions ─────────────────────────────────── */

static void test_measure(void)
{
    printf("Measure pane (explicit: the model edits the selection, the screen says the group moved)\n");
    open_in(OSC_ROW_PARAM);
    const uint16_t m0 = (uint16_t)G(P(CH1_MEAS));
    env_epochs_t e0 = env_epochs();
    CHECK("the pane starts on Freq of CH1", S()->item == OSC_PRM_CHIP0 && S()->meas_ch == 0);
    scr_res_t r = env_key(BTN_OK);
    env_epochs_t e1 = env_epochs();
    const uint16_t bit = (uint16_t)(1u << osc_menu_prm_of_chip(0));
    CHECK("OK on a chip toggles its bit of ch1.meas; `get` and the ch1 epoch follow, ch2's does not",
          r == SCR_DRAW && (uint16_t)G(P(CH1_MEAS)) == (uint16_t)(m0 ^ bit) &&
          env_epochs_moved(&e0, &e1) == (1u << PG_CH1));
    (void)env_key(BTN_OK);
    CHECK("OK again puts it back", (uint16_t)G(P(CH1_MEAS)) == m0);
    (void)env_key(BTN_UP); (void)env_key(BTN_RIGHT);             /* row 0: tab, All, None */
    CHECK("the cursor is on All", S()->item == OSC_PRM_ALL);
    (void)env_key(BTN_OK);
    CHECK("All: ch1.meas = every measurement", (uint16_t)G(P(CH1_MEAS)) == 0x0FFFu);
    (void)env_key(BTN_RIGHT); (void)env_key(BTN_OK);
    CHECK("None: ch1.meas = 0", G(P(CH1_MEAS)) == 0 && G(P(CH2_MEAS)) == 0x0507);
    (void)env_key(BTN_LEFT); (void)env_key(BTN_LEFT);
    e0 = env_epochs();
    r = env_key(BTN_OK);
    e1 = env_epochs();
    CHECK("the channel tab flips the page: UI state, no property, no epoch",
          r == SCR_DRAW && S()->meas_ch == 1 && env_epochs_moved(&e0, &e1) == 0);
    (void)env_key(BTN_RIGHT);
    e0 = env_epochs();
    (void)env_key(BTN_OK);
    e1 = env_epochs();
    CHECK("All on CH2's page: ch2.meas, the ch2 epoch", (uint16_t)G(P(CH2_MEAS)) == 0x0FFFu &&
          G(P(CH1_MEAS)) == 0 && env_epochs_moved(&e0, &e1) == (1u << PG_CH2));
    CHECK("no hardware, no note", env_log_is("") && note()[0] == '\0');
}

/* ── a set from the shell while the page is open; idle ─────────────── */

/* The display task's gate (main.c layer_paint_update): paint when the epoch
 * is not the one last painted. Returns the paints it made. */
static uint32_t g_gate; static bool g_gate_primed;
static int frames(int n)
{
    const int d0 = g_draws;
    for (int i = 0; i < n; i++) {
        const uint32_t e = osc_menu_screen.epoch();
        if (g_gate_primed && e == g_gate) continue;
        osc_menu_screen.draw(SCR_PAINT_UPDATE, 0);
        g_gate = e; g_gate_primed = true;
    }
    return g_draws - d0;
}

static void test_shell_set_and_idle(void)
{
    printf("a set from the shell while the page is open; an idle page\n");
    open_in(OSC_ROW_CH1);
    g_gate_primed = false;
    CHECK("the page is painted once when it comes up", frames(3) == 1);
    CHECK("idle: 200 frames, no paint", frames(200) == 0);

    static const struct { const char *name; int32_t v; } SETS[] = {
        { "ch1.enabled", 0 }, { "ch1.probe", 1 }, { "ch1.coupling", 1 },
        { "ch2.enabled", 0 }, { "ch2.probe", 1 }, { "ch2.coupling", 1 },
        { "trig.mode", 1 }, { "trig.edge", 1 }, { "trig.source", 1 },
        { "view.mode", 3 }, { "view.cursor", 2 }, { "view.persist", 2 }, { "view.math", 4 },
        { "ch1.meas", 0x0001 }, { "ch2.meas", 0x0800 },
    };
    int ok = 1, inplace = 1;
    for (unsigned i = 0; i < sizeof SETS / sizeof SETS[0]; i++) {
        open_in(OSC_ROW_CH1);
        g_gate_primed = false;
        (void)frames(1);
        const prop_desc_t *p = prop_find(SETS[i].name);
        const int inv = g_invalidates;
        (void)model_touched_take();
        const prop_status_t st = prop_set_local(p, SETS[i].v, ORIGIN_SHELL, NULL);
        const uint16_t touched = model_touched_take();
        const int one = st == PS_OK && frames(1) == 1 && frames(50) == 0 && g_invalidates == inv;
        if (!one) printf("    set %s: not painted exactly once in place\n", SETS[i].name);
        ok &= one;
        /* the executor's choice for this change: the page in place (no clear).
         * ch1.enabled off moves the active channel too (ui): a full repaint. */
        const bool want_inplace = strcmp(SETS[i].name, "ch1.enabled") != 0;
        inplace &= osc_menu_screen_paints(touched) == want_inplace;
    }
    CHECK("each of the 15 bound properties set from the shell: one in-place paint, then none", ok);
    CHECK("... and the executor may post it as the page's own paint (a set that also moves ui.ach: not)",
          inplace);
    CHECK("a change of something the page does not draw is a full repaint: sys, tb, ui, gen, none",
          !osc_menu_screen_paints(1u << PG_SYS) && !osc_menu_screen_paints(1u << PG_TB) &&
          !osc_menu_screen_paints((1u << PG_CH1) | (1u << PG_UI)) && !osc_menu_screen_paints(1u << PG_GEN) &&
          !osc_menu_screen_paints(0) && osc_menu_screen_paints((1u << PG_CH1) | (1u << PG_VIEW)));

    open_in(OSC_ROW_TRIGGER);
    g_gate_primed = false;
    (void)frames(1);
    (void)prop_set_local(P(TRIG_MODE), TRIG_NORMAL, ORIGIN_SHELL, NULL);
    char t[16];
    osc_menu_bind_text(OSC_ROW_TRIGGER, OSC_TRIG_MODE, t, sizeof t);
    CHECK("the row then shows the shell's value, and the next key steps from it",
          strcmp(t, "Normal") == 0 && frames(1) == 1 && env_key(BTN_RIGHT) == SCR_DRAW &&
          G(P(TRIG_MODE)) == TRIG_SINGLE);
    CHECK("a theme change repaints it too", (theme_init(THEME_CLASSIC_GREEN), frames(1) == 1));

    open_in(OSC_ROW_CH1);
    g_gate_primed = false;
    (void)frames(1);
    CHECK("a cursor move is one paint", (env_key(BTN_DOWN), frames(1) == 1) && frames(20) == 0);
    CHECK("a key at a refusal that shows the same note again: no paint",
          (open_in(OSC_ROW_CH2), env_key(BTN_RIGHT), open_page(OSC_ROW_CH1, false, BTN_CH1),
           (void)env_key(BTN_OK), (void)env_key(BTN_RIGHT), g_gate_primed = false, frames(1) == 1) &&
          (env_key(BTN_RIGHT), frames(5) == 0));
}

/* ── the router: who is asked, in which order ──────────────────────── */

static void test_router_order(void)
{
    printf("router order: the lock and the globals before the page, POWER never, nothing below\n");
    open_in(OSC_ROW_CH1);
    g_globals_asked = g_locked_asked = g_scope_keys = g_after = 0;
    CHECK("an arrow: lock asked, globals asked, the page takes it, the scope under it is not asked",
          env_key(BTN_DOWN) == SCR_DRAW && g_locked_asked == 1 && g_globals_asked == 1 && g_scope_keys == 0 &&
          screen_last_handler() == SCR_OSC_MENU && g_after == 1 && g_after_res == SCR_DRAW);
    const uint8_t item = S()->item;
    CHECK("POWER: the globals', the page is not asked and stays as it is",
          env_key(BTN_POWER) == SCR_HANDLED && screen_last_handler() == SCR_COUNT && S()->item == item &&
          osc_menu_is_open() && g_scope_keys == 0);
    CHECK("SAVE and long SAVE: the globals' (capture, gallery), not the page's",
          env_key(BTN_SAVE) == SCR_HANDLED && env_key(BTN_SAVE | BTN_LONG_FLAG) == SCR_HANDLED &&
          screen_last_handler() == SCR_COUNT && osc_menu_is_open());
    CHECK("the long mode keys: the globals'",
          env_key(BTN_MENU | BTN_LONG_FLAG) == SCR_HANDLED && env_key(BTN_MOVE | BTN_LONG_FLAG) == SCR_HANDLED &&
          screen_last_handler() == SCR_COUNT);
    g_lock_on = true;
    const int32_t was = G(P(CH1_PROBE));
    CHECK("self-cal lock on: the lock takes the key, the page is not asked (OPAQUE, not GRAB)",
          env_key(BTN_RIGHT) == SCR_HANDLED && screen_last_handler() == SCR_COUNT && G(P(CH1_PROBE)) == was &&
          S()->item == item);
    g_lock_on = false;
    CHECK("MOVE / SELECT / AUTO: the page's, nothing happens, no paint asked",
          env_key(BTN_MOVE) == SCR_HANDLED && env_key(BTN_SELECT) == SCR_HANDLED &&
          env_key(BTN_AUTO) == SCR_HANDLED && screen_last_handler() == SCR_OSC_MENU && g_scope_keys == 0);
    CHECK("the plot's long keys do nothing while it is up (long CH1 does not switch the channel off)",
          env_key(BTN_CH1 | BTN_LONG_FLAG) == SCR_HANDLED && env_key(BTN_OK | BTN_LONG_FLAG) == SCR_HANDLED &&
          env_key(BTN_AUTO | BTN_LONG_FLAG) == SCR_HANDLED && env_key(BTN_PRM | BTN_LONG_FLAG) == SCR_HANDLED &&
          G(P(CH1_ENABLED)) == 1 && g_scope_keys == 0 && env_log_is(""));
    CHECK("the page is the visible layer: the groups shown are its own, the bars stay",
          screen_groups() == osc_menu_screen.groups && screen_bars_wanted());
}

/* ── opening, jumping, closing ─────────────────────────────────────── */

static void test_open_close(void)
{
    printf("open, deep links, close\n");
    boot();
    open_page(OSC_ROW_DISPLAY, false, BTN_MENU);
    CHECK("opened by MENU: the full page on Display, rail focus, opener MENU",
          osc_menu_is_open() && !osc_menu_is_locked() && S()->row == OSC_ROW_DISPLAY && !S()->in_pane &&
          osc_menu_screen_opener() == BTN_MENU && screen_top() == SCR_OSC_MENU);
    CHECK("CH2 jumps to its row and owns the close", env_key(BTN_CH2) == SCR_DRAW && S()->row == OSC_ROW_CH2 &&
          osc_menu_screen_opener() == BTN_CH2);
    g_after = 0;
    CHECK("CH2 again closes: the layer popped, a full repaint asked, the scope on top",
          env_key(BTN_CH2) == SCR_DRAW_FULL && !osc_menu_is_open() && screen_top() == SCR_SCOPE &&
          screen_snapshot().n == 1 && osc_menu_screen_opener() == BTN_NONE &&
          screen_last_handler() == SCR_OSC_MENU && g_after == 1 && g_after_res == SCR_DRAW_FULL);
    CHECK("after the close the scope gets the keys", (g_scope_keys = 0, env_key(BTN_UP) == SCR_HANDLED) &&
          g_scope_keys == 1);

    boot();
    open_page(OSC_ROW_CH1, false, BTN_CH1);
    (void)env_key(BTN_OK);
    CHECK("MENU in a pane goes back to the rail (the page stays)",
          env_key(BTN_MENU) == SCR_DRAW && osc_menu_is_open() && !S()->in_pane);
    CHECK("MENU on the rail closes", env_key(BTN_MENU) == SCR_DRAW_FULL && !osc_menu_is_open() &&
          screen_top() == SCR_SCOPE);

    boot();
    open_page(OSC_ROW_TRIGGER, true, BTN_TRIGGER);
    CHECK("opened by TRIGGER: locked in the Trigger pane",
          osc_menu_is_locked() && S()->row == OSC_ROW_TRIGGER && S()->in_pane);
    CHECK("CH1 does nothing there; RIGHT changes the row's property",
          env_key(BTN_CH1) == SCR_HANDLED && S()->row == OSC_ROW_TRIGGER &&
          env_key(BTN_RIGHT) == SCR_DRAW && G(P(TRIG_MODE)) == TRIG_NORMAL);
    CHECK("PRM moves the lock to Measure", env_key(BTN_PRM) == SCR_DRAW && S()->row == OSC_ROW_PARAM &&
          osc_menu_is_locked() && osc_menu_screen_opener() == BTN_PRM);
    CHECK("PRM again closes", env_key(BTN_PRM) == SCR_DRAW_FULL && !osc_menu_is_open() && screen_top() == SCR_SCOPE);
    open_page(OSC_ROW_TRIGGER, true, BTN_TRIGGER);
    CHECK("locked: MENU closes at once", env_key(BTN_MENU) == SCR_DRAW_FULL && !osc_menu_is_open());

    boot();
    open_page(OSC_ROW_MATH, false, BTN_MENU);
    (void)prop_set_local(P(SYS_MODE), MODE_MULTIMETER, ORIGIN_SHELL, NULL);
    CHECK("a mode change under it closes the page (it never reappears on the way back)",
          screen_top() == SCR_METER && !osc_menu_is_open() && osc_menu_screen_opener() == BTN_NONE);

    boot();
    open_page(OSC_ROW_MATH, false, BTN_MENU);
    CHECK("the page is up exactly while its layer is on the stack; a second open pushes nothing",
          screen_is_open(SCR_OSC_MENU) && screen_snapshot().n == 2 &&
          (osc_menu_screen_open(OSC_ROW_MATH, false, BTN_MENU), screen_snapshot().n == 2));
    (void)screen_close(SCR_OSC_MENU);                            /* the layer closed from outside */
    CHECK("its layer closed: the page's model is shut with it, the next key is the scope's",
          !osc_menu_is_open() && osc_menu_screen_opener() == BTN_NONE && screen_top() == SCR_SCOPE &&
          G(P(VIEW_MATH)) == 0);
}

/* ── the paints and `ui screen` ────────────────────────────────────── */

static void test_draw_describe(void)
{
    printf("the paints; describe\n");
    open_in(OSC_ROW_CH1);
    g_draws = g_invalidates = 0;
    osc_menu_screen.draw(SCR_PAINT_UPDATE, 0);
    CHECK("UPDATE: the page in place, nothing invalidated", g_draws == 1 && g_invalidates == 0);
    osc_menu_screen.draw(SCR_PAINT_FULL, 0);
    CHECK("FULL (the LCD was cleared): invalidated first", g_draws == 2 && g_invalidates == 1);
    osc_menu_screen.draw(SCR_PAINT_RESTORE, 0);
    CHECK("RESTORE (a popup box went): invalidated first", g_draws == 3 && g_invalidates == 2);

    char d[48];
    down(2);
    osc_menu_screen.describe(d, sizeof d);
    CHECK("describe: row=ch1 pane=pane item=2", strcmp(d, "row=ch1 pane=pane item=2") == 0);
    (void)env_key(BTN_MENU);
    osc_menu_screen.describe(d, sizeof d);
    CHECK("on the rail: row=ch1 pane=rail item=2", strcmp(d, "row=ch1 pane=rail item=2") == 0);
    boot();
    open_page(OSC_ROW_PARAM, true, BTN_PRM);
    osc_menu_screen.describe(d, sizeof d);
    CHECK("locked: row=param pane=locked item=3", strcmp(d, "row=param pane=locked item=3") == 0);
    static const char *const names[OSC_ROW_COUNT] = { "display", "cursor", "param", "ch1", "ch2", "trigger",
                                                       "persist", "math" };
    int ok = 1;
    for (int r = 0; r < OSC_ROW_COUNT; r++) {
        char w[48];
        boot();
        open_page((osc_row_t)r, false, BTN_MENU);
        osc_menu_screen.describe(d, sizeof d);
        snprintf(w, sizeof w, "row=%s pane=rail item=0", names[r]);
        ok &= strcmp(d, w) == 0;
    }
    CHECK("the eight row names are the ones the walk scripts read", ok);
    char line[96];
    boot();
    open_page(OSC_ROW_CH2, false, BTN_CH2);
    screen_snapshot_text(line, sizeof line);
    CHECK("the snapshot text: scope > osc_menu ... | row=ch2 pane=rail item=0",
          strncmp(line, "scope > osc_menu gen=", 21) == 0 && strstr(line, " | row=ch2 pane=rail item=0") != NULL);
}

int main(void)
{
    test_screen_entry();
    test_table();
    test_value_text();
    test_values_rows();
    test_radio_panes();
    test_rules_and_notes();
    test_measure();
    test_shell_set_and_idle();
    test_router_order();
    test_open_close();
    test_draw_describe();
    printf("\n%d/%d passed\n", passed, run);
    return passed == run ? 0 : 1;
}
