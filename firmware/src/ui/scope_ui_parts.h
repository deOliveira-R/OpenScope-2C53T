/*
 * OpenScope 2C53T - shared between the files of the oscilloscope screen
 *
 * The layout constants, shared types and cross-file symbols of scope_ui.c
 * and the scope_*.c drawing files (scope_view, scope_yt, scope_yt_frame,
 * scope_cursor_draw, scope_xy, scope_spectrum,
 * scope_waterfall, scope_furniture, scope_popup). Nothing else includes this; the public
 * interface is ui.h.
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
#include "plot_pane.h"
#include "label_place.h"
#include "math_channel.h"
#include "paused_view.h"
#include "persistence.h"
#include "fpga.h"
#include "scope_vpos.h"
#include "scope_roll_meas.h"
#include "FreeRTOS.h"   /* pvPortMalloc: the X-Y snapshot buffers */
#include "at32f403a_407.h"  /* the cycle counter (DWT) and system_core_clock, for the frame timing */
#include "ui_perf.h"       /* UI_TIMED: the transform's time for `ui dump` */
#include "task.h"       /* xTaskGetTickCount: popup timeouts, persistence fade */
#include "scope_trigger.h"
#include "trig_edge.h"
#include "trig_policy.h"
#include "osc_menu_model.h"
#include "screen.h"            /* screen_is_open: is the Oscilloscope Settings page up */
#include "meas_select.h"
#include "scope_mask.h"
#include "fft_live.h"
#include "fft_source.h"
#include "fft_test_signals.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "can_scope.h"

/* ── Layout constants ── */

#define SCOPE_TOP       18      /* Below status bar */

#define SCOPE_BOT       (LCD_HEIGHT - 16)  /* Above info bar */

#define SCOPE_H         (SCOPE_BOT - SCOPE_TOP)    /* plot height, rows */

#define SCOPE_MID_Y     (SCOPE_TOP + SCOPE_H / 2)  /* plot middle row; the pixel convention's 0 */

#define SCOPE_DIV_PX    PLOT_PANE_DIV_PX(SCOPE_H)  /* the full plot's graticule division: 26 rows */

/* A trace is this many rows thick: the sample's row and the rows under it
 * (Y-T, roll and the demo picture). */
#define SCOPE_TRACE_ROWS 2

/* The graticule's centre line: the 4th of the lines ruled every SCOPE_DIV_PX
 * rows from SCOPE_TOP. True scale puts 0 V (position 0) on it. */
#define SCOPE_GRID_MID_Y (SCOPE_TOP + 4 * SCOPE_DIV_PX)

/* Bottom of the measurement strip (measure_strip.h anchors its rows there). */
#define STRIP_BOTTOM    SCOPE_BOT

/*
 * How many consecutive refusals the Freq badge holds its last reading for.
 * At the measured ~46% refusal rate a hold of 8 frames makes a dropout
 * visible only when the signal genuinely goes away, while still expiring
 * fast enough that a removed probe blanks the badge within a second.
 */
#define FREQ_HOLD_FRAMES  8u

/* Quick-change popup box, centred on the screen */
#define POPUP_W         200

#define POPUP_H         36

#define POPUP_X         ((LCD_WIDTH - POPUP_W) / 2)

#define POPUP_Y         ((LCD_HEIGHT - POPUP_H) / 2)

/* The Y-T window, [channel][column]: the record code each of the 301 plot
 * columns shows this frame (view_build fills it). */
#define yt_cols (ui_draw_scratch.yt)

/* One channel's vertical transform, true scale or autofit (yt_vmap.h). */
typedef yt_vmap_t autofit_t;

/* The columns of yt_cols that hold data; the rest of the plot stays blank. */
#define g_view_first (g_span.first)

#define g_view_end   (g_span.end)

/*
 * The vertical map each channel's band was drawn with this frame, and what
 * its codes are worth, for the cursor lines (scope_cursor_draw.c). A level
 * line is put on the active channel's band through the very map its trace
 * was drawn with, true scale or a fit alike (yt_fits); a fit rescales every
 * frame, so the map is read off the drawing, never assumed.
 *
 * on == false: the channel's band was not drawn this frame.
 */
typedef struct {
    bool                on;
    yt_vmap_t           map;
    float               zero_code;        /* 0 V in the map's codes */
    float               volts_per_code;   /* at the probe tip; 0 = the range has no volts */
    scope_cursor_conf_t conf;             /* the gain's tier */
    int16_t             row_end;          /* the band's last visible row + 1 (the strip covers the rest) */
} vband_t;

/* ── Plot-edge markers (plot_markers.h) ──
 * Ground (0 V) tag per channel at the left edge, trigger-level tag at the
 * right edge, both on the row where that level is ON THE TRACE: mapped
 * through the same map the band was drawn with (yt_fits), not
 * `SCOPE_MID_Y - position`, since the position is already in the record.
 * Off the band, the tag sits on the band edge pointing up or down. Both
 * renderers build the set with markers_build() and ask plot_marker_px() for
 * every pixel, so they draw the same shapes. */

/* One placed tag. */
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

/* The mask bounds are drawn as two rows per column through the band's own
 * map, dotted: only on the columns where this is true. */
#define MASK_BOUND_DOT(x) (((x) & 1u) == 0u)

/* Height of the mask verdict strip at the bottom of the band, rows. */
#define MASK_STRIP_H 3u

/* The cursor layer: dashed lines over the band (the selected one solid) and
 * their V1/V2/H1/H2 labels, transparent text. Built once per frame
 * (cursor_layer_build) and read per pixel (cursor_layer_px). */
#define CUR_LABELS_MAX  4       /* V1, V2, H1, H2 */
typedef struct {
    bool     v, h;
    int16_t  vx[2], hy[2];
    uint16_t vc[2], hc[2];
    bool     vsolid[2], hsolid[2];
    uint8_t  nlab;
    struct { int16_t x, y, w; char s[3]; uint16_t c; } lab[CUR_LABELS_MAX];
} cur_layer_t;

/* ── Math trace (stock's 8 ops on the records; scope_yt_frame.c) ── */

/* One frame of the math trace (math_prepare): the inputs, the 1-2-5 step per division chosen from this
 * window, and the label. */
typedef struct {
    bool       ok;
    math_op_t  op;
    math_in_t  in;
    float      step;
    plot_pane_t pane;           /* the pane it is drawn in: step per division about its centre line */
    char       label[24];
} math_frame_t;

#define MATH_LABEL_X  130u      /* math label's column, top row of the plot */

/* ── Text for the column-composed views ──
 * A view that composes each column's final pixels cannot draw text over a
 * finished image, so its labels are a small list it asks per pixel
 * (font_text_px). Opaque labels also own their box (glyph gaps in bg). */
#define CTEXT_MAX  14           /* labels per set */

#define CTEXT_LEN  24           /* bytes per label with the NUL; longer ones are cut */

typedef struct {
    uint8_t     n;
    label_box_t box[CTEXT_MAX];     /* each label's box: the ones a later label must not meet */
    struct {
        uint16_t fg, bg;
        bool     opaque;
        char     s[CTEXT_LEN];
    } t[CTEXT_MAX];
} ctext_set_t;

/* Where the spectrum's input came from (fft_prepare_input). */
typedef enum {
    FFT_SRC_NONE = 0,   /* nothing to analyse: no capture yet, or the channel is off */
    FFT_SRC_DEMO,       /* synthetic 1 kHz square at FFT_DEMO_SAMPLE_RATE_HZ (emulator) */
    FFT_SRC_LIVE        /* the selected channel's record, head skipped */
} fft_src_t;

/* scope_ui.c */
/* Display-task scratch for draw paths that never run at the same time. */
typedef union {
    uint16_t          waterfall[SHMEM_FFT_WATERFALL_COLS];
    uint8_t           yt[2][SCOPE_GEOM_COLS];
} ui_draw_scratch_t;
/* The one instance of that scratch. */
extern ui_draw_scratch_t ui_draw_scratch;

/* scope_ui.c */
/* The time view's pane when it fills the plot: SCOPE_TOP to SCOPE_BOT, its
 * traces ending at the measurement strip's top, on the plot's canvas. */
plot_pane_t scope_pane_full(const scope_state_t *ss);
/* A time view's frame reports the channels it drew at the converter's limit
 * and off the plot (bit 0 CH1, bit 1 CH2). */
void scope_ui_plot_clip_set(uint8_t at_limit, uint8_t off_plot);

/* scope_popup.c */
/* A plot popup is set (stays set until a frame is drawn without the box). */
extern bool     popup_on;
/* The last frame left the popup's rectangle untouched (a hole). */
extern bool     popup_hole_last;
/* The plot popup is set and its time has not run out. */
bool popup_live(void);
/* A frame of rows [top, bot) keeps a hole for the box (popup_live, and the
 * box has rows there). The view's renderer, which knows all its panes, sets
 * popup_hole_last from its panes' answers. */
bool popup_hole_in(int top, int bot);
/* Paint the popup box with its text. */
void draw_popup_box(const theme_t *th);
/* After a full frame: paint the box if the frame kept its hole, else end
 * the popup. */
void draw_popup(const theme_t *th);

/* scope_furniture.c */
/* Top row of the measurement strip: the live band ends here. */
uint16_t strip_top(const scope_state_t *ss);
/* The measurement strip's rows and cells for the current state. */
void strip_layout(const scope_state_t *ss, mstrip_layout_t *L);
/* Paint the strip's cells; refresh repaints in place without clearing. */
void draw_strip_cells(const mstrip_layout_t *L, const scope_params_t *p1,
                             const scope_params_t *p2, const theme_t *th, bool refresh);
/* The spectrum's strip: its top row, and its cells from the spectrum's
 * measurements (NULL: none). */
uint16_t fft_strip_top(const scope_state_t *ss);
void draw_fft_strip(const scope_state_t *ss, const theme_t *th, const fft_meas_t *m, bool ch2, bool refresh);

/* scope_spectrum.c */
/* What the furniture of a spectrum needs of the frame that was drawn. */
typedef struct {
    fft_meas_t meas[FFT_MEAS_COUNT];
    bool       ch2;                         /* the spectrum is CH2's */
    fft_axes_t ax;
} fft_drawn_t;
/* Draw the spectrum into pane p through its canvas: every column (all) or
 * only the columns that changed; *drawn for its furniture. Returns true when
 * it kept a hole for the popup box. */
bool fft_pane_draw(const plot_pane_t *p, bool all, fft_drawn_t *drawn);
/* The spectrum's strip and cursor readout under band_bot; refresh: in place. */
void fft_furniture_draw(const scope_state_t *ss, const fft_drawn_t *d, uint16_t band_bot, bool refresh);
/* Measure what the time view shows (the record, or the roll points the last
 * frame laid out) and paint the measurement strip. */
void draw_measurement_badges(const scope_state_t *ss, const theme_t *th, bool refresh);

/* scope_view.c */
/* Sample rate of the record on screen, for measurements on the record. */
float record_rate_for_display(const scope_state_t *ss);
/* The view is drawn as the two-converter merge. */
bool view_merged(const scope_state_t *ss);
/* A column span [first, end). */
typedef struct { uint16_t first, end; } g_span_t;
/* The columns of the last window that show the record (view_build). */
extern g_span_t g_span;
/* The time axis of the time view's columns, for the cursors: what a column
 * is worth in the source's time unit (Y-T: ticks, SCOPE_TIMEBASE_TICK_HZ;
 * roll: ms), how well that is known, and the column time is counted from
 * (the trigger point; in roll the newest point, so earlier is negative). */
typedef struct {
    bool                roll;
    uint32_t            per_col;
    scope_cursor_conf_t conf;
    int32_t             ref_col;
} view_time_axis_t;
void view_time_axis(const scope_state_t *ss, view_time_axis_t *ax);
/* Volts per count of channel ch's record (the capture's when held). */
float rec_vpc(const scope_state_t *ss, uint8_t ch);
/* 0 V code of channel ch's record (the capture's when held). */
uint8_t rec_zero(const scope_state_t *ss, uint8_t ch);
/* Fill autofit map a for buf over the band [y_top, y_bot). */
void autofit_prep(autofit_t *a, const volatile uint8_t *buf,
                         int16_t y_top, int16_t y_bot, int8_t mask_ch);
/* Row of code v through map a, not clipped. */
int autofit_y_raw(const autofit_t *a, int v);
/* Row of code v through map a, clipped to the band. */
int16_t autofit_y(const autofit_t *a, uint8_t v);
/* The record code true scale puts on the graticule's centre line. */
uint8_t yt_centre_code(void);
/* Record code of channel ch's 0 V at its current position. */
int chan_zero_code(const scope_state_t *ss, uint8_t ch);
/* The trigger-position pointer covers column x, row dy under the plot's top,
 * for trigger column trig_x. */
bool tpos_px(int trig_x, int x, int dy);
/* How a record is placed in the plot: the window, the column where the
 * trigger point landed and what the placement was anchored on. */
typedef enum {
    VIEW_ANCHOR_NONE = 0,   /* free-running: the window starts at the record's start */
    VIEW_ANCHOR_SOFT,       /* a search about the record's midline */
    VIEW_ANCHOR_HW,         /* the level crossing near the trigger point of a time-ordered record */
} view_anchor_t;
typedef struct {
    scope_window_t win;      /* scope_geom.h; L == 0 marks the general held-record path */
    int16_t        trig_x;   /* window column of the trigger point, -1 when free-running */
    uint8_t        anchor;   /* a view_anchor_t */
    bool           exact;    /* VIEW_ANCHOR_HW from the core's index, not from a search */
} scope_placement_t;
/* The placement the display draws with. Only view_build() writes it. */
extern scope_placement_t g_place;
/* Place the window on the trigger in src_buf at zoom L, into *pl; returns
 * column 0's sample. */
uint16_t scope_soft_trigger_offset(const scope_state_t *ss,
                                          const volatile uint8_t *src_buf, uint8_t L,
                                          int16_t *back_q8, scope_placement_t *pl);
/* Place the window and fill yt_cols and g_span for both channels. */
void view_build(const scope_state_t *ss, const volatile uint8_t *b1,
                       const volatile uint8_t *b2);
/* The roll FIFO as the window: fill yt_cols and g_span, no placement.
 * Returns the points shown. */
uint16_t view_build_roll(void);

/* scope_yt.c */
/* Draw the Y-T plot into pane p through its canvas: every column (all) or
 * only the columns that changed. frame: the display's frame counter.
 * Returns true when it kept a hole for the popup box. */
bool scope_yt_draw(const plot_pane_t *p, bool all, uint32_t frame);

/* scope_xy.c */
/* Draw X-Y as a half of split into pane p through its canvas, its trail in
 * the FFT pool's slot: every column (all) or only the columns that changed.
 * Returns true when it kept a hole for the popup box. */
bool xy_half_draw(const plot_pane_t *p, bool all);

/* scope_yt_frame.c */
/* Publish channel ch's (0/1) band in pane p, once a frame, drawn or not: its
 * map a (off: not drawn), its 0 V code and volts per code in a's codes, the
 * range whose gain they come from. Display task. */
void vband_note(uint8_t ch, const plot_pane_t *p, const autofit_t *a, float zero_code,
                float volts_per_code, uint8_t range_idx);
/* A copy of channel ch's (0/1) band as the last frame published it, for the
 * cursors. Any task. */
void vband_get(uint8_t ch, vband_t *out);
/* Row of the hardware trigger level through map a, in pane p. */
int16_t trigger_marker_y(const scope_state_t *ss, const plot_pane_t *p, const autofit_t *a);
/* The two Y-T maps for the window in yt_cols, in pane p. */
void yt_fits(const scope_state_t *ss, const plot_pane_t *p, autofit_t *a1, autofit_t *a2);
/* Both maps off: nothing is plotted. */
void yt_fits_none(autofit_t *a1, autofit_t *a2);
/* The two maps for the roll points in yt_cols, in pane p. */
void yt_fits_roll(const scope_state_t *ss, const plot_pane_t *p, autofit_t *a1, autofit_t *a2);
/* Build the plot-edge tag set m from the maps as drawn in pane p. */
void markers_build(const scope_state_t *ss, const theme_t *th, const plot_pane_t *p,
                   const autofit_t *a1, const autofit_t *a2, const uint8_t zero[2], pm_set_t *m);
/* Colour of tag set m at pixel (x, y), or false. */
bool markers_px(const pm_set_t *m, int x, int y, uint16_t bg,
                              uint16_t *c);
/* Draw the mask bounds over the traces this frame. */
extern bool     g_mask_bounds_on;
/* Arm the mask overlay for this frame (after view_build). */
void mask_overlay_begin(void);
/* No mask overlay this frame. */
void mask_overlay_off(void);
/* Channel ch failed the mask at column x. */
bool mask_px_fail(int8_t ch, int x);
/* Mask strip colour of column x, or 0; *dotted for every other pixel. */
uint16_t mask_strip_color(int x, const theme_t *th, bool *dotted);
/* A record is drawn this frame, so yt_cols is valid for the math trace. */
extern bool     g_math_live;
/* Add trace rows prev_y..y (screen rows of pane p) at column x to the
 * persistence buffer. */
void persist_note(const plot_pane_t *p, uint16_t x, int16_t prev_y, int16_t y);
/* Clear persistence on a geometry change (pane p's among it), then fade it
 * by wall time. */
void persist_frame_decay(const plot_pane_t *p);
/* A ghost's base colour at screen row y. */
uint16_t persist_base(const scope_state_t *ss, const theme_t *th, bool lower);
/* Prepare this frame's math trace in pane p: inputs, scale, label, cursor scale. */
void math_prepare(const scope_state_t *ss, const plot_pane_t *p, math_frame_t *mf);
/* Row of the math trace at column x. */
int16_t math_row(const math_frame_t *mf, uint16_t x);

/* scope_cursor_draw.c */
/* Build the cursor layer L from the cursor state, in pane p: each line
 * where its time and level fall on the plot as drawn this frame. */
void cursor_layer_build(const scope_state_t *ss, const theme_t *th, const plot_pane_t *p, cur_layer_t *L);
/* The cursor layer of a spectrum drawn on the axes ax, its labels clear of
 * the spectrum's own (T); clear: the first row under its header. */
void cursor_layer_build_spectrum(const scope_state_t *ss, const fft_axes_t *ax, const ctext_set_t *T,
                                 int16_t clear, const theme_t *th, cur_layer_t *L);
/* The spectrum strip's AUX row at row y: the cursor readout. */
void spectrum_cursor_row_paint(const scope_state_t *ss, const fft_axes_t *ax, uint16_t y, bool force);
/* The axes of the spectrum the screen shows (on false: none), for the keys
 * that move a cursor by what a pixel is worth (scope_screen.c). Any task. */
void scope_ui_fft_axes(fft_axes_t *out);
/* Colour of cursor layer L at pixel (x, y), or false. */
bool cursor_layer_px(const cur_layer_t *L, int x, int y, uint16_t *c);
/* Paint cursor layer L's part of column x into strip s. */
struct canvas_strip;
void cursor_layer_paint(const cur_layer_t *L, uint16_t x, const struct canvas_strip *s);
/* Repaint the cursor readout row when its text changed, or when force. */
void cursor_row_paint(const scope_state_t *ss, bool force);

#if FEATURE_BENCHSHELL
/* `fpga scope livecheck` (scope_yt.c): frames left to check, pixels
 * compared, pixels that differ and the first difference. */
extern volatile uint32_t live_check_frames, live_check_pixels, live_check_diffs;
extern volatile uint16_t live_check_first_x, live_check_first_y, live_check_first_want, live_check_first_got;
#endif

/* scope_spectrum.c (the spectrum and the waterfall compose their text) */
/* Add label s at (x, y) to text set T. */
void ctext_add(ctext_set_t *T, int x, int y, const char *s,
                      uint16_t fg, uint16_t bg, bool opaque);
/* Add label s at (x, y) to T only where it lies inside area and meets no
 * label already in T (label_place); false: left out. */
bool ctext_place(ctext_set_t *T, const label_box_t *area, int x, int y, const char *s,
                 uint16_t fg, uint16_t bg, bool opaque);
/* Colour of text set T at pixel (x, y), or false. */
bool ctext_px(const ctext_set_t *T, int x, int y, uint16_t *c);
/* Paint column x of the labels into s (the picture ctext_px gives). */
void ctext_paint_column(const ctext_set_t *T, int x, const canvas_strip_t *s);

/* scope_spectrum.c */
/* The FFT source changed: the waterfall starts again. */
extern bool    waterfall_restart;
/* Fill sbuf with the samples to transform; returns where they came from. */
fft_src_t fft_prepare_input(int16_t *sbuf, uint16_t *n_out,
                                   float *fs_out, fft_source_t *sel);
/* The spectrum's tracked reference level, dB, over bins start..end. */
float fft_display_ref(const fft_config_t *cfg, const float *data,
                             uint16_t start_bin, uint16_t end_bin);
/* A one-line message over black in rows [y_top, y_top + height). */
void fft_note_compose(uint16_t y_top, uint16_t height, const char *msg);

#endif /* SCOPE_UI_PARTS_H */
