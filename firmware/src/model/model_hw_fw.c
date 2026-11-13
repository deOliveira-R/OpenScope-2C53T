/*
 * OpenScope 2C53T - the real hardware ops and the FreeRTOS port of the
 * executor (ARCH-1 M1)
 *
 * Each op CALLS the owner API the key path calls today (input_handler.c
 * exec_key_cmd, scope_apply.c, osc_menu_ui.c, meter_ui.c, signal_gen.c,
 * sys_pwm.c, fpga.c, scope_mask.c). Nothing here is a second copy of an
 * owner's logic, and nothing writes a register (src/model/ owns no
 * peripheral: scripts/reg_owners.txt).
 *
 * Channel numbers: the ops take 0 = CH1, 1 = CH2 (model.h). fpga_apply_vdiv
 * and scope_vpos_set take 1 / 2; fpga_apply_coupling takes 0 / 1.
 *
 * NOT WIRED YET. Where the real setter is not a callable function today (its
 * logic is inline in a key handler, or its state is static in the key
 * module), the property is refused through the executor with the reason
 * below, instead of moving key-handler code in this step. The UI-lane tasks
 * that own those files make them callable and drop the row.
 *
 * Every apply op starts with model_exec_check_context(): a hardware apply
 * made off the input task counts in model_foreign_sets. Getter ops (state an
 * owner keeps in its own file) run on any task and do not count.
 */
#include "model_exec.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "ui.h"
#include "fpga.h"
#include "scope_vpos.h"
#include "scope_apply.h"
#include "scope_state.h"
#include "persistence.h"
#include "scope_mask.h"
#include "signal_gen.h"
#include "sys_pwm.h"
#include "settings_store.h"
#include "shell_persist.h"
#include "scope_selfcal_run.h"
#include "watchdog.h"

bool input_inject_button(button_id_t b);   /* main.c (the `btn` shell command's mailbox) */

#define CTX() model_exec_check_context()

/* ═══ scope front end and acquisition ════════════════════════════════ */

static bool op_vdiv(uint8_t ch, uint8_t idx)          { CTX(); return fpga_apply_vdiv((uint8_t)(ch + 1u), idx); }
static bool op_coupling(uint8_t ch, uint8_t c)        { CTX(); return fpga_apply_coupling(ch, c); }
static bool op_channel_enables(void)                  { CTX(); return fpga_apply_channel_enables(); }
static bool op_interleave_active(void)                { return fpga_interleave_active(); }
static bool op_vpos(uint8_t ch, int16_t pos)          { CTX(); return scope_vpos_set((uint8_t)(ch + 1u), pos); }
static bool op_timebase(uint8_t code)                 { CTX(); return fpga_apply_timebase(code); }
static bool op_roll(void)                             { CTX(); return fpga_apply_roll(); }
static bool op_trigger_source(void)                   { CTX(); return fpga_apply_trigger_source(); }
static bool op_trigger_level(int16_t level)           { CTX(); return fpga_apply_trigger_level(level); }
static void op_single_rearm(void)                     { CTX(); fpga_acq_single_rearm(); }

/* ═══ views and the shared pool (scope_apply.c) ══════════════════════ */

static bool op_view_select(uint8_t view)
{
    CTX();
#ifdef FEATURE_FFT
    if (view >= (uint8_t)SCOPE_VIEW_COUNT) return false;
    scope_view_select((scope_view_t)view);
    return scope_view == (scope_view_t)view;
#else
    return view == 0u;
#endif
}
static bool    op_persist_on(uint8_t mode) { CTX(); return scope_apply_persist_on((persist_mode_t)mode); }
static void    op_persist_off(void)        { CTX(); scope_apply_persist_off(); }
static uint8_t op_persist_mode(void)       { return (uint8_t)persist_get_mode(); }

/* ═══ mask test (requests the acquisition task applies) ══════════════ */

/* The key path's wait (input_handler.c mask_req: 20 x 5 ms), so the answer
 * reports the applied value when the acquisition task is polling. A request
 * still unacknowledged is remembered; fw_service() bumps the mask epoch when
 * it lands (a posted change, model.h model_touch). */
static uint32_t g_mask_pending;

static bool mask_post_wait(scope_mask_req_t k, uint8_t a, uint8_t b)
{
    const uint32_t seq = scope_mask_post(k, a, b, 0);
    for (int i = 0; i < 20 && !scope_mask_acked(seq); i++)
        vTaskDelay(pdMS_TO_TICKS(5));
    if (!scope_mask_acked(seq)) g_mask_pending = seq;
    return true;                                     /* posted: the newest request wins */
}

static bool op_mask_present(void) { return scope_mask_state() != MASK_PF_EMPTY; }
static void op_mask_get(uint8_t *tol_v, uint8_t *tol_h, bool *stop_on_fail)
{
    scope_mask_summary_t m;
    scope_mask_summary(&m);
    *tol_v = m.tol_v;
    *tol_h = m.tol_h;
    *stop_on_fail = m.stop_on_fail;
}
static bool op_mask_tol(uint8_t v, uint8_t h) { CTX(); return mask_post_wait(SCOPE_MASK_REQ_TOL, v, h); }
static bool op_mask_stopfail(bool on)         { CTX(); return mask_post_wait(SCOPE_MASK_REQ_STOPFAIL, on ? 1u : 0u, 0u); }

/* ═══ generator (signal_gen.c owns TMR7/DMA2/DAC2) ═══════════════════ */

static void op_gen_wave(uint8_t w)       { CTX(); siggen_set_waveform(w); }
static void op_gen_freq(uint32_t hz)     { CTX(); siggen_set_frequency(hz); }
static void op_gen_amp(uint8_t dv)       { CTX(); siggen_set_amplitude_dv(dv); }
static void op_gen_duty(percent_t d)     { CTX(); siggen_set_duty(d.pct); }
static void op_gen_out(bool on)          { CTX(); siggen_enable(on); }
static void op_gen_bootout(bool on)      { CTX(); siggen_set_start_on(on); }

/* ═══ meter ══════════════════════════════════════════════════════════ */

/* meter_function_select (meter_ui.c) is the key's function change: cancel
 * the autoselect, store, reset min/max/hold/rel, post the meter posture. It
 * cancels the autoselect unconditionally, so the autoselect's own pick
 * (keep_autoselect) is not served yet (B6 splits it). Outside the meter the
 * old `meter func` refuses too: a posture request then would switch the
 * meter SoC on under the scope. */
static bool op_meter_func(uint8_t submode, bool keep_autoselect)
{
    CTX();
    if (keep_autoselect || current_mode != MODE_MULTIMETER) return false;
    meter_function_select(submode);
    return true;
}

/* ═══ system ═════════════════════════════════════════════════════════ */

/* Not reachable through the executor (fw_unwired), bound only so that a
 * NULL op never stands for "done": props_sys.c writes current_mode itself
 * when the op is missing. */
static bool op_mode(uint8_t m)           { CTX(); (void)m; return false; }
static void op_backlight(uint8_t pct)    { CTX(); (void)pct; sys_pwm_apply_brightness(); }  /* reads sys_prefs */
static void op_autooff(uint8_t mins)     { CTX(); (void)mins; }                             /* not wired */

/* ═══ interleave (fpga.c keeps floats; typed ints at this boundary) ══ */

static int32_t round_i32(float x) { return (int32_t)(x >= 0.0f ? x + 0.5f : x - 0.5f); }
static bool op_il_enable(bool on)        { CTX(); fpga_interleave_set(on); return true; }
static bool op_il_enabled(void)          { return fpga_interleave_get(); }
static bool op_il_corr_set(gain_e6_t g, int32_t offset_x100)
{
    CTX();
    fpga_interleave_corr_set((float)g.e6 / 1000000.0f, (float)offset_x100 / 100.0f);
    return true;
}
static void op_il_corr_get(gain_e6_t *g, int32_t *offset_x100)
{
    float gain = 1.0f, off = 0.0f;
    fpga_interleave_corr_get(&gain, &off);
    g->e6 = round_i32(gain * 1000000.0f);
    *offset_x100 = round_i32(off * 100.0f);
}
static bool op_il_skew_set(ppm_period_t s) { CTX(); return fpga_interleave_skew_set(s.ppm); }
static ppm_period_t op_il_skew_get(void)   { ppm_period_t s = { fpga_interleave_skew_get() }; return s; }

/* ═══ executor services ══════════════════════════════════════════════ */

static bool op_selfcal_busy(void)  { return scope_selfcal_busy(); }
static bool op_shell_persist(void) { return shell_persist_enabled(); }
static void op_note_change(void)
{
    settings_store_note_change((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
}

static const model_hw_ops_t k_fw_ops = {
    .vdiv = op_vdiv, .coupling = op_coupling, .channel_enables = op_channel_enables,
    .interleave_active = op_interleave_active, .vpos = op_vpos, .timebase = op_timebase,
    .roll = op_roll, .trigger_source = op_trigger_source, .trigger_level = op_trigger_level,
    .single_rearm = op_single_rearm,
    .view_select = op_view_select, .persist_on = op_persist_on, .persist_off = op_persist_off,
    .persist_mode = op_persist_mode,
    .mask_present = op_mask_present, .mask_get = op_mask_get, .mask_tol = op_mask_tol,
    .mask_stopfail = op_mask_stopfail,
    .gen_wave = op_gen_wave, .gen_freq = op_gen_freq, .gen_amp = op_gen_amp,
    .gen_duty = op_gen_duty, .gen_out = op_gen_out, .gen_bootout = op_gen_bootout,
    .meter_func = op_meter_func,
    .mode = op_mode, .backlight = op_backlight, .autooff = op_autooff,
    .il_enable = op_il_enable, .il_enabled = op_il_enabled, .il_corr_set = op_il_corr_set,
    .il_corr_get = op_il_corr_get, .il_skew_set = op_il_skew_set, .il_skew_get = op_il_skew_get,
    .selfcal_busy = op_selfcal_busy, .shell_persist = op_shell_persist,
    .note_change = op_note_change,
};

/* ═══ not wired yet ══════════════════════════════════════════════════ */

/* The reason fits a prop_note_t (23 characters). */
static const struct { uint16_t id; const char *why; } k_unwired[] = {
    { PROP_ID_SYS_INSTRUMENT, "mode switch: keys (B1)" },    /* enter_mode is static in input_handler.c */
    { PROP_ID_SYS_MODE,       "mode switch: keys (B1)" },
    { PROP_ID_SYS_AUTOOFF,    "idle timer: keys (B4)" },     /* the timer re-arm is static there too    */
    { PROP_ID_VIEW_RUN,       "RUN/STOP: OK key (B7)" },     /* STOP latches the paused ranges inline   */
    { PROP_ID_METER_LAYOUT,   "View softkey (B6)" },         /* Fuse forces DC V, Limits inits: inline  */
    { PROP_ID_METER_FUSETYPE, "fuse type keys (B6)" },       /* keeps the rating in amps: static helper */
    /* Stored only: their record fields are carried by settings_store.c's own
     * image until M3 moves them onto the model, so a set here would neither
     * be saved nor read back from the record. */
    { PROP_ID_SYS_LANG,       "stored only until M3" },
    { PROP_ID_FFT_WINDOW,     "stored only until M3" },
    { PROP_ID_FFT_AVG,        "stored only until M3" },
    { PROP_ID_FFT_MAXHOLD,    "stored only until M3" },
};

static const char *fw_unwired(uint16_t id)
{
    for (unsigned i = 0; i < sizeof k_unwired / sizeof k_unwired[0]; i++)
        if (k_unwired[i].id == id) return k_unwired[i].why;
    return NULL;
}

/* ═══ the FreeRTOS port ══════════════════════════════════════════════ */

static QueueHandle_t g_model_q;
static QueueHandle_t g_display_q;
static TaskHandle_t  g_executor;

static bool port_send(const model_req_t *r)  { return xQueueSend(g_model_q, r, 0) == pdTRUE; }
static bool port_receive(model_req_t *r)     { return xQueueReceive(g_model_q, r, 0) == pdTRUE; }
/* The wake shares the 15-deep key queue with the TMR3 scan. Only the first
 * request waiting posts one (the drain takes everything queued behind it),
 * so a stuck input task and a retrying script cannot fill the key queue and
 * cost the user key presses: at most one wake is outstanding. */
static void port_wake(void)
{
    if (uxQueueMessagesWaiting(g_model_q) == 1u) (void)input_inject_button(BTN_NONE);
}
static void port_sleep_ms(uint32_t ms)       { vTaskDelay(pdMS_TO_TICKS(ms)); }
static void port_checkin(void)               { health_checkin_self(); }
static bool port_on_executor(void)           { return xTaskGetCurrentTaskHandle() == g_executor; }

/* One full repaint per shell request that changed something, as a key's
 * popup_and_redraw does; never waits (a full display queue drops it, and
 * the periodic redraw gates catch up). */
static void port_changed(void)
{
    const uint8_t cmd = DCMD_REDRAW_ALL;
    if (g_display_q) (void)xQueueSend(g_display_q, &cmd, 0);
}

static void port_service(void)
{
    if (g_mask_pending && scope_mask_acked(g_mask_pending)) {
        g_mask_pending = 0u;
        model_touch(PG_MASK);
    }
}

static const model_exec_port_t k_fw_port = {
    .send = port_send, .receive = port_receive, .wake = port_wake, .sleep_ms = port_sleep_ms,
    .checkin = port_checkin, .changed = port_changed, .service = port_service,
    .on_executor = port_on_executor, .unwired = fw_unwired,
};

void model_fw_start(void *display_queue)
{
    if (g_model_q) return;
    g_model_q = xQueueCreate(MODEL_EXEC_QUEUE_LEN, sizeof(model_req_t));
    if (g_model_q == NULL) return;                 /* no heap: prop_request stays BUSY */
    g_display_q = (QueueHandle_t)display_queue;
    g_executor = xTaskGetCurrentTaskHandle();
    model_bind_hw(&k_fw_ops);
    model_exec_init(&k_fw_port);
}
