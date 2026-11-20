/*
 * OpenScope 2C53T - oscilloscope screen: the view of a record
 *
 * Which part of a record the plot shows and at what scale: held and merged
 * views, the autofit, the trigger anchor and the software trigger offset.
 */

#include "scope_ui_parts.h"

/* Rate for anything timed off the record on screen (Freq, widths, cursors):
 * the PUBLISHED record's rate — doubled when it is the interleaved merge
 * (fpga_record_sample_rate) — while the hardware is running the code the UI
 * shows; the table rate for the selected code otherwise. One copy, so the
 * badges and the cursors cannot disagree. */
float record_rate_for_display(const scope_state_t *ss)
{
    /* A held record keeps the rate it was captured at, whatever the code now. */
    if (!ss->running && !ss->roll && fpga_data_ready())
        return fpga_record_sample_rate();
    return (fpga_acq_rate_idx_get() == ss->timebase_idx)
         ? fpga_record_sample_rate()
         : scope_timebase_sample_rate(ss->timebase_idx);
}

/* A record is HELD (STOP, zoom while paused, C14): no new record will be
 * published, so the screen re-samples the one it has. */
static inline bool view_held(const scope_state_t *ss)
{
    return !ss->running && !ss->roll && fpga_data_ready();
}

/* Is the view drawn as the two-converter merge? Running: what the published
 * record IS (fpga_acq_record_interleaved). Held (STOP, C14): what the code
 * now on the status bar would capture (fpga_interleave_active: enabled, one
 * channel, code <= 0x03), because the held record keeps its own rate
 * (fpga_record_sample_rate) and the screen is re-sampled to the code's
 * column rate. Stock's display time per pixel depends on the code alone
 * (0x08030A70), so a held merged record viewed at 0x04 reads 200 ns/div and is
 * zoomed out 2x, not relabelled 100 ns. At 0x00-0x03 either flag gives the same
 * column rate (124.8 MS/s x 20 == 249.6 MS/s x 10), so only that case differs.
 * The status bar, the timebase popup and the shell label read the same flag
 * (scope_ui_view_merged), so the label and the column rate cannot disagree. */
bool view_merged(const scope_state_t *ss)
{
    return view_held(ss) ? fpga_interleave_active() : fpga_acq_record_interleaved();
}

bool scope_ui_view_merged(void) { return view_merged(scope_state_get()); }

/* The display zoom for the code on the status bar (parity C5,
 * scope_timebase_interp_factor): 1 everywhere but 0x00-0x03. The label
 * (status_bar.c) and the window plan (view_plan) derive from the same call. */
static uint8_t view_factor(const scope_state_t *ss)
{
    return scope_timebase_interp_factor(ss->timebase_idx, view_merged(ss));
}

/* ── The Y-T window's horizontal mapping (parity C5 x C14) ───────────────
 * ONE window, yt_cols, built once per frame by view_build() and read by every
 * consumer (full path, live compositor, trigger marker, math trace; the
 * cursors read the column rate it implies, the mask overlay is gated on it).
 * Its mapping comes from one plan (pv_view_plan, src/dsp/paused_view.c):
 *
 *   ratio R   record samples per column x 1000. Running: 1000 / L, L the
 *             fast-code zoom of parity C5 (10/5/2/1 merged, 20/10/4/2 per
 *             converter at 0x00-0x03, 1 elsewhere). Held (STOP): stock
 *             re-renders the held record when the timebase changes
 *             (0x08021178..0x08021348, docs/re/parity2_static.md s3; it never
 *             re-arms), so R = 1000 fs_rec / fs_col, the held record's own
 *             rate (fpga_record_sample_rate = rate at commit) over the column
 *             rate of the code now selected.
 *   L > 0     R == 1000 / L with L dividing 20: the C5 integer window
 *             (scope_geom.c), trigger fitted to 1/L sample, window clamped
 *             inside the record. Covers running, and STOP at (or between) fast
 *             codes of one record rate.
 *   L == 0    any other held ratio (zoom while paused, C14): pv_fill, the
 *             trigger column stays put, nearest sample for R >= 1000, the same
 *             16-tap sinc as C5 at any phase below it, columns outside the
 *             record blank as in stock: [g_view_first, g_view_end) is the
 *             span that shows the record, and every consumer stays inside it.
 *
 * Vertical (C14): in true scale only (autofit is range-blind by
 * construction), a held record is re-drawn as stock does (0x080212BC):
 * code' = zero_now + k (code - zero_cap), k = volts/count at capture over
 * volts/count now (pv_vscale), so it reads true at the range and position now
 * selected. Measurements of a held record use the capture's volts/count and
 * 0 V (rec_vpc / rec_zero).
 * Stock's position term is not applied: a position key moves the offset DAC
 * for the NEXT capture, so the record already carries it.
 * Cursor and badge rates: the cursors read the column rate (1000 fs_rec / R);
 * the measurements read the record's own (record_rate_for_display). */
/* The span of the last build that shows the record, [first, end): all 301
 * columns but on the general path. Zero-initialised on purpose (bss, not
 * .data: the warm-handoff builds have no RAM to spare); view_build() sets it
 * before any consumer reads it. */
g_span_t g_span;

/* What the record's codes mean: volts/count and the 0 V code of each channel
 * when it was captured. Latched every running frame and at STOP. */
static struct {
    float   vpc[2];
    uint8_t zero[2];
    bool    valid;
} g_cap;

int chan_zero_code(const scope_state_t *ss, uint8_t ch);

/* The plan for the current state: returns L (0 = general path), *ratio = R,
 * *fs_rec = the rate R was taken against. A pure function of the state, so
 * the cursor shell command and the readouts answer without a draw having run. */
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
 * record rate x L on the integer window (C5) and the selected code's column
 * rate on a re-sampled held record (C14, up to the 100x clamp). Only the
 * column-domain readouts (cursor dt, 1/dt, X) use it; everything measured on
 * the record itself (Freq, the C6 parameters) keeps the record rate, so a
 * zoom can never leak into a measurement. */
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

/* Volts per count and 0 V code of channel ch's record: the capture's for a
 * held record (the record does not change with the range or position now
 * selected), the current ones otherwise. */
static bool rec_held_cap(const scope_state_t *ss)
{
    return g_cap.valid && !ss->running && !ss->roll && fpga_data_ready();
}

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

uint32_t scope_ui_view_ratio(void)
{
    uint32_t r = PV_RATIO_ONE;
    (void)view_plan(scope_state_get(), &r, NULL);
    return r;
}

/* The true-scale path's held-record rescale (stock 0x080212BC):
 * zero_now + k (v - zero_cap), k = volts/count at capture / now. A held
 * record uses the latched tuple; a running one its own capture
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

/* mask_ch: the channel this band draws (0/1), or -1. When a finished mask
 * covers it, the fit also spans the mask's extent, so the bounds drawn over
 * the trace stay on the glass instead of clamping to the band edge. Every
 * caller passes the channel it draws -- trace, compositor, trigger marker --
 * so all three keep agreeing on the scale. */
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
 * True scale rescales a held record to the range now selected first. */
int autofit_y_raw(const autofit_t *a, int v)
{
    if (a->fx_ch >= 0)
        v = pv_vscale((uint8_t)v, a->fx_ch);
    return yt_vmap_raw(a, v);
}

int16_t autofit_y(const autofit_t *a, uint8_t v)
{
    return yt_vmap_clip(a, autofit_y_raw(a, v));
}

/* The record code true scale puts on the centre line: 0 V at position 0
 * (raw 128, stock's zero, record 100 with the calibration page; the legacy
 * centring targets record 128). Position moves the signal in the record. */
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
 * actually landed (g_trig_x_actual), at the top of the plot. */
bool tpos_px(int trig_x, int x, int y)
{
    return trig_x >= 0 &&
           plot_marker_tpos_px(x - (trig_x - PM_TPOS_W / 2), y - SCOPE_TOP) == PM_PX_FILL;
}

/*
 * Mask pass/fail overlay (docs/specs/scope/mask-pass-fail.md).
 *
 * The verdict is computed in the acquisition task (scope_mask.c); this only
 * PAINTS it, per column, from the result already recorded -- no analysis on
 * the display path. Painted only when the window is on the HARDWARE anchor
 * (g_trig_anchor == 2: the alignment the mask was taught on) and only for the
 * frame generation that was actually judged; anything else draws as if there
 * were no mask, rather than colouring a frame the verdict does not describe.
 *
 *   trace   CH1/CH2 trace pixels turn th->warning on columns that were
 *           outside the mask in that record.
 *   strip   3 px at the bottom of the live band over the masked span:
 *           green = passed, red = this column failed, dotted red = the record
 *           failed elsewhere, dotted amber = refused (SKIP), amber = teaching.
 */
uint8_t  g_trig_anchor = 0;        /* see scope_soft_trigger_offset() */

int16_t  g_trig_x_actual = -1;

/* The window last placed (scope_geom.h). L == 0 marks the general held-record
 * path of view_build (C14), so L == 1 means exactly "one column per sample". */
scope_window_t g_view;

/* Software display trigger — the fix for the free-running "dancing" trace.
 *
 * Real scopes hold a waveform still by starting every sweep at the same point
 * on the wave: the moment the signal crosses a chosen level going a chosen
 * direction. We do that in software here — find that crossing in the trigger
 * source buffer and return its index, so the render window starts on a
 * repeatable phase. The offset is applied to BOTH channels so they stay
 * time-aligned. It never distorts the samples: it only chooses WHERE the drawn
 * window begins, so amplitude and frequency are untouched.
 *
 * 0 = free-run (disabled, NULL buffer, or no crossing found). "No crossing" is
 * the normal outcome when the level sits outside the signal, and free-running
 * then is exactly AUTO-mode behaviour.
 */
/* Where the trigger point landed on the glass in the last computed window:
 * column 0..319, or -1 when free-running. g_trig_anchor: 0 none, 1 soft
 * (midline crossing), 2 hardware (the level crossing nearest index 512 of a
 * time-ordered record). Read by the position marker and by `spi3 frame`. */
/* g_trig_x_actual / g_trig_anchor are declared with the mask overlay above
 * (it reads them); they start at -1 / 0 via scope_soft_trigger_offset(). */

/* The hardware trigger point of a time-ordered record: the crossing of the
 * level (code - 28, EXP-55/56) in the chosen direction nearest index 512,
 * within +/-48 (EXP-53: 501..520). Schmitt-armed like the soft trigger.
 * Returns the index or -1. */
static bool g_trig_anchor_exact;         /* the last hardware anchor was the core's index, not a search */

static int hw_trigger_anchor(const scope_state_t *ss, const volatile uint8_t *buf)
{
    /* Moved to trig_edge_anchor() 2026-10-03 so the mask test aligns on the
     * same crossing the glass does; behaviour unchanged. A record our core
     * ordered has its trigger at a known index (trig_edge_anchor_of). */
    return trig_edge_anchor_of(buf, trig_level_rec((adc_raw_t){ fpga_acq_trig_code_get() }),
                               ss->trigger.edge == TRIG_RISING,
                               fpga_acq_record_core_ordered(), &g_trig_anchor_exact);
}

/* Free-run window: from index 0. */
static uint16_t view_free_run(uint8_t L)
{
    g_view.L  = L;
    g_view.u0 = 0;
    return (uint16_t)scope_geom_first_sample(&g_view);
}

/* Returns the record index window column 0 shows (the old `toff`, which is
 * what `spi3 frame` prints); the window itself is left in g_view, placed at
 * zoom L (the plan's; 1 for the general held-record path, which pivots on the
 * column this leaves in g_trig_x_actual), and in *back_q8 (may be NULL) how
 * far before the anchor sample the crossing lies, in 1/256 sample (0 when
 * nothing was locked). */
uint16_t scope_soft_trigger_offset(const scope_state_t *ss,
                                          const volatile uint8_t *src_buf, uint8_t L,
                                          int16_t *back_q8)
{
    g_trig_x_actual = -1;
    g_trig_anchor = 0;
    if (back_q8) *back_q8 = 0;
    if (!ss->soft_trigger || src_buf == NULL)
        return view_free_run(L);

    const int32_t N = (int32_t)FPGA_ADC_BUF_SIZE;

    /* Horizontal position: the column the trigger point should land on,
     * stock's 150 + hpos with hpos +-150 (scope_geom.h). */
    int tx = ss->trig_x;
    if (tx < SCOPE_GEOM_TRIG_COL_MIN) tx = SCOPE_GEOM_TRIG_COL_MIN;
    if (tx > SCOPE_GEOM_TRIG_COL_MAX) tx = SCOPE_GEOM_TRIG_COL_MAX;
    int32_t before, after;           /* record samples left / right of tx */
    scope_geom_span(L, tx, &before, &after);

    /* 1. Hardware anchor (2026-09-22): a time-ordered record carries the
     *    hardware trigger at index 512, and the crossing nearest it IS the
     *    event that fired. On the stock design CH1 only -- the level register
     *    was measured there; on our core the level is exact on both
     *    converters, so CH2 as the source is anchored the same way (before,
     *    it fell to the soft anchor below, which searches its own level:
     *    bench 2026-11-16, a sine's peak sat on the trigger column).
     *    Tested on the trigger SOURCE, not the buffer address: `spi3 frame`
     *    passes a snapshot copy, and an address test made the bench check
     *    exercise the fallback while the screen used the anchor (v17).
     *    Zoomed (L > 1), the crossing is placed to 1/L sample from the two
     *    samples around it -- stock re-finds it on the interpolated copy for
     *    the same reason (0x08021EF0): at 10 ns/div one sample is 10-20 px. */
    if (fpga_acq_record_time_ordered()) {
        int a = hw_trigger_anchor(ss, src_buf);
        if (a >= 1) {
            /* From the search, a >= 1 means the level is 4..251 and the clamp
             * in trig_level_rec() never acts; at the core's index the level
             * may be at a rail, and the two samples still straddle it. */
            const int c = (int)trig_level_rec((adc_raw_t){ fpga_acq_trig_code_get() }).v;
            const int32_t back = scope_geom_back_q8(src_buf[a - 1], src_buf[a], c);
            if (back_q8) *back_q8 = (int16_t)back;
            g_trig_x_actual = (int16_t)scope_geom_fit(&g_view, L, a, back, tx, 0, N);
            g_trig_anchor = 2;
            return (uint16_t)scope_geom_first_sample(&g_view);
        }
    }

    /* Auto-level: base the threshold at the SIGNAL'S OWN midline, not a fixed
     * ADC 128. The capture is frequently not centred (e.g. mean 84, range
     * 34..136 on the bench), so a fixed 128 sits up near the peak where the
     * slope is shallow and the crossing jitters frame to frame — which reads on
     * screen as the trace dancing. The 50% level (min+max)/2 is the steep
     * mid-slope crossing and locks cleanly regardless of DC offset;
     * trigger.level then shifts it up/down from there (true-scale slope:
     * SCOPE_H px == 256 counts). Min/max over the guarded region only, so a
     * seam's glitch cannot skew the threshold either. */
    uint8_t mn = 255, mx = 0;
    for (uint16_t i = 0; i < FPGA_ADC_BUF_SIZE; i++) {
        uint8_t s = src_buf[i];
        if (s < mn) mn = s;
        if (s > mx) mx = s;
    }
    if ((int)mx - (int)mn < 8)
        return view_free_run(L);                  /* ~flat: nothing to lock to */

    int thr = ((int)mn + (int)mx) / 2 + ((int)ss->trigger.level * 256) / SCOPE_H;
    if (thr < (int)mn + 1) thr = (int)mn + 1;     /* keep it inside the signal */
    if (thr > (int)mx - 1) thr = (int)mx - 1;

    /* 2. Soft anchor: the midline crossing, searched so that the window
     *    (`before` samples left of the crossing, `after` right) stays inside
     *    the record and clear of the guarded head. */
    /*    Stock's three-point edge confirm (0x08028DE4, parity C3) on top of
     *    the Schmitt arming this always had: a crossing whose next sample
     *    falls straight back is a one-sample spike, not the edge. SINGLE
     *    keeps two points, as stock does. The measured case for adding it
     *    is in trig_edge.h (tests/test_trig_edge.c [7]-[9]). */
    const int lo = (int)before;
    const int hi = (int)N - (int)after;           /* last start that fits, +1 */
    int a = trig_edge_search(src_buf, (int)FPGA_ADC_BUF_SIZE, lo, hi, thr,
                             ss->trigger.edge == TRIG_RISING, 3u,
                             ss->trigger.mode == TRIG_SINGLE ? 0u : 1u);
    /* The old search returned index 0 of its span as "nothing": keep that
     * (a crossing on the very first sample has no "before"). */
    if (a <= lo)
        return view_free_run(L);
    const int32_t back = scope_geom_back_q8(src_buf[a - 1], src_buf[a], thr);
    if (back_q8) *back_q8 = (int16_t)back;
    g_trig_x_actual = (int16_t)scope_geom_fit(&g_view, L, a, back, tx,
                                              0, N);
    g_trig_anchor = 1;
    return (uint16_t)scope_geom_first_sample(&g_view);
}

/* Place the window on the trigger source and fill yt_cols for BOTH channels
 * through it (a channel with no buffer reads zeros). One copy for every
 * caller -- full path, live compositor, trigger marker, math trace -- so a
 * change of renderer can never move or re-phase the trace. The mapping is
 * view_plan()'s: the C5 integer window, or (L == 0) the general ratio of a
 * held record about the trigger column, with the blank span recorded. */
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
        (void)scope_soft_trigger_offset(ss, src, L, NULL);
        scope_geom_fill(b1, N, &g_view, yt_cols[0], SCOPE_GEOM_COLS);
        scope_geom_fill(b2, N, &g_view, yt_cols[1], SCOPE_GEOM_COLS);
        return;
    }
    /* General (C14): find and place the trigger as the one-sample-per-column
     * window would, then keep that column (the trigger's, else the centre)
     * on that sample -- on the crossing itself, to 1/1000 sample, when
     * zoomed in -- and scale time about it. */
    int16_t back = 0;
    (void)scope_soft_trigger_offset(ss, src, 1u, &back);
    const int32_t pc = (g_trig_x_actual >= 0) ? g_trig_x_actual : SCOPE_GEOM_CENTER_COL;
    int32_t pos0 = (g_view.u0 + pc) * 1000 - pc * (int32_t)r;
    if (r < PV_RATIO_ONE && g_trig_x_actual >= 0)
        pos0 -= ((int32_t)back * 1000 + 128) >> 8;
    uint16_t f = 0, e = 0;
    /* The span from a channel that has a buffer (both: same record length,
     * same span; a NULL buffer reads blank throughout). */
    pv_fill(b1, 0, N, pos0, r, yt_cols[0], SCOPE_GEOM_COLS, b1 ? &f : NULL, b1 ? &e : NULL);
    pv_fill(b2, 0, N, pos0, r, yt_cols[1], SCOPE_GEOM_COLS, b1 ? NULL : &f, b1 ? NULL : &e);
    g_view_first = f;
    g_view_end = e;
    g_view.L = 0u;              /* not one column per sample: no mask overlay */
}

int16_t scope_ui_trig_x_actual(void) { return g_trig_x_actual; }

/* The plan's L: the integer zoom, 0 on the general held-record path. */
uint8_t scope_ui_view_factor(void)
{
    return view_plan(scope_state_get(), NULL, NULL);
}

uint8_t scope_ui_trig_anchor(void)   { return g_trig_anchor; }

bool    scope_ui_trig_anchor_exact(void) { return g_trig_anchor == 2u && g_trig_anchor_exact; }

/* Exported for bench validation (`spi3 frame`): the offset the renderer would
 * start the drawn window at for the CURRENT scope state. Exporting the real
 * function rather than letting the host re-implement it is deliberate — a
 * host replica passing while the on-screen algorithm fails is exactly the
 * class of stable-wrong-number this project keeps paying for. On the general
 * held-record path (L 0) it is the one-sample-per-column window the pivot is
 * taken from. */
uint16_t scope_ui_soft_trigger_offset(const volatile uint8_t *src_buf)
{
    const scope_state_t *ss = scope_state_get();
    const uint8_t L = view_plan(ss, NULL, NULL);
    return scope_soft_trigger_offset(ss, src_buf, L ? L : 1u, NULL);
}
