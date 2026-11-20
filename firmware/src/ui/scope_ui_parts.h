/*
 * OpenScope 2C53T - shared between the files of the oscilloscope screen
 *
 * scope_ui.c and the scope_*.c files split from it. Nothing else includes
 * this; the public interface is ui.h.
 */
#ifndef SCOPE_UI_PARTS_H
#define SCOPE_UI_PARTS_H

#include "ui.h"
#include "lcd.h"
#include "font.h"
#include "theme.h"
#include "scope_state.h"
#include "scope_measure.h"
#include "measure_strip.h"
#include "plot_markers.h"
#include "scope_cal.h"
#include "scope_timebase.h"
#include "scope_roll.h"
#include "scope_cursor.h"
#include "scope_freq.h"
#include "scope_geom.h"
#include "yt_vmap.h"
#include "math_channel.h"
#include "paused_view.h"
#include "persistence.h"
#include "fpga.h"
#include "scope_vpos.h"
#include "scope_roll_meas.h"
#include "FreeRTOS.h"   /* pvPortMalloc — the X-Y snapshot buffers */
#include "task.h"       /* xTaskGetTickCount — the out-of-scope popup overlay */
#include "scope_trigger.h"
#include "trig_edge.h"
#include "trig_policy.h"
#include "osc_menu_model.h"
#include "screen.h"            /* screen_is_open: is the Oscilloscope Settings page up */
#include "meas_select.h"
#include "scope_mask.h"
#if FEATURE_FFT
#include "fft_live.h"
#include "fft_source.h"
#include "fft_test_signals.h"
#endif
#include "at32f403a_407.h"  /* GPIO port reads in the debug overlay */
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "can_scope.h"

/* ═══════════════════════════════════════════════════════════════════
 * Layout constants
 * ═══════════════════════════════════════════════════════════════════ */

#define SCOPE_TOP       18      /* Below status bar */

#define SCOPE_BOT       (LCD_HEIGHT - 16)  /* Above info bar */

#define SCOPE_H         (SCOPE_BOT - SCOPE_TOP)

#define SCOPE_MID_Y     (SCOPE_TOP + SCOPE_H / 2)

#define SCOPE_DIV_PX    26      /* graticule division, rows (grid + math scale) */

/* The graticule's centre line: the 4th of the lines ruled every SCOPE_DIV_PX
 * rows from SCOPE_TOP. True scale puts 0 V (position 0) on it. */
#define SCOPE_GRID_MID_Y (SCOPE_TOP + 4 * SCOPE_DIV_PX)

#ifndef SCOPE_PLOT_STOP_BOX
/* The red STOP box on the plot (top centre). UI round 1: R1-B adds a RUN/STOP
 * tab to the status bar; until that is merged this box is the only STOP
 * indicator, so it defaults ON. Build with -DSCOPE_PLOT_STOP_BOX=0 once the
 * bar's tab is in, so no build is ever without one. */
#define SCOPE_PLOT_STOP_BOX 1

#endif
#define STOP_BOX_W      40

#define STOP_BOX_H      16

#define STOP_BOX_X      ((LCD_WIDTH - STOP_BOX_W) / 2)

#define STOP_BOX_Y      (SCOPE_TOP + PM_TPOS_H + 1)   /* under the trigger-position pointer */

/* Bottom of the measurement strip (measure_strip.h anchors its rows there). */
#define STRIP_BOTTOM    SCOPE_BOT

/*
 * How many consecutive refusals the Freq badge holds its last reading for.
 * At the measured ~46% refusal rate a hold of 8 frames makes a dropout
 * visible only when the signal genuinely goes away, while still expiring
 * fast enough that a removed probe blanks the badge within a second.
 */
#define FREQ_HOLD_FRAMES  8u

/* Quick-change popup */
#define POPUP_W         200

#define POPUP_H         36

#define POPUP_X         ((LCD_WIDTH - POPUP_W) / 2)

#define POPUP_Y         ((LCD_HEIGHT - POPUP_H) / 2)

#define yt_cols (ui_draw_scratch.yt)

/* One channel's vertical transform, true scale or autofit (yt_vmap.h). */
typedef yt_vmap_t autofit_t;

#define g_view_first (g_span.first)

#define g_view_end   (g_span.end)

/*
 * What the renderer ACTUALLY did to the vertical axis, recorded per band for
 * the cursor readout.
 *
 * The cursor cannot assume the true-scale transform, because a band
 * may be autofit (a range without calibration, or view.truescale off), and
 * autofit's pixels-per-count is a property of the frame that was drawn — it
 * rescales from the record's own min/max, every frame. Reading the scale off the drawing rather than off an assumption is
 * the same "one copy, two callers" rule that autofit_prep() itself exists to
 * enforce; the two-copies version of that bug (full path vs compositor)
 * already cost this project a week.
 *
 * on == false means "nothing was plotted in this band", which the cursor
 * reports as pixels rather than converting.
 */
typedef struct {
    bool    on;
    int16_t y_top, y_bot;
    float   counts_per_pixel;
    uint8_t channel;            /* 1 or 2 */
    uint8_t range_idx;
    bool    mid_known;          /* mid_row is the channel's 0 V (true scale) */
    int16_t mid_row;
    float   mid_counts;         /* that code relative to 0 V (centred = 0)   */
} vband_t;

/* ═══════════════════════════════════════════════════════════════════
 * Plot-edge markers (plot_markers.h, UI_STUDY S10)
 *
 * Ground (0 V) tag per channel at the left edge, trigger-level tag at the
 * right edge, both on the row where that level is ON THE TRACE: mapped through
 * the same map the band was drawn with (yt_fits), not
 * `SCOPE_MID_Y - position`: the position is already in the record. Off the band, the tag sits on the band edge pointing up or
 * down. Both renderers build the set with markers_build() and ask
 * plot_marker_px() for every pixel, so the live frames keep the tags (before
 * round 1 the compositor painted over them) and draw the same shapes.
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    bool       on;
    pm_dir_t   dir;
    pm_label_t label;
    int16_t    x0, y0;          /* top-left of the 9x9 tag */
    uint16_t   color;
} pm_tag_t;

/* Drawing priority: [0] the trigger tag, [1] the active channel's ground,
 * [2] the other channel's ground (the active one wins an overlap). */
typedef struct { pm_tag_t t[3]; } pm_set_t;

/* Mask bounds for screen column x of channel ch, as two y values through the
 * band's own transform (yfn). Dotted: every other column. */
#define MASK_BOUND_DOT(x) (((x) & 1u) == 0u)

#define MASK_STRIP_H 3u

/* The cursor layer, as both renderers draw it: dashed lines over the live
 * band (the selected one solid) and their V1/V2/H1/H2 labels, transparent
 * text. Built once per frame; cursor_layer_px() is the compositor's form. */
typedef struct {
    bool     v, h;
    int16_t  vx[2], hy[2];
    uint16_t vc[2], hc[2];
    bool     vsolid[2], hsolid[2];
    uint8_t  nlab;
    struct { int16_t x, y, w; char s[3]; uint16_t c; } lab[4];
} cur_layer_t;

/* ═══════════════════════════════════════════════════════════════════
 * Math channel overlay (C12: stock's 8 ops on the real records)
 *
 * Reads the same 301-column window the traces were drawn from (yt_cols,
 * placed by view_build: trigger offset and, at the fast codes, the zoom). Volts where the range is calibrated
 * (scope_cal gain, 0 V = scope_measure_zero_for), counts where not. Drawn
 * about the graticule centre on its OWN scale (a 1-2-5 step per division
 * chosen from the result, shown in the label): A*B and A/B have no common
 * graticule with A and B.
 * ═══════════════════════════════════════════════════════════════════ */

/* One frame of the math trace, shared by the full path and the live frame:
 * the inputs, the 1-2-5 scale chosen from this window, and the label. */
typedef struct {
    bool       ok;
    math_op_t  op;
    math_in_t  in;
    float      step;
    char       label[24];
} math_frame_t;

#define MATH_LABEL_X  130u

/* ═══════════════════════════════════════════════════════════════════
 * Main scope screen compositor
 * ═══════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════
 * X-Y (Lissajous) view — CH1 drives X, CH2 drives Y
 *
 * Plots the two ADC buffers against each other instead of against time:
 * op04 (CH1) -> horizontal, op05 (CH2) -> vertical. The buffers come from
 * one synchronised acquisition and were bench-confirmed time-aligned
 * (~20 deg phase spread, EXP-21 2026-08-21), so (ch1[i], ch2[i]) is a
 * single instant and a Lissajous figure renders coherently.
 *
 * CH2's readout is a dead buffer until its vertical-offset reference
 * (TMR13 CH1 PWM-DAC on PA6) is armed — the same thing `trig2 raw` does
 * from the shell — so this view arms it once on first entry. Square plot
 * area (side = SCOPE_H) so frequency ratios are not distorted.
 *
 * Reads the volatile buffers directly (no tear-check copy): a frame the
 * acq task is mid-updating just displaces a few points, cosmetically
 * negligible here. Consecutive samples are connected so the locus reads as
 * a continuous trace, the way an analog scope draws X-Y.
 * ═══════════════════════════════════════════════════════════════════ */

/* ── Text for the column compositors ─────────────────────────────────
 * A view that composes each column's final pixels cannot draw text over a
 * finished image, so its labels are a small list it asks per pixel
 * (font_text_px). Opaque labels also own their box (glyph gaps in bg). */
#define CTEXT_MAX  14

#define CTEXT_LEN  24

typedef struct {
    uint8_t n;
    struct {
        int16_t  x, y, w;
        uint16_t fg, bg;
        bool     opaque;
        char     s[CTEXT_LEN];
    } t[CTEXT_MAX];
} ctext_set_t;

#if FEATURE_FFT
typedef enum {
    FFT_SRC_NONE = 0,   /* live build, no capture yet: nothing to analyse */
    FFT_SRC_DEMO,       /* synthetic 1 kHz square at FFT_DEMO_SAMPLE_RATE_HZ */
    FFT_SRC_LIVE        /* the active channel's acquisition record, head skipped */
} fft_src_t;

#endif

/* scope_ui.c */
typedef union {
    scope_roll_fifo_t roll;
    uint16_t          waterfall[SHMEM_FFT_WATERFALL_COLS];
    uint8_t           yt[2][SCOPE_GEOM_COLS];
} ui_draw_scratch_t;
extern ui_draw_scratch_t ui_draw_scratch;

/* scope_popup.c */
extern bool     popup_on;
extern bool     popup_hole_last;
bool popup_live(void);
void draw_popup_box(const theme_t *th);
void draw_popup(const theme_t *th);

/* scope_furniture.c */
void draw_run_stop(const scope_state_t *ss, const theme_t *th);
uint16_t strip_top(const scope_state_t *ss);
void strip_layout(const scope_state_t *ss, mstrip_layout_t *L);
void draw_strip_cells(const mstrip_layout_t *L, const scope_params_t *p1,
                             const scope_params_t *p2, const theme_t *th, bool refresh);
void draw_measurement_badges(const scope_state_t *ss, const theme_t *th, bool refresh);

/* scope_view.c */
float record_rate_for_display(const scope_state_t *ss);
bool view_merged(const scope_state_t *ss);
typedef struct { uint16_t first, end; } g_span_t;
extern g_span_t g_span;
float column_rate_for_display(const scope_state_t *ss);
float rec_vpc(const scope_state_t *ss, uint8_t ch);
uint8_t rec_zero(const scope_state_t *ss, uint8_t ch);
void autofit_prep(autofit_t *a, const volatile uint8_t *buf,
                         int16_t y_top, int16_t y_bot, int8_t mask_ch);
int autofit_y_raw(const autofit_t *a, int v);
int16_t autofit_y(const autofit_t *a, uint8_t v);
uint8_t yt_centre_code(void);
int chan_zero_code(const scope_state_t *ss, uint8_t ch);
bool tpos_px(int trig_x, int x, int y);
extern uint8_t  g_trig_anchor;
extern int16_t  g_trig_x_actual;
extern scope_window_t g_view;
uint16_t scope_soft_trigger_offset(const scope_state_t *ss,
                                          const volatile uint8_t *src_buf, uint8_t L,
                                          int16_t *back_q8);
void view_build(const scope_state_t *ss, const volatile uint8_t *b1,
                       const volatile uint8_t *b2);

/* scope_yt.c */
void draw_trigger_line(const theme_t *th, int16_t trig_y);
extern vband_t g_vband[2];
typedef struct {
    bool                on;
    uint8_t             direct;        
    float               unit_per_px;
    scope_cursor_conf_t conf;
} g_math_cur_t;
extern g_math_cur_t g_math_cur;
void vband_clear(void);
void vband_note(uint8_t slot, uint8_t channel, uint8_t range_idx,
                       const autofit_t *a, float counts_per_pixel);
int16_t trigger_marker_y(const scope_state_t *ss, const autofit_t *a);
int16_t trigger_marker_y_full(const scope_state_t *ss);
void yt_fits(const scope_state_t *ss, autofit_t *a1, autofit_t *a2);
void markers_build(const scope_state_t *ss, const theme_t *th,
                          const autofit_t *a1, const autofit_t *a2,
                          int band_bot, pm_set_t *m);
bool markers_px(const pm_set_t *m, int x, int y, uint16_t bg,
                              uint16_t *c);
void draw_plot_markers(const pm_set_t *m, const theme_t *th);
extern bool     g_mask_bounds_on;
void mask_overlay_begin(void);
bool mask_px_fail(int8_t ch, int x);
uint16_t mask_strip_color(int x, const theme_t *th, bool *dotted);
void draw_mask_strip(const theme_t *th, uint16_t band_bot);
extern bool     g_math_live;
void persist_note(uint16_t x, int16_t prev_y, int16_t y);
void persist_frame_decay(void);
uint16_t persist_base(const scope_state_t *ss, const theme_t *th, uint16_t y);
void draw_persistence_overlay(void);
void math_prepare(const scope_state_t *ss, math_frame_t *mf);
int16_t math_row(const math_frame_t *mf, uint16_t x);
void draw_math_waveform(void);

/* scope_cursor_draw.c */
void cursor_layer_build(const scope_state_t *ss, const theme_t *th, cur_layer_t *L);
bool cursor_layer_px(const cur_layer_t *L, int x, int y, uint16_t *c);
void draw_cursors(uint16_t band_bot);
void cursor_row_paint(const scope_state_t *ss, bool force);

/* scope_xy.c */
void ctext_add(ctext_set_t *T, int x, int y, const char *s,
                      uint16_t fg, uint16_t bg, bool opaque);
bool ctext_px(const ctext_set_t *T, int x, int y, uint16_t *c);

/* scope_spectrum.c */
#if FEATURE_FFT
extern bool    waterfall_restart;
fft_src_t fft_prepare_input(int16_t *sbuf, uint16_t *n_out,
                                   float *fs_out, fft_source_t *sel);
float fft_display_ref(const fft_config_t *cfg, const float *data,
                             uint16_t start_bin, uint16_t end_bin);
void fft_note_compose(uint16_t y_top, uint16_t height, const char *msg);
#endif

#endif /* SCOPE_UI_PARTS_H */
