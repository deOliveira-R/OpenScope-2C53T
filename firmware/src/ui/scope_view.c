/*
 * OpenScope 2C53T - oscilloscope screen: the view of a record
 *
 * Which part of a record the plot shows and at what scale: held and merged
 * views, the autofit, the trigger anchor and the software trigger offset.
 */

#include "scope_ui_parts.h"

/* Sample rate of the record on screen, for everything measured on the record
 * itself (Freq, widths; the cursors use column_rate_for_display instead).
 * The published record's rate (fpga_record_sample_rate, doubled for the
 * two-converter merge) when the record is held or the hardware runs the code
 * the UI shows; the table rate of the selected code otherwise. */
float record_rate_for_display(const scope_state_t *ss)
{
    /* A held record keeps the rate it was captured at, whatever the code now. */
    if (!ss->running && !ss->roll && fpga_data_ready())
        return fpga_record_sample_rate();
    return (fpga_acq_rate_idx_get() == ss->timebase_idx)
         ? fpga_record_sample_rate()
         : scope_timebase_sample_rate(ss->timebase_idx);
}

/* A record is HELD (STOP, zoom while paused): no new record will be
 * published, so the screen re-samples the one it has. */
static inline bool view_held(const scope_state_t *ss)
{
    return !ss->running && !ss->roll && fpga_data_ready();
}

/* Is the view drawn as the two-converter merge? Running: what the published
 * record is (fpga_acq_record_interleaved). Held: what the code now on the
 * status bar would capture (fpga_interleave_active: merge enabled, channel
 * mask 1, code <= 0x03), because the held record keeps its own rate
 * (fpga_record_sample_rate) and is re-sampled to the column rate of the code
 * now selected. The display time per pixel depends on the code alone (as in
 * stock, 0x08030A70), so a held merged record viewed at 0x04 reads 200 ns/div
 * and is zoomed out 2x, not relabelled 100 ns. At 0x00-0x03 either flag gives
 * the same column rate (124.8 MS/s x 20 == 249.6 MS/s x 10), so the choice
 * matters only at 0x04 and above. The status bar label, the window plan
 * (view_plan), the persistence geometry key and the shell's timebase label
 * read this flag, so the label and the column rate cannot disagree. */
bool view_merged(const scope_state_t *ss)
{
    return view_held(ss) ? fpga_interleave_active() : fpga_acq_record_interleaved();
}

bool scope_ui_view_merged(void) { return view_merged(scope_state_get()); }

/* The display zoom for the code on the status bar
 * (scope_timebase_interp_factor): 1 everywhere but 0x00-0x03. The label
 * (status_bar.c) and the window plan (view_plan) derive from the same call. */
static uint8_t view_factor(const scope_state_t *ss)
{
    return scope_timebase_interp_factor(ss->timebase_idx, view_merged(ss));
}

/* ── The Y-T window's mapping ─────────────────────────────────────────
 * ONE window, yt_cols, built once per frame by view_build() and read by every
 * consumer (full redraw, live frame, trigger marker, math trace, split view;
 * the cursors read the column rate it implies, the mask overlay is gated on
 * it). Its horizontal mapping comes from one plan (pv_view_plan,
 * dsp/paused_view.c):
 *
 *   ratio R   record samples per column x 1000. Running: 1000 / L, L the
 *             fast-code zoom (10/5/2/1 merged, 20/10/4/2 per converter at
 *             0x00-0x03, 1 elsewhere). Held: a timebase change re-renders the
 *             held record without re-arming (as stock does, 0x08021178..
 *             0x08021348, docs/re/parity2_static.md s3), so R = 1000 fs_rec /
 *             fs_col, the held record's own rate (fpga_record_sample_rate)
 *             over the column rate of the code now selected.
 *   L > 0     R == 1000 / L with L dividing 20: the integer window
 *             (scope_geom.c), trigger fitted to 1/L sample, window clamped
 *             inside the record. Covers running, and a held record at (or
 *             between) fast codes of one record rate.
 *   L == 0    any other held ratio (zoom while paused): pv_fill, the trigger
 *             column stays put, nearest sample for R >= 1000, the same 16-tap
 *             sinc at any phase below it, columns outside the record blank:
 *             [g_view_first, g_view_end) is the span that shows the record,
 *             and every consumer stays inside it.
 *
 * Vertical, true scale only (autofit is range-blind by construction): a
 * record whose capture scale differs from the scale now selected (a held
 * record, or the running record still on the glass right after a range or
 * position change) is re-drawn as code' = zero_now + k (code - zero_cap),
 * k = volts/count at capture over volts/count now (pv_vscale; stock's map at
 * 0x080212BC), so it reads true at the range and position now selected. The
 * position enters through the two 0 V codes. Measurements of a held record
 * use the capture's volts/count and 0 V (rec_vpc / rec_zero).
 *
 * Rates: the cursors read the column rate (1000 fs_rec / R); the
 * measurements read the record's own (record_rate_for_display). */

/* The span of the last build that shows the record, [first, end): all 301
 * columns except on the general path. Zero-initialised on purpose (bss, not
 * .data: the warm-handoff builds have no RAM to spare); view_build() sets it
 * before any consumer reads it. */
g_span_t g_span;

/* What the record's codes mean: volts/count and the 0 V code of each channel
 * when it was captured. Latched by scope_ui_paused_latch() on every running
 * frame (yt_fits) and at STOP (the model's paused_latch hook). */
static struct {
    float   vpc[2];
    uint8_t zero[2];
    bool    valid;
} g_cap;


/* The plan for the current state: returns L (0 = general path), *ratio = R,
 * *fs_rec = the rate R was taken against (both may be NULL). Depends only on
 * the state and the published record, so the cursor shell command and the
 * readouts answer without a draw having run. */
static uint8_t view_plan(const scope_state_t *ss, uint32_t *ratio, float *fs_rec)
{
    const bool held = view_held(ss);
    const uint8_t L_code = view_factor(ss);
    float fr, fc = 0.0f;
    if (held) {
        fr = fpga_record_sample_rate();
        fc = scope_timebase_sample_rate(ss->timebase_idx) *
             (view_merged(ss) ? 2.0f : 1.0f) * (float)L_code;
    } else {
        fr = record_rate_for_display(ss);
    }
    const uint8_t L = pv_view_plan(held, fr, fc, L_code, ratio);
    if (fs_rec) *fs_rec = fr;
    return L;
}

/* What one COLUMN is worth for the cursors: 1000 fs_rec / R, which is the
 * record rate x L on the integer window and the selected code's column rate
 * on a re-sampled held record (up to the 100x clamp, PV_RATIO_MIN). Only the
 * column-domain readouts (cursor dt, 1/dt, X) use it; everything measured on
 * the record itself (Freq, the measurement parameters) keeps the record rate,
 * so a zoom can never leak into a measurement. */
void view_time_axis(const scope_state_t *ss, view_time_axis_t *ax)
{
    if (ss->roll) {
        /* One point per column, paced by the MCU's own tick: the period is
         * set, not looked up in a table that could be provisional. */
        const uint32_t ms = scope_roll_period_ms(ss->roll_step).ms;
        ax->rate = (sample_rate_t){ ms ? 1000.0f / (float)ms : 0.0f };
        ax->conf = SCOPE_CURSOR_RAW;
        ax->ref_col = (int32_t)SCOPE_GEOM_COLS - 1;
        return;
    }
    ax->rate = (sample_rate_t){ column_rate_for_display(ss) };
    ax->conf = scope_cursor_tb_conf((tb_code_t){ ss->timebase_idx });
    const int16_t ta = g_place.trig_x;
    ax->ref_col = (ta >= 0) ? (int32_t)ta : (int32_t)ss->trig_x;
}

float column_rate_for_display(const scope_state_t *ss)
{
    uint32_t r = PV_RATIO_ONE;
    float fs = 0.0f;
    (void)view_plan(ss, &r, &fs);
    return (r > 0u) ? fs * (float)PV_RATIO_ONE / (float)r : fs;
}

/* The published record's OWN volts/count and 0 V code (the configuration it
 * was captured with, dsp/acq_cfg.h); false before the first record that has
 * one. After a range or position change the record on the glass is still the
 * old one for at least a cycle (records captured across the change are
 * refused), so "the settings now" would mislabel it. */
static bool rec_own_scale(uint8_t ch, float *k, int *z)
{
    const acq_cfg_t *c = fpga_acq_record_cfg_ptr();
    if (c->mask == 0u || ch > 1u)
        return false;
    const float pf = (c->probe[ch] == (uint8_t)PROBE_10X) ? 10.0f : 1.0f;
    *k = scope_cal_mv_per_count((uint8_t)(ch + 1u), c->vdiv[ch]) / 1000.0f * pf;
    *z = scope_measure_zero_for(scope_vpos_active(), c->pos[ch]).v;
    return true;
}

/* Latch the capture tuple (every running frame, and the RUN/STOP key): the
 * published record's own scale, so a STOP right after a range change holds
 * the record with the range it was captured at. */
void scope_ui_paused_latch(void)
{
    const scope_state_t *ss = scope_state_get();
    for (uint8_t ch = 0; ch < 2u; ch++) {
        float k;
        int z;
        if (rec_own_scale(ch, &k, &z)) {
            g_cap.vpc[ch]  = k;
            g_cap.zero[ch] = (uint8_t)z;
        } else {
            g_cap.vpc[ch]  = scope_cal_volts_per_count((uint8_t)(ch + 1u),
                                                       ch ? ss->ch2.vdiv_idx : ss->ch1.vdiv_idx);
            g_cap.zero[ch] = (uint8_t)chan_zero_code(ss, ch);
        }
    }
    g_cap.valid = true;
}

/* Is the record held and the capture tuple latched? (view_held plus
 * g_cap.valid.) */
static bool rec_held_cap(const scope_state_t *ss)
{
    return g_cap.valid && !ss->running && !ss->roll && fpga_data_ready();
}

/* Volts per count (rec_vpc) and 0 V code (rec_zero) of channel ch's record:
 * the latched capture tuple for a held record (it does not change with the
 * range or position now selected); else the published record's own capture
 * configuration; else, before the first record that has one, the current
 * settings. */
float rec_vpc(const scope_state_t *ss, uint8_t ch)
{
    float k;
    int z;
    if (rec_held_cap(ss)) return g_cap.vpc[ch];
    if (rec_own_scale(ch, &k, &z)) return k;
    return scope_cal_volts_per_count((uint8_t)(ch + 1u), ch ? ss->ch2.vdiv_idx : ss->ch1.vdiv_idx);
}

uint8_t rec_zero(const scope_state_t *ss, uint8_t ch)
{
    float k;
    int z;
    if (rec_held_cap(ss)) return g_cap.zero[ch];
    if (rec_own_scale(ch, &k, &z)) return (uint8_t)z;
    return (uint8_t)chan_zero_code(ss, ch);
}

/* The lowest position (record codes, -100..0) of channel ch that keeps its
 * 0 V row at least half a division above the measurement strip. Only in true
 * scale with the calibration page (scope_vpos_active) and a calibrated range;
 * -100 (no limit) otherwise. */
int16_t scope_ui_lowest_visible_pos(uint8_t ch)
{
    const scope_state_t *ss = scope_state_get();
    const uint8_t r = ch ? ss->ch2.vdiv_idx : ss->ch1.vdiv_idx;
    const float cpd = scope_cal_codes_per_div((uint8_t)(ch + 1u), r);
    if (!ss->true_scale || !(cpd > 0.0f) || !scope_vpos_active())
        return -100;
    /* rows from the centre line down to half a division above the strip */
    const int rows = (int)strip_top(ss) - SCOPE_GRID_MID_Y - SCOPE_DIV_PX / 2;
    int p = -(int)((float)rows * cpd / (float)SCOPE_DIV_PX);
    if (p < -100) p = -100;
    if (p > 0) p = 0;
    return (int16_t)p;
}

/* The plan's R (record samples per column x 1000) for the current state. */
uint32_t scope_ui_view_ratio(void)
{
    uint32_t r = PV_RATIO_ONE;
    (void)view_plan(scope_state_get(), &r, NULL);
    return r;
}

/* The true-scale rescale of a record captured at another range or position
 * (stock's map at 0x080212BC): zero_now + k (v - zero_cap), k = volts/count
 * at capture / now. Channel ch is 0/1; anything else, or roll, is identity.
 * A held record uses the latched tuple; a running one its own capture
 * configuration, so the record still on the glass in the cycle after a range
 * or position change is drawn at its true volts. Identity when range and
 * position are the capture's (every steady running frame). */
static inline uint8_t pv_vscale(uint8_t v, int8_t ch)
{
    const scope_state_t *ss = scope_state_get();
    if (ch < 0 || ch > 1)
        return v;
    float kc;
    int zc;
    if (rec_held_cap(ss)) {
        kc = g_cap.vpc[ch];
        zc = g_cap.zero[ch];
    } else if (ss->roll || !rec_own_scale((uint8_t)ch, &kc, &zc)) {
        return v;
    }
    const float kn = scope_cal_volts_per_count((uint8_t)(ch + 1),
                                               ch ? ss->ch2.vdiv_idx : ss->ch1.vdiv_idx);
    const int zn = chan_zero_code(ss, (uint8_t)ch);
    if (!(kc > 0.0f) || !(kn > 0.0f) || (kc == kn && zc == zn))
        return v;
    return (uint8_t)pv_y_map((int)v, 0, zc, zn, kc, kn, 0, 255);
}

/* Fit the autofit map *a to the band [y_top, y_bot] from buf (yt_cols of the
 * channel). mask_ch: the channel this band draws (0/1), or -1. When a
 * finished mask covers it, the fit also spans the mask's extent, so the
 * bounds drawn over the trace stay on the glass instead of clamping to the
 * band edge. Callers: yt_fits (the maps of the full redraw, the live frame
 * and the trigger marker) and the split view, each passing the channel it
 * draws. */
void autofit_prep(autofit_t *a, const volatile uint8_t *buf,
                         int16_t y_top, int16_t y_bot, int8_t mask_ch)
{
    /* Over the window as DRAWN (the interpolated columns, not the source
     * samples): sin(x)/x overshoots a fast edge by a few counts, and fitting
     * the source would clip that flat at the band edge. */
    uint8_t mn = 255, mx = 0;
    for (uint16_t x = g_view_first; x < g_view_end; x++) {   /* not the blank columns */
        const uint8_t sv = buf[x];
        if (sv < mn) mn = sv;
        if (sv > mx) mx = sv;
    }
    uint8_t elo, ehi;
    if (mask_ch >= 0 && scope_mask_extent((uint8_t)mask_ch, &elo, &ehi)) {
        if (elo < mn) mn = elo;
        if (ehi > mx) mx = ehi;
    }
    yt_vmap_autofit(a, mn, mx, y_top, y_bot);
}

/* Where code v lands, unclipped (the tags) and clipped (everything drawn).
 * True scale first rescales a record captured at another range or position
 * to the one now selected (pv_vscale). */
int autofit_y_raw(const autofit_t *a, int v)
{
    if (a->fx_ch >= 0 && !a->as_stored)
        v = pv_vscale((uint8_t)v, a->fx_ch);
    return yt_vmap_raw(a, v);
}

int16_t autofit_y(const autofit_t *a, uint8_t v)
{
    return yt_vmap_clip(a, autofit_y_raw(a, v));
}

/* The record code true scale puts on the centre line: 0 V at position 0
 * (raw 128, record 100, with the calibration page; record 128 without it,
 * scope_measure_zero_for). Position moves the signal in the record. */
uint8_t yt_centre_code(void)
{
    return scope_measure_zero_for(scope_vpos_active(), 0).v;
}

/* Record code of the channel's 0 V (ch 0/1): the ground tag, the badges and
 * the cursors' absolute Y all use it. */
int chan_zero_code(const scope_state_t *ss, uint8_t ch)
{
    return scope_measure_zero_for(scope_vpos_active(),
                                  ch ? ss->ch2.position : ss->ch1.position).v;
}

/* The trigger-position pointer: over the column where the trigger point
 * actually landed (g_place.trig_x), at the top of the plot. */
bool tpos_px(int trig_x, int x, int dy)
{
    return trig_x >= 0 &&
           plot_marker_tpos_px(x - (trig_x - PM_TPOS_W / 2), dy) == PM_PX_FILL;
}

/* The placement the display draws with (scope_ui_parts.h). */
scope_placement_t g_place = { .trig_x = -1 };

/* The hardware trigger point of a time-ordered record (trig_edge_anchor_of,
 * dsp/trig_edge.h): the known index when our core ordered the record, else a
 * search near index 512. The mask test uses the same function, so it aligns
 * on the crossing the glass shows. Returns the index or -1. */
static int hw_trigger_anchor(const scope_state_t *ss, const volatile uint8_t *buf, bool *exact)
{
    return trig_edge_anchor_of(buf, trig_level_rec((adc_raw_t){ fpga_acq_trig_code_get() }),
                               ss->trigger.edge == TRIG_RISING,
                               fpga_acq_record_core_ordered(), exact);
}

/* Free-run window: from index 0. */
static uint16_t view_free_run(scope_placement_t *pl, uint8_t L)
{
    pl->win.L  = L;
    pl->win.u0 = 0;
    return (uint16_t)scope_geom_first_sample(&pl->win);
}

/* Software display trigger: place the window so the trigger point of src_buf
 * lands on the trigger column, so every frame starts on the same phase of the
 * wave. Only where the window starts changes; the samples are untouched. Both
 * channels are filled through the same window (view_build), so they stay
 * time-aligned.
 *
 * Order: free-run from index 0 when the soft trigger is off or there is no
 * buffer; else the hardware anchor of a time-ordered record; else a software
 * edge search about the record's midline; else free-run (a flat record, or
 * no crossing: the level outside the signal, which is AUTO behaviour).
 *
 * Returns the record index window column 0 shows (what `spi3 frame` prints
 * as off). Fills *pl: the window placed at zoom L (the plan's; 1 for the
 * general held-record path, which pivots on the column left in pl->trig_x),
 * the trigger column and the anchor kind. Puts in
 * *back_q8 (may be NULL) how far before the anchor sample the crossing lies,
 * in 1/256 sample (0 when nothing was locked). */
uint16_t scope_soft_trigger_offset(const scope_state_t *ss,
                                          const volatile uint8_t *src_buf, uint8_t L,
                                          int16_t *back_q8, scope_placement_t *pl)
{
    pl->trig_x = -1;
    pl->anchor = VIEW_ANCHOR_NONE;
    if (back_q8) *back_q8 = 0;
    if (!ss->soft_trigger || src_buf == NULL)
        return view_free_run(pl, L);

    const int32_t N = (int32_t)FPGA_ADC_BUF_SIZE;

    /* Horizontal position: the column the trigger point should land on,
     * stock's 150 + hpos with hpos +-150 (scope_geom.h). */
    int tx = ss->trig_x;
    if (tx < SCOPE_GEOM_TRIG_COL_MIN) tx = SCOPE_GEOM_TRIG_COL_MIN;
    if (tx > SCOPE_GEOM_TRIG_COL_MAX) tx = SCOPE_GEOM_TRIG_COL_MAX;
    int32_t before, after;           /* record samples left / right of tx */
    scope_geom_span(L, tx, &before, &after);

    /* 1. Hardware anchor: a time-ordered record carries the hardware trigger
     *    at index 512, and the crossing there is the event that fired. Our
     *    core's level is exact on both converters, so either channel as the
     *    source is anchored this way. The test is on the record's state, not
     *    on the buffer address, so `spi3 frame`, which passes a snapshot
     *    copy, takes the same path as the screen.
     *    Zoomed (L > 1), the crossing is placed to 1/L sample from the two
     *    samples around it: at 10 ns/div one sample is 10-20 columns (stock
     *    re-finds it on its interpolated copy for the same reason,
     *    0x08021EF0). */
    if (fpga_acq_record_time_ordered()) {
        int a = hw_trigger_anchor(ss, src_buf, &pl->exact);
        if (a >= 1) {
            /* From the search, a >= 1 means the level is 4..251 and the clamp
             * in trig_level_rec() never acts; at the core's index the level
             * may be at a rail, and the two samples still straddle it. */
            const int c = (int)trig_level_rec((adc_raw_t){ fpga_acq_trig_code_get() }).v;
            const int32_t back = scope_geom_back_q8(src_buf[a - 1], src_buf[a], c);
            if (back_q8) *back_q8 = (int16_t)back;
            pl->trig_x = (int16_t)scope_geom_fit(&pl->win, L, a, back, tx, 0, N);
            pl->anchor = VIEW_ANCHOR_HW;
            return (uint16_t)scope_geom_first_sample(&pl->win);
        }
    }

    /* Auto-level: the threshold sits at the signal's own midline, not at a
     * fixed code. A record is often not centred, so a fixed code can sit near
     * a peak where the slope is shallow and the crossing jitters frame to
     * frame. The 50% level (min+max)/2 is the steep mid-slope crossing and
     * locks regardless of DC offset; trigger.level shifts it from there
     * (SCOPE_H px == 256 counts). Min/max over the whole record. */
    uint8_t mn = 255, mx = 0;
    for (uint16_t i = 0; i < FPGA_ADC_BUF_SIZE; i++) {
        uint8_t s = src_buf[i];
        if (s < mn) mn = s;
        if (s > mx) mx = s;
    }
    if ((int)mx - (int)mn < 8)
        return view_free_run(pl, L);                  /* ~flat: nothing to lock to */

    int thr = ((int)mn + (int)mx) / 2 + ((int)ss->trigger.level * 256) / SCOPE_H;
    if (thr < (int)mn + 1) thr = (int)mn + 1;     /* keep it inside the signal */
    if (thr > (int)mx - 1) thr = (int)mx - 1;

    /* 2. Soft anchor: the crossing of thr, searched where the window
     *    (`before` samples left of the crossing, `after` right) stays inside
     *    the record. Schmitt-armed (3 counts) with a three-point confirm
     *    (stock's test, 0x08028DE4): a crossing whose next sample falls
     *    straight back is a one-sample spike, not the edge. SINGLE keeps two
     *    points, as stock does. The measurements behind this are in
     *    trig_edge.h (tests/test_trig_edge.c [7]-[9]). */
    const int lo = (int)before;
    const int hi = (int)N - (int)after;           /* last anchor that fits, +1 */
    int a = trig_edge_search(src_buf, (int)FPGA_ADC_BUF_SIZE, lo, hi, thr,
                             ss->trigger.edge == TRIG_RISING, 3u,
                             ss->trigger.mode == TRIG_SINGLE ? 0u : 1u);
    /* -1 is no crossing. A crossing at index lo itself is treated as none
     * too. */
    if (a <= lo)
        return view_free_run(pl, L);
    const int32_t back = scope_geom_back_q8(src_buf[a - 1], src_buf[a], thr);
    if (back_q8) *back_q8 = (int16_t)back;
    pl->trig_x = (int16_t)scope_geom_fit(&pl->win, L, a, back, tx,
                                              0, N);
    pl->anchor = VIEW_ANCHOR_SOFT;
    return (uint16_t)scope_geom_first_sample(&pl->win);
}

/* The roll FIFO is laid out one point per column of the graticule. */
_Static_assert(SCOPE_ROLL_POINTS == SCOPE_GEOM_COLS, "one roll point per plot column");

/* The roll source (scope_roll.h): the FIFO the acquisition task is filling,
 * laid out in yt_cols as the time view shows it, newest at the right edge.
 * g_span is the columns that hold a point; g_place has no placement (no
 * trigger column, nothing anchored: the mask overlay stays off). Returns the
 * points shown. */
uint16_t view_build_roll(void)
{
    uint16_t n = 0u;
    (void)scope_roll_fifo_columns(fpga_roll_fifo(), yt_cols, &n, NULL);
    g_span.first = (uint16_t)(SCOPE_GEOM_COLS - n);
    g_span.end   = SCOPE_GEOM_COLS;
    g_place = (scope_placement_t){ .trig_x = -1, .anchor = VIEW_ANCHOR_NONE };
    return n;
}

/* Place the window on the trigger source and fill yt_cols for BOTH channels
 * through it (a channel with no buffer reads zeros). One copy for every
 * caller (full redraw, live frame, trigger marker, split view), so a change
 * of renderer can never move or re-phase the trace. The mapping is
 * view_plan()'s: the integer window, or (L == 0) the general ratio of a held
 * record about the trigger column. Sets yt_cols, g_place.win, g_span
 * (g_view_first, g_view_end), g_place.trig_x and g_place.anchor. */
void view_build(const scope_state_t *ss, const volatile uint8_t *b1,
                       const volatile uint8_t *b2)
{
    const volatile uint8_t *src =
        (ss->trigger.source == TRIG_SRC_CH2 && b2) ? b2 : (b1 ? b1 : b2);
    const int32_t N = (int32_t)FPGA_ADC_BUF_SIZE;
    uint32_t r = PV_RATIO_ONE;
    const uint8_t L = view_plan(ss, &r, NULL);
    g_view_first = 0;
    g_view_end = SCOPE_GEOM_COLS;
    if (L != 0u) {
        (void)scope_soft_trigger_offset(ss, src, L, NULL, &g_place);
        scope_geom_fill(b1, N, &g_place.win, yt_cols[0], SCOPE_GEOM_COLS);
        scope_geom_fill(b2, N, &g_place.win, yt_cols[1], SCOPE_GEOM_COLS);
        return;
    }
    /* General path: find and place the trigger as the one-sample-per-column
     * window would, then keep that column (the trigger's, else the centre)
     * on that sample (on the crossing itself, to 1/1000 sample, when
     * zoomed in) and scale time about it. */
    int16_t back = 0;
    (void)scope_soft_trigger_offset(ss, src, 1u, &back, &g_place);
    const int32_t pc = (g_place.trig_x >= 0) ? g_place.trig_x : SCOPE_GEOM_CENTER_COL;
    int32_t pos0 = (g_place.win.u0 + pc) * 1000 - pc * (int32_t)r;
    if (r < PV_RATIO_ONE && g_place.trig_x >= 0)
        pos0 -= ((int32_t)back * 1000 + 128) >> 8;
    uint16_t f = 0, e = 0;
    /* The span from a channel that has a buffer (both: same record length,
     * same span; a NULL buffer reads blank throughout). */
    pv_fill(b1, 0, N, pos0, r, yt_cols[0], SCOPE_GEOM_COLS, b1 ? &f : NULL, b1 ? &e : NULL);
    pv_fill(b2, 0, N, pos0, r, yt_cols[1], SCOPE_GEOM_COLS, b1 ? NULL : &f, b1 ? NULL : &e);
    g_view_first = f;
    g_view_end = e;
    g_place.win.L = 0u;              /* not one column per sample: no mask overlay */
}

int16_t scope_ui_trig_x_actual(void) { return g_place.trig_x; }

/* The plan's L: the integer zoom, 0 on the general held-record path. */
uint8_t scope_ui_view_factor(void)
{
    return view_plan(scope_state_get(), NULL, NULL);
}

uint8_t scope_ui_trig_anchor(void)   { return g_place.anchor; }

bool    scope_ui_trig_anchor_exact(void) { return g_place.anchor == VIEW_ANCHOR_HW && g_place.exact; }

/* For bench validation (`spi3 frame`, shell task): the offset the display
 * would start the drawn window at for the current scope state and this
 * record, computed by the display's own function into a placement of its
 * own, so the display's is not touched; the trigger column, the anchor kind
 * (a view_anchor_t) and whether it is exact come back through the pointers.
 * On the general held-record path the offset is the one-sample-per-column
 * window the pivot is taken from. */
uint16_t scope_ui_soft_trigger_offset(const volatile uint8_t *src_buf, int16_t *trig_x,
                                      uint8_t *anchor, bool *exact)
{
    const scope_state_t *ss = scope_state_get();
    const uint8_t L = view_plan(ss, NULL, NULL);
    scope_placement_t pl = { .trig_x = -1 };
    const uint16_t off = scope_soft_trigger_offset(ss, src_buf, L ? L : 1u, NULL, &pl);
    *trig_x = pl.trig_x;
    *anchor = pl.anchor;
    *exact  = pl.anchor == VIEW_ANCHOR_HW && pl.exact;
    return off;
}
