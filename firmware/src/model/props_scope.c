/*
 * OpenScope 2C53T - scope properties: ch1/ch2, tb, trig, view, il, mask
 *
 * The setters are the key paths of today (input_handler.c KA_*, osc_menu_ui.c
 * *_select / channel_change, scope_apply.c) with one rule changed: a value
 * is stored only once the owner said yes ("report the wire, not the
 * intent"). Where an owner reads the state itself (channel enables, roll,
 * the trigger source) the setter stores, asks, and puts the old value back
 * on a refusal, so the observable result is the same. Getters read
 * scope_state_t and the ui.h globals; hardware is model_hw() only.
 */
#define MODEL_INTERNAL
#include "model.h"
#include "scope_state.h"
#include "scope_cal.h"
#include "scope_timebase.h"
#include "scope_measure.h"
#include "scope_geom.h"
#include "scope_roll.h"
#include "meas_select.h"
#include "persistence.h"
#include "math_channel.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>

#define HW model_hw()

_Static_assert(VDIV_COUNT == 10 && TIMEBASE_COUNT == 21, "ch.vdiv 0..9, tb.code 0..20 (model_table.h)");
_Static_assert(SCOPE_ROLL_STEPS == 9u, "tb.rollstep 0..8");
_Static_assert(SCOPE_GEOM_TRIG_COL_MIN == 0 && SCOPE_GEOM_TRIG_COL_MAX == 300 &&
               SCOPE_GEOM_CENTER_COL == 150, "trig.x 0..300, default 150");
_Static_assert(SCOPE_POSITION_MAX == 100 && SCOPE_POSITION_STEP == 5, "chN.pos -100..100 step 5");
_Static_assert(SCOPE_PRM_ALL_MASK == 0x0FFFu, "chN.meas mask 0x0FFF");
_Static_assert(MEAS_SELECT_CH1_DEFAULT == 0x0507u && MEAS_SELECT_CH2_DEFAULT == 0x0507u,
               "chN.meas default (config_init_defaults)");
_Static_assert(CURSOR_OFF == 0 && CURSOR_VERTICAL == 1 && CURSOR_HORIZONTAL == 2 &&
               CURSOR_BOTH == 3, "view.cursor values = cursor_mode_t");
_Static_assert(PERSIST_OFF == 0 && PERSIST_INFINITE == 4, "view.persist values = persist_mode_t");
_Static_assert(MATH_COUNT == 8, "view.math = 0 off, 1 + math_op_t");

static channel_state_t *chan(const prop_desc_t *p)
{
    scope_state_t *ss = scope_state_get();
    return p->arg ? &ss->ch2 : &ss->ch1;
}

/* ═══ ch1 / ch2 ══════════════════════════════════════════════════════ */

int32_t ch_enabled_get(const prop_desc_t *p) { return chan(p)->enabled ? 1 : 0; }
void    ch_enabled_store(const prop_desc_t *p, int32_t v) { chan(p)->enabled = (v != 0); }

/* KA_CH_ENABLE_TOGGLE / osc menu OSC_CH_ENABLE: flip, route, put it back if
 * the routing refuses (both off); the arrows follow the live channel. */
prop_status_t ch_enabled_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin;
    channel_state_t *c = chan(p);
    const bool was = c->enabled, want = (v != 0);
    if (want == was) return PS_OK;
    c->enabled = want;
    if (HW->channel_enables && !HW->channel_enables()) {
        c->enabled = was;
        model_note(note, "Keep one CH on");
        return PS_REFUSED;
    }
    if (!want && active_channel == p->arg) active_channel = (uint8_t)(1u - p->arg);
    if (HW->interleave_active && HW->interleave_active()) {
        char t[24];
        snprintf(t, sizeof t, "CH%u alone: 250 MS/s", (unsigned)(2u - p->arg));
        model_note(note, t);
    }
    return PS_OK;
}

int32_t ch_vdiv_get(const prop_desc_t *p) { return chan(p)->vdiv_idx; }
void    ch_vdiv_store(const prop_desc_t *p, int32_t v) { chan(p)->vdiv_idx = (uint8_t)v; }

/* KA_VDIV_STEP stored first and kept the value on a failed relay apply;
 * here the relays go first and the state follows only on success. */
prop_status_t ch_vdiv_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin;
    if (HW->vdiv && !HW->vdiv(p->arg, (uint8_t)v)) {
        model_note(note, "relay apply failed");
        return PS_NOT_SET;
    }
    chan(p)->vdiv_idx = (uint8_t)v;
    return PS_OK;
}

bool vdiv_label(const prop_desc_t *p, int32_t v, char *out, size_t n)
{
    if (v < 0 || v >= VDIV_COUNT) return false;
    scope_cal_range_label((uint8_t)(p->arg + 1u), (uint8_t)v, out, (uint32_t)n);
    return out[0] != '\0';
}

int32_t ch_coupling_get(const prop_desc_t *p) { return (int32_t)chan(p)->coupling; }
void    ch_coupling_store(const prop_desc_t *p, int32_t v) { chan(p)->coupling = (coupling_t)v; }

/* scope_apply_coupling_toggle: the label changes only if the relay did.
 * GND (2) is outside the property's range: no ground path is known. */
prop_status_t ch_coupling_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin;
    if (HW->coupling && !HW->coupling(p->arg, (uint8_t)v)) {
        model_note(note, "relay readback failed");
        return PS_NOT_SET;
    }
    chan(p)->coupling = (coupling_t)v;
    return PS_OK;
}

int32_t ch_probe_get(const prop_desc_t *p) { return (int32_t)chan(p)->probe; }
void    ch_probe_store(const prop_desc_t *p, int32_t v) { chan(p)->probe = (probe_t)v; }
prop_status_t ch_probe_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin; (void)note;
    ch_probe_store(p, v);
    return PS_OK;
}

int32_t ch_bw_get(const prop_desc_t *p) { return chan(p)->bw_limit ? 1 : 0; }
void    ch_bw_store(const prop_desc_t *p, int32_t v) { chan(p)->bw_limit = (v != 0); }
prop_status_t ch_bw_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin; (void)note;
    ch_bw_store(p, v);
    return PS_OK;
}

int32_t ch_pos_get(const prop_desc_t *p) { return chan(p)->position; }
void    ch_pos_store(const prop_desc_t *p, int32_t v) { chan(p)->position = (int16_t)v; }

/* KA_CHPOS_STEP: the position is the ground marker's even when the offset
 * DAC cannot follow (no usable calibration page), so it is stored either
 * way, as today; the note says when only the marker moved. */
prop_status_t ch_pos_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin;
    chan(p)->position = (int16_t)v;
    if (HW->vpos && !HW->vpos(p->arg, (int16_t)v)) model_note(note, "marker only (no cal page)");
    return PS_OK;
}

int32_t ch_meas_get(const prop_desc_t *p) { return meas_select_get(p->arg); }
void    ch_meas_store(const prop_desc_t *p, int32_t v) { meas_select_set(p->arg, (uint16_t)v); }
prop_status_t ch_meas_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin; (void)note;
    ch_meas_store(p, v);
    return PS_OK;
}

bool meas_label(const prop_desc_t *p, int32_t bit, char *out, size_t n)
{
    (void)p;
    if (bit < 0 || bit >= SCOPE_PRM_COUNT || !n) return false;
    snprintf(out, n, "%s", scope_prm_label((unsigned)bit));
    return out[0] != '\0';
}

/* ═══ tb ═════════════════════════════════════════════════════════════ */

int32_t tb_code_get(const prop_desc_t *p) { (void)p; return scope_state_get()->timebase_idx; }
void    tb_code_store(const prop_desc_t *p, int32_t v) { (void)p; scope_state_get()->timebase_idx = (uint8_t)v; }

/* KA_TB_STEP's Y-T half. In roll the code is the one to return to: stored
 * without a write, as the key does (was_roll -> no apply). */
prop_status_t tb_code_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)p; (void)origin;
    scope_state_t *ss = scope_state_get();
    if (!ss->roll && HW->timebase && !HW->timebase((uint8_t)v)) {
        model_note(note, "timebase write failed");
        return PS_NOT_SET;
    }
    ss->timebase_idx = (uint8_t)v;
    return PS_OK;
}

bool tb_label(const prop_desc_t *p, int32_t v, char *out, size_t n)
{
    (void)p;
    if (v < 0 || v >= TIMEBASE_COUNT) return false;
    const bool il = HW->interleave_active && HW->interleave_active();
    scope_timebase_view_label((uint8_t)v, il, out, (uint32_t)n);
    return out[0] != '\0';
}

/* scope_apply_roll: Roll through the FPGA, the old mode kept on refusal. */
static prop_status_t roll_apply(bool want, uint8_t step, prop_note_t *note)
{
    scope_state_t *ss = scope_state_get();
    const bool was = ss->roll;
    const uint8_t was_step = ss->roll_step;
    ss->roll = want;
    ss->roll_step = step;
    if ((want || was) && HW->roll && !HW->roll()) {
        ss->roll = was;
        ss->roll_step = was_step;
        model_note(note, "Roll NOT SET");
        return PS_NOT_SET;
    }
    return PS_OK;
}

int32_t tb_roll_get(const prop_desc_t *p) { (void)p; return scope_state_get()->roll ? 1 : 0; }
void    tb_roll_store(const prop_desc_t *p, int32_t v) { (void)p; scope_state_get()->roll = (v != 0); }
prop_status_t tb_roll_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)p; (void)origin;
    const scope_state_t *ss = scope_state_get();
    if ((v != 0) == ss->roll) return PS_OK;
    return roll_apply(v != 0, ss->roll_step, note);
}

int32_t tb_rollstep_get(const prop_desc_t *p) { (void)p; return scope_state_get()->roll_step; }
void    tb_rollstep_store(const prop_desc_t *p, int32_t v) { (void)p; scope_state_get()->roll_step = (uint8_t)v; }
prop_status_t tb_rollstep_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)p; (void)origin;
    const scope_state_t *ss = scope_state_get();
    if (!ss->roll) { tb_rollstep_store(p, v); return PS_OK; }   /* used on the next roll */
    return roll_apply(true, (uint8_t)v, note);
}

/* ═══ trig ═══════════════════════════════════════════════════════════ */

int32_t trig_mode_get(const prop_desc_t *p) { (void)p; return (int32_t)scope_state_get()->trigger.mode; }
void    trig_mode_store(const prop_desc_t *p, int32_t v) { (void)p; scope_state_get()->trigger.mode = (trigger_mode_t)v; }

/* scope_apply_trigger_change, mode: selecting Single arms the one-shot. */
prop_status_t trig_mode_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin;
    scope_state_t *ss = scope_state_get();
    trig_mode_store(p, v);
    if (v == TRIG_SINGLE) {
        ss->running = true;
        if (HW->single_rearm) HW->single_rearm();
        model_note(note, "Await Trigger...");
    }
    return PS_OK;
}

int32_t trig_edge_get(const prop_desc_t *p) { (void)p; return (int32_t)scope_state_get()->trigger.edge; }
void    trig_edge_store(const prop_desc_t *p, int32_t v) { (void)p; scope_state_get()->trigger.edge = (trigger_edge_t)v; }
prop_status_t trig_edge_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin; (void)note;
    trig_edge_store(p, v);      /* the MCU edge filter and the soft trigger: no register */
    return PS_OK;
}

int32_t trig_source_get(const prop_desc_t *p) { (void)p; return (int32_t)scope_state_get()->trigger.source; }
void    trig_source_store(const prop_desc_t *p, int32_t v) { (void)p; scope_state_get()->trigger.source = (trigger_source_t)v; }

/* trig_menu_change: a channel that is off is refused; then reg 0x06. */
prop_status_t trig_source_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin;
    scope_state_t *ss = scope_state_get();
    const bool on = (v == TRIG_SRC_CH2) ? ss->ch2.enabled : ss->ch1.enabled;
    if (!on) {
        model_note(note, v == TRIG_SRC_CH2 ? "CH2 is off" : "CH1 is off");
        return PS_REFUSED;
    }
    const trigger_source_t was = ss->trigger.source;
    trig_source_store(p, v);
    if (HW->trigger_source && !HW->trigger_source()) {
        ss->trigger.source = was;
        model_note(note, "reg 0x06 NOT SET");
        return PS_NOT_SET;
    }
    return PS_OK;
}

int32_t trig_level_get(const prop_desc_t *p) { (void)p; return scope_state_get()->trigger.level; }
void    trig_level_store(const prop_desc_t *p, int32_t v) { (void)p; scope_state_get()->trigger.level = (int16_t)v; }

/* KA_TRIGLVL_STEP stored first; the one writer of reg 0x08 goes first here. */
prop_status_t trig_level_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin;
    if (HW->trigger_level && !HW->trigger_level((int16_t)v)) {
        model_note(note, "reg 0x08 NOT SET");
        return PS_NOT_SET;
    }
    trig_level_store(p, v);
    return PS_OK;
}

int32_t trig_x_get(const prop_desc_t *p) { (void)p; return scope_state_get()->trig_x; }
void    trig_x_store(const prop_desc_t *p, int32_t v) { (void)p; scope_state_get()->trig_x = (int16_t)v; }
prop_status_t trig_x_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin; (void)note;
    trig_x_store(p, v);
    return PS_OK;
}

int32_t trig_soft_get(const prop_desc_t *p) { (void)p; return scope_state_get()->soft_trigger ? 1 : 0; }
void    trig_soft_store(const prop_desc_t *p, int32_t v) { (void)p; scope_state_get()->soft_trigger = (v != 0); }
prop_status_t trig_soft_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin; (void)note;
    trig_soft_store(p, v);
    return PS_OK;
}

/* ═══ view ═══════════════════════════════════════════════════════════ */

/* view.mode: one composite of the scope view and Roll (what the Display
 * pane sets), values yt, roll, xy, fft, split, waterfall. */
enum { VM_YT, VM_ROLL, VM_XY, VM_FFT, VM_SPLIT, VM_WATERFALL, VM_COUNT };
const char *const lbl_view_mode[VM_COUNT] = { "yt", "roll", "xy", "fft", "split", "waterfall" };

int32_t view_mode_get(const prop_desc_t *p)
{
    (void)p;
#ifdef FEATURE_FFT
    switch (scope_view) {
    case SCOPE_VIEW_XY:        return VM_XY;
    case SCOPE_VIEW_FFT:       return VM_FFT;
    case SCOPE_VIEW_SPLIT:     return VM_SPLIT;
    case SCOPE_VIEW_WATERFALL: return VM_WATERFALL;
    default: break;
    }
#endif
    return scope_state_get()->roll ? VM_ROLL : VM_YT;
}

#ifdef FEATURE_FFT
/* view.mode -> scope_view_t (scope_view exists only in an FFT build). */
static const uint8_t k_view_of[VM_COUNT] = {
    SCOPE_VIEW_TIME, SCOPE_VIEW_TIME, SCOPE_VIEW_XY, SCOPE_VIEW_FFT, SCOPE_VIEW_SPLIT,
    SCOPE_VIEW_WATERFALL,
};
#define VIEW_OF(v) k_view_of[(v)]
#else
#define VIEW_OF(v) ((void)(v), 0u)     /* the time view: yt and roll only */
#endif

void view_mode_store(const prop_desc_t *p, int32_t v)
{
    (void)p;
#ifdef FEATURE_FFT
    scope_view = (scope_view_t)VIEW_OF(v);
#endif
    scope_state_get()->roll = (v == VM_ROLL);
}

/* display_select (+ the CH1 pane's FFT row for split): the view first
 * (scope_view_select claims or releases the pool), then Roll. Split needs
 * Roll off first, as channel_change does. */
prop_status_t view_mode_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)p; (void)origin;
#ifndef FEATURE_FFT
    if (v != VM_YT && v != VM_ROLL) { model_note(note, "Not in this build"); return PS_REFUSED; }
#endif
    if (v == VM_SPLIT && roll_apply(false, scope_state_get()->roll_step, note) != PS_OK)
        return PS_NOT_SET;
    if (HW->view_select && !HW->view_select(VIEW_OF(v))) {
        model_note(note, "Not in this build");
        return PS_REFUSED;
    }
#ifdef FEATURE_FFT
    if (!HW->view_select) scope_view = (scope_view_t)VIEW_OF(v);   /* no owner bound */
#endif
    const bool want_roll = (v == VM_ROLL);
    if (want_roll != scope_state_get()->roll &&
        roll_apply(want_roll, scope_state_get()->roll_step, note) != PS_OK)
        return PS_NOT_SET;
    return PS_OK;
}

const char *const lbl_view_persist[5] = { "off", "500ms", "1s", "2s", "inf" };

int32_t view_persist_get(const prop_desc_t *p)
{
    (void)p;
    if (!persist_enabled) return PERSIST_OFF;
    return HW->persist_mode ? HW->persist_mode() : PERSIST_OFF;
}
void view_persist_store(const prop_desc_t *p, int32_t v)
{
    (void)p; (void)v;
    /* Not restored at boot on purpose (settings_store.c): turning
     * persistence on claims the shared pool and evicts the FFT. A store
     * leaves it off; only the setter turns it on. */
}
/* persist_select: off releases the pool; on claims it (a spectrum view
 * goes back to Y-T first). */
prop_status_t view_persist_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)p; (void)origin;
    if (v == PERSIST_OFF) {
        if (HW->persist_off) HW->persist_off();
        return PS_OK;
    }
    if (!HW->persist_on || !HW->persist_on((uint8_t)v)) {
        model_note(note, "No memory for persistence");
        return PS_NOT_SET;
    }
    return PS_OK;
}

/* view.math: 0 = off, 1 + math_op_t. Off keeps math_op (the Math pane does). */
const char *const lbl_view_math[1 + MATH_COUNT] = {
    "off", "A+B", "A-B", "A*B", "A/B", "-A", "-B", "|A|", "|B|",
};
int32_t view_math_get(const prop_desc_t *p) { (void)p; return math_enabled ? 1 + (int32_t)math_op : 0; }
void view_math_store(const prop_desc_t *p, int32_t v)
{
    (void)p;
    if (v == 0) { math_enabled = false; return; }
    math_op = (uint8_t)(v - 1);
    math_enabled = true;
}
prop_status_t view_math_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin; (void)note;
    view_math_store(p, v);
    return PS_OK;
}

const char *const lbl_view_cursor[CURSOR_MODE_COUNT] = { "off", "time", "volt", "both" };
int32_t view_cursor_get(const prop_desc_t *p) { (void)p; return (int32_t)scope_state_get()->cursor.mode; }
/* cursor_select: the horizontal pair starts on H1, the others on V1. */
void view_cursor_store(const prop_desc_t *p, int32_t v)
{
    (void)p;
    cursor_state_t *c = &scope_state_get()->cursor;
    c->mode   = (cursor_mode_t)v;
    c->active = (c->mode == CURSOR_HORIZONTAL) ? CURSOR_SEL_H1 : CURSOR_SEL_V1;
}
prop_status_t view_cursor_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin; (void)note;
    view_cursor_store(p, v);
    return PS_OK;
}

int32_t view_ts_get(const prop_desc_t *p) { (void)p; return scope_state_get()->true_scale ? 1 : 0; }
void    view_ts_store(const prop_desc_t *p, int32_t v) { (void)p; scope_state_get()->true_scale = (v != 0); }
prop_status_t view_ts_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin; (void)note;
    view_ts_store(p, v);
    return PS_OK;
}

int32_t view_mh_get(const prop_desc_t *p) { (void)p; return scope_state_get()->prm_hidden ? 1 : 0; }
void    view_mh_store(const prop_desc_t *p, int32_t v) { (void)p; scope_state_get()->prm_hidden = (v != 0); }
prop_status_t view_mh_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin; (void)note;
    view_mh_store(p, v);
    return PS_OK;
}

int32_t view_run_get(const prop_desc_t *p) { (void)p; return scope_state_get()->running ? 1 : 0; }
void    view_run_store(const prop_desc_t *p, int32_t v) { (void)p; scope_state_get()->running = (v != 0); }
prop_status_t view_run_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin; (void)note;
    view_run_store(p, v);
    return PS_OK;
}

/* ═══ il (fpga.c keeps the state; typed at the ops boundary) ═════════ */

int32_t il_enabled_get(const prop_desc_t *p) { (void)p; return (HW->il_enabled && HW->il_enabled()) ? 1 : 0; }
void    il_enabled_store(const prop_desc_t *p, int32_t v) { (void)p; if (HW->il_enable) (void)HW->il_enable(v != 0); }
prop_status_t il_enabled_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)p; (void)origin;
    if (!HW->il_enable || !HW->il_enable(v != 0)) { model_note(note, "interleave NOT SET"); return PS_NOT_SET; }
    return PS_OK;
}

static void il_corr(gain_e6_t *g, int32_t *o)
{
    g->e6 = 1000000;
    *o = 0;
    if (HW->il_corr_get) HW->il_corr_get(g, o);
}

int32_t il_gain_get(const prop_desc_t *p)   { (void)p; gain_e6_t g; int32_t o; il_corr(&g, &o); return g.e6; }
int32_t il_offset_get(const prop_desc_t *p) { (void)p; gain_e6_t g; int32_t o; il_corr(&g, &o); return o; }

static prop_status_t il_corr_apply(gain_e6_t g, int32_t o, prop_note_t *note)
{
    if (!HW->il_corr_set || !HW->il_corr_set(g, o)) { model_note(note, "correction NOT SET"); return PS_NOT_SET; }
    return PS_OK;
}
void il_gain_store(const prop_desc_t *p, int32_t v)
{
    (void)p; gain_e6_t g; int32_t o; il_corr(&g, &o); g.e6 = v; (void)il_corr_apply(g, o, NULL);
}
void il_offset_store(const prop_desc_t *p, int32_t v)
{
    (void)p; gain_e6_t g; int32_t o; il_corr(&g, &o); (void)il_corr_apply(g, v, NULL);
}
prop_status_t il_gain_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)p; (void)origin; gain_e6_t g; int32_t o; il_corr(&g, &o); g.e6 = v;
    return il_corr_apply(g, o, note);
}
prop_status_t il_offset_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)p; (void)origin; gain_e6_t g; int32_t o; il_corr(&g, &o);
    return il_corr_apply(g, v, note);
}

int32_t il_skew_get(const prop_desc_t *p) { (void)p; return HW->il_skew_get ? HW->il_skew_get().ppm : 0; }
void il_skew_store(const prop_desc_t *p, int32_t v)
{
    (void)p; ppm_period_t s = { v }; if (HW->il_skew_set) (void)HW->il_skew_set(s);
}
prop_status_t il_skew_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)p; (void)origin;
    ppm_period_t s = { v };
    if (!HW->il_skew_set || !HW->il_skew_set(s)) { model_note(note, "skew NOT SET"); return PS_NOT_SET; }
    return PS_OK;
}

/* ═══ mask (the acquisition task applies posted requests) ════════════ */

static void mask_now(uint8_t *v, uint8_t *h, bool *sf)
{
    *v = 8u; *h = 2u; *sf = true;     /* scope_mask.c's initial values */
    if (HW->mask_get) HW->mask_get(v, h, sf);
}

int32_t mask_tolv_get(const prop_desc_t *p) { (void)p; uint8_t v, h; bool s; mask_now(&v, &h, &s); return v; }
int32_t mask_tolh_get(const prop_desc_t *p) { (void)p; uint8_t v, h; bool s; mask_now(&v, &h, &s); return h; }
int32_t mask_stopfail_get(const prop_desc_t *p) { (void)p; uint8_t v, h; bool s; mask_now(&v, &h, &s); return s; }

static prop_status_t mask_post_tol(int32_t tv, int32_t th, prop_note_t *note)
{
    if (!HW->mask_tol || !HW->mask_tol((uint8_t)tv, (uint8_t)th)) { model_note(note, "mask request NOT posted"); return PS_NOT_SET; }
    model_note(note, "posted");
    return PS_OK;
}
void mask_tolv_store(const prop_desc_t *p, int32_t x) { (void)p; (void)mask_post_tol(x, mask_tolh_get(p), NULL); }
void mask_tolh_store(const prop_desc_t *p, int32_t x) { (void)p; (void)mask_post_tol(mask_tolv_get(p), x, NULL); }
/* mask_adjust_tol: tolerances are kept for the next teach even with no mask. */
prop_status_t mask_tolv_set(const prop_desc_t *p, int32_t x, uint8_t origin, prop_note_t *note)
{
    (void)origin; return mask_post_tol(x, mask_tolh_get(p), note);
}
prop_status_t mask_tolh_set(const prop_desc_t *p, int32_t x, uint8_t origin, prop_note_t *note)
{
    (void)origin; return mask_post_tol(mask_tolv_get(p), x, note);
}
void mask_stopfail_store(const prop_desc_t *p, int32_t x) { (void)p; if (HW->mask_stopfail) (void)HW->mask_stopfail(x != 0); }
prop_status_t mask_stopfail_set(const prop_desc_t *p, int32_t x, uint8_t origin, prop_note_t *note)
{
    (void)p; (void)origin;
    if (!HW->mask_stopfail || !HW->mask_stopfail(x != 0)) { model_note(note, "mask request NOT posted"); return PS_NOT_SET; }
    model_note(note, "posted");
    return PS_OK;
}
