/*
 * Host test for the executor (src/model/model_exec.c) and the shell's raw
 * route (src/model/model_shell.c), behind a host port instead of FreeRTOS:
 *
 *   - not up -> BUSY; called on the executor -> BUSY (no self-wait)
 *   - queue order (posts and requests applied in arrival order)
 *   - seq-tagged result slots; a late answer is never read as the next one's
 *   - timeout: withdrawn (never applied later) vs in progress (threaded case)
 *   - seq wrap past 0xFFFFFFFF (0 is skipped, the withdrawal compare holds)
 *   - all-or-nothing validation on both sides; stop at the first failure
 *     with failed/applied; check-in between pairs; one redraw per request
 *   - the not-wired list refuses before anything is queued
 *   - queue full -> BUSY; foreign-context counter
 *   - shell lines: OK / CLAMPED / NOT_SET / INVALID / REFUSED / TIMEOUT, and
 *     every `get *` line parses back through prop_line_parse to its value
 *
 * Build/run:  make -C firmware test-model-exec
 */
#define MODEL_INTERNAL
#include "model.h"
#include "model_exec.h"
#include "model_shell.h"
#include "scope_state.h"
#include "meas_select.h"
#include "signal_gen.h"
#include "sys_prefs.h"
#include "theme.h"
#include "math_channel.h"
#include "ui.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECKF(c, ...) do { checks++; if (!(c)) { fails++; \
    printf("FAIL %s:%d  %s: ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

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

/* ═══ fake hardware ops ═════════════════════════════════════════════ */
static char     g_log[2048];
static bool     g_fail, g_busy;
static unsigned g_fail_at, g_calls;
static volatile bool g_block_vdiv, g_in_vdiv;   /* threaded case: the op holds */

static void logf_(const char *fmt, ...)
{
    char t[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    strncat(g_log, t, sizeof g_log - strlen(g_log) - 1);
}
static bool nok(void) { return g_fail || (g_fail_at && ++g_calls == g_fail_at); }

static bool f_vdiv(uint8_t ch, uint8_t i)
{
    g_in_vdiv = true;
    while (g_block_vdiv) usleep(1000);
    g_in_vdiv = false;
    logf_("vdiv(%u,%u);", ch, i);
    return !nok();
}
static bool f_coupling(uint8_t ch, uint8_t c) { logf_("coupling(%u,%u);", ch, c); return !nok(); }
static bool f_timebase(uint8_t c)             { logf_("tb(%u);", c); return !nok(); }
static void f_gen_freq(uint32_t f)            { logf_("gen_freq(%u);", (unsigned)f); siggen_set_frequency(f); }
static void f_gen_amp(uint8_t a)              { logf_("gen_amp(%u);", a); siggen_set_amplitude_dv(a); }
static void f_gen_out(bool on)                { logf_("gen_out(%d);", on); siggen_enable(on); }
static bool f_busy(void)                      { return g_busy; }
static bool     g_shell_persist = true;      /* `persist off` for bench sweeps when false */
static unsigned g_notes;                     /* settings_store_note_change calls */
static bool f_shell_persist(void)             { return g_shell_persist; }
static void f_note_change(void)               { g_notes++; }

static const model_hw_ops_t k_fake = {
    .vdiv = f_vdiv, .coupling = f_coupling, .timebase = f_timebase,
    .gen_freq = f_gen_freq, .gen_amp = f_gen_amp, .gen_out = f_gen_out,
    .selfcal_busy = f_busy, .shell_persist = f_shell_persist, .note_change = f_note_change,
};

/* ═══ the single-threaded host port ═════════════════════════════════ */
static model_req_t q[MODEL_EXEC_QUEUE_LEN];
static unsigned q_head, q_count;
static unsigned n_wakes, n_sleeps, n_checkins, n_changed, n_service;
static bool     responsive = true;     /* a sleep lets the "input task" run the drain */
static bool     in_exec;               /* the drain is running (on_executor)          */
static int      last_origin = -1;      /* origin of the last request queued            */
static unsigned n_sent;

static bool tp_send(const model_req_t *r)
{
    last_origin = r->origin;
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
static void run_executor(void) { in_exec = true; (void)model_exec_drain(); in_exec = false; }
static void tp_wake(void)      { n_wakes++; }
static void tp_sleep(uint32_t ms) { (void)ms; n_sleeps++; if (responsive) run_executor(); }
static void tp_checkin(void)   { n_checkins++; }
static void tp_changed(void)   { n_changed++; }
static void tp_service(void)   { n_service++; }
static bool tp_on_exec(void)   { return in_exec; }
static const char *tp_unwired(uint16_t id)
{
    return (id == PROP_ID_SYS_MODE || id == PROP_ID_SYS_INSTRUMENT) ? "mode switch: keys (B1)" : NULL;
}

static const model_exec_port_t k_port = {
    .send = tp_send, .receive = tp_receive, .wake = tp_wake, .sleep_ms = tp_sleep,
    .checkin = tp_checkin, .changed = tp_changed, .service = tp_service,
    .on_executor = tp_on_exec, .unwired = tp_unwired,
};

static void fresh_state(void)
{
    scope_state_init(scope_state_get());
    meas_select_defaults();
    siggen_init(NULL);
    sys_prefs_set_brightness(SYS_BRIGHT_DEFAULT);
    sys_prefs_set_volume(SYS_VOL_DEFAULT);
    theme_init(THEME_DARK_BLUE);
    model_bind_hw(&k_fake);
    (void)model_load_defaults(PF_PERSIST | PF_SESSION | PF_USERCAL, NULL, false);
    current_mode = MODE_OSCILLOSCOPE;    /* (sys.instrument's built-in default is the meter) */
    g_log[0] = '\0';
    g_fail = g_busy = false;
    g_fail_at = g_calls = 0;
    g_block_vdiv = false;
    g_shell_persist = true;
    g_notes = 0;
}

static void fresh(uint32_t first_seq)
{
    fresh_state();
    q_head = q_count = 0;
    last_origin = -1;
    n_sent = 0;
    n_wakes = n_sleeps = n_checkins = n_changed = n_service = 0;
    responsive = true;
    in_exec = false;
    model_exec_test_reset(first_seq);
    model_exec_init(&k_port);
}

static prop_pair_t pr(uint16_t id, int32_t v) { prop_pair_t p = { id, v }; return p; }

static prop_status_t req1(uint16_t id, int32_t v, uint32_t ms, model_result_t *r)
{
    prop_pair_t p = pr(id, v);
    return prop_request(&p, 1, ORIGIN_SHELL, ms, r);
}

/* ═══ executor ══════════════════════════════════════════════════════ */

static void test_not_up(void)
{
    fresh_state();
    model_exec_test_reset(1);
    model_result_t r;
    CHECK(req1(PROP_ID_CH1_VDIV, 4, 600, &r) == PS_BUSY);
    CHECK(!prop_post(PROP_ID_CH1_VDIV, 4, ORIGIN_AUTO));
    CHECK(model_exec_drain() == 0);
    CHECK(prop_get(P(CH1_VDIV)) == 3);
}

static void test_basic(void)
{
    fresh(1);
    model_result_t r;
    CHECK(req1(PROP_ID_CH1_VDIV, 4, 600, &r) == PS_OK);
    CHECK(r.seq == 1 && r.status == PS_OK && r.failed == 1 && r.applied == 1 && !r.in_progress);
    CHECK(prop_get(P(CH1_VDIV)) == 4);
    CHECKF(strcmp(g_log, "vdiv(0,4);") == 0, "%s", g_log);
    CHECK(n_wakes == 1 && n_sleeps == 1 && n_changed == 1);
    CHECK(model_foreign_sets == 0);
    /* the same value again: OK, no change, no redraw */
    CHECK(req1(PROP_ID_CH1_VDIV, 4, 600, &r) == PS_OK && r.seq == 2);
    CHECK(n_changed == 1);
    /* an int is clamped: CLAMPED and the clamped value */
    CHECK(req1(PROP_ID_GEN_FREQ, 99999, 600, &r) == PS_CLAMPED);
    CHECK(prop_get(P(GEN_FREQ)) == 50000);
    model_exec_stats_t st;
    model_exec_stats(&st);
    CHECK(st.requests == 3 && st.applied == 3 && st.timeouts == 0 && st.withdrawn == 0);
}

static void test_order(void)
{
    fresh(1);
    CHECK(prop_post(PROP_ID_CH1_VDIV, 5, ORIGIN_AUTO));
    CHECK(prop_post(PROP_ID_GEN_FREQ, 2000, ORIGIN_AUTO));
    CHECK(prop_post(PROP_ID_CH1_VDIV, 6, ORIGIN_AUTO));
    CHECK(q_count == 3 && n_wakes == 3);
    model_result_t r;
    prop_pair_t p = pr(PROP_ID_CH2_COUPLING, 1);
    CHECK(prop_request(&p, 1, ORIGIN_SHELL, 600, &r) == PS_OK);   /* the drain takes all four */
    CHECKF(strcmp(g_log, "vdiv(0,5);gen_freq(2000);vdiv(0,6);coupling(1,1);") == 0, "%s", g_log);
    CHECK(prop_get(P(CH1_VDIV)) == 6 && prop_get(P(CH2_COUPLING)) == 1);
    CHECK(r.seq == 1);                  /* posts carry no seq: the first request is 1 */
    /* one redraw per request (posts are AUTO: they redraw too) */
    CHECK(n_changed == 4);
}

static void test_all_or_nothing(void)
{
    fresh(1);
    model_result_t r;
    /* caller side: the second pair is out of range -> nothing queued */
    prop_pair_t a[2] = { pr(PROP_ID_CH1_VDIV, 4), pr(PROP_ID_CH1_COUPLING, 7) };
    CHECK(prop_request(a, 2, ORIGIN_SHELL, 600, &r) == PS_INVALID);
    CHECK(r.failed == 1 && q_count == 0 && n_wakes == 0 && g_log[0] == '\0');
    CHECK(prop_get(P(CH1_VDIV)) == 3);
    /* executor side: a post with a bad value applies nothing */
    CHECK(prop_post(PROP_ID_CH1_COUPLING, 7, ORIGIN_AUTO));
    run_executor();
    CHECK(g_log[0] == '\0' && prop_get(P(CH1_COUPLING)) == 0);
    /* a read-only property */
    CHECK(req1(PROP_ID_UI_FOCUS, 1, 600, &r) == PS_INVALID && q_count == 0);
    /* 5 PF_HW pairs, 9 pairs */
    prop_pair_t h[9] = { pr(PROP_ID_CH1_VDIV, 1), pr(PROP_ID_CH2_VDIV, 1), pr(PROP_ID_TB_CODE, 1),
                         pr(PROP_ID_CH1_COUPLING, 1), pr(PROP_ID_CH2_COUPLING, 1),
                         pr(PROP_ID_CH1_PROBE, 1), pr(PROP_ID_CH2_PROBE, 1), pr(PROP_ID_TRIG_EDGE, 1),
                         pr(PROP_ID_VIEW_TRUESCALE, 1) };
    CHECK(prop_request(h, 5, ORIGIN_SHELL, 600, &r) == PS_INVALID && r.failed == 5);
    CHECK(prop_request(h, 9, ORIGIN_SHELL, 600, &r) == PS_INVALID && r.failed == 9);
    CHECK(q_count == 0 && g_log[0] == '\0');
    /* 4 PF_HW + 4 plain = 8 pairs: fine */
    prop_pair_t ok[8] = { h[0], h[1], h[2], h[3], h[5], h[6], h[7], h[8] };
    CHECK(prop_request(ok, 8, ORIGIN_SHELL, 1500, &r) == PS_OK && r.applied == 8);
    CHECK(n_checkins >= 7 + 1);         /* between pairs, and after the request */
}

static void test_stop_at_failure(void)
{
    fresh(1);
    model_result_t r;
    prop_pair_t a[3] = { pr(PROP_ID_CH1_VDIV, 2), pr(PROP_ID_CH2_VDIV, 2), pr(PROP_ID_TB_CODE, 5) };
    g_fail_at = 2;                       /* the second owner call says no */
    CHECK(prop_request(a, 3, ORIGIN_SHELL, 1500, &r) == PS_NOT_SET);
    CHECK(r.failed == 1 && r.applied == 1);
    CHECKF(strcmp(r.note.text, "relay apply failed") == 0, "%s", r.note.text);
    CHECK(prop_get(P(CH1_VDIV)) == 2);   /* applied, not rolled back */
    CHECK(prop_get(P(CH2_VDIV)) == 5);   /* NOT_SET keeps the old value */
    CHECK(prop_get(P(TB_CODE)) == 10);   /* never reached */
    CHECKF(strcmp(g_log, "vdiv(0,2);vdiv(1,2);") == 0, "%s", g_log);
    /* self-cal running: scope sets from the shell are BUSY */
    g_fail_at = 0;
    g_busy = true;
    CHECK(req1(PROP_ID_CH1_VDIV, 4, 600, &r) == PS_BUSY && r.failed == 0);
    CHECK(prop_get(P(CH1_VDIV)) == 2);
    g_busy = false;
}

static void test_unwired(void)
{
    fresh(1);
    model_result_t r;
    CHECK(req1(PROP_ID_SYS_MODE, MODE_MULTIMETER, 600, &r) == PS_REFUSED);
    CHECK(r.failed == 0 && q_count == 0 && n_wakes == 0);
    CHECK(strcmp(r.note.text, "mode switch: keys (B1)") == 0);
    CHECK(prop_unwired_reason(P(SYS_INSTRUMENT)) != NULL);
    CHECK(prop_unwired_reason(P(CH1_VDIV)) == NULL);
    /* the drain refuses a post too */
    CHECK(prop_post(PROP_ID_SYS_MODE, MODE_MULTIMETER, ORIGIN_AUTO));
    run_executor();
    CHECK(current_mode == MODE_OSCILLOSCOPE);
}

static void test_timeout_withdrawn(void)
{
    fresh(1);
    model_result_t r;
    responsive = false;                  /* the input task is stuck (gallery, countdown) */
    CHECK(req1(PROP_ID_CH1_VDIV, 7, 50, &r) == PS_TIMEOUT);
    CHECK(!r.in_progress && r.seq == 1 && r.applied == 0);
    CHECK(n_sleeps == 5);                /* 50 ms in 10 ms polls, then gave up */
    CHECK(q_count == 1);
    /* the input task comes back: the withdrawn request is skipped */
    responsive = true;
    CHECK(req1(PROP_ID_CH1_VDIV, 2, 600, &r) == PS_OK);
    CHECK(r.seq == 2 && prop_get(P(CH1_VDIV)) == 2);
    CHECKF(strcmp(g_log, "vdiv(0,2);") == 0, "%s", g_log);   /* 7 was never applied */
    model_exec_stats_t st;
    model_exec_stats(&st);
    CHECK(st.withdrawn == 1 && st.timeouts == 1 && st.applied == 1);
}

static void test_queue_full(void)
{
    fresh(1);
    model_result_t r;
    responsive = false;
    for (int i = 0; i < 4; i++) CHECK(prop_post(PROP_ID_GEN_FREQ, 100 + i, ORIGIN_AUTO));
    CHECK(!prop_post(PROP_ID_GEN_FREQ, 200, ORIGIN_AUTO));
    CHECK(req1(PROP_ID_CH1_VDIV, 4, 600, &r) == PS_BUSY);
    CHECK(strcmp(r.note.text, "request queue full") == 0 && n_sleeps == 0);
    model_exec_stats_t st;
    model_exec_stats(&st);
    CHECK(st.full == 2);
    run_executor();
    CHECK(prop_get(P(GEN_FREQ)) == 103 && q_count == 0);
}

static void test_seq_wrap(void)
{
    fresh(0xFFFFFFFEu);
    model_result_t r;
    CHECK(req1(PROP_ID_CH1_VDIV, 1, 600, &r) == PS_OK && r.seq == 0xFFFFFFFEu);
    responsive = false;
    CHECK(req1(PROP_ID_CH1_VDIV, 8, 30, &r) == PS_TIMEOUT && r.seq == 0xFFFFFFFFu);
    responsive = true;
    CHECK(req1(PROP_ID_CH1_VDIV, 2, 600, &r) == PS_OK);
    CHECK(r.seq == 1u);                  /* 0 is skipped */
    CHECK(prop_get(P(CH1_VDIV)) == 2);
    CHECKF(strcmp(g_log, "vdiv(0,1);vdiv(0,2);") == 0, "%s", g_log);   /* 8: withdrawn */
    CHECK(req1(PROP_ID_CH1_VDIV, 3, 600, &r) == PS_OK && r.seq == 2u);
}

static void test_context(void)
{
    fresh(1);
    model_result_t r;
    in_exec = true;                      /* a request from the executor would wait for itself */
    CHECK(req1(PROP_ID_CH1_VDIV, 4, 600, &r) == PS_BUSY && q_count == 0);
    model_exec_check_context();
    CHECK(model_foreign_sets == 0);
    in_exec = false;
    model_exec_check_context();          /* an apply op on another task */
    CHECK(model_foreign_sets == 1);
    (void)model_exec_drain();            /* a drain on another task */
    CHECK(model_foreign_sets == 2);
    CHECK(n_service == 1);
}

/* ═══ threaded: a real executor, a hold in the owner call ═══════════ */

static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_t exec_thread;
static volatile bool exec_stop;

static bool th_send(const model_req_t *r)  { pthread_mutex_lock(&mx); bool b = tp_send(r); pthread_mutex_unlock(&mx); return b; }
static bool th_receive(model_req_t *r)     { pthread_mutex_lock(&mx); bool b = tp_receive(r); pthread_mutex_unlock(&mx); return b; }
/* While the owner call is held, the caller's clock waits for the executor
 * to be inside it, so "in progress" does not depend on host scheduling. */
static void th_sleep(uint32_t ms)
{
    usleep(ms * 1000u);
    while (g_block_vdiv && !g_in_vdiv) usleep(1000);
}
static bool th_on_exec(void)               { return pthread_equal(pthread_self(), exec_thread); }
static void th_nop(void)                   {}

static const model_exec_port_t k_thread_port = {
    .send = th_send, .receive = th_receive, .wake = th_nop, .sleep_ms = th_sleep,
    .checkin = th_nop, .changed = th_nop, .service = th_nop,
    .on_executor = th_on_exec, .unwired = tp_unwired,
};

static void *exec_main(void *arg)
{
    (void)arg;
    while (!exec_stop) { (void)model_exec_drain(); usleep(1000); }
    return NULL;
}

static void test_threaded_in_progress(void)
{
    fresh_state();
    q_head = q_count = 0;
    model_exec_test_reset(1);
    model_exec_init(&k_thread_port);
    exec_stop = false;
    CHECK(pthread_create(&exec_thread, NULL, exec_main, NULL) == 0);

    model_result_t r;
    CHECK(req1(PROP_ID_GEN_AMP, 15, 600, &r) == PS_OK && r.seq == 1);   /* the executor runs */

    g_block_vdiv = true;                 /* the owner call holds, like a 1 s fpga_acq_pause */
    CHECK(req1(PROP_ID_CH1_VDIV, 7, 40, &r) == PS_TIMEOUT);
    CHECK(r.in_progress && r.seq == 2);  /* it had started: "still applying" */
    CHECK(g_in_vdiv);
    g_block_vdiv = false;                /* it finishes late, into slot 2 */
    for (int i = 0; i < 200 && prop_get(P(CH1_VDIV)) != 7; i++) usleep(1000);
    CHECK(prop_get(P(CH1_VDIV)) == 7);

    /* The next request reads its own answer, not request 2's late one. */
    g_fail = true;
    CHECK(req1(PROP_ID_CH2_VDIV, 1, 600, &r) == PS_NOT_SET);
    CHECK(r.seq == 3 && r.failed == 0 && r.applied == 0);
    g_fail = false;
    CHECK(req1(PROP_ID_GEN_OUT, 1, 600, &r) == PS_OK && r.seq == 4);
    CHECK(model_foreign_sets == 0);

    exec_stop = true;
    pthread_join(exec_thread, NULL);
}

/* ═══ the shell ═════════════════════════════════════════════════════ */

static char out[64][192];
static unsigned n_out;
static void collect(const char *line)
{
    if (n_out < 64) { strncpy(out[n_out], line, sizeof out[0] - 1); out[n_out][sizeof out[0] - 1] = '\0'; }
    n_out++;
}
static void sh_reset(void) { n_out = 0; }
static bool starts(unsigned i, const char *s) { return i < n_out && i < 64 && strncmp(out[i], s, strlen(s)) == 0; }

static void test_shell_set(void)
{
    fresh(1);
    sh_reset(); model_shell_set("ch1.vdiv 4", collect);
    CHECKF(n_out == 1 && starts(0, "OK ch1.vdiv=4  # "), "%s", out[0]);
    sh_reset(); model_shell_set("gen.amp 1.5 V", collect);
    CHECKF(n_out == 1 && strcmp(out[0], "OK gen.amp=1.5 V") == 0, "%s", out[0]);
    sh_reset(); model_shell_set("gen.freq 99999", collect);
    CHECKF(n_out == 1 && strcmp(out[0], "CLAMPED gen.freq=50000 Hz") == 0, "%s", out[0]);
    sh_reset(); model_shell_set("ch1.coupling=AC ch1.vdiv=+1", collect);
    CHECKF(n_out == 2 && strcmp(out[0], "OK ch1.coupling=AC") == 0 && starts(1, "OK ch1.vdiv=5"),
           "%s | %s", out[0], out[1]);
    g_fail = true;
    sh_reset(); model_shell_set("ch1.vdiv 6", collect);
    CHECKF(n_out == 1 && starts(0, "NOT_SET ch1.vdiv (relay apply failed); old value kept: 5"), "%s", out[0]);
    g_fail_at = 2; g_calls = 0; g_fail = false;
    sh_reset(); model_shell_set("ch1.vdiv=1 ch2.vdiv=1 tb.code=4", collect);
    CHECKF(n_out == 3 && starts(0, "OK ch1.vdiv=1") && starts(1, "NOT_SET ch2.vdiv (relay apply failed)") &&
           strcmp(out[2], "  not applied: tb.code") == 0, "%s | %s | %s", out[0], out[1], out[2]);
    g_fail_at = 0;
    sh_reset(); model_shell_set("nope 1", collect);
    CHECK(n_out == 1 && strcmp(out[0], "INVALID nope (unknown property); nothing applied") == 0);
    sh_reset(); model_shell_set("ui.focus 1", collect);
    CHECK(n_out == 1 && strcmp(out[0], "INVALID ui.focus (read only); nothing applied") == 0);
    sh_reset(); model_shell_set("ch1.coupling GND", collect);
    CHECKF(n_out == 1 && starts(0, "INVALID ch1.coupling=GND (bad value"), "%s", out[0]);
    sh_reset(); model_shell_set("ch1.coupling 2", collect);
    CHECKF(n_out == 1 && starts(0, "INVALID ch1.coupling=2 (read only/out of range); nothing applied"), "%s", out[0]);
    sh_reset(); model_shell_set("sys.mode meter", collect);
    CHECKF(n_out == 1 && starts(0, "REFUSED sys.mode (mode switch: keys (B1)); old value kept: scope"), "%s", out[0]);
    sh_reset(); model_shell_set("ch1.vdiv=4 sys.mode=meter", collect);
    CHECKF(n_out == 1 && starts(0, "REFUSED sys.mode (mode switch: keys (B1)); old value kept: scope; nothing applied"),
           "%s", out[0]);
    CHECK(prop_get(P(CH1_VDIV)) == 1);   /* unchanged by the refused pair set */
    sh_reset(); model_shell_set("ch1.vdiv", collect);
    CHECKF(n_out == 1 && starts(0, "INVALID ch1.vdiv= (bad value"), "%s", out[0]);
    responsive = false;
    sh_reset(); model_shell_set("ch1.vdiv 3", collect);
    CHECKF(n_out == 1 && strcmp(out[0], "TIMEOUT: input task busy (gallery/countdown/selfcal?); nothing applied") == 0,
           "%s", out[0]);
    CHECK(n_sleeps >= MODEL_EXEC_HW_TIMEOUT_MS / MODEL_EXEC_POLL_MS);   /* PF_HW: the 1.5 s budget */
    unsigned s0 = n_sleeps;
    sh_reset(); model_shell_set("ch1.probe 10x", collect);
    CHECK(n_sleeps - s0 == MODEL_EXEC_TIMEOUT_MS / MODEL_EXEC_POLL_MS);  /* no HW: 600 ms */
    responsive = true;
    sh_reset(); model_shell_set("ch1.bwlimit on", collect);
    CHECK(prop_get(P(CH1_VDIV)) == 1 && prop_get(P(CH1_PROBE)) == 0);   /* withdrawn, not applied */
}

static void test_shell_get_roundtrip(void)
{
    fresh(1);
    sh_reset(); model_shell_get("*", collect);
    CHECK(n_out == prop_count());
    for (unsigned i = 0; i < n_out && i < 64; i++) {
        const prop_desc_t *p = NULL;
        int32_t v = 0;
        const prop_status_t s = prop_line_parse(out[i], &p, &v);
        CHECKF(s == PS_OK && p == prop_at((uint16_t)i) && v == prop_get(p), "line '%s'", out[i]);
    }
    sh_reset(); model_shell_get("ch1.*", collect);
    CHECK(n_out == 7);
    sh_reset(); model_shell_get("gen.amp tb.code", collect);
    CHECKF(n_out == 2 && strcmp(out[0], "gen.amp=3.0 V") == 0 && starts(1, "tb.code=10  # "), "%s | %s", out[0], out[1]);
    sh_reset(); model_shell_get("bogus", collect);
    CHECK(n_out == 1 && starts(0, "ERR unknown property 'bogus'"));
    sh_reset(); model_shell_get("zz.*", collect);
    CHECK(n_out == 1 && starts(0, "ERR no property matches"));
    sh_reset(); model_shell_list("", collect);
    CHECK(n_out == (unsigned)prop_count() + 1u);
    sh_reset(); model_shell_list("sys.", collect);
    CHECK(n_out == 8);
    CHECK(strstr(out[1], "NOWIRE") != NULL);   /* sys.mode */
    sh_reset(); model_shell_describe("ch1.vdiv", collect);
    bool vals = false, owner = false;
    for (unsigned i = 0; i < n_out && i < 64; i++) {
        if (strstr(out[i], "values:")) vals = true;
        if (strstr(out[i], "owner:   fpga_apply_vdiv")) owner = true;
    }
    CHECK(vals && owner && starts(0, "ch1.vdiv: enum 0..9"));
    sh_reset(); model_shell_describe("sys.mode", collect);
    CHECK(strstr(out[n_out - 1], "get only for now: mode switch") != NULL);
    for (unsigned i = 0; i < n_out && i < 64; i++) CHECK(strlen(out[i]) < 128);
}

/* The shell set queues ORIGIN_SHELL: that origin is what `persist off`
 * (shell_persist) silences, so a bench sweep never wears the W25Q. */
static void test_shell_origin(void)
{
    fresh(1);
    g_shell_persist = false;
    sh_reset(); model_shell_set("ch1.coupling 1", collect);
    CHECKF(n_out == 1 && strcmp(out[0], "OK ch1.coupling=AC") == 0, "%s", out[0]);
    CHECK(last_origin == ORIGIN_SHELL && n_sent == 1);
    CHECK(prop_get(P(CH1_COUPLING)) == 1 && g_notes == 0);        /* changed, not noted */
    sh_reset(); model_shell_set("ch1.coupling=DC ch1.probe=10x", collect);
    CHECK(last_origin == ORIGIN_SHELL && g_notes == 0 && prop_get(P(CH1_COUPLING)) == 0);
    /* control: persist on, the same set is noted once */
    g_shell_persist = true;
    sh_reset(); model_shell_set("ch1.coupling 1", collect);
    CHECK(last_origin == ORIGIN_SHELL && g_notes == 1);
    /* and a key-origin request is noted even under persist off */
    g_shell_persist = false;
    model_result_t r;
    prop_pair_t p = pr(PROP_ID_CH1_COUPLING, 0);
    CHECK(prop_request(&p, 1, ORIGIN_KEY, 600, &r) == PS_OK && g_notes == 2);
}

int main(void)
{
    test_not_up();
    test_basic();
    test_order();
    test_all_or_nothing();
    test_stop_at_failure();
    test_unwired();
    test_timeout_withdrawn();
    test_queue_full();
    test_seq_wrap();
    test_context();
    test_threaded_in_progress();
    test_shell_set();
    test_shell_get_roundtrip();
    test_shell_origin();
    printf("test_model_exec: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
