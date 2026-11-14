/*
 * Shared environment of the migrated screens' host tests (test_home_screen.c,
 * test_siggen_screen.c): the REAL property model (src/model/) and the real
 * pure modules it reads, with fake hardware ops that log every owner call,
 * so a test sees a key become a property set and the set become the owner
 * call. The same pattern as tests/test_model.c, cut down to the ops a screen
 * can reach (mode, the generator, the executor services); every other op is
 * unbound (NULL: the setter skips the call).
 *
 * Include once, from the test's .c file.
 */
#ifndef SCREEN_TEST_ENV_H
#define SCREEN_TEST_ENV_H

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "model.h"
#include "screen.h"
#include "scope_state.h"
#include "meas_select.h"
#include "signal_gen.h"
#include "sys_prefs.h"
#include "theme.h"
#include "ui.h"

static int run, passed;
#define CHECK(name, cond) do { run++; if (cond) { passed++; printf("  %-70s[PASS]\n", name); } \
                               else { printf("  %-70s[FAIL]\n", name); } } while (0)

/* ── the globals main.c / input_handler.c own in the firmware ──────── */
volatile device_mode_t  current_mode  = MODE_OSCILLOSCOPE;
volatile startup_mode_t startup_mode  = STARTUP_SCOPE;
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

/* ── signal_gen.c's output driver: every call that would reach TMR7 /
 *    DMA2 / DAC2 is logged, in order ─────────────────────────────── */
static char     env_log[1024];
static void env_logf(const char *fmt, ...)
{
    char t[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    strncat(env_log, t, sizeof env_log - strlen(env_log) - 1);
}
static void env_log_clear(void) { env_log[0] = '\0'; }
static bool env_log_is(const char *want) { return strcmp(env_log, want) == 0; }

static bool env_log_dac;               /* log the driver calls too (the register side) */
static uint16_t env_dac_buf[SIGGEN_CYCLE_SAMPLES];
void dac_output_apply(const siggen_timing_t *t) { (void)t; if (env_log_dac) env_logf("dac_apply;"); }
void dac_output_stop(void) { if (env_log_dac) env_logf("dac_stop;"); }
uint16_t *dac_output_get_buffer(void) { if (env_log_dac) env_logf("dac_buf;"); return env_dac_buf; }
uint32_t dac_output_timer_clock_hz(void) { return 240000000u; }

/* ── fake hardware ops ─────────────────────────────────────────────── */
static bool     env_busy;              /* self-cal running */
static unsigned env_notes;             /* settings note_change calls */
/* The mode op: the firmware's posts the switch and enter_mode makes it
 * (current_mode, then the router's base). env_mode_applies = false keeps it
 * a pure log, to see that nothing else moved the mode. */
static bool     env_mode_applies = true;

static bool f_mode(uint8_t m)
{
    env_logf("mode(%u);", m);
    if (env_mode_applies) {
        current_mode = (device_mode_t)m;
        screen_switch(screen_base_for_mode((device_mode_t)m));
    }
    return true;
}
static void f_gen_wave(uint8_t w)    { env_logf("gen_wave(%u);", w); siggen_set_waveform(w); }
static void f_gen_freq(uint32_t f)   { env_logf("gen_freq(%u);", (unsigned)f); siggen_set_frequency(f); }
static void f_gen_amp(uint8_t a)     { env_logf("gen_amp(%u);", a); siggen_set_amplitude_dv(a); }
static void f_gen_duty(percent_t d)  { env_logf("gen_duty(%u);", d.pct); siggen_set_duty(d.pct); }
static void f_gen_out(bool on)       { env_logf("gen_out(%d);", on); siggen_enable(on); }
static void f_gen_boot(bool on)      { env_logf("gen_boot(%d);", on); siggen_set_start_on(on); }
static bool f_busy(void)             { return env_busy; }
static bool f_shell_persist(void)    { return true; }
static void f_note(void)             { env_notes++; }

static const model_hw_ops_t env_ops = {
    .gen_wave = f_gen_wave, .gen_freq = f_gen_freq, .gen_amp = f_gen_amp, .gen_duty = f_gen_duty,
    .gen_out = f_gen_out, .gen_bootout = f_gen_boot, .mode = f_mode,
    .selfcal_busy = f_busy, .shell_persist = f_shell_persist, .note_change = f_note,
};

/* A device as a first boot leaves it, the fake ops bound, the log empty. */
static void env_fresh(device_mode_t mode)
{
    scope_state_init(scope_state_get());
    meas_select_defaults();
    siggen_init(NULL);
    sys_prefs_set_brightness(SYS_BRIGHT_DEFAULT);
    sys_prefs_set_volume(SYS_VOL_DEFAULT);
    sys_prefs_set_autooff_mins(0);
    theme_init(THEME_DARK_BLUE);
    current_mode = mode; startup_mode = STARTUP_SCOPE; active_channel = 0;
    meter_submode = 0; meter_layout = 0; math_enabled = false; math_op = 0;
    persist_enabled = false; scope_view = SCOPE_VIEW_TIME;
    env_busy = false; env_mode_applies = true; env_log_dac = false;
    model_bind_hw(&env_ops);
    (void)model_load_defaults(PF_PERSIST, NULL, false);
    current_mode = mode;
    env_notes = 0;
    env_log_clear();
}

/* Every group's change epoch, to see what a key touched. */
typedef struct { uint32_t e[PG_COUNT]; } env_epochs_t;
static env_epochs_t env_epochs(void)
{
    env_epochs_t x;
    for (unsigned g = 0; g < PG_COUNT; g++) x.e[g] = prop_group_epoch((uint8_t)g);
    return x;
}
/* Bit g set: group g's epoch moved between a and b. */
static uint16_t env_epochs_moved(const env_epochs_t *a, const env_epochs_t *b)
{
    uint16_t m = 0;
    for (unsigned g = 0; g < PG_COUNT; g++) if (a->e[g] != b->e[g]) m |= (uint16_t)(1u << g);
    return m;
}

/* Stand-in bases for the screens a test does not have under test. */
#define ENV_BASE(nm, sid, md, ins) \
    { .name = nm, .id = sid, .mode = md, .instrument = ins }

static scr_res_t env_key(uint8_t raw) { return (scr_res_t)screen_route_key((button_id_t)raw); }

#endif /* SCREEN_TEST_ENV_H */
