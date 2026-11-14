/*
 * OpenScope 2C53T — honest measurements over a raw capture record
 *
 * WHAT THIS IS FOR
 * ----------------
 * The scope UI needs numbers to put in its measurement badges. This module
 * computes ONLY the quantities that are genuinely derivable from the samples
 * we actually have, and nothing else. Everything here is expressed in the
 * units the instrument really observes today:
 *
 *   - ADC COUNTS for anything vertical. Counts -> volts needs the per-range
 *     gain/offset calibration this firmware does not have yet (dev plan §F2:
 *     "per-range scope calibration ... needs known signals at known
 *     amplitudes"). Until then a volt figure would be invented, so none is
 *     produced.
 *   - SAMPLES / RECORD for anything horizontal. Samples -> seconds needs a
 *     known sample rate, i.e. a timebase; this firmware has no timebase
 *     control at all (dev plan §F4: TMR3 is stock's acquisition pacer and
 *     ours is owned by the 500 Hz button scan). So no frequency, no period,
 *     no rise time is produced.
 *
 * What survives that filter is real:
 *   min / max / peak-to-peak / mean / AC RMS  in counts,
 *   duty cycle                                as a pure ratio,
 *   cycles per record                         as a pure count,
 *   period                                    in samples.
 *
 * Duty and cycle count are invariant under ANY affine counts->volts mapping
 * (y = a*x + b, a > 0): the mid-level threshold, the sample ordering and the
 * crossing instants all move together. That is why they are correct despite
 * the missing vertical calibration, and it is asserted directly by
 * tests/test_scope_measure.c.
 *
 * WHY NOT measurement_compute() (src/tasks/measurement.c)?
 * It is unit-tested and correct on its own terms, but its terms are not this
 * hardware's: it scales every voltage by 3.3/32768 (a 16-bit ADC over a 3.3 V
 * span — ours is 8-bit through an analog frontend of unknown gain) and its
 * frequency/period/rise-time outputs are all multiplied by a caller-supplied
 * sample_rate we do not know. Feeding it counts and unwinding its scale
 * factor afterwards would produce the same numbers this file produces, but
 * coupled to a constant in another module that is wrong for this board — a
 * silent-breakage hazard. When a real timebase and real cal exist, the right
 * move is to fix that constant, pass the true sample rate, and call it.
 */

#ifndef SCOPE_MEASURE_H
#define SCOPE_MEASURE_H

#include <stdint.h>
#include <stdbool.h>

/*
 * A record whose peak-to-peak is below this is treated as "no discernible
 * signal": the mid-level threshold would be sitting inside the noise, so duty
 * and cycle count would be counting noise, not the waveform. 6 counts of
 * 255 ~ 2.4% of the ADC span.
 */
#define SCOPE_MEASURE_MIN_PP  6u

/* Crossing intervals (rise-rise, fall-fall, and the two periods) must agree
 * within this fraction for a period to be reported: with 1.5..3 periods in a
 * record a single misplaced crossing is a wrong period, so it is "--". */
#define SCOPE_MEASURE_PERIOD_TOL 0.04f

typedef struct {
    bool     valid;         /* a usable record was supplied at all           */

    /* Vertical — ADC COUNTS, never volts (see header comment). */
    uint8_t  min;
    uint8_t  max;
    uint8_t  pp;            /* max - min                                     */
    /* pp with 0.5% of samples trimmed from each end of the DISTRIBUTION
     * before taking the extremes. `pp` is an extreme-value statistic: over a
     * 1024-sample noisy 8-bit record the noise tails inflate both ends, and
     * EXP-19 measured that inflation at +4..+10% of commanded amplitude,
     * shrinking as amplitude grows — the additive signature. Trimming is
     * shape-safe for the waveforms this instrument shows: sines (arcsine
     * density, mass AT the extremes) and squares (mass at the levels) lose
     * nothing; a pure triangle loses ~1% by construction. Use THIS for any
     * voltage the user reads; keep `pp` for validity thresholds. */
    uint8_t  pp_robust;
    float    mean;          /* DC level                                      */
    float    ac_rms;        /* RMS about the mean (mean removed)             */

    /* True once pp >= SCOPE_MEASURE_MIN_PP. When false, duty_pct and cycles
     * are meaningless and MUST NOT be displayed. */
    bool     level_valid;

    /* Horizontal — in SAMPLES, or dimensionless. No sample rate involved. */
    float    duty_pct;      /* % of samples above the mid-level              */
    uint16_t cycles;        /* rising mid-level crossings within the record  */
    bool     cycles_valid;  /* level_valid && >= 2 crossings                 */
    float    period_samples;/* samples per cycle, first->last crossing       */
                            /* valid only when every crossing interval agrees
                             * within SCOPE_MEASURE_PERIOD_TOL (few periods)  */
    bool     period_valid;

    /* Edge timing — in SAMPLES, never seconds (same rule as period_samples).
     *
     * rise_samples: 10%->90% span on the FIRST clean rising edge.
     * fall_samples: 90%->10% span on the FIRST clean falling edge.
     *
     * This is the one quantity the retired measurement_compute() engine
     * (src/tasks/measurement.c) produced that nothing else here did. It now
     * lives in this module, in the instrument's honest horizontal unit, gated
     * on level_valid like duty and cycles. The 10/90 references are taken from
     * the TRIMMED span [pp_robust], so a few noise-tail outliers do not stretch
     * them. Multiply by the sample interval for seconds the day a timebase
     * exists — the same one-multiply wiring job as period_samples. */
    float    rise_samples;
    bool     rise_valid;
    float    fall_samples;
    bool     fall_valid;

    /* ── Stock's 12-parameter set (C6) ─────────────────────────────────
     * Extra raw ingredients for Vmax/Vmin/Amp/Wid+/Wid-, same rules as the
     * rest of this struct: counts and samples only, never volts or seconds. */

    /* Trimmed extremes (the same 0.5% walk as pp_robust: pp_robust ==
     * rob_max - rob_min). Vmax/Vmin read these, for the reason pp_robust
     * exists. */
    uint8_t  rob_min;
    uint8_t  rob_max;

    /* Amplitude = top - base, the two histogram MODES (the levels the
     * waveform dwells at) of the upper and lower half about the mid-level.
     * Square: its two rails, ignoring overshoot, so Amp < Vp-p when there is
     * ringing. Sine: the modes sit at the arcsine peaks, so Amp ~ Vp-p.
     * Stock's exact Amp definition is not decoded; this is the standard
     * top-base one [H]. */
    uint8_t  top;
    uint8_t  base;

    /* Mean width of the COMPLETE high / low pulses (mid-level crossing to the
     * next opposite crossing, sub-sample interpolated), in SAMPLES. A pulse
     * cut by either record edge is not complete and is not counted. */
    float    wid_hi_samples;
    bool     wid_hi_valid;
    float    wid_lo_samples;
    bool     wid_lo_valid;
} scope_measure_t;

/*
 * Analyse one capture record of unsigned 8-bit samples.
 *
 * samples/n may be NULL/0 — the result is then simply invalid, all fields
 * zero. Never reads outside [0, n).
 */
void scope_measure_record(const uint8_t *samples, uint16_t n,
                          scope_measure_t *out);

/*
 * Software edge-trigger for display stability.
 *
 * Scan buf[0..max_start] for the first sample that crosses `threshold` in the
 * chosen direction (rising: low->high; falling: high->low) AFTER first being
 * "armed" by `hyst` counts on the far side. The arm step is a Schmitt gate: it
 * rejects noise wiggles sitting right on the threshold, so a clean periodic
 * signal triggers once per cycle at a repeatable phase.
 *
 * Returns that index (>= 0), or -1 if no qualifying crossing exists in the
 * search span — the caller then draws from 0 (free-run), which is exactly what
 * a real scope's AUTO mode does when the level sits outside the signal.
 *
 * Pure and side-effect-free; never reads past buf[max_start]. Host-tested.
 */
int scope_measure_find_trigger(const uint8_t *buf, uint16_t max_start,
                               uint8_t threshold, bool rising, uint8_t hyst);

/* ═══════════════════════════════════════════════════════════════════
 * Stock's 12 measurements per channel (C6)
 *
 * Order and labels are stock V1.2.0's table at 0x0804BF28: Freq, Peri,
 * Duty+, Duty-, Wid+, Wid-, Vmax, Vmin, Vp-p, Amp, Vrms, Avg.
 *
 * TIME QUANTITIES USE THE PUBLISHED RECORD'S SAMPLE RATE, not the table's:
 * pass fpga_record_sample_rate() (2x the table rate for an interleaved
 * record). This module owns no rate.
 * VOLTS use the caller's volts-per-count (scope_cal_volts_per_count); k <= 0
 * means uncalibrated and the vertical quantities stay in ADC COUNTS, exactly
 * as the badges always did.
 * ═══════════════════════════════════════════════════════════════════ */

typedef enum {
    SCOPE_PRM_FREQ = 0, SCOPE_PRM_PERI, SCOPE_PRM_DUTY_POS, SCOPE_PRM_DUTY_NEG,
    SCOPE_PRM_WID_POS,  SCOPE_PRM_WID_NEG, SCOPE_PRM_VMAX,  SCOPE_PRM_VMIN,
    SCOPE_PRM_VPP,      SCOPE_PRM_AMP,     SCOPE_PRM_VRMS,  SCOPE_PRM_AVG,
    SCOPE_PRM_COUNT
} scope_prm_id_t;

typedef enum {
    SCOPE_PRM_UNIT_NONE = 0,   /* refused: render "--"                      */
    SCOPE_PRM_UNIT_HZ,
    SCOPE_PRM_UNIT_SECONDS,
    SCOPE_PRM_UNIT_SAMPLES,    /* time quantity with no known rate          */
    SCOPE_PRM_UNIT_PERCENT,
    SCOPE_PRM_UNIT_VOLTS,
    SCOPE_PRM_UNIT_COUNTS,     /* vertical quantity with no calibration     */
} scope_prm_unit_t;

typedef struct {
    scope_prm_unit_t unit;
    float            value;    /* signed, in `unit`; 0 when unit is NONE    */
} scope_prm_t;

typedef struct {
    scope_prm_t v[SCOPE_PRM_COUNT];
} scope_params_t;

typedef struct {
    float sample_rate;   /* S/s of THIS record (fpga_record_sample_rate()); <=0 = unknown */
    float volts_per_count;/* probe-tip V per ADC count; <=0 = uncalibrated  */
    float freq_hz;       /* the SPECTRAL estimate (scope_freq), 0 = declined.
                          * The crossing period was 5-50% out (EXP-13) because
                          * records were torn (EXP-16): it is dressed in Hz
                          * only for a record proven whole (record_whole) and
                          * only when every crossing interval agrees
                          * (period_valid). Else Freq and Peri are both "--"
                          * (Peri in SAMPLES when there is no rate at all). */
    bool  record_whole;  /* the record is one continuous stretch of time:
                          * un-rotated at its seam (fpga_acq_record_time_
                          * ordered()), or MCU-paced points (Roll)            */
    uint8_t zero_count;  /* ADC code of 0 V. Vmax/Vmin/Avg are volts relative to
                          * it; SCOPE_MEASURE_ZERO_COUNT when the channel is
                          * centred (`fpga scope center`).                   */
} scope_prm_ctx_t;

/* ADC mid-scale: where a centred baseline sits, and the 'center' the
 * true-scale renderer already uses. Nominal, not a per-unit number. */
#define SCOPE_MEASURE_ZERO_COUNT  128u

/* Record code of 0 V for a channel. With the stock calibration page in use
 * (scope_vpos), the offset DAC puts 0 V at raw 128 + pos, and records are
 * raw + adc_offset (FPGA_ADC_OFFSET, -28), so 0 V sits at 100 + pos. Without
 * the page the legacy centring targets record 128. Clamped to 0..255. */
static inline uint8_t scope_measure_zero_for(int calibrated, int pos, int adc_offset)
{
    int z = calibrated ? 128 + adc_offset + pos : (int)SCOPE_MEASURE_ZERO_COUNT;
    if (z < 0) z = 0;
    if (z > 255) z = 255;
    return (uint8_t)z;
}

/* Compute all 12 from an analysed record. m == NULL or !m->valid refuses all. */
void scope_params_compute(const scope_measure_t *m, const scope_prm_ctx_t *ctx,
                          scope_params_t *out);

/* Stock's label ("Freq", "Vp-p", ...); "?" out of range. */
const char *scope_prm_label(unsigned id);

/* "1.00kHz", "50.0%", "-1.20V", "37cnt", "45smp", "--". NUL-terminated;
 * 16 bytes is enough. Reuses the cursor formatter so the units read alike. */
void scope_prm_format(const scope_prm_t *p, char *out, uint32_t n);

/*
 * Parameter selection (stock's short PRM) and hide (long PRM).
 *
 * `mask` is bit i = scope_prm_id_t i shown. The default is the five badges the
 * instrument has always shown for CH1, so selecting nothing changes nothing:
 * Freq, Peri, Duty+, Vp-p, Vrms.
 */
#define SCOPE_PRM_ALL_MASK      ((1u << SCOPE_PRM_COUNT) - 1u)
#define SCOPE_PRM_DEFAULT_MASK  ((1u << SCOPE_PRM_FREQ) | (1u << SCOPE_PRM_PERI) | \
                                 (1u << SCOPE_PRM_DUTY_POS) | (1u << SCOPE_PRM_VPP) | \
                                 (1u << SCOPE_PRM_VRMS))

/* Fill ids[] with the selected parameters in stock order, at most max of them;
 * returns how many were written. Bits outside the 12 are ignored. */
unsigned scope_prm_select(uint16_t mask, uint8_t *ids, unsigned max);

#endif /* SCOPE_MEASURE_H */
