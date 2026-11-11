/*
 * scope_selfcal.h — autoset (short AUTO), baseline correction (long AUTO) and
 * 50 % centre (long OK): the decisions, as pure step engines (parity items
 * C9/C10). No RTOS, no registers: tests/test_scope_selfcal.c drives the very
 * code the device runs, on synthetic records and a simulated channel. The
 * device half (fresh records, DAC/relay writes, popups, keys) is
 * src/drivers/scope_selfcal_run.c.
 *
 * Both engines are "observation in, settings out": the caller applies the
 * settings the engine holds (range, position, timebase, DAC codes), waits for
 * a record known to have been read after them, reduces it with
 * sc_rec_stats() and calls the step function again, until it stops returning
 * RUNNING. Every loop is bounded, so a run always ends.
 *
 * RECORD CONVENTIONS (OpenScope, not stock)
 * ---------------------------------------------------------------------------
 * A record byte is clamp(raw - 28) (fpga.c, FPGA_ADC_OFFSET), so it spans
 * 0..227 and both ends are rails: a byte at 0 may be any raw <= 28. The first
 * SCOPE_RECORD_HEAD_SKIP samples are invalid (scope_record.h). With the
 * calibration page in use, 0 V sits at record 100 + pos (commit 113ce11:
 * stock's raw 128 + pos zero reference, ADDENDUM_A S7).
 *
 * AUTOSET — own design. Stock's handler 0x0800CB50 (ms[0x2F] = 1 at
 * 0x0800CC38, precondition ms[0x14] != 0 at 0x0800CC20) walks ranges inside
 * the acquisition function 0x08020E98; that walker is not decoded, so this is
 * not a copy of it. From each record's min/max it predicts the span on every
 * range through the nominal ladder ratios (stock's 0x0804BFB8, via
 * scope_cal_nominal_mv) and takes the most sensitive range on which the
 * signal spans <= 6 divisions and stays inside the rails once centred; it
 * re-measures after each move because a clipped record hides the true span.
 * The timebase comes from the period between rising crossings, aiming for 3
 * periods across the screen (2..4 accepted); the trigger level goes to the
 * signal's midline (50 %) and the position centres it.
 *
 * BASELINE CORRECTION — stock's servo (SYNTHESIS 1.2, ADDENDUM_A S8, ADDENDUM_B
 * S15) without its traps:
 *   - stock servos at pos -100 / +100 onto raw 28 / 228. Raw 28 is record 0,
 *     the clamp rail here, where a mean cannot tell "on target" from
 *     "clipped", so this servos at pos -50 / +50 (record 50 / 150) and
 *     extrapolates both endpoints from the two codes;
 *   - every step acts on a record read after the DAC write and a >= 400 ms
 *     settle (S15's fix, done structurally by the caller) and on the mean of
 *     two such records that agree;
 *   - a clipped or non-flat record is never stepped on; codes are checked
 *     against 0..4095 instead of wrapping in 16 bits (stock's strh) or being
 *     cut to 12 bits (stock's CH1 bfi, the 0xDC5B trap);
 *   - a non-quiet input (a probe still on a signal, or a DC level) refuses
 *     the run instead of being "corrected" into the table;
 *   - results are per-(channel, bank, range) deltas on the page's endpoints,
 *     handed to the caller one entry at a time; nothing here, and nothing in
 *     the device glue, writes flash (persistence is a later, owner decision).
 * Stock's amplitude calibration (long OK with invalid m, 0x0800D954) is not
 * implemented: long OK is always the 50 % centre, so a 0 V "amplitude cal"
 * can never be accepted (calpage_gain() refuses an m outside 148..216 anyway).
 */
#ifndef SCOPE_SELFCAL_H
#define SCOPE_SELFCAL_H

#include <stdint.h>
#include <stdbool.h>

#define SC_CH               2u
#define SC_RANGES           10u
#define SC_TB_CODES         21u
#define SC_REC_MAX          227u   /* clamp(255 - 28): the upper rail */
#define SC_ZERO_REC         100    /* record level of 0 V at pos 0 (page in use) */
#define SC_POS_MAX          100
#define SC_COUNTS_PER_DIV   32     /* OpenScope's graticule (SCOPE_CAL_COUNTS_PER_DIV) */
#define SC_MEAN_N           500u   /* stock's 500-sample mean (0x08022E6C) */

/* ── One record, reduced ───────────────────────────────────────────── */
typedef struct {
    uint8_t  min, max;      /* over the body */
    float    mean;          /* the middle SC_MEAN_N samples of the body */
    bool     clipped;       /* a body sample sits on a rail (0 or SC_REC_MAX) */
    uint16_t n_rise;        /* rising crossings of the midline, with hysteresis */
    float    period;        /* samples per period from those; 0 = fewer than 2 */
    /* Glitch-trimmed view (baseline correction), as fpga.c's ilcal_window:
     * pass 1 takes the mean, pass 2 drops samples more than SC_TRIM counts
     * from it. Interference bursts (10-40 wild samples, ~5 % of a record on
     * unit #2 at tb <= 4) stay out of the spread, the mean and the rails. */
    float    tmean;         /* the middle SC_MEAN_N samples, without the dropped ones */
    uint8_t  spread;        /* p-p of the kept samples without their 8 most extreme per side; 255 = too many dropped */
    uint8_t  tclip;         /* a KEPT sample sits on a rail */
    uint16_t n_out;         /* samples dropped over the whole body */
} sc_rec_stats_t;

#define SC_TRIM             24      /* ilcal's ILCAL_GLITCH */
#define SC_SPREAD_SKIP      8u      /* extreme kept samples ignored per side (~1 % of a record) */
#define SC_TRIM_MAX_PCT     15u     /* ilcal's 77 of 512: more is not a glitch any more */

/* `rec` is a whole record of `len` bytes; the first `head_skip` are ignored.
 * Returns false (and zeroes *out) when the body is shorter than SC_MEAN_N. */
bool sc_rec_stats(const uint8_t *rec, uint16_t len, uint16_t head_skip, sc_rec_stats_t *out);

/* Inverse of fpga_trigger_code_from_level(): the UI level (-100..100) whose
 * reg-0x08 code is `raw`. raw = record + 28. */
int sc_level_for_raw(float raw);

/* ── 50 % centre (long OK) ─────────────────────────────────────────── */
/* Stock 0x0800DA3A: with both channels on (ms[0x14] == 3, ms[0x34] == 0)
 * CH1 goes to +50 and CH2 to -50, otherwise both to 0; the horizontal
 * offsets are zeroed and the trigger level follows the trigger channel's
 * position (ms[0x1C] = ms[4 + src]). Fills pos[2] and returns the trigger
 * level that puts reg 0x08 on that channel's 0 V (raw 128 + pos). */
int sc_center50(bool ch1_on, bool ch2_on, uint8_t trig_src, int16_t pos[SC_CH]);

/* ── Autoset ───────────────────────────────────────────────────────── */
typedef enum {
    SC_AS_RUNNING = 0,
    SC_AS_DONE,         /* at least one channel set up on a signal */
    SC_AS_DC,           /* only DC levels found: ranges/positions set, timebase kept */
    SC_AS_NO_SIGNAL,    /* nothing on any channel: everything restored */
} sc_as_status_t;

typedef enum {          /* per channel */
    SC_ASC_OFF = 0,
    SC_ASC_PENDING,
    SC_ASC_SIGNAL,
    SC_ASC_DC,
    SC_ASC_NONE,        /* flat at the most sensitive usable range: restored */
    SC_ASC_OVER,        /* still clipped on the least sensitive range */
} sc_asc_t;

typedef struct {
    bool         ch_on[SC_CH];
    bool         pos_volts;        /* the page is in use: 0 V = record 100 + pos */
    uint8_t      range_min;        /* most sensitive range with a usable gain */
    const float *mv;               /* SC_RANGES nominal mV/count (ratios only) */
    const float *rate;             /* SC_TB_CODES sample rates, 0 = unusable */
    uint8_t      tb_min, tb_max;   /* codes autoset may choose */
    uint8_t      tb_probe;         /* code for the slow look when all looks flat */
    uint16_t     window;           /* samples across the screen */
    uint8_t      trig_src;         /* current trigger channel, 0/1 */
} sc_as_cfg_t;

typedef struct {
    sc_as_cfg_t cfg;
    /* the settings to apply (the engine's output) */
    uint8_t  range[SC_CH];
    int16_t  pos[SC_CH];
    uint8_t  tb;
    uint8_t  trig_ch;              /* valid when done */
    int16_t  trig_level;           /* valid when done */
    /* state */
    uint8_t  phase, steps, vsteps, hsteps;
    uint8_t  r_lo[SC_CH];          /* most sensitive range not yet seen clipped */
    sc_asc_t res[SC_CH];
    bool     probed;
    bool     h_started, h_done;
    uint8_t  v_pp[SC_CH];          /* p-p the vertical pass sized each channel for */
    uint8_t  orig_range[SC_CH];
    int16_t  orig_pos[SC_CH];
    uint8_t  orig_tb;
} sc_autoset_t;

#define SC_AS_MAX_STEPS   24u   /* whole run */
#define SC_AS_MAX_V       8u    /* per vertical pass */
#define SC_AS_MAX_H       8u
#define SC_AS_PP_MAX      (6 * SC_COUNTS_PER_DIV)   /* <= 6 div */
#define SC_AS_MARGIN      6     /* counts kept off each rail */
#define SC_AS_FLAT_PP     6     /* below this p-p a record is "flat" */
#define SC_AS_DC_MIN      4     /* |offset| from 0 V that is a DC level, counts */
#define SC_AS_DC_DIVS     3     /* a DC level is shown this many divs off centre */
#define SC_AS_MIN_PERIOD  6.0f  /* fewer samples per period: may be aliased */

void sc_autoset_begin(sc_autoset_t *a, const sc_as_cfg_t *cfg,
                      const uint8_t range[SC_CH], const int16_t pos[SC_CH], uint8_t tb);
/* obs[ch] is the record read at the settings a->range/pos/tb now hold; the
 * entry of a channel that is off is ignored. */
sc_as_status_t sc_autoset_step(sc_autoset_t *a, const sc_rec_stats_t obs[SC_CH]);
const char *sc_as_status_str(sc_as_status_t s);

/* ── Baseline correction ───────────────────────────────────────────── */
typedef enum {
    SC_BC_RUNNING = 0,
    SC_BC_DONE,          /* every pair it could reach was corrected or reported */
    SC_BC_NOT_QUIET,     /* input not flat (a signal on it): refused */
    SC_BC_DC_INPUT,      /* a DC level on the input: refused */
    SC_BC_FAILED,        /* nothing could be corrected */
    SC_BC_OPEN,          /* every channel with a page was open/noisy: nothing corrected */
} sc_bc_status_t;

typedef enum {           /* why an entry got no correction */
    SC_BCF_NONE = 0,
    SC_BCF_NO_PAGE,      /* the page has no clean pair for it */
    SC_BCF_CLIPPED,
    SC_BCF_NOISY,        /* consecutive records disagree */
    SC_BCF_NOT_FLAT,
    SC_BCF_DAC_RANGE,    /* the servo wanted a code outside 0..4095 */
    SC_BCF_DIVERGED,
    SC_BCF_NO_CONVERGE,
    SC_BCF_SPAN,         /* the extrapolated pair is implausible */
    SC_BCF_OPEN,         /* skipped: this channel was declared open/noisy earlier */
} sc_bc_fail_t;

/* Per-entry log (one byte each): 0 = not attempted, 1 = corrected,
 * 2 + sc_bc_fail_t = refused. Indexed [ch][bank][range]. */
#define SC_BC_LOG_OK        1u
#define SC_BC_LOG_FAIL0     2u

/* The page's clean pair (after substitution) for (ch 0/1, bank, range). */
typedef bool (*sc_bc_pair_fn)(void *ctx, uint8_t ch, uint8_t bank, uint8_t range,
                              uint16_t *base, uint16_t *upper);
/* A finished entry: the corrected pair is page + (d_base, d_upper). */
typedef void (*sc_bc_commit_fn)(void *ctx, uint8_t ch, uint8_t bank, uint8_t range,
                                int16_t d_base, int16_t d_upper);

typedef struct {
    uint8_t         banks;         /* bit b: walk bank b */
    sc_bc_pair_fn   pair;
    sc_bc_commit_fn commit;
    void           *ctx;
} sc_bc_cfg_t;

/* One walk phase: the bank and the converter routing that select it under
 * stock's rule (calpage_bank_select), with the timebase code to use. */
typedef struct {
    uint8_t bank, mask, tb;
} sc_bc_phase_t;

typedef struct {
    sc_bc_cfg_t cfg;
    /* output: what to apply */
    uint8_t  phase;                /* index into sc_bc_phases[] */
    uint8_t  range;
    uint16_t dac[SC_CH];           /* codes to write while running */
    bool     active[SC_CH];        /* channel is being servoed now */
    /* state */
    uint8_t  stage;                /* pre-check, walk */
    uint8_t  pt;                   /* 0: pos -50, 1: pos +50 */
    uint8_t  nrec;                 /* records of the current step taken */
    uint8_t  retries;
    float    m_first[SC_CH];
    uint16_t base[SC_CH], upper[SC_CH];      /* the page pair of this entry */
    uint16_t conv[SC_CH][2];                 /* converged code per point */
    uint16_t prev_dac[SC_CH];
    float    prev_err[SC_CH];
    bool     have_prev[SC_CH];
    uint8_t  nsteps[SC_CH];
    uint8_t  st[SC_CH];                      /* per entry: running/converged/failed */
    float    pre_err[SC_CH][2];              /* pre-check: r9, r6 offsets */
    uint8_t  n_not_flat;
    uint8_t  run_noisy[SC_CH];               /* consecutive noisy refusals per channel */
    uint8_t  cur_spread[SC_CH];              /* worst trimmed spread seen at this entry */
    uint8_t  cur_drift[SC_CH];               /* worst |record 2 - record 1| x4, capped */
    bool     open_ch[SC_CH];                 /* declared open/noisy: no further entries */
    /* per-entry results, [ch][bank][range]: what `fpga scope selfcal` lists */
    uint8_t  log[SC_CH][3][SC_RANGES];
    uint8_t  log_spread[SC_CH][3][SC_RANGES];   /* worst trimmed spread, counts */
    uint8_t  log_drift[SC_CH][3][SC_RANGES];    /* worst record-to-record mean step x4 */
    /* results */
    uint8_t  n_ok, n_fail;
    sc_bc_fail_t last_fail;
    uint8_t  last_fail_ch, last_fail_bank, last_fail_range;
} sc_basecal_t;

#define SC_BC_PT_POS        50       /* servo points: pos -50 and +50 */
#define SC_BC_TOL           0.5f     /* |mean - target| that is converged, counts */
#define SC_BC_AGREE         1.5f     /* two records of one step must agree this well */
#define SC_BC_RETRIES       3u
#define SC_BC_FLAT_PP       12u      /* max trimmed p-p of a quiet input at the coarse ranges, counts */
#define SC_BC_OPEN_RUN      4u       /* this many noisy refusals in a row: the input is open */
#define SC_BC_COARSE_R      6u       /* range >= this: a not-flat input is a signal, not noise pickup */
#define SC_BC_MAX_STEPS     10u
#define SC_BC_MAX_STEP      400      /* largest single DAC move */
#define SC_BC_SLOPE_MIN     2.0f     /* believable codes per count */
#define SC_BC_SLOPE_MAX     40.0f
#define SC_BC_SPAN_NOM      1580     /* calpage.h CALPAGE_SPAN_NOM */
#define SC_BC_SPAN_TOL      240      /* wider than the page check: this is a measurement */
#define SC_BC_DC_MIN        8.0f     /* pre-check: r6 offset that may be a DC input */
#define SC_BC_NOT_FLAT_MAX  3u       /* this many non-flat entries abort the run */

extern const sc_bc_phase_t sc_bc_phases[4];

/* Range-aware limits. The same input noise is more counts on the sensitive
 * ranges (0.4 mV/count at r0, 400 at r9: a floating or merely noisy input
 * reads 10-1000x more counts there), so the flat limit and the agreement of
 * two records widen with the gain. Coarse ranges keep the original 12 / 1.5. */
unsigned sc_bc_flat_limit(uint8_t range);
float    sc_bc_agree_limit(uint8_t range);
/* The entry's log (see SC_BC_LOG_*); false if out of range. */
bool sc_basecal_entry(const sc_basecal_t *b, uint8_t ch, uint8_t bank, uint8_t range,
                      uint8_t *state, uint8_t *spread, uint8_t *drift_x4);

void sc_basecal_begin(sc_basecal_t *b, const sc_bc_cfg_t *cfg);
/* obs[ch]: the record read at b->phase/range/dac (channels not active are
 * ignored). `fresh_settings` tells the caller whether the next record must
 * follow a settle (true after any change of codes, range or phase). */
sc_bc_status_t sc_basecal_step(sc_basecal_t *b, const sc_rec_stats_t obs[SC_CH],
                               bool *fresh_settings);
/* Pre-check active? (for progress text) */
bool sc_basecal_prechecking(const sc_basecal_t *b);
const char *sc_bc_status_str(sc_bc_status_t s);
const char *sc_bc_fail_str(sc_bc_fail_t f);

/* Apply a correction to a page pair, refusing anything outside 0..4095 or an
 * inverted pair. The device's offset path (scope_vpos) uses this. */
bool sc_corr_pair(uint16_t base, uint16_t upper, int16_t d_base, int16_t d_upper,
                  uint16_t *cb, uint16_t *cu);

#endif /* SCOPE_SELFCAL_H */
