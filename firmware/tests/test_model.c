/*
 * Host test for the property model (src/model/): the table, lookup and glob,
 * parse/format with units, clamp/step/wrap, all-or-nothing multi-set
 * validation, epochs, read-only refusal, "NOT_SET keeps the old value" for
 * every hardware property, the save forms and the readable profile line,
 * built-in defaults vs current values, and instruments.
 *
 * The model runs on the real pure modules (scope_state, meas_select,
 * signal_gen with its DAC stubbed, sys_prefs, theme, key_model) and on fake
 * hardware ops that log every owner call and can be told to fail.
 *
 * Build/run:  make -C firmware test-model
 */
#define MODEL_INTERNAL
#include "model.h"
#include "scope_state.h"
#include "meas_select.h"
#include "signal_gen.h"
#include "sys_prefs.h"
#include "theme.h"
#include "fuse_model.h"
#include "math_channel.h"
#include "ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECKF(c, ...) do { checks++; if (!(c)) { fails++; \
    printf("FAIL %s:%d  %s: ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ═══ the globals main.c / input_handler.c own in the firmware ═══════ */
volatile device_mode_t  current_mode  = MODE_MULTIMETER;
volatile startup_mode_t startup_mode  = STARTUP_METER;
volatile uint8_t        active_channel = 0;
volatile uint8_t        meter_submode = 0;
volatile uint8_t        meter_layout  = 0;
volatile bool           math_enabled  = false;
volatile uint8_t        math_op       = 0;
volatile bool           persist_enabled = false;
volatile scope_view_t   scope_view    = SCOPE_VIEW_TIME;
volatile uint8_t        fuse_type     = 0;
volatile uint8_t        fuse_rating_idx = 6;
volatile uint8_t        fuse_view     = 0;
volatile float          fuse_scan_threshold_mv = 0.1f;
static uint8_t g_focus;
uint8_t key_focus_get(void) { return g_focus; }

/* signal_gen.c's output driver */
void dac_output_apply(const siggen_timing_t *t) { (void)t; }
void dac_output_stop(void) {}
static uint16_t g_dac_buf[SIGGEN_CYCLE_SAMPLES];
uint16_t *dac_output_get_buffer(void) { return g_dac_buf; }
uint32_t dac_output_timer_clock_hz(void) { return 240000000u; }

/* ═══ fake hardware ops ═════════════════════════════════════════════ */
static char     g_log[2048];
static bool     g_fail;                /* every bool op says no */
static bool     g_busy, g_shell_persist = true;
static unsigned g_notes;
static bool     g_il_en = true;
static int32_t  g_il_gain = 1000000, g_il_off = 0, g_il_skew = 0;
static uint8_t  g_il_knots;            /* piecewise knots in the level map (fpga.c) */
static uint8_t  g_tolv = 8, g_tolh = 2, g_pmode;
static bool     g_stopfail = true, g_keep_auto;

static unsigned g_fail_at, g_calls;   /* g_fail_at = n: the n-th bool owner call says no */
static bool nok(void) { return g_fail || (g_fail_at && ++g_calls == g_fail_at); }

static void logf_(const char *fmt, ...)
{
    char t[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    strncat(g_log, t, sizeof g_log - strlen(g_log) - 1);
}
static void log_clear(void) { g_log[0] = '\0'; }

static bool f_vdiv(uint8_t ch, uint8_t i)        { logf_("vdiv(%u,%u);", ch, i); return !nok(); }
static bool f_coupling(uint8_t ch, uint8_t c)    { logf_("coupling(%u,%u);", ch, c); return !nok(); }
static bool f_enables(void)                      { logf_("enables;"); return !nok(); }
static bool f_il_active(void)                    { return false; }
static bool f_vpos(uint8_t ch, int16_t p)        { logf_("vpos(%u,%d);", ch, p); return !nok(); }
static bool f_timebase(uint8_t c)                { logf_("tb(%u);", c); return !nok(); }
static bool f_roll(void)                         { logf_("roll;"); return !nok(); }
static bool f_trig_src(void)                     { logf_("trigsrc;"); return !nok(); }
static bool f_trig_lvl(int16_t l)                { logf_("triglvl(%d);", l); return !nok(); }
static void f_rearm(void)                        { logf_("rearm;"); }
static bool f_view(uint8_t v)                    { logf_("view(%u);", v); if (nok()) return false; scope_view = (scope_view_t)v; return true; }
static bool f_pers_on(uint8_t m)                 { logf_("pers_on(%u);", m); if (nok()) return false; persist_enabled = true; g_pmode = m; return true; }
static void f_pers_off(void)                     { logf_("pers_off;"); persist_enabled = false; }
static uint8_t f_pers_mode(void)                 { return g_pmode; }
static bool f_mask_present(void)                 { return true; }
static void f_mask_get(uint8_t *v, uint8_t *h, bool *s) { *v = g_tolv; *h = g_tolh; *s = g_stopfail; }
static bool f_mask_tol(uint8_t v, uint8_t h)     { logf_("mask_tol(%u,%u);", v, h); if (nok()) return false; g_tolv = v; g_tolh = h; return true; }
static bool f_mask_sf(bool on)                   { logf_("mask_sf(%d);", on); if (nok()) return false; g_stopfail = on; return true; }
static void f_gen_wave(uint8_t w)                { logf_("gen_wave(%u);", w); siggen_set_waveform(w); }
static void f_gen_freq(uint32_t f)               { logf_("gen_freq(%u);", (unsigned)f); siggen_set_frequency(f); }
static void f_gen_amp(uint8_t a)                 { logf_("gen_amp(%u);", a); siggen_set_amplitude_dv(a); }
static void f_gen_duty(percent_t d)              { logf_("gen_duty(%u);", d.pct); siggen_set_duty(d.pct); }
static void f_gen_out(bool on)                   { logf_("gen_out(%d);", on); siggen_enable(on); }
static void f_gen_boot(bool on)                  { logf_("gen_boot(%d);", on); siggen_set_start_on(on); }
static bool f_meter(uint8_t m, bool keep)        { logf_("meter(%u,%d);", m, keep); g_keep_auto = keep; return !nok(); }
static bool f_mode(uint8_t m)                    { logf_("mode(%u);", m); if (nok()) return false; current_mode = (device_mode_t)m; return true; }
static void f_backlight(uint8_t p)               { logf_("backlight(%u);", p); }
static void f_autooff(uint8_t m)                 { logf_("autooff(%u);", m); }
static bool f_il_en(bool on)                     { logf_("il_en(%d);", on); if (nok()) return false; g_il_en = on; return true; }
static bool f_il_enabled(void)                   { return g_il_en; }
/* like fpga_interleave_corr_set: the line replaces the map, knots and all */
static bool f_il_corr_set(gain_e6_t g, int32_t o){ logf_("il_corr(%ld,%ld);", (long)g.e6, (long)o); if (nok()) return false; g_il_gain = g.e6; g_il_off = o; g_il_knots = 0; return true; }
static uint8_t f_il_knots(void)                  { return g_il_knots; }
static void f_il_corr_get(gain_e6_t *g, int32_t *o) { g->e6 = g_il_gain; *o = g_il_off; }
static bool f_il_skew_set(ppm_period_t s)        { logf_("il_skew(%ld);", (long)s.ppm); if (nok()) return false; g_il_skew = s.ppm; return true; }
static ppm_period_t f_il_skew_get(void)          { ppm_period_t s = { g_il_skew }; return s; }
static bool f_busy(void)                         { return g_busy; }
static bool f_shell_persist(void)                { return g_shell_persist; }
static void f_note(void)                         { g_notes++; }

static const model_hw_ops_t k_fake = {
    .vdiv = f_vdiv, .coupling = f_coupling, .channel_enables = f_enables,
    .interleave_active = f_il_active, .vpos = f_vpos, .timebase = f_timebase, .roll = f_roll,
    .trigger_source = f_trig_src, .trigger_level = f_trig_lvl, .single_rearm = f_rearm,
    .view_select = f_view, .persist_on = f_pers_on, .persist_off = f_pers_off,
    .persist_mode = f_pers_mode, .mask_present = f_mask_present, .mask_get = f_mask_get,
    .mask_tol = f_mask_tol, .mask_stopfail = f_mask_sf,
    .gen_wave = f_gen_wave, .gen_freq = f_gen_freq, .gen_amp = f_gen_amp, .gen_duty = f_gen_duty,
    .gen_out = f_gen_out, .gen_bootout = f_gen_boot, .meter_func = f_meter, .mode = f_mode,
    .backlight = f_backlight, .autooff = f_autooff, .il_enable = f_il_en, .il_enabled = f_il_enabled,
    .il_corr_set = f_il_corr_set, .il_corr_get = f_il_corr_get, .il_knots = f_il_knots, .il_skew_set = f_il_skew_set,
    .il_skew_get = f_il_skew_get, .selfcal_busy = f_busy, .shell_persist = f_shell_persist,
    .note_change = f_note,
};

/* A device as a first boot leaves it, with the fake ops bound. */
static void fresh(void)
{
    scope_state_init(scope_state_get());
    meas_select_defaults();
    siggen_init(NULL);
    sys_prefs_set_brightness(SYS_BRIGHT_DEFAULT);
    sys_prefs_set_volume(SYS_VOL_DEFAULT);
    sys_prefs_set_autooff_mins(0);
    theme_init(THEME_DARK_BLUE);
    current_mode = MODE_MULTIMETER; startup_mode = STARTUP_METER; active_channel = 0;
    meter_submode = 0; meter_layout = 0; math_enabled = false; math_op = 0;
    persist_enabled = false; scope_view = SCOPE_VIEW_TIME; g_pmode = 0;
    fuse_type = 0; fuse_rating_idx = 6; fuse_scan_threshold_mv = 0.1f; g_focus = 0;
    g_fail = false; g_fail_at = 0; g_calls = 0; g_busy = false; g_shell_persist = true; g_notes = 0;
    g_il_en = true; g_il_gain = 1000000; g_il_off = 0; g_il_skew = 0; g_il_knots = 0;
    g_tolv = 8; g_tolh = 2; g_stopfail = true;
    model_bind_hw(&k_fake);
    /* the persisted defaults on top (config_init_defaults differs from
     * scope_state_init: CH1 vdiv 3 vs 8, timebase 10 vs 12) */
    (void)model_load_defaults(PF_PERSIST | PF_SESSION | PF_USERCAL, NULL, false);
    log_clear();
}

static const char *fmtv(const prop_desc_t *p, int32_t v)
{
    static char b[64];
    prop_format(p, v, b, sizeof b);
    return b;
}

/* ═══ the table ═════════════════════════════════════════════════════ */

static void report(const char *what, const prop_desc_t *p, void *ctx)
{
    (void)ctx;
    printf("  table: %s: %s\n", p ? p->name : "?", what);
}

static void test_table(void)
{
    CHECK(model_table_check(report, NULL) == 0);
    CHECK(prop_count() == PROP_IX_COUNT);
    CHECK(prop_count() >= 60);

    /* sym is the name in capitals with '.' -> '_' (the X-macro's two spellings) */
    static const struct { const char *sym, *name; } syms[] = {
#define PROP(sym, id, nm, ...) { #sym, nm },
#include "model_table.h"
#undef PROP
    };
    CHECK(sizeof syms / sizeof syms[0] == PROP_IX_COUNT);
    for (unsigned i = 0; i < PROP_IX_COUNT; i++) {
        char want[40];
        size_t k = 0;
        for (const char *c = syms[i].name; *c && k + 1 < sizeof want; c++)
            want[k++] = (*c == '.') ? '_' : (char)((*c >= 'a' && *c <= 'z') ? *c - 32 : *c);
        want[k] = '\0';
        CHECKF(strcmp(want, syms[i].sym) == 0, "%s vs %s", syms[i].sym, syms[i].name);
    }

    for (uint16_t i = 0; i < prop_count(); i++) {
        const prop_desc_t *p = prop_at(i);
        CHECK(prop_find(p->name) == p);
        CHECK(prop_by_id(p->id) == p);
        for (uint16_t j = i + 1; j < prop_count(); j++) {
            CHECK(strcmp(p->name, prop_at(j)->name) != 0);
            CHECK(p->id < prop_at(j)->id);
        }
        if (p->type == PT_ENUM) {                     /* every enum value has a label */
            for (int32_t v = p->min; v <= p->max; v++) {
                char t[24];
                int32_t back = -1;
                prop_format(p, v, t, sizeof t);
                CHECKF(t[0], "%s=%ld", p->name, (long)v);
                /* a label array's text reads back as its own value */
                if (p->labels && !p->label)
                    CHECKF(prop_parse(p, t, &back) == PS_OK && back == v, "%s=%ld '%s'", p->name, (long)v, t);
            }
            CHECKF(p->labels || p->label, "%s", p->name);
        }
        /* the RO ones and only them refuse */
        CHECK(((p->flags & PF_RO) != 0) == !prop_is_settable(p));
    }
    CHECK(P(CH1_VDIV)->id == PROP_ID_CH1_VDIV && PROP_ID_CH1_VDIV == 0x0102);
    CHECK(prop_find("ch1.nope") == NULL && prop_find(NULL) == NULL);
    CHECK(prop_by_id(0x0100) == NULL && prop_by_id(0xFFFF) == NULL);
    CHECK(prop_at(prop_count()) == NULL);

    /* the list the plan names, all present (2.4) */
    static const char *const must[] = {
        "ch1.enabled", "ch1.vdiv", "ch1.coupling", "ch1.probe", "ch1.bwlimit", "ch1.pos", "ch1.meas",
        "ch2.enabled", "ch2.vdiv", "ch2.coupling", "ch2.probe", "ch2.bwlimit", "ch2.pos", "ch2.meas",
        "tb.code", "tb.roll", "tb.rollstep", "trig.mode", "trig.edge", "trig.source", "trig.level",
        "trig.x", "trig.soft", "view.mode", "view.persist", "view.math", "view.cursor",
        "view.truescale", "view.meashide", "view.run", "gen.wave", "gen.freq", "gen.amp",
        "gen.duty", "gen.out", "gen.bootout", "meter.func", "meter.layout", "sys.mode",
        "sys.instrument", "sys.startup", "sys.brightness", "sys.volume", "sys.autooff",
        "sys.theme", "sys.lang", "fft.window", "fft.avg", "fft.maxhold", "il.enabled",
        "il.gain", "il.offset", "il.skew", "ui.focus", "ui.ach", "ui.screen",
    };
    for (unsigned i = 0; i < sizeof must / sizeof must[0]; i++)
        CHECKF(prop_find(must[i]) != NULL, "%s", must[i]);

    /* ranges that mirror a module's constant at run time (floats) */
    CHECK(P(METER_FUSETHRESH)->min == (int32_t)(FUSE_SCAN_THRESH_MIN_MV * 10.0f + 0.5f));
    CHECK(P(METER_FUSETHRESH)->max == (int32_t)(FUSE_SCAN_THRESH_MAX_MV * 10.0f + 0.5f));
}

static void test_glob(void)
{
    uint16_t i = 0, n = 0;
    while (prop_glob("ch1.*", &i)) { n++; CHECK(strncmp(prop_at((uint16_t)(i - 1))->name, "ch1.", 4) == 0); }
    CHECK(n == 7);
    i = 0; n = 0;
    while (prop_glob("*.enabled", &i)) n++;
    CHECK(n == 3);                                  /* ch1, ch2, il */
    i = 0; n = 0;
    while (prop_glob("*", &i)) n++;
    CHECK(n == prop_count());
    CHECK(prop_glob_match("t*.m*e", "trig.mode"));
    CHECK(!prop_glob_match("tb.*", "trig.mode"));
    CHECK(prop_glob_match("gen.amp", "gen.amp") && !prop_glob_match("gen.am", "gen.amp"));
}

/* ═══ parse / format ════════════════════════════════════════════════ */

static void test_parse_format(void)
{
    fresh();
    int32_t v = -99;
    CHECK(prop_parse(P(CH1_COUPLING), "AC", &v) == PS_OK && v == 1);
    CHECK(prop_parse(P(CH1_COUPLING), "ac", &v) == PS_OK && v == 1);   /* case-insensitive */
    CHECK(prop_parse(P(CH1_COUPLING), "GND", &v) == PS_INVALID);       /* outside the range: no label */
    CHECK(prop_parse(P(CH1_VDIV), "3", &v) == PS_OK && v == 3);
    CHECK(prop_parse(P(GEN_AMP), "1.5", &v) == PS_OK && v == 15);
    CHECK(prop_parse(P(GEN_AMP), "1.5 V", &v) == PS_OK && v == 15);
    CHECK(prop_parse(P(GEN_AMP), "1.5V", &v) == PS_OK && v == 15);
    CHECK(prop_parse(P(GEN_FREQ), "1000Hz", &v) == PS_OK && v == 1000);
    v = -99;
    CHECK(prop_parse(P(GEN_AMP), "1.55", &v) == PS_INVALID && v == -99);  /* no silent rounding */
    CHECK(prop_parse(P(GEN_AMP), "1.5 Hz", &v) == PS_INVALID);
    CHECK(prop_parse(P(CH1_VDIV), "+1", &v) == PS_OK && v == 4);         /* 3 + 1 */
    CHECK(prop_parse(P(CH1_VDIV), "-1", &v) == PS_OK && v == 2);
    CHECK(prop_parse(P(CH1_VDIV), "+=2", &v) == PS_OK && v == 5);
    CHECK(prop_parse(P(CH1_VDIV), "+20", &v) == PS_OK && v == 9);        /* saturates */
    CHECK(prop_parse(P(CH1_COUPLING), "+1", &v) == PS_OK && v == 1);     /* DC -> AC */
    CHECK(prop_set_local(P(CH1_COUPLING), 1, ORIGIN_SHELL, NULL) == PS_OK);
    CHECK(prop_parse(P(CH1_COUPLING), "+1", &v) == PS_OK && v == 0);     /* wraps */
    CHECK(prop_parse(P(TRIG_LEVEL), "-5", &v) == PS_OK && v == -5);      /* signed: absolute */
    CHECK(prop_parse(P(TRIG_LEVEL), "+=5", &v) == PS_OK && v == 5);      /* relative spelled out */
    CHECK(prop_parse(P(IL_OFFSET), "-0.05", &v) == PS_OK && v == -5);
    CHECK(prop_parse(P(IL_GAIN), "1.0025", &v) == PS_OK && v == 1002500);
    CHECK(prop_parse(P(VIEW_MATH), "-A", &v) == PS_OK && v == 5);        /* a label, not a step */
    CHECK(prop_parse(P(VIEW_MATH), "off", &v) == PS_OK && v == 0);
    CHECK(prop_parse(P(CH1_ENABLED), "on", &v) == PS_OK && v == 1);
    CHECK(prop_parse(P(CH1_ENABLED), "OFF", &v) == PS_OK && v == 0);
    CHECK(prop_parse(P(TRIG_MODE), "single", &v) == PS_OK && v == 2);
    CHECK(prop_parse(P(CH1_PROBE), "10x", &v) == PS_OK && v == 1);
    CHECK(prop_parse(P(SYS_AUTOOFF), "30min", &v) == PS_OK && v == 2);
    CHECK(prop_parse(P(CH1_MEAS), "Freq,Vp-p", &v) == PS_OK && v == ((1 << 0) | (1 << 8)));
    CHECK(prop_parse(P(CH1_MEAS), "0x507", &v) == PS_OK && v == 0x507);
    CHECK(prop_parse(P(CH1_MEAS), "none", &v) == PS_OK && v == 0);
    CHECK(prop_parse(P(CH1_MEAS), "all", &v) == PS_OK && v == 0x0FFF);
    CHECK(prop_parse(P(CH1_MEAS), "Freq,Bogus", &v) == PS_INVALID);
    CHECK(prop_parse(P(CH1_MEAS), "+1", &v) == PS_INVALID);
    /* bad text */
    CHECK(prop_parse(P(CH1_COUPLING), "XYZ", &v) == PS_INVALID);
    CHECK(prop_parse(P(CH1_VDIV), "", &v) == PS_INVALID);
    CHECK(prop_parse(P(CH1_VDIV), "3x", &v) == PS_INVALID);
    CHECK(prop_parse(P(GEN_FREQ), "abc", &v) == PS_INVALID);
    CHECK(prop_parse(P(CH1_ENABLED), "maybe", &v) == PS_INVALID);
    CHECK(prop_parse(NULL, "1", &v) == PS_INVALID);

    CHECK(strcmp(fmtv(P(CH1_COUPLING), 1), "AC") == 0);
    CHECK(strcmp(fmtv(P(CH1_ENABLED), 1), "on") == 0);
    CHECK(strcmp(fmtv(P(GEN_AMP), 15), "1.5 V") == 0);
    CHECK(strcmp(fmtv(P(GEN_FREQ), 1000), "1000 Hz") == 0);
    CHECK(strcmp(fmtv(P(IL_OFFSET), -60), "-0.60 counts") == 0);         /* the sign survives */
    CHECK(strcmp(fmtv(P(CH1_MEAS), 0x101), "Freq,Vp-p") == 0);
    CHECK(strcmp(fmtv(P(CH1_MEAS), 0), "none") == 0);
    CHECK(strcmp(fmtv(P(SYS_INSTRUMENT), 2), "gen") == 0);
    CHECK(strcmp(fmtv(P(UI_FOCUS), 2), "trigmove") == 0);
    CHECK(strcmp(fmtv(P(METER_FUSERATING), 6), "10A") == 0);              /* ATO table, index 6 */
    char t[16];
    prop_format_value(P(CH1_VDIV), 3, t, sizeof t);
    CHECK(strcmp(t, "3") == 0);                                          /* measured label: index */
}

/* ═══ bounds, step, wrap ════════════════════════════════════════════ */

static void test_bounds_step(void)
{
    fresh();
    prop_note_t n;
    CHECK(prop_set_local(P(CH1_VDIV), 10, ORIGIN_SHELL, &n) == PS_INVALID);
    CHECK(prop_get(P(CH1_VDIV)) == 3 && g_log[0] == '\0');               /* nothing touched */
    CHECK(prop_set_local(P(CH1_VDIV), -1, ORIGIN_SHELL, &n) == PS_INVALID);
    CHECK(prop_set_local(P(CH1_COUPLING), COUPLING_GND, ORIGIN_SHELL, &n) == PS_INVALID);
    CHECK(prop_set_local(P(CH1_ENABLED), 2, ORIGIN_SHELL, &n) == PS_INVALID);
    CHECK(prop_set_local(P(CH1_MEAS), 0x1000, ORIGIN_SHELL, &n) == PS_INVALID);
    CHECK(prop_set_local(P(GEN_FREQ), 60000, ORIGIN_SHELL, &n) == PS_CLAMPED);
    CHECK(prop_get(P(GEN_FREQ)) == 50000);
    CHECK(prop_set_local(P(SYS_BRIGHTNESS), 10, ORIGIN_SHELL, &n) == PS_CLAMPED);
    CHECK(prop_get(P(SYS_BRIGHTNESS)) == 25);
    CHECK(prop_set_local(P(TRIG_LEVEL), -500, ORIGIN_SHELL, &n) == PS_CLAMPED);
    CHECK(prop_get(P(TRIG_LEVEL)) == -100);

    /* saturate: vdiv stops at 9, and at the limit nothing is called */
    CHECK(prop_set_local(P(CH1_VDIV), 8, ORIGIN_KEY, &n) == PS_OK);
    CHECK(prop_step_local(P(CH1_VDIV), +1, ORIGIN_KEY, &n) == PS_OK && prop_get(P(CH1_VDIV)) == 9);
    log_clear();
    CHECK(prop_step_local(P(CH1_VDIV), +1, ORIGIN_KEY, &n) == PS_OK && prop_get(P(CH1_VDIV)) == 9);
    CHECK(g_log[0] == '\0');
    CHECK(prop_step_local(P(CH1_VDIV), -1, ORIGIN_KEY, &n) == PS_OK && prop_get(P(CH1_VDIV)) == 8);
    CHECK(strcmp(g_log, "vdiv(0,8);") == 0);
    /* wrap: trig.mode Single -> Auto, coupling AC -> DC */
    CHECK(prop_set_local(P(TRIG_MODE), TRIG_SINGLE, ORIGIN_KEY, &n) == PS_OK);
    CHECK(strcmp(n.text, "Await Trigger...") == 0);
    CHECK(prop_step_local(P(TRIG_MODE), +1, ORIGIN_KEY, &n) == PS_OK && prop_get(P(TRIG_MODE)) == TRIG_AUTO);
    CHECK(prop_step_local(P(TRIG_MODE), -1, ORIGIN_KEY, &n) == PS_OK && prop_get(P(TRIG_MODE)) == TRIG_SINGLE);
    /* int steps: trig.level by 5, saturating at 100 */
    CHECK(prop_set_local(P(TRIG_LEVEL), 98, ORIGIN_KEY, &n) == PS_OK);
    CHECK(prop_step_local(P(TRIG_LEVEL), +1, ORIGIN_KEY, &n) == PS_OK && prop_get(P(TRIG_LEVEL)) == 100);
    CHECK(prop_step_local(P(SYS_BRIGHTNESS), -1, ORIGIN_KEY, &n) == PS_OK && prop_get(P(SYS_BRIGHTNESS)) == 25);
    CHECK(prop_set_local(P(SYS_BRIGHTNESS), 100, ORIGIN_KEY, &n) == PS_OK);
    CHECK(prop_step_local(P(SYS_BRIGHTNESS), -1, ORIGIN_KEY, &n) == PS_OK && prop_get(P(SYS_BRIGHTNESS)) == 95);
    /* bools: with PF_WRAP any step toggles */
    CHECK(prop_step_local(P(CH1_BWLIMIT), +1, ORIGIN_KEY, &n) == PS_OK && prop_get(P(CH1_BWLIMIT)) == 1);
    CHECK(prop_step_local(P(CH1_BWLIMIT), +1, ORIGIN_KEY, &n) == PS_OK && prop_get(P(CH1_BWLIMIT)) == 0);
    /* a mask has no step */
    CHECK(prop_step_local(P(CH1_MEAS), +1, ORIGIN_KEY, &n) == PS_INVALID);
    CHECK(prop_step_local(P(CH1_VDIV), 0, ORIGIN_KEY, &n) == PS_OK);
}

/* ═══ multi-set: validated together, applied in order ═══════════════ */

static void test_pairs(void)
{
    fresh();
    uint8_t failed = 0xAA;
    prop_note_t n;
    const uint32_t e = prop_group_epoch(PG_CH1);
    prop_pair_t bad[] = { { PROP_ID_CH1_VDIV, 4 }, { PROP_ID_CH1_COUPLING, COUPLING_GND } };
    CHECK(prop_set_pairs_local(bad, 2, ORIGIN_SHELL, &n, &failed) == PS_INVALID && failed == 1);
    CHECK(prop_get(P(CH1_VDIV)) == 3 && g_log[0] == '\0');      /* none applied */
    CHECK(prop_group_epoch(PG_CH1) == e);

    prop_pair_t ro[] = { { PROP_ID_CH1_VDIV, 4 }, { PROP_ID_UI_FOCUS, 1 } };
    CHECK(prop_validate_pairs(ro, 2, &failed) == PS_INVALID && failed == 1);
    prop_pair_t unknown[] = { { 0x01FF, 1 } };
    CHECK(prop_validate_pairs(unknown, 1, &failed) == PS_INVALID && failed == 0);

    prop_pair_t hw5[] = { { PROP_ID_CH1_VDIV, 4 }, { PROP_ID_CH2_VDIV, 4 }, { PROP_ID_TB_CODE, 9 },
                          { PROP_ID_TRIG_LEVEL, 10 }, { PROP_ID_CH1_COUPLING, 1 } };
    CHECK(prop_validate_pairs(hw5, 5, &failed) == PS_INVALID && failed == 5);   /* > 4 PF_HW */
    prop_pair_t nine[9];
    for (int i = 0; i < 9; i++) { nine[i].id = PROP_ID_CH1_PROBE; nine[i].value = 1; }
    CHECK(prop_validate_pairs(nine, 9, &failed) == PS_INVALID && failed == 9);  /* > 8 pairs */
    CHECK(prop_validate_pairs(nine, 8, &failed) == PS_OK);

    prop_pair_t ok[] = { { PROP_ID_CH1_VDIV, 4 }, { PROP_ID_CH1_PROBE, 1 }, { PROP_ID_GEN_FREQ, 99999 } };
    CHECK(prop_set_pairs_local(ok, 3, ORIGIN_SHELL, &n, &failed) == PS_CLAMPED && failed == 3);
    CHECK(prop_get(P(CH1_VDIV)) == 4 && prop_get(P(CH1_PROBE)) == 1 && prop_get(P(GEN_FREQ)) == 50000);

    /* a hardware failure mid-way stops there and reports the index; earlier pairs stay */
    prop_pair_t mid[] = { { PROP_ID_CH1_PROBE, 0 }, { PROP_ID_CH1_VDIV, 6 }, { PROP_ID_CH2_PROBE, 1 } };
    g_fail = true;
    CHECK(prop_set_pairs_local(mid, 3, ORIGIN_SHELL, &n, &failed) == PS_NOT_SET && failed == 1);
    g_fail = false;
    CHECK(prop_get(P(CH1_PROBE)) == 0 && prop_get(P(CH1_VDIV)) == 4 && prop_get(P(CH2_PROBE)) == 0);
}

/* ═══ epochs and the settings note ══════════════════════════════════ */

static void test_epochs_notes(void)
{
    fresh();
    prop_note_t n;
    uint32_t e = prop_group_epoch(PG_CH1);
    CHECK(prop_set_local(P(CH1_VDIV), 3, ORIGIN_KEY, &n) == PS_OK);     /* same value */
    CHECK(prop_group_epoch(PG_CH1) == e && g_notes == 0);
    CHECK(prop_set_local(P(CH1_VDIV), 4, ORIGIN_KEY, &n) == PS_OK);
    CHECK(prop_group_epoch(PG_CH1) == e + 1 && g_notes == 1);
    CHECK(prop_group_epoch(PG_CH2) == prop_epoch[PG_CH2]);
    /* a session property bumps its epoch but is not a settings change */
    e = prop_group_epoch(PG_TRIG);
    CHECK(prop_set_local(P(TRIG_X), 166, ORIGIN_KEY, &n) == PS_OK);
    CHECK(prop_group_epoch(PG_TRIG) == e + 1 && g_notes == 1);
    /* shell_persist off: shell sets are not noted, keys still are */
    g_shell_persist = false;
    CHECK(prop_set_local(P(CH1_VDIV), 5, ORIGIN_SHELL, &n) == PS_OK && g_notes == 1);
    CHECK(prop_set_local(P(CH1_VDIV), 6, ORIGIN_KEY, &n) == PS_OK && g_notes == 2);
    CHECK(prop_set_local(P(CH1_VDIV), 7, ORIGIN_BOOT, &n) == PS_OK && g_notes == 2);
    g_shell_persist = true;
    /* a failed apply bumps nothing */
    e = prop_group_epoch(PG_CH1);
    g_fail = true;
    CHECK(prop_set_local(P(CH1_VDIV), 2, ORIGIN_KEY, &n) == PS_NOT_SET);
    g_fail = false;
    CHECK(prop_group_epoch(PG_CH1) == e && g_notes == 2 && prop_get(P(CH1_VDIV)) == 7);
    CHECK(strcmp(n.text, "relay apply failed") == 0);
}

/* ═══ read only, busy, rules ════════════════════════════════════════ */

static void test_rules(void)
{
    fresh();
    prop_note_t n;
    g_focus = 2;
    CHECK(prop_get(P(UI_FOCUS)) == 2);
    CHECK(prop_set_local(P(UI_FOCUS), 0, ORIGIN_SHELL, &n) == PS_INVALID && prop_get(P(UI_FOCUS)) == 2);
    CHECK(prop_step_local(P(UI_ACH), +1, ORIGIN_SHELL, &n) == PS_INVALID);
    CHECK(prop_set_local(P(UI_SCREEN), 0, ORIGIN_SHELL, &n) == PS_INVALID);
    CHECK(prop_set_local(NULL, 0, ORIGIN_SHELL, &n) == PS_INVALID);

    /* self-cal: scope sets from keys and shell are BUSY, its own (AUTO) are not */
    g_busy = true;
    CHECK(prop_set_local(P(CH1_VDIV), 4, ORIGIN_KEY, &n) == PS_BUSY && prop_get(P(CH1_VDIV)) == 3);
    CHECK(prop_set_local(P(TB_CODE), 9, ORIGIN_SHELL, &n) == PS_BUSY);
    CHECK(prop_set_local(P(CH1_VDIV), 4, ORIGIN_AUTO, &n) == PS_OK && prop_get(P(CH1_VDIV)) == 4);
    CHECK(prop_set_local(P(GEN_FREQ), 2000, ORIGIN_SHELL, &n) == PS_OK);   /* not a scope property */
    g_busy = false;

    /* "Keep one CH on": refused, CH2 stays on; the arrows follow the live channel */
    CHECK(prop_set_local(P(CH1_ENABLED), 0, ORIGIN_KEY, &n) == PS_OK && active_channel == 1);
    g_fail = true;
    CHECK(prop_set_local(P(CH2_ENABLED), 0, ORIGIN_KEY, &n) == PS_REFUSED);
    g_fail = false;
    CHECK(prop_get(P(CH2_ENABLED)) == 1 && strcmp(n.text, "Keep one CH on") == 0);
    /* a trigger source on a channel that is off is refused, nothing written */
    log_clear();
    CHECK(prop_set_local(P(TRIG_SOURCE), TRIG_SRC_CH1, ORIGIN_KEY, &n) == PS_REFUSED);
    CHECK(strstr(g_log, "trigsrc") == NULL && strcmp(n.text, "CH1 is off") == 0);
    CHECK(prop_set_local(P(TRIG_SOURCE), TRIG_SRC_CH2, ORIGIN_KEY, &n) == PS_OK);
    CHECK(strcmp(g_log, "trigsrc;") == 0);

    /* view.math off keeps math_op (the Math pane; the record keeps both) */
    CHECK(prop_set_local(P(VIEW_MATH), 1 + MATH_MUL, ORIGIN_KEY, &n) == PS_OK);
    CHECK(math_enabled && math_op == MATH_MUL);
    CHECK(prop_set_local(P(VIEW_MATH), 0, ORIGIN_KEY, &n) == PS_OK);
    CHECK(!math_enabled && math_op == MATH_MUL && prop_get(P(VIEW_MATH)) == 0);

    /* view.mode: split needs Roll off first, then the view */
    CHECK(prop_set_local(P(VIEW_MODE), 1, ORIGIN_KEY, &n) == PS_OK && scope_state_get()->roll);
    log_clear();
    CHECK(prop_set_local(P(VIEW_MODE), 4, ORIGIN_KEY, &n) == PS_OK);
    CHECK(strcmp(g_log, "roll;view(2);") == 0);
    CHECK(!scope_state_get()->roll && scope_view == SCOPE_VIEW_SPLIT && prop_get(P(VIEW_MODE)) == 4);

    /* meter.func from meter_auto keeps the autoselect running */
    CHECK(prop_set_local(P(METER_FUNC), 3, ORIGIN_AUTO, &n) == PS_OK && g_keep_auto);
    CHECK(prop_set_local(P(METER_FUNC), 4, ORIGIN_KEY, &n) == PS_OK && !g_keep_auto);
    CHECK(meter_submode == 4 && strcmp(n.text, "posture posted") == 0);

    /* fuse type: a set keeps the rating in amps, like the TYPE keys */
    {
        const uint8_t amps0 = fuse_tables[0].entries[6].rating_amps;          /* ATO idx 6 */
        CHECK(fuse_type == 0 && fuse_rating_idx == 6 && amps0 == 10);
        CHECK(prop_set_local(P(METER_FUSETYPE), 2, ORIGIN_SHELL, &n) == PS_OK);  /* micro2 */
        CHECKF(fuse_tables[2].entries[fuse_rating_idx].rating_amps == 10, "idx %u", fuse_rating_idx);
        CHECK(fuse_rating_idx == 3);
        CHECK(prop_set_local(P(METER_FUSETYPE), 0, ORIGIN_SHELL, &n) == PS_OK);  /* back: 10 A */
        CHECK(fuse_rating_idx == 6);
        CHECK(prop_set_local(P(METER_FUSETYPE), 3, ORIGIN_SHELL, &n) == PS_OK);  /* maxi: none <= 10 A */
        CHECK(fuse_rating_idx == 0 && fuse_tables[3].entries[0].rating_amps == 20);
        CHECK(prop_set_local(P(METER_FUSERATING), 6, ORIGIN_KEY, &n) == PS_OK);  /* 60 A */
        CHECK(prop_set_local(P(METER_FUSETYPE), 4, ORIGIN_SHELL, &n) == PS_OK);  /* jcase: 60 A */
        CHECK(fuse_tables[4].entries[fuse_rating_idx].rating_amps == 60);
        CHECK(prop_set_local(P(METER_FUSETYPE), 1, ORIGIN_SHELL, &n) == PS_OK);  /* mini: 30 A, the top */
        CHECK(fuse_tables[1].entries[fuse_rating_idx].rating_amps == 30);
        CHECK(prop_set_local(P(METER_FUSETYPE), 0, ORIGIN_SHELL, &n) == PS_OK);
        CHECK(prop_set_local(P(METER_FUSERATING), 6, ORIGIN_SHELL, &n) == PS_OK);
    }

    /* fuse rating: a shorter table clamps */
    CHECK(prop_set_local(P(METER_FUSETYPE), 4, ORIGIN_KEY, &n) == PS_OK);      /* J-Case: 7 */
    CHECK(prop_set_local(P(METER_FUSERATING), 12, ORIGIN_KEY, &n) == PS_CLAMPED);
    CHECK(prop_get(P(METER_FUSERATING)) == 6);

    /* sys.instrument and its alias sys.mode share current_mode */
    CHECK(prop_get(P(SYS_INSTRUMENT)) == 1 && prop_get(P(SYS_MODE)) == MODE_MULTIMETER);
    CHECK(prop_set_local(P(SYS_INSTRUMENT), 0, ORIGIN_SHELL, &n) == PS_OK && current_mode == MODE_OSCILLOSCOPE);
    CHECK(strcmp(n.text, "posted") == 0 && prop_get(P(SYS_MODE)) == 0);
    CHECK(prop_set_local(P(SYS_MODE), MODE_HOME, ORIGIN_SHELL, &n) == PS_OK);
    CHECK(prop_get(P(SYS_INSTRUMENT)) == 3 && strcmp(fmtv(P(SYS_INSTRUMENT), 3), "none") == 0);
    CHECK(prop_set_local(P(SYS_INSTRUMENT), 3, ORIGIN_SHELL, &n) == PS_REFUSED);
    CHECK(current_mode == MODE_HOME);
    CHECK(prop_get(P(UI_SCREEN)) == MODE_HOME);

    /* il.gain and il.offset are one correction pair at the owner */
    CHECK(prop_set_local(P(IL_OFFSET), -60, ORIGIN_SHELL, &n) == PS_OK);
    CHECK(prop_set_local(P(IL_GAIN), 1002500, ORIGIN_SHELL, &n) == PS_OK);
    CHECK(g_il_gain == 1002500 && g_il_off == -60);

    /* A piecewise map is active: the same value is the identity (no owner
     * call, the knots stay), a new one is refused, never a silent line. */
    g_il_knots = 5;
    log_clear();
    CHECK(prop_set_local(P(IL_GAIN), 1002500, ORIGIN_SHELL, &n) == PS_OK);
    CHECK(prop_set_local(P(IL_OFFSET), -60, ORIGIN_SHELL, &n) == PS_OK);
    CHECKF(g_log[0] == '\0' && g_il_knots == 5, "%s knots %u", g_log, g_il_knots);
    P(IL_GAIN)->store(P(IL_GAIN), prop_get(P(IL_GAIN)));      /* the load hook too */
    P(IL_OFFSET)->store(P(IL_OFFSET), prop_get(P(IL_OFFSET)));
    CHECK(g_log[0] == '\0' && g_il_knots == 5);
    {   /* get then set, through the readable profile line */
        char line[PROP_LINE_MAX];
        const prop_desc_t *q = NULL;
        int32_t v = 0;
        prop_line_format(P(IL_GAIN), prop_get(P(IL_GAIN)), line, sizeof line);
        CHECK(prop_line_parse(line, &q, &v) == PS_OK && q == P(IL_GAIN));
        CHECK(prop_set_local(q, v, ORIGIN_SHELL, &n) == PS_OK && g_il_knots == 5 && g_log[0] == '\0');
    }
    CHECK(prop_set_local(P(IL_GAIN), 1003000, ORIGIN_SHELL, &n) == PS_REFUSED);
    CHECK(strcmp(n.text, "piecewise cal: clear it") == 0);
    CHECK(prop_set_local(P(IL_OFFSET), -50, ORIGIN_SHELL, &n) == PS_REFUSED);
    P(IL_GAIN)->store(P(IL_GAIN), 1003000);
    CHECK(g_il_gain == 1002500 && g_il_off == -60 && g_il_knots == 5 && g_log[0] == '\0');
    g_il_knots = 0;                    /* linear again: a change goes through */
    CHECK(prop_set_local(P(IL_GAIN), 1003000, ORIGIN_SHELL, &n) == PS_OK && g_il_gain == 1003000);
}

/* ═══ every hardware property: a failed owner call keeps the old value ═ */

static bool cannot_fail(const prop_desc_t *p)
{
    /* owners whose call is void (signal_gen.c, sys_pwm, the idle timer, the
     * SINGLE re-arm), and ch.pos, which moves the ground marker even when
     * the offset DAC cannot follow (today's behaviour, noted). */
    static const uint16_t ids[] = {
        PROP_ID_GEN_WAVE, PROP_ID_GEN_FREQ, PROP_ID_GEN_AMP, PROP_ID_GEN_DUTY, PROP_ID_GEN_OUT,
        PROP_ID_GEN_BOOTOUT, PROP_ID_SYS_BRIGHTNESS, PROP_ID_SYS_AUTOOFF, PROP_ID_TRIG_MODE,
        PROP_ID_CH1_POS, PROP_ID_CH2_POS,
    };
    for (unsigned i = 0; i < sizeof ids / sizeof ids[0]; i++) if (ids[i] == p->id) return true;
    return false;
}

static void test_not_set_keeps_old(void)
{
    unsigned tried = 0;
    for (uint16_t i = 0; i < prop_count(); i++) {
        const prop_desc_t *p = prop_at(i);
        if (!(p->flags & PF_HW) || !prop_is_settable(p)) continue;
        fresh();
        if (p->id == PROP_ID_TB_ROLLSTEP) (void)prop_set_local(P(TB_ROLL), 1, ORIGIN_KEY, NULL);
        const int32_t old = prop_get(p);
        int32_t want = old + (int32_t)p->step;
        if (want > p->max) want = old - (int32_t)p->step;
        if (p->id == PROP_ID_SYS_INSTRUMENT) want = 0;
        if (p->id == PROP_ID_SYS_MODE)       want = MODE_SETTINGS;
        const uint32_t e = prop_group_epoch(p->group);
        const unsigned notes = g_notes;
        prop_note_t n;
        g_fail = true;
        const prop_status_t s = prop_set_local(p, want, ORIGIN_KEY, &n);
        g_fail = false;
        tried++;
        if (cannot_fail(p)) {
            CHECKF(s == PS_OK && prop_get(p) == want, "%s: %s", p->name, prop_status_name(s));
            continue;
        }
        CHECKF(s == PS_NOT_SET || s == PS_REFUSED, "%s: %s", p->name, prop_status_name(s));
        CHECKF(prop_get(p) == old, "%s: %ld -> %ld", p->name, (long)old, (long)prop_get(p));
        CHECKF(prop_group_epoch(p->group) == e && g_notes == notes, "%s", p->name);
        CHECKF(n.text[0] != '\0', "%s: no note", p->name);
    }
    CHECK(tried >= 30);
}

/* Each owner call failing in turn (the first, the second, ...): a setter
 * with several calls must undo the ones before, so a non-OK status always
 * leaves the old value and the epoch alone. */
static void test_fail_each_call(void)
{
    unsigned cases = 0;
    for (uint16_t i = 0; i < prop_count(); i++) {
        const prop_desc_t *p = prop_at(i);
        if (!(p->flags & PF_HW) || !prop_is_settable(p)) continue;
        for (int32_t want = p->min; want <= p->max && want <= p->min + 6; want++) {
            for (unsigned k = 1; k <= 4; k++) {
                fresh();
                const int32_t old = prop_get(p);
                if (want == old) continue;
                const uint32_t e = prop_group_epoch(p->group);
                g_fail_at = k;
                const prop_status_t s = prop_set_local(p, want, ORIGIN_KEY, NULL);
                g_fail_at = 0;
                cases++;
                if (s == PS_OK || s == PS_CLAMPED) continue;
                CHECKF(prop_get(p) == old && prop_group_epoch(p->group) == e,
                       "%s %ld->%ld, call %u failed: now %ld", p->name, (long)old, (long)want, k,
                       (long)prop_get(p));
            }
        }
    }
    CHECK(cases > 200);

    /* the two-step view changes, by hand: fft -> roll with Roll refused,
     * roll -> split with the view refused */
    prop_note_t n;
    fresh();
    CHECK(prop_set_local(P(VIEW_MODE), 3, ORIGIN_KEY, &n) == PS_OK && scope_view == SCOPE_VIEW_FFT);
    g_fail_at = 2; g_calls = 0;                       /* view(0) ok, roll refused */
    CHECK(prop_set_local(P(VIEW_MODE), 1, ORIGIN_KEY, &n) == PS_NOT_SET);
    g_fail_at = 0;
    CHECK(prop_get(P(VIEW_MODE)) == 3 && scope_view == SCOPE_VIEW_FFT && !scope_state_get()->roll);
    fresh();
    CHECK(prop_set_local(P(VIEW_MODE), 1, ORIGIN_KEY, &n) == PS_OK && scope_state_get()->roll);
    log_clear();
    g_fail_at = 2; g_calls = 0;                       /* roll off ok, view refused */
    CHECK(prop_set_local(P(VIEW_MODE), 4, ORIGIN_KEY, &n) == PS_NOT_SET);
    g_fail_at = 0;
    CHECK(prop_get(P(VIEW_MODE)) == 1 && scope_state_get()->roll && scope_view == SCOPE_VIEW_TIME);
    CHECK(strcmp(g_log, "roll;view(2);roll;") == 0);
}

/* A setter's change in another group bumps that group too. */
static void test_cross_group(void)
{
    fresh();
    uint32_t e = prop_group_epoch(PG_UI);
    CHECK(prop_set_local(P(CH1_ENABLED), 0, ORIGIN_KEY, NULL) == PS_OK && active_channel == 1);
    CHECK(prop_group_epoch(PG_UI) == e + 1);                      /* ui.ach moved */
    scope_state_get()->running = false;
    e = prop_group_epoch(PG_VIEW);
    CHECK(prop_set_local(P(TRIG_SOURCE), TRIG_SRC_CH2, ORIGIN_KEY, NULL) == PS_OK);
    CHECK(prop_set_local(P(TRIG_MODE), TRIG_SINGLE, ORIGIN_KEY, NULL) == PS_OK);
    CHECK(prop_group_epoch(PG_VIEW) == e + 1 && prop_get(P(VIEW_RUN)) == 1);   /* view.run */
    e = prop_group_epoch(PG_VIEW);
    uint32_t t = prop_group_epoch(PG_TB);
    CHECK(prop_set_local(P(TB_ROLL), 1, ORIGIN_KEY, NULL) == PS_OK);
    CHECK(prop_group_epoch(PG_VIEW) > e && prop_group_epoch(PG_TB) > t);       /* view.mode = roll */
    CHECK(prop_get(P(VIEW_MODE)) == 1);
    t = prop_group_epoch(PG_TB);
    CHECK(prop_set_local(P(VIEW_MODE), 0, ORIGIN_KEY, NULL) == PS_OK);
    CHECK(prop_group_epoch(PG_TB) > t && prop_get(P(TB_ROLL)) == 0);           /* tb.roll */
    /* a posted change lands later: the owner bumps (model_touch) */
    e = prop_group_epoch(PG_SYS);
    model_touch(PG_SYS);
    CHECK(prop_group_epoch(PG_SYS) == e + 1);
    model_touch(PG_COUNT);                                        /* out of range: ignored */
}

/* ═══ save forms, profile lines, defaults, instruments ══════════════ */

static void test_save_forms(void)
{
    unsigned persist = 0, session = 0, usercal = 0;
    for (uint16_t i = 0; i < prop_count(); i++) {
        const prop_desc_t *p = prop_at(i);
        if (p->flags & PF_PERSIST) persist++;
        if (p->flags & PF_SESSION) session++;
        if (p->flags & PF_USERCAL) usercal++;
    }
    printf("  save forms: %u persisted, %u session, %u usercal\n", persist, session, usercal);
    CHECK(persist >= 30 && session >= 20 && usercal == 3);
    CHECK(P(CH1_ENABLED)->flags & PF_SESSION);             /* v5 stores a constant true */
    CHECK(!(P(CH1_ENABLED)->flags & PF_PERSIST));
    CHECK(P(SYS_INSTRUMENT)->flags & PF_SESSION);
    CHECK(P(SYS_MODE)->flags & PF_ALIAS);
    CHECK((P(SYS_LANG)->flags & (PF_PERSIST | PF_STORED_ONLY)) == (PF_PERSIST | PF_STORED_ONLY));
}

static void test_profile_lines(void)
{
    fresh();
    char line[96];
    prop_line_format(P(GEN_AMP), 15, line, sizeof line);
    CHECK(strcmp(line, "gen.amp = 1.5 V") == 0);
    prop_line_format(P(CH1_COUPLING), 1, line, sizeof line);
    CHECK(strcmp(line, "ch1.coupling = AC") == 0);
    prop_line_format(P(CH1_VDIV), 3, line, sizeof line);
    CHECK(strncmp(line, "ch1.vdiv = 3  # ", 16) == 0);
    prop_line_format(P(CH1_MEAS), 0x507, line, sizeof line);
    CHECK(strcmp(line, "ch1.meas = Freq,Peri,Duty+,Vp-p,Vrms") == 0);
    prop_line_format(P(IL_SKEW), -1500, line, sizeof line);
    CHECK(strcmp(line, "il.skew = -1500 ppm") == 0);
    prop_line_format(P(IL_GAIN), 1002500, line, sizeof line);
    CHECK(strcmp(line, "il.gain = 1.002500") == 0);                /* the decimals are the scale */

    /* every settable property at min, max and default: format -> parse -> same */
    unsigned rt = 0;
    for (uint16_t i = 0; i < prop_count(); i++) {
        const prop_desc_t *p = prop_at(i);
        if (!prop_is_settable(p)) continue;
        const int32_t vals[3] = { p->min, p->max, p->def };
        for (int k = 0; k < 3; k++) {
            const prop_desc_t *q = NULL;
            int32_t v = -12345;
            prop_line_format(p, vals[k], line, sizeof line);
            const prop_status_t s = prop_line_parse(line, &q, &v);
            CHECKF(s == PS_OK && q == p && v == vals[k], "'%s' -> %s %ld", line,
                   prop_status_name(s), (long)v);
            rt++;
        }
    }
    CHECK(rt >= 150);

    const prop_desc_t *q = (const prop_desc_t *)1;
    int32_t v = 7;
    CHECK(prop_line_parse("", &q, &v) == PS_OK && q == NULL);
    CHECK(prop_line_parse("   # a comment", &q, &v) == PS_OK && q == NULL);
    CHECK(prop_line_parse("gen.freq = 2500 Hz   # note", &q, &v) == PS_OK && q == P(GEN_FREQ) && v == 2500);
    CHECK(prop_line_parse("  sys.theme=green", &q, &v) == PS_OK && v == 1);
    CHECK(prop_line_parse("bogus.name = 1", &q, &v) == PS_INVALID && q == NULL);
    CHECK(prop_line_parse("gen.freq 2500", &q, &v) == PS_INVALID);
    CHECK(prop_line_parse("gen.freq = +5", &q, &v) == PS_OK && v == 5);       /* absolute in a profile */
    CHECK(prop_line_parse("gen.freq = +=5", &q, &v) == PS_INVALID);
    CHECK(prop_line_parse("ch1.vdiv = 12", &q, &v) == PS_OK && v == 12);      /* parse ok ... */
    CHECK(prop_set_local(q, v, ORIGIN_PROFILE, NULL) == PS_INVALID);         /* ... the setter says no */
}

static unsigned g_lines;
static char     g_first[96];
static void emit(const char *line, void *ctx)
{
    (void)ctx;
    if (!g_lines) snprintf(g_first, sizeof g_first, "%s", line);
    g_lines++;
    /* each line reads back to the property's current value */
    const prop_desc_t *q;
    int32_t v;
    CHECKF(prop_line_parse(line, &q, &v) == PS_OK && q && v == prop_get(q), "%s", line);
    CHECKF(!(q->flags & (PF_ALIAS | PF_RO)), "%s", line);
}

static void test_profile_write_and_defaults(void)
{
    fresh();
    g_lines = 0;
    const unsigned n = model_profile_write(PF_PERSIST, emit, NULL);
    CHECK(n == g_lines && n >= 30);
    CHECK(strncmp(g_first, "ch1.vdiv = 3", 12) == 0);       /* ch1.enabled is session-only */

    /* defaults are not the current value: change things, then load them */
    prop_note_t note;
    CHECK(prop_set_local(P(CH1_VDIV), 7, ORIGIN_KEY, &note) == PS_OK);
    CHECK(prop_set_local(P(GEN_AMP), 12, ORIGIN_KEY, &note) == PS_OK);
    CHECK(prop_set_local(P(TRIG_X), 22, ORIGIN_KEY, &note) == PS_OK);
    CHECK(prop_default(P(CH1_VDIV)) == 3 && prop_default(P(GEN_AMP)) == 30);
    const unsigned notes = g_notes;
    /* "Start with defaults": the persisted set, no settings note (the user
     * profile is untouched) */
    CHECK(model_load_defaults(PF_PERSIST, NULL, false) == 2);
    CHECK(prop_get(P(CH1_VDIV)) == 3 && prop_get(P(GEN_AMP)) == 30 && prop_get(P(TRIG_X)) == 22);
    CHECK(g_notes == notes);
    CHECK(model_load_defaults(PF_PERSIST, NULL, false) == 0);   /* already there */
    /* session defaults too, and "Reset to defaults" notes the change */
    CHECK(prop_set_local(P(SYS_THEME), 2, ORIGIN_KEY, &note) == PS_OK);
    const unsigned notes2 = g_notes;
    CHECK(model_load_defaults(PF_PERSIST | PF_SESSION, NULL, true) == 2);
    CHECK(prop_get(P(TRIG_X)) == 150 && prop_get(P(SYS_THEME)) == 0 && g_notes == notes2 + 1);
    /* RO and alias properties are never loaded */
    current_mode = MODE_SETTINGS;
    (void)model_load_defaults(0xFFFF, NULL, false);
    CHECK(prop_get(P(SYS_MODE)) == MODE_MULTIMETER);            /* sys.instrument's default (meter) */

    /* every default is in range and labelled (model_table_check too) */
    for (uint16_t i = 0; i < prop_count(); i++) {
        const prop_desc_t *p = prop_at(i);
        CHECKF(p->def >= p->min && p->def <= p->max, "%s", p->name);
    }
}

static void test_instruments(void)
{
    fresh();
    CHECK(model_instrument_count() == 3);
    const instrument_desc_t *sc = model_instrument_find("scope");
    const instrument_desc_t *me = model_instrument_find("meter");
    const instrument_desc_t *ge = model_instrument_find("gen");
    CHECK(sc && me && ge && !model_instrument_find("can"));
    for (uint8_t i = 0; i < model_instrument_count(); i++) {
        const instrument_desc_t *in = model_instrument_at(i);
        /* value i is sys.instrument's label i, and maps to that mode */
        CHECK(in->value == i && strcmp(fmtv(P(SYS_INSTRUMENT), i), in->name) == 0);
        CHECK(strcmp(fmtv(P(SYS_MODE), in->mode), i == 2 ? "siggen" : in->name) == 0);
        unsigned members = 0;
        for (uint16_t k = 0; k < prop_count(); k++) members += prop_in_instrument(prop_at(k), in);
        CHECK(members >= 5);
    }
    CHECK(prop_in_instrument(P(CH1_VDIV), sc) && !prop_in_instrument(P(CH1_VDIV), me));
    CHECK(prop_in_instrument(P(METER_FUNC), me) && prop_in_instrument(P(GEN_AMP), ge));
    CHECK(!prop_in_instrument(P(SYS_THEME), sc));                /* system-wide */
    CHECK(prop_instrument_default(P(TB_CODE), sc) == 10);

    /* The structure carries a CANscope: the scope's properties with their
     * own defaults (CH1/CH2 as CAN_H/CAN_L, 1.25 MS/s for a whole 500 kbit/s
     * frame, can_scope.h). Starting it on defaults touches only its own. */
    static const prop_pair_t can_defs[] = {
        { PROP_ID_TB_CODE, 0x0A }, { PROP_ID_CH1_VDIV, 6 }, { PROP_ID_TRIG_MODE, TRIG_NORMAL },
    };
    const instrument_desc_t can = { "canscope", 3, 0, INST_SCOPE, 3, can_defs };
    CHECK(prop_instrument_default(P(TB_CODE), &can) == 0x0A);
    CHECK(prop_instrument_default(P(CH1_VDIV), &can) == 6);
    CHECK(prop_instrument_default(P(CH2_VDIV), &can) == 5);       /* no override: built in */
    CHECK(prop_set_local(P(TB_CODE), 4, ORIGIN_KEY, NULL) == PS_OK);
    CHECK(prop_set_local(P(GEN_AMP), 5, ORIGIN_KEY, NULL) == PS_OK);
    CHECK(prop_set_local(P(SYS_THEME), 2, ORIGIN_KEY, NULL) == PS_OK);
    CHECK(model_load_defaults(PF_PERSIST, &can, false) == 3);    /* tb.code, ch1.vdiv, trig.mode */
    CHECK(prop_get(P(TB_CODE)) == 0x0A && prop_get(P(CH1_VDIV)) == 6 && prop_get(P(TRIG_MODE)) == TRIG_NORMAL);
    CHECK(prop_get(P(GEN_AMP)) == 5);                            /* not one of its properties */
    CHECK(prop_get(P(SYS_THEME)) == 2);                          /* system-wide: left alone */
    CHECK(model_load_defaults(PF_PERSIST | PF_SESSION, sc, false) > 0);
    CHECK(prop_get(P(SYS_THEME)) == 2 && prop_get(P(TB_CODE)) == 10 && prop_get(P(CH1_VDIV)) == 3);
}

static void test_sizes(void)
{
    printf("  host: sizeof(prop_desc_t) = %u, table = %u B (%u properties), epochs = %u B\n",
           (unsigned)sizeof(prop_desc_t), (unsigned)(sizeof(prop_desc_t) * PROP_IX_COUNT),
           (unsigned)PROP_IX_COUNT, (unsigned)sizeof prop_epoch);
    CHECK(sizeof(prop_desc_t) <= 80);
}

int main(void)
{
    test_table();
    test_glob();
    test_parse_format();
    test_bounds_step();
    test_pairs();
    test_epochs_notes();
    test_rules();
    test_not_set_keeps_old();
    test_fail_each_call();
    test_cross_group();
    test_save_forms();
    test_profile_lines();
    test_profile_write_and_defaults();
    test_instruments();
    test_sizes();
    printf("test_model: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
