/*
 * OpenScope 2C53T — horizontal (time) calibration
 *
 * WHAT THIS IS
 * ------------
 * One number per reg-0x01 timebase code: the capture sample rate in samples
 * per second. Everything horizontal the instrument could claim — seconds per
 * division, frequency, period, rise time — is derived from that rate and a
 * sample count. There is nothing else in the horizontal path.
 *
 * WHERE THE NUMBERS COME FROM
 * ---------------------------
 * From OpenScope's capture core: its clock (125 MHz) over the integer its
 * package states for each code, and the converters' own 50 MHz clock at the
 * 50 MS/s code (drivers/fpga_core_regs.h, generated from the core's source).
 * The rates are exact ratios of one clock, so the same signal reads the same
 * at every timebase (bench 2026-11-17: within the measurement's +-200 ppm at
 * codes 4..19). Their absolute value rests on the unit's 25 MHz crystal,
 * which nothing here measures.
 *
 * (Until 2026-11-17 this was a table of rates measured against the scope's
 * factory FPGA design; the method and its lessons are in docs/experiments/.)
 *
 * SECONDS PER DIVISION IS A PROPERTY OF OUR RENDERER, NOT OF THE HARDWARE
 * -----------------------------------------------------------------------
 * scope_yt.c draws one sample per screen column and rules a vertical grid line
 * every 25 px (stock's pitch since parity C5; it was 32 px until then, which
 * is the geometry the paragraph below measured), so a division is 25 samples
 * and s/div = 25/fs, divided by the zoom at codes 0x00-0x03. That is what our
 * grid means, and the label must say so.
 *
 * Worth recording, because it is a genuinely different situation from the
 * volts case: the nominal `timebase_table` labels ("5ns" ... "20ms") are a
 * CONSTANT factor away from that on every code we have measured. Before the
 * 2026-08-19 source correction that factor read as 2.12-2.15x; with the
 * corrected rates it is 2.51-2.56x, i.e. the nominal labels are almost exactly
 * 2.5x our s/div. Unlike `vdiv_table` — which was 2.7-3.5x out with NO
 * consistent factor and was simply invented — this table is evidently real and
 * describes a renderer that draws a different number of samples per division
 * than ours does. That is a fact about STOCK's geometry, not a defect in ours.
 * If the renderer's geometry is ever changed, change
 * SCOPE_TIMEBASE_SAMPLES_PER_DIV with it and the labels follow. (C5 did: at
 * 25 samples/div our labels are STOCK's ladder -- 200 ns at 0x04, 5 us at
 * 0x08, 2 ms at 0x10 (period table 0x080465E0 x 25) -- to the 0.2 % the
 * measured rates sit from the round ones. `timebase_table` is still 2x off.)
 *
 * A NOTE ON THE ROUND NUMBERS
 * ---------------------------
 * The measured ladder is very close to 12.5k / 25k / 50k / 125k S/s — a clean
 * 1-2-5 sequence — and Stlkv's independent rig reports 12,437 / 25,000 /
 * 125,000. The round values sit inside our run-to-run spread on every code, so
 * they are probably the truth. They are deliberately NOT written into the
 * table: what is stored is what was measured. Rounding measurements toward the
 * answer we expect is how a table stops being evidence.
 */

#ifndef SCOPE_TIMEBASE_H
#define SCOPE_TIMEBASE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Timebase codes are the UI's timebase_idx written straight to SPI reg 0x01
 * (see fpga_stock_timebase_byte()), so this must match TIMEBASE_COUNT. */
#define SCOPE_TIMEBASE_CODE_COUNT   21u

/*
 * Samples per screen division — set by the renderer, not the hardware.
 * scope_yt.c: one sample per column (at zoom 1), vertical grid every 25 px,
 * stock's geometry since parity C5 (scope_geom.h; 32 px before, which made
 * every label 1.28x stock's on the same code). scope_furniture.c carries a
 * compile-time assertion tying this to its geometry.
 */
#define SCOPE_TIMEBASE_SAMPLES_PER_DIV   25.0f

typedef enum {
    SCOPE_TB_NONE = 0,      /* not a timebase code: no rate */
    SCOPE_TB_PROVISIONAL,   /* (no code is provisional any more; kept for the cursor read-outs' wording) */
    SCOPE_TB_MEASURED,      /* a code of the core: an exact rate */
} scope_tb_tier_t;

/* Capture sample rate in samples/second, or 0.0f when unknown. */
float scope_timebase_sample_rate(uint8_t code);

/* The whole table (SCOPE_TIMEBASE_CODE_COUNT entries, 0.0f = no rate), for a
 * caller that searches it (autoset, scope_selfcal.h). */
const float *scope_timebase_rate_table(void);

/* How much to trust it. */
scope_tb_tier_t scope_timebase_get_tier(uint8_t code);

/* Seconds between adjacent samples, or 0.0f when unknown. */
float scope_timebase_seconds_per_sample(uint8_t code);

/* Seconds per screen division, derived from the rate and the renderer's
 * samples-per-division. 0.0f when unknown. */
float scope_timebase_seconds_per_div(uint8_t code);

/*
 * Status-bar label, e.g. "2.15ms", "~213us" (provisional, leading tilde) or
 * "--" (no rate). Always NUL-terminated; 10 bytes is enough for every entry.
 */
void scope_timebase_label(uint8_t code, char *out, uint32_t n);
/* Same, for a display whose column rate is `rate_factor` x the table's (2
 * for the single-channel interleave, times the zoom at the fast codes). */
void scope_timebase_label_scaled(uint8_t code, uint8_t rate_factor, char *out, uint32_t n);

/*
 * Display zoom at the fast codes (parity C5): screen columns per record
 * sample. The converters run at their top rate on every code 0x00-0x03
 * (124.8 MS/s each, 2x when the published record is the single-channel
 * merge), so the codes differ only in this factor. Stock's factors
 * (ms[0xE04] = 1000 / ratio, 0x08030B4E..0x08030B6C):
 *     code          0x00 0x01 0x02 0x03   others
 *     merged          10    5    2    1      1
 *     per converter   20   10    4    2      1
 * so the time per division is the same 10/20/50/100 ns whichever record
 * the hardware delivered. `interleaved` is the published record's flag
 * (fpga_acq_record_interleaved), not the channel count: a single channel
 * whose merge is off still gets the per-converter row.
 */
uint8_t scope_timebase_interp_factor(uint8_t code, bool interleaved);

/* The status-bar label for what the screen shows: the code's rate, x2 for a
 * merged record, times the zoom above ("10ns" at 0x00 either way). */
void scope_timebase_view_label(uint8_t code, bool interleaved, char *out, uint32_t n);

/*
 * Hz from a measured period in SAMPLES, or 0.0f when the code has no rate or
 * the period is not positive. The caller is expected to print samples instead
 * when this returns 0.
 */
float scope_timebase_hz_from_period(uint8_t code, float period_samples);

#ifdef __cplusplus
}
#endif

#endif /* SCOPE_TIMEBASE_H */
