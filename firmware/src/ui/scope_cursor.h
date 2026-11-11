/*
 * OpenScope 2C53T — cursor delta readouts
 *
 * WHAT THIS IS
 * ------------
 * The arithmetic between "the user put two cursors N pixels apart" and the
 * string on the glass. It owns NO constants of its own: seconds come from
 * scope_timebase.c, volts come from scope_cal.c, and the pixel->sample and
 * pixel->count transforms are supplied by the renderer that actually drew the
 * trace. If neither table has an entry, this module refuses to name a unit it
 * cannot support and hands back the exact raw quantity instead.
 *
 * WHY IT EXISTS
 * -------------
 * The cursor readout used to be driven by two floats in cursor_state_t,
 * `time_per_pixel` and `volts_per_pixel`. They were seeded with 10 ms across
 * the screen and 8 V down it, and nothing ever updated them — not the
 * timebase control, not the range control. Every "dt = 1.2 ms" and
 * "dV = 340 mV" the instrument ever printed came from those two numbers, so
 * the readout tracked the CURSOR POSITIONS and nothing else, in a feature
 * whose only purpose is measurement. That is the same defect the volts/div
 * labels had (docs/devlog/2026-08-18-the-labels-were-never-measured.md) and
 * the same one the two hardcoded mV/count constants in scope_ui.c had.
 *
 * On 2026-08-18 the two floats were zeroed rather than fixed, which made the
 * readout honest (it fell back to samples and counts) but left it
 * permanently disconnected from the measured tables that had just landed:
 * with a real 12,490 S/s rate and a real 42.95 mV/count gain in the build,
 * the cursor still said "45smp" and "37cnt". This module connects them, and
 * keeps the refusal for the cases where there is genuinely nothing to
 * connect.
 *
 * THE RULES
 * ---------
 *   - A calibrated axis prints its real unit, with a leading '~' when the
 *     table row it came from is PROVISIONAL. Same marker, same meaning as
 *     scope_cal_range_label() and scope_timebase_label().
 *   - An uncalibrated axis prints the exact raw quantity — samples, ADC
 *     counts or screen pixels — never a converted one.
 *   - Where not even a raw quantity is meaningful (1/dt with the cursors on
 *     the same column, or with no sample rate at all) it prints "--", the
 *     same string the measurement badges use.
 *
 * WHY THE VERTICAL MAP IS AN ARGUMENT
 * -----------------------------------
 * Pixels mean a fixed number of ADC counts only in the TRUE-SCALE render
 * path. The default path is autofit, which rescales every frame from the
 * record's own min/max, so pixels-per-count is a property of the frame that
 * was drawn, not of the hardware. The renderer therefore reports the
 * transform it used (see scope_ui.c) and this module converts with THAT.
 * Assuming the fixed transform while autofit is on the screen would be a
 * plausible wrong number of exactly the kind this module exists to stop —
 * and it is what the old code did.
 *
 * Host-testable: no hardware, no RTOS. See tests/test_scope_cursor.c.
 */

#ifndef SCOPE_CURSOR_H
#define SCOPE_CURSOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One screen column is one sample. This is a property of the RENDERER —
 * draw_channel_autofit()/draw_channel_fixed() plot buf[x] at column x — not
 * of the capture. scope_ui.c carries a compile-time assertion tying the two
 * together, so changing the plot without changing this is a build error
 * rather than a silently wrong dt.
 */
#define SCOPE_CURSOR_SAMPLES_PER_PIXEL   1.0f

/* What a reading is expressed in. */
typedef enum {
    SCOPE_CURSOR_UNIT_NONE = 0,   /* nothing honest to print — renders "--" */
    SCOPE_CURSOR_UNIT_SAMPLES,    /* raw fallback for the time axis         */
    SCOPE_CURSOR_UNIT_COUNTS,     /* raw fallback for the vertical axis     */
    SCOPE_CURSOR_UNIT_PIXELS,     /* raw fallback when even counts are unknown */
    SCOPE_CURSOR_UNIT_SECONDS,
    SCOPE_CURSOR_UNIT_VOLTS,
    SCOPE_CURSOR_UNIT_HERTZ,
} scope_cursor_unit_t;

/* How much the reading is worth. RAW is not a lesser tier — a raw count is
 * exact; it just is not volts. */
typedef enum {
    SCOPE_CURSOR_RAW = 0,       /* no calibration used, quantity is exact  */
    SCOPE_CURSOR_PROVISIONAL,   /* converted through a PROVISIONAL row     */
    SCOPE_CURSOR_MEASURED,      /* converted through a MEASURED row        */
} scope_cursor_conf_t;

typedef struct {
    scope_cursor_unit_t unit;
    scope_cursor_conf_t confidence;
    float               value;   /* signed, in `unit`; 0 when unit is NONE */
} scope_cursor_reading_t;

/*
 * The vertical pixel -> ADC count transform the renderer ACTUALLY USED for
 * the band the cursors sit in, plus which (channel, range) that band shows.
 *
 * counts_per_pixel <= 0 means "the transform is unknown" — no trace drawn,
 * the demo waveform, or the two cursors straddling two differently-scaled
 * bands. The reading then stays in pixels.
 */
typedef struct {
    float   counts_per_pixel;
    uint8_t channel;            /* 1 or 2; anything else => no volts */
    uint8_t range_idx;

    /* Absolute Y (C7). Where the band's middle row sits relative to 0 V, in
     * counts: known (mid_known != 0) only for a renderer that anchors a fixed
     * ADC code to that row (the true-scale path: code 128 at the band middle,
     * so mid_counts = 0 for a centred baseline). Autofit rescales and
     * re-anchors every frame, so it leaves this zero and absolute Y is "--".
     * Zero-initialised by the existing `{ cpp, ch, rng }` initialisers. */
    int16_t mid_row;
    uint8_t mid_known;
    float   mid_counts;

    /* DIRECT scale (math trace). The math overlay is drawn on its own scale
     * about the graticule centre (value = rows from mid / px_per_div x step),
     * not through an ADC code and a (channel, range) gain, so the renderer
     * reports the result in its own unit: direct = DIRECT_VOLTS / DIRECT_COUNTS
     * and the readout is `unit_per_pixel` x rows from mid_row (mid_counts is
     * the zero, unused). Other math units (V^2, counts^2, A/B) have no unit
     * this module can format: counts_per_pixel stays 0 and the readout is in
     * pixels, never a converted number. direct_conf is the worst tier of the
     * source channels' calibration. Zero = not a direct scale. */
    uint8_t             direct;
    scope_cursor_conf_t direct_conf;
    float               unit_per_pixel;
} scope_cursor_vmap_t;

#define SCOPE_CURSOR_DIRECT_NONE    0u
#define SCOPE_CURSOR_DIRECT_VOLTS   1u
#define SCOPE_CURSOR_DIRECT_COUNTS  2u

/* Confidence of a (channel, range) calibration row, for direct scales. */
scope_cursor_conf_t scope_cursor_cal_conf(uint8_t channel, uint8_t range_idx);

/*
 * Rate-explicit forms. The record's sample rate is NOT always the table's:
 * an interleaved record is published at twice the per-converter rate, so the
 * caller passes fpga_record_sample_rate() (when the record in hand is the one
 * the timebase code describes) and the table tier of that code as `conf`.
 * fs <= 0 behaves as "no rate" exactly like an unmeasured code. The tb_code
 * forms below are these with the table's own rate.
 */
scope_cursor_conf_t scope_cursor_tb_conf(uint8_t tb_code);
scope_cursor_reading_t scope_cursor_delta_t_fs(float fs, scope_cursor_conf_t conf,
                                               int32_t dx_px);
scope_cursor_reading_t scope_cursor_one_over_dt_fs(float fs, scope_cursor_conf_t conf,
                                                   int32_t dx_px);

/* X1 / X2 (C7): time of a screen column relative to the reference column
 * (the trigger point), signed; samples when there is no rate. */
scope_cursor_reading_t scope_cursor_x_fs(float fs, scope_cursor_conf_t conf,
                                         int32_t col, int32_t ref_col);

/* Y1 / Y2 (C7): level of a screen row relative to 0 V — volts when the range
 * is calibrated, counts otherwise, NONE (--) unless map->mid_known. */
scope_cursor_reading_t scope_cursor_y(const scope_cursor_vmap_t *map, int32_t row);

/* Time between two vertical cursors, dx screen columns apart (signed). */
scope_cursor_reading_t scope_cursor_delta_t(uint8_t tb_code, int32_t dx_px);

/* 1/dt — always positive, and NONE when dx is 0 or the code has no rate. */
scope_cursor_reading_t scope_cursor_one_over_dt(uint8_t tb_code, int32_t dx_px);

/* Voltage between two horizontal cursors, dy screen rows apart (signed). */
scope_cursor_reading_t scope_cursor_delta_v(const scope_cursor_vmap_t *map,
                                            int32_t dy_px);

/*
 * Render a reading: "1.23ms", "~213us" (provisional), "-45smp", "37cnt",
 * "12px", "--". Always NUL-terminated; 16 bytes is enough for every case.
 * The '~' leads, as it does in scope_cal_range_label().
 */
void scope_cursor_format(const scope_cursor_reading_t *r, char *out, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif /* SCOPE_CURSOR_H */
