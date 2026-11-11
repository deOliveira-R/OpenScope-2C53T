/*
 * Horizontal geometry of the Y-T plot (parity C5): which record sample, at
 * which interpolation phase, each screen column shows.
 *
 * STOCK [V] (docs/re/parity2_static.md s3.4-3.5, ADDENDUM_A s3):
 *  - 25 px per division (0x08030A70: ns/px = div_ns / 25), 12 divisions, a
 *    301-column window (the stopped re-render walks r6 = 0..300, 0x08021178);
 *  - the trigger point T at column 150 + hpos, hpos +-150 (0x0803B2F8 clamp);
 *    col x = buf[T + x - hpos - 150] (0x08021232);
 *  - at codes 0x00-0x03 an integer zoom L (scope_timebase_interp_factor)
 *    filled by a sin(x)/x FIR (sinc_interp.h).
 *
 * OURS. Window column x is screen column x (left-aligned: stock's left
 * margin on the LCD is not recorded anywhere [H]; the trigger-level arrow and
 * the trigger badge keep the 19 columns right of it). Positions are counted
 * in 1/L sample units, u = n * L + p: column x shows u0 + x, i.e. sample
 * floor(u / L) at phase u mod L. At L == 1 this is exactly the old
 * buf[toff + x].
 *
 * Pure C: host-built by `make test-scope-geom`.
 */
#ifndef SCOPE_GEOM_H
#define SCOPE_GEOM_H

#include <stdint.h>
#include <stdbool.h>

#define SCOPE_GEOM_DIV_PX       25u     /* stock: 25 samples (at L 1) per div   */
#define SCOPE_GEOM_DIVS         12u
#define SCOPE_GEOM_COLS         301u    /* columns 0..300 = 12 x 25 + 1         */
#define SCOPE_GEOM_CENTER_COL   150     /* stock T column at hpos 0             */
#define SCOPE_GEOM_HPOS_MAX     150     /* trigger column 0..300                */
#define SCOPE_GEOM_TRIG_COL_MIN (SCOPE_GEOM_CENTER_COL - SCOPE_GEOM_HPOS_MAX)
#define SCOPE_GEOM_TRIG_COL_MAX (SCOPE_GEOM_CENTER_COL + SCOPE_GEOM_HPOS_MAX)
#define SCOPE_GEOM_MAX_L        20u     /* the largest zoom (two channels, tb 0) */

typedef struct {
    int32_t u0;         /* column 0's position, in 1/L samples */
    uint8_t L;          /* columns per record sample, 1..20, divides 20 */
} scope_window_t;

/* True for the columns that carry a vertical grid line (0, 25, .., 300). */
static inline bool scope_geom_vgrid(int x)
{
    return x >= 0 && x < (int)SCOPE_GEOM_COLS && (x % (int)SCOPE_GEOM_DIV_PX) == 0;
}

/* Column x -> record sample *n and phase *p (0..L-1). */
void scope_geom_col_src(const scope_window_t *v, int x, int32_t *n, uint8_t *p);

/* The record sample at window column 0 (floor), for `spi3 frame`'s off=. */
int32_t scope_geom_first_sample(const scope_window_t *v);

/*
 * Place the window so the anchor lands on column tx (clamped to 0..300).
 *   anchor   the record index the trigger code chose (first sample past the
 *            level, the sample the L == 1 window has always put on tx);
 *   back_q8  how far before `anchor` the true crossing lies, in 1/256
 *            sample (0..256). Applied only when L > 1: at one column per
 *            sample the anchor sample itself sits on tx, which is the
 *            alignment the mask overlay was taught on.
 *   lo, n_rec  the window's samples must stay inside [lo, n_rec).
 * Returns the column the anchor actually landed on (it moves off tx when
 * the window is clamped at a record edge).
 */
int scope_geom_fit(scope_window_t *v, uint8_t L, int32_t anchor, int32_t back_q8,
                   int tx, int32_t lo, int32_t n_rec);

/* Window start for a crossing search: the samples the window spans with the
 * trigger on column tx are [anchor - before, anchor + after]. */
void scope_geom_span(uint8_t L, int tx, int32_t *before, int32_t *after);

/* How far before `cur` a crossing of `level` between prev and cur lies, in
 * 1/256 sample, by linear interpolation: 0 when cur sits on the level, 256
 * when prev does. Either direction; 0 for a flat pair. */
int32_t scope_geom_back_q8(int prev, int cur, int level);

/*
 * Fill out[0..ncols) with what columns 0..ncols-1 show from rec[0..n_rec).
 * L == 1 reads the record directly; L > 1 copies the few source samples the
 * window needs (<= 301/2 + 17) to the stack once, so the FIR does not make
 * 16 volatile reads of the acquisition buffer per column, then interpolates.
 */
void scope_geom_fill(const volatile uint8_t *rec, int32_t n_rec,
                     const scope_window_t *v, uint8_t *out, uint16_t ncols);

#endif /* SCOPE_GEOM_H */
