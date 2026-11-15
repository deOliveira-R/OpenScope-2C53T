/*
 * Host tests for the scope screen (src/ui/scope_screen.c; ARCH-1 B7): the
 * real screen, the real router (screen.c), the real key table (key_model.c),
 * the real Oscilloscope Settings page layer (osc_menu_screen.c) and the real
 * property model with the scope's setters (props_scope.c), with fake
 * hardware ops and fake procedure owners that log every call.
 *
 * What is checked:
 *   - every key, short and long, in every arrow stage and view, against the
 *     ORACLE: the scope arms of input_handler.c's exec_key_cmd() and
 *     mode_key() as they were before B7 (base fd20895), transcribed below
 *     with the driver calls logged under the names the fake ops log. Over
 *     thousands of random scope states the screen must leave the same
 *     state, make the same owner calls in the same order, show the same
 *     popup and ask for the same repaint. The deliberate differences are
 *     tested by name (a failed relay / register write keeps the old value;
 *     FFT AUTO is a request);
 *   - every change of scope state is a property set: the value prop_get
 *     answers, the group epoch, the settings note for a saved property;
 *   - a key and the direct route's `set` of the same value are one path:
 *     same state, same owner calls (V/div, timebase and roll, position,
 *     trigger level and column, coupling, enable, run/stop, the cursors);
 *   - RUN/STOP: STOP latches, SINGLE re-arms, a held mask failure resumes;
 *   - the mask keys build on the request in flight;
 *   - the two epochs the display task reads: which fields are layout (one
 *     plot repaint) and which are soft (one live frame, no clear).
 *
 * Build: make test-scope-screen.
 */
#define SCREEN_TEST_ENV_REAL_FOCUS
#include "screen_test_env.h"
#include "scope_state_pre_b7.h"
#include "scope_screen.h"
#include "osc_menu_screen.h"
#include "osc_menu_model.h"
#include "scope_mask.h"
#include "scope_roll.h"
#include "scope_geom.h"
#include "scope_cal.h"
#include "scope_timebase.h"
#include "persistence.h"
#include "math_channel.h"
#include "fft.h"

#define LONG(b) ((uint8_t)((b) | BTN_LONG_FLAG))
static int32_t G(const prop_desc_t *p) { return prop_get(p); }

/* ── the paint side ────────────────────────────────────────────────── */
static char g_popup[64];
static int  g_draws[3], g_ticks;
void scope_show_popup(const char *t) { snprintf(g_popup, sizeof g_popup, "%s", t); }
void scope_ui_request_live(void)     { env_logf("live;"); }
void scope_view_draw(scr_paint_t how, uint32_t frame) { (void)frame; g_draws[how]++; }
redraw_action_t scope_view_tick(uint32_t frame) { (void)frame; g_ticks++; return REDRAW_SKIP; }
static uint32_t g_fft_epoch;
uint32_t scope_view_fft_epoch(void)  { return g_fft_epoch; }
void osc_menu_draw(void)       { }
void osc_menu_invalidate(void) { }
static uint8_t t_persist_mode;
persist_mode_t persist_get_mode(void) { return (persist_mode_t)t_persist_mode; }

/* ── the owners: one log for the fake ops (the screen's path) and for the
 *    driver functions the oracle calls (the pre-B7 path) ─────────────── */
static bool t_fail;                    /* every relay / register write says no */
static bool t_il;                      /* single-channel interleave in force */
static bool t_single_done, t_busy_req; /* SINGLE holds its record; a self-cal run is queued */
static uint8_t t_trig_code = 91;

/* The mask's mailbox (scope_mask.c): one slot, newest wins; t_mask_slow =
 * the acquisition task does not poll until mask_poll(). */
static bool     t_mask_slow, t_mask_hold;
static uint8_t  t_mask_state;                      /* mask_pf_state_t */
static uint8_t  t_tolv, t_tolh; static bool t_sf;  /* applied */
static uint32_t t_seq, t_applied_seq, t_tol_seq, t_pending_seq;
static struct { bool full; scope_mask_req_t k; uint8_t a, b; } t_mbox;
static void mask_poll(void)
{
    if (!t_mbox.full) return;
    t_mbox.full = false;
    t_applied_seq = t_seq;
    switch (t_mbox.k) {
    case SCOPE_MASK_REQ_TOL:      t_tolv = t_mbox.a; t_tolh = t_mbox.b; break;
    case SCOPE_MASK_REQ_STOPFAIL: t_sf = t_mbox.a != 0; break;
    case SCOPE_MASK_REQ_RELEASE:  t_mask_hold = false; break;
    case SCOPE_MASK_REQ_CLEAR:    t_mask_state = MASK_PF_EMPTY; break;
    case SCOPE_MASK_REQ_TEACH:    t_mask_state = MASK_PF_TEACHING; break;
    default: break;
    }
}
uint32_t scope_mask_post(scope_mask_req_t k, uint8_t a, uint8_t b, uint16_t n)
{
    (void)n;
    env_logf("mask_post(%d,%u,%u);", (int)k, a, b);
    t_mbox.full = true; t_mbox.k = k; t_mbox.a = a; t_mbox.b = b;
    t_seq++;
    if (!t_mask_slow) mask_poll();
    return t_seq;
}
bool scope_mask_acked(uint32_t seq) { return (int32_t)(t_applied_seq - seq) >= 0; }
bool scope_mask_hold_active(void)   { return t_mask_hold; }
mask_pf_state_t scope_mask_state(void) { return (mask_pf_state_t)t_mask_state; }
void scope_mask_summary(scope_mask_summary_t *s)
{
    memset(s, 0, sizeof *s);
    s->state = (mask_pf_state_t)t_mask_state; s->tol_v = t_tolv; s->tol_h = t_tolh; s->stop_on_fail = t_sf;
}
/* The AUTO key's mask procedure (scope_mask.c in the firmware): both paths
 * call the one function. */
bool scope_mask_key_auto(char *text, unsigned n)
{
    env_logf("mask_auto;");
    if (t_mask_state == MASK_PF_EMPTY) { snprintf(text, n, "MASK: teach 8"); return false; }
    snprintf(text, n, t_mask_state == MASK_PF_TEACHING ? "MASK: cancelled" : "MASK: off");
    t_mask_state = MASK_PF_EMPTY;
    return true;
}

bool scope_selfcal_request_autoset(void)          { env_logf("autoset;"); return !t_busy_req; }
bool scope_selfcal_request_basecal(uint8_t banks) { env_logf("basecal(%u);", banks); return !t_busy_req; }
bool scope_selfcal_request_center50(void)         { env_logf("center50;"); return !t_busy_req; }

fft_window_t fft_cycle_window(void)        { env_logf("fft_window;"); return (fft_window_t)0; }
float fft_adjust_ref_level(float d)        { env_logf("fft_ref(%d);", (int)d); return 0.0f; }
void  fft_zoom_in(void)                    { env_logf("fft_zoom_in;"); }
void  fft_zoom_out(void)                   { env_logf("fft_zoom_out;"); }
void  fft_auto_request(void)               { env_logf("fft_auto;"); }

/* The driver calls of the pre-B7 key handlers (fpga.c, scope_vpos.c,
 * scope_ui.c), logged as the ops below log theirs. Channels: 0 = CH1. */
static bool d_vdiv(uint8_t ch, uint8_t idx)   { env_logf("vdiv(%u,%u);", ch, idx); return !t_fail; }
static bool d_coupling(uint8_t ch, uint8_t c) { env_logf("coupling(%u,%u);", ch, c); return !t_fail; }
static bool d_enables(void)
{
    const scope_state_t *ss = scope_state_get();
    env_logf("enables(%d,%d);", ss->ch1.enabled, ss->ch2.enabled);
    return ss->ch1.enabled || ss->ch2.enabled;
}
static bool d_vpos(uint8_t ch, int16_t pos)   { env_logf("vpos(%u,%d);", ch, pos); return true; }
static bool d_timebase(uint8_t c)             { env_logf("tb(%u);", c); return !t_fail; }
static bool d_roll(void)
{
    const scope_state_t *ss = scope_state_get();
    env_logf("roll(%d,%u);", ss->roll, ss->roll_step);
    return !t_fail;
}
static bool d_trig_level(int16_t l)           { env_logf("triglvl(%d);", l); return !t_fail; }
static void d_rearm(void)                     { env_logf("rearm;"); }
static void d_latch(void)                     { env_logf("latch;"); }

/* the ops (model_hw_fw.c in the firmware) */
static bool f_il(void)            { return t_il; }
static bool f_single_done(void)   { return t_single_done; }
static uint8_t f_trig_code(void)  { return t_trig_code; }
static void f_mask_get(uint8_t *v, uint8_t *h, bool *s) { *v = t_tolv; *h = t_tolh; *s = t_sf; }
static bool f_mask_tol(uint8_t v, uint8_t h)
{
    t_tol_seq = scope_mask_post(SCOPE_MASK_REQ_TOL, v, h, 0);
    if (!scope_mask_acked(t_tol_seq)) t_pending_seq = t_tol_seq;
    return true;
}
static bool f_mask_sf(bool on)
{
    const uint32_t seq = scope_mask_post(SCOPE_MASK_REQ_STOPFAIL, on ? 1u : 0u, 0, 0);
    if (!scope_mask_acked(seq)) t_pending_seq = seq;
    return true;
}
static uint8_t f_mask_state(void)   { return t_mask_state; }
static bool f_mask_hold(void)       { return t_mask_hold; }
static bool f_mask_pending(void)    { return t_pending_seq != 0u && !scope_mask_acked(t_pending_seq); }
static bool f_mask_tol_pending(void){ return t_tol_seq != 0u && !scope_mask_acked(t_tol_seq); }
static bool f_view(uint8_t v)       { env_logf("view(%u);", v); scope_view = (scope_view_t)v; return true; }
static bool f_persist_on(uint8_t m) { persist_enabled = true; t_persist_mode = m; return true; }
static void f_persist_off(void)     { persist_enabled = false; }
static uint8_t f_persist_mode(void) { return t_persist_mode; }
static bool f_trigsrc(void)         { env_logf("trigsrc;"); return true; }

static model_hw_ops_t t_ops;

/* ── the router's hooks, as legacy_screens.c wires them ─────────────── */
static int g_after; static scr_res_t g_after_res;
static void h_after(scr_res_t r) { g_after++; g_after_res = r; }
static bool h_globals(button_id_t b, bool l) { return key_map_global(NULL, b, l).act != KA_LEGACY; }
static const screen_hooks_t HOOKS = { .globals = h_globals, .after_key = h_after };
static const screen_t S_METER = ENV_BASE("meter", SCR_METER, MODE_MULTIMETER, 1);

static void boot(void)
{
    env_fresh(MODE_OSCILLOSCOPE);
    t_ops = env_ops;
    t_ops.vdiv = d_vdiv; t_ops.coupling = d_coupling; t_ops.channel_enables = d_enables;
    t_ops.interleave_active = f_il; t_ops.vpos = d_vpos; t_ops.timebase = d_timebase; t_ops.roll = d_roll;
    t_ops.trigger_source = f_trigsrc; t_ops.trigger_level = d_trig_level; t_ops.single_rearm = d_rearm;
    t_ops.single_done = f_single_done; t_ops.paused_latch = d_latch; t_ops.trigger_code = f_trig_code;
    t_ops.view_select = f_view; t_ops.persist_on = f_persist_on; t_ops.persist_off = f_persist_off;
    t_ops.persist_mode = f_persist_mode;
    t_ops.mask_get = f_mask_get; t_ops.mask_tol = f_mask_tol; t_ops.mask_stopfail = f_mask_sf;
    t_ops.mask_state = f_mask_state; t_ops.mask_hold = f_mask_hold; t_ops.mask_pending = f_mask_pending;
    t_ops.mask_tol_pending = f_mask_tol_pending;
    t_fail = t_il = t_single_done = t_busy_req = false;
    t_mask_slow = t_mask_hold = false; t_mask_state = MASK_PF_EMPTY; t_tolv = 8; t_tolh = 2; t_sf = true;
    t_seq = t_applied_seq = t_tol_seq = t_pending_seq = 0; t_mbox.full = false;
    t_persist_mode = 0; g_fft_epoch = 0;
    model_bind_hw(&t_ops);
    (void)model_load_defaults(PF_SESSION, NULL, false);
    mask_poll();
    scope_key_focus = KF_ZOOM;
    scope_trig_level_focus = scope_hpos_focus = scope_mask_focus = false;
    active_channel = 0;
    osc_menu_close();
    screen_reset();
    screen_register(&scope_screen); screen_register(&S_METER); screen_register(&osc_menu_screen);
    screen_set_hooks(&HOOKS);
    screen_init(SCR_SCOPE);
    g_popup[0] = '\0'; g_after = 0; g_draws[0] = g_draws[1] = g_draws[2] = 0; g_ticks = 0;
    env_notes = 0;
    env_log_clear();
}

/* ═══ the oracle: input_handler.c before B7 (fd20895), the scope's arms ═══
 * key_ctx_now() + exec_key_cmd() + mode_key(), with osc_menu_env_cursors /
 * set_focus as they were. o_cmd: 'F' = DCMD_REDRAW_ALL sent, 'P' = the
 * page's paint sent, '-' = nothing. */
static char o_cmd;
static key_focus_t o_focus;                 /* scope_key_focus of the old code */
static void o_set_focus(key_focus_t f) { o_focus = f; }
static void o_popup_and_redraw(const char *t) { scope_show_popup(t); o_cmd = 'F'; }
static uint32_t o_tol_pending_seq; static uint8_t o_tol_pending_v, o_tol_pending_h;

static bool o_mask_req(scope_mask_req_t k, uint8_t a, uint8_t b, uint16_t n)
{
    return scope_mask_acked(scope_mask_post(k, a, b, n));
}
static void o_mask_adjust_tol(int dv, int dh, char *pb, size_t n)
{
    scope_mask_summary_t m;
    scope_mask_summary(&m);
    int base_v = m.tol_v, base_h = m.tol_h;
    if (o_tol_pending_seq && !scope_mask_acked(o_tol_pending_seq)) { base_v = o_tol_pending_v; base_h = o_tol_pending_h; }
    int v = base_v + dv, h = base_h + dh;
    if (v < 0) v = 0;
    if (v > 64) v = 64;
    if (h < 0) h = 0;
    if (h > (int)MASK_PF_HMAX) h = (int)MASK_PF_HMAX;
    o_tol_pending_v = (uint8_t)v; o_tol_pending_h = (uint8_t)h;
    o_tol_pending_seq = scope_mask_post(SCOPE_MASK_REQ_TOL, (uint8_t)v, (uint8_t)h, 0);
    if (!scope_mask_acked(o_tol_pending_seq)) { snprintf(pb, n, "Mask V%u H%u ...", (unsigned)v, (unsigned)h); return; }
    scope_mask_summary(&m);
    snprintf(pb, n, "Mask V%u H%u", (unsigned)m.tol_v, (unsigned)m.tol_h);
}

static key_ctx_t o_ctx(void)
{
    const scope_state_t *ss = scope_state_get();
    key_ctx_t c;
    memset(&c, 0, sizeof c);
    c.mode = MODE_OSCILLOSCOPE;
    c.scope_time_view = (scope_view == SCOPE_VIEW_TIME);
    c.cursors_on = (ss->cursor.mode != CURSOR_OFF);
    c.mask_present = (scope_mask_state() != MASK_PF_EMPTY);
    const key_focus_t f = key_focus_sanitize(o_focus, c.cursors_on, c.mask_present);
    if (f != o_focus) o_set_focus(f);
    c.focus = f;
    c.active_channel = active_channel;
    c.osc_menu_open = osc_menu_is_open();
    c.osc_menu_locked = c.osc_menu_open && osc_menu_is_locked();
    c.osc_menu_opener = osc_menu_screen_opener();
    return c;
}

static void o_exec(key_cmd_t k, button_id_t btn)
{
    scope_state_t *ss = scope_state_get();
    char pb[24];
    channel_state_t *ach = (active_channel == 0) ? &ss->ch1 : &ss->ch2;
    const int dir = k.arg;

    switch (k.act) {
    case KA_MEAS_HIDE_TOGGLE:
        ss->prm_hidden = !ss->prm_hidden;
        o_popup_and_redraw(ss->prm_hidden ? "Measurements hidden" : "Measurements shown");
        break;
    case KA_CH_ENABLE_TOGGLE: {
        channel_state_t *c = (dir == 0) ? &ss->ch1 : &ss->ch2;
        const bool was = c->enabled;
        c->enabled = !was;
        if (!d_enables()) { c->enabled = was; o_popup_and_redraw("Keep one CH on"); break; }
        if (!c->enabled && (active_channel == (uint8_t)dir)) active_channel = (uint8_t)(1u - (uint8_t)dir);
        snprintf(pb, sizeof(pb), "CH%d %s%s", dir + 1, c->enabled ? "ON" : "OFF", t_il ? " 250MS/s" : "");
        o_popup_and_redraw(pb);
        break;
    }
    case KA_BASECAL:
        if (!scope_selfcal_request_basecal(0x03u)) o_popup_and_redraw("Busy");
        break;
    case KA_CENTER50:
        if (!scope_selfcal_request_center50()) o_popup_and_redraw("Busy");
        break;
    case KA_FOCUS_NEXT: {
        const key_focus_t f = key_focus_next(o_focus, ss->cursor.mode != CURSOR_OFF,
                                             scope_mask_state() != MASK_PF_EMPTY);
        o_set_focus(f);
        o_popup_and_redraw(key_focus_popup(f));
        break;
    }
    case KA_ADJ_CH_SWITCH: {
        const uint8_t other = (uint8_t)(1u - active_channel);
        const channel_state_t *oc = (other == 0) ? &ss->ch1 : &ss->ch2;
        if (oc->enabled) { active_channel = other; snprintf(pb, sizeof(pb), "Adjust CH%d", other + 1); }
        else             { snprintf(pb, sizeof(pb), "CH%d is off", other + 1); }
        o_popup_and_redraw(pb);
        break;
    }
    case KA_TB_STEP: {
        const bool was_roll = ss->roll;
        const uint8_t was_step = ss->roll_step;
        const bool roll_changed = scope_adjust_timebase(ss, dir);
        const bool tb_ok = roll_changed ? d_roll() : (was_roll ? true : d_timebase(ss->timebase_idx));
        if (roll_changed && !tb_ok) { ss->roll = was_roll; ss->roll_step = was_step; }
        if (tb_ok) { o_cmd = 'F'; break; }
        char tb[12];
        if (ss->roll) scope_roll_label(ss->roll_step, tb, sizeof(tb));
        else          scope_timebase_view_label(ss->timebase_idx, t_il, tb, sizeof(tb));
        snprintf(pb, sizeof(pb), "H=%s NOT SET", tb);
        o_popup_and_redraw(pb);
        break;
    }
    case KA_VDIV_STEP: {
        scope_adjust_vdiv(ach, dir);
        const bool vd_ok = d_vdiv(active_channel, ach->vdiv_idx);
        if (vd_ok) { o_cmd = 'F'; break; }
        char vd[12];
        scope_cal_range_label((uint8_t)(active_channel + 1), ach->vdiv_idx, vd, sizeof(vd));
        snprintf(pb, sizeof(pb), "CH%d %s NOT SET", active_channel + 1, vd);
        o_popup_and_redraw(pb);
        break;
    }
    case KA_CHPOS_STEP:
        scope_adjust_position(ach, dir);
        (void)d_vpos(active_channel, ach->position);
        snprintf(pb, sizeof(pb), "CH%d pos %+d", active_channel + 1, (int)ach->position);
        o_popup_and_redraw(pb);
        break;
    case KA_TRIGCOL_STEP: {
        int tx = ss->trig_x + dir * 16;
        if (tx < SCOPE_GEOM_TRIG_COL_MIN) tx = SCOPE_GEOM_TRIG_COL_MIN;
        if (tx > SCOPE_GEOM_TRIG_COL_MAX) tx = SCOPE_GEOM_TRIG_COL_MAX;
        ss->trig_x = (int16_t)tx;
        snprintf(pb, sizeof(pb), "Trig at x=%d", tx);
        o_popup_and_redraw(pb);
        break;
    }
    case KA_TRIGLVL_STEP: {
        scope_adjust_trigger_level(ss, dir);
        const bool lv_ok = d_trig_level(ss->trigger.level);
        snprintf(pb, sizeof(pb), lv_ok ? "Trig %+d  code %u" : "Trig %+d  NOT SET",
                 ss->trigger.level, (unsigned)t_trig_code);
        o_popup_and_redraw(pb);
        break;
    }
    case KA_CURSOR_MOVE:
        scope_cursor_move((int16_t)(dir * 4));
        scope_ui_request_live();
        break;
    case KA_CURSOR_NEXT:
        scope_cursor_next_sel();
        scope_ui_request_live();
        break;
    case KA_MASK_TOL_V:
        o_mask_adjust_tol(dir * 2, 0, pb, sizeof(pb));
        o_popup_and_redraw(pb);
        break;
    case KA_MASK_TOL_H:
        o_mask_adjust_tol(0, dir, pb, sizeof(pb));
        o_popup_and_redraw(pb);
        break;
    case KA_OSCMENU_OPEN:
        osc_menu_screen_open((osc_row_t)dir, false, (uint8_t)btn);
        o_cmd = 'P';
        break;
    case KA_OSCMENU_PANE:
        osc_menu_screen_open((osc_row_t)dir, true, (uint8_t)btn);
        o_cmd = 'P';
        break;
    case KA_MASK_STOPFAIL: {
        scope_mask_summary_t m;
        scope_mask_summary(&m);
        bool ok = o_mask_req(SCOPE_MASK_REQ_STOPFAIL, m.stop_on_fail ? 0u : 1u, 0, 0);
        scope_mask_summary(&m);
        snprintf(pb, sizeof(pb), ok ? "Mask stop-on-fail %s" : "Mask stop NOT SET", m.stop_on_fail ? "ON" : "OFF");
        o_popup_and_redraw(pb);
        break;
    }
    default:
        break;
    }
}

/* scope_apply_coupling_toggle (scope_apply.c) */
static void o_coupling_toggle(uint8_t ch, char *pb, size_t n)
{
    scope_state_t *ss = scope_state_get();
    channel_state_t *c = ch ? &ss->ch2 : &ss->ch1;
    const coupling_t next = (c->coupling == COUPLING_DC) ? COUPLING_AC : COUPLING_DC;
    const bool ok = d_coupling(ch, (uint8_t)next);
    if (ok) c->coupling = next;
    snprintf(pb, n, ok ? "CH%u %s" : "CH%u %s NOT SET", (unsigned)ch + 1u, coupling_labels[next]);
}

static void o_mode_key(button_id_t button)
{
    scope_state_t *ss = scope_state_get();
    char pb[24];
    switch (button) {
    case BTN_CH1: active_channel = 0; o_coupling_toggle(0, pb, sizeof(pb)); o_popup_and_redraw(pb); break;
    case BTN_CH2: active_channel = 1; o_coupling_toggle(1, pb, sizeof(pb)); o_popup_and_redraw(pb); break;
    case BTN_AUTO:
        if (scope_view != SCOPE_VIEW_TIME) {
            /* fft_auto_configure() on the record the view analyses, from the
             * input task; a request the display task serves since B7. X-Y
             * holds no FFT buffer: nothing. */
            if (scope_view != SCOPE_VIEW_XY) env_logf("fft_auto;");
            o_cmd = 'F';
            break;
        }
        if (scope_mask_state() != MASK_PF_EMPTY || o_focus == KF_MASK) {
            /* mask_auto_key(): the same procedure; the clear / cancel branch
             * dropped the mask stage */
            if (scope_mask_key_auto(pb, sizeof(pb)) && o_focus == KF_MASK) o_set_focus(KF_ZOOM);
            o_popup_and_redraw(pb);
        } else if (!scope_selfcal_request_autoset()) {
            o_popup_and_redraw("Busy");
        }
        break;
    case BTN_SELECT:
        if (scope_view != SCOPE_VIEW_TIME) { (void)fft_cycle_window(); o_cmd = 'F'; }
        break;
    case BTN_UP:    if (scope_view != SCOPE_VIEW_TIME) { (void)fft_adjust_ref_level(5.0f);  o_cmd = 'F'; } break;
    case BTN_DOWN:  if (scope_view != SCOPE_VIEW_TIME) { (void)fft_adjust_ref_level(-5.0f); o_cmd = 'F'; } break;
    case BTN_LEFT:  if (scope_view != SCOPE_VIEW_TIME) { fft_zoom_in();  o_cmd = 'F'; } break;
    case BTN_RIGHT: if (scope_view != SCOPE_VIEW_TIME) { fft_zoom_out(); o_cmd = 'F'; } break;
    case BTN_OK:
        if (scope_mask_hold_active()) {
            (void)scope_mask_post(SCOPE_MASK_REQ_RELEASE, 0, 0, 0);
            scope_show_popup("MASK: RESUME");
        } else if (ss->trigger.mode == TRIG_SINGLE && (!ss->running || t_single_done)) {
            ss->running = true;
            d_rearm();
            scope_show_popup("Await Trigger...");
        } else {
            scope_toggle_running(ss);
            if (!ss->running) d_latch();
        }
        o_cmd = 'F';
        break;
    default:
        break;
    }
}

/* The pre-B7 key_model.c answered KA_LEGACY for what B7 named: those went
 * to the switch. */
static bool b7_named(key_action_t a)
{
    return a == KA_COUPLING_TOGGLE || a == KA_RUNSTOP || a == KA_AUTOSET || a == KA_FFT_AUTO ||
           a == KA_FFT_WINDOW || a == KA_FFT_REF || a == KA_FFT_ZOOM;
}
static void o_key(uint8_t raw)
{
    const button_id_t b = (button_id_t)(raw & (uint8_t)~BTN_LONG_FLAG);
    const bool lng = (raw & BTN_LONG_FLAG) != 0;
    o_cmd = '-';
    const key_ctx_t c = o_ctx();
    const key_cmd_t k = key_map(&c, b, lng);
    if (lng) { o_exec(k, b); return; }
    if (k.act != KA_LEGACY && !b7_named(k.act)) { o_exec(k, b); return; }
    o_mode_key(b);
}

/* ═══ states ════════════════════════════════════════════════════════ */

typedef struct {
    scope_state_t ss;
    uint8_t  ach, view, focus;
    uint8_t  mask_state; bool mask_hold, mask_slow, single_done, il, busy_req, fail;
} st_t;

typedef struct {
    scope_state_t ss;
    uint8_t  ach, view, focus;
    bool     f_trig, f_hpos, f_mask;
    osc_menu_state_t menu; uint8_t opener;
    uint8_t  tolv, tolh; bool sf, hold; uint8_t mask_state;
    char     log[1024], popup[64], cmd;
} out_t;

static unsigned rnd_s = 2463534242u;
static unsigned rnd(unsigned n) { rnd_s ^= rnd_s << 13; rnd_s ^= rnd_s >> 17; rnd_s ^= rnd_s << 5; return rnd_s % n; }

static void st_random(st_t *s)
{
    memset(s, 0, sizeof *s);
    scope_state_init(&s->ss);
    s->ss.ch1.vdiv_idx = (uint8_t)rnd(VDIV_COUNT); s->ss.ch2.vdiv_idx = (uint8_t)rnd(VDIV_COUNT);
    s->ss.ch1.position = (int16_t)((int)rnd(41) * 5 - 100); s->ss.ch2.position = (int16_t)((int)rnd(41) * 5 - 100);
    s->ss.ch1.coupling = (coupling_t)rnd(2); s->ss.ch2.coupling = (coupling_t)rnd(2);
    const unsigned en = 1u + rnd(3);
    s->ss.ch1.enabled = en & 1u; s->ss.ch2.enabled = (en >> 1) & 1u;
    /* the ends of every ladder more often than chance */
    s->ss.timebase_idx = rnd(4) == 0 ? (rnd(2) ? 0 : TIMEBASE_COUNT - 1) : (uint8_t)rnd(TIMEBASE_COUNT);
    s->ss.roll = rnd(4) == 0; s->ss.roll_step = rnd(3) == 0 ? (rnd(2) ? 0 : SCOPE_ROLL_STEPS - 1) : (uint8_t)rnd(SCOPE_ROLL_STEPS);
    if (rnd(5) == 0) s->ss.ch1.vdiv_idx = rnd(2) ? 0 : VDIV_COUNT - 1;
    if (rnd(5) == 0) s->ss.ch1.position = rnd(2) ? -100 : 100;
    s->ss.trigger.mode = (trigger_mode_t)rnd(3); s->ss.trigger.edge = (trigger_edge_t)rnd(2);
    s->ss.trigger.source = (trigger_source_t)rnd(2);
    s->ss.trigger.level = rnd(5) == 0 ? (rnd(2) ? -100 : 100) : (int16_t)((int)rnd(41) * 5 - 100);
    s->ss.trig_x = rnd(5) == 0 ? (rnd(2) ? 0 : 300) : (int16_t)rnd(301);
    s->ss.running = rnd(3) != 0; s->ss.prm_hidden = rnd(4) == 0;
    s->ss.cursor.mode = (cursor_mode_t)rnd(4);
    s->ss.cursor.active = s->ss.cursor.mode == CURSOR_HORIZONTAL ? (cursor_sel_t)(2 + rnd(2))
                        : s->ss.cursor.mode == CURSOR_BOTH ? (cursor_sel_t)rnd(4) : (cursor_sel_t)rnd(2);
    s->ss.cursor.v1_x = rnd(6) == 0 ? (rnd(2) ? 0 : 319) : (uint16_t)rnd(320);
    s->ss.cursor.v2_x = (uint16_t)rnd(320);
    s->ss.cursor.h1_y = rnd(6) == 0 ? (rnd(2) ? 18 : 224) : (uint16_t)(18 + rnd(207));
    s->ss.cursor.h2_y = (uint16_t)(18 + rnd(207));
    s->ach = (uint8_t)rnd(2);
    s->view = rnd(3) == 0 ? (uint8_t)(1 + rnd(4)) : (uint8_t)SCOPE_VIEW_TIME;
    if (s->view != SCOPE_VIEW_TIME) s->ss.roll = false;
    s->focus = (uint8_t)rnd(KF_COUNT);              /* possibly a stage that no longer exists */
    s->mask_state = rnd(3) == 0 ? (uint8_t)(1 + rnd(2)) : (uint8_t)MASK_PF_EMPTY;
    s->mask_hold = s->mask_state == MASK_PF_READY && rnd(3) == 0;
    s->mask_slow = rnd(4) == 0;
    s->single_done = rnd(2) != 0; s->il = rnd(4) == 0; s->busy_req = rnd(6) == 0;
}

static void st_load(const st_t *s)
{
    boot();
    *scope_state_get() = s->ss;
    active_channel = s->ach; scope_view = (scope_view_t)s->view;
    scope_key_focus = (key_focus_t)s->focus; o_focus = (key_focus_t)s->focus;
    scope_trig_level_focus = (s->focus == KF_TRIGMOVE);
    scope_hpos_focus = (s->focus == KF_CHMOVE || s->focus == KF_TRIGMOVE);
    scope_mask_focus = (s->focus == KF_MASK);
    t_mask_state = s->mask_state; t_mask_hold = s->mask_hold; t_mask_slow = s->mask_slow;
    t_single_done = s->single_done; t_il = s->il; t_busy_req = s->busy_req; t_fail = s->fail;
    o_tol_pending_seq = 0;
    g_popup[0] = '\0'; env_notes = 0;
    env_log_clear();
}

static void out_take(out_t *o, bool old)
{
    memset(o, 0, sizeof *o);
    o->ss = *scope_state_get();
    o->ach = active_channel; o->view = (uint8_t)scope_view;
    o->focus = old ? (uint8_t)o_focus : (uint8_t)scope_key_focus;
    o->f_trig = old ? (o_focus == KF_TRIGMOVE) : scope_trig_level_focus;
    o->f_hpos = old ? (o_focus == KF_CHMOVE || o_focus == KF_TRIGMOVE) : scope_hpos_focus;
    o->f_mask = old ? (o_focus == KF_MASK) : scope_mask_focus;
    o->menu = *osc_menu_state(); o->opener = osc_menu_screen_opener();
    o->menu.opens = 0;                               /* a visit counter: not state of the page */
    mask_poll();                                     /* what the acquisition task finally applies */
    o->tolv = t_tolv; o->tolh = t_tolh; o->sf = t_sf; o->hold = t_mask_hold; o->mask_state = t_mask_state;
    snprintf(o->log, sizeof o->log, "%s", env_log);
    snprintf(o->popup, sizeof o->popup, "%s", g_popup);
}

static char res_cmd(scr_res_t r) { return r == SCR_DRAW_FULL ? 'F' : r == SCR_DRAW ? 'P' : '-'; }

static void run_old(const st_t *s, uint8_t raw, out_t *o)
{
    st_load(s);
    o_key(raw);
    out_take(o, true);
    o->cmd = o_cmd;
}
static void run_new(const st_t *s, uint8_t raw, out_t *o)
{
    st_load(s);
    g_after = 0;
    const scr_res_t r = env_key(raw);
    out_take(o, false);
    o->cmd = res_cmd(r);
    if (g_after != 1 || g_after_res != r) o->cmd = '!';     /* the wiring posts the answer once */
}

static bool out_same(const out_t *a, const out_t *b, const char **why)
{
    *why = "";
    if (memcmp(&a->ss, &b->ss, sizeof a->ss) != 0) { *why = "scope state"; return false; }
    if (a->ach != b->ach) { *why = "active channel"; return false; }
    if (a->view != b->view) { *why = "view"; return false; }
    if (a->focus != b->focus || a->f_trig != b->f_trig || a->f_hpos != b->f_hpos || a->f_mask != b->f_mask) { *why = "focus"; return false; }
    if (memcmp(&a->menu, &b->menu, sizeof a->menu) != 0 || a->opener != b->opener) { *why = "page"; return false; }
    if (a->tolv != b->tolv || a->tolh != b->tolh || a->sf != b->sf || a->hold != b->hold || a->mask_state != b->mask_state) { *why = "mask"; return false; }
    if (strcmp(a->log, b->log) != 0) { *why = "owner calls"; return false; }
    if (strcmp(a->popup, b->popup) != 0) { *why = "popup"; return false; }
    if (a->cmd != b->cmd) { *why = "repaint"; return false; }
    return true;
}

static const uint8_t ALL_KEYS[] = {
    BTN_CH1, BTN_CH2, BTN_MOVE, BTN_SELECT, BTN_TRIGGER, BTN_PRM, BTN_AUTO, BTN_MENU,
    BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_OK,
    LONG(BTN_CH1), LONG(BTN_CH2), LONG(BTN_PRM), LONG(BTN_AUTO), LONG(BTN_OK),
};
#define N_KEYS (sizeof ALL_KEYS / sizeof ALL_KEYS[0])

/* ═══ tests ═════════════════════════════════════════════════════════ */

static void test_screen_entry(void)
{
    printf("the screen\n");
    boot();
    CHECK("name scope, id SCR_SCOPE, MODE_OSCILLOSCOPE, instrument 0 (scope), no flags",
          strcmp(scope_screen.name, "scope") == 0 && scope_screen.id == SCR_SCOPE &&
          scope_screen.mode == MODE_OSCILLOSCOPE && scope_screen.instrument == 0 && scope_screen.flags == 0);
    CHECK("a real screen: key, draw, tick and epoch; no describe (ui screen's fields are properties)",
          scope_screen.key && scope_screen.draw && scope_screen.tick && scope_screen.epoch && !scope_screen.describe);
    const uint16_t want = (uint16_t)((1u << PG_GEN) | (1u << PG_SYS) | (1u << PG_UI) | (1u << PG_CH1) |
                                     (1u << PG_CH2) | (1u << PG_TB) | (1u << PG_TRIG) | (1u << PG_VIEW) |
                                     (1u << PG_FFT) | (1u << PG_IL) | (1u << PG_MASK));
    CHECK("groups: the scope's and the bars' (not the meter's)", scope_screen.groups == want);
    CHECK("the globals never reach it: SAVE, long SAVE, the long mode keys",
          (env_key(BTN_SAVE), env_key(LONG(BTN_SAVE)), env_key(LONG(BTN_MENU)), env_key(LONG(BTN_MOVE)),
           env_log_is("")) && g_popup[0] == '\0');
    scope_screen.draw(SCR_PAINT_FULL, 3);
    (void)scope_screen.tick(4);
    CHECK("draw and tick are the paint module's", g_draws[SCR_PAINT_FULL] == 1 && g_ticks == 1);
}

/* Every key from every random state: the screen = the pre-B7 handlers. */
static void test_oracle(void)
{
    printf("every key against the pre-B7 handlers (the oracle)\n");
    unsigned cases = 0, bad = 0, by_key[N_KEYS] = {0};
    unsigned seen_stage[KF_COUNT] = {0}, seen_view[SCOPE_VIEW_COUNT] = {0};
    for (unsigned it = 0; it < 4000; it++) {
        st_t s;
        st_random(&s);
        for (unsigned k = 0; k < N_KEYS; k++) {
            out_t a, b;
            const char *why;
            run_old(&s, ALL_KEYS[k], &a);
            run_new(&s, ALL_KEYS[k], &b);
            cases++;
            if (!out_same(&a, &b, &why)) {
                if (bad++ < 12)
                    printf("    key %u view %u focus %u ach %u: %s differs: old log [%s] popup [%s] cmd %c | new log [%s] popup [%s] cmd %c\n",
                           ALL_KEYS[k], s.view, s.focus, s.ach, why, a.log, a.popup, a.cmd, b.log, b.popup, b.cmd);
                by_key[k]++;
            }
        }
        seen_stage[key_focus_sanitize((key_focus_t)s.focus, s.ss.cursor.mode != CURSOR_OFF,
                                      s.mask_state != MASK_PF_EMPTY)]++;
        seen_view[s.view]++;
    }
    printf("    %u cases\n", cases);
    CHECK("18 keys x 4000 random states: same state, owner calls, popup and repaint", bad == 0 && cases == 72000);
    bool all = true;
    for (unsigned i = 0; i < KF_COUNT; i++) all &= seen_stage[i] > 50;
    for (unsigned i = 0; i < SCOPE_VIEW_COUNT; i++) all &= seen_view[i] > 50;
    CHECK("the states cover every arrow stage and every view", all);
}

/* What each key asks of the model, by name: the property, its value, the
 * owner call, the epoch, the settings note. */
typedef struct {
    const char *name; uint8_t focus, key; const prop_desc_t *p; int32_t before, after;
    const char *log, *popup; scr_res_t res; uint16_t groups; unsigned notes;
} row_t;

static void test_key_table(void)
{
    printf("each key is one property set\n");
#define GR(g) ((uint16_t)(1u << (g)))
    const row_t rows[] = {
        { "UP (zoom): ch1.vdiv +1",              KF_ZOOM, BTN_UP,    P(CH1_VDIV), 3, 4, "vdiv(0,4);", "", SCR_DRAW_FULL, GR(PG_CH1), 1 },
        { "DOWN (zoom): ch1.vdiv -1",            KF_ZOOM, BTN_DOWN,  P(CH1_VDIV), 3, 2, "vdiv(0,2);", "", SCR_DRAW_FULL, GR(PG_CH1), 1 },
        { "RIGHT (zoom): tb.code +1",            KF_ZOOM, BTN_RIGHT, P(TB_CODE), 10, 11, "tb(11);", "", SCR_DRAW_FULL, GR(PG_TB), 1 },
        { "LEFT (zoom): tb.code -1",             KF_ZOOM, BTN_LEFT,  P(TB_CODE), 10, 9, "tb(9);", "", SCR_DRAW_FULL, GR(PG_TB), 1 },
        { "UP (channel move): ch1.pos +5",       KF_CHMOVE, BTN_UP,   P(CH1_POS), 0, 5, "vpos(0,5);", "CH1 pos +5", SCR_DRAW_FULL, GR(PG_CH1), 0 },
        { "DOWN (channel move): ch1.pos -5",     KF_CHMOVE, BTN_DOWN, P(CH1_POS), 0, -5, "vpos(0,-5);", "CH1 pos -5", SCR_DRAW_FULL, GR(PG_CH1), 0 },
        { "RIGHT (channel move): trig.x +16",    KF_CHMOVE, BTN_RIGHT, P(TRIG_X), 150, 166, "", "Trig at x=166", SCR_DRAW_FULL, GR(PG_TRIG), 0 },
        { "LEFT (trigger move): trig.x -16",     KF_TRIGMOVE, BTN_LEFT, P(TRIG_X), 150, 134, "", "Trig at x=134", SCR_DRAW_FULL, GR(PG_TRIG), 0 },
        { "UP (trigger move): trig.level +5",    KF_TRIGMOVE, BTN_UP, P(TRIG_LEVEL), 0, 5, "triglvl(5);", "Trig +5  code 91", SCR_DRAW_FULL, GR(PG_TRIG), 1 },
        { "DOWN (trigger move): trig.level -5",  KF_TRIGMOVE, BTN_DOWN, P(TRIG_LEVEL), 0, -5, "triglvl(-5);", "Trig -5  code 91", SCR_DRAW_FULL, GR(PG_TRIG), 1 },
        { "CH1: ch1.coupling AC",                KF_ZOOM, BTN_CH1,   P(CH1_COUPLING), 0, 1, "coupling(0,1);", "CH1 AC", SCR_DRAW_FULL, GR(PG_CH1), 1 },
        { "CH2: ch2.coupling AC, active = CH2",  KF_ZOOM, BTN_CH2,   P(CH2_COUPLING), 0, 1, "coupling(1,1);", "CH2 AC", SCR_DRAW_FULL, GR(PG_CH2) | GR(PG_UI), 1 },
        { "long CH2: ch2.enabled off",           KF_ZOOM, LONG(BTN_CH2), P(CH2_ENABLED), 1, 0, "enables(1,0);", "CH2 OFF", SCR_DRAW_FULL, GR(PG_CH2), 0 },
        { "long PRM: view.meashide on",          KF_ZOOM, LONG(BTN_PRM), P(VIEW_MEASHIDE), 0, 1, "", "Measurements hidden", SCR_DRAW_FULL, GR(PG_VIEW), 0 },
        { "OK: view.run off (STOP latches)",     KF_ZOOM, BTN_OK,    P(VIEW_RUN), 1, 0, "latch;", "", SCR_DRAW_FULL, GR(PG_VIEW), 0 },
        { "AUTO: the autoset request",           KF_ZOOM, BTN_AUTO,  NULL, 0, 0, "autoset;", "", SCR_HANDLED, 0, 0 },
        { "long AUTO: the baseline request",     KF_ZOOM, LONG(BTN_AUTO), NULL, 0, 0, "basecal(3);", "", SCR_HANDLED, 0, 0 },
        { "long OK: the 50 % centre request",    KF_ZOOM, LONG(BTN_OK), NULL, 0, 0, "center50;", "", SCR_HANDLED, 0, 0 },
        { "SELECT: the next arrow stage (UI state)", KF_ZOOM, BTN_SELECT, P(UI_FOCUS), KF_ZOOM, KF_CHMOVE, "", "UP/DN: CH pos  LT/RT: Trig x", SCR_DRAW_FULL, GR(PG_UI), 0 },
        { "MOVE: the other channel (UI state)",  KF_ZOOM, BTN_MOVE,  P(UI_ACH), 0, 1, "", "Adjust CH2", SCR_DRAW_FULL, GR(PG_UI), 0 },
    };
#undef GR
    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        const row_t *r = &rows[i];
        boot();
        scope_key_focus = (key_focus_t)r->focus;
        const bool pre = !r->p || G(r->p) == r->before;
        const env_epochs_t e0 = env_epochs();
        const scr_res_t res = env_key(r->key);
        const env_epochs_t e1 = env_epochs();
        const bool ok = pre && res == r->res && env_log_is(r->log) && strcmp(g_popup, r->popup) == 0 &&
                        (!r->p || G(r->p) == r->after) && env_epochs_moved(&e0, &e1) == r->groups &&
                        env_notes == r->notes;
        if (!ok) printf("    res %d log [%s] popup [%s] value %ld groups 0x%x notes %u\n", res, env_log, g_popup,
                        r->p ? (long)G(r->p) : 0L, env_epochs_moved(&e0, &e1), env_notes);
        CHECK(r->name, ok);
    }

    /* the arrows act on the ACTIVE channel */
    boot(); active_channel = 1;
    CHECK("UP with CH2 active: ch2.vdiv, not ch1's",
          env_key(BTN_UP) == SCR_DRAW_FULL && G(P(CH2_VDIV)) == 6 && G(P(CH1_VDIV)) == 3 && env_log_is("vdiv(1,6);"));

    /* cursors: each line is a property; the selection is one */
    boot();
    (void)prop_set_local(P(VIEW_CURSOR), CURSOR_BOTH, ORIGIN_SHELL, NULL);
    scope_key_focus = KF_CURSOR; env_log_clear();
    CHECK("UP (cursor): view.curv1 -4, one live frame, no repaint command",
          env_key(BTN_UP) == SCR_HANDLED && G(P(VIEW_CURV1)) == 102 && env_log_is("live;") && g_popup[0] == '\0');
    CHECK("RIGHT (cursor): view.cursel v1 -> v2 -> h1 -> h2 -> v1",
          (env_key(BTN_RIGHT), G(P(VIEW_CURSEL)) == CURSOR_SEL_V2) && (env_key(BTN_LEFT), G(P(VIEW_CURSEL)) == CURSOR_SEL_H1) &&
          (env_key(BTN_RIGHT), G(P(VIEW_CURSEL)) == CURSOR_SEL_H2) && (env_key(BTN_RIGHT), G(P(VIEW_CURSEL)) == CURSOR_SEL_V1));
    (void)prop_set_local(P(VIEW_CURSEL), CURSOR_SEL_H2, ORIGIN_SHELL, NULL);
    CHECK("DOWN with h2 selected: view.curh2 +4", (env_key(BTN_DOWN), G(P(VIEW_CURH2)) == 159) && G(P(VIEW_CURV1)) == 102);

    /* the page: UI state, opened by the screen, painted as a layer */
    boot();
    CHECK("MENU opens the page at Display; PRM / TRIGGER locked in their pane",
          env_key(BTN_MENU) == SCR_DRAW && osc_menu_is_open() && !osc_menu_is_locked() &&
          osc_menu_state()->row == OSC_ROW_DISPLAY && osc_menu_screen_opener() == BTN_MENU &&
          (osc_menu_close(), env_key(BTN_PRM) == SCR_DRAW) && osc_menu_is_locked() && osc_menu_state()->row == OSC_ROW_PARAM &&
          (osc_menu_close(), env_key(BTN_TRIGGER) == SCR_DRAW) && osc_menu_is_locked() && osc_menu_state()->row == OSC_ROW_TRIGGER &&
          env_log_is(""));

    /* spectrum views: the live FFT configuration (no property) */
    boot();
    (void)prop_set_local(P(VIEW_MODE), 3 /* fft */, ORIGIN_SHELL, NULL);
    env_log_clear();
    env_key(BTN_UP); env_key(BTN_DOWN); env_key(BTN_LEFT); env_key(BTN_RIGHT); env_key(BTN_SELECT); env_key(BTN_AUTO);
    CHECK("FFT view: UP/DOWN ref level, LEFT/RIGHT zoom, SELECT window, AUTO = the fft_auto request",
          env_log_is("fft_ref(5);fft_ref(-5);fft_zoom_in;fft_zoom_out;fft_window;fft_auto;"));
    (void)prop_set_local(P(VIEW_MODE), 2 /* xy */, ORIGIN_SHELL, NULL);
    env_log_clear();
    CHECK("X-Y: AUTO asks for nothing (no spectrum), still repaints", env_key(BTN_AUTO) == SCR_DRAW_FULL && env_log_is(""));
}

/* The ends of the ladders and the refusals. */
static void test_limits_and_failures(void)
{
    printf("limits, refusals, failed writes\n");
    boot();
    (void)prop_set_local(P(CH1_VDIV), 9, ORIGIN_SHELL, NULL); env_log_clear();
    CHECK("UP at the last range: the relays are re-applied (as before), value kept",
          env_key(BTN_UP) == SCR_DRAW_FULL && env_log_is("vdiv(0,9);") && G(P(CH1_VDIV)) == 9 && g_popup[0] == '\0');
    (void)prop_set_local(P(TB_CODE), 0, ORIGIN_SHELL, NULL); env_log_clear();
    CHECK("LEFT at the fastest timebase: the code is re-applied, value kept",
          env_key(BTN_LEFT) == SCR_DRAW_FULL && env_log_is("tb(0);") && G(P(TB_CODE)) == 0);

    /* the timebase ladder into roll and back */
    boot();
    (void)prop_set_local(P(TB_CODE), 20, ORIGIN_SHELL, NULL); env_log_clear();
    CHECK("RIGHT at the last code: tb.roll on at step 0, one roll apply",
          env_key(BTN_RIGHT) == SCR_DRAW_FULL && G(P(TB_ROLL)) == 1 && G(P(TB_ROLLSTEP)) == 0 && env_log_is("roll(1,0);"));
    env_log_clear();
    CHECK("RIGHT in roll: tb.rollstep +1, one roll apply",
          env_key(BTN_RIGHT) == SCR_DRAW_FULL && G(P(TB_ROLLSTEP)) == 1 && env_log_is("roll(1,1);"));
    (void)prop_set_local(P(TB_ROLLSTEP), 8, ORIGIN_SHELL, NULL); env_log_clear();
    CHECK("RIGHT at the slowest roll step: nothing is written",
          env_key(BTN_RIGHT) == SCR_DRAW_FULL && G(P(TB_ROLLSTEP)) == 8 && env_log_is(""));
    (void)prop_set_local(P(TB_ROLLSTEP), 0, ORIGIN_SHELL, NULL); env_log_clear();
    CHECK("LEFT at roll step 0: tb.roll off onto the last code, one roll apply",
          env_key(BTN_LEFT) == SCR_DRAW_FULL && G(P(TB_ROLL)) == 0 && G(P(TB_CODE)) == 20 && env_log_is("roll(0,0);"));
    /* a stale step from an earlier visit does not come back on entry */
    (void)prop_set_local(P(TB_ROLLSTEP), 5, ORIGIN_SHELL, NULL); env_log_clear();
    CHECK("entering roll again starts at step 0 whatever tb.rollstep held",
          (env_key(BTN_RIGHT), G(P(TB_ROLL)) == 1) && G(P(TB_ROLLSTEP)) == 0 && env_log_is("roll(1,0);"));

    /* a failed write: the old value is kept (the fix of ARCH 2.12); popup as before */
    boot(); t_fail = true;
    CHECK("V/div relay write fails: ch1.vdiv keeps 3, popup names the range asked for",
          env_key(BTN_UP) == SCR_DRAW_FULL && G(P(CH1_VDIV)) == 3 && strstr(g_popup, "CH1 ") && strstr(g_popup, " NOT SET"));
    CHECK("timebase write fails: tb.code keeps 10, 'H=... NOT SET'",
          env_key(BTN_RIGHT) == SCR_DRAW_FULL && G(P(TB_CODE)) == 10 && strncmp(g_popup, "H=", 2) == 0 && strstr(g_popup, " NOT SET"));
    scope_key_focus = KF_TRIGMOVE;
    CHECK("trigger level write fails: trig.level keeps 0, 'Trig +5  NOT SET'",
          env_key(BTN_UP) == SCR_DRAW_FULL && G(P(TRIG_LEVEL)) == 0 && strcmp(g_popup, "Trig +5  NOT SET") == 0);
    CHECK("coupling relay fails: label kept, 'CH1 AC NOT SET'",
          env_key(BTN_CH1) == SCR_DRAW_FULL && G(P(CH1_COUPLING)) == 0 && strcmp(g_popup, "CH1 AC NOT SET") == 0);
    boot();
    (void)prop_set_local(P(TB_CODE), 20, ORIGIN_SHELL, NULL);
    (void)prop_set_local(P(TB_ROLLSTEP), 5, ORIGIN_SHELL, NULL);
    t_fail = true;
    CHECK("roll refused on entry: Y-T kept, tb.rollstep put back, the popup names the code kept",
          env_key(BTN_RIGHT) == SCR_DRAW_FULL && G(P(TB_ROLL)) == 0 && G(P(TB_ROLLSTEP)) == 5 &&
          strncmp(g_popup, "H=", 2) == 0 && strstr(g_popup, " NOT SET"));

    /* refusals */
    boot();
    (void)prop_set_local(P(CH2_ENABLED), 0, ORIGIN_SHELL, NULL); env_log_clear();
    CHECK("long CH1 with CH2 off: refused, 'Keep one CH on'",
          env_key(LONG(BTN_CH1)) == SCR_DRAW_FULL && G(P(CH1_ENABLED)) == 1 && strcmp(g_popup, "Keep one CH on") == 0);
    CHECK("MOVE onto a channel that is off: 'CH2 is off', the active channel stays",
          env_key(BTN_MOVE) == SCR_DRAW_FULL && active_channel == 0 && strcmp(g_popup, "CH2 is off") == 0);
    boot(); t_il = true; active_channel = 1;
    CHECK("long CH2 (active) off with interleave: ' 250MS/s', the arrows follow CH1",
          env_key(LONG(BTN_CH2)) == SCR_DRAW_FULL && strcmp(g_popup, "CH2 OFF 250MS/s") == 0 && active_channel == 0);
    boot(); t_busy_req = true;
    CHECK("a run already queued: AUTO, long AUTO, long OK say 'Busy'",
          env_key(BTN_AUTO) == SCR_DRAW_FULL && strcmp(g_popup, "Busy") == 0 &&
          (g_popup[0] = '\0', env_key(LONG(BTN_AUTO)) == SCR_DRAW_FULL) && strcmp(g_popup, "Busy") == 0 &&
          (g_popup[0] = '\0', env_key(LONG(BTN_OK)) == SCR_DRAW_FULL) && strcmp(g_popup, "Busy") == 0);
}

/* RUN/STOP: the setter is the key. */
static void test_run_stop(void)
{
    printf("RUN/STOP, SINGLE, the mask hold\n");
    scope_state_t *ss = scope_state_get();
    prop_note_t n;
    boot();
    CHECK("OK: STOP latches the paused ranges; OK again: RUN, no latch",
          env_key(BTN_OK) == SCR_DRAW_FULL && !ss->running && env_log_is("latch;") &&
          (env_log_clear(), env_key(BTN_OK) == SCR_DRAW_FULL) && ss->running && env_log_is("") && g_popup[0] == '\0');
    /* `set view.run off` = the OK key: state and owner calls */
    boot();
    env_key(BTN_OK);
    const scope_state_t by_key = *ss; char log_key[64]; snprintf(log_key, sizeof log_key, "%s", env_log);
    boot();
    CHECK("set view.run off (the direct route) = the OK key",
          prop_set_local(P(VIEW_RUN), 0, ORIGIN_SHELL, &n) == PS_OK && memcmp(ss, &by_key, sizeof by_key) == 0 &&
          env_log_is(log_key));
    /* SINGLE */
    boot();
    ss->trigger.mode = TRIG_SINGLE; ss->running = false;
    CHECK("SINGLE, stopped: OK re-arms, 'Await Trigger...'",
          env_key(BTN_OK) == SCR_DRAW_FULL && ss->running && env_log_is("rearm;") && strcmp(g_popup, "Await Trigger...") == 0);
    env_log_clear(); g_popup[0] = '\0'; t_single_done = false;
    CHECK("SINGLE, armed and waiting: OK stops (and latches)",
          env_key(BTN_OK) == SCR_DRAW_FULL && !ss->running && env_log_is("latch;") && g_popup[0] == '\0');
    ss->running = true; t_single_done = true; env_log_clear();
    CHECK("SINGLE, record taken: OK re-arms and stays RUN",
          env_key(BTN_OK) == SCR_DRAW_FULL && ss->running && env_log_is("rearm;") && strcmp(g_popup, "Await Trigger...") == 0);
    env_log_clear();
    CHECK("... and so does set view.run on",
          prop_set_local(P(VIEW_RUN), 1, ORIGIN_SHELL, &n) == PS_OK && env_log_is("rearm;") && strcmp(n.text, "Await Trigger...") == 0);
    /* a held mask failure */
    boot();
    t_mask_state = MASK_PF_READY; t_mask_hold = true;
    CHECK("a mask failure held: OK resumes the test, RUN/STOP untouched",
          env_key(BTN_OK) == SCR_DRAW_FULL && ss->running && !t_mask_hold &&
          env_log_is("mask_post(5,0,0);") && strcmp(g_popup, "MASK: RESUME") == 0);
}

/* A key and the direct route's set of the same value: one path. */
static void test_key_equals_set(void)
{
    printf("a key = the direct route's set (random states)\n");
    static const struct { uint8_t focus, key; } K[] = {
        { KF_ZOOM, BTN_UP }, { KF_ZOOM, BTN_DOWN }, { KF_ZOOM, BTN_LEFT }, { KF_ZOOM, BTN_RIGHT },
        { KF_CHMOVE, BTN_UP }, { KF_CHMOVE, BTN_DOWN }, { KF_CHMOVE, BTN_LEFT }, { KF_CHMOVE, BTN_RIGHT },
        { KF_TRIGMOVE, BTN_UP }, { KF_TRIGMOVE, BTN_DOWN }, { KF_ZOOM, BTN_OK },
        { KF_ZOOM, BTN_CH1 }, { KF_ZOOM, BTN_CH2 }, { KF_ZOOM, LONG(BTN_CH1) }, { KF_ZOOM, LONG(BTN_CH2) },
        { KF_ZOOM, LONG(BTN_PRM) }, { KF_CURSOR, BTN_UP }, { KF_CURSOR, BTN_DOWN }, { KF_CURSOR, BTN_RIGHT },
    };
    unsigned bad = 0, cases = 0, changed = 0;
    for (unsigned it = 0; it < 3000; it++) {
        st_t s;
        st_random(&s);
        s.view = SCOPE_VIEW_TIME; s.mask_state = MASK_PF_EMPTY; s.mask_hold = false; s.busy_req = false;
        if (s.ss.cursor.mode == CURSOR_OFF) s.ss.cursor.mode = CURSOR_BOTH;
        for (unsigned k = 0; k < sizeof K / sizeof K[0]; k++) {
            s.focus = K[k].focus;
            /* the key */
            st_load(&s);
            const scope_state_t before = *scope_state_get();
            (void)env_key(K[k].key);
            const scope_state_t by_key = *scope_state_get();
            char log_key[256];
            snprintf(log_key, sizeof log_key, "%s", env_log);
            /* which properties moved, and to what */
            prop_pair_t pairs[4]; unsigned n = 0;
            for (uint16_t i = 0; i < prop_count() && n < 4; i++) {
                const prop_desc_t *p = prop_at(i);
                if (!(p->inst & INST_SCOPE) || !prop_is_settable(p) || p->group == PG_MASK) continue;
                const int32_t v = prop_get(p);
                *scope_state_get() = before;
                const int32_t v0 = prop_get(p);
                *scope_state_get() = by_key;
                if (v != v0) { pairs[n].id = p->id; pairs[n].value = v; n++; }
            }
            /* the same values on the direct route, in table order except
             * that the roll switch goes last (the ladder's order) */
            st_load(&s);
            for (unsigned pass = 0; pass < 2; pass++)
                for (unsigned i = 0; i < n; i++)
                    if ((pairs[i].id == PROP_ID_TB_ROLL) == (pass == 1))
                        (void)prop_set_local(prop_by_id(pairs[i].id), pairs[i].value, ORIGIN_SHELL, NULL);
            /* live requests are the display's, not an owner call */
            char *lv;
            while ((lv = strstr(log_key, "live;")) != NULL) memmove(lv, lv + 5, strlen(lv + 5) + 1);
            cases++;
            if (n) changed++;
            const bool same_state = memcmp(scope_state_get(), &by_key, sizeof by_key) == 0;
            /* at the end of a range the key re-applies the value in force;
             * a set of nothing makes no call: compare only real changes */
            if (!same_state || (n && strcmp(env_log, log_key) != 0)) {
                if (bad++ < 8) printf("    key %u focus %u: state %d, set log [%s] key log [%s]\n",
                                      K[k].key, K[k].focus, same_state, env_log, log_key);
            }
        }
    }
    printf("    %u cases, %u changed a property\n", cases, changed);
    CHECK("19 keys x 3000 states: `set` of what the key changed gives the same state and owner calls",
          bad == 0 && changed > cases / 2);
}

/* The mask stage. */
static void test_mask_keys(void)
{
    printf("the mask stage\n");
    boot();
    t_mask_state = MASK_PF_READY; scope_key_focus = KF_MASK;
    CHECK("UP: mask.tolv +2 as one request with tolh, popup = what the mask reports",
          env_key(BTN_UP) == SCR_DRAW_FULL && env_log_is("mask_post(2,10,2);") && t_tolv == 10 &&
          strcmp(g_popup, "Mask V10 H2") == 0);
    env_log_clear();
    CHECK("RIGHT: mask.tolh +1", env_key(BTN_RIGHT) == SCR_DRAW_FULL && env_log_is("mask_post(2,10,3);") &&
          strcmp(g_popup, "Mask V10 H3") == 0);
    /* acquisition asleep: two UPs build on the request in flight (mask_bench H) */
    t_mask_slow = true; env_log_clear();
    env_key(BTN_UP);
    const bool first = strcmp(g_popup, "Mask V12 H3 ...") == 0;
    env_key(BTN_UP);
    CHECK("two UPs while the acquisition task sleeps: 12 then 14, '...' while in flight",
          first && strcmp(g_popup, "Mask V14 H3 ...") == 0 && env_log_is("mask_post(2,12,3);mask_post(2,14,3);") &&
          (mask_poll(), t_tolv == 14 && t_tolh == 3));
    t_mask_slow = false; env_log_clear();
    CHECK("MOVE in the mask stage: mask.stopfail toggles",
          env_key(BTN_MOVE) == SCR_DRAW_FULL && !t_sf && strcmp(g_popup, "Mask stop-on-fail OFF") == 0 &&
          env_log_is("mask_post(6,0,0);"));
    t_mask_slow = true;
    CHECK("... and says NOT SET while unacknowledged", env_key(BTN_MOVE) == SCR_DRAW_FULL &&
          strcmp(g_popup, "Mask stop NOT SET") == 0);
    mask_poll(); t_mask_slow = false; env_log_clear();
    CHECK("AUTO with a mask: the mask's procedure (clear), the stage goes back to zoom, no autoset",
          env_key(BTN_AUTO) == SCR_DRAW_FULL && env_log_is("mask_auto;") && strcmp(g_popup, "MASK: off") == 0 &&
          scope_key_focus == KF_ZOOM && !scope_mask_focus);
    env_log_clear();
    CHECK("AUTO with no mask: autoset", env_key(BTN_AUTO) == SCR_HANDLED && env_log_is("autoset;"));
}

/* The arrow stage: UI state the screen owns. */
static void test_focus(void)
{
    printf("the arrow stage\n");
    boot();
    env_key(BTN_SELECT);
    CHECK("SELECT: zoom -> channel move; the renderer's flags follow; ui.focus reports it",
          scope_key_focus == KF_CHMOVE && scope_hpos_focus && !scope_trig_level_focus &&
          G(P(UI_FOCUS)) == KF_CHMOVE && key_focus_get() == KF_CHMOVE);
    env_key(BTN_SELECT);
    CHECK("SELECT: -> trigger move", scope_key_focus == KF_TRIGMOVE && scope_trig_level_focus && scope_hpos_focus);
    env_key(BTN_SELECT);
    CHECK("SELECT: -> zoom (no cursors, no mask)", scope_key_focus == KF_ZOOM && !scope_hpos_focus);
    /* cursors switched on by a key on the page: the stage follows */
    osc_menu_env_cursors(true);
    CHECK("cursors switched on from the page: the cursor stage", scope_key_focus == KF_CURSOR);
    /* a `set view.cursor` leaves the stage; the next key drops a dead one */
    boot();
    (void)prop_set_local(P(VIEW_CURSOR), CURSOR_VERTICAL, ORIGIN_SHELL, NULL);
    CHECK("set view.cursor time does not move the stage", scope_key_focus == KF_ZOOM);
    scope_key_focus = KF_CURSOR;
    (void)prop_set_local(P(VIEW_CURSOR), CURSOR_OFF, ORIGIN_SHELL, NULL);
    CHECK("set view.cursor off leaves it until a key", scope_key_focus == KF_CURSOR);
    env_log_clear();
    CHECK("the next key acts in the zoom stage (the dead stage is dropped first)",
          env_key(BTN_UP) == SCR_DRAW_FULL && scope_key_focus == KF_ZOOM && env_log_is("vdiv(0,4);"));
    const key_ctx_t c = scope_screen_ctx();
    CHECK("scope_screen_ctx: the scope's context as it stands",
          c.mode == MODE_OSCILLOSCOPE && c.scope_time_view && !c.cursors_on && !c.mask_present &&
          c.focus == KF_ZOOM && c.active_channel == 0 && !c.osc_menu_open);
}

/* The page over the scope: its keys are the page's; the scope's long keys
 * do nothing under it. */
static void test_under_page(void)
{
    printf("under the Oscilloscope Settings page\n");
    boot();
    env_key(BTN_MENU);
    (void)screen_push(SCR_OSC_MENU);
    env_log_clear(); g_popup[0] = '\0';
    const int32_t v = G(P(CH1_VDIV));
    env_key(BTN_MOVE); env_key(BTN_SELECT); env_key(BTN_AUTO); env_key(LONG(BTN_OK)); env_key(LONG(BTN_CH1));
    CHECK("page up: MOVE / SELECT / AUTO / long OK / long CH1 change nothing of the scope's",
          env_log_is("") && g_popup[0] == '\0' && G(P(CH1_VDIV)) == v && scope_key_focus == KF_ZOOM &&
          G(P(CH1_ENABLED)) == 1);
}

/* What the display task reads: layout vs soft. */
static void test_epochs(void)
{
    printf("the two epochs\n");
    static const struct { const char *name; const prop_desc_t *p; int32_t v; bool layout, soft; } F[] = {
        { "ch1.vdiv", P(CH1_VDIV), 5, false, true },     { "ch2.vdiv", P(CH2_VDIV), 2, false, true },
        { "ch1.pos", P(CH1_POS), 20, false, true },      { "ch2.pos", P(CH2_POS), -15, false, true },
        { "ch1.coupling", P(CH1_COUPLING), 1, false, true }, { "ch1.probe", P(CH1_PROBE), 1, false, true },
        { "ch1.bwlimit", P(CH1_BWLIMIT), 1, false, true },
        { "tb.code", P(TB_CODE), 7, false, true },
        { "trig.level", P(TRIG_LEVEL), 25, false, true }, { "trig.mode", P(TRIG_MODE), 1, false, true },
        { "trig.edge", P(TRIG_EDGE), 1, false, true },   { "trig.source", P(TRIG_SOURCE), 1, false, true },
        { "trig.x", P(TRIG_X), 100, false, true },
        { "ch2.enabled", P(CH2_ENABLED), 0, true, false }, { "view.cursor", P(VIEW_CURSOR), 1, true, false },
        { "view.meashide", P(VIEW_MEASHIDE), 1, true, false }, { "ch1.meas", P(CH1_MEAS), 0x3, true, false },
        { "view.math", P(VIEW_MATH), 2, true, false },   { "view.persist", P(VIEW_PERSIST), 2, true, false },
        { "view.run", P(VIEW_RUN), 0, true, false },     { "tb.roll", P(TB_ROLL), 1, true, false },
        { "view.mode", P(VIEW_MODE), 3, true, false },
        { "view.curv1", P(VIEW_CURV1), 10, false, false }, { "view.curh2", P(VIEW_CURH2), 200, false, false },
    };
    bool ok = true;
    for (unsigned i = 0; i < sizeof F / sizeof F[0]; i++) {
        boot();
        uint32_t l0, s0, l1, s1;
        scope_screen_epochs(&l0, &s0);
        const uint32_t c0 = scope_screen_cursor_epoch(), e0 = scope_screen.epoch();
        (void)prop_set_local(F[i].p, F[i].v, ORIGIN_SHELL, NULL);
        scope_screen_epochs(&l1, &s1);
        const bool is_cur = !F[i].layout && !F[i].soft;
        const bool good = (l0 != l1) == F[i].layout && (s0 != s1) == F[i].soft &&
                          (c0 != scope_screen_cursor_epoch()) == is_cur && scope_screen.epoch() != e0;
        if (!good) printf("    %s: layout %d soft %d\n", F[i].name, l0 != l1, s0 != s1);
        ok &= good;
    }
    CHECK("each scope property is layout (one plot repaint), soft (one live frame) or a cursor line", ok);
    boot();
    uint32_t l0, s0, l1, s1;
    scope_screen_epochs(&l0, &s0);
    active_channel = 1;
    scope_screen_epochs(&l1, &s1);
    CHECK("the active channel is soft (the markers and the bar follow)", l0 == l1 && s0 != s1);
    g_fft_epoch = 99;
    scope_screen_epochs(&l1, &s0);
    CHECK("the live FFT configuration is layout (the spectrum views redraw their titles)", l0 != l1);
    boot();
    scope_screen_epochs(&l0, &s0);
    const uint32_t e0 = scope_screen.epoch();
    scope_screen_epochs(&l1, &s1);
    CHECK("nothing changed: nothing moves", l0 == l1 && s0 == s1 && scope_screen.epoch() == e0);
}

/* What the display task does about a change made on the direct route
 * (main.c scope_view_tick asks scope_screen_changed). */
static void test_direct_route_paint(void)
{
    printf("a set while the scope is shown: what is repainted\n");
    enum { LIVE, LAYOUT, WHOLE, CURSOR };
    static const struct { const char *name; const prop_desc_t *p; int32_t v; int kind; } F[] = {
        { "ch1.vdiv", P(CH1_VDIV), 5, LIVE },         { "ch2.vdiv", P(CH2_VDIV), 2, LIVE },
        { "ch1.pos", P(CH1_POS), 20, LIVE },          { "ch1.coupling", P(CH1_COUPLING), 1, LIVE },
        { "ch1.probe", P(CH1_PROBE), 1, LIVE },       { "tb.code", P(TB_CODE), 7, LIVE },
        { "trig.level", P(TRIG_LEVEL), 25, LIVE },    { "trig.mode", P(TRIG_MODE), 1, LIVE },
        { "trig.edge", P(TRIG_EDGE), 1, LIVE },       { "trig.source", P(TRIG_SOURCE), 1, LIVE },
        { "trig.x", P(TRIG_X), 100, LIVE },
        { "ch2.enabled", P(CH2_ENABLED), 0, LAYOUT }, { "view.cursor", P(VIEW_CURSOR), 1, LAYOUT },
        { "view.meashide", P(VIEW_MEASHIDE), 1, LAYOUT }, { "ch1.meas", P(CH1_MEAS), 0x3, LAYOUT },
        { "view.math", P(VIEW_MATH), 2, LAYOUT },     { "view.persist", P(VIEW_PERSIST), 2, LAYOUT },
        { "view.run", P(VIEW_RUN), 0, LAYOUT },
        { "tb.roll", P(TB_ROLL), 1, WHOLE },          { "view.mode fft", P(VIEW_MODE), 3, WHOLE },
        { "view.mode xy", P(VIEW_MODE), 2, WHOLE },
        { "view.curv1", P(VIEW_CURV1), 10, CURSOR },  { "view.curh2", P(VIEW_CURH2), 200, CURSOR },
    };
    bool ok = true;
    for (unsigned i = 0; i < sizeof F / sizeof F[0]; i++)
        for (int comp = 0; comp < 2; comp++) {
            boot();
            scope_shown_t seen, now;
            scope_screen_shown(&seen);
            (void)prop_set_local(F[i].p, F[i].v, ORIGIN_SHELL, NULL);
            scope_screen_shown(&now);
            const scope_change_t c = scope_screen_changed(&seen, &now, true, comp != 0);
            bool good;
            switch (F[i].kind) {
            case LIVE:   /* one live frame + the strip's cells with the compositor; the view in full without */
                good = c.bar && !c.whole && !c.layout && c.live == (comp != 0) && c.strip == (comp != 0) &&
                       c.force == (comp == 0);
                break;
            case LAYOUT: /* the gate repaints the plot area (its epoch is the layout part) */
                good = c.bar && !c.whole && c.layout && !c.live && !c.strip && !c.force;
                break;
            case WHOLE:  /* another kind of view: the whole screen */
                good = c.whole;
                break;
            default:     /* a cursor line: one frame, nothing else, the bar untouched */
                good = !c.bar && !c.whole && !c.layout && c.live && !c.strip && !c.force;
                break;
            }
            if (!good) printf("    %s comp %d: bar %d whole %d layout %d live %d strip %d force %d\n", F[i].name, comp,
                              c.bar, c.whole, c.layout, c.live, c.strip, c.force);
            ok &= good;
            /* not painted since the scope was entered: only the bar is known to differ */
            const scope_change_t u = scope_screen_changed(&seen, &now, false, comp != 0);
            ok &= !u.whole && !u.layout && !u.live && !u.strip && !u.force;
        }
    CHECK("23 properties x compositor on/off: live frame, plot repaint, whole screen or a cursor frame", ok);

    /* a soft change in another view: that view in full (never a live frame of the Y-T band) */
    boot();
    (void)prop_set_local(P(VIEW_MODE), 3, ORIGIN_SHELL, NULL);
    scope_shown_t seen, now;
    scope_screen_shown(&seen);
    (void)prop_set_local(P(CH1_VDIV), 6, ORIGIN_SHELL, NULL);
    scope_screen_shown(&now);
    scope_change_t c = scope_screen_changed(&seen, &now, true, true);
    CHECK("FFT view: a range change repaints the view in full", c.force && !c.live && !c.whole);
    (void)prop_set_local(P(VIEW_MODE), 0, ORIGIN_SHELL, NULL);
    (void)prop_set_local(P(TB_ROLL), 1, ORIGIN_SHELL, NULL);
    scope_screen_shown(&seen);
    (void)prop_set_local(P(CH1_VDIV), 7, ORIGIN_SHELL, NULL);
    scope_screen_shown(&now);
    c = scope_screen_changed(&seen, &now, true, true);
    CHECK("roll: a range change repaints the view in full", c.force && !c.live && !c.whole);
    scope_screen_shown(&seen);
    c = scope_screen_changed(&seen, &seen, true, true);
    CHECK("nothing changed: nothing to do", !c.bar && !c.whole && !c.layout && !c.live && !c.strip && !c.force);

    /* which sets the screen paints by itself (model_hw_fw.c port_changed) */
    CHECK("scope_screen_paints: the scope's own groups, not the bars' others, not nothing",
          scope_screen_paints((uint16_t)(1u << PG_CH1)) &&
          scope_screen_paints((uint16_t)((1u << PG_TB) | (1u << PG_VIEW) | (1u << PG_TRIG) | (1u << PG_MASK))) &&
          !scope_screen_paints((uint16_t)((1u << PG_CH1) | (1u << PG_SYS))) &&
          !scope_screen_paints((uint16_t)(1u << PG_GEN)) && !scope_screen_paints((uint16_t)(1u << PG_UI)) &&
          !scope_screen_paints(0));
}

int main(void)
{
    test_screen_entry();
    test_oracle();
    test_key_table();
    test_limits_and_failures();
    test_run_stop();
    test_key_equals_set();
    test_mask_keys();
    test_focus();
    test_under_page();
    test_epochs();
    test_direct_route_paint();
    printf("\n%d/%d passed\n", passed, run);
    return passed == run ? 0 : 1;
}
