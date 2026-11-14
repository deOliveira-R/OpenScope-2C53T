/*
 * Host transcript test for the old state commands as aliases of the property
 * model (src/drivers/shell_alias.c, ARCH-1 M4).
 *
 * Each converted handler is driven with its old command line against the
 * real model (model.c, props_*.c) with fake hardware ops, behind the
 * executor's host port (model_exec.c: the request really goes through the
 * queue and is applied by the "input task"). Its output is compared byte for
 * byte with what the handler printed BEFORE the conversion. The expected
 * text is transcribed from the old handlers (usb_debug.c at d56e4c1: the
 * usb_debug_printf / usb_send_str calls on the path each line takes), with
 * the fixed values the fake environment returns.
 *
 * The multi-line status blocks (trigger, timebase, roll, interleave, skew,
 * vpos, prm, siggen, mask, the `meter func` report) are printed by code that
 * did not move (usb_debug.c); here the environment prints a marker
 * "<NAME arg>" in their place, so the test also pins WHERE each handler
 * prints its block, and with which argument.
 *
 * INTENTIONAL DIFFERENCES from the old handlers, each asserted below (D1..):
 *   D1  a failed hardware apply leaves the state unchanged (the old `fpga
 *       scope level`, `timebase` and `roll` stored first and kept the value);
 *   D2  TIMEOUT / BUSY replies exist (the executor), and nothing is applied;
 *   D3  a change whose hardware path is not live is stored and says so after
 *       the old line (the old handlers drove the hardware on any screen);
 *   D4  `fpga scope ch` moves the active channel off a channel it switches
 *       off, and does not re-apply the routing when nothing changes;
 *   D5  `fpga roll off` out of roll and `mode scope` in scope do nothing
 *       (the old ones re-wrote reg 0x01 / re-entered the scope posture);
 *   D6  `mode startup` knows restore, and its usage line names it;
 *   D7  `mask tol` beyond the model's range is clamped, and says so;
 *   D8  `fpga interleave corr` drops a piecewise map by asking for it
 *       (il.knots = 0) before the line: same result, three applies;
 *   D9  `mode meter` prints the function and layout read back;
 *   D10 the meter function's side effects are the key's (the fake op only
 *       records that the setter ran; model_hw_fw.c op_meter_func has them).
 *
 * Build/run:  make -C firmware test-shell-alias
 */
#define MODEL_INTERNAL
#include "model.h"
#include "model_exec.h"
#include "shell_alias.h"
#include "scope_state.h"
#include "scope_cal.h"
#include "meas_select.h"
#include "signal_gen.h"
#include "sys_prefs.h"
#include "theme.h"
#include "math_channel.h"
#include "ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ═══ the globals main.c / input_handler.c own in the firmware ═══════ */
volatile device_mode_t  current_mode  = MODE_OSCILLOSCOPE;
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
uint8_t key_focus_get(void) { return 0; }
volatile uint32_t input_events_done;

void dac_output_apply(const siggen_timing_t *t) { (void)t; }
void dac_output_stop(void) {}
static uint16_t g_dac_buf[SIGGEN_CYCLE_SAMPLES];
uint16_t *dac_output_get_buffer(void) { return g_dac_buf; }
uint32_t dac_output_timer_clock_hz(void) { return 240000000u; }

/* ═══ fake hardware ops (a log of what the setters asked for) ════════ */
static char     g_log[1024];
static bool     g_fail;                 /* every apply op answers "not done"      */
static bool     g_busy;                 /* self-cal running                       */
static bool     g_fe_off;               /* the scope front end is not live        */
static bool     g_link_on;              /* the meter link                         */
static bool     g_mask_dead;            /* the acquisition task applies nothing   */
static bool     g_il_on = true;
static int32_t  g_il_gain = 1000000, g_il_off, g_il_skew;
static uint8_t  g_il_knots;
static uint8_t  g_mask_v = 8, g_mask_h = 2;
static bool     g_mask_stop = true;
static bool     g_degf;
static unsigned g_notes;

static void logf_(const char *fmt, ...)
{
    char t[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    strncat(g_log, t, sizeof g_log - strlen(g_log) - 1);
}

static bool fe_off(const char *what, int v)
{
    if (!g_fe_off) return false;
    logf_("%s-stored(%d);", what, v);
    model_hw_deferred("stored: scope FE off");
    return true;
}
static bool f_vdiv(uint8_t ch, uint8_t i)
{
    if (fe_off("vdiv", i)) return true;
    logf_("vdiv(%u,%u);", ch, i);
    return !g_fail;
}
static bool f_channel_enables(void)
{
    const scope_state_t *ss = scope_state_get();
    logf_("enables(%d,%d);", ss->ch1.enabled, ss->ch2.enabled);
    return ss->ch1.enabled || ss->ch2.enabled;      /* fpga_apply_channel_enables: one stays on */
}
static bool f_il_active(void) { return g_il_on && !scope_state_get()->ch2.enabled; }
static bool f_vpos(uint8_t ch, int16_t pos) { logf_("vpos(%u,%d);", ch, pos); return true; }
static bool f_timebase(uint8_t c)
{
    if (fe_off("tb", c)) return true;
    logf_("tb(%u);", c);
    return !g_fail;
}
static bool f_roll(void)
{
    const scope_state_t *ss = scope_state_get();
    if (fe_off("roll", ss->roll)) return true;
    logf_("roll(%d,%u);", ss->roll, ss->roll_step);
    return !g_fail;
}
static bool f_trig_src(void)
{
    const scope_state_t *ss = scope_state_get();
    if (fe_off("src", ss->trigger.source)) return true;
    logf_("src(%d);", ss->trigger.source);
    return !g_fail;
}
static bool f_trig_level(int16_t l)
{
    if (fe_off("lvl", l)) return true;
    logf_("lvl(%d);", l);
    return !g_fail;
}
static void f_single_rearm(void) { logf_("rearm;"); }
static void f_mask_get(uint8_t *v, uint8_t *h, bool *sf) { *v = g_mask_v; *h = g_mask_h; *sf = g_mask_stop; }
static bool f_mask_tol(uint8_t v, uint8_t h)
{
    logf_("mask_tol(%u,%u);", v, h);
    if (!g_mask_dead) { g_mask_v = v; g_mask_h = h; }
    return true;                                    /* posted (model_hw_fw.c) */
}
static bool f_mask_stopfail(bool on)
{
    logf_("mask_stop(%d);", on);
    if (!g_mask_dead) g_mask_stop = on;
    return true;
}
static void f_gen_wave(uint8_t w)   { logf_("gen_wave(%u);", w); siggen_set_waveform(w); }
static void f_gen_freq(uint32_t f)  { logf_("gen_freq(%u);", (unsigned)f); siggen_set_frequency(f); }
static void f_gen_amp(uint8_t a)    { logf_("gen_amp(%u);", a); siggen_set_amplitude_dv(a); }
static void f_gen_duty(percent_t d) { logf_("gen_duty(%u);", d.pct); siggen_set_duty(d.pct); }
static void f_gen_out(bool on)      { logf_("gen_out(%d);", on); siggen_enable(on); }
static void f_gen_bootout(bool on)  { logf_("gen_boot(%d);", on); siggen_set_start_on(on); }
/* As model_hw_fw.c op_meter_func: posted while the meter is served, else
 * stored for the next meter entry. */
static bool f_meter_func(uint8_t m, bool keep)
{
    (void)keep;
    if (current_mode != MODE_MULTIMETER && !g_link_on) {
        logf_("meter-stored(%u);", m);
        model_hw_deferred("stored: meter FE off");
        return true;
    }
    logf_("meter_func(%u);", m);
    return !g_fail;
}
static bool f_meter_link_on(void) { return g_link_on; }
static bool f_meter_flag_get(uint8_t which) { return which == METER_FLAG_DEGF ? g_degf : false; }
static bool f_meter_flag_set(uint8_t which, bool on, bool user)
{
    logf_("flag(%u,%d,%d);", which, on, user);
    if (which == METER_FLAG_DEGF) g_degf = on;
    return true;
}
static void f_meter_layout(uint8_t l) { logf_("layout(%u);", l); }
static bool f_mode(uint8_t m) { logf_("mode(%u);", m); current_mode = (device_mode_t)m; return !g_fail; }
static bool f_il_enable(bool on) { logf_("il(%d);", on); g_il_on = on; return true; }
static bool f_il_enabled(void) { return g_il_on; }
/* fpga_interleave_corr_set: replaces the whole level map (drops the knots). */
static bool f_il_corr_set(gain_e6_t g, int32_t o)
{
    logf_("il_corr(%d,%d);", (int)g.e6, (int)o);
    g_il_gain = g.e6; g_il_off = o; g_il_knots = 0;
    return true;
}
static void f_il_corr_get(gain_e6_t *g, int32_t *o) { g->e6 = g_il_gain; *o = g_il_off; }
static uint8_t f_il_knots(void) { return g_il_knots; }
static bool f_il_skew_set(ppm_period_t s) { logf_("il_skew(%d);", (int)s.ppm); g_il_skew = s.ppm; return !g_fail; }
static ppm_period_t f_il_skew_get(void) { ppm_period_t s = { g_il_skew }; return s; }
static bool f_busy(void) { return g_busy; }
static bool f_shell_persist(void) { return true; }
static void f_note_change(void) { g_notes++; }

static const model_hw_ops_t k_fake = {
    .vdiv = f_vdiv, .channel_enables = f_channel_enables, .interleave_active = f_il_active,
    .vpos = f_vpos, .timebase = f_timebase, .roll = f_roll, .trigger_source = f_trig_src,
    .trigger_level = f_trig_level, .single_rearm = f_single_rearm,
    .mask_get = f_mask_get, .mask_tol = f_mask_tol, .mask_stopfail = f_mask_stopfail,
    .gen_wave = f_gen_wave, .gen_freq = f_gen_freq, .gen_amp = f_gen_amp, .gen_duty = f_gen_duty,
    .gen_out = f_gen_out, .gen_bootout = f_gen_bootout,
    .meter_func = f_meter_func, .meter_link_on = f_meter_link_on,
    .meter_flag_get = f_meter_flag_get, .meter_flag_set = f_meter_flag_set, .meter_layout = f_meter_layout,
    .mode = f_mode,
    .il_enable = f_il_enable, .il_enabled = f_il_enabled, .il_corr_set = f_il_corr_set,
    .il_corr_get = f_il_corr_get, .il_knots = f_il_knots, .il_skew_set = f_il_skew_set,
    .il_skew_get = f_il_skew_get,
    .selfcal_busy = f_busy, .shell_persist = f_shell_persist, .note_change = f_note_change,
};

/* ═══ the executor's host port (single thread: a sleep runs the drain) ═ */
static model_req_t q[MODEL_EXEC_QUEUE_LEN];
static unsigned q_head, q_count, n_sent;
static bool     responsive = true;
static bool     in_exec;

static bool tp_send(const model_req_t *r)
{
    n_sent++;
    if (q_count == MODEL_EXEC_QUEUE_LEN) return false;
    q[(q_head + q_count) % MODEL_EXEC_QUEUE_LEN] = *r;
    q_count++;
    return true;
}
static bool tp_receive(model_req_t *r)
{
    if (!q_count) return false;
    *r = q[q_head];
    q_head = (q_head + 1u) % MODEL_EXEC_QUEUE_LEN;
    q_count--;
    return true;
}
static void tp_wake(void) {}
static void tp_sleep(uint32_t ms)
{
    (void)ms;
    if (responsive) { in_exec = true; (void)model_exec_drain(); in_exec = false; }
}
static void tp_nop(void) {}
static void tp_changed(uint16_t g) { (void)g; }
static bool tp_on_exec(void) { return in_exec; }
static const char *tp_unwired(uint16_t id) { (void)id; return NULL; }

static const model_exec_port_t k_port = {
    .send = tp_send, .receive = tp_receive, .wake = tp_wake, .sleep_ms = tp_sleep,
    .checkin = tp_nop, .changed = tp_changed, .service = tp_nop,
    .on_executor = tp_on_exec, .unwired = tp_unwired,
};

/* ═══ the alias environment: fixed read-backs, markers for the blocks ═ */
static char g_out[4096];
static int  g_roll_code = -2;           /* the last roll_code_override() */
static bool g_flush_answers = true;
static unsigned g_begins, g_waits;

static void e_emit(const char *t) { strncat(g_out, t, sizeof g_out - strlen(g_out) - 1); }
static void e_status(shell_alias_status_t which, int arg)
{
    static const char *const name[SA_ST_COUNT] = {
        "TRIGGER", "TIMEBASE", "ROLL", "INTERLEAVE", "SKEW", "VPOS", "PRM", "SIGGEN", "MASK",
        "METER_FUNC_BEGIN", "METER_FUNC",
    };
    char t[48];
    snprintf(t, sizeof t, "<%s %d>\r\n", name[which], arg);
    e_emit(t);
}
static int         e_trig_x_actual(void)  { return 147; }
static uint8_t     e_trig_anchor(void)    { return 2; }
static bool        e_edge_filter(void)    { return true; }
static uint8_t     e_channel_mask(void)   { return 3; }
static uint8_t     e_trig_code(void)      { return 0x7F; }
static uint8_t     e_trig_reconcile(void) { return 0x80; }
static uint8_t     e_trig_src_code(void)  { return 0x01; }
static const char *e_startup_name(uint8_t m)
{
    static const char *const n[] = { "Scope", "Meter", "Home", "Signal Gen", "Last Used" };   /* main.c */
    return m < 5 ? n[m] : "?";
}
static const char *e_func_name(uint8_t f)
{
    static const char *const n[] = { "DCV", "ACV", "DCmA", "DCA", "ACmA", "ACA", "Ohm", "Cont",
                                     "Diode", "Cap", "Temp", "Auto" };
    return f < 12 ? n[f] : "?";
}
static const char *e_layout_name(uint8_t l)
{
    static const char *const n[] = { "big", "chart", "stats", "fuse", "limits" };
    return l < 5 ? n[l] : "?";
}
static bool e_flush(uint32_t ms, const char **outcome)
{
    (void)ms;
    if (!g_flush_answers) return false;
    *outcome = "saved";
    return true;
}
static void e_begin(void) { g_begins++; }
static void e_wait(void)  { g_waits++; }
static void e_roll_code(int c) { g_roll_code = c; }

static const shell_alias_env_t k_env = {
    .emit = e_emit, .sleep_ms = NULL, .status = e_status,
    .trig_x_actual = e_trig_x_actual, .trig_anchor = e_trig_anchor, .edge_filter = e_edge_filter,
    .channel_mask = e_channel_mask, .trig_code = e_trig_code, .trig_reconcile_code = e_trig_reconcile,
    .trig_src_code = e_trig_src_code, .startup_name = e_startup_name,
    .meter_func_name = e_func_name, .meter_layout_name = e_layout_name,
    .settings_flush = e_flush, .meter_switch_begin = e_begin, .meter_switch_wait = e_wait,
    .roll_code_override = e_roll_code,
};

/* ═══ harness ════════════════════════════════════════════════════════ */

static void fresh(void)
{
    scope_state_init(scope_state_get());
    meas_select_defaults();
    siggen_init(NULL);
    sys_prefs_set_brightness(SYS_BRIGHT_DEFAULT);
    sys_prefs_set_volume(SYS_VOL_DEFAULT);
    theme_init(THEME_DARK_BLUE);
    g_il_on = true; g_il_gain = 1000000; g_il_off = 0; g_il_skew = 0; g_il_knots = 0;
    g_mask_v = 8; g_mask_h = 2; g_mask_stop = true; g_mask_dead = false;
    g_degf = false;
    g_fail = g_busy = g_fe_off = g_link_on = false;
    model_bind_hw(&k_fake);
    (void)model_load_defaults(PF_PERSIST | PF_SESSION | PF_USERCAL, NULL, false);
    current_mode = MODE_OSCILLOSCOPE;
    active_channel = 0;
    q_head = q_count = n_sent = 0;
    responsive = true;
    in_exec = false;
    g_flush_answers = true;
    g_begins = g_waits = 0;
    g_roll_code = -2;
    g_notes = 0;
    model_exec_test_reset(1);
    model_exec_init(&k_port);
    g_log[0] = '\0';
    g_out[0] = '\0';
}

typedef void (*alias_fn)(const char *args, const shell_alias_env_t *e);

/* Run one command; true when the output is exactly `want`. */
static bool says(alias_fn fn, const char *args, const char *want, int line)
{
    g_out[0] = '\0';
    g_log[0] = '\0';
    fn(args, &k_env);
    checks++;
    if (strcmp(g_out, want) == 0) return true;
    fails++;
    printf("FAIL %s:%d  args \"%s\"\n  got:  %s\n  want: %s\n", __FILE__, line, args, g_out, want);
    return false;
}
#define SAYS(fn, args, want) (void)says(fn, args, want, __LINE__)
#define OPS(want) do { checks++; if (strcmp(g_log, want) != 0) { fails++; \
    printf("FAIL %s:%d  ops: got \"%s\" want \"%s\"\n", __FILE__, __LINE__, g_log, want); } } while (0)

#define TIMEOUT_NOTHING "TIMEOUT: input task busy (gallery/countdown/selfcal?); nothing applied\r\n"

/* ═══ channels ═══════════════════════════════════════════════════════ */

static void test_vdiv(void)
{
    scope_state_t *ss = scope_state_get();
    char lbl[12], want[160];
    fresh();
    scope_cal_range_label(1, 4, lbl, sizeof lbl);
    snprintf(want, sizeof want, "vdiv CH1 = range 4 (%s/div)\r\n", lbl);
    SAYS(shell_alias_scope_vdiv, "1 4", want);
    OPS("vdiv(0,4);");
    CHECK(ss->ch1.vdiv_idx == 4);
    CHECK(n_sent == 1);                                    /* through the executor */
    scope_cal_range_label(2, 9, lbl, sizeof lbl);
    snprintf(want, sizeof want, "vdiv CH2 = range 9 (%s/div)\r\n", lbl);
    SAYS(shell_alias_scope_vdiv, "2   9", want);
    CHECK(ss->ch2.vdiv_idx == 9);
    /* usage, as before: no channel, a bad channel, no range, a range past 9 */
    SAYS(shell_alias_scope_vdiv, "", "Usage: fpga scope vdiv <1|2> <0-9>\r\n");
    SAYS(shell_alias_scope_vdiv, "3 4", "Usage: fpga scope vdiv <1|2> <0-9>\r\n");
    SAYS(shell_alias_scope_vdiv, "1", "Usage: fpga scope vdiv <1|2> <0-9>\r\n");
    SAYS(shell_alias_scope_vdiv, "1 10", "Usage: fpga scope vdiv <1|2> <0-9>\r\n");
    SAYS(shell_alias_scope_vdiv, "1 x", "Usage: fpga scope vdiv <1|2> <0-9>\r\n");
    OPS("");
    /* the relay apply fails: the old line, and the state as it was */
    g_fail = true;
    SAYS(shell_alias_scope_vdiv, "1 7", "vdiv: relay apply failed\r\n");
    CHECK(ss->ch1.vdiv_idx == 4);
    g_fail = false;
    /* D2: the input task does not answer / self-cal runs */
    responsive = false;
    SAYS(shell_alias_scope_vdiv, "1 7", TIMEOUT_NOTHING);
    CHECK(ss->ch1.vdiv_idx == 4);
    responsive = true;
    (void)model_exec_drain();                              /* the withdrawn request: never applied */
    CHECK(ss->ch1.vdiv_idx == 4);
    g_busy = true;
    SAYS(shell_alias_scope_vdiv, "1 7", "BUSY ch1.vdiv (self-cal running)\r\n");
    g_busy = false;
    /* D3: the scope front end is not live: stored, and it says so */
    g_fe_off = true;
    scope_cal_range_label(1, 7, lbl, sizeof lbl);
    snprintf(want, sizeof want, "vdiv CH1 = range 7 (%s/div)\r\n"
             "  (ch1.vdiv stored: scope FE off; applied when it is next entered)\r\n", lbl);
    SAYS(shell_alias_scope_vdiv, "1 7", want);
    OPS("vdiv-stored(7);");
    CHECK(ss->ch1.vdiv_idx == 7);
}

static void test_ch(void)
{
    scope_state_t *ss = scope_state_get();
    fresh();
    SAYS(shell_alias_scope_ch, "1", "CH1 on  CH2 on  mask 3  trigger CH1  interleave inactive\r\n");
    OPS("");
    SAYS(shell_alias_scope_ch, "2 off", "CH1 on  CH2 off  mask 3  trigger CH1  interleave ACTIVE (250 MS/s)\r\n");
    OPS("enables(1,0);");
    CHECK(!ss->ch2.enabled);
    /* the last channel: the old refusal, and it stays on */
    SAYS(shell_alias_scope_ch, "1 off", "refused: at least one channel must stay on\r\n");
    CHECK(ss->ch1.enabled);
    SAYS(shell_alias_scope_ch, "2 on", "CH1 on  CH2 on  mask 3  trigger CH1  interleave inactive\r\n");
    SAYS(shell_alias_scope_ch, "0", "usage: fpga scope ch <1|2> [on|off]\r\n");
    SAYS(shell_alias_scope_ch, "1 maybe", "usage: fpga scope ch <1|2> [on|off]\r\n");
    /* D4: the arrows follow the live channel; no re-apply when unchanged */
    active_channel = 0;
    SAYS(shell_alias_scope_ch, "1 off", "CH1 off  CH2 on  mask 3  trigger CH1  interleave inactive\r\n");
    CHECK(active_channel == 1);
    SAYS(shell_alias_scope_ch, "1 off", "CH1 off  CH2 on  mask 3  trigger CH1  interleave inactive\r\n");
    OPS("");
}

static void test_probe(void)
{
    fresh();
    SAYS(shell_alias_scope_probe, "1", "CH1 probe 1X\r\n");
    SAYS(shell_alias_scope_probe, "1 10x", "CH1 probe 10X\r\n");
    CHECK(scope_state_get()->ch1.probe == PROBE_10X);
    SAYS(shell_alias_scope_probe, "2 1x", "CH2 probe 1X\r\n");
    SAYS(shell_alias_scope_probe, "2 5x", "usage: fpga scope probe <1|2> [1x|10x]\r\n");
    SAYS(shell_alias_scope_probe, "", "usage: fpga scope probe <1|2> [1x|10x]\r\n");
    CHECK(g_notes == 1);                                   /* a persisted setting: noted for the save */
}

static void test_cal_pos(void)
{
    scope_state_t *ss = scope_state_get();
    fresh();
    SAYS(shell_alias_cal_pos, "1", "<VPOS 1>\r\n");
    OPS("");
    SAYS(shell_alias_cal_pos, "2 -35", "<VPOS 2>\r\n");
    OPS("vpos(1,-35);");
    CHECK(ss->ch2.position == -35);
    SAYS(shell_alias_cal_pos, "1 250", "<VPOS 1>\r\n");    /* clamped without a word, as scope_vpos_set did */
    CHECK(ss->ch1.position == 100);
    SAYS(shell_alias_cal_pos, "3", "Usage: cal pos <1|2> [-100..100]\r\n");
    SAYS(shell_alias_cal_pos, "1 up", "Usage: cal pos <1|2> [-100..100]\r\n");
    SAYS(shell_alias_cal_pos, "", "Usage: cal pos <1|2> [-100..100]\r\n");
}

/* ═══ timebase and roll ══════════════════════════════════════════════ */

static void test_timebase(void)
{
    scope_state_t *ss = scope_state_get();
    fresh();
    SAYS(shell_alias_scope_timebase, "", "<TIMEBASE 0>\r\n");
    OPS("");
    SAYS(shell_alias_scope_timebase, "0a", "<TIMEBASE 1>\r\n");
    OPS("tb(10);");
    CHECK(ss->timebase_idx == 0x0A);
    SAYS(shell_alias_scope_timebase, "0x12", "<TIMEBASE 1>\r\n");
    CHECK(ss->timebase_idx == 0x12);
    SAYS(shell_alias_scope_timebase, "fast", "usage: fpga scope timebase <hex code 00-14>\r\n");
    SAYS(shell_alias_scope_timebase, "15", "usage: fpga scope timebase <hex code 00-14>\r\n");
    /* a Y-T code leaves roll: one write, of the new code */
    ss->roll = true;
    SAYS(shell_alias_scope_timebase, "03", "<TIMEBASE 1>\r\n");
    OPS("roll(0,0);");
    CHECK(!ss->roll && ss->timebase_idx == 3);
    /* D1: nothing written, nothing changed (the old one kept the new code) */
    g_fail = true;
    SAYS(shell_alias_scope_timebase, "05", "acq task would not park — reg 0x01 NOT written\r\n");
    CHECK(ss->timebase_idx == 3);
    g_fail = false;
    g_fe_off = true;                                       /* D3 */
    SAYS(shell_alias_scope_timebase, "05", "<TIMEBASE 1>\r\n"
         "  (tb.code stored: scope FE off; applied when it is next entered)\r\n");
    CHECK(ss->timebase_idx == 5);
}

static void test_roll(void)
{
    scope_state_t *ss = scope_state_get();
    fresh();
    SAYS(shell_alias_roll, "", "<ROLL 0>\r\n");
    OPS("");
    SAYS(shell_alias_roll, "3", "<ROLL 0>\r\n");           /* a step enters roll there: one write */
    OPS("roll(1,3);");
    CHECK(ss->roll && ss->roll_step == 3);
    SAYS(shell_alias_roll, "5", "<ROLL 0>\r\n");
    OPS("roll(1,5);");
    SAYS(shell_alias_roll, "9", "usage: fpga roll [off|0-8|code <hex>|code stock]\r\n");
    SAYS(shell_alias_roll, "fast", "usage: fpga roll [off|0-8|code <hex>|code stock]\r\n");
    /* code: the override is the environment's; rolling, the entry is re-applied */
    SAYS(shell_alias_roll, "code 16", "<ROLL 0>\r\n");
    CHECK(g_roll_code == 0x16);
    OPS("roll(1,5);");
    SAYS(shell_alias_roll, "code stock", "<ROLL 0>\r\n");
    CHECK(g_roll_code == -1);
    SAYS(shell_alias_roll, "code", "usage: fpga roll code <hex 00-FF>|stock\r\n");
    SAYS(shell_alias_roll, "code 100", "usage: fpga roll code <hex 00-FF>|stock\r\n");
    g_fail = true;                                         /* D1 */
    SAYS(shell_alias_roll, "off", "acq task would not park -- reg 0x01 NOT written\r\n");
    CHECK(ss->roll);
    g_fail = false;
    SAYS(shell_alias_roll, "off", "<ROLL 0>\r\n");
    OPS("roll(0,5);");
    CHECK(!ss->roll);
    SAYS(shell_alias_roll, "off", "<ROLL 0>\r\n");         /* D5: already off, nothing is written */
    OPS("");
    SAYS(shell_alias_roll, "code 15", "<ROLL 0>\r\n");     /* not rolling: the override only */
    OPS("");
}

/* ═══ trigger ════════════════════════════════════════════════════════ */

static void test_trigger(void)
{
    scope_state_t *ss = scope_state_get();
    fresh();
    /* hpos */
    SAYS(shell_alias_scope_hpos, "", "trigger position: asked column 150; last window landed at 147 (anchor hardware)\r\n");
    SAYS(shell_alias_scope_hpos, "160", "trigger position: asked column 160; last window landed at 147 (anchor hardware)\r\n");
    CHECK(ss->trig_x == 160);
    SAYS(shell_alias_scope_hpos, "301", "usage: fpga scope hpos [0..300]\r\n");
    SAYS(shell_alias_scope_hpos, "left", "usage: fpga scope hpos [0..300]\r\n");
    /* edge */
    SAYS(shell_alias_scope_edge, "", "trigger edge Rising (edge filter ON)\r\n");
    SAYS(shell_alias_scope_edge, "falling", "trigger edge Falling (edge filter ON)\r\n");
    CHECK(ss->trigger.edge == TRIG_FALLING);
    SAYS(shell_alias_scope_edge, "rising", "trigger edge Rising (edge filter ON)\r\n");
    SAYS(shell_alias_scope_edge, "both", "usage: fpga scope edge [rising|falling]\r\n");
    /* trigmode */
    SAYS(shell_alias_scope_trigmode, "", "trigger mode Auto (acq wait policy: AUTO falls back after the edge-wait; NORMAL/SINGLE hold)\r\n<TRIGGER 0>\r\n");
    SAYS(shell_alias_scope_trigmode, "normal", "trigger mode Normal (acq wait policy: AUTO falls back after the edge-wait; NORMAL/SINGLE hold)\r\n<TRIGGER 0>\r\n");
    CHECK(ss->trigger.mode == TRIG_NORMAL);
    ss->running = false;
    SAYS(shell_alias_scope_trigmode, "single", "trigger mode Single (acq wait policy: AUTO falls back after the edge-wait; NORMAL/SINGLE hold)\r\n<TRIGGER 0>\r\n");
    OPS("rearm;");                                         /* as the panel: Single arms the one-shot */
    CHECK(ss->trigger.mode == TRIG_SINGLE && ss->running);
    SAYS(shell_alias_scope_trigmode, "auto", "trigger mode Auto (acq wait policy: AUTO falls back after the edge-wait; NORMAL/SINGLE hold)\r\n<TRIGGER 0>\r\n");
    SAYS(shell_alias_scope_trigmode, "never", "usage: fpga scope trigmode [auto|normal|single]\r\n");
    /* single */
    ss->running = false;
    SAYS(shell_alias_scope_single, "", "single armed (Await Trigger...)\r\n");
    OPS("rearm;");
    CHECK(ss->trigger.mode == TRIG_SINGLE && ss->running);
    /* source */
    SAYS(shell_alias_scope_source, "", "trigger source CH1 -> reg 0x06 = 0x01\r\n");
    SAYS(shell_alias_scope_source, "ch2", "trigger source CH2 -> reg 0x06 = 0x01\r\n");
    OPS("src(1);");
    CHECK(ss->trigger.source == TRIG_SRC_CH2);
    SAYS(shell_alias_scope_source, "ext", "usage: fpga scope source [ch1|ch2]\r\n");
    ss->ch1.enabled = false;
    SAYS(shell_alias_scope_source, "ch1", "trigger source CH1 refused: that channel is off\r\n");
    CHECK(ss->trigger.source == TRIG_SRC_CH2);
    ss->ch1.enabled = true;
    g_fail = true;
    SAYS(shell_alias_scope_source, "ch1", "trigger source NOT SET (acq task would not park)\r\n");
    CHECK(ss->trigger.source == TRIG_SRC_CH2);
    g_fail = false;
    /* level */
    SAYS(shell_alias_scope_level, "", "trigger level 0 -> reg 0x08 code 0x7F in force (boot reconcile wrote 0x80)\r\n");
    SAYS(shell_alias_scope_level, "-40", "trigger level -40 -> reg 0x08 code 0x7F in force (boot reconcile wrote 0x80)\r\n");
    OPS("lvl(-40);");
    CHECK(ss->trigger.level == -40);
    SAYS(shell_alias_scope_level, "101", "usage: fpga scope level [-100..100]\r\n");
    SAYS(shell_alias_scope_level, "high", "usage: fpga scope level [-100..100]\r\n");
    g_fail = true;                                         /* D1: the old one kept the new level */
    SAYS(shell_alias_scope_level, "25", "acq task would not park — reg 0x08 NOT written\r\n");
    CHECK(ss->trigger.level == -40);
    g_fail = false;
    g_fe_off = true;                                       /* D3 */
    SAYS(shell_alias_scope_level, "25", "trigger level 25 -> reg 0x08 code 0x7F in force (boot reconcile wrote 0x80)\r\n"
         "  (trig.level stored: scope FE off; applied when it is next entered)\r\n");
    g_fe_off = false;
    /* softtrig */
    SAYS(shell_alias_scope_softtrig, "", "softtrig: ON (trace locked to trigger)   (level=25 px, rising, src=CH2)\r\n");
    SAYS(shell_alias_scope_softtrig, "off", "softtrig: off (free-run / dancing)   (level=25 px, rising, src=CH2)\r\n");
    CHECK(!ss->soft_trigger);
    SAYS(shell_alias_scope_softtrig, "toggle", "softtrig: ON (trace locked to trigger)   (level=25 px, rising, src=CH2)\r\n");
    SAYS(shell_alias_scope_softtrig, "on", "softtrig: ON (trace locked to trigger)   (level=25 px, rising, src=CH2)\r\n");
    SAYS(shell_alias_scope_softtrig, "maybe", "usage: fpga scope softtrig [on|off|toggle]\r\n");
}

/* ═══ view ═══════════════════════════════════════════════════════════ */

static void test_view(void)
{
    scope_state_t *ss = scope_state_get();
    char l1[12], l2[12], want[320];
    fresh();
    SAYS(shell_alias_scope_graticule, "auto", "graticule: autofit (grid == position only)\r\n");
    CHECK(!ss->true_scale);
    scope_cal_range_label(1, ss->ch1.vdiv_idx, l1, sizeof l1);
    scope_cal_range_label(2, ss->ch2.vdiv_idx, l2, sizeof l2);
    snprintf(want, sizeof want, "graticule: TRUE SCALE (grid == volts/div)\r\n"
             "  CH1 rng %u -> %s/div %s\r\n  CH2 rng %u -> %s/div %s\r\n"
             "  center the baseline first: `fpga scope center`\r\n",
             (unsigned)ss->ch1.vdiv_idx, l1,
             scope_cal_true_scale_ok(1, ss->ch1.vdiv_idx) ? "" : "(no cal -> autofit)",
             (unsigned)ss->ch2.vdiv_idx, l2,
             scope_cal_true_scale_ok(2, ss->ch2.vdiv_idx) ? "" : "(no cal -> autofit)");
    SAYS(shell_alias_scope_graticule, "true", want);
    CHECK(ss->true_scale);
    SAYS(shell_alias_scope_graticule, "", want);
    SAYS(shell_alias_scope_graticule, "toggle", "graticule: autofit (grid == position only)\r\n");
    SAYS(shell_alias_scope_graticule, "big", "usage: fpga scope graticule [auto|true|toggle]\r\n");

    /* prm: the report is usb_debug.c's block; the selection is the model's */
    SAYS(shell_alias_scope_prm, "", "<PRM 0>\r\n");
    SAYS(shell_alias_scope_prm, "all", "<PRM 0>\r\n");
    CHECK(meas_select_get(0) == 0x0FFF);
    SAYS(shell_alias_scope_prm, "ch2 none", "<PRM 0>\r\n");
    CHECK(meas_select_get(1) == 0 && meas_select_get(0) == 0x0FFF);
    SAYS(shell_alias_scope_prm, "ch2 toggle 3", "<PRM 0>\r\n");
    CHECK(meas_select_get(1) == 0x0008);
    SAYS(shell_alias_scope_prm, "ch2 toggle 3", "<PRM 0>\r\n");
    CHECK(meas_select_get(1) == 0);
    SAYS(shell_alias_scope_prm, "CH1 0x1005", "<PRM 0>\r\n");          /* masked to the 12 parameters */
    CHECK(meas_select_get(0) == 0x0005);
    SAYS(shell_alias_scope_prm, "default", "<PRM 0>\r\n");
    CHECK(meas_select_get(0) == MEAS_SELECT_CH1_DEFAULT);
    SAYS(shell_alias_scope_prm, "hide", "<PRM 0>\r\n");
    CHECK(ss->prm_hidden);
    SAYS(shell_alias_scope_prm, "show", "<PRM 0>\r\n");
    CHECK(!ss->prm_hidden);
    SAYS(shell_alias_scope_prm, "toggle 12", "usage: fpga scope prm [ch1|ch2] toggle <0-11>\r\n");
    SAYS(shell_alias_scope_prm, "ch1 most",
         "usage: fpga scope prm [ch1|ch2] [mask|all|default|none|toggle <0-11>|hide|show]\r\n");
}

/* ═══ interleave ═════════════════════════════════════════════════════ */

static void test_interleave(void)
{
    static const char skew_usage[] =
        "usage: fpga interleave skew [measure|set <ppm>|off]  ppm = B's deviation from half a "
        "converter period, ppm of a period (B at 0.451 -> -49000), |ppm| <= 150000\r\n";
    fresh();
    SAYS(shell_alias_interleave, "", "<INTERLEAVE 0>\r\n");
    OPS("");
    SAYS(shell_alias_interleave, "off", "<INTERLEAVE 0>\r\n");
    OPS("il(0);");
    SAYS(shell_alias_interleave, "on", "<INTERLEAVE 0>\r\n");
    CHECK(g_il_on);
    SAYS(shell_alias_interleave, "maybe", "usage: fpga interleave [on|off|corr <g> <o>|cal|skew ...]\r\n");
    /* corr: gain x10000 and offset x100, the old arguments */
    SAYS(shell_alias_interleave, "corr 10123 -45", "<INTERLEAVE 0>\r\n");
    CHECK(g_il_gain == 1012300 && g_il_off == -45);
    SAYS(shell_alias_interleave, "corr 4999 0",
         "usage: fpga interleave corr <gain x10000, 5000-20000> <offset x100, signed>\r\n");
    SAYS(shell_alias_interleave, "corr 10000",
         "usage: fpga interleave corr <gain x10000, 5000-20000> <offset x100, signed>\r\n");
    SAYS(shell_alias_interleave, "corr 10000 10001",
         "usage: fpga interleave corr <gain x10000, 5000-20000> <offset x100, signed>\r\n");
    /* D8: with a piecewise map the old command replaced it by the line; so
     * does the alias (a bare `set il.gain` is refused there) */
    g_il_knots = 5;
    SAYS(shell_alias_interleave, "corr 9900 20", "<INTERLEAVE 0>\r\n");
    OPS("il_corr(1012300,-45);il_corr(990000,-45);il_corr(990000,20);");
    CHECK(g_il_knots == 0 && g_il_gain == 990000 && g_il_off == 20);
    /* skew */
    SAYS(shell_alias_interleave, "skew", "<SKEW 0>\r\n");
    SAYS(shell_alias_interleave, "skew set -49000", "<SKEW 0>\r\n");
    OPS("il_skew(-49000);");
    CHECK(g_il_skew == -49000);
    SAYS(shell_alias_interleave, "skew set +1200", "<SKEW 0>\r\n");
    CHECK(g_il_skew == 1200);
    SAYS(shell_alias_interleave, "skew off", "<SKEW 0>\r\n");
    CHECK(g_il_skew == 0);
    SAYS(shell_alias_interleave, "skew set 150001", skew_usage);
    SAYS(shell_alias_interleave, "skew set", "<SKEW 0>\r\n");          /* as before: parse_int("") is 0 */
    SAYS(shell_alias_interleave, "skew wide", skew_usage);
    g_fail = true;                                                     /* the owner refuses: usage, as before */
    SAYS(shell_alias_interleave, "skew set 500", skew_usage);
    g_fail = false;
}

/* ═══ generator ══════════════════════════════════════════════════════ */

static void test_siggen(void)
{
    const siggen_config_t *c = siggen_get_config();
    fresh();
    SAYS(shell_alias_siggen, "", "<SIGGEN 0>\r\n");
    OPS("");
    SAYS(shell_alias_siggen, "wave 3 amp 15 freq 2000 on", "<SIGGEN 0>\r\n");
    OPS("gen_wave(3);gen_amp(15);gen_freq(2000);gen_out(1);");        /* left to right */
    CHECK(c->waveform == 3 && c->amplitude_dv == 15 && c->frequency_hz == 2000 && c->output_enabled);
    CHECK(n_sent == 4);                                                /* one request per token */
    SAYS(shell_alias_siggen, "duty 25 boot on off", "<SIGGEN 0>\r\n");
    OPS("gen_duty(25);gen_boot(1);gen_out(0);");
    /* a bad token stops the line; what came before it stays applied */
    SAYS(shell_alias_siggen, "freq 1000 wave 13 on", "siggen: wave 0..12\r\n");
    OPS("gen_freq(1000);");
    CHECK(c->frequency_hz == 1000 && !c->output_enabled);
    SAYS(shell_alias_siggen, "freq 50001", "siggen: freq 0..50000 Hz\r\n");
    SAYS(shell_alias_siggen, "amp 31", "siggen: amp 0..30 (0.1 V)\r\n");
    SAYS(shell_alias_siggen, "duty 101", "siggen: duty 0..100\r\n");
    SAYS(shell_alias_siggen, "freq", "siggen: 'freq' needs a number\r\n");
    SAYS(shell_alias_siggen, "freq fast", "siggen: 'freq' needs a number\r\n");
    SAYS(shell_alias_siggen, "phase 3", "siggen: unknown 'phase'\r\n");
    SAYS(shell_alias_siggen, "boot", "siggen: boot on|off\r\n");
    SAYS(shell_alias_siggen, "boot maybe", "siggen: boot on|off\r\n");
    /* dump: ten lines of ten samples, then the state block */
    g_out[0] = '\0';
    shell_alias_siggen("dump", &k_env);
    {
        const uint16_t *cy = siggen_get_cycle();
        char want[1200] = "", t[96];
        for (unsigned i = 0; i < SIGGEN_CYCLE_SAMPLES; i += 10) {
            snprintf(t, sizeof t, "  %3u: %4u %4u %4u %4u %4u %4u %4u %4u %4u %4u\r\n", i,
                     cy[i], cy[i + 1], cy[i + 2], cy[i + 3], cy[i + 4],
                     cy[i + 5], cy[i + 6], cy[i + 7], cy[i + 8], cy[i + 9]);
            strcat(want, t);
        }
        strcat(want, "<SIGGEN 0>\r\n");
        CHECK(strcmp(g_out, want) == 0);
    }
    responsive = false;                                                /* D2 */
    SAYS(shell_alias_siggen, "freq 300 on", TIMEOUT_NOTHING);
    responsive = true;
    (void)model_exec_drain();
    CHECK(c->frequency_hz == 1000);
}

/* ═══ mask ═══════════════════════════════════════════════════════════ */

static void test_mask(void)
{
    static const char not_applied[] =
        "NOT APPLIED: the acquisition loop did not pick the request up in 400 ms\r\n"
        "(not a live-scope build, scope not initialised, or acq parked).\r\n";
    fresh();
    SAYS(shell_alias_mask_tol, " 12 4", "<MASK 0>\r\n");
    CHECK(g_mask_v == 12 && g_mask_h == 4);
    SAYS(shell_alias_mask_tol, " 6", "<MASK 0>\r\n");                  /* h not given: the default, as before */
    CHECK(g_mask_v == 6 && g_mask_h == 2);
    SAYS(shell_alias_mask_tol, " 256", "usage: mask tol <v counts 0..255> [h samples 0..16]\r\n");
    SAYS(shell_alias_mask_tol, " 8 17", "usage: mask tol <v counts 0..255> [h samples 0..16]\r\n");
    SAYS(shell_alias_mask_tol, " wide", "usage: mask tol <v counts 0..255> [h samples 0..16]\r\n");
    /* D7: the model's range is 0..64 */
    SAYS(shell_alias_mask_tol, " 200 3",
         "mask tol: v 200 is past the model's range, set to 64 (mask.tolv 0..64)\r\n<MASK 0>\r\n");
    CHECK(g_mask_v == 64 && g_mask_h == 3);
    SAYS(shell_alias_mask_stop, " off", "<MASK 0>\r\n");
    CHECK(!g_mask_stop);
    SAYS(shell_alias_mask_stop, " on", "<MASK 0>\r\n");
    CHECK(g_mask_stop);
    SAYS(shell_alias_mask_stop, "", "usage: mask stop on|off\r\n");
    /* the acquisition task does not pick it up: the old words */
    g_mask_dead = true;
    SAYS(shell_alias_mask_tol, " 20 5", not_applied);
    SAYS(shell_alias_mask_stop, " off", not_applied);
}

/* ═══ meter and mode ═════════════════════════════════════════════════ */

static void test_meter(void)
{
    fresh();
    /* on the scope with the link off: the old refusal, nothing asked */
    SAYS(shell_alias_meter_func, "6", "meter func: the meter link is off (`mode meter`, or see `meter state`)\r\n");
    OPS("");
    CHECK(meter_submode == 0 && n_sent == 0);
    SAYS(shell_alias_meter_func, "", "usage: meter func <0-11>  (0 DCV 1 ACV 2 DCmA 3 DCA 4 ACmA 5 ACA 6 Ohm 7 Cont 8 Diode 9 Cap 10 Temp 11 Auto)\r\n");
    SAYS(shell_alias_meter_func, "12", "usage: meter func <0-11>  (0 DCV 1 ACV 2 DCmA 3 DCA 4 ACmA 5 ACA 6 Ohm 7 Cont 8 Diode 9 Cap 10 Temp 11 Auto)\r\n");
    current_mode = MODE_MULTIMETER;
    SAYS(shell_alias_meter_func, "6", "<METER_FUNC_BEGIN 6>\r\n<METER_FUNC 6>\r\n");
    OPS("meter_func(6);");
    CHECK(meter_submode == 6);
    current_mode = MODE_OSCILLOSCOPE;                                  /* the link on under the scope */
    g_link_on = true;
    SAYS(shell_alias_meter_func, "1", "<METER_FUNC_BEGIN 1>\r\n<METER_FUNC 1>\r\n");
    OPS("meter_func(1);");
    g_fail = true;                                                     /* not posted: no report, the state kept */
    SAYS(shell_alias_meter_func, "2", "<METER_FUNC_BEGIN 2>\r\nNOT_SET meter.func (meter posture NOT poste)\r\n");   /* the note is 23 characters */
    CHECK(meter_submode == 1);
    g_fail = false;

    SAYS(shell_alias_meter_degf, "", "meter degF: off\r\n");
    SAYS(shell_alias_meter_degf, "on", "meter degF: on\r\n");
    OPS("flag(2,1,1);");                                               /* the key's action (user = true) */
    SAYS(shell_alias_meter_degf, " off", "meter degF: off\r\n");
    SAYS(shell_alias_meter_degf, "kelvin", "usage: meter degf [on|off]\r\n");
}

static void test_mode(void)
{
    fresh();
    startup_mode = STARTUP_METER;
    meter_submode = 6;
    meter_layout = 1;
    SAYS(shell_alias_mode, "", "current=0 startup=Meter meter_submode=6 (Ohm) layout=1 (chart)\r\n");
    SAYS(shell_alias_mode, "meter", "mode=meter submode=6 (Ohm) layout=1 (chart)\r\n");
    OPS("meter-stored(6);mode(1);");                       /* the function, then the entry that applies it */
    CHECK(current_mode == MODE_MULTIMETER && g_begins == 1 && g_waits == 1);
    SAYS(shell_alias_mode, "meter 3 2", "mode=meter submode=3 (DCA) layout=2 (stats)\r\n");
    OPS("meter_func(3);layout(2);mode(1);");
    CHECK(meter_submode == 3 && meter_layout == 2);
    SAYS(shell_alias_mode, "1 0", "mode=meter submode=0 (DCV) layout=2 (stats)\r\n");      /* the numeric form */
    SAYS(shell_alias_mode, "meter 12", "Usage: mode meter [0-11] [0-4]\r\n");
    SAYS(shell_alias_mode, "meter 0 5", "Usage: mode meter [0-11] [0-4]\r\n");
    SAYS(shell_alias_mode, "meter 6 3", "mode=meter submode=0 (DCV) layout=3 (fuse)\r\n"); /* D9: the fuse tester reads DC V */
    SAYS(shell_alias_mode, "scope", "mode=scope\r\n");
    OPS("mode(0);");
    CHECK(current_mode == MODE_OSCILLOSCOPE);
    SAYS(shell_alias_mode, "0", "mode=scope\r\n");
    SAYS(shell_alias_mode, "2", "mode: unrecognised argument '2'\r\n"
         "Usage: mode [scope|0 | meter|1 [submode] [layout] | startup [scope|meter]]\r\n");
    SAYS(shell_alias_mode, "siggen", "mode: unrecognised argument 'siggen'\r\n"
         "Usage: mode [scope|0 | meter|1 [submode] [layout] | startup [scope|meter]]\r\n");
    g_busy = true;                                                     /* D2: as the long mode keys answer "Busy" */
    SAYS(shell_alias_mode, "meter", "BUSY sys.mode (self-cal running)\r\n");
    g_busy = false;

    /* startup */
    SAYS(shell_alias_mode, "startup", "startup=Meter\r\n");
    SAYS(shell_alias_mode, "startup scope", "startup=Scope (saved)\r\n");
    CHECK(startup_mode == STARTUP_SCOPE);
    SAYS(shell_alias_mode, "startup meter", "startup=Meter (saved)\r\n");
    SAYS(shell_alias_mode, "startup home", "startup=Home (saved)\r\n");
    SAYS(shell_alias_mode, "startup siggen", "startup=Signal Gen (saved)\r\n");
    SAYS(shell_alias_mode, "startup restore", "startup=Last Used (saved)\r\n");            /* D6 */
    CHECK(startup_mode == STARTUP_RESTORE);
    SAYS(shell_alias_mode, "startup never", "Usage: mode startup [home|meter|scope|siggen|restore]\r\n");
    g_flush_answers = false;
    SAYS(shell_alias_mode, "startup home", "startup=Home (save pending: input task did not answer in 500 ms)\r\n");
    responsive = false;                                                /* D2 */
    SAYS(shell_alias_mode, "startup scope", TIMEOUT_NOTHING);
    responsive = true;
    (void)model_exec_drain();
    CHECK(startup_mode == STARTUP_HOME);
}

int main(void)
{
    test_vdiv();
    test_ch();
    test_probe();
    test_cal_pos();
    test_timebase();
    test_roll();
    test_trigger();
    test_view();
    test_interleave();
    test_siggen();
    test_mask();
    test_meter();
    test_mode();
    printf("test_shell_alias: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
