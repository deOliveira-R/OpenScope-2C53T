/*
 * OpenScope 2C53T -- zoom while paused (parity C14)
 *
 * WHAT STOCK DOES (docs/re/parity2_static.md section 3, V1.2.0)
 * -------------------------------------------------------------
 * After STOP the held 1 Kpts record and the capture metadata stay frozen.
 * A timebase or range change does NOT re-arm: it only re-renders the held
 * record (0x08021178..0x08021348):
 *   horizontal  ratio_x1000 = 1000 * display ns per px / record ns per sample
 *               (0x08030A70); R < 1000 is a zoom IN and goes through a 16-tap
 *               polyphase FIR (0x0803D5D4), R >= 1000 is a plain per-column
 *               time-to-index map;
 *   vertical    y = clamp(centre + pos_now + k * (raw - centre - pos_cap)),
 *               k = V[range_cap] / V[range_now].
 *
 * WHAT THIS MODULE IS
 * -------------------
 * The pure arithmetic only (no hardware, no globals), so it is host-tested.
 * scope_ui.c supplies the capture rate / ranges and plots through it.
 *
 *  - "Display time per column" is 1 / fs_col, the column rate of the code
 *    now on the status bar: its sample rate (x2 merged) times the fast-code
 *    zoom of parity C5 (scope_timebase_interp_factor). So R = 1000 * fs_cap /
 *    fs_col, the number stock's table arithmetic yields; pv_stock_ratio_x1000()
 *    is stock's own formula, kept for the test against the bench dumps.
 *  - The centre constant of the vertical map is a PARAMETER. Stock's raw
 *    baseline (128) is not ours; the caller passes its own.
 *  - ONE interpolation kernel (since the C5 merge): sinc_interp.c's 16-tap
 *    sinc x Kaiser table, blended between its 20 rows for a phase that is
 *    not a row (sinc_interp_frac). The paused view and the running fast-code
 *    zoom therefore draw one record identically; pv_view_plan() even hands a
 *    held record whose R is 1000 / L (L dividing 20) back to the C5 integer
 *    window. Ours is symmetric, stock's skewed ~0.25 sample late; they agree
 *    to ~1 % of full scale per tap (test_paused_view.c), invisible on 8 bits.
 *
 * HOW THE RENDERER USES IT (ui/scope_ui.c view_build)
 * ---------------------------------------------------
 * pv_view_plan() picks the mapping of the one Y-T window (yt_cols): L > 0 is
 * the integer fast-code zoom (scope_geom.c, trigger fitted to 1/L sample,
 * window clamped inside the record), L == 0 the general ratio of a held record
 * (pv_fill: pivot on the trigger column, columns outside the record blank).
 */
#ifndef PAUSED_VIEW_H
#define PAUSED_VIEW_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PV_TAPS        16u
#define PV_RATIO_ONE   1000u          /* ratio_x1000 of an unzoomed view */
#define PV_RATIO_MIN   10u            /* 100x in */
#define PV_RATIO_MAX   1000000u       /* 1000x out */
#define PV_PLAN_MAX_L  20u            /* integer zooms: divisors of 20 (sinc_interp rows) */

/* R = 1000 * fs_cap / fs_now (display time per column over record time per
 * sample). Either rate <= 0 (unknown code) -> PV_RATIO_ONE: no zoom is the
 * honest answer when a rate is not known. Clamped to PV_RATIO_MIN..MAX. */
uint32_t pv_ratio_x1000(float fs_cap, float fs_now);

/* Stock's own formula (0x08030A70): M[tb%3] * 10^(tb/3) is the display time per
 * pixel in units of 0.4 ns (M = 1,2,5); record_period_0p4ns is the u64 table
 * entry at 0x080465E0 (10,10,10,10,20,50,... for tb 0..5); merged doubles it
 * (captured tb < 4 with both channels). */
uint32_t pv_stock_ratio_x1000(uint8_t tb_now, uint32_t record_period_0p4ns,
                              bool merged);

/* 16 symmetric-window sinc taps (sinc_interp_taps: the C5 table, blended) for the output position `frac` (0 <= frac < 1)
 * after input index n+7, i.e. between in[n+7] (frac 0) and in[n+8].
 * Normalised to unity DC gain, so a flat input is reproduced exactly. */
void pv_taps(float frac, float taps[PV_TAPS]);

/* Source position, in 1/1000 sample, of screen column `col` when column
 * `pivot_col` stays put at source index pivot_idx:
 *   pos = pivot_idx * 1000 + (col - pivot_col) * ratio_x1000.
 * (Stock pivots on the trigger / centre column the same way.) */
int32_t pv_src_pos_x1000(int32_t col, int32_t pivot_col, int32_t pivot_idx,
                         uint32_t ratio_x1000);

/* The sample for a column. `rec` is indexed from `lo` (inclusive) to `hi`
 * (exclusive) -- the caller may hand a pointer already offset into the record,
 * so these are in ITS index space (lo may be negative). Returns false (and
 * *out = 0) when the position is outside the record: stock leaves those
 * columns blank, so the caller must skip them.
 *   ratio >= 1000 : the nearest source sample (no peak detect in stock);
 *   ratio <  1000 : the 16-tap interpolation, taps beyond the record clamp
 *                   to its end samples. */
bool pv_sample(const volatile uint8_t *rec, int32_t lo, int32_t hi,
               int32_t pos_x1000, uint32_t ratio_x1000, uint8_t *out);

/* Vertical re-scale of one raw sample about `centre`, closed form of stock's
 * 0x080212BC..0x08021348:
 *   y = centre + pos_now + k * (raw - centre - pos_cap),  k = v_cap / v_now
 * clamped to lo..hi. v_cap / v_now are volts (or mV) per division of the
 * range at capture and now; either <= 0 (uncalibrated) -> k = 1. */
/*
 * Fill out[0..ncols) with columns whose source position is
 *   pos0_x1000 + x * ratio_x1000   (1/1000 sample, rec's index space)
 * through pv_sample(); columns outside [lo, hi) read 0 and are reported as
 * blank: [*first, *end) is the span that shows the record (monotonic, so the
 * blank columns are a prefix and/or a suffix). Both 0 when none does.
 */
void pv_fill(const volatile uint8_t *rec, int32_t lo, int32_t hi,
             int32_t pos0_x1000, uint32_t ratio_x1000,
             uint8_t *out, uint16_t ncols, uint16_t *first, uint16_t *end);

/*
 * The one horizontal mapping of the Y-T window (C5 x C14). L_code is the
 * fast-code zoom of the code on the status bar (1 at codes >= 0x04).
 *   !held (running), or a rate unknown: the C5 window, R = 1000 / L_code,
 *       returns L_code;
 *   held: R = pv_ratio_x1000(fs_rec, fs_col) -- the held record's rate over
 *       the current code's column rate -- and returns the integer zoom L when
 *       R == 1000 / L with L dividing 20 (stopped at, or between, fast codes
 *       of one record rate: still the C5 window), else 0: the general path.
 * *ratio_x1000 (may be NULL) is R either way; the column rate on the glass is
 * 1000 * fs_rec / R, which is what the cursors must read.
 */
uint8_t pv_view_plan(bool held, float fs_rec, float fs_col, uint8_t L_code,
                     uint32_t *ratio_x1000);

int pv_y_map(int raw, int centre, int pos_cap, int pos_now,
             float v_cap, float v_now, int lo, int hi);

#ifdef __cplusplus
}
#endif
#endif /* PAUSED_VIEW_H */
