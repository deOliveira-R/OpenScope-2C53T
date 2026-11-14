/*
 * scope_selfcal.c — autoset, baseline correction and 50 % centre: the pure
 * decisions (parity C9/C10). See scope_selfcal.h for what each engine does,
 * why it differs from stock, and the record conventions. Host-tested in
 * tests/test_scope_selfcal.c; the device glue is
 * src/drivers/scope_selfcal_run.c.
 */
#include "scope_selfcal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════
 * Record reduction
 * ═══════════════════════════════════════════════════════════════════ */

bool sc_rec_stats(const uint8_t *rec, uint16_t len, uint16_t head_skip, sc_rec_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    if (rec == NULL || len <= head_skip || (uint16_t)(len - head_skip) < SC_MEAN_N)
        return false;
    const uint16_t n_body = (uint16_t)(len - head_skip);
    uint8_t mn = 255u, mx = 0u;
    for (uint16_t i = head_skip; i < len; i++) {
        if (rec[i] < mn) mn = rec[i];
        if (rec[i] > mx) mx = rec[i];
    }
    /* Stock averages the middle 500 samples (0x08022E6C, and 0x10624DD3 =
     * /500 in the amplitude cal); the middle is furthest from both the head
     * defect and the record's tail. */
    const uint16_t m0 = (uint16_t)(head_skip + (n_body - SC_MEAN_N) / 2u);
    uint32_t sum = 0;
    for (uint16_t i = m0; i < m0 + SC_MEAN_N; i++) sum += rec[i];
    out->min = mn;
    out->max = mx;
    out->mean = (float)sum / (float)SC_MEAN_N;
    out->clipped = (mn == 0u) || (mx >= SC_REC_MAX);

    /* Glitch-trimmed view (see the header): window of +-SC_TRIM counts around
     * the middle-500 mean; spread, rails and the mean use only what is inside. */
    {
        const int c1 = (int)lroundf(out->mean);
        uint32_t ts = 0, tn = 0;
        uint8_t kmn = 255u, kmx = 0u;
        uint16_t n_out = 0, n_in = 0;
        /* The spread skips the SC_SPREAD_SKIP most extreme kept samples on each
         * side: a burst of wild values lands a few samples inside the window
         * by chance, and one such sample must not make a quiet input "not
         * flat". hi[] is sorted descending, lo[] ascending. */
        uint8_t hi[SC_SPREAD_SKIP + 1u], lo[SC_SPREAD_SKIP + 1u];
        memset(hi, 0, sizeof(hi));
        memset(lo, 255, sizeof(lo));
        for (uint16_t i = head_skip; i < len; i++) {
            const int d = (int)rec[i] - c1;
            if (d > SC_TRIM || d < -SC_TRIM) { n_out++; continue; }
            n_in++;
            const uint8_t x = rec[i];
            if (x < kmn) kmn = x;
            if (x > kmx) kmx = x;
            if (x > hi[SC_SPREAD_SKIP]) {
                unsigned j = SC_SPREAD_SKIP;
                while (j > 0u && hi[j - 1u] < x) { hi[j] = hi[j - 1u]; j--; }
                hi[j] = x;
            }
            if (x < lo[SC_SPREAD_SKIP]) {
                unsigned j = SC_SPREAD_SKIP;
                while (j > 0u && lo[j - 1u] > x) { lo[j] = lo[j - 1u]; j--; }
                lo[j] = x;
            }
            if (i >= m0 && i < m0 + SC_MEAN_N) { ts += x; tn++; }
        }
        out->n_out = n_out;
        out->tmean = tn ? (float)ts / (float)tn : out->mean;
        const bool too_many = ((uint32_t)n_out * 100u > (uint32_t)n_body * SC_TRIM_MAX_PCT);
        out->spread = (too_many || n_in <= 2u * SC_SPREAD_SKIP + 1u)
                          ? 255u : (uint8_t)(hi[SC_SPREAD_SKIP] - lo[SC_SPREAD_SKIP]);
        out->tclip = (n_in == 0u) || kmn == 0u || kmx >= SC_REC_MAX;
    }

    /* Rising crossings of the midline with a hysteresis of 1/8 p-p, so noise
     * on a slow edge counts once. Below the flat limit there is no edge to
     * count. */
    if ((int)mx - (int)mn >= SC_AS_FLAT_PP) {
        const float mid = 0.5f * ((float)mn + (float)mx);
        float h = ((float)mx - (float)mn) / 8.0f;
        if (h < 1.0f) h = 1.0f;
        bool armed = false;
        uint16_t xs[SC_XING_MAX];
        uint16_t first = 0, last = 0, n = 0;
        for (uint16_t i = head_skip; i < len; i++) {
            const float s = (float)rec[i];
            if (s < mid - h) {
                armed = true;
            } else if (armed && s > mid + h) {
                armed = false;
                if (n == 0u) first = i;
                if (n < SC_XING_MAX) xs[n] = i;
                last = i;
                n++;
            }
        }
        if (n > SC_XING_MAX) {
            /* Dozens of periods: the average spacing is the period. */
            out->n_rise = n;
            out->period = (float)(last - first) / (float)(n - 1u);
        } else {
            out->n_rise = sc_period_from_crossings(xs, n, head_skip, len, &out->period);
        }
    }
    return true;
}

uint16_t sc_period_from_crossings(uint16_t *xs, uint16_t n, uint16_t head_skip,
                                  uint16_t len, float *period)
{
    *period = 0.0f;
    if (n < 2u) return n;
    /* Edge ringing puts a burst of crossings a few samples apart after an
     * edge: keep one crossing per burst (closer than half the largest gap to
     * the last one kept is the same edge). */
    uint16_t gmax = 0;
    for (uint16_t k = 1; k < n; k++)
        if ((uint16_t)(xs[k] - xs[k - 1u]) > gmax) gmax = (uint16_t)(xs[k] - xs[k - 1u]);
    uint16_t m = 1;
    for (uint16_t k = 1; k < n; k++)
        if ((uint16_t)(xs[k] - xs[m - 1u]) >= gmax / 2u) xs[m++] = xs[k];
    if (m < 2u) return m;
    const float p = (float)(xs[m - 1u] - xs[0]) / (float)(m - 1u);
    /* A period is believed only if it holds over the whole record: evenly
     * spaced crossings, and no stretch at either end longer than 1.5
     * periods without one. A ringing burst after a lone edge fails this. */
    for (uint16_t k = 1; k < m; k++)
        if (fabsf((float)(xs[k] - xs[k - 1u]) - p) > 0.25f * p) return m;
    if ((float)((int)xs[0] - (int)head_skip) > 1.5f * p ||
        (float)((int)len - 1 - (int)xs[m - 1u]) > 1.5f * p)
        return m;
    *period = p;
    return m;
}

int sc_level_for_raw(float raw)
{
    /* fpga_trigger_code_from_level(): code = 128 + level * 256 / 206. */
    float l = (raw - 128.0f) * 206.0f / 256.0f;
    int level = (int)lroundf(l);
    if (level < -100) level = -100;
    if (level > 100)  level = 100;
    return level;
}

int sc_center50(bool ch1_on, bool ch2_on, uint8_t trig_src, const int16_t min_pos[SC_CH],
                int16_t pos[SC_CH])
{
    /* 0x0800DA3A: eor ms[0x14] with 3, or ms[0x34]; zero -> strb #50 to
     * ms[4] and #-50 (mvn #49) to ms[5], else 0 to both. */
    const bool both = ch1_on && ch2_on;
    pos[0] = both ? 50 : 0;
    pos[1] = both ? -50 : 0;
    for (unsigned c = 0; c < SC_CH; c++)
        if (min_pos != NULL && pos[c] < min_pos[c] && min_pos[c] <= 0) pos[c] = min_pos[c];
    /* ms[0x1C] = ms[4 + src]: the trigger sits on the trigger channel's 0 V,
     * which is raw 128 + pos. */
    const int16_t p = pos[trig_src ? 1u : 0u];
    return sc_level_for_raw(128.0f + (float)p);
}

/* ═══════════════════════════════════════════════════════════════════
 * Autoset
 * ═══════════════════════════════════════════════════════════════════ */

enum { AS_V1 = 0, AS_H, AS_V2, AS_FIN };

static int clamp_pos(long p)
{
    if (p < -SC_POS_MAX) return -SC_POS_MAX;
    if (p > SC_POS_MAX)  return SC_POS_MAX;
    return (int)p;
}

void sc_autoset_begin(sc_autoset_t *a, const sc_as_cfg_t *cfg,
                      const uint8_t range[SC_CH], const int16_t pos[SC_CH], uint8_t tb)
{
    memset(a, 0, sizeof(*a));
    a->cfg = *cfg;
    for (unsigned c = 0; c < SC_CH; c++) {
        a->range[c] = a->orig_range[c] = range[c];
        a->pos[c] = a->orig_pos[c] = pos[c];
        a->r_lo[c] = cfg->range_min;
        a->res[c] = cfg->ch_on[c] ? SC_ASC_PENDING : SC_ASC_OFF;
    }
    a->tb = a->orig_tb = tb;
    a->phase = AS_V1;
    a->trig_ch = cfg->trig_src ? 1u : 0u;
}

/* Gain ratio between ranges: counts on `to` per count on `from`. */
static float k_ratio(const sc_autoset_t *a, uint8_t from, uint8_t to)
{
    const float mf = a->cfg.mv[from], mt = a->cfg.mv[to];
    return (mt > 0.0f) ? mf / mt : 0.0f;
}

/* One vertical decision for channel c. Returns true when it changed the
 * channel's settings (so another record is needed), false when the channel
 * is settled. */
static bool as_vertical(sc_autoset_t *a, unsigned c, const sc_rec_stats_t *o)
{
    const uint8_t r = a->range[c];
    if (o->clipped) {
        /* The true span is hidden: everything at or below this range is out.
         * Two steps of the 1-2-5 ladder (~5x) per look; the position goes to
         * 0 so an offset left over from the last guess cannot be the clip. */
        if (r + 1u > a->r_lo[c]) a->r_lo[c] = (uint8_t)(r + 1u);
        if (r >= SC_RANGES - 1u) {
            a->res[c] = SC_ASC_OVER;
            return false;
        }
        uint8_t nr = (uint8_t)(r + 2u);
        if (nr > SC_RANGES - 1u) nr = SC_RANGES - 1u;
        if (nr < a->r_lo[c]) nr = a->r_lo[c];
        a->range[c] = nr;
        if (a->cfg.pos_volts) a->pos[c] = 0;
        a->res[c] = SC_ASC_PENDING;
        return true;
    }

    const float pp = (float)o->max - (float)o->min;
    const float mid = 0.5f * ((float)o->min + (float)o->max);
    /* Offset of the signal's middle from 0 V, in counts on range r. Known
     * only when the page places 0 V at record 100 + pos. */
    const float c0 = a->cfg.pos_volts ? mid - (float)(SC_ZERO_REC + a->pos[c]) : 0.0f;

    if (pp < (float)SC_AS_FLAT_PP) {
        if (a->cfg.pos_volts && fabsf(c0) >= (float)SC_AS_DC_MIN) {
            /* A DC level: the most sensitive range that shows it within
             * SC_AS_DC_DIVS of the centre with the position at 0, so the
             * 0 V marker and the level are both on screen. */
            uint8_t best = SC_RANGES - 1u;
            for (uint8_t nr = a->r_lo[c]; nr < SC_RANGES; nr++) {
                const float cn = c0 * k_ratio(a, r, nr);
                const float lvl = (float)SC_ZERO_REC + cn;
                if (fabsf(cn) <= (float)(SC_AS_DC_DIVS * SC_COUNTS_PER_DIV) &&
                    lvl >= (float)SC_AS_MARGIN && lvl <= (float)(SC_REC_MAX - SC_AS_MARGIN)) {
                    best = nr;
                    break;
                }
            }
            a->res[c] = SC_ASC_DC;
            const bool ch = (best != r) || (a->pos[c] != 0);
            a->range[c] = best;
            a->pos[c] = 0;
            return ch;
        }
        if (r > a->r_lo[c]) {
            /* Nothing measurable here: look on the most sensitive range not
             * yet seen clipped, in one move (stepping would cost a record
             * per range for nothing). */
            a->range[c] = a->r_lo[c];
            a->res[c] = SC_ASC_PENDING;
            return true;
        }
        /* Flat on the most sensitive allowed range. If a more sensitive one
         * clipped, a DC level is there (no page to place it): keep this. */
        a->res[c] = (a->r_lo[c] > a->cfg.range_min) ? SC_ASC_DC : SC_ASC_NONE;
        return false;
    }

    /* A signal: the most sensitive range on which it spans <= 6 div, keeps
     * off both rails, and (page) can be centred by the +-100 position. If
     * no range can centre it (a small signal on a large DC level), the most
     * sensitive one on which it at least fits, off centre. */
    uint8_t best = SC_RANGES - 1u;
    int best_pos = a->pos[c];
    bool found = false;
    for (unsigned pass = 0; pass < 2u && !found; pass++) {
        for (uint8_t nr = a->r_lo[c]; nr < SC_RANGES; nr++) {
            const float k = k_ratio(a, r, nr);
            if (k <= 0.0f) continue;
            const float ppn = pp * k;
            int pn = a->pos[c];
            float midn = mid;
            if (a->cfg.pos_volts) {
                const float cn = c0 * k;
                if (pass == 0u && fabsf(cn) > (float)SC_POS_MAX) continue;
                pn = clamp_pos(lroundf(-cn));
                midn = (float)(SC_ZERO_REC + pn) + cn;
            }
            const float lo = midn - ppn * 0.5f, hi = midn + ppn * 0.5f;
            if (nr == SC_RANGES - 1u) best_pos = pn;   /* fallback: least sensitive */
            if (ppn <= (float)SC_AS_PP_MAX && lo >= (float)SC_AS_MARGIN &&
                hi <= (float)(SC_REC_MAX - SC_AS_MARGIN)) {
                best = nr;
                best_pos = pn;
                found = true;
                break;
            }
        }
        if (!a->cfg.pos_volts) break;   /* the second pass differs only with a page */
    }
    a->v_pp[c] = (uint8_t)((pp * k_ratio(a, r, best) > 255.0f) ? 255u
                                                               : (unsigned)(pp * k_ratio(a, r, best)));
    a->res[c] = SC_ASC_SIGNAL;
    /* A few counts of position are measurement noise, not a reason for
     * another record. */
    const bool changed = (best != r) || (abs(best_pos - a->pos[c]) > 3);
    if (changed) {
        a->range[c] = best;
        a->pos[c] = (int16_t)best_pos;
    }
    return changed;
}

/* Best code for a tone of f Hz: 2 to 3 periods across the window, nearest
 * 2.5 (in ratio); fewer than 2 only if no code gives 2. */
static uint8_t as_code_for_freq(const sc_autoset_t *a, float f)
{
    uint8_t best = a->tb;
    float best_err = 1e30f;
    for (uint8_t c = a->cfg.tb_min; c <= a->cfg.tb_max && c < SC_TB_CODES; c++) {
        const float r = a->cfg.rate[c];
        if (r <= 0.0f) continue;
        const float periods = (float)a->cfg.window * f / r;
        const float e = (periods < 2.0f) ? 1.0f + logf(2.0f / periods)
                                         : fabsf(logf(periods / 2.5f));
        /* Ties (codes sharing one rate) go to the slower code: the same
         * picture with the longer record. */
        if (e < best_err - 1e-6f || (fabsf(e - best_err) <= 1e-6f && c > best)) {
            best_err = e;
            best = c;
        }
    }
    return best;
}

/* The usable code whose rate is nearest (in ratio) to `want`, on one side of
 * the current rate: faster if `faster`, else slower. 0xFF = none. */
static uint8_t as_code_toward(const sc_autoset_t *a, float want, bool faster)
{
    const float cur = a->cfg.rate[a->tb];
    uint8_t best = 0xFFu;
    float best_err = 1e30f;
    for (uint8_t c = a->cfg.tb_min; c <= a->cfg.tb_max && c < SC_TB_CODES; c++) {
        const float r = a->cfg.rate[c];
        if (r <= 0.0f) continue;
        if (faster ? !(r > cur * 1.01f) : !(r < cur * 0.99f)) continue;
        const float e = fabsf(logf(r / want));
        if (e < best_err) { best_err = e; best = c; }
    }
    return best;
}

/* One timebase decision on the trigger channel's record. True = changed.
 *
 * The search starts at the fastest usable code and only ever moves slower
 * until the record holds two periods: a code reached that way had fewer than
 * two periods in the faster record before it, so the tone is far below its
 * Nyquist rate and the estimate cannot be an alias. Starting from the user's
 * code instead would read e.g. 300 kHz at 50 kS/s as a 420 Hz alias. Each
 * slower look is ~8x, so the fastest-to-slowest walk is five records. */
static bool as_horizontal(sc_autoset_t *a, const sc_rec_stats_t *o, unsigned tc)
{
    if (!a->h_started) {
        a->h_started = true;
        uint8_t fastest = a->tb;
        float fr = 0.0f;
        for (uint8_t c = a->cfg.tb_min; c <= a->cfg.tb_max && c < SC_TB_CODES; c++)
            if (a->cfg.rate[c] > fr * 1.01f) { fr = a->cfg.rate[c]; fastest = c; }
        if (fastest != a->tb && fr > 0.0f) {
            a->tb = fastest;
            return true;
        }
    }
    const float cur = (a->tb < SC_TB_CODES) ? a->cfg.rate[a->tb] : 0.0f;
    if (cur <= 0.0f) return false;
    /* The vertical pass sized this channel on its full swing. A record that
     * shows much less of it is a slice of a slow wave, and its "crossings"
     * are noise: treat it as under two periods. */
    const int pp = (int)o->max - (int)o->min;
    int need = (int)a->v_pp[tc] / 4;
    if (need < SC_AS_FLAT_PP) need = SC_AS_FLAT_PP;
    const bool full = pp >= need;
    if (full && o->period >= SC_AS_MIN_PERIOD) {
        const uint8_t next = as_code_for_freq(a, cur / o->period);
        a->h_done = true;
        if (next == a->tb) return false;
        a->tb = next;
        return true;
    }
    if (full && o->period > 0.0f) {
        a->h_done = true;      /* faster than the fastest code can frame */
        return false;
    }
    /* No period that holds over this record (fewer than two periods, or a
     * ringing burst after one edge): look 8x slower. */
    const uint8_t next = as_code_toward(a, cur / 8.0f, false);
    if (next == 0xFFu) {
        a->h_done = true;      /* slower than the slowest code allowed */
        return false;
    }
    a->tb = next;
    return true;
}

static bool as_any(const sc_autoset_t *a, sc_asc_t r)
{
    for (unsigned c = 0; c < SC_CH; c++)
        if (a->res[c] == r) return true;
    return false;
}

/* Trigger channel: the current source if it carries a signal, else the
 * channel with the larger span. */
static int as_pick_trig(const sc_autoset_t *a, const sc_rec_stats_t obs[SC_CH])
{
    const unsigned src = a->cfg.trig_src ? 1u : 0u;
    if (a->res[src] == SC_ASC_SIGNAL || a->res[src] == SC_ASC_OVER) return (int)src;
    int best = -1, best_pp = -1;
    for (unsigned c = 0; c < SC_CH; c++) {
        if (a->res[c] != SC_ASC_SIGNAL && a->res[c] != SC_ASC_OVER) continue;
        const int pp = (int)obs[c].max - (int)obs[c].min;
        if (pp > best_pp) { best_pp = pp; best = (int)c; }
    }
    return best;
}

static sc_as_status_t as_finish(sc_autoset_t *a, const sc_rec_stats_t obs[SC_CH])
{
    a->phase = AS_FIN;
    for (unsigned c = 0; c < SC_CH; c++) {
        if (a->res[c] == SC_ASC_NONE || a->res[c] == SC_ASC_PENDING) {
            a->range[c] = a->orig_range[c];      /* nothing found: as it was */
            a->pos[c] = a->orig_pos[c];
            if (a->res[c] == SC_ASC_PENDING) a->res[c] = SC_ASC_NONE;
        }
    }
    const int tc = as_pick_trig(a, obs);
    if (tc >= 0) {
        a->trig_ch = (uint8_t)tc;
        /* 50 %: reg 0x08 on the midline of the record just measured at the
         * final settings (raw = record + 28). */
        const float mid = 0.5f * ((float)obs[tc].min + (float)obs[tc].max);
        a->trig_level = (int16_t)sc_level_for_raw(mid + 28.0f);
        return SC_AS_DONE;
    }
    a->tb = a->orig_tb;                          /* no period to set it from */
    if (as_any(a, SC_ASC_DC)) {
        a->trig_level = 0;
        return SC_AS_DC;
    }
    for (unsigned c = 0; c < SC_CH; c++) {
        a->range[c] = a->orig_range[c];
        a->pos[c] = a->orig_pos[c];
    }
    return SC_AS_NO_SIGNAL;
}

sc_as_status_t sc_autoset_step(sc_autoset_t *a, const sc_rec_stats_t obs[SC_CH])
{
    if (a->phase == AS_FIN) return SC_AS_DONE;
    if (++a->steps > SC_AS_MAX_STEPS) return as_finish(a, obs);

    for (;;) {
        if (a->phase == AS_V1 || a->phase == AS_V2) {
            bool changed = false;
            for (unsigned c = 0; c < SC_CH; c++) {
                if (!a->cfg.ch_on[c] || a->res[c] == SC_ASC_OFF) continue;
                if (a->phase == AS_V2 && a->res[c] == SC_ASC_NONE) continue;
                if (as_vertical(a, c, &obs[c])) changed = true;
            }
            if (changed && ++a->vsteps <= SC_AS_MAX_V) return SC_AS_RUNNING;
            a->vsteps = 0;
            if (a->phase == AS_V2) return as_finish(a, obs);
            /* End of the first vertical pass. If every channel looks flat,
             * the signal may simply be slow for this record: one look at the
             * probe code before calling it DC. */
            if (!as_any(a, SC_ASC_SIGNAL) && !as_any(a, SC_ASC_OVER) &&
                !a->probed && a->tb != a->cfg.tb_probe) {
                a->probed = true;
                a->tb = a->cfg.tb_probe;
                return SC_AS_RUNNING;
            }
            if (as_pick_trig(a, obs) < 0) return as_finish(a, obs);
            a->phase = AS_H;
            continue;   /* this record also serves the timebase decision */
        }
        if (a->phase == AS_H) {
            const int tc = as_pick_trig(a, obs);
            if (tc >= 0 && !a->h_done && as_horizontal(a, &obs[tc], (unsigned)tc) &&
                ++a->hsteps <= SC_AS_MAX_H)
                return SC_AS_RUNNING;
            a->phase = AS_V2;
            continue;
        }
        return as_finish(a, obs);
    }
}

const char *sc_as_status_str(sc_as_status_t s)
{
    switch (s) {
    case SC_AS_RUNNING:   return "running";
    case SC_AS_DONE:      return "done";
    case SC_AS_DC:        return "DC only";
    case SC_AS_NO_SIGNAL: return "no signal";
    }
    return "?";
}

/* ═══════════════════════════════════════════════════════════════════
 * Baseline correction
 * ═══════════════════════════════════════════════════════════════════ */

/* Stock's walk order (ADDENDUM_A S8): bank 0 then bank 1 with both channels
 * in lockstep, then bank 2 per channel (mask 1, then mask 2). The codes pick
 * each bank under calpage_bank_select(): tb >= 5 -> 0 (stock uses tb 5);
 * tb 4 -> 1; tb <= 3 with one channel -> 2. */
const sc_bc_phase_t sc_bc_phases[4] = {
    { 0u, 3u, 0x05u },
    { 1u, 3u, 0x04u },
    { 2u, 1u, 0x03u },
    { 2u, 2u, 0x03u },
};
#define BC_NPHASE  (sizeof(sc_bc_phases) / sizeof(sc_bc_phases[0]))

enum { BC_PRE = 0, BC_WALK, BC_END };
enum { ST_IDLE = 0, ST_RUN, ST_PT, ST_FAIL };

#define BC_PRE_R_COARSE   9u     /* pre-check ranges: 400 and 40 mV/count nominal, */
#define BC_PRE_R_FINE     6u     /* 10x apart: a real DC level scales by 10 */

/* Trimmed-spread limit and two-record agreement per range (see the header).
 * r0..r9 = 0.4 .. 400 mV/count: noise in counts scales with the gain. The
 * coarse end keeps the original quiet limit; stock has no such check, so the
 * steps in between are OpenScope's own and only widen towards the gain. */
static const uint8_t bc_flat_tab[SC_RANGES]  = { 48, 40, 32, 28, 24, 20, 12, 12, 12, 12 };
static const uint8_t bc_agree_tab[SC_RANGES] = { 6, 6, 5, 5, 4, 3, 3, 3, 3, 3 };   /* x0.5 counts */

unsigned sc_bc_flat_limit(uint8_t range)
{
    return bc_flat_tab[range < SC_RANGES ? range : SC_RANGES - 1u];
}

float sc_bc_agree_limit(uint8_t range)
{
    return 0.5f * (float)bc_agree_tab[range < SC_RANGES ? range : SC_RANGES - 1u];
}

static int bc_point_pos(uint8_t pt) { return pt ? SC_BC_PT_POS : -SC_BC_PT_POS; }

static uint16_t bc_code(uint16_t base, uint16_t upper, int pos)
{
    const float f = (float)base + ((float)upper - (float)base) * (float)(pos + 100) / 200.0f;
    if (f <= 0.0f) return 0u;
    if (f >= 4095.0f) return 4095u;
    return (uint16_t)lroundf(f);
}

static bool bc_phase_has(const sc_bc_phase_t *p, unsigned c)
{
    return (p->mask >> c) & 1u;
}

static void bc_log(sc_basecal_t *b, unsigned c, uint8_t bank, uint8_t range, uint8_t v)
{
    if (bank >= 3u || range >= SC_RANGES) return;
    b->log[c][bank][range] = v;
    b->log_spread[c][bank][range] = b->cur_spread[c];
    b->log_drift[c][bank][range] = b->cur_drift[c];
}

bool sc_basecal_entry(const sc_basecal_t *b, uint8_t ch, uint8_t bank, uint8_t range,
                      uint8_t *state, uint8_t *spread, uint8_t *drift_x4)
{
    if (ch >= SC_CH || bank >= 3u || range >= SC_RANGES) return false;
    if (state)    *state    = b->log[ch][bank][range];
    if (spread)   *spread   = b->log_spread[ch][bank][range];
    if (drift_x4) *drift_x4 = b->log_drift[ch][bank][range];
    return true;
}

static void bc_fail(sc_basecal_t *b, unsigned c, sc_bc_fail_t why)
{
    /* Noise at the sensitive ranges, entry after entry: the input is open
     * (a floating BNC picks up hum that only shows once amplified; the
     * coarse ranges look quiet, so the pre-check cannot see it). Refuse the
     * channel for the rest of the run rather than correct it at r6..r9. */
    if (b->range < SC_BC_COARSE_R && (why == SC_BCF_NOT_FLAT || why == SC_BCF_NOISY)) {
        if (++b->run_noisy[c] >= SC_BC_OPEN_RUN) b->open_ch[c] = true;
    }
    bc_log(b, c, sc_bc_phases[b->phase].bank, b->range, (uint8_t)(SC_BC_LOG_FAIL0 + why));
    b->st[c] = ST_FAIL;
    b->active[c] = false;
    b->n_fail++;
    b->last_fail = why;
    b->last_fail_ch = (uint8_t)c;
    b->last_fail_bank = sc_bc_phases[b->phase].bank;
    b->last_fail_range = b->range;
}

/* Load the page pairs for (phase, range) and the codes for the current
 * point. Returns true if any channel is servoed. */
static bool bc_entry_load(sc_basecal_t *b, int pos)
{
    const sc_bc_phase_t *p = &sc_bc_phases[b->phase];
    bool any = false;
    for (unsigned c = 0; c < SC_CH; c++) {
        b->st[c] = ST_IDLE;
        b->active[c] = false;
        b->cur_spread[c] = 0;
        b->cur_drift[c] = 0;
        if (!bc_phase_has(p, c)) continue;
        if (b->open_ch[c]) {
            if (b->stage == BC_WALK) {
                bc_log(b, c, p->bank, b->range, (uint8_t)(SC_BC_LOG_FAIL0 + SC_BCF_OPEN));
                b->n_fail++;
            }
            continue;
        }
        uint16_t base, upper;
        if (b->cfg.pair == NULL ||
            !b->cfg.pair(b->cfg.ctx, (uint8_t)c, p->bank, b->range, &base, &upper)) {
            if (b->stage == BC_WALK) bc_fail(b, c, SC_BCF_NO_PAGE);
            continue;
        }
        b->base[c] = base;
        b->upper[c] = upper;
        b->dac[c] = bc_code(base, upper, pos);
        b->st[c] = ST_RUN;
        b->active[c] = true;
        b->nsteps[c] = 0;
        b->have_prev[c] = false;
        any = true;
    }
    b->nrec = 0;
    b->retries = 0;
    return any;
}

/* First phase at or after `from` that the caller asked for; BC_NPHASE = none. */
static uint8_t bc_next_phase(const sc_basecal_t *b, uint8_t from)
{
    for (uint8_t i = from; i < BC_NPHASE; i++)
        if (b->cfg.banks & (1u << sc_bc_phases[i].bank)) return i;
    return (uint8_t)BC_NPHASE;
}

/* Start the pre-check of the current phase; skips phases where no channel
 * has a page pair at the pre-check ranges. Returns false when no phase is left. */
static bool bc_phase_start(sc_basecal_t *b)
{
    for (;;) {
        if (b->phase >= BC_NPHASE) return false;
        b->stage = BC_PRE;
        b->pt = 0;
        b->range = BC_PRE_R_COARSE;
        memset(b->pre_err, 0, sizeof(b->pre_err));
        if (bc_entry_load(b, 0)) return true;
        b->range = BC_PRE_R_FINE;
        if (bc_entry_load(b, 0)) return true;
        b->phase = bc_next_phase(b, (uint8_t)(b->phase + 1u));
    }
}

void sc_basecal_begin(sc_basecal_t *b, const sc_bc_cfg_t *cfg)
{
    memset(b, 0, sizeof(*b));
    b->cfg = *cfg;
    b->phase = bc_next_phase(b, 0u);
    if (!bc_phase_start(b)) b->stage = BC_END;
}

bool sc_basecal_prechecking(const sc_basecal_t *b)
{
    return b->stage == BC_PRE;
}

static sc_bc_status_t bc_final(const sc_basecal_t *b)
{
    if (b->n_ok > 0u) return SC_BC_DONE;
    return (b->open_ch[0] || b->open_ch[1]) ? SC_BC_OPEN : SC_BC_FAILED;
}

static sc_bc_status_t bc_end(sc_basecal_t *b)
{
    b->stage = BC_END;
    b->active[0] = b->active[1] = false;
    return bc_final(b);
}

/* Next walk entry (range, then phase). Returns RUNNING or the end status. */
static sc_bc_status_t bc_advance(sc_basecal_t *b)
{
    for (;;) {
        if (b->stage == BC_WALK) {
            if (b->range + 1u < SC_RANGES) {
                b->range++;
            } else {
                b->phase = bc_next_phase(b, (uint8_t)(b->phase + 1u));
                if (!bc_phase_start(b)) return bc_end(b);
                return SC_BC_RUNNING;
            }
        } else {
            return bc_end(b);
        }
        b->pt = 0;
        if (bc_entry_load(b, bc_point_pos(0))) return SC_BC_RUNNING;
    }
}

/* Both points converged: extrapolate the endpoints and hand them over.
 * DAC(pos) = base + (upper - base)(pos + 100)/200, so with dl at -50 and dh
 * at +50: upper - base = 2(dh - dl), base = dl - (dh - dl)/2. */
static void bc_finish_entry(sc_basecal_t *b, unsigned c)
{
    const float dl = (float)b->conv[c][0], dh = (float)b->conv[c][1];
    const long nb = lroundf(dl - (dh - dl) * 0.5f);
    const long nu = lroundf(dh + (dh - dl) * 0.5f);
    const long span = nu - nb;
    if (nb < 0 || nu > 4095 || span <= 0 ||
        labs(span - SC_BC_SPAN_NOM) > SC_BC_SPAN_TOL) {
        bc_fail(b, c, SC_BCF_SPAN);
        return;
    }
    if (b->cfg.commit)
        b->cfg.commit(b->cfg.ctx, (uint8_t)c, sc_bc_phases[b->phase].bank, b->range,
                      (int16_t)(nb - (long)b->base[c]), (int16_t)(nu - (long)b->upper[c]));
    b->n_ok++;
    b->run_noisy[c] = 0;
    bc_log(b, c, sc_bc_phases[b->phase].bank, b->range, SC_BC_LOG_OK);
    b->st[c] = ST_IDLE;
    b->active[c] = false;
}

/* One servo step for channel c on mean m. Returns true if the code moved. */
static bool bc_servo(sc_basecal_t *b, unsigned c, float m)
{
    const float target = (float)(SC_ZERO_REC + bc_point_pos(b->pt));
    const float e = m - target;
    if (fabsf(e) <= SC_BC_TOL) {
        b->conv[c][b->pt] = b->dac[c];
        b->st[c] = ST_PT;
        b->active[c] = false;
        return false;
    }
    if (b->have_prev[c] && fabsf(e) > fabsf(b->prev_err[c]) * 1.5f + 1.0f) {
        bc_fail(b, c, SC_BCF_DIVERGED);   /* wrong sign or a moving input */
        return false;
    }
    /* Codes per count: the page's own slope first (span / 200 positions,
     * one count per position), then the secant of the last move when it is
     * believable. Stock steps -8e blindly (0x08022E80..); that is what laps
     * on a clipped mean. */
    float s = ((float)b->upper[c] - (float)b->base[c]) / 200.0f;
    if (b->have_prev[c]) {
        const float dm = e - b->prev_err[c];
        const float dd = (float)b->dac[c] - (float)b->prev_dac[c];
        if (fabsf(dm) >= 1.0f && dd != 0.0f) {
            const float sm = dd / dm;
            if (sm >= SC_BC_SLOPE_MIN && sm <= SC_BC_SLOPE_MAX) s = sm;
        }
    }
    if (s < SC_BC_SLOPE_MIN || s > SC_BC_SLOPE_MAX) {
        bc_fail(b, c, SC_BCF_SPAN);
        return false;
    }
    long step = lroundf(-e * s);
    if (step == 0) step = (e > 0.0f) ? -1 : 1;
    if (step > SC_BC_MAX_STEP)  step = SC_BC_MAX_STEP;
    if (step < -SC_BC_MAX_STEP) step = -SC_BC_MAX_STEP;
    const long nd = (long)b->dac[c] + step;
    if (nd < 0 || nd > 4095) {
        bc_fail(b, c, SC_BCF_DAC_RANGE);  /* never wrap, never cut to 12 bits */
        return false;
    }
    if (++b->nsteps[c] > SC_BC_MAX_STEPS) {
        bc_fail(b, c, SC_BCF_NO_CONVERGE);
        return false;
    }
    b->prev_dac[c] = b->dac[c];
    b->prev_err[c] = e;
    b->have_prev[c] = true;
    b->dac[c] = (uint16_t)nd;
    return true;
}

sc_bc_status_t sc_basecal_step(sc_basecal_t *b, const sc_rec_stats_t obs[SC_CH],
                               bool *fresh_settings)
{
    bool dummy;
    if (fresh_settings == NULL) fresh_settings = &dummy;
    *fresh_settings = false;
    if (b->stage == BC_END) return bc_final(b);

    const bool pre = (b->stage == BC_PRE);
    const unsigned flat_pp = pre ? SC_BC_FLAT_PP : sc_bc_flat_limit(b->range);
    const float agree = pre ? SC_BC_AGREE : sc_bc_agree_limit(b->range);

    /* Every record, first or second, must be quiet and unclipped. */
    for (unsigned c = 0; c < SC_CH; c++) {
        if (!b->active[c]) continue;
        const sc_rec_stats_t *o = &obs[c];
        const unsigned pp = o->spread;   /* glitch-trimmed: bursts are not "not flat" */
        if (o->spread != 255u && o->spread > b->cur_spread[c]) b->cur_spread[c] = o->spread;
        else if (o->spread == 255u) b->cur_spread[c] = 255u;
        if (pre) {
            /* Coarse ranges, page codes at pos 0: a signal or a big DC level
             * shows as a spread or a rail. The whole run is refused. */
            if (o->tclip || pp > SC_BC_FLAT_PP) {
                b->stage = BC_END;
                b->active[0] = b->active[1] = false;
                return SC_BC_NOT_QUIET;
            }
            continue;
        }
        if (o->tclip) {
            bc_fail(b, c, SC_BCF_CLIPPED);
        } else if (pp > flat_pp) {
            bc_fail(b, c, SC_BCF_NOT_FLAT);
            /* Coarse ranges do not see pickup: a not-flat input there is a
             * signal. (Sensitive ranges feed the open-input rule in bc_fail.) */
            if (b->range >= SC_BC_COARSE_R && ++b->n_not_flat >= SC_BC_NOT_FLAT_MAX) {
                b->stage = BC_END;
                b->active[0] = b->active[1] = false;
                return SC_BC_NOT_QUIET;
            }
        }
    }

    bool any_active = b->active[0] || b->active[1];
    if (any_active) {
        /* Two records per step, which must agree: hum or a slow drift moves
         * the mean between records even when each one is flat. */
        if (b->nrec == 0u) {
            for (unsigned c = 0; c < SC_CH; c++) b->m_first[c] = obs[c].tmean;
            b->nrec = 1;
            return SC_BC_RUNNING;
        }
        bool disagree = false, bad[SC_CH] = { false, false };
        for (unsigned c = 0; c < SC_CH; c++) {
            if (!b->active[c]) continue;
            const float dm = fabsf(obs[c].tmean - b->m_first[c]);
            const float d4 = 4.0f * dm;
            const uint8_t u = (d4 > 255.0f) ? 255u : (uint8_t)d4;
            if (u > b->cur_drift[c]) b->cur_drift[c] = u;
            if (dm > agree) { disagree = true; bad[c] = true; }
        }
        if (disagree) {
            if (++b->retries > SC_BC_RETRIES) {
                if (pre) {
                    b->stage = BC_END;
                    b->active[0] = b->active[1] = false;
                    return SC_BC_NOT_QUIET;
                }
                /* Only the channel that wandered is noisy: blaming a quiet
                 * channel too would feed its open-input streak (bc_fail) and
                 * abandon a shorted CH1 because CH2 floats. The other one
                 * agreed on this pair and servoes on it below. */
                for (unsigned c = 0; c < SC_CH; c++)
                    if (b->active[c] && bad[c]) bc_fail(b, c, SC_BCF_NOISY);
            } else {
                for (unsigned c = 0; c < SC_CH; c++) b->m_first[c] = obs[c].tmean;
                return SC_BC_RUNNING;
            }
        }
        b->nrec = 0;
        b->retries = 0;
    }

    if (pre) {
        const unsigned slot = (b->range == BC_PRE_R_COARSE) ? 0u : 1u;
        for (unsigned c = 0; c < SC_CH; c++)
            if (b->active[c])
                b->pre_err[c][slot] = 0.5f * (obs[c].tmean + b->m_first[c]) - (float)SC_ZERO_REC;
        if (b->range == BC_PRE_R_COARSE) {
            b->range = BC_PRE_R_FINE;
            if (bc_entry_load(b, 0)) { *fresh_settings = true; return SC_BC_RUNNING; }
        }
        /* A DC level on the input scales with the gain: 10x from r9 to r6.
         * The page's own offset errors do not. */
        for (unsigned c = 0; c < SC_CH; c++) {
            const float e9 = b->pre_err[c][0], e6 = b->pre_err[c][1];
            if (fabsf(e6) >= SC_BC_DC_MIN && fabsf(e9) >= 0.5f &&
                (e6 > 0.0f) == (e9 > 0.0f) && fabsf(e6) >= 4.0f * fabsf(e9)) {
                b->stage = BC_END;
                b->active[0] = b->active[1] = false;
                return SC_BC_DC_INPUT;
            }
        }
        b->stage = BC_WALK;
        b->range = 0;
        b->pt = 0;
        *fresh_settings = true;
        if (bc_entry_load(b, bc_point_pos(0))) return SC_BC_RUNNING;
        return bc_advance(b);
    }

    bool moved = false;
    for (unsigned c = 0; c < SC_CH; c++) {
        if (!b->active[c]) continue;
        const float m = 0.5f * (obs[c].tmean + b->m_first[c]);
        if (bc_servo(b, c, m)) moved = true;
    }
    if (b->active[0] || b->active[1]) {
        *fresh_settings = moved;
        return SC_BC_RUNNING;
    }

    /* No channel is still servoing this point. */
    if (b->pt == 0u) {
        bool any = false;
        b->pt = 1;
        for (unsigned c = 0; c < SC_CH; c++) {
            if (b->st[c] != ST_PT) continue;
            b->st[c] = ST_RUN;
            b->active[c] = true;
            b->dac[c] = bc_code(b->base[c], b->upper[c], bc_point_pos(1));
            b->nsteps[c] = 0;
            b->have_prev[c] = false;
            any = true;
        }
        b->nrec = 0;
        b->retries = 0;
        if (any) { *fresh_settings = true; return SC_BC_RUNNING; }
    } else {
        for (unsigned c = 0; c < SC_CH; c++)
            if (b->st[c] == ST_PT) bc_finish_entry(b, c);
    }
    *fresh_settings = true;
    return bc_advance(b);
}

bool sc_corr_pair(uint16_t base, uint16_t upper, int16_t d_base, int16_t d_upper,
                  uint16_t *cb, uint16_t *cu)
{
    const long nb = (long)base + d_base, nu = (long)upper + d_upper;
    if (nb < 0 || nu > 4095 || nu <= nb) return false;
    *cb = (uint16_t)nb;
    *cu = (uint16_t)nu;
    return true;
}

const char *sc_bc_status_str(sc_bc_status_t s)
{
    switch (s) {
    case SC_BC_RUNNING:   return "running";
    case SC_BC_DONE:      return "done";
    case SC_BC_NOT_QUIET: return "refused: input not quiet (remove the probe)";
    case SC_BC_DC_INPUT:  return "refused: DC level on the input";
    case SC_BC_FAILED:    return "failed: nothing corrected";
    case SC_BC_OPEN:      return "refused: input open/noisy (short or ground it)";
    }
    return "?";
}

const char *sc_bc_fail_str(sc_bc_fail_t f)
{
    switch (f) {
    case SC_BCF_NONE:        return "-";
    case SC_BCF_NO_PAGE:     return "no page pair";
    case SC_BCF_CLIPPED:     return "clipped";
    case SC_BCF_NOISY:       return "records disagree";
    case SC_BCF_NOT_FLAT:    return "not flat";
    case SC_BCF_DAC_RANGE:   return "code outside 0..4095";
    case SC_BCF_DIVERGED:    return "diverged";
    case SC_BCF_NO_CONVERGE: return "no convergence";
    case SC_BCF_SPAN:        return "implausible span";
    case SC_BCF_OPEN:        return "skipped: input open/noisy";
    }
    return "?";
}
