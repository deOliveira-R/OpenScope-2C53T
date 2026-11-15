/*
 * OpenScope 2C53T - Oscilloscope Runtime State
 */

#include "scope_state.h"
#include "scope_measure.h"
#include "scope_roll.h"
#include "scope_geom.h"

/* ═══════════════════════════════════════════════════════════════════
 * Lookup tables
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * REMOVED 2026-08-18 — the nominal vdiv_table ("5mV", "10mV" ... "5V").
 *
 * Nothing ever derived those labels from a measurement, and the 2026-08-17
 * bench sweep showed they were 2.7x-3.5x out on the three ranges we have
 * cross-validated: the range labelled "2V" ruled a 2.8 V division. They are
 * deleted rather than left unused because an unused table of plausible
 * numbers is an invitation to wire it back up.
 *
 * Volts/div now comes from scope_cal.c, per channel as well as per range,
 * derived as (measured mV/count) x (the renderer's counts per division).
 * VDIV_COUNT survives as the range-index count and is still the bound on
 * ch->vdiv_idx.
 */

/*
 * REMOVED 2026-08-18 — the nominal timebase_table ("5ns" ... "20ms").
 *
 * Unlike vdiv_table, this one was NOT invented: on every code we have
 * measured its labels are a CONSTANT 2.12-2.15x away from what our grid
 * means, and at 16 samples per division they land within 6-8%. That is the
 * signature of a real table belonging to a design that draws 16 samples per
 * division, where ours draws 32. So it was right about the hardware and wrong
 * about this renderer.
 *
 * It is removed rather than kept because a label that does not match the grid
 * in front of it is the same defect either way, and an unused table of
 * plausible numbers is an invitation to wire it back up. Time/div now comes
 * from scope_timebase.c, derived as (samples per division) / (measured sample
 * rate). TIMEBASE_COUNT survives as the code count and the bound on
 * timebase_idx.
 */

const char *coupling_labels[COUPLING_COUNT] = { "DC", "AC", "GND" };
const char *probe_labels[PROBE_COUNT] = { "1X", "10X" };
const char *trigger_mode_labels[TRIG_COUNT] = { "Auto", "Normal", "Single" };
const char *trigger_edge_labels[TRIG_EDGE_COUNT] = { "Rising", "Falling" };
const char *trigger_source_labels[TRIG_SRC_COUNT] = { "CH1", "CH2" };

/* ═══════════════════════════════════════════════════════════════════
 * Global singleton
 * ═══════════════════════════════════════════════════════════════════ */

static scope_state_t g_scope;

void scope_state_init(scope_state_t *s)
{
    /* CH1: enabled, 2V/div, DC coupling, 1X probe */
    s->ch1.enabled   = true;
    s->ch1.vdiv_idx  = 8;     /* 2V */
    s->ch1.coupling  = COUPLING_DC;
    s->ch1.probe     = PROBE_1X;
    s->ch1.bw_limit  = false;
    s->ch1.position  = 0;

    /* CH2: enabled, 200mV/div, DC coupling, 1X probe */
    s->ch2.enabled   = true;
    s->ch2.vdiv_idx  = 5;     /* 200mV */
    s->ch2.coupling  = COUPLING_DC;
    s->ch2.probe     = PROBE_1X;
    s->ch2.bw_limit  = false;
    s->ch2.position  = 0;

    /* Trigger: auto, rising edge, CH1, center level */
    s->trigger.mode   = TRIG_AUTO;
    s->trigger.edge   = TRIG_RISING;
    s->trigger.source = TRIG_SRC_CH1;
    s->trigger.level  = 0;

    /* Timebase: 50us/div */
    s->timebase_idx = 12;     /* 50us */

    /* Running */
    s->running = true;

    /* Graticule: true scale by default (see scope_state.h). */
    s->true_scale = true;
    s->soft_trigger = true;
    s->trig_x = SCOPE_GEOM_CENTER_COL;   /* stock hpos 0 (scope_geom.h) */
    s->prm_mask = SCOPE_PRM_DEFAULT_MASK;
    s->prm_hidden = false;
    s->roll = false;          /* boot is Y-T; roll is not persisted */
    s->roll_step = 0;

    /* Cursor defaults */
    s->cursor.mode   = CURSOR_OFF;
    s->cursor.active = CURSOR_SEL_V1;
    s->cursor.v1_x = CURSOR_SCOPE_LEFT + CURSOR_SCOPE_WIDTH / 3;
    s->cursor.v2_x = CURSOR_SCOPE_LEFT + (CURSOR_SCOPE_WIDTH * 2) / 3;
    s->cursor.h1_y = CURSOR_SCOPE_TOP + CURSOR_SCOPE_HEIGHT / 3;
    s->cursor.h2_y = CURSOR_SCOPE_TOP + (CURSOR_SCOPE_HEIGHT * 2) / 3;
    /*
     * No cursor scale factors are initialised here any more, and that is the
     * point — see cursor_state_t. The two that used to live here (10 ms
     * across the screen, 8 V down it) were never updated by anything, so the
     * cursor readout was a function of the cursor positions alone. Seconds
     * now come from scope_timebase.c and volts from scope_cal.c, read at
     * draw time by scope_cursor.c, which refuses to print a unit when
     * neither table has an entry.
     */
}

void scope_trigger_summary(const scope_state_t *s, char *buf, unsigned n)
{
    static const char *const mode_abbr[TRIG_COUNT] = { "Auto", "Norm", "Sngl" };
    const char *m = ((unsigned)s->trigger.mode < (unsigned)TRIG_COUNT)
                        ? mode_abbr[s->trigger.mode] : "?";
    const char e = (s->trigger.edge == TRIG_FALLING) ? 'F' : 'R';
    const char c = (s->trigger.source == TRIG_SRC_CH2) ? '2' : '1';
    if (n == 0) return;
    /* No snprintf: this file is also built on the host and in tests with
     * nothing but the header. */
    unsigned i = 0;
    for (; *m && i + 1 < n; m++) buf[i++] = *m;
    const char tail[5] = { ' ', e, ' ', 'C', c };
    for (unsigned k = 0; k < 5 && i + 1 < n; k++) buf[i++] = tail[k];
    buf[i] = '\0';
}

scope_state_t *scope_state_get(void)
{
    return &g_scope;
}

/* The bounds-checked mutators that lived here (scope_adjust_vdiv /
 * _timebase / _trigger_level / _position, scope_cycle_*, scope_toggle_*,
 * scope_cursor_cycle_mode / _next_sel / _move) went with their last callers
 * at ARCH-1 B7: every change of scope state is a property set
 * (model/props_scope.c: range, step and wrap are the table's). The host
 * tests keep them as the pre-B7 reference (tests/scope_state_pre_b7.h). */

static bool g_scope_acquisition_ready = false;

bool scope_acquisition_ready(void)
{
    return g_scope_acquisition_ready;
}

void scope_mark_acquisition_ready(void)
{
    g_scope_acquisition_ready = true;
}
