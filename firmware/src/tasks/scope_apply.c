/*
 * OpenScope 2C53T - the hardware side of a scope setting change (scope_apply.h)
 *
 * Hoisted at the UI round 1 merge from input_handler.c (trigger_after_change,
 * channel_cycle_coupling, the Roll half of display_mode_step,
 * scope_view_select, the Settings list's persistence cycle) and
 * osc_menu_ui.c (trigger_change, roll_set, view_set, persist_select), which
 * were copies of each other.
 */
#include "scope_apply.h"
#include "scope_state.h"
#include "trig_menu.h"
#include "persistence.h"
#include "fpga.h"
#ifdef FEATURE_FFT
#include "fft.h"
#endif
#include <stdio.h>

bool scope_apply_coupling_toggle(uint8_t ch, char *pb, size_t n)
{
    scope_state_t *ss = scope_state_get();
    channel_state_t *c = ch ? &ss->ch2 : &ss->ch1;
    const coupling_t next = (c->coupling == COUPLING_DC) ? COUPLING_AC : COUPLING_DC;
    const bool ok = fpga_apply_coupling(ch, (uint8_t)next);
    if (ok) c->coupling = next;
    snprintf(pb, n, ok ? "CH%u %s" : "CH%u %s NOT SET", (unsigned)ch + 1u, coupling_labels[next]);
    return ok;
}

bool scope_apply_trigger_change(uint8_t what, char *pb, size_t n)
{
    scope_state_t *ss = scope_state_get();
    if (what & TM_REFUSED_OFF) {
        snprintf(pb, n, "CH%d is off", ss->trigger.source == TRIG_SRC_CH2 ? 1 : 2);
        return true;
    }
    if (what & TM_CHANGED_SOURCE) {
        const bool ok = fpga_apply_trigger_source();
        snprintf(pb, n, ok ? "Trig source %s" : "Trig %s NOT SET",
                 trigger_source_labels[ss->trigger.source]);
        return !ok;
    }
    if (what & TM_CHANGED_MODE) {
        if (ss->trigger.mode == TRIG_SINGLE) {
            ss->running = true;
            fpga_acq_single_rearm();
            snprintf(pb, n, "Await Trigger...");
            return true;
        }
        snprintf(pb, n, "Trig: %s", trigger_mode_labels[ss->trigger.mode]);
        return false;
    }
    if (what & TM_CHANGED_EDGE) {
        snprintf(pb, n, "Edge: %s", trigger_edge_labels[ss->trigger.edge]);
        return false;
    }
    if (n) pb[0] = '\0';
    return false;
}

bool scope_apply_roll(bool want)
{
    scope_state_t *ss = scope_state_get();
    if (want == ss->roll) return true;
    ss->roll = want;
    if (!fpga_apply_roll()) {
        ss->roll = !want;   /* nothing was written: keep the old mode */
        return false;
    }
    return true;
}

#ifdef FEATURE_FFT
bool scope_view_uses_fft(scope_view_t v)
{
    return v == SCOPE_VIEW_FFT || v == SCOPE_VIEW_SPLIT || v == SCOPE_VIEW_WATERFALL;
}

void scope_view_select(scope_view_t v)
{
    if ((unsigned)v >= (unsigned)SCOPE_VIEW_COUNT || v == scope_view) return;

    if (!scope_view_uses_fft(v)) {
        scope_view = v;                 /* stop drawing a spectrum before the pool goes */
        fft_deinit();                   /* no-op if the FFT does not hold it */
        return;
    }
    if (!fft_is_initialized()) {
        /* The pool is shared with persistence: hand it over cleanly instead
         * of leaving persistence drawing from a buffer the FFT now owns. */
        scope_apply_persist_off();
        fft_config_t c;
        c.window         = FFT_WINDOW_HANNING;
        c.sample_rate_hz = 44100.0f;
        c.ref_level_db   = 0.0f;
        c.db_range       = 80.0f;
        c.peak_count     = 4;
        c.avg_count      = 0;
        c.max_hold       = false;
        c.zoom_start_bin = 1;
        c.zoom_end_bin   = FFT_BINS - 1;
        fft_init(&c);
    }
    scope_view = v;
}
#endif

void scope_apply_persist_off(void)
{
    if (!persist_enabled) return;
    persist_enabled = false;
    persist_deinit();
}

bool scope_apply_persist_on(persist_mode_t m)
{
    if (persist_enabled) {
        persist_set_mode(m);
        return true;
    }
#ifdef FEATURE_FFT
    if (scope_view_uses_fft(scope_view))
        scope_view_select(SCOPE_VIEW_TIME);   /* releases the FFT's pool */
    if (fft_is_initialized()) fft_deinit();
#endif
    persist_init();
    if (!persist_is_initialized()) return false;
    persist_set_mode(m);
    persist_enabled = true;
    return true;
}
