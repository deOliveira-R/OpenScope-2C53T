/*
 * OpenScope 2C53T - oscilloscope screen: the Y-T display type
 *
 * Voltage against time (docs/design/display_architecture.md). One frame is
 * built once from the sources (yt_prepare) and then painted column by column
 * through the canvas (ui/canvas.h): each element of yt_elements writes the
 * pixels it owns into the column's strip, lowest layer first. A full redraw
 * and a live update are the same frame; the full one sends every column, the
 * live one only the columns that changed.
 *
 * What a frame is built from is in scope_view.c (the window of the record)
 * and scope_yt_frame.c (the maps, the tags, the mask overlay, persistence,
 * math).
 */

#include "scope_ui_parts.h"
#include "key_model.h"      /* the job the arrows are on: the trigger line shows it */
#include "canvas.h"
#include "plot_elements.h"

/* Columns sent and skipped by the last frame (`ui dump`). */
volatile uint16_t ui_scope_live_cols_sent, ui_scope_live_cols_skipped;

volatile uint32_t ui_scope_live_send_us;     /* of the frame's time, the part spent sending columns */

#if FEATURE_BENCHSHELL
/* `fpga scope livecheck <frames>`: while live_check_frames is not zero each
 * frame of a record also evaluates every pixel with a per-pixel rule (one
 * chain of tests in z-order, yt_check_column) and counts the pixels where
 * the painted column differs from it; the first difference is kept. */
volatile uint32_t live_check_frames, live_check_pixels, live_check_diffs;

volatile uint16_t live_check_first_x, live_check_first_y, live_check_first_want, live_check_first_got;

#endif
/* Time view_build() took in the last frame of a record, microseconds (`ui dump`). */
volatile uint32_t ui_scope_view_us_last;

/* ── Geometry of the elements that only Y-T has ── */

/* The trigger-level line keeps this many rows clear of the plot's top and
 * bottom edge, so a level off the plot still shows as a line at the edge. */
#define TRIG_LINE_MARGIN_TOP   2
#define TRIG_LINE_MARGIN_BOT   3

/* It stops this many columns short of the right edge. Inherited from the
 * first renderer; the trigger-level tag (plot_markers.h, PM_TAG_W columns at
 * the right edge) is painted over the line's end either way. */
#define TRIG_LINE_RIGHT_GAP    6u

/* Its dots: a column carries one when (x & mask) == 0. Every 2nd column
 * while UP/DOWN move the level, so the user sees which control the arrows
 * own; every 4th otherwise. */
#define TRIG_LINE_DOTS_FOCUS   1u
#define TRIG_LINE_DOTS         3u

/* The math trace's zero tick: this many columns of the centre line, from the
 * left edge. */
#define MATH_ZERO_TICK_W       6u

/* The demo picture, shown until the first record has arrived: a sine on CH1
 * and a square on CH2, in rows about the channel's position (the pixel
 * convention, no volts). Its sizes are decoration. */
#define DEMO_STEPS             64u     /* phase steps in a period (the sine table's length) */
#define DEMO_STEPS_PER_COLUMN  4u      /* 16 columns a period */
#define DEMO_SINE_AMP_ROWS     40      /* amplitude at range 0 ... */
#define DEMO_SINE_AMP_PER_RANGE 2      /* ... less this for each range above it ... */
#define DEMO_SINE_AMP_MIN      10      /* ... and never less than this */
#define DEMO_SQUARE_OFFSET     50      /* the square sits this many rows under the middle */
#define DEMO_SQUARE_AMP_ROWS   25
#define DEMO_LABEL_X           4
#define DEMO_LABEL_DY          14      /* rows under the pane's top */

/* ── One frame ── */

/* A frame's inputs, built once (yt_prepare) and read by every element for
 * every column. p1, p2 and mp carry the previous column's rows from one
 * column to the next, for the connecting spans. */
typedef struct {
    const theme_t          *th;
    const scope_state_t    *ss;
    plot_pane_t  p;                         /* the pane it is drawn in */
    bool         record;                    /* a record is shown (else nothing, or the demo) */
    bool         demo;                      /* no record has ever arrived: the demo picture */
    uint32_t     demo_phase;
    const volatile uint8_t *b1, *b2;        /* the window's columns (yt_cols) */
    autofit_t    a1, a2;                    /* the two maps (yt_fits); off without a record */
    pm_set_t     pm;                        /* the edge tags */
    cur_layer_t  cl;                        /* the cursor layer */
    bool         curs;                      /* it has something to draw */
    math_frame_t mf;
    bool         mth;                       /* math is on */
    int16_t      mlab_w;                    /* width of its label, pixels */
    const uint8_t *pbuf;                    /* persistence intensities, or NULL */
    uint16_t     pbase_top, pbase_bot;      /* ghost colours above and below pbase_mid */
    uint16_t     pbase_mid;                 /* the row that splits the fitted bands */
    /* Columns where a trace sits on a converter rail: one bit per column,
     * per channel (plot_rail_columns). */
    uint8_t      rail[2][PLOT_RAIL_BYTES];
    uint8_t      off_plot;                  /* bit per channel: a column's sample was off the plot this frame */
    int16_t      trig_y;                    /* the trigger line's row */
    uint16_t     trig_dots;                 /* its dot mask */
    int16_t      p1, p2, mp;
} yt_frame_t;

/* What one column shows, worked out once (yt_column) and read by the
 * elements. Index 0 is CH1, 1 is CH2. */
typedef struct {
    bool     trace;                 /* the window has a sample for this column */
    plot_trace_col_t tr[2];         /* the channel's trace here (plot_elements.h) */
    int16_t  bound_lo[2], bound_hi[2];   /* rows of the mask bounds, -1: none */
    uint16_t strip_color;           /* the mask verdict strip's colour here, 0: none */
    bool     strip_dotted;          /* ... on every other pixel only */
    int16_t  mlo, mhi;              /* the math trace's rows; mhi < mlo: none */
    bool     edge;                  /* a column that can carry an edge tag */
    bool     tcol;                  /* ... the trigger-position pointer */
    bool     mcol;                  /* ... the math label */
} yt_col_t;

/* Build the frame: read the sources once and work out everything that does
 * not depend on the column.
 *   1. With a record: the window (view_build), the mask overlay, the maps
 *      (yt_fits) and their scales for the cursor readout (vband_note).
 *      Without one: maps off, so the tags and the trigger line fall back to
 *      the pixel convention; the demo picture only if no record has ever
 *      arrived, so a stall in acquisition never shows a made-up waveform.
 *   2. The edge tags, the cursor layer, the math frame, persistence decay
 *      and colours, the trigger line's row.
 * Changes: yt_cols and the view globals, the cursor readout's scales
 * (vband, g_math_cur), the mask overlay, persistence (decay), the latch that
 * says a record has been seen. frame: the display's frame counter, the
 * demo's phase. */
static void yt_prepare(yt_frame_t *f, const plot_pane_t *p, uint32_t frame)
{
    f->p = *p;
    const theme_t *const th = f->th = theme_get();
    const scope_state_t *const ss = f->ss = scope_state_get();
    const volatile uint8_t *b1 = fpga_get_ch1_buf();
    const volatile uint8_t *b2 = fpga_get_ch2_buf();
    f->record = fpga_data_ready() && b1 != NULL && b2 != NULL;

    /* Forget last frame's vertical scales: a band that is not plotted this
     * frame must leave the cursors reporting pixels, not converting through
     * the scale of a trace that is no longer on the screen. */
    vband_clear();

    if (f->record) {
        scope_mark_acquisition_ready();
        /* The window: placed on the trigger source's crossing and filled into
         * yt_cols for both channels, so the trace holds still and the
         * channels stay time-aligned. A zoomed or re-sampled held record is
         * placed here too. */
        const uint32_t vb0 = DWT->CYCCNT;
        view_build(ss, b1, b2);
        ui_scope_view_us_last = (DWT->CYCCNT - vb0) / (system_core_clock / 1000000u);
        mask_overlay_begin();
        g_math_live = true;            /* yt_cols is this frame's window */
        /* A fitted trace is stretched over the pane's rows above
         * band_bot; a true-scale one keeps the graticule, and anything under
         * band_bot (the measurement strip) covers its last rows. */
        yt_fits(ss, p, &f->a1, &f->a2);
        plot_rail_columns(yt_cols[0], g_view_first, g_view_end, f->rail[0]);
        plot_rail_columns(yt_cols[1], g_view_first, g_view_end, f->rail[1]);
        uint8_t slot = 0;
        if (f->a1.on) vband_note(slot++, 1u, ss->ch1.vdiv_idx, &f->a1, yt_vmap_counts_per_px(&f->a1));
        if (f->a2.on) vband_note(slot, 2u, ss->ch2.vdiv_idx, &f->a2, yt_vmap_counts_per_px(&f->a2));
    } else {
        mask_overlay_off();
        g_math_live = false;
        yt_fits_none(&f->a1, &f->a2);
    }
    f->b1 = yt_cols[0];
    f->b2 = yt_cols[1];

    /* The demo's phase moves with the frame counter while running and holds
     * at the last running frame while stopped. */
    static uint32_t demo_frozen;
    if (ss->running) demo_frozen = frame;
    f->demo = !f->record && !scope_acquisition_ready();
    f->demo_phase = demo_frozen;

    /* The tags, on the same maps. */
    markers_build(ss, th, p, &f->a1, &f->a2, &f->pm);

    cursor_layer_build(ss, th, p, &f->cl);
    f->curs = f->cl.v || f->cl.h;

    /* Math: one row per column, its label as per-pixel text. After
     * vband_clear, which also forgets the math scale math_prepare sets. */
    f->mth = math_enabled;
    if (f->mth) math_prepare(ss, p, &f->mf);
    f->mlab_w = f->mth ? (int16_t)font_string_width(f->mf.label, &font_small) : 0;

    /* Persistence: the ghosts are read from the shared-pool buffer
     * (PERSIST_WIDTH x PERSIST_HEIGHT intensities, not .bss), decayed once
     * per frame; this frame's traces are added to it column by column after
     * the column's ghosts were read (yt_paint_column). */
    f->pbuf = (persist_enabled && persist_is_initialized()) ? persist_get_buffer() : NULL;
    if (f->pbuf) persist_frame_decay(p);
    f->pbase_top = persist_base(ss, th, false);
    f->pbase_bot = persist_base(ss, th, true);
    f->pbase_mid = (uint16_t)yt_vmap_band_mid(p->top, p->band_bot);

    /* The trigger line: on the trigger source's map, at the record code
     * where the hardware fires (trigger_marker_y). */
    f->trig_y = trigger_marker_y(ss, p, (ss->trigger.source == TRIG_SRC_CH2) ? &f->a2 : &f->a1);
    if (f->trig_y < p->top + TRIG_LINE_MARGIN_TOP) f->trig_y = (int16_t)(p->top + TRIG_LINE_MARGIN_TOP);
    if (f->trig_y > p->bot - TRIG_LINE_MARGIN_BOT) f->trig_y = (int16_t)(p->bot - TRIG_LINE_MARGIN_BOT);
    f->trig_dots = (key_focus_get() == KF_TRIGMOVE) ? TRIG_LINE_DOTS_FOCUS : TRIG_LINE_DOTS;

    f->p1 = f->p2 = f->mp = 0;
    f->off_plot = 0;
}

/* Work out column x into c, and advance the frame's previous-column rows. */
static void yt_column(yt_frame_t *f, uint16_t x, yt_col_t *c)
{
    const theme_t *const th = f->th;

    /* The trace lives in the graticule's 301 columns (scope_geom.h); right of
     * them a column carries only the trigger line, the tags, the cursors and
     * background. Inside, a re-sampled held record may not reach every
     * column: [g_view_first, g_view_end) is what view_build filled. */
    c->trace = f->record && x >= g_view_first && x < g_view_end;
    const bool joined = (x > g_view_first);     /* no span from a blank column */

    /* The traces (plot_trace_col). The band that can be seen ends where the
     * measurement strip starts. A column outside the mask is drawn in the
     * warning colour, span and all. */
    const autofit_t *const a[2] = { &f->a1, &f->a2 };
    const volatile uint8_t *const b[2] = { f->b1, f->b2 };
    int16_t *const prev[2] = { &f->p1, &f->p2 };
    const uint16_t ch_color[2] = { th->ch1, th->ch2 };
    for (uint8_t ch = 0; ch < 2u; ch++) {
        plot_trace_col_t *const t = &c->tr[ch];
        t->on = a[ch]->on && c->trace;
        if (t->on) {
            plot_trace_col(t, a[ch], autofit_y_raw(a[ch], b[ch][x]), joined, *prev[ch], (int)f->p.band_bot,
                           plot_rail_at(f->rail[ch], x),
                           mask_px_fail((int8_t)ch, (int)x) ? th->warning : ch_color[ch], th);
            *prev[ch] = t->y;
            if (t->off_plot) f->off_plot |= (uint8_t)(1u << ch);
        }
        /* The mask's bounds are dotted (every other column), through the
         * channel's own map. */
        c->bound_lo[ch] = c->bound_hi[ch] = -1;
        uint8_t lo, hi;
        if (g_mask_bounds_on && MASK_BOUND_DOT(x) && t->on &&
            scope_mask_bound_at(ch, (int)x, g_place.trig_x, &lo, &hi)) {
            c->bound_lo[ch] = autofit_y(a[ch], lo);
            c->bound_hi[ch] = autofit_y(a[ch], hi);
        }
    }

    /* The verdicts are written by the acquisition task: read once per
     * column, here, so everything that uses one sees the same. */
    c->strip_dotted = false;
    c->strip_color = mask_strip_color((int)x, th, &c->strip_dotted);

    /* Math: this column's row and the span from the previous one. */
    c->mlo = -1;
    c->mhi = -2;
    if (f->mth && f->mf.ok && c->trace) {
        const int16_t r = math_row(&f->mf, x);
        c->mlo = c->mhi = r;
        if (joined) {
            if (f->mp < c->mlo) c->mlo = f->mp;
            if (f->mp > c->mhi) c->mhi = f->mp;
        }
        f->mp = r;
    }

    /* Only the edge columns can carry a tag, only the PM_TPOS_W columns
     * round the trigger point the pointer, only the label's columns the math
     * label: their per-pixel shape tests run inside these boxes only. */
    c->edge = (x < PM_TAG_W) || (x >= LCD_WIDTH - PM_TAG_W);
    c->tcol = g_place.trig_x >= 0 &&
              (int)x + PM_TPOS_W / 2 >= (int)g_place.trig_x &&
              (int)x <= (int)g_place.trig_x + PM_TPOS_W / 2;
    c->mcol = f->mth && (int)x >= (int)MATH_LABEL_X && (int)x < (int)MATH_LABEL_X + f->mlab_w;
}

/* The colour of a persistence ghost of intensity in (not zero) at screen row
 * y, or bg when it scales to black. */
static inline uint16_t yt_ghost(uint8_t in, uint16_t y, uint16_t mid, uint16_t base_top, uint16_t base_bot, uint16_t bg)
{
    const uint16_t g = persist_scale_rgb565((y < mid) ? base_top : base_bot, in);
    return (g != 0x0000) ? g : bg;
}

/* ── The elements ──
 * Each paints what it owns of column x into strip s (rows s->lo up to, not
 * including, s->hi) and leaves every other pixel alone. yt_elements lists
 * them lowest layer first, so a later one covers an earlier one. */

typedef void (*yt_element_fn)(const yt_frame_t *f, const yt_col_t *c, uint16_t x,
                              const canvas_strip_t *s);

/* Background and persistence ghosts: every pixel of the strip. */
static void el_ground(const yt_frame_t *f, const yt_col_t *c, uint16_t x, const canvas_strip_t *s)
{
    (void)c;
    const uint16_t rows = (uint16_t)(s->hi - s->lo);
    const uint16_t bg = f->th->background;
    uint16_t *const px = s->px;
    if (!f->pbuf) {
        for (uint16_t i = 0; i < rows; i++) px[i] = bg;
        return;
    }
    /* The frame's values in locals: the strip is written through a pointer
     * the compiler must assume could point at them. */
    const uint8_t *in = &f->pbuf[(s->lo - f->p.top) * PERSIST_WIDTH + x];
    const uint16_t base_top = f->pbase_top, base_bot = f->pbase_bot, mid = f->pbase_mid;
    for (uint16_t i = 0; i < rows; i++, in += PERSIST_WIDTH)
        px[i] = *in ? yt_ghost(*in, (uint16_t)(s->lo + i), mid, base_top, base_bot, bg) : bg;
}

/* The graticule (plot_elements.h). */
static void el_grid(const yt_frame_t *f, const yt_col_t *c, uint16_t x, const canvas_strip_t *s)
{
    (void)c;
    plot_grid_paint(f->th, &f->p, x, s);
}

/* The trigger level, a dotted line across the plot. */
static void el_trigger_line(const yt_frame_t *f, const yt_col_t *c, uint16_t x, const canvas_strip_t *s)
{
    (void)c;
    if ((x & f->trig_dots) == 0 && x < LCD_WIDTH - TRIG_LINE_RIGHT_GAP)
        strip_put(s, f->trig_y, f->th->trigger);
}

/* The mask's bounds, under the traces. */
static void el_mask_bounds(const yt_frame_t *f, const yt_col_t *c, uint16_t x, const canvas_strip_t *s)
{
    (void)x;
    for (uint8_t ch = 0; ch < 2u; ch++) {
        if (c->bound_lo[ch] >= 0) strip_put(s, c->bound_lo[ch], f->th->text_secondary);
        if (c->bound_hi[ch] >= 0) strip_put(s, c->bound_hi[ch], f->th->text_secondary);
    }
}

/* The traces: CH1, then CH2 over it. */
static void el_traces(const yt_frame_t *f, const yt_col_t *c, uint16_t x, const canvas_strip_t *s)
{
    (void)f; (void)x;
    for (uint8_t ch = 0; ch < 2u; ch++) plot_trace_paint(&c->tr[ch], s);
}

/* The demo picture and its "DEMO" mark, in the warning colour beside the
 * trace: the waveform is what catches the eye and it is the thing that is
 * not real. */
static void el_demo(const yt_frame_t *f, const yt_col_t *c, uint16_t x, const canvas_strip_t *s)
{
    (void)c;
    if (!f->demo) return;
    static const int8_t sine_percent[DEMO_STEPS] = {
         0, 10, 19, 29, 38, 47, 56, 63, 71, 77, 83, 88, 92, 96, 98, 99,
        100, 99, 98, 96, 92, 88, 83, 77, 71, 63, 56, 47, 38, 29, 19, 10,
         0,-10,-19,-29,-38,-47,-56,-63,-71,-77,-83,-88,-92,-96,-98,-99,
       -100,-99,-98,-96,-92,-88,-83,-77,-71,-63,-56,-47,-38,-29,-19,-10,
    };
    const theme_t *const th = f->th;
    const scope_state_t *const ss = f->ss;
    static const char label[] = "DEMO";
    const int lx = (int)x - DEMO_LABEL_X;
    if (lx >= 0 && lx < (int)font_string_width(label, &font_small))
        for (int dy = 0; dy < (int)font_small.height; dy++)
            if (font_text_px(label, &font_small, lx, dy))
                strip_put(s, f->p.top + DEMO_LABEL_DY + dy, th->warning);

    const uint8_t phase = (uint8_t)((x * DEMO_STEPS_PER_COLUMN + f->demo_phase) % DEMO_STEPS);
    if (ss->ch1.enabled) {              /* a sine, smaller at higher range indexes */
        int amp = DEMO_SINE_AMP_ROWS - (int)ss->ch1.vdiv_idx * DEMO_SINE_AMP_PER_RANGE;
        if (amp < DEMO_SINE_AMP_MIN) amp = DEMO_SINE_AMP_MIN;
        const int y = f->p.mid - ss->ch1.position - sine_percent[phase] * amp / 100;
        if (y >= s->lo) strip_span(s, y, y + SCOPE_TRACE_ROWS - 1, th->ch1);
    }
    if (ss->ch2.enabled) {              /* a square, its edges joined */
        const int mid = f->p.mid + DEMO_SQUARE_OFFSET - ss->ch2.position;
        const bool high = phase < DEMO_STEPS / 2u;
        strip_put(s, mid + (high ? -DEMO_SQUARE_AMP_ROWS : DEMO_SQUARE_AMP_ROWS), th->ch2);
        if (x > 0) {
            const uint8_t before = (uint8_t)(((x - 1u) * DEMO_STEPS_PER_COLUMN + f->demo_phase) % DEMO_STEPS);
            if ((before < DEMO_STEPS / 2u) != high)
                strip_span(s, mid - DEMO_SQUARE_AMP_ROWS, mid + DEMO_SQUARE_AMP_ROWS, th->ch2);
        }
    }
}

/* The mask verdict of this column: MASK_STRIP_H rows at the band's bottom. */
static void el_mask_strip(const yt_frame_t *f, const yt_col_t *c, uint16_t x, const canvas_strip_t *s)
{
    (void)f;
    if (!c->strip_color) return;
    for (int y = s->hi - (int)MASK_STRIP_H; y < s->hi; y++)
        if (!c->strip_dotted || ((x + (unsigned)y) & 1u) == 0u) strip_put(s, y, c->strip_color);
}

/* The math trace, its zero tick at the left edge and its label. */
static void el_math(const yt_frame_t *f, const yt_col_t *c, uint16_t x, const canvas_strip_t *s)
{
    if (!f->mth) return;
    const uint16_t color = f->th->warning;
    if (f->mf.ok && x < MATH_ZERO_TICK_W) strip_put(s, f->p.grid_mid, color);
    if (c->mhi >= c->mlo) strip_span(s, c->mlo, c->mhi, color);
    if (c->mcol)
        for (int dy = 0; dy < (int)font_small.height; dy++)
            if (font_text_px(f->mf.label, &font_small, (int)x - (int)MATH_LABEL_X, dy))
                strip_put(s, f->p.top + dy, color);
}

/* The ground tags (left edge) and the trigger-level tag (right edge). */
static void el_tags(const yt_frame_t *f, const yt_col_t *c, uint16_t x, const canvas_strip_t *s)
{
    if (!c->edge) return;
    for (int y = s->lo; y < s->hi; y++) {
        uint16_t color;
        if (markers_px(&f->pm, (int)x, y, f->th->background, &color)) strip_put(s, y, color);
    }
}

/* The pointer over the column where the trigger point landed
 * (g_place.trig_x), which the soft-trigger fallback may put elsewhere than
 * it was asked. */
static void el_trigger_pointer(const yt_frame_t *f, const yt_col_t *c, uint16_t x, const canvas_strip_t *s)
{
    if (!c->tcol) return;
    for (int y = f->p.top; y < f->p.top + PM_TPOS_H; y++)
        if (tpos_px(g_place.trig_x, (int)x, y - f->p.top)) strip_put(s, y, f->th->trigger);
}

/* Cursor lines and their labels, over everything (the readout is in the
 * measurement strip's AUX row). */
static void el_cursors(const yt_frame_t *f, const yt_col_t *c, uint16_t x, const canvas_strip_t *s)
{
    (void)c;
    if (f->curs) cursor_layer_paint(&f->cl, x, s);
}

/* What Y-T draws, lowest layer first. */
static const yt_element_fn yt_elements[] = {
    el_ground,
    el_grid,
    el_trigger_line,
    el_mask_bounds,
    el_traces,
    el_demo,
    el_mask_strip,
    el_math,
    el_tags,
    el_trigger_pointer,
    el_cursors,
};

#if FEATURE_BENCHSHELL
/* `fpga scope livecheck`: every pixel of column x against the per-pixel
 * rule, one chain of tests from the top layer down in the elements' order.
 * It checks that the elements, each painting only the rows it can occupy,
 * give what the rule gives; the rule is written here a second time on
 * purpose. Counts the pixels that differ; draws nothing. The demo picture is
 * not part of the rule (called for a record only). */
static void yt_check_column(const yt_frame_t *f, const yt_col_t *c, uint16_t x, const canvas_strip_t *s)
{
    const theme_t *const th = f->th;
    const uint16_t mstrip = c->strip_color;
    const bool mdot = c->strip_dotted;
    for (uint16_t y = (uint16_t)s->lo; y < (uint16_t)s->hi; y++) {
        uint16_t want;
        if (f->curs && cursor_layer_px(&f->cl, (int)x, (int)y, &want))
            ;                                      /* cursor lines and labels */
        else if (c->tcol && y < f->p.top + PM_TPOS_H && tpos_px(g_place.trig_x, (int)x, (int)y - f->p.top))
            want = th->trigger;                    /* trigger-position pointer */
        else if (c->edge && markers_px(&f->pm, (int)x, (int)y, th->background, &want))
            ;                                      /* ground / trigger tags */
        else if (c->mcol && y < f->p.top + font_small.height &&
                 font_text_px(f->mf.label, &font_small, (int)x - (int)MATH_LABEL_X, (int)y - f->p.top))
            want = th->warning;                    /* math label */
        else if ((int16_t)y >= c->mlo && (int16_t)y <= c->mhi)
            want = th->warning;                    /* math trace */
        else if (f->mth && f->mf.ok && x < MATH_ZERO_TICK_W && (int16_t)y == f->p.grid_mid)
            want = th->warning;                    /* math zero tick */
        else if (mstrip && y >= (uint16_t)s->hi - MASK_STRIP_H && (!mdot || ((x + y) & 1u) == 0u))
            want = mstrip;
        else if (plot_trace_px(&c->tr[1], (int)y, &want))
            ;                                      /* CH2 */
        else if (plot_trace_px(&c->tr[0], (int)y, &want))
            ;                                      /* CH1 under it */
        else if ((int16_t)y == c->bound_lo[0] || (int16_t)y == c->bound_hi[0] ||
                 (int16_t)y == c->bound_lo[1] || (int16_t)y == c->bound_hi[1])
            want = th->text_secondary;
        else if ((int16_t)y == f->trig_y && (x & f->trig_dots) == 0 && x < LCD_WIDTH - TRIG_LINE_RIGHT_GAP)
            want = th->trigger;
        else if (plot_grid_px(th, &f->p, x, (int)y, &want))
            ;                                      /* graticule */
        else {
            const uint8_t in = f->pbuf ? f->pbuf[(y - f->p.top) * PERSIST_WIDTH + x] : 0u;
            want = in ? yt_ghost(in, y, f->pbase_mid, f->pbase_top, f->pbase_bot, th->background) : th->background;
        }
        const uint16_t got = s->px[y - (uint16_t)s->lo];
        live_check_pixels++;
        if (want != got && live_check_diffs++ == 0) {
            live_check_first_x = x; live_check_first_y = y;
            live_check_first_want = want; live_check_first_got = got;
        }
    }
}
#endif

/* Paint column x of the band into s: the canvas's paint function. */
static void yt_paint_column(void *ctx, uint16_t x, const canvas_strip_t *s)
{
    yt_frame_t *const f = ctx;
    yt_col_t c;
    yt_column(f, x, &c);
    for (unsigned i = 0; i < sizeof(yt_elements) / sizeof(yt_elements[0]); i++)
        yt_elements[i](f, &c, x, s);
#if FEATURE_BENCHSHELL
    if (live_check_frames && f->record) yt_check_column(f, &c, x, s);
#endif
    /* Feed persistence with this column's traces, now that its ghosts have
     * been read: a ghost is where the trace was, through the map that drew
     * it. */
    if (f->pbuf && c.trace)
        for (uint8_t ch = 0; ch < 2u; ch++)
            if (c.tr[ch].on) persist_note(&f->p, x, c.tr[ch].from, c.tr[ch].y);
}

/* Draw the Y-T plot into pane p: one canvas frame in column strips over
 * the pane's rows down to its band_bot, all LCD_WIDTH columns, on the pane's
 * canvas. all: send every column (a full redraw); otherwise only the columns
 * whose pixels changed. The glass never shows a blank band either way. The
 * popup box is a hole of the frame, not a layer: its glyphs are the one
 * thing the elements cannot recompute per pixel, so the canvas steps around
 * it, and when the popup ends the hole closes and the next frame paints the
 * band back. Not drawn here: the measurement strip and the popup box itself.
 * Changes: what yt_prepare changes, the persistence buffer (feed),
 * popup_hole_last and the `ui dump` counters. */
void scope_yt_draw(const plot_pane_t *p, bool all, uint32_t frame)
{
    yt_frame_t f;
    yt_prepare(&f, p, frame);

    const canvas_rect_t popup = { POPUP_X, POPUP_Y, POPUP_W, POPUP_H };
    const bool hole = popup_live() && POPUP_Y < p->band_bot && POPUP_Y + POPUP_H > p->top;
    popup_hole_last = hole;         /* the box has rows inside the band and was left alone */

    const canvas_frame_t cf = {
        .id = p->canvas,
        .rect = { 0, (uint16_t)p->top, LCD_WIDTH, (uint16_t)(p->band_bot - p->top) },
        .holes = &popup, .nholes = hole ? 1u : 0u,
        .all = all,
        .paint = yt_paint_column, .ctx = &f,
    };
    const uint32_t send0 = lcd_perf_column_cycles;
    const uint16_t cols_sent = canvas_draw_columns(&cf);

    uint8_t at_limit = 0;
    for (uint8_t ch = 0; ch < 2u; ch++)
        for (unsigned i = 0; f.record && i < PLOT_RAIL_BYTES; i++)
            if (f.rail[ch][i]) at_limit |= (uint8_t)(1u << ch);
    scope_ui_plot_clip_set(at_limit, f.off_plot);
    ui_scope_live_cols_sent = cols_sent;
    ui_scope_live_send_us = (lcd_perf_column_cycles - send0) / (system_core_clock / 1000000u);
    ui_scope_live_cols_skipped = (uint16_t)(LCD_WIDTH - cols_sent);

#if FEATURE_BENCHSHELL
    if (live_check_frames) live_check_frames--;
#endif
}
