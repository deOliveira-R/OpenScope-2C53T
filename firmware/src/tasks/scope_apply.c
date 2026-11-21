/*
 * OpenScope 2C53T - the hardware side of a scope setting change (scope_apply.h)
 *
 * Hoisted at the UI round 1 merge from input_handler.c (channel_cycle_coupling,
 * the Roll half of display_mode_step, scope_view_select, the Settings list's
 * persistence cycle) and osc_menu_ui.c, which were copies of each other.
 * Since ARCH-1 B5 the Oscilloscope Settings page changes settings only
 * through the properties (model/props_scope.c: the trigger rules are in
 * trig.* setters, and scope_apply_trigger_change went with the page's
 * trigger_change). Since B7 the plot's keys do too (ui/scope_screen.c), and
 * the coupling toggle and the Roll switch went with their callers
 * (chN.coupling, tb.roll): what is left here is what the property ops call
 * (model_hw_fw.c), the view with its pool and X-Y's bring-up, and
 * persistence.
 */
#include "scope_apply.h"
#include "scope_state.h"
#include "persistence.h"
#include "fpga.h"
#if FEATURE_FFT
#include "fft.h"
#include "model.h"           /* the fft.* properties */
#include "scope_vpos.h"
#include "scope_trigger.h"
#endif
#include <stdio.h>

#if FEATURE_FFT
/* X-Y: what its view needs before it is drawn. The phosphor trail lives in
 * the shared persistence buffer (X-Y and the time-domain persistence overlay
 * never run at once): claimed here with a short tail and cleared. Without
 * the calibration page the old bring-up of the offset outputs still runs;
 * with it, the page's offset codes (scope_vpos) are left alone. On the first
 * entry, and whenever the buffer is not there (another owner had the pool).
 * Until ARCH-1 B7 the X-Y renderer did this itself on its first frame, from
 * the display task; draw_xy_screen() now only draws ("no signal" without a
 * buffer). Input task, as every view change. */
static void xy_prepare(void)
{
    static bool done;
    if (done && persist_is_initialized()) return;
    if (!scope_vpos_active()) {
        scope_trigger_dac_init();
        scope_trigger_ch2_init();
    }
    if (!persist_is_initialized()) persist_init();
    persist_set_mode(PERSIST_LOW);             /* short phosphor tail */
    persist_clear();
    done = true;
}

bool scope_view_uses_fft(scope_view_t v)
{
    return v == SCOPE_VIEW_FFT || v == SCOPE_VIEW_SPLIT || v == SCOPE_VIEW_WATERFALL;
}

/* The fft.* properties (window, averaging, max hold) into the live FFT.
 * Nothing to do while no spectrum view holds its memory: the view takes
 * them when it starts. The averaging and max-hold history restart. */
void scope_apply_fft_settings(void)
{
    if (!fft_is_initialized()) return;
    fft_set_window((fft_window_t)prop_get(P(FFT_WINDOW)));
    fft_set_averaging(model_fft_avg_counts[prop_get(P(FFT_AVG))]);
    fft_set_max_hold(prop_get(P(FFT_MAXHOLD)) != 0);
}

void scope_view_select(scope_view_t v)
{
    if ((unsigned)v >= (unsigned)SCOPE_VIEW_COUNT || v == scope_view) return;

    if (!scope_view_uses_fft(v)) {
        scope_view = v;                 /* stop drawing a spectrum before the pool goes */
        fft_deinit();                   /* no-op if the FFT does not hold it */
        if (v == SCOPE_VIEW_XY) xy_prepare();
        return;
    }
    if (!fft_is_initialized()) {
        /* The pool is shared with persistence: hand it over cleanly instead
         * of leaving persistence drawing from a buffer the FFT now owns. */
        scope_apply_persist_off();
        fft_config_t c;
        c.window         = (fft_window_t)prop_get(P(FFT_WINDOW));
        c.sample_rate_hz = 44100.0f;
        c.ref_level_db   = 0.0f;
        c.db_range       = 80.0f;
        c.peak_count     = 4;
        c.avg_count      = model_fft_avg_counts[prop_get(P(FFT_AVG))];
        c.max_hold       = prop_get(P(FFT_MAXHOLD)) != 0;
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
#if FEATURE_FFT
    /* Persistence switched on and off again inside X-Y took the buffer its
     * trail is drawn in: X-Y gets it back. */
    if (scope_view == SCOPE_VIEW_XY) xy_prepare();
#endif
}

bool scope_apply_persist_on(persist_mode_t m)
{
    if (persist_enabled) {
        persist_set_mode(m);
        return true;
    }
#if FEATURE_FFT
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
