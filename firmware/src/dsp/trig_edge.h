/*
 * Which edge fired a committed record? (trigger-modes S2 (c), 2026-09-22)
 *
 * The FPGA triggers on EITHER edge of (reg 0x08 - 28) and no register selects
 * one (EXP-53 50j, EXP-55). So "Rising" / "Falling" is a filter on the MCU:
 * classify each strobed record and let the acquisition loop drop the wrong
 * ones. The record is one continuous 1024-sample segment rotated so the seam
 * sits at the FPGA's write pointer, with the trigger 512 samples after it
 * (EXP-53/54).
 *
 * The classifier REFUSES rather than guesses. It answers UNKNOWN when the seam
 * does not stand out from the signal's own sample-to-sample steps (fast or
 * integer-period signals, where the pointer is not recoverable), when the
 * record does not cross the level near the trigger point, or when no slope
 * clears the noise. The caller commits UNKNOWN records unfiltered: on a fast
 * periodic signal both edges are in every record and the display's soft
 * trigger already aligns to the chosen one.
 *
 * Host-tested on 58 real records (EXP-55/56) and a synthetic sweep, with a
 * negative control: tests/test_trig_edge.c.
 */
#ifndef TRIG_EDGE_H
#define TRIG_EDGE_H

#include <stdint.h>

typedef enum {
    TRIG_EDGE_CLASS_UNKNOWN = 0,
    TRIG_EDGE_CLASS_RISING,
    TRIG_EDGE_CLASS_FALLING
} trig_edge_class_t;

#define TRIG_EDGE_REC_N 1024u

/* rec: the committed CH1 record (1024 samples, record units = ADC - 28).
 * crossing: the level in record units (reg 0x08 code - 28). */
trig_edge_class_t trig_edge_classify(const volatile uint8_t *rec, int crossing);

/* The seam of a committed record: the largest circular step, accepted only
 * when it is >= 2x every other step and >= 6 counts. On success *k is the
 * index of the OLDEST sample (the newer side of the step), so rotating the
 * record left by k puts it in time order with the trigger at index 512.
 * Returns 0 (and leaves *k alone) when the seam does not stand out -- for an
 * integer number of periods the wrap is continuous and there is nothing to
 * undo; for a fast signal the pointer is not recoverable. */
int trig_edge_find_seam(const volatile uint8_t *rec, uint32_t *k);

/* The seam by linear prediction, for records where the value step does not
 * stand out (fast periodic signals: the two segments can meet at similar
 * values but different phase). An order-8 predictor is fitted to the
 * circular record (autocorrelation + Levinson); a continuous signal,
 * harmonics included, is predicted well everywhere except where the
 * predictor reaches back across the seam. Accepted only when the peak
 * prediction error is >= 3x the largest error more than 8 samples away; the
 * seam is the earliest index within 8 before the peak whose error is >= 35%
 * of it. Bias measured on synthetic records: exact or 1-2 samples LATE,
 * never early. Integer periods (no seam in the circular record) refused.
 * No working buffer: the error is recomputed per pass. */
int trig_edge_find_seam_lpc(const volatile uint8_t *rec, uint32_t *k);

/* Value step first (never off in any test), else linear prediction. */
int trig_edge_find_seam_any(const volatile uint8_t *rec, uint32_t *k);

/* The hardware trigger point of a TIME-ORDERED record: the crossing of
 * `crossing` (record units, = reg 0x08 code - 28, EXP-55/56) in the chosen
 * direction nearest index 512, within +/-48 (EXP-53: 501..520). Schmitt-armed
 * with 3 counts of hysteresis, searched over 448..560. Returns the index or
 * -1. Only meaningful on a record un-rotated at its seam -- on a raw rotated
 * record index 512 is not the trigger.
 *
 * One copy, two callers: the display's hardware anchor (scope_ui.c) and the
 * mask pass/fail test (scope_mask.c). If they ever disagreed, the mask would
 * test a different alignment from the one on the glass. */
int trig_edge_anchor(const volatile uint8_t *rec, int crossing, int rising);

/* ── Software edge search (parity C3, stock 0x08028DE4) ──────────────────
 *
 * Stock finds its display trigger in software, in the raw record (ADDENDUM_A
 * S3, emulated: 30,000 random cases, 0 mismatches). At each i in its window,
 * rising: s[i-1] <= L < s[i] and, in NORMAL/AUTO, s[i+1] > L too (the third
 * point); falling is the mirror. SINGLE tests two points only. The third point
 * is a one-sample glitch filter: a lone spike over the level is not an edge.
 *
 * trig_edge_search() is stock's test plus OpenScope's Schmitt arming (the old
 * soft trigger, scope_measure_find_trigger): a crossing counts only once the
 * signal has been `hyst` counts on the far side since the window opened. A
 * crossing whose confirm fails does NOT disarm (stock does not either; doing
 * so lost half the edges of a slow noisy ramp, the chatter right after the
 * first touch failing the confirm). Measured in tests/test_trig_edge.c:
 *   [7] hyst 0 = stock's test exactly, 30,000 random cases;
 *   [8] a lone spike: the third point skips it, the old Schmitt took it; a
 *       slow ramp under +/-3 counts of noise: rms error 5.3 samples with the
 *       third point vs 8.7 with the old Schmitt alone (early noise touches);
 *   [9] the 57 committed unit-#1 records: with confirm 0 the search is the
 *       hardware anchor (55/57, the other two an earlier touch); with the
 *       third point it finds all 57, never earlier, +0..17 samples later
 *       (2 Hz triangles: a 20-sample band of chatter at the level).
 * So OpenScope's fallback anchor adopts the third point in NORMAL/AUTO, and
 * keeps two points in SINGLE as stock does (a one-shot is where a one-sample
 * glitch is the thing being hunted).
 *
 * "Past the level" is INCLUSIVE here: s >= level rising, s <= level falling,
 * the compare the FPGA itself makes (the committed records fire on the code,
 * [9]) and the old soft trigger's. Stock's strict L < s is level = L + 1.
 *
 * Searched i in [lo, hi), clamped to [1, n - 1 - confirm]. `confirm` = the
 * samples after i that must stay past the level: 1 = stock NORMAL/AUTO,
 * 0 = stock SINGLE. Returns i, the first sample past the level (stock stores
 * i - 1, the last one before it), or -1. */
int trig_edge_search(const volatile uint8_t *rec, int n, int lo, int hi,
                     int level, int rising, unsigned hyst, unsigned confirm);

/* Stock's own test with no arming, transcribed literally from S3 (strict
 * L < s), the reference tests [7] compare trig_edge_search(hyst = 0) with. */
int trig_edge_search_stock(const volatile uint8_t *rec, int n, int lo, int hi,
                           int level, int rising, unsigned confirm);

#endif
