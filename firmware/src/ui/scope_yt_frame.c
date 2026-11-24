/*
 * OpenScope 2C53T - oscilloscope screen: what a Y-T frame is built from
 *
 * The inputs of the Y-T display type (scope_yt.c) that are worked out once
 * per frame: the two vertical maps and their scales for the cursors, the
 * trigger level's row, the edge tags, the mask overlay, persistence, the
 * math trace.
 */

#include "scope_ui_parts.h"

/* The vertical transform of each plotted band as drawn this frame, filled by
 * vband_note(); the cursor readout (scope_cursor_draw.c) converts rows with
 * it. Slot 0 is the first band plotted, slot 1 the second. */
vband_t g_vband[2];

/* The math trace's own scale, as drawn this frame (math_prepare): the
 * cursors read it instead of the channel bands while the math trace is on,
 * since it spans the whole plot over them. */
g_math_cur_t g_math_cur;

/* Forget every recorded scale, so a band not plotted this frame leaves the
 * cursors reporting pixels. */
void vband_clear(void)
{
    g_vband[0].on = false;
    g_vband[1].on = false;
    g_math_cur.on = false;
}

/* Record in g_vband[slot] the map a band was drawn with: channel 1/2, its
 * range, the map a, and the counts one row is worth (0 = nothing plotted).
 * True scale anchors the channel's 0 V code to a row, so absolute Y is known
 * (mid_counts = 0 at the ground tag's row); autofit re-anchors every frame
 * and leaves it unknown. */
void vband_note(uint8_t slot, uint8_t channel, uint8_t range_idx,
                       const autofit_t *a, float counts_per_pixel)
{
    if (slot > 1u) return;
    vband_t *b = &g_vband[slot];
    b->on               = (counts_per_pixel > 0.0f);
    b->y_top            = a->y_top;
    b->y_bot            = a->y_bot;
    b->counts_per_pixel = counts_per_pixel;
    b->channel          = channel;
    b->range_idx        = range_idx;
    b->mid_known        = b->on && a->fx_ch >= 0;
    b->mid_row          = b->mid_known
        ? (int16_t)autofit_y_raw(a, rec_zero(scope_state_get(), (uint8_t)(channel - 1u)))
        : 0;
    b->mid_counts       = 0.0f;
}

/*
 * The row where the HARDWARE trigger fires, through map a.
 *
 * The FPGA compares the ADC against reg 0x08, but the record carries
 * ADC - ADC_REC_OFFSET (28, measured on the unit and equal to stock's ADC
 * offset), so trig_level_rec() turns the register code into a record code.
 * That code goes through the SAME map the trigger-source trace is drawn
 * with, so the line sits on the trace where NORMAL would fire. Outside the
 * band it clamps to the band edge (autofit_y), which reads as "level above /
 * below the signal". In true scale the map is the fixed one (yt_vmap.h), so
 * the line sits at the level's place on the graticule.
 *
 * The code is the one fpga_acq.c records as written (fpga_acq_trig_code_get),
 * not recomputed from the UI level: report the wire, not the intent.
 * a NULL or off (no data): the pixel convention, level in rows above
 * SCOPE_MID_Y.
 */
int16_t trigger_marker_y(const scope_state_t *ss, const autofit_t *a)
{
    if (a == NULL || !a->on)
        return (int16_t)(SCOPE_MID_Y - ss->trigger.level);
    return autofit_y(a, trig_level_rec((adc_raw_t){ fpga_acq_trig_code_get() }).v);
}

/* Both maps off: nothing is plotted (no record). */
void yt_fits_none(autofit_t *a1, autofit_t *a2)
{
    *a1 = *a2 = (autofit_t){ .fx_ch = -1, .span = YT_VMAP_MIN_SPAN };
}

/* The two Y-T maps, the only place that decides them (from the window
 * view_build() last filled into yt_cols). True scale where the range has a
 * calibrated volts/div, over the full plot; autofit elsewhere (an
 * uncalibrated range, or view.truescale off), split into halves when both
 * channels autofit (yt_vmap_split). A channel not drawn gets a map with
 * on == false. While running it also latches the ranges of the record on
 * screen (scope_ui_paused_latch). */
void yt_fits(const scope_state_t *ss, uint16_t band_bot, autofit_t *a1, autofit_t *a2)
{
    autofit_t *a[2] = { a1, a2 };
    /* A channel is drawn only when the published record holds its own
     * samples (fpga_acq_record_has_ch, fpga_acq.c): right after CH2 is
     * switched back on, the record on screen was captured with CH1 alone and
     * its CH2 buffer is the second converter's copy of CH1. The band appears
     * with the first record captured with it. */
    const bool en[2] = { ss->ch1.enabled && fpga_acq_record_has_ch(0u),
                         ss->ch2.enabled && fpga_acq_record_has_ch(1u) };
    const bool fx[2] = {
        en[0] && ss->true_scale && scope_cal_true_scale_ok(1u, ss->ch1.vdiv_idx),
        en[1] && ss->true_scale && scope_cal_true_scale_ok(2u, ss->ch2.vdiv_idx) };
    const bool split = yt_vmap_split(en[0], en[1], fx[0], fx[1]);
    /* While running, keep the ranges of the record on screen latched, so a
     * stop by any route (key, view.run, shell) rescales from the right one. */
    if (ss->running)
        scope_ui_paused_latch();
    yt_fits_none(a1, a2);
    for (uint8_t ch = 0; ch < 2u; ch++) {
        if (fx[ch]) {
            const uint8_t r = ch ? ss->ch2.vdiv_idx : ss->ch1.vdiv_idx;
            yt_vmap_true(a[ch], SCOPE_TOP, SCOPE_BOT, SCOPE_GRID_MID_Y, (int8_t)ch,
                         yt_centre_code(), scope_cal_codes_per_div((uint8_t)(ch + 1u), r));
        } else if (en[ch]) {
            int16_t t, b;
            yt_vmap_band(SCOPE_TOP, (int16_t)band_bot, split, ch, &t, &b);
            autofit_prep(a[ch], yt_cols[ch], t, b, (int8_t)ch);
        }
    }
}

/* Place one tag for level row inside band [top, bot) at column x0: pointing
 * horiz with its label when the row is on the band, up or down on the band
 * edge (no label) when it is off. Off when the band is shorter than a tag. */
static void tag_put(pm_tag_t *t, int row, int top, int bot, pm_dir_t horiz,
                    int x0, pm_label_t label, uint16_t color)
{
    t->on = (bot - top) >= PM_TAG_H;
    if (!t->on)
        return;
    int y0;
    t->dir   = plot_marker_place(row, top, bot, horiz, &y0);
    t->label = (t->dir == horiz) ? label : PM_LABEL_NONE;
    t->x0    = (int16_t)x0;
    t->y0    = (int16_t)y0;
    t->color = color;
}

/* Build the plot-edge tag set m: a ground tag per enabled channel at the
 * left edge, the trigger-level tag at the right edge. a1/a2: the maps as
 * drawn (NULL or off = nothing plotted: the pixel convention, position and
 * level in rows about SCOPE_MID_Y). band_bot: the
 * measurement strip's top. */
void markers_build(const scope_state_t *ss, const theme_t *th,
                          const autofit_t *a1, const autofit_t *a2,
                          int band_bot, pm_set_t *m)
{
    memset(m, 0, sizeof(*m));
    const uint8_t act = active_channel ? 1u : 0u;
    for (uint8_t ch = 0; ch < 2u; ch++) {
        const channel_state_t *c = ch ? &ss->ch2 : &ss->ch1;
        pm_tag_t *t = &m->t[(ch == act) ? 1 : 2];
        if (!c->enabled)
            continue;
        const autofit_t *a = ch ? a2 : a1;
        int top = SCOPE_TOP, bot = band_bot, row;
        if (a != NULL && a->on) {
            top = a->y_top;
            if (a->y_bot < bot) bot = a->y_bot;
            row = autofit_y_raw(a, rec_zero(ss, ch));
        } else {
            row = SCOPE_MID_Y - c->position;
        }
        tag_put(t, row, top, bot, PM_POINT_RIGHT, 0,
                ch ? PM_LABEL_2 : PM_LABEL_1, ch ? th->ch2 : th->ch1);
    }

    /* Trigger level: the record code where the hardware fires
     * (trigger_marker_y), on the trigger source's band. */
    const autofit_t *src = (ss->trigger.source == TRIG_SRC_CH2) ? a2 : a1;
    int top = SCOPE_TOP, bot = band_bot, row;
    if (src != NULL && src->on) {
        row = autofit_y_raw(src, (int)trig_level_rec((adc_raw_t){ fpga_acq_trig_code_get() }).v);
        top = src->y_top;
        if (src->y_bot < bot) bot = src->y_bot;
    } else {
        row = SCOPE_MID_Y - ss->trigger.level;
    }
    tag_put(&m->t[0], row, top, bot, PM_POINT_LEFT, LCD_WIDTH - PM_TAG_W,
            PM_LABEL_T, th->trigger);
}

/* The colour of the tag set at pixel (x, y) into *c, or false.
 * Tags are tried in priority order; a knock-out pixel takes bg. */
bool markers_px(const pm_set_t *m, int x, int y, uint16_t bg,
                              uint16_t *c)
{
    for (int i = 0; i < 3; i++) {
        const pm_tag_t *t = &m->t[i];
        if (!t->on || x < t->x0 || x >= t->x0 + PM_TAG_W ||
            y < t->y0 || y >= t->y0 + PM_TAG_H)
            continue;
        const pm_px_t p = plot_marker_px(t->dir, t->label, x - t->x0, y - t->y0);
        if (p == PM_PX_NONE)
            continue;
        *c = (p == PM_PX_KNOCK) ? bg : t->color;
        return true;
    }
    return false;
}

/* ── Mask overlay ──
 * Paints the pass/fail verdict the acquisition task recorded (scope_mask.c);
 * no analysis on the display path. Failing trace columns take th->warning,
 * the mask bounds are dotted under the traces, and a MASK_STRIP_H-row strip
 * at the bottom of the band shows each column's verdict. */

static bool     g_mask_ov_on;      /* overlay armed for this frame */

bool     g_mask_bounds_on;         /* also draw the mask bounds (mask ready) */

static uint32_t g_mask_ov_gen;     /* the record generation the overlay paints */

/* Arm the overlay for this frame, after view_build(): only on the hardware
 * anchor (g_place.anchor == VIEW_ANCHOR_HW, the alignment the mask was taught on) with a
 * mask present, and only for the record generation now on screen. */
void mask_overlay_begin(void)
{
    /* One column per sample only (g_place.win.L == 1): the mask indexes record
     * SAMPLES by column offset from the anchor (scope_mask_column), which
     * holds only then. A zoomed window or a re-sampled held record draws the
     * trace without the overlay rather than colouring columns off the
     * samples that were judged. */
    g_mask_ov_on = (g_place.anchor == VIEW_ANCHOR_HW) && g_place.trig_x >= 0 &&
                   g_place.win.L == 1u &&
                   scope_mask_state() != MASK_PF_EMPTY;
    /* The bounds depend on the alignment, not on which record was judged,
     * so they are not gated on the generation: gating them would blink them
     * off for the frame between a commit and its verdict. */
    g_mask_bounds_on = g_mask_ov_on && scope_mask_state() == MASK_PF_READY;
    g_mask_ov_gen = fpga_acq_frame_generation();
}

/* No overlay this frame (no record). */
void mask_overlay_off(void)
{
    g_mask_ov_on = false;
    g_mask_bounds_on = false;
}

/* True when channel ch (0/1, or -1 for none) failed the mask at column x. */
bool mask_px_fail(int8_t ch, int x)
{
    return g_mask_ov_on && ch >= 0 &&
           scope_mask_column((uint8_t)ch, x, g_place.trig_x, g_mask_ov_gen) == SCOPE_MASK_COL_FAIL;
}

/* Strip colour for column x from the worse of the two channels' verdicts, or
 * 0 for none; *dotted = paint every other pixel. Green pass, red fail, dotted
 * red the record failed elsewhere, dotted amber refused (SKIP), amber teach. */
uint16_t mask_strip_color(int x, const theme_t *th, bool *dotted)
{
    *dotted = false;
    if (!g_mask_ov_on) return 0;
    scope_mask_col_t a = scope_mask_column(0, x, g_place.trig_x, g_mask_ov_gen);
    scope_mask_col_t b = scope_mask_column(1, x, g_place.trig_x, g_mask_ov_gen);
    scope_mask_col_t k = (a > b) ? a : b;        /* enum order = severity */
    switch (k) {
    case SCOPE_MASK_COL_PASS:       return th->success;
    case SCOPE_MASK_COL_FAIL:       return th->warning;
    case SCOPE_MASK_COL_FAIL_FRAME: *dotted = true; return th->warning;
    case SCOPE_MASK_COL_SKIP:       *dotted = true; return th->highlight;
    case SCOPE_MASK_COL_TEACH:      return th->highlight;
    default:                        return 0;
    }
}

/* ── Persistence ──
 * The bottom layer shows what earlier frames left in the buffer, before this
 * frame's traces go on top; the frame adds its own traces column by column
 * (persist_note), for the next frames. The fade runs on wall time
 * (persist_decay_ms), so 500 ms / 1 s mean that whatever the frame rate. One
 * buffer holds both channels, so a ghost's colour comes from its row
 * (persist_base). */

/* Add one trace column to the persistence buffer: rows prev_y..y at column x
 * (prev_y < 0: first column). Called as the column is painted, so a ghost
 * is exactly where the trace was, through whichever map drew it, and needs
 * no per-column storage (RAM is tight). */
void persist_note(uint16_t x, int16_t prev_y, int16_t y)
{
    if (persist_enabled && persist_is_initialized())
        persist_add_column(x, prev_y < 0 ? -1 : (int)prev_y - SCOPE_TOP,
                           (int)y - SCOPE_TOP);
}

/* Before the ghosts are read, every frame: clear them if the signal-to-screen mapping moved (persist_follow_geometry: one
 * place that sees keys, shell sets, autoset and loads alike), then fade by
 * the wall time since the last call. */
void persist_frame_decay(void)
{
    const scope_state_t *gss = scope_state_get();
    const uint8_t gview = (uint8_t)scope_view;
    (void)persist_follow_geometry(persist_geometry_key(gss, gview, view_merged(gss), strip_top(gss)),
                                  gss->running);
    static uint32_t last_tick;
    static bool     have_tick;
    uint32_t now = (uint32_t)xTaskGetTickCount() * (1000u / configTICK_RATE_HZ);
    persist_decay_ms(have_tick ? (uint32_t)(now - last_tick) : 0u);
    last_tick = now;
    have_tick = true;
}

/* A ghost's base colour at screen row y: the enabled channel's when only one
 * is on; with both on, by band (CH1 above the split row, CH2 below) when the
 * bands are split, neutral when they overlap (one buffer cannot say whose a
 * ghost is). lower: the half below the row that splits the plot
 * (yt_vmap_band_mid of the band the traces were fitted to). */
uint16_t persist_base(const scope_state_t *ss, const theme_t *th, bool lower)
{
    if (!(ss->ch1.enabled && ss->ch2.enabled))
        return ss->ch1.enabled ? th->ch1 : th->ch2;
    const bool split = yt_vmap_split(true, true,
        ss->true_scale && scope_cal_true_scale_ok(1u, ss->ch1.vdiv_idx),
        ss->true_scale && scope_cal_true_scale_ok(2u, ss->ch2.vdiv_idx));
    if (!split) return th->text_secondary;
    return lower ? th->ch2 : th->ch1;
}

/* ── Math trace ──
 * Reads the same 301-column window the traces were drawn from (yt_cols).
 * Volts where the range is calibrated, counts where not. Drawn about the
 * graticule centre on its OWN scale (a 1-2-5 step per division chosen from
 * the result, shown in the label): A*B and A/B share no graticule with A
 * and B. */

bool     g_math_live;    /* a record is drawn this frame (yt_cols is its window) */

/* Prepare one frame of the math trace into mf:
 *   1. ok only with a record drawn this frame and the op's inputs on;
 *      otherwise only the "--" label.
 *   2. the inputs' scales (rec_zero, rec_vpc) and the result's unit;
 *   3. the peak over the window, the 1-2-5 step that fits it, never finer
 *      than the source channel's own division;
 *   4. the cursors' view of that scale (g_math_cur) and the label. */
void math_prepare(const scope_state_t *ss, math_frame_t *mf)
{
    memset(mf, 0, sizeof(*mf));
    mf->op = ((math_op_t)math_op < MATH_COUNT) ? (math_op_t)math_op : MATH_ADD;
    const math_op_t op = mf->op;
    const volatile uint8_t *s1 = fpga_get_ch1_buf();
    const volatile uint8_t *s2 = fpga_get_ch2_buf();
    mf->ok = g_math_live
        && (!math_op_uses_a(op) || (ss->ch1.enabled && s1 != NULL))
        && (!math_op_uses_b(op) || (ss->ch2.enabled && fpga_acq_record_has_ch(1u) && s2 != NULL));
    if (!mf->ok) {
        /* No record, or a channel the op reads is off: say so, draw nothing. */
        snprintf(mf->label, sizeof(mf->label), "M:%s --", math_channel_name(op));
        return;
    }
    mf->in.zero_a = rec_zero(ss, 0u);
    mf->in.zero_b = rec_zero(ss, 1u);
    mf->in.k_a = rec_vpc(ss, 0u);
    mf->in.k_b = rec_vpc(ss, 1u);
    const math_unit_t unit = math_unit_for(op, &mf->in);

    /* The peak over the window, in 64-column chunks (no 301-float scratch). */
    enum { CH = 64 };
    uint8_t a[CH], b[CH];
    float v[CH];
    float peak = 0.0f;
    for (uint16_t x0 = g_view_first; x0 < g_view_end; x0 += CH) {
        const uint16_t nc = (g_view_end - x0 < CH) ? (uint16_t)(g_view_end - x0) : CH;
        for (uint16_t i = 0; i < nc; i++) {
            a[i] = yt_cols[0][x0 + i];
            b[i] = yt_cols[1][x0 + i];
        }
        math_compute(op, a, b, nc, &mf->in, v);
        const float pk = math_peak(v, nc);
        if (pk > peak) peak = pk;
    }

    /* Scale choice keeps its last step while it still fits (no flicker). */
    static int scale_idx = -1;
    static uint8_t scale_key = 0xFFu;
    const uint8_t key = (uint8_t)(op | ((uint8_t)unit << 4));
    if (key != scale_key) { scale_idx = -1; scale_key = key; }
    scale_idx = math_scale_pick(peak, scale_idx);
    /* Never finer than the source channel's own division, so ADC noise on a
     * quiet result (A-B of one signal) stays a flat line, not a full screen. */
    const int floor_idx = math_scale_floor_idx(
        math_step_floor(op, &mf->in, SCOPE_CAL_COUNTS_PER_DIV));
    if (scale_idx < floor_idx) scale_idx = floor_idx;
    mf->step = math_scale_value(scale_idx);

    /* The cursors' view of this scale: `step` per SCOPE_DIV_PX rows. */
    g_math_cur.on = true;
    g_math_cur.direct = (unit == MATH_UNIT_VOLTS)  ? SCOPE_CURSOR_DIRECT_VOLTS
                      : (unit == MATH_UNIT_COUNTS) ? SCOPE_CURSOR_DIRECT_COUNTS
                                                   : SCOPE_CURSOR_DIRECT_NONE;
    g_math_cur.unit_per_px = mf->step / (float)SCOPE_DIV_PX;
    {
        scope_cursor_conf_t c = SCOPE_CURSOR_MEASURED;
        if (math_op_uses_a(op)) {
            scope_cursor_conf_t ca = scope_cursor_cal_conf(1u, ss->ch1.vdiv_idx);
            if (ca < c) c = ca;
        }
        if (math_op_uses_b(op)) {
            scope_cursor_conf_t cb = scope_cursor_cal_conf(2u, ss->ch2.vdiv_idx);
            if (cb < c) c = cb;
        }
        g_math_cur.conf = c;
    }
    char sc[16];
    math_scale_label(scale_idx, unit, sc, sizeof(sc));
    snprintf(mf->label, sizeof(mf->label), "M:%s %s", math_channel_name(op), sc);
}

/* The math trace's row at column x (within [g_view_first, g_view_end)). */
int16_t math_row(const math_frame_t *mf, uint16_t x)
{
    const uint8_t a = yt_cols[0][x], b = yt_cols[1][x];
    float v;
    int16_t row;
    math_compute(mf->op, &a, &b, 1u, &mf->in, &v);
    math_to_rows(&v, 1u, mf->step, SCOPE_DIV_PX, SCOPE_GRID_MID_Y, SCOPE_TOP, SCOPE_BOT, &row);
    return row;
}
