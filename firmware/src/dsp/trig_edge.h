/*
 * Where the trigger is in a record, and the software edge search.
 *
 * A record is 1024 samples in record units (ADC - 28). OpenScope's capture
 * core names the record's start in its reply header (dsp/trig_edge_reg.h);
 * the acquisition task rotates the record there (trig_edge_rotate_left), so
 * index 0 is the oldest sample and a triggered record has its crossing at
 * index 512. The functions here find that crossing (the anchor) for the
 * display and the mask test, and search an edge in software where there is
 * no hardware one.
 *
 * Host-tested: tests/test_trig_edge.c.
 */
#ifndef TRIG_EDGE_H
#define TRIG_EDGE_H

#include <stdbool.h>
#include <stdint.h>

#include "../util/units.h"   /* rec_code_t, trig_level_rec */

#define TRIG_EDGE_REC_N 1024u
#define TRIG_EDGE_TRIG_POS 512u   /* a triggered record's crossing, counted from the record's start */

/* The hardware trigger point of a TIME-ORDERED record: the crossing of
 * `crossing` (a record code, trig_level_rec(reg 0x08 code), units.h; EXP-55/56)
 * in the chosen direction nearest index 512, within +/-48 (EXP-53: 501..520).
 * Schmitt-armed with 3 counts of hysteresis, searched over 448..560. Returns
 * the index or -1. A comparator code below the ADC offset clamps to record 0,
 * where no crossing exists (samples are never below 0). Only meaningful on a
 * record rotated to its start -- on a raw rotated record index 512 is not
 * the trigger.
 *
 * One copy, two callers: the display's hardware anchor (scope_view.c) and the
 * mask pass/fail test (scope_mask.c). If they ever disagreed, the mask would
 * test a different alignment from the one on the glass. */
int trig_edge_anchor(const volatile uint8_t *rec, rec_code_t crossing, int rising);

/* The same on a record whose start OpenScope's capture core named (the seam in
 * the reply header): the core stores the trigger crossing TRIG_EDGE_TRIG_POS
 * samples after the seam, so there is nothing to search. Rising: rec[511] <
 * crossing <= rec[512]. In the 250 MS/s merged record the other converter's
 * sample sits between those two, and the crossing may be one index earlier.
 * Returns 512, 511, or -1 when neither place holds the crossing (the level
 * as the firmware knows it is not the core's: the caller then searches).
 * No Schmitt arming and no level range: a slow ramp that never goes 3 counts
 * below the level inside the search window still has its trigger here. */
int trig_edge_anchor_known(const volatile uint8_t *rec, rec_code_t crossing, int rising);

/* The anchor of a time-ordered record: the known place when the core ordered
 * it, else (or when that check fails) the search. *exact, when not NULL,
 * says which. */
static inline int trig_edge_anchor_of(const volatile uint8_t *rec, rec_code_t crossing, int rising,
                                      bool core_ordered, bool *exact)
{
    int a = core_ordered ? trig_edge_anchor_known(rec, crossing, rising) : -1;
    if (exact) *exact = (a >= 0);
    return (a >= 0) ? a : trig_edge_anchor(rec, crossing, rising);
}

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
 * `level` stays a plain int: this is the generic search, used with a
 * software threshold (the record's own midline, scope_view.c) and compared
 * with stock's strict test at level +/- 1, which leaves 0..255.
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

/* Rotate rec[0..n) left by k in place (k >= n is taken mod n): afterwards
 * rec[i] is the old rec[(i + k) % n]. Putting a record in time order is
 * exactly this with k = the seam the core's reply header names. By three
 * reversals, so it needs no working buffer: fpga.c used to stage it through a
 * 1 KB static tmp[] that was only ever live inside this copy (RAM diet,
 * 2026-11-11). */
void trig_edge_rotate_left(volatile uint8_t *rec, uint32_t n, uint32_t k);

#endif
