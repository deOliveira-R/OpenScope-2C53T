/*
 * OpenScope 2C53T - oscilloscope screen: furniture
 *
 * What surrounds the plot: the measurement strip and its badges,
 * the mask slot, the CAN row, the held frequency.
 */

#include "scope_ui_parts.h"
#include "key_model.h"      /* scope_view_sel */

/* Repaint the measurement strip between full frames. The live frame never
 * touches the strip, and plain Y-T has no full frames in a steady state, so
 * this is what keeps the badges (Freq included) current. The cells are
 * repainted in place (draw_strip_cell boxed), never blanked. The measurement
 * passes are tear-checked against the acquisition's frame generation
 * (measure_live_channel, freq_held), as in the full path. In roll, the
 * points the last frame laid out. Does nothing before the first record. */
void draw_measurement_badges(const scope_state_t *ss, const theme_t *th, bool refresh);

void scope_ui_strip_refresh(void)
{
    const scope_state_t *ss = scope_state_get();
    if (!ss->roll && !fpga_data_ready()) return;
    draw_measurement_badges(ss, theme_get(), true);
}

/* The 12 measurements over the roll points the time view shows (yt_cols from
 * g_view_first, oldest first; scope_roll_meas.h) into the cells of L: the
 * same slots, order and units as the record's, the time base being the roll
 * point rate. Zero and scale are per channel, the settings in force (the
 * points were taken with them): chan_zero_code() and the scope_cal gain.
 * refresh: repaint the cells in place. */
static void draw_roll_cells(const scope_state_t *ss, const mstrip_layout_t *L, const theme_t *th, bool refresh)
{
    const uint16_t n = (uint16_t)(g_view_end - g_view_first);
    scope_params_t p1, p2;
    const uint8_t z1 = (uint8_t)chan_zero_code(ss, 0u);   /* the one zero (chan_zero_code) */
    const uint8_t z2 = (uint8_t)chan_zero_code(ss, 1u);
    const bool have1 = ss->ch1.enabled &&
        scope_roll_measure(&yt_cols[0][g_view_first], n, ss->roll_step,
                           (volts_per_count_t){ scope_cal_volts_per_count(1u, ss->ch1.vdiv_idx) },
                           (rec_code_t){ z1 }, &p1, NULL);
    const bool have2 = ss->ch2.enabled &&
        scope_roll_measure(&yt_cols[1][g_view_first], n, ss->roll_step,
                           (volts_per_count_t){ scope_cal_volts_per_count(2u, ss->ch2.vdiv_idx) },
                           (rec_code_t){ z2 }, &p2, NULL);
    draw_strip_cells(L, have1 ? &p1 : NULL, have2 ? &p2 : NULL, th, refresh);
}

/* ═══ Measurement strip (geometry in measure_strip.h) ═══ */

/* The full-width AUX row (above the measurement rows) is reserved while the
 * CAN analyser's line, the cursor readout or the mask pass/fail readout has
 * something to say; it shows the first of them, in that order. */
static bool strip_aux(void)
{
    return can_scope_screen_on() || scope_state_get()->cursor.mode != CURSOR_OFF ||
           scope_mask_state() != MASK_PF_EMPTY;
}

void cursor_row_paint(const scope_state_t *ss, bool force);

/* The measurement rows are hidden by the results setting of the view on
 * screen (view_table.h: the full views' view.meashide, split's own
 * view.splithide); the AUX row is not. */
static bool rows_hidden(const scope_state_t *ss)
{
    return scope_results_hidden(ss, view_desc_of(scope_view_sel()).results);
}

/* Every user of the strip's geometry (the live band, the roll band, the mask
 * strip, the cursor readout, the AUX row) reads this one function, so they
 * cannot disagree about where the strip starts. */
uint16_t strip_top(const scope_state_t *ss)
{
    return measure_strip_top(meas_select_get(MEAS_SET_CH1), ss->ch1.enabled,
                             meas_select_get(MEAS_SET_CH2), ss->ch2.enabled,
                             rows_hidden(ss), strip_aux(), STRIP_BOTTOM);
}

void strip_layout(const scope_state_t *ss, mstrip_layout_t *L)
{
    measure_strip_layout(meas_select_get(MEAS_SET_CH1), ss->ch1.enabled,
                         meas_select_get(MEAS_SET_CH2), ss->ch2.enabled,
                         rows_hidden(ss), strip_aux(), STRIP_BOTTOM, L);
}

/* The spectrum's strip: the same rows and cells, holding the spectrum's
 * measurement set (dsp/fft_live.h) as one block, since a spectrum view
 * analyses one channel. */
_Static_assert(MEAS_SELECT_FFT_COUNT == FFT_MEAS_COUNT &&
               MEAS_SELECT_FFT_DEFAULT == FFT_MEAS_DEFAULT_MASK,
               "meas_select.h counts the spectrum's measurements as fft_live.h has them");
_Static_assert(FFT_MEAS_COUNT <= MSTRIP_MAX_PER_CH, "every spectrum measurement has a cell");

/* Its AUX row is reserved while the cursors are on: their readout. */
static void fft_strip_layout(const scope_state_t *ss, mstrip_layout_t *L)
{
    measure_strip_layout(meas_select_get(MEAS_SET_FFT), true, 0u, false,
                         rows_hidden(ss), ss->cursor.mode != CURSOR_OFF, STRIP_BOTTOM, L);
}

uint16_t fft_strip_top(const scope_state_t *ss)
{
    return measure_strip_top(meas_select_get(MEAS_SET_FFT), true, 0u, false,
                             rows_hidden(ss), ss->cursor.mode != CURSOR_OFF, STRIP_BOTTOM);
}

/* One cell: label left in the dimmed channel colour chc, the value buf right
 * in chc (text_secondary when it is not a measured value: "--").
 * measure_strip_place() guarantees the two never overlap; when they would
 * not fit it keeps the value only (the host test shows no label/value pair
 * of the measurement sets gets there). */
static void strip_cell_paint(const mstrip_cell_t *c, const char *label, const char *buf,
                             bool measured, uint16_t chc, const theme_t *th, bool boxed)
{
    const uint16_t vc = measured ? chc : th->text_secondary;
    uint16_t lx, vx;
    const bool both = measure_strip_place(c->x, font_string_width(label, &font_small),
                                          font_string_width(buf, &font_small), &lx, &vx);
    if (both)
        font_draw_string(lx, (uint16_t)(c->y + 1), label,
                         measure_strip_dim(chc, th->background), th->background, &font_small);
    if (!boxed) {
        font_draw_string(vx, (uint16_t)(c->y + 1), buf, vc, th->background, &font_small);
        return;
    }
    /* In-place update: the value's whole box is painted (glyphs, gaps and
     * the rest of the box), so a shorter value leaves nothing behind and the
     * row is never blanked first. */
    const uint16_t end = (uint16_t)(c->x + MSTRIP_CELL_W - MSTRIP_PAD);
    const uint16_t bx = both ? vx : (uint16_t)(c->x + MSTRIP_PAD);
    if (end > bx)
        font_draw_string_box(bx, (uint16_t)(c->y + 1), (uint16_t)(end - bx), buf, vc,
                             th->background, &font_small,
                             both ? FONT_ALIGN_LEFT : FONT_ALIGN_RIGHT);
}

/* Paint the measurement rows. refresh false: clear them first (full width,
 * so the live band and the strip meet on one row edge), then paint every
 * cell. refresh true: repaint the cells in place. p1/p2 NULL = that channel
 * has no record. */
void draw_strip_cells(const mstrip_layout_t *L, const scope_params_t *p1,
                             const scope_params_t *p2, const theme_t *th, bool refresh)
{
    if (L->rows == 0u)
        return;
    const uint16_t y0 = (uint16_t)(L->bottom - L->rows * MSTRIP_ROW_H);
    if (!refresh)
        lcd_fill_rect(0, y0, LCD_WIDTH, (uint16_t)(L->rows * MSTRIP_ROW_H), th->background);
    for (unsigned k = 0; k < L->n; k++) {
        const mstrip_cell_t *c = &L->cell[k];
        const scope_params_t *pp = c->ch ? p2 : p1;
        const scope_prm_t *p = pp ? &pp->v[c->prm] : NULL;
        char buf[16];
        scope_prm_format(p, buf, sizeof(buf));          /* "--" when it cannot be measured */
        strip_cell_paint(c, scope_prm_label(c->prm), buf, p != NULL && p->unit != SCOPE_PRM_UNIT_NONE,
                         c->ch ? th->ch2 : th->ch1, th, refresh);
    }
}

/* The spectrum's strip: m holds the spectrum's measurements (fft_live_measure),
 * or is NULL when there is no spectrum; ch2: it is CH2's. refresh as
 * draw_strip_cells. */
void draw_fft_strip(const scope_state_t *ss, const theme_t *th, const fft_meas_t *m, bool ch2, bool refresh)
{
    mstrip_layout_t L;
    fft_strip_layout(ss, &L);
    if (!refresh && L.top < L.bottom)
        lcd_fill_rect(0, L.top, LCD_WIDTH, (uint16_t)(L.bottom - L.top), th->background);
    for (unsigned k = 0; k < L.n; k++) {
        const mstrip_cell_t *c = &L.cell[k];
        const fft_meas_t *v = m ? &m[c->prm] : NULL;
        char buf[16];
        fft_meas_format(v, buf, sizeof(buf));
        strip_cell_paint(c, meas_select_label(MEAS_SET_FFT, c->prm), buf,
                         v != NULL && v->unit != FFT_MEAS_UNIT_NONE, ch2 ? th->ch2 : th->ch1, th, refresh);
    }
}

/*
 * Measure one channel's live capture record.
 *
 * Returns false, and leaves *m zeroed, unless the FPGA has actually
 * delivered samples. fpga_data_ready() is the same gate the waveform plot
 * uses (yt_prepare, scope_yt.c), so the badges and the trace always agree
 * about whether what is on screen is real.
 */
static bool measure_live_channel(const volatile uint8_t *buf,
                                 scope_measure_t *m)
{
    memset(m, 0, sizeof(*m));
    if (!fpga_data_ready() || buf == NULL)
        return false;

    /* Tear-checked read. fpga_acq.c stages each capture and commits it in a
     * ~10 µs window bracketed by the frame generation counter (odd = commit
     * in progress). A commit landing inside our analysis pass would splice
     * two frames through every statistic, so the pass is re-run when the
     * generation moved. Commits arrive at ≤ ~34 Hz, so the second attempt
     * virtually always stands; after three, the torn result is accepted
     * rather than spinning. The volatile cast is safe because the generation
     * check is what detects concurrent writes. */
    for (int attempt = 0; attempt < 3; attempt++) {
        uint32_t g0 = fpga_acq_frame_generation();
        scope_measure_record((const uint8_t *)buf, FPGA_ADC_BUF_SIZE, m);
        if ((g0 & 1u) == 0u && fpga_acq_frame_generation() == g0)
            break;
    }
    return m->valid;
}

/*
 * Vertical calibration lives in scope_cal.c, per CHANNEL as well as per
 * range, because the two channels have different frontends and measure
 * different gains on the same range index. The provenance of the gains and
 * the reason the table is tiered rather than flat are in scope_cal.h.
 *
 * The volts/div label is the range's nominal value; true scale draws one
 * division of the pane's graticule as scope_cal_codes_per_div() codes
 * (yt_vmap.h), the grid and the map taking the division from the same pane
 * (plot_pane.h), so the grid means what the label says.
 */

/* The cal table is indexed by the frontend range index, so its length and
 * the UI's range count are the same quantity counted twice. */
_Static_assert(SCOPE_CAL_RANGE_COUNT == VDIV_COUNT,
               "scope_cal range table length must match VDIV_COUNT");

/* The horizontal counterpart: one sample per screen column at zoom 1,
 * vertical grid lines every SCOPE_GEOM_DIV_PX (25) px, so a division is 25
 * samples; at timebase codes 0x00-0x03 the zoom divides it
 * (scope_timebase_view_label).
 * If either changes, the seconds/div label and every frequency silently start
 * meaning something else. */
_Static_assert(SCOPE_TIMEBASE_SAMPLES_PER_DIV == (float)SCOPE_GEOM_DIV_PX,
               "grid pitch no longer matches SCOPE_TIMEBASE_SAMPLES_PER_DIV");

_Static_assert(SCOPE_GEOM_COLS <= LCD_WIDTH, "the Y-T window must fit the glass");

_Static_assert(SCOPE_TIMEBASE_CODE_COUNT == TIMEBASE_COUNT,
               "timebase rate table length must match TIMEBASE_COUNT");

/*
 * Mask readout: the strip's AUX row (measure_strip.h), reserved while a mask
 * exists. It lies below the live band, so the live frame never paints over
 * it. main.c refreshes it on its own epoch (scope_ui_mask_slot_refresh), so
 * a pass/fail counter is never stale until the next full repaint. A SKIP is
 * shown as such: a frozen PASS count over records that are no longer being
 * judged would be a stable, plausible and wrong number.
 */
#define MASK_SLOT_X 4u

/* Short on-screen text for the reason a mask test was skipped or rejected. */
static const char *mask_skip_short(mask_pf_reason_t r)
{
    switch (r) {
    case MASK_PF_R_STALE:       return "stale";
    case MASK_PF_R_UNTRIGGERED: return "free-run";
    case MASK_PF_R_NOT_ORDERED: return "no seam";
    case MASK_PF_R_NO_ANCHOR:   return "no trig";
    case MASK_PF_R_SETTINGS:    return "settings";
    case MASK_PF_R_SPAN:        return "span";
    case MASK_PF_R_CLIPPED:     return "clipped";
    default:                    return "?";
    }
}

/* Paint the AUX row with the mask state: teaching progress, FAIL with hold,
 * skip reason, ready, or the pass/fail counts. Clears the row first. */
static void draw_mask_slot(const theme_t *th)
{
    scope_mask_summary_t m;
    scope_mask_summary(&m);
    char b[32];
    uint16_t c = th->text_secondary;
    const uint16_t x = MASK_SLOT_X, y = strip_top(scope_state_get());   /* the AUX row */

    if (m.state == MASK_PF_TEACHING) {
        if (m.teach_last_reject != MASK_PF_R_OK)
            snprintf(b, sizeof(b), "MASK teach %u/%u %s", (unsigned)m.teach_got,
                     (unsigned)m.teach_target, mask_skip_short(m.teach_last_reject));
        else
            snprintf(b, sizeof(b), "MASK teach %u/%u", (unsigned)m.teach_got,
                     (unsigned)m.teach_target);
        c = th->highlight;
    } else if (m.hold) {
        snprintf(b, sizeof(b), "FAIL %lu/%lu HOLD-OK", (unsigned long)m.failed,
                 (unsigned long)m.tested);
        c = th->warning;
    } else if (m.last.verdict == MASK_PF_V_SKIP) {
        snprintf(b, sizeof(b), "MASK skip: %s", mask_skip_short(m.last.reason));
        c = th->highlight;
    } else if (m.tested == 0u) {
        snprintf(b, sizeof(b), "MASK ready");
        c = th->success;
    } else {
        snprintf(b, sizeof(b), "P%lu F%lu", (unsigned long)m.passed,
                 (unsigned long)m.failed);
        c = (m.last.verdict == MASK_PF_V_FAIL) ? th->warning : th->success;
    }
    lcd_fill_rect(0, y, LCD_WIDTH, MSTRIP_ROW_H, th->background);
    font_draw_string(x, y + 1, b, c, th->background, &font_small);
}

/* CAN readout: the strip's AUX row while the CAN screen is on (can_scope_screen_on;
 * it wins the row over the mask readout). Below the live band like the mask
 * slot, refreshed from main.c at 5 Hz. Green = every frame acknowledged with
 * a good CRC; warning colour = NAK, CRC failure or bus error. */
void scope_ui_can_row_refresh(void)
{
    const theme_t *th = theme_get();
    char line[56];
    int ok = can_scope_screen_line(line, sizeof(line));
    const uint16_t y = strip_top(scope_state_get());          /* the AUX row */
    lcd_fill_rect(0, y, LCD_WIDTH, MSTRIP_ROW_H, th->background);
    font_draw_string(4, (uint16_t)(y + 1), line,
                     ok ? th->success : th->warning, th->background, &font_small);
}

/* Refresh the mask readout in place. Does nothing while the CAN line or the
 * cursor readout owns the AUX row; blanks the row when the mask is cleared. */
void scope_ui_mask_slot_refresh(void)
{
    if (can_scope_screen_on() || scope_state_get()->cursor.mode != CURSOR_OFF)
        return;                                  /* the CAN line / cursor readout owns the AUX row */
    if (scope_mask_state() == MASK_PF_EMPTY) {
        /* Mask cleared: the AUX row is released. Blank the row it had
         * (the top row a strip WITH aux would have); the live band extends
         * over it on its next frame, the next full repaint redraws the grid. */
        const scope_state_t *ss = scope_state_get();
        const uint16_t y = measure_strip_top(meas_select_get(0), ss->ch1.enabled,
                                             meas_select_get(1), ss->ch2.enabled,
                                             rows_hidden(ss), true, STRIP_BOTTOM);
        lcd_fill_rect(0, y, LCD_WIDTH, MSTRIP_ROW_H, theme_get()->background);
    } else {
        draw_mask_slot(theme_get());
    }
}

/* Freq for one channel, from the spectral estimator, HELD across a short run
 * of refusals. `need` false (no Freq/Peri cell for that channel) skips the
 * estimate and forgets the hold, so a later selection never shows an old
 * reading.
 *
 * Freq is spectral, not edge-counted, and the estimator declines rather than
 * guesses. Edge counting (fs / period in samples) measured +1.6% to +112%
 * against a known drive, because the rising-crossing period estimator finds
 * spurious crossings. Edge counting, autocorrelation and a spectral peak all
 * fail on the same bench records: roughly a fifth to a half of capture
 * records are TORN, with their spectral energy smeared over ~6 bins. One
 * record scored 0.380 coherence whole and 0.999 over its last half. Tearing
 * does not track signal amplitude, so it cannot be screened out in advance;
 * it has to be detected per record, which is what scope_freq does. Against
 * 72 bench captures it answers 54% of the time, is within 3.4% when it does,
 * and is never wrong.
 *
 * The reading is HELD across a short run of refusals so the badge does not
 * strobe at the tearing rate. After FREQ_HOLD_FRAMES consecutive refusals it
 * blanks, because a stale number with no expiry is a wrong number.
 */
static float freq_held(uint8_t ch, bool need, bool have,
                       const volatile uint8_t *buf, float fs)
{
    static float    freq_hz[2];
    static uint16_t freq_stale[2];
    if (ch > 1u)
        return 0.0f;
    if (!need) {
        freq_hz[ch] = 0.0f;
        freq_stale[ch] = FREQ_HOLD_FRAMES;
        return 0.0f;
    }
    /* The acquisition buffer is `volatile` because the acq task fills it.
     * The estimator only reads, and its coherence gate refuses torn records,
     * so an MCU-side tear can produce a refusal but never a wrong number.
     * The generation retry recovers those refusals: a commit landing
     * mid-estimate re-runs the estimate on the settled frame instead of
     * burning a hold frame. */
    _Static_assert(FPGA_ADC_BUF_SIZE == SCOPE_FREQ_MAX_N,
                   "estimator window must match the capture buffer");
    scope_freq_t fr;
    bool fe = false;
    if (have && buf != NULL) {
        for (int attempt = 0; attempt < 3; attempt++) {
            uint32_t g0 = fpga_acq_frame_generation();
            fe = scope_freq_estimate((const uint8_t *)buf, FPGA_ADC_BUF_SIZE, fs, &fr);
            if ((g0 & 1u) == 0u && fpga_acq_frame_generation() == g0)
                break;
        }
    }
    if (fe) {
        freq_hz[ch] = fr.hz;
        freq_stale[ch] = 0u;
    } else if (freq_stale[ch] < FREQ_HOLD_FRAMES) {
        freq_stale[ch]++;
    } else {
        freq_hz[ch] = 0.0f;
    }
    return freq_hz[ch];
}

float   rec_vpc(const scope_state_t *ss, uint8_t ch);

uint8_t rec_zero(const scope_state_t *ss, uint8_t ch);

/*
 * Draw the measurement strip and the AUX row from the live record.
 *
 * refresh true: in place (boxed cells, no row clear, AUX row untouched), for
 * the steady-state update between full frames (scope_ui_strip_refresh).
 *
 * Every value is measured from the live record, in units this instrument can
 * defend (see scope_measure.h):
 *
 *   Vpp / Vrms   VOLTS wherever scope_cal.c has a bench-measured
 *                volts-per-count for that CHANNEL on that range: ranges 4-9,
 *                with 5/6/7 cross-validated three ways and 4/8/9 provisional.
 *                Ranges 0-3 rail on both channels and have no gain, so they
 *                show ADC COUNTS ("Ncnt") rather than an invented voltage.
 *   Duty         a pure ratio, invariant under any affine counts-to-volts
 *                mapping. Asserted in tests/test_scope_measure.c.
 *   Freq         Hz from the SPECTRAL estimator (scope_freq.c) on measured
 *                timebase codes; it declines rather than guess, with a short
 *                hold (freq_held). "--" on unmeasured codes.
 *   Per          SECONDS as 1/Freq while the spectral estimate stands;
 *                otherwise SAMPLES between mid-level crossings, never
 *                converted, because that crossing period is 5-50% out and a
 *                bad number in real units reads as true.
 *
 * Vpp = pp * k and Vrms = ac_rms * k, with k from
 * scope_cal_volts_per_count(ch, vdiv_idx); k == 0 means "show counts". Hz
 * needs the measured rate table in scope_timebase.c and the reg-0x01 code in
 * force. `fpga scope measure` prints the same quantities raw for the bench,
 * from the same sources.
 */
void draw_measurement_badges(const scope_state_t *ss, const theme_t *th, bool refresh)
{
    mstrip_layout_t L;
    strip_layout(ss, &L);   /* long PRM (rows_hidden) = no rows; the AUX row stays */

    if (L.rows > 0u && ss->roll) {
        draw_roll_cells(ss, &L, th, refresh);
    } else if (L.rows > 0u) {
        /* Which channels have cells, and whether any of them is time-based on
         * the spectral estimate (Freq, Peri). */
        bool cells[2] = { false, false }, need_f[2] = { false, false };
        for (unsigned k = 0; k < L.n; k++) {
            cells[L.cell[k].ch] = true;
            if (L.cell[k].prm == SCOPE_PRM_FREQ || L.cell[k].prm == SCOPE_PRM_PERI)
                need_f[L.cell[k].ch] = true;
        }
        scope_measure_t m1, m2;
        const volatile uint8_t *b1 = fpga_get_ch1_buf(), *b2 = fpga_get_ch2_buf();
        const bool have1 = cells[0] && fpga_acq_record_has_ch(0u) && measure_live_channel(b1, &m1);
        const bool have2 = cells[1] && fpga_acq_record_has_ch(1u) && measure_live_channel(b2, &m2);

        /* The 12 measurements, per channel. All arithmetic is in
         * scope_params_compute(): time uses the PUBLISHED record's rate
         * (doubled when interleaved), volts the scope_cal gain with k == 0
         * meaning counts, 0 V the channel's scope_measure_zero_for(). Freq/Per
         * come from the spectral estimate above, with its hold; below four
         * periods from the checked crossing period of a whole record, else
         * "--" (scope_params_compute). A channel with no record refuses
         * all twelve ("--"). */
        const float rec_fs = record_rate_for_display(ss);
        scope_params_t p1, p2;
        scope_prm_ctx_t ctx;
        ctx.sample_rate.hz  = rec_fs;
        ctx.record_whole    = fpga_acq_record_time_ordered();   /* un-rotated: one stretch */
        ctx.volts_per_count.vpc = rec_vpc(ss, 0u);
        ctx.freq_hz         = freq_held(0u, need_f[0], have1, b1, rec_fs);
        ctx.zero_count.v    = rec_zero(ss, 0u);
        scope_params_compute(have1 ? &m1 : NULL, &ctx, &p1);
        ctx.volts_per_count.vpc = rec_vpc(ss, 1u);
        ctx.freq_hz         = freq_held(1u, need_f[1], have2, b2, rec_fs);
        ctx.zero_count.v    = rec_zero(ss, 1u);
        scope_params_compute(have2 ? &m2 : NULL, &ctx, &p2);

        draw_strip_cells(&L, &p1, &p2, th, refresh);
    }

    /* The AUX row: the CAN analyser's line, else the cursor readout, else
     * the mask readout. */
    if (L.aux && !refresh) {
        if (can_scope_screen_on())
            scope_ui_can_row_refresh();
        else if (ss->cursor.mode != CURSOR_OFF)
            cursor_row_paint(ss, true);
        else
            draw_mask_slot(th);
    }
}
