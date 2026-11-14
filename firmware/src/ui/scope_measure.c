/*
 * OpenScope 2C53T — honest measurements over a raw capture record.
 * See scope_measure.h for what is computed and, more importantly, what is
 * deliberately not.
 *
 * Pure C, no hardware, no floats-in-printf: host-testable as-is, and the host
 * test (tests/test_scope_measure.c) compiles THIS file rather than a
 * transcription of it.
 */

#include "scope_measure.h"

#include "scope_cursor.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Sub-sample position where the trace crossed the mid-level (mid2 = min+max,
 * compared against 2*s) on the edge the Schmitt gate just confirmed at index
 * i. Walks back to the bracketing pair, bounded by the length of one edge.
 * false when the record's start is reached while still on the far side (the
 * pulse began before the record, so it is not a genuine crossing). */
static bool mid_crossing(const uint8_t *s, uint16_t i, uint16_t mid2,
                         bool rising, float *pos)
{
    int j = (int)i - 1;
    if (rising) {
        while (j >= 0 && (uint16_t)(2u * s[j]) >= mid2) j--;
        if (j < 0) return false;
        float a = (float)(2u * s[j]), b = (float)(2u * s[j + 1]);
        *pos = (float)j + ((float)mid2 - a) / (b - a);
    } else {
        while (j >= 0 && (uint16_t)(2u * s[j]) < mid2) j--;
        if (j < 0) return false;
        float a = (float)(2u * s[j]), b = (float)(2u * s[j + 1]);
        *pos = (float)j + (a - (float)mid2) / (a - b);
    }
    return true;
}

void scope_measure_record(const uint8_t *samples, uint16_t n,
                          scope_measure_t *out)
{
    if (out == NULL)
        return;

    memset(out, 0, sizeof(*out));

    if (samples == NULL || n < 2)
        return;                     /* out->valid stays false */

    /* ── Pass 1: extrema and the two integer accumulators ──────────────
     *
     * Integer accumulation is exact here and cheap: with n <= 65535 and
     * samples <= 255, sum <= 1.6e7 (fits uint32) and sum_sq <= 4.3e9, which
     * needs the uint64. Doing this in float would lose bits at the top of
     * the range for long records (24-bit mantissa vs sum_sq up to 2^32). */
    uint8_t  smin = samples[0];
    uint8_t  smax = samples[0];
    uint64_t sum = 0;
    uint64_t sum_sq = 0;
    uint16_t hist[256];
    memset(hist, 0, sizeof(hist));

    for (uint16_t i = 0; i < n; i++) {
        uint8_t s = samples[i];
        if (s < smin) smin = s;
        if (s > smax) smax = s;
        sum    += s;
        sum_sq += (uint64_t)s * (uint64_t)s;
        hist[s]++;
    }

    out->valid = true;
    out->rob_min = smin;            /* refined by the trim walk below */
    out->rob_max = smax;
    out->min   = smin;
    out->max   = smax;
    out->pp    = (uint8_t)(smax - smin);
    out->mean  = (float)sum / (float)n;

    /* Trimmed extremes, filled by the robust-pp walk below and reused by the
     * rise/fall edge references in pass 4. Default to the raw extremes so they
     * are always defined. */
    uint8_t rob_lo = smin, rob_hi = smax;

    /* Robust peak-to-peak: walk the histogram in from both ends until 0.5%
     * of the samples (n/200, at least 1) have been discarded on each side,
     * then take the span of what remains. See the header for why: raw
     * max-min is inflated by the noise tails, measured at +4..+10% of a
     * commanded amplitude in EXP-19. O(256), no second pass over samples. */
    {
        uint16_t trim = (uint16_t)(n / 200u);
        if (trim == 0u) trim = 1u;

        uint16_t lo = smin, hi = smax;
        uint32_t acc = 0;
        while (lo < hi && acc + hist[lo] <= trim) { acc += hist[lo]; lo++; }
        acc = 0;
        while (hi > lo && acc + hist[hi] <= trim) { acc += hist[hi]; hi--; }
        out->pp_robust = (uint8_t)(hi - lo);
        rob_lo = (uint8_t)lo;
        rob_hi = (uint8_t)hi;
        out->rob_min = rob_lo;
        out->rob_max = rob_hi;
    }

    /* Variance about the mean, computed as E[x^2] - E[x]^2 in integer form
     * (n*sum_sq - sum^2) / n^2 so the subtraction happens before any
     * rounding. Clamped at 0 against a -0.0 from a perfectly flat record. */
    {
        double num = (double)n * (double)sum_sq - (double)sum * (double)sum;
        double var = num / ((double)n * (double)n);
        if (var < 0.0) var = 0.0;
        out->ac_rms = sqrtf((float)var);
    }

    out->level_valid = (out->pp >= SCOPE_MEASURE_MIN_PP);
    if (!out->level_valid)
        return;                     /* duty/cycles would be counting noise */

    /* Top and base: the dwell levels, i.e. the histogram mode of each half
     * about the mid-level (ties go to the extreme-most bin). Restricted to the
     * trimmed span so a stray outlier cannot be a "level". */
    {
        uint16_t mid2 = (uint16_t)smin + (uint16_t)smax;
        uint16_t bt = 0, bb = 0;        /* best counts */
        out->top = rob_hi;
        out->base = rob_lo;
        for (uint16_t v = rob_lo; v <= rob_hi; v++) {
            if (hist[v] == 0u) continue;
            if ((uint16_t)(2u * v) > mid2) {
                if (hist[v] >= bt) { bt = hist[v]; out->top = (uint8_t)v; }
            } else {
                if (hist[v] > bb) { bb = hist[v]; out->base = (uint8_t)v; }
            }
        }
    }

    /* ── Pass 2: duty cycle ────────────────────────────────────────────
     *
     * Fraction of the record spent above the mid-level, mid = (min+max)/2.
     * Both the threshold and the comparison are order-preserving under any
     * affine counts->volts mapping, so this number is already correct in the
     * calibrated world — it does not need the calibration to exist.
     *
     * The mid-level is computed in halves (mid2 = min+max, compared against
     * 2*s) so no rounding is introduced for odd min+max. */
    /* The mid-level and the hysteresis of passes 2 and 3 come from the
     * TRIMMED span [rob_lo, rob_hi], like the levels and the 10/90 points:
     * one-sample outliers at the edges (bench 2026-11-14: a 93..195 square
     * with samples near code 0, raw pp 192 against robust 102) move the
     * raw extremes, and with them the thresholds, so the Schmitt trigger
     * re-armed on an outlier and counted one edge twice (host test: periods
     * of 3 samples from a 499-sample square). */
    const uint16_t mid2_r = (uint16_t)rob_lo + (uint16_t)rob_hi;
    {
        uint16_t mid2 = mid2_r;
        uint32_t above = 0;
        for (uint16_t i = 0; i < n; i++) {
            if ((uint16_t)(2u * (uint16_t)samples[i]) > mid2)
                above++;
        }
        out->duty_pct = (float)above * 100.0f / (float)n;
    }

    /* ── Pass 3: cycles within the record ──────────────────────────────
     *
     * Count low->high transitions of a Schmitt trigger centred on the
     * mid-level, with hysteresis of pp/8 (>= 1 count) so a noisy trace does
     * not chatter across the threshold. Each such transition is one full
     * cycle of whatever is on screen.
     *
     * This is a COUNT, not a frequency. Turning it into Hz needs the sample
     * rate; when a timebase exists, frequency = cycles * rate / n and this
     * field is what to multiply. */
    {
        uint16_t mid2  = mid2_r;
        uint16_t hyst2 = (uint16_t)(out->pp_robust / 4u);   /* 2 * (pp/8)   */
        if (hyst2 < 2u) hyst2 = 2u;                  /* 2 * 1 count minimum */

        uint16_t hi_thr2 = (uint16_t)(mid2 + hyst2);
        uint16_t lo_thr2 = (mid2 > hyst2) ? (uint16_t)(mid2 - hyst2) : 0u;

        int state = 0;              /* 0 = below/unknown, 1 = above */
        uint16_t rising = 0;
        float    pc_first = 0.0f, pc_last = 0.0f; /* interpolated crossings   */
        uint16_t pc_count = 0;                    /* genuine crossings only   */
        uint16_t gr_first = 0, gr_last = 0;       /* their integer indices    */
        float    pc_prev = 0.0f, rmin = 0.0f, rmax = 0.0f;   /* rise intervals */
        float    fc_first = 0.0f, fc_last = 0.0f, fc_prev = 0.0f, fmin = 0.0f, fmax = 0.0f;
        uint16_t fc_count = 0;                    /* genuine falling crossings */
        uint16_t prev_s2 = (uint16_t)(2u * (uint16_t)samples[0]);

        /* Pulse widths (Wid+/Wid-): mid-level crossings of each confirmed
         * edge, paired with the previous genuine edge of the other sense. */
        float    edge_pos = 0.0f;
        int      edge_kind = 0;               /* 0 none, 1 rise, 2 fall      */
        float    hi_sum = 0.0f, lo_sum = 0.0f;
        uint16_t hi_n = 0, lo_n = 0;

        for (uint16_t i = 0; i < n; i++) {
            uint16_t s2 = (uint16_t)(2u * (uint16_t)samples[i]);
            if (state == 0) {
                if (s2 >= hi_thr2) {
                    state = 1;
                    {
                        float cp;
                        if (mid_crossing(samples, i, mid2, true, &cp)) {
                            if (edge_kind == 2) { lo_sum += cp - edge_pos; lo_n++; }
                            edge_pos = cp; edge_kind = 1;
                        } else {
                            edge_kind = 0;
                        }
                    }
                    rising++;

                    /* Sub-sample crossing (issue #26). The integer index i is
                     * the first sample AT OR ABOVE hi_thr2 — rounded up to the
                     * sample after the true crossing. That sub-sample bias is
                     * the same on the first and last edge only on AVERAGE; with
                     * few periods in the window it does not cancel, the span
                     * reads short, and the period reads low (Stlkv measured
                     * -0.4% at ~40 periods worsening to -5% at ~6, #18). Locate
                     * where the signal actually crossed hi_thr2 between i-1 and
                     * i and use that fractional index for the span instead.
                     *
                     * A rise at i == 0 is the record starting mid-high-phase,
                     * not a threshold crossing: it still counts as a cycle
                     * (rising++ above) but it must NOT anchor the period, or its
                     * integer index would be mixed with interpolated ones and
                     * reintroduce a whole-sample error. Every rise at i > 0 is a
                     * genuine crossing (the previous sample was below hi_thr2). */
                    if (i > 0u && s2 > prev_s2) {
                        float frac = (float)((int)hi_thr2 - (int)prev_s2) /
                                     (float)((int)s2 - (int)prev_s2);
                        float cross = (float)(i - 1u) + frac;
                        if (pc_count == 0u) { pc_first = cross; gr_first = i; }
                        else {
                            const float d = cross - pc_prev;
                            if (pc_count == 1u || d < rmin) rmin = d;
                            if (pc_count == 1u || d > rmax) rmax = d;
                        }
                        pc_prev = cross;
                        pc_last = cross;
                        gr_last = i;
                        pc_count++;
                    }
                }
            } else {
                if (s2 <= lo_thr2) {
                    state = 0;
                    /* The falling twin of the sub-sample rise above: a second,
                     * independent period estimate for the few-period check. */
                    if (i > 0u && s2 < prev_s2) {
                        const float cross = (float)(i - 1u) +
                            (float)((int)prev_s2 - (int)lo_thr2) / (float)((int)prev_s2 - (int)s2);
                        if (fc_count == 0u) fc_first = cross;
                        else {
                            const float d = cross - fc_prev;
                            if (fc_count == 1u || d < fmin) fmin = d;
                            if (fc_count == 1u || d > fmax) fmax = d;
                        }
                        fc_prev = cross;
                        fc_last = cross;
                        fc_count++;
                    }
                    float cp;
                    if (mid_crossing(samples, i, mid2, false, &cp)) {
                        if (edge_kind == 1) { hi_sum += cp - edge_pos; hi_n++; }
                        edge_pos = cp; edge_kind = 2;
                    } else {
                        edge_kind = 0;
                    }
                }
            }
            prev_s2 = s2;
        }

        if (hi_n > 0u) { out->wid_hi_samples = hi_sum / (float)hi_n; out->wid_hi_valid = true; }
        if (lo_n > 0u) { out->wid_lo_samples = lo_sum / (float)lo_n; out->wid_lo_valid = true; }

        /* One crossing only tells you where the record happened to start
         * relative to the waveform. A period is the interval BETWEEN
         * crossings, so two are the minimum for any cycle statement. */
        out->cycles = rising;
        out->cycles_valid = (rising >= 2u);

        if (out->cycles_valid) {
            /* Period measured the accurate way: the span from the first to
             * the last GENUINE crossing (interpolated, i > 0), divided by the
             * number of whole periods in that span. Units are SAMPLES — the
             * only horizontal unit this instrument can honestly quote.
             *
             * Few periods (bench 2026-11-14): with 1.5..3 periods in the
             * record one bad crossing is a wrong period, not a small error,
             * so the answer is "no period" unless the evidence agrees:
             *   - two genuine crossings of one sense at least (a rise at
             *     i == 0 is where the record starts, not an edge: the old
             *     fallback divided the distance from sample 0 to the next
             *     rise and read 0.6..0.9 of the period at 1.5 periods);
             *   - every interval within SCOPE_MEASURE_PERIOD_TOL of every
             *     other, rises and falls alike;
             *   - when both senses give a period, they agree as closely;
             *   - the number of edges in the whole record fits the period. */
            const float tol = SCOPE_MEASURE_PERIOD_TOL;
            float pr = 0.0f, pf = 0.0f;
            bool ok = true;
            if (pc_count >= 2u) {
                pr = (pc_last - pc_first) / (float)(pc_count - 1u);
                if (pc_count >= 3u && rmax > rmin * (1.0f + tol)) ok = false;
            }
            if (fc_count >= 2u) {
                pf = (fc_last - fc_first) / (float)(fc_count - 1u);
                if (fc_count >= 3u && fmax > fmin * (1.0f + tol)) ok = false;
            }
            if (pr > 0.0f && pf > 0.0f &&
                (pr > pf * (1.0f + tol) || pf > pr * (1.0f + tol))) ok = false;
            if (pr <= 0.0f && pf <= 0.0f) ok = false;
            /* ... and the period must account for the edges the whole record
             * shows: n / period cycles fit in it, so the Schmitt rises
             * (the one at i == 0 included) number between n/P - 1 and
             * n/P + 2. Two crossings a few samples apart (an outlier pair
             * inside one edge) give a "period" the record contradicts. */
            if (ok) {
                const float per = (pr > 0.0f) ? pr : pf;
                const float fit = (float)n / per;
                if ((float)rising < fit - 1.0f || (float)rising > fit + 2.0f) ok = false;
            }
            out->period_samples = ok ? (pr > 0.0f ? pr : pf) : 0.0f;
            out->period_valid = ok && (out->period_samples > 0.0f);

            /* ── Duty, recomputed over WHOLE CYCLES ────────────────────
             *
             * BENCH 2026-08-14. Pass 2 measures duty across the entire
             * record, which is only unbiased when the record holds an
             * integer number of cycles. It does not: a 2 Hz square at the
             * current acquisition rate gives ~600 samples/cycle in a 1024
             * sample buffer = ~1.7 cycles, so the leftover 0.7 cycle drags
             * the answer around with the window phase. Measured on a
             * commanded 24.7% square, the badge wandered across roughly
             * 20-40% — and the predicted phase-bias range for that record
             * is 14-29%, which matches.
             *
             * [first genuine rise, last genuine rise) spans whole
             * cycles by construction, so duty over that span is
             * phase-independent. Still affine-invariant, so it remains
             * correct before any calibration exists.
             *
             * NOTE this does not explain readings near 50% on that signal,
             * which phase bias alone cannot produce. That, and the cycle
             * detector intermittently finding nothing, point at the record
             * not being a uniform-rate time series — the open timebase
             * question (dev plan F4), not an arithmetic problem here. */
            /* Over GENUINE rises only (a rise at i == 0 is not a cycle
             * boundary); with fewer than two the whole-record duty stays. */
            const uint16_t d0 = (pc_count >= 2u) ? gr_first : 0u;
            const uint16_t d1 = (pc_count >= 2u) ? gr_last : 0u;
            uint32_t above_c = 0;
            for (uint16_t i = d0; i < d1; i++) {
                if ((uint16_t)(2u * (uint16_t)samples[i]) > mid2)
                    above_c++;
            }
            uint16_t span = (uint16_t)(d1 - d0);
            if (span > 0u)
                out->duty_pct = (float)above_c * 100.0f / (float)span;
        }
    }

    /* ── Pass 4: edge rise / fall time, in SAMPLES ─────────────────────
     *
     * 10%->90% on the first clean rising edge, 90%->10% on the first clean
     * falling edge. This retires src/tasks/measurement.c: rise/fall time was
     * the only quantity that engine computed which this module did not, and it
     * did it in fabricated volts-and-seconds (a hardcoded 3.3/32768 scale and a
     * caller-supplied sample_rate this board does not have). Here it is in the
     * unit the instrument actually observes.
     *
     * The 10/90 references come from the TRIMMED span [rob_lo, rob_hi], not the
     * raw extremes, so a handful of noise-tail samples cannot stretch the
     * levels and shorten every edge — the same reasoning as pp_robust. Each
     * crossing is linearly interpolated between its bracketing samples;
     * (level - prev) / (cur - prev) is correct for both directions of travel.
     * Only the first edge of each kind is reported: with an unknown, possibly
     * sub-cycle window, averaging edges of different quality would be a worse
     * number than one clean one. */
    {
        float lo = (float)rob_lo, hi = (float)rob_hi;
        float amp = hi - lo;
        if (amp >= 1.0f) {
            float l10 = lo + 0.10f * amp;
            float l90 = lo + 0.90f * amp;
            float rise_start = -1.0f, rise_end = -1.0f;
            float fall_start = -1.0f, fall_end = -1.0f;

            for (uint16_t i = 1; i < n; i++) {
                float prev = (float)samples[i - 1];
                float cur  = (float)samples[i];

                if (rise_start < 0.0f && prev < l10 && cur >= l10)
                    rise_start = (float)(i - 1u) + (l10 - prev) / (cur - prev);
                if (rise_start >= 0.0f && rise_end < 0.0f &&
                    prev < l90 && cur >= l90)
                    rise_end = (float)(i - 1u) + (l90 - prev) / (cur - prev);

                if (fall_start < 0.0f && prev > l90 && cur <= l90)
                    fall_start = (float)(i - 1u) + (l90 - prev) / (cur - prev);
                if (fall_start >= 0.0f && fall_end < 0.0f &&
                    prev > l10 && cur <= l10)
                    fall_end = (float)(i - 1u) + (l10 - prev) / (cur - prev);
            }

            if (rise_start >= 0.0f && rise_end >= rise_start) {
                out->rise_samples = rise_end - rise_start;
                out->rise_valid   = true;
            }
            if (fall_start >= 0.0f && fall_end >= fall_start) {
                out->fall_samples = fall_end - fall_start;
                out->fall_valid   = true;
            }
        }
    }
}

/* ── Software display trigger ──────────────────────────────────────────
 *
 * See the header for the contract. The arm/fire split is what makes this a
 * trigger rather than a bare comparator: on a noisy signal a plain "first
 * sample >= threshold" fires on the first stray sample, and the phase it
 * returns walks with the noise. Requiring the signal to first sit `hyst`
 * counts on the far side means it fires on the genuine edge, once per cycle,
 * at the same phase every frame — which is the whole point of triggering.
 */
int scope_measure_find_trigger(const uint8_t *buf, uint16_t max_start,
                               uint8_t threshold, bool rising, uint8_t hyst)
{
    if (buf == NULL)
        return -1;

    int thr = (int)threshold;
    int lo  = thr - (int)hyst; if (lo < 0)   lo = 0;
    int hi  = thr + (int)hyst; if (hi > 255) hi = 255;

    bool armed = false;
    for (uint32_t i = 0; i <= (uint32_t)max_start; i++) {
        int s = (int)buf[i];
        if (rising) {
            if (!armed) {
                if (s <= lo) armed = true;
            } else if (s >= thr) {
                return (int)i;
            }
        } else {
            if (!armed) {
                if (s >= hi) armed = true;
            } else if (s <= thr) {
                return (int)i;
            }
        }
    }
    return -1;
}

/* ═══════════════════════════════════════════════════════════════════
 * Stock's 12 measurements (C6) — see the header block for the unit rules.
 * ═══════════════════════════════════════════════════════════════════ */

static const char *const prm_labels[SCOPE_PRM_COUNT] = {
    "Freq", "Peri", "Duty+", "Duty-", "Wid+", "Wid-",
    "Vmax", "Vmin", "Vp-p", "Amp", "Vrms", "Avg"
};

const char *scope_prm_label(unsigned id)
{
    return (id < SCOPE_PRM_COUNT) ? prm_labels[id] : "?";
}

static scope_prm_t prm(scope_prm_unit_t u, float v)
{
    scope_prm_t p; p.unit = u; p.value = v; return p;
}

/* Time quantity: seconds when the record's rate is known, else samples. */
static scope_prm_t prm_time(float samples, float fs)
{
    return (fs > 0.0f) ? prm(SCOPE_PRM_UNIT_SECONDS, samples / fs)
                       : prm(SCOPE_PRM_UNIT_SAMPLES, samples);
}

/* Vertical quantity from a count RELATIVE to zero_count (signed) or, with no
 * calibration, the raw absolute ADC code (a code is exact; a volt is not). */
static scope_prm_t prm_level(float code, float k, uint8_t zero)
{
    return (k > 0.0f) ? prm(SCOPE_PRM_UNIT_VOLTS, (code - (float)zero) * k)
                      : prm(SCOPE_PRM_UNIT_COUNTS, code);
}

/* A span (difference of two levels): zero cancels. */
static scope_prm_t prm_span(float counts, float k)
{
    return (k > 0.0f) ? prm(SCOPE_PRM_UNIT_VOLTS, counts * k)
                      : prm(SCOPE_PRM_UNIT_COUNTS, counts);
}

void scope_params_compute(const scope_measure_t *m, const scope_prm_ctx_t *ctx,
                          scope_params_t *out)
{
    if (out == NULL)
        return;
    memset(out, 0, sizeof(*out));
    if (m == NULL || !m->valid || ctx == NULL)
        return;

    const float fs = ctx->sample_rate;
    const float k  = ctx->volts_per_count;
    const uint8_t z = ctx->zero_count;

    /* Freq/Peri: the spectral estimate when it answers (four periods or
     * more: SCOPE_FREQ_MIN_BIN). Below that, the crossing period, which
     * reports only when every crossing interval agrees (scope_measure_record,
     * SCOPE_MEASURE_PERIOD_TOL), of a record proven whole (ctx->record_whole),
     * turned into both with the record's rate, so Freq and Peri are the same
     * number or both "--". Without a rate, Peri in SAMPLES as before. */
    if (ctx->freq_hz > 0.0f) {
        out->v[SCOPE_PRM_FREQ] = prm(SCOPE_PRM_UNIT_HZ, ctx->freq_hz);
        out->v[SCOPE_PRM_PERI] = prm(SCOPE_PRM_UNIT_SECONDS, 1.0f / ctx->freq_hz);
    } else if (fs > 0.0f) {
        /* A rate: Hz from the checked crossing period of a whole record, or
         * nothing for both (a torn record's crossing period is the number
         * EXP-13 measured 5-50% out). */
        if (m->level_valid && m->period_valid && ctx->record_whole) {
            out->v[SCOPE_PRM_FREQ] = prm(SCOPE_PRM_UNIT_HZ, fs / m->period_samples);
            out->v[SCOPE_PRM_PERI] = prm(SCOPE_PRM_UNIT_SECONDS, m->period_samples / fs);
        }
    } else if (m->level_valid && m->period_valid) {
        out->v[SCOPE_PRM_PERI] = prm(SCOPE_PRM_UNIT_SAMPLES, m->period_samples);
    }

    /* Duty and widths need a discernible signal (level_valid) like the badge. */
    if (m->level_valid) {
        out->v[SCOPE_PRM_DUTY_POS] = prm(SCOPE_PRM_UNIT_PERCENT, m->duty_pct);
        out->v[SCOPE_PRM_DUTY_NEG] = prm(SCOPE_PRM_UNIT_PERCENT, 100.0f - m->duty_pct);
        if (m->wid_hi_valid)
            out->v[SCOPE_PRM_WID_POS] = prm_time(m->wid_hi_samples, fs);
        if (m->wid_lo_valid)
            out->v[SCOPE_PRM_WID_NEG] = prm_time(m->wid_lo_samples, fs);
        out->v[SCOPE_PRM_AMP] = prm_span((float)m->top - (float)m->base, k);
    }

    out->v[SCOPE_PRM_VMAX] = prm_level((float)m->rob_max, k, z);
    out->v[SCOPE_PRM_VMIN] = prm_level((float)m->rob_min, k, z);
    out->v[SCOPE_PRM_VPP]  = prm_span((float)m->pp_robust, k);
    out->v[SCOPE_PRM_VRMS] = prm_span(m->ac_rms, k);
    out->v[SCOPE_PRM_AVG]  = prm_level(m->mean, k, z);
}

void scope_prm_format(const scope_prm_t *p, char *out, uint32_t n)
{
    if (out == NULL || n == 0u)
        return;

    scope_cursor_reading_t r;
    r.confidence = SCOPE_CURSOR_RAW;
    r.value = 0.0f;
    r.unit = SCOPE_CURSOR_UNIT_NONE;

    if (p != NULL) {
        r.value = p->value;
        switch (p->unit) {
        case SCOPE_PRM_UNIT_HZ:      r.unit = SCOPE_CURSOR_UNIT_HERTZ;   break;
        case SCOPE_PRM_UNIT_SECONDS: r.unit = SCOPE_CURSOR_UNIT_SECONDS; break;
        case SCOPE_PRM_UNIT_SAMPLES: r.unit = SCOPE_CURSOR_UNIT_SAMPLES; break;
        case SCOPE_PRM_UNIT_VOLTS:   r.unit = SCOPE_CURSOR_UNIT_VOLTS;   break;
        case SCOPE_PRM_UNIT_COUNTS:  r.unit = SCOPE_CURSOR_UNIT_COUNTS;  break;
        case SCOPE_PRM_UNIT_PERCENT: {
            float v = (p->value < 0.0f) ? 0.0f : p->value;
            unsigned t = (unsigned)(v * 10.0f + 0.5f);
            snprintf(out, n, "%u.%u%%", t / 10u, t % 10u);
            return;
        }
        default: break;
        }
    }
    scope_cursor_format(&r, out, n);
}

unsigned scope_prm_select(uint16_t mask, uint8_t *ids, unsigned max)
{
    unsigned c = 0;
    if (ids == NULL)
        return 0;
    for (unsigned i = 0; i < SCOPE_PRM_COUNT && c < max; i++)
        if (mask & (1u << i))
            ids[c++] = (uint8_t)i;
    return c;
}
