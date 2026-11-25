/*
 * OpenScope 2C53T - the real hardware ops and the FreeRTOS port of the
 * executor (ARCH-1 M1)
 *
 * Each op CALLS the owner API (fpga.c, scope_vpos.c, scope_apply.c,
 * scope_view.c's latch, meter_ui.c, signal_gen.c, sys_pwm.c, scope_mask.c);
 * since B7 the ops are the only callers of the scope's. Nothing here is a second copy of an
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
#include "input_handler.h"   /* input_autooff_apply */
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
#include "meter_autoselect.h"
#include "meter_data.h"
#include "gallery_ui.h"
#include "screen.h"
#include "screen.h"
#include "osc_menu_screen.h"
#include "scope_screen.h"

bool input_inject_button(button_id_t b);   /* main.c (the `btn` shell command's mailbox) */

#define CTX() model_exec_check_context()

/* ═══ live or deferred (model.h DEFERRED) ════════════════════════════
 *
 * Every instrument's properties can be set whatever the screen is bound
 * to. The state always updates; the hardware apply is live when that
 * instrument's hardware path is live in the current front-end posture, else
 * deferred to the posture's entry code. Read from the code, per op, for the
 * bench image (app-coldtrace-meter); the other builds differ at (*):
 *
 *   op (property)                  scope screen  meter screen  gen/home/settings
 *   vdiv relays (chN.vdiv)         live          DEFERRED *    live *
 *   vdiv offset half, vpos (DAC1 / TMR13: chN.vdiv, chN.pos)  live in every posture **
 *   coupling (PD12/PD13)           live          live          live
 *   channel_enables (PC1/PC2 mask, offsets)                     live in every posture
 *     (its reg 0x06 write when the source moves: as *** below; re-sent too)
 *   timebase/roll/trigger (SPI3 regs 0x01/0x06/0x08, parked acq task)
 *                                  live          DEFERRED ***  live * ***
 *   view/persist/mask/il (MCU state, the FFT/persistence pool, acq-task requests): live in every posture
 *   generator (DAC2/PA5, TMR7, DMA2)                           live in every posture
 *   meter_func (meter SoC function, USART2)  DEFERRED  live    DEFERRED
 *   state-only properties (probe, bw, meas, edge, x, soft, math, cursor, ...): no hardware
 *
 * Why the relays defer: the meter's posture (fpga.c
 * fpga_set_meter_frontend_for_submode -> fpga_apply_meter_mux_gpio_state)
 * drives PC12/PE4..6 (CH1's range relays) and PA15/PB11/PB10/PA10 (CH2's)
 * and PA6, so a live range write in the meter would move the meter's mux.
 * Where the stored range is applied:
 *   - FPGA_WARM_HANDOFF_TEST + FPGA_METER_SUBMODES (app-coldtrace-meter, the
 *     bench image): with `meter relays on` the meter link follows the
 *     screen (main.c meter_link_service), so the display task calls
 *     fpga_set_meter_mux(false) on leaving the meter, and that re-applies
 *     the mask, both ranges and PA6 from scope_state
 *     (fpga_set_scope_frontend_ranges). So the scope front end is live
 *     everywhere but the meter screen. (By default the meter leaves the
 *     relays alone and the front end is live on every screen.)
 *   - FPGA_WARM_HANDOFF_TEST without SUBMODES (app-coldtrace): the meter
 *     never touches the relays (fpga_set_meter_mode returns at once) and
 *     nothing re-applies them, so the ranges are written live everywhere.
 *   - no warm handoff (the default image): the meter's transition writes the
 *     relays and only fpga_enter_scope_mode -> fpga_scope_reinit puts the
 *     scope's back (on scope entry; leaving the meter for Home keeps the
 *     meter's), so the relays are live on the scope screen only (*).
 * (**) DAC1/TMR13 are not in the meter's pin set, so the offset half of a
 * range change is applied live even when the relays defer (the relay
 * re-apply does not redo it). The generator does not hold it back: it is
 * on DAC channel 2, and the DAC owner keeps the channels apart (A1 removed
 * scope_vpos's old generator guard), so CH1's offset is applied whether the
 * generator runs or not.
 * (***) as the relays: written at once while the scope front end is live
 * (scope_fe_live), else stored and re-sent from scope_state when the
 * scope's posture is back (fpga_scope_posture_gen moves: fpga_set_meter_mux
 * (false) in the bench image, i.e. the meter link going off, which with
 * `meter relays on` is leaving the meter for any screen;
 * fpga_scope_reinit on scope entry in the default image). The re-send runs
 * on the executor (port_service -> model_scope_reapply): reg 0x01 (the roll
 * code with roll on, else tb.code; each also re-runs scope_vpos_apply, DAC1
 * and TMR13), reg 0x06, reg 0x08, each in its own acquisition park. Only
 * after such a stored set: with none, scope entry adds no SPI3 write. In
 * the bench image the front end counts as not live from meter entry until
 * the display task has run mux(false) (fpga_meter_holds_fe), so a set made
 * in that window is stored too. The default image's scope entry also
 * resends the USART timing and trigger block from scope_state
 * (fpga_send_scope_sequence), as before.
 * meter_func: the request is served while the meter link is on (fpga.c
 * fpga_meter_link_set: the meter screen, a USB host, or meter.link =
 * always), whatever screen is shown. With the link off the poll task drops
 * it (and without a poll task it would switch the meter SoC on under the
 * scope), so the op does the state half only, and the stored function is
 * applied when the link next comes on: fpga_meter_link_set(true) requests
 * it, as does meter entry (input_mode_switch -> fpga_meter_screen_entered,
 * input_handler.c; main.c at boot). */

static bool scope_fe_live(void)
{
    /* mux(false) re-applies on any other screen. Until the display task has
     * run it after leaving the meter, the meter's posture still holds the
     * front end (PB11 may be low): not live yet. */
    /* The meter takes the relays only with `meter relays on` (fpga.h):
     * by default the scope front end is live on every screen. */
    if (!fpga_meter_relays_get()) return true;
    return current_mode != MODE_MULTIMETER && !fpga_meter_holds_fe();
}

/* SPI3 registers stored while the scope front end was not live (timebase,
 * roll, trigger source and level): re-sent from scope_state by
 * model_scope_reapply() on the executor (port_service) once the scope's
 * posture has been put back after the deferral (fpga_scope_posture_gen
 * moved) and the front end is live. 2 bytes. */
static bool    g_scope_regs_stored;
static uint8_t g_scope_gen_seen;

/* Mark the registers for the re-send on the scope's return. */
static void scope_regs_mark(void)
{
    if (!g_scope_regs_stored) {
        g_scope_regs_stored = true;
        g_scope_gen_seen = fpga_scope_posture_gen;
    }
}

static void scope_regs_defer(void)
{
    scope_regs_mark();
    model_hw_deferred("stored: scope FE off");
}

static void scope_regs_service(void)
{
    if (!g_scope_regs_stored || fpga_scope_posture_gen == g_scope_gen_seen || !scope_fe_live())
        return;
    g_scope_regs_stored = false;
    /* A park that fails leaves the registers as they were: try again on the
     * next posture return, never on every pass (one park can take 1 s).
     * The check-in between the parks keeps one park the longest gap. */
    if (!model_scope_reapply(health_checkin_self)) {
        g_scope_regs_stored = true;
        g_scope_gen_seen = fpga_scope_posture_gen;
    }
}

/* ═══ scope front end and acquisition ════════════════════════════════ */

static bool op_vdiv(uint8_t ch, uint8_t idx)
{
    CTX();
    if (scope_fe_live()) return fpga_apply_vdiv((uint8_t)(ch + 1u), idx);
    /* The relays wait for the scope posture; the offset reference (DAC1 /
     * TMR13, outside the meter's pins) follows the new range now, because
     * the relay re-apply does not redo it. */
    if (idx >= VDIV_COUNT) return false;
    scope_vpos_apply_ch((uint8_t)(ch + 1u), idx);
    model_hw_deferred("stored: scope FE off");
    return true;
}
static bool op_coupling(uint8_t ch, uint8_t c)        { CTX(); return fpga_apply_coupling(ch, c); }
/* The converter routing (PC1/PC2) is live in every posture. When a channel
 * going off moves the trigger source, the setter has reg 0x06 written just
 * before (op_trigger_source_follow), at once in every posture, as
 * fpga_apply_channel_enables did itself until B8; with the scope front end
 * not live that write is as unproven as a stored trigger set, so the
 * registers are re-sent on the scope's return too (the answer stays live:
 * the routing is). */
static bool op_trigger_source_follow(void)            { CTX(); return fpga_apply_trigger_source(); }
static bool op_channel_enables(void)
{
    CTX();
    const bool live = scope_fe_live();
    if (!fpga_apply_channel_enables()) return false;
    if (!live) scope_regs_mark();
    return true;
}
static bool op_interleave_active(void)                { return fpga_interleave_active(); }
static bool op_vpos(uint8_t ch, int16_t pos)          { CTX(); return scope_vpos_set((uint8_t)(ch + 1u), pos); }
/* SPI3 regs 0x01 / 0x06 / 0x08: live when the scope front end is; else
 * stored (the setter keeps the state) and re-sent on scope entry. */
static bool op_timebase(uint8_t code)
{
    CTX();
    if (scope_fe_live()) return fpga_apply_timebase(code);
    if (code >= TIMEBASE_COUNT) return false;
    scope_regs_defer();
    return true;
}
static bool op_roll(void)
{
    CTX();
    if (scope_fe_live()) return fpga_apply_roll();
    scope_regs_defer();
    return true;
}
static bool op_trigger_source(void)
{
    CTX();
    if (scope_fe_live()) return fpga_apply_trigger_source();
    scope_regs_defer();
    return true;
}
static bool op_trigger_level(int16_t level)
{
    CTX();
    if (scope_fe_live()) return fpga_apply_trigger_level(level);
    scope_regs_defer();
    return true;
}
static void op_single_rearm(void)                     { CTX(); fpga_acq_single_rearm(); }
static bool op_single_done(void)                      { return fpga_acq_single_done(); }
static void op_paused_latch(void)                     { CTX(); scope_ui_paused_latch(); }
static void op_cursor_axes(struct scope_cursor_axes *ax) { CTX(); scope_ui_cursor_axes(ax); }
static uint8_t op_trigger_code(void)                  { return fpga_acq_trig_code_get(); }

/* ═══ views and the shared pool (scope_apply.c) ══════════════════════ */

static bool op_view_select(uint8_t view)
{
    CTX();
    if (view >= (uint8_t)SCOPE_VIEW_COUNT) return false;
    const scope_view_t was = scope_view;
    scope_view_select((scope_view_t)view);
    if (scope_view != was) scope_screen_view_changed();
    return scope_view == (scope_view_t)view;
}
static void    op_view_half_enter(uint8_t view) { CTX(); scope_view_half_enter(view); }
static bool    op_persist_on(uint8_t mode) { CTX(); return scope_apply_persist_on((persist_mode_t)mode); }
static void    op_persist_off(void)        { CTX(); scope_apply_persist_off(); }
static uint8_t op_persist_mode(void)       { return (uint8_t)persist_get_mode(); }
static void    op_fft_settings(void)
{
    CTX();
    scope_apply_fft_settings();
}

/* ═══ mask test (requests the acquisition task applies) ══════════════ */

/* The key path's wait (input_handler.c mask_req: 20 x 5 ms), so the answer
 * reports the applied value when the acquisition task is polling. A request
 * still unacknowledged is remembered; fw_service() bumps the mask epoch when
 * it lands (a posted change, model.h model_touch). */
static uint32_t g_mask_pending;
static uint32_t g_mask_tol_seq;        /* the last tolerance request (0: none) */

static bool mask_post_wait(scope_mask_req_t k, uint8_t a, uint8_t b)
{
    const uint32_t seq = scope_mask_post(k, a, b, 0);
    for (int i = 0; i < 20 && !scope_mask_acked(seq); i++)
        vTaskDelay(pdMS_TO_TICKS(5));
    if (!scope_mask_acked(seq)) g_mask_pending = seq;
    return true;                                     /* posted: the newest request wins */
}

/* The mask's procedures asked for by another task (the shell's `mask teach /
 * clear / reset / run`, through input_call_sync): posted here, on the input
 * task, with the keys' wait; one still unacknowledged is remembered like a
 * property's, so the mask's epoch moves when it lands. Returns the request's
 * number (scope_mask_acked). */
uint32_t model_fw_mask_request(uint8_t kind, uint8_t a, uint8_t b, uint16_t n)
{
    CTX();
    const uint32_t seq = scope_mask_post((scope_mask_req_t)kind, a, b, n);
    for (int i = 0; i < 20 && !scope_mask_acked(seq); i++)
        vTaskDelay(pdMS_TO_TICKS(5));
    if (!scope_mask_acked(seq)) g_mask_pending = seq;
    else                        model_touch(PG_MASK);
    return seq;
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
/* One post carries both tolerances. Its sequence is kept: until it is
 * acknowledged the model builds the next set on this pair (props_scope.c). */
static bool op_mask_tol(uint8_t v, uint8_t h)
{
    CTX();
    const uint32_t seq = scope_mask_post(SCOPE_MASK_REQ_TOL, v, h, 0);
    g_mask_tol_seq = seq;
    for (int i = 0; i < 20 && !scope_mask_acked(seq); i++)
        vTaskDelay(pdMS_TO_TICKS(5));
    if (!scope_mask_acked(seq)) g_mask_pending = seq;
    return true;                                     /* posted: the newest request wins */
}
static uint8_t op_mask_state(void)       { return (uint8_t)scope_mask_state(); }
static bool    op_mask_hold(void)        { return scope_mask_hold_active(); }
static bool    op_mask_pending(void)     { return g_mask_pending != 0u && !scope_mask_acked(g_mask_pending); }
static bool    op_mask_tol_pending(void) { return g_mask_tol_seq != 0u && !scope_mask_acked(g_mask_tol_seq); }
static bool op_mask_stopfail(bool on)         { CTX(); return mask_post_wait(SCOPE_MASK_REQ_STOPFAIL, on ? 1u : 0u, 0u); }

/* ═══ generator (signal_gen.c owns TMR7/DMA2/DAC2) ═══════════════════ */

static void op_gen_wave(uint8_t w)       { CTX(); siggen_set_waveform(w); }
static void op_gen_freq(uint32_t hz)     { CTX(); siggen_set_frequency(hz); }
static void op_gen_amp(uint8_t dv)       { CTX(); siggen_set_amplitude_dv(dv); }
static void op_gen_duty(percent_t d)     { CTX(); siggen_set_duty(d.pct); }
static void op_gen_out(bool on)          { CTX(); siggen_enable(on); }
static void op_gen_bootout(bool on)      { CTX(); siggen_set_start_on(on); }

/* ═══ meter ══════════════════════════════════════════════════════════ */

/* The rest of a function change once the setter has stored it (what
 * meter_ui.c's meter_function_set did): cancel the software autoselect
 * unless the change is its own candidate (keep_autoselect: meter.func
 * posted by meter_autoselect.c as ORIGIN_AUTO), reset min/max, release hold
 * and relative (a candidate keeps them, as it always did, and resets the
 * voltage waveform instead), post the meter posture. With the meter link
 * off the request must not go out (it would switch the meter SoC on under
 * the scope in a build without the poll task): the state half only, and the
 * function is applied when the link comes on (see the table above). The
 * meter screen counts as "on": its link follows within a frame. */
static bool op_meter_func(uint8_t submode, bool keep_autoselect)
{
    CTX();
    if (keep_autoselect) {
        meter_reset_minmaxavg();
    } else {
        if (meter_autoselect_is_running()) meter_autoselect_cancel();
        meter_reset_minmaxavg();
        meter_hold_release();
        if (meter_rel_enabled) meter_toggle_relative();    /* a reference in another function means nothing */
    }
    if (current_mode == MODE_MULTIMETER || fpga_meter_link_on()) {
        fpga_request_meter_mode(submode);                  /* in the background: the UI shows dashes meanwhile */
        return true;
    }
    model_hw_deferred("stored: meter FE off");            /* the reply text is kept */
    return true;
}

/* meter.hold / meter.rel / meter.degf: flags their owners keep. */
static bool op_meter_flag_get(uint8_t which)
{
    switch (which) {
    case METER_FLAG_HOLD: return meter_hold_enabled;
    case METER_FLAG_REL:  return meter_rel_enabled;
    case METER_FLAG_DEGF: return meter_data_fahrenheit();
    default:              return false;
    }
}
static bool op_meter_flag_set(uint8_t which, bool on, bool user)
{
    CTX();
    switch (which) {
    case METER_FLAG_HOLD:
        if (!on) { meter_hold_release(); return true; }
        return user && meter_ui_hold_set(true);            /* a load cannot hold a reading */
    case METER_FLAG_REL:
        meter_ui_rel_set(on);
        return true;
    case METER_FLAG_DEGF:
        if (user) meter_ui_degf_set(on);
        else      meter_data_set_fahrenheit(on);
        return true;
    default:
        return false;
    }
}
static void op_meter_layout(uint8_t layout) { CTX(); meter_ui_layout_changed(layout); }

/* ═══ system ═════════════════════════════════════════════════════════ */

/* sys.mode / sys.instrument: the mode switch (input_mode_switch: the router
 * switches the screen, then the mode's posture, the settings flush, the
 * repaint). The long mode keys reach it through the same setter
 * (ui/ui_router.c globals -> prop_set_local(sys.mode, ORIGIN_KEY)), so a
 * shell set and a long MOVE are one path. The gallery is closed first, also
 * when the mode is already the one asked for (ui_router_switch_mode): a
 * mode change hands the screen to the mode. */
static bool op_mode(uint8_t m)
{
    CTX();
    return input_mode_switch((device_mode_t)m);
}
static void op_backlight(uint8_t pct)    { CTX(); (void)pct; sys_pwm_apply_brightness(); }  /* reads sys_prefs */
static void op_autooff(uint8_t mins)     { CTX(); input_autooff_apply(mins); }              /* restarts the timer */

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
static uint8_t op_il_knots(void)         { return fpga_interleave_knots_get(NULL, NULL); }
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
    .trigger_source_follow = op_trigger_source_follow,
    .interleave_active = op_interleave_active, .vpos = op_vpos, .timebase = op_timebase,
    .roll = op_roll, .trigger_source = op_trigger_source, .trigger_level = op_trigger_level,
    .single_rearm = op_single_rearm, .single_done = op_single_done, .paused_latch = op_paused_latch,
    .cursor_axes = op_cursor_axes,
    .trigger_code = op_trigger_code,
    .view_select = op_view_select, .view_half_enter = op_view_half_enter, .persist_on = op_persist_on, .persist_off = op_persist_off,
    .persist_mode = op_persist_mode, .fft_settings = op_fft_settings,
    .mask_present = op_mask_present, .mask_get = op_mask_get, .mask_tol = op_mask_tol,
    .mask_stopfail = op_mask_stopfail, .mask_state = op_mask_state, .mask_hold = op_mask_hold,
    .mask_pending = op_mask_pending, .mask_tol_pending = op_mask_tol_pending,
    .gen_wave = op_gen_wave, .gen_freq = op_gen_freq, .gen_amp = op_gen_amp,
    .gen_duty = op_gen_duty, .gen_out = op_gen_out, .gen_bootout = op_gen_bootout,
    .meter_func = op_meter_func, .meter_link_on = fpga_meter_link_on,
    .meter_flag_get = op_meter_flag_get, .meter_flag_set = op_meter_flag_set, .meter_layout = op_meter_layout,
    .mode = op_mode, .backlight = op_backlight, .autooff = op_autooff,
    .il_enable = op_il_enable, .il_enabled = op_il_enabled, .il_corr_set = op_il_corr_set,
    .il_corr_get = op_il_corr_get, .il_knots = op_il_knots, .il_skew_set = op_il_skew_set, .il_skew_get = op_il_skew_get,
    .selfcal_busy = op_selfcal_busy, .shell_persist = op_shell_persist,
    .note_change = op_note_change,
};

/* ═══ not wired yet ══════════════════════════════════════════════════ */

/* The reason fits a prop_note_t (23 characters). */
static const struct { uint16_t id; const char *why; } k_unwired[] = {
    /* (view.run left this table with B7: its setter latches and re-arms.)
     * Stored only: nothing applies the language yet, so a set would change
     * nothing the user can see. */
    { PROP_ID_SYS_LANG,       "no languages yet" },
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

/* One repaint per shell request that changed something the screen shows
 * (the visible screens' property groups, screen_groups()), as a key's
 * repaint does. A set the screen does not show, e.g. a stored
 * meter.func on the scope, repaints nothing. A full repaint, except on the
 * meter screen for a change of the meter's own group only: that screen
 * paints such a change in place (its epoch; ui/meter_screen.c), so a
 * `set meter.func ohm` there does not clear the LCD. Never waits (a full
 * display queue drops it, and the periodic redraw gates catch up). */
static void port_changed(uint16_t groups)
{
    const uint16_t shown = (uint16_t)(groups & screen_groups());
    if (!shown) return;
    uint8_t cmd = DCMD_REDRAW_ALL;
    if (shown == (uint16_t)(1u << PG_METER) && screen_top() == SCR_METER) {
        /* The software autoselect steps through its candidates as posted
         * meter.func sets: the page is held still while it runs, as it
         * always was, and its tick paints the result when it ends. */
        if (meter_autoselect_is_running()) return;
        cmd = (uint8_t)(DCMD_DRAW_ACTIVE + (uint8_t)SCR_METER);
    } else if (screen_top() == SCR_SCOPE && scope_screen_paints(shown)) {
        /* The scope paints what changed from its own epochs (ui/
         * scope_screen.h, main.c scope_view_tick): a range, a position or a
         * trigger setting is one live frame and the bars' own slots, no
         * clear; a layout change one repaint of the plot area. The command
         * is only a wake-up. */
        cmd = (uint8_t)(DCMD_DRAW_ACTIVE + (uint8_t)SCR_SCOPE);
    } else if (screen_top() == SCR_OSC_MENU && osc_menu_screen_paints(shown)) {
        /* The Oscilloscope Settings page shows the value: painted in place
         * (its epoch moved), no clear. */
        cmd = (uint8_t)(DCMD_DRAW_ACTIVE + (uint8_t)SCR_OSC_MENU);
    }
    if (g_display_q) (void)xQueueSend(g_display_q, &cmd, 0);
}

static void port_service(void)
{
    scope_regs_service();
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
    /* What the loads kept for the setters (model.h, "VALUES A LOAD KEEPS"):
     * the display view the settings record holds and, with Startup on Boot
     * = Restore, the session snapshot's values. Through their setters,
     * ORIGIN_BOOT, here, because this is the first point where the owners
     * can be called: the input task, the scheduler running, the ops bound.
     * Refused (no pool, a park timed out, a rule): that property stays
     * where the settings record put it and `session status` lists it. The
     * groups that moved go to the screen as after any request. A check-in
     * first and between the sets: the boot logo's hold (~1 s, no check-in)
     * is behind us and each park can take 1 s more, against a 2 s health
     * deadline. */
    port_checkin();
    (void)model_boot_apply(port_checkin);
    port_changed(model_touched_take());
}
