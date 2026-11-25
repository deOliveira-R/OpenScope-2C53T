/*
 * OpenScope 2C53T - Oscilloscope Runtime State
 *
 * Live state for the oscilloscope mode: channel config, trigger,
 * timebase, and display options. This is the single source of truth
 * that button handlers modify and UI screens read.
 *
 * Separate from config.h (which is for save/load persistence).
 * On boot, scope_state is initialized from config; on save, it's
 * written back.
 */

#ifndef SCOPE_STATE_H
#define SCOPE_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include "view_table.h"     /* view_results_t: which setting hides a view's strip */

/* ═══════════════════════════════════════════════════════════════════
 * Enums
 * ═══════════════════════════════════════════════════════════════════ */

typedef enum {
    COUPLING_DC = 0,
    COUPLING_AC,
    COUPLING_GND,
    COUPLING_COUNT
} coupling_t;

typedef enum {
    PROBE_1X = 0,
    PROBE_10X,
    PROBE_COUNT
} probe_t;

/* The attenuation of the 10X setting: the largest probe factor. */
#define SCOPE_PROBE_10X_FACTOR  10

typedef enum {
    TRIG_AUTO = 0,
    TRIG_NORMAL,
    TRIG_SINGLE,
    TRIG_COUNT
} trigger_mode_t;

typedef enum {
    TRIG_RISING = 0,
    TRIG_FALLING,
    TRIG_EDGE_COUNT
} trigger_edge_t;

typedef enum {
    TRIG_SRC_CH1 = 0,
    TRIG_SRC_CH2,
    TRIG_SRC_COUNT
} trigger_source_t;

/* ═══════════════════════════════════════════════════════════════════
 * Volts/div and timebase tables
 * ═══════════════════════════════════════════════════════════════════ */

#define VDIV_COUNT      10
#define TIMEBASE_COUNT  21

/* Per-channel configuration */
typedef struct {
    bool            enabled;
    uint8_t         vdiv_idx;       /* Frontend range index, 0..VDIV_COUNT-1.
                                     * Written straight to the attenuator bank;
                                     * calibrated in scope_cal.c. */
    coupling_t      coupling;
    probe_t         probe;
    bool            bw_limit;       /* 20MHz bandwidth limit */
    int16_t         position;       /* Vertical position, stock units -100..+100
                                     * (stock ms[4]/ms[5]): indexes the cal-page
                                     * offset-DAC interpolation; set it through
                                     * scope_vpos_set() (drivers/scope_vpos.h). */
} channel_state_t;

/* Trigger configuration */
typedef struct {
    trigger_mode_t  mode;
    trigger_edge_t  edge;
    trigger_source_t source;
    int16_t         level;          /* Trigger level (pixel offset from center) */
} trigger_state_t;

/* ═══════════════════════════════════════════════════════════════════
 * Cursor measurement types
 * ═══════════════════════════════════════════════════════════════════ */

/* One arrow press moves the selected line this many pixels of the plot it
 * is drawn on, in every domain (time, roll, spectrum) and at every scale. */
#define CURSOR_STEP_PX  4

typedef enum {
    CURSOR_OFF = 0,
    CURSOR_VERTICAL,
    CURSOR_HORIZONTAL,
    CURSOR_BOTH,
    CURSOR_MODE_COUNT
} cursor_mode_t;

typedef enum {
    CURSOR_SEL_V1 = 0,
    CURSOR_SEL_V2,
    CURSOR_SEL_H1,
    CURSOR_SEL_H2,
} cursor_sel_t;

/* The time views' cursors: each line is kept in the unit of what it marks,
 * so it stays on its time and its level when the time base, the range or
 * the position moves (scope_cursor.h has the units, the ranges and the
 * conversions to the screen). Y-T and roll keep a time pair each; the level
 * pair is shared. A line starts at the end of its range, off the plot at
 * every scale, and is put on the plot when its pair comes onto the screen. */
typedef struct {
    cursor_mode_t mode;
    cursor_sel_t  active;

    int32_t t_tick[2];      /* Y-T V1/V2: from the trigger point, SCOPE_TIMEBASE_TICK_HZ ticks */
    int32_t roll_ms[2];     /* roll V1/V2: ms back from the newest point (0 = newest) */
    int32_t level_uv[2];    /* H1/H2: microvolts at the probe tip of the active channel */

    /* No scale here: a line's pixel is worked out at draw time from the
     * time axis and the vertical map on the screen (scope_cursor.h), never
     * cached beside the values it converts. */
} cursor_state_t;

/* The spectrum's cursors: each line is kept in the spectrum's own units (a
 * bin of the transform, a level), so it stays on its frequency or its level
 * when the span or the reference level moves. The mode and the selected line
 * are the time cursors' (cursor_state_t): one cursor setting, read on the
 * axes of the domain it is drawn in. */
#define SPECTRUM_CURSOR_BIN_MIN      1        /* bin 0 is DC, never shown */
#define SPECTRUM_CURSOR_BIN_MAX      2047     /* the last bin of the 4096-point transform */
#define SPECTRUM_CURSOR_LEVEL_UNIT   10       /* levels are kept in tenths of a dB */
#define SPECTRUM_CURSOR_LEVEL_MAX    2000     /* +-200 dB: beyond any level the transform gives */
/* Where the lines start: a third and two thirds of the whole span, and two
 * levels a spectrum of a signal that fills the converter's range has. */
#define SPECTRUM_CURSOR_BIN1_DEFAULT   683
#define SPECTRUM_CURSOR_BIN2_DEFAULT   1365
#define SPECTRUM_CURSOR_LEVEL1_DEFAULT 400
#define SPECTRUM_CURSOR_LEVEL2_DEFAULT 200

typedef struct {
    uint16_t bin[2];        /* the frequency lines (V1, V2) */
    int16_t  level[2];      /* the level lines (H1, H2), SPECTRUM_CURSOR_LEVEL_UNIT per dB */
} spectrum_cursor_t;

/* Full oscilloscope state */
typedef struct {
    channel_state_t ch1;
    channel_state_t ch2;
    trigger_state_t trigger;
    uint8_t         timebase_idx;   /* SPI reg 0x01 timebase code, written
                                     * straight to the FPGA; calibrated in
                                     * scope_timebase.c. */
    bool            running;        /* Acquisition running/stopped */
    cursor_state_t  cursor;         /* Cursor measurement state */
    spectrum_cursor_t spectrum_cursor;   /* where the cursor lines are on a spectrum */

    /* Vertical graticule mode (view.truescale). true (default) = TRUE SCALE:
     * stock's geometry over the full plot, both channels overlaid: 0 V at
     * position 0 on the centre line, one grid division exactly the (nominal)
     * volts/div the status bar prints — but only
     * on ranges scope_cal calls usable; a NONE range always autofits. The
     * vertical position is the offset DAC's (in the record); an uncentred
     * baseline shifts the trace and may clip it at the plot edge.
     * false = AUTOFIT: each channel's trace is scaled from its own min/max to
     * fill its band (half bands with both channels on), so the grid is a
     * position reference, not a volts reference. */
    bool            true_scale;

    /* Software display trigger. true (default) = each frame's render window is
     * shifted to start on the trigger source's level crossing (edge/level/source
     * from `trigger`), so a periodic trace stands still instead of free-running.
     * false = draw from sample 0 every frame (the old free-run "dancing"). When
     * no crossing is found the render free-runs regardless, which is AUTO-mode
     * behaviour. Toggle on the bench with `fpga scope softtrig`. */
    bool            soft_trigger;

    /* Horizontal position (2026-09-22): the screen column (0..319) where the
     * trigger point is drawn. Default 160 = mid-screen, so half the window is
     * pre-trigger. The record holds 512 samples each side of the hardware
     * trigger once it is time-ordered (EXP-53, dev plan 2.3), so any column
     * is reachable at 1 sample per pixel. Not persisted. */
    int16_t         trig_x;

    /* Parameter selection (C6): which of stock's 12 measurements the CH1 badge
     * slots show (bit i = scope_prm_id_t i, see scope_measure.h), and whether
     * they are hidden altogether (stock: long PRM): prm_hidden in the full
     * views (view.meashide), split_prm_hidden in split (view.splithide; read
     * both through scope_results_hidden). Not persisted. */
    uint16_t        prm_mask;
    bool            prm_hidden;
    bool            split_prm_hidden;

    /* Roll mode (C11, scope_roll.h): stock's slow timebases 0x14..0x1C
     * (50 ms..20 s/div) are MCU-paced points, not FPGA records. Kept beside
     * timebase_idx rather than folded into it, because timebase_idx IS the
     * reg-0x01 code of a measured Y-T record (scope_timebase.c) and every
     * consumer of it assumes so. roll = true: the scope shows the roll FIFO
     * at roll_step (0..SCOPE_ROLL_STEPS-1); timebase_idx keeps the Y-T code
     * to return to. Not persisted: boot is always Y-T. Change both through
     * scope_adjust_timebase() / fpga_apply_roll() so the FPGA follows. */
    bool            roll;
    uint8_t         roll_step;
} scope_state_t;

/* The value of results setting r (view_results_t, view_table.h): true when
 * the measurement rows of a view that uses it are hidden. */
static inline bool scope_results_hidden(const scope_state_t *s, uint8_t r)
{
    return (r == VIEW_RESULTS_SPLIT) ? s->split_prm_hidden : s->prm_hidden;
}

/* ═══════════════════════════════════════════════════════════════════
 * Lookup tables (defined in scope_state.c)
 * ═══════════════════════════════════════════════════════════════════ */

/* vdiv_entry_t / vdiv_table removed 2026-08-18 — volts/div is measured, not
 * nominal. See scope_cal.h and the note in scope_state.c. */

/* timebase_entry_t / timebase_table removed 2026-08-18 — time/div is derived
 * from the measured sample rate. See scope_timebase.h and scope_state.c. */


extern const char *coupling_labels[COUPLING_COUNT];
extern const char *probe_labels[PROBE_COUNT];
extern const char *trigger_mode_labels[TRIG_COUNT];
extern const char *trigger_edge_labels[TRIG_EDGE_COUNT];
extern const char *trigger_source_labels[TRIG_SRC_COUNT];

/* ═══════════════════════════════════════════════════════════════════
 * API
 * ═══════════════════════════════════════════════════════════════════ */

/* "Auto R C1": trigger mode, edge and source in <= 9 characters, for the
 * status bar (stock shows the same three; manual 1.2 item 10). The mode is
 * abbreviated (Auto/Norm/Sngl), the edge is R or F (the 95-glyph font has no
 * arrow), the source C1/C2. 60 px at font_small, the widest case. */
void scope_trigger_summary(const scope_state_t *s, char *buf, unsigned n);

/* Initialize scope state with defaults */
void scope_state_init(scope_state_t *s);

/* Get the global scope state (singleton, defined in scope_state.c) */
scope_state_t *scope_state_get(void);

/* Channel vertical position (chN.pos): pixels, +1 = trace up, +-
 * SCOPE_POSITION_MAX (stock's +-100 range, parity plan C4) in steps of
 * SCOPE_POSITION_STEP. The property's range and step (model_table.h). */
#define SCOPE_POSITION_MAX   100
#define SCOPE_POSITION_STEP  5

/* Acquisition-ready latch.
 *
 * Starts false at boot and stays false until the scope has observed at
 * least one real ADC frame from the FPGA. The UI uses this to decide
 * whether to show the synthetic demo waveform (shipped as a "not
 * connected" placeholder) or to leave the grid empty.
 *
 * Phase 4 will flip this via scope_mark_acquisition_ready() as soon as
 * the SPI3 bringup is working. */
bool scope_acquisition_ready(void);
void scope_mark_acquisition_ready(void);

#endif /* SCOPE_STATE_H */
