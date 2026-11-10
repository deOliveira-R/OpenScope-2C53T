#include "can_bus.h"
#include <stdio.h>
#include <string.h>

/* Minimum dominant/recessive separation (diff counts) to call it a bus signal. */
#define CAN_BUS_MIN_SWING 8

float can_bus_measure_spb(const int16_t *s, uint32_t n, int16_t thr)
{
    /* Histogram of run lengths (partial first/last runs and 1-sample glitches
     * ignored; runs >= CAN_BUS_HIST are idle and not needed). 256 bytes of
     * stack. Resolves up to ~20 samples per bit. */
    enum { CAN_BUS_HIST = 128 };
    uint16_t hist[CAN_BUS_HIST];
    memset(hist, 0, sizeof(hist));
    uint32_t run = 1, nr = 0;
    int cur = s[0] >= thr, first = 1;
    for (uint32_t i = 1; i < n; i++) {
        int hi = s[i] >= thr;
        if (hi == cur) { run++; continue; }
        if (!first && run >= 2 && run < CAN_BUS_HIST) { hist[run]++; nr++; }
        first = 0;
        cur = hi;
        run = 1;
    }
    if (nr < 3) return 0.0f;

    /* One bit: the shortest length that repeats, with +-1 sample of jitter. */
    uint32_t lmin = 0;
    for (uint32_t L = 2; L + 1 < CAN_BUS_HIST; L++)
        if (hist[L] && hist[L] + hist[L + 1] >= 2) { lmin = L; break; }
    if (!lmin) return 0.0f;

    /* First estimate: mean of the one-bit cluster [lmin, 1.5 lmin]. */
    uint32_t sum = 0, bits = 0;
    for (uint32_t L = lmin; L < CAN_BUS_HIST && L * 2 <= lmin * 3; L++) {
        sum += L * hist[L];
        bits += hist[L];
    }
    if (!bits) return 0.0f;
    float spb = (float)sum / (float)bits;

    /* Fit: the bit width that puts every in-frame run (1..6 bits) closest to a
     * whole number of bits, searched within +-25 % of the first estimate. The
     * error is relative (residual / width) so a narrower width cannot win just
     * by dividing runs into more, smaller bits. At 2.5 samples per bit the run
     * lengths are 2/3, 5, 7/8, 10, 12/13: averaging alone lands on 2.3 or 2.7
     * when one rounding dominates (bench, 2026-11-10); the fit finds 2.50. */
    /* The runs to fit are fixed BEFORE the search: everything shorter than 7.5
     * bits at the first estimate (stuffing caps in-frame runs at 5 bits, plus
     * margin; idle and EOF are longer). If each candidate chose its own runs, a
     * too-small width could drop the runs it fits worst: at 2.5 samples per bit
     * the 13-sample runs of five zero bits read as 7.6 bits at 1.71 and were
     * excluded, and 1.71 won (bench record, 2026-11-10). */
    const uint32_t lmax = (uint32_t)(7.5f * spb);
    float best_s = spb, best_e = -1.0f;
    const float step = spb * 0.002f;          /* 250 candidates at any width */
    for (float cand = spb * 0.75f; cand <= spb * 1.25f; cand += step) {
        float e = 0.0f;
        uint32_t used = 0;
        for (uint32_t L = 2; L < CAN_BUS_HIST && L <= lmax; L++) {
            if (!hist[L]) continue;
            float q = (float)L / cand;
            uint32_t k = (uint32_t)(q + 0.5f);
            if (k < 1) k = 1;
            float r = q - (float)k;               /* residual in bits */
            e += (float)hist[L] * r * r;
            used += hist[L];
        }
        if (used < 3) continue;
        e /= (float)used;
        if (best_e < 0.0f || e < best_e - 1e-6f) { best_e = e; best_s = cand; }
    }
    if (best_e < 0.0f) return 0.0f;

    /* Final refine at the fitted width: total samples / total bits. */
    sum = 0;
    bits = 0;
    for (uint32_t L = 2; L < CAN_BUS_HIST; L++) {
        if (!hist[L]) continue;
        uint32_t kb = (uint32_t)((float)L / best_s + 0.5f);
        if (kb >= 1 && kb <= 6) { sum += L * hist[L]; bits += kb * hist[L]; }
    }
    return bits ? (float)sum / (float)bits : best_s;
}

static void reverse(int16_t *s, uint32_t a, uint32_t b)   /* [a, b) */
{
    while (a + 1 < b) {
        int16_t t = s[a];
        s[a++] = s[--b];
        s[b] = t;
    }
}

void can_bus_rotate(int16_t *s, uint32_t n, uint32_t k)
{
    if (!n) return;
    k %= n;
    if (!k) return;
    reverse(s, 0, k);
    reverse(s, k, n);
    reverse(s, 0, n);
}

/* Start index of the longest run of `level` (1 = >= thr), counted around the
 * ring; n when the whole record is one level. */
static uint32_t longest_cyclic_run(const int16_t *s, uint32_t n, int16_t thr, int level,
                                   uint32_t *len_out)
{
    /* Begin scanning at a transition so no run is split by index 0. */
    uint32_t t0 = n;
    for (uint32_t i = 0; i < n; i++)
        if ((s[i] >= thr) != (s[(i + n - 1) % n] >= thr)) { t0 = i; break; }
    if (t0 == n) { *len_out = n; return n; }
    uint32_t best = 0, best_start = 0, run = 0, start = t0;
    for (uint32_t j = 0; j < n; j++) {
        uint32_t i = (t0 + j) % n;
        if ((s[i] >= thr) == level) {
            if (run == 0) start = i;
            run++;
            if (run > best) { best = run; best_start = start; }
        } else {
            run = 0;
        }
    }
    *len_out = best;
    return best_start;
}

uint32_t can_bus_analyse(const volatile uint8_t *ch1, const volatile uint8_t *ch2,
                         uint32_t n, float fs, uint8_t rotated, int16_t *diff,
                         can_bus_report_t *rep, decode_result_t *res)
{
    memset(rep, 0, sizeof(*rep));
    rep->rotation = -1;
    memset(res, 0, sizeof(*res));
    res->type = DECODE_CAN;
    if (!ch1 || !diff || n < 16) return 0;

    int16_t mn = 32767, mx = -32768;
    for (uint32_t i = 0; i < n; i++) {
        int16_t v = (int16_t)((int16_t)ch1[i] - (ch2 ? (int16_t)ch2[i] : 0));
        diff[i] = v;
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    if (mx - mn < CAN_BUS_MIN_SWING) return 0;
    int16_t thr = (int16_t)((mn + mx) / 2);
    rep->threshold = thr;

    if (rotated) {
        /* The 2C53T commits a ring buffer with the trigger 512 samples after
         * its seam (OpenScope EXP-53/54). On a CAN bus the trigger is the start
         * of frame that ends the longest idle stretch, so rotating that edge to
         * n/2 restores time order exactly: the pre-trigger idle before it, the
         * frame, then the post-frame idle after it. (Starting the record at the
         * longest idle instead moves the post-frame idle to the front and cuts
         * the frame's end: bench, 2026-11-10.) */
        uint32_t lh = 0, ll = 0;
        uint32_t sh = longest_cyclic_run(diff, n, thr, 1, &lh);
        uint32_t sl = longest_cyclic_run(diff, n, thr, 0, &ll);
        uint32_t start = (lh >= ll) ? sh : sl, len = (lh >= ll) ? lh : ll;
        if (start < n && len < n) {
            uint32_t sof = (start + len) % n;
            uint32_t k = (sof + n - n / 2) % n;
            can_bus_rotate(diff, n, k);
            rep->rotation = (int32_t)k;
        }
    }

    float spb = can_bus_measure_spb(diff, n, thr);
    if (spb < 2.0f) return 0;
    /* With a known sample rate, snap to a standard bus rate within 6 %: buses
     * run at these, and an exact width keeps a 2.5-sample-per-bit decode
     * aligned to the end of a frame. */
    if (fs > 0.0f) {
        static const float std_rates[] = { 10e3f, 20e3f, 33.333e3f, 50e3f, 62.5e3f, 83.333e3f,
                                           100e3f, 125e3f, 250e3f, 500e3f, 800e3f, 1e6f };
        float br = fs / spb;
        for (unsigned i = 0; i < sizeof(std_rates) / sizeof(std_rates[0]); i++) {
            float d = (br - std_rates[i]) / std_rates[i];
            if (d < 0.06f && d > -0.06f) { spb = fs / std_rates[i]; break; }
        }
    }
    rep->spb = spb;
    rep->signal = 1;
    if (fs > 0.0f) rep->bit_rate = fs / spb;

    /* Levels: mean of each side (for the report only). */
    int32_t sh = 0, sl = 0, nh = 0, nl = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (diff[i] >= thr) { sh += diff[i]; nh++; } else { sl += diff[i]; nl++; }
    }
    int16_t hi = nh ? (int16_t)(sh / nh) : mx, lo = nl ? (int16_t)(sl / nl) : mn;

    can_config_t cfg = { .bit_rate = 500000, .threshold = thr, .polarity = CAN_POL_AUTO };
    decode_can(diff, n, spb * 500000.0f, &cfg, res);

    /* The decoder picked the idle level; report it. Recessive = longest run. */
    {
        uint32_t best = 0, run = 0;
        int cur = -1, best_hi = 1;
        for (uint32_t i = 0; i < n; i++) {
            int h = diff[i] >= thr;
            if (h == cur) run++; else { cur = h; run = 1; }
            if (run > best) { best = run; best_hi = h; }
        }
        rep->level_rec = best_hi ? hi : lo;
        rep->level_dom = best_hi ? lo : hi;
    }
    return res->num_frames;
}

void can_bus_stats_reset(can_bus_stats_t *st)
{
    memset(st, 0, sizeof(*st));
}

void can_bus_stats_add(can_bus_stats_t *st, const can_bus_report_t *rep,
                       const decode_result_t *res)
{
    st->records++;
    if (!rep->signal) { st->idle++; return; }
    for (uint16_t f = 0; f < res->num_frames; f++) {
        const decode_frame_t *fr = &res->frames[f];
        if (fr->flags & DECODE_FLAG_TRUNC) { st->trunc++; continue; }
        st->frames++;
        if (fr->flags & DECODE_FLAG_ACK)   st->ack++;
        if (fr->flags & DECODE_FLAG_NAK)   st->nak++;
        if (fr->flags & DECODE_FLAG_CRC)   st->crc++;
        if (fr->flags & DECODE_FLAG_ERROR) st->err++;
        /* Only trust the ID of a frame whose CRC checked. */
        if (fr->flags & (DECODE_FLAG_CRC | DECODE_FLAG_ERROR)) continue;
        uint8_t ext = (fr->flags & DECODE_FLAG_EXT) ? 1 : 0;
        can_bus_id_t *e = 0;
        for (uint8_t k = 0; k < st->nids; k++)
            if (st->ids[k].id == fr->id && st->ids[k].ext == ext) { e = &st->ids[k]; break; }
        if (!e) {
            if (st->nids >= CAN_BUS_MAX_IDS) { st->ids_dropped++; continue; }
            e = &st->ids[st->nids++];
            memset(e, 0, sizeof(*e));
            e->id = fr->id;
            e->ext = ext;
        }
        e->seen++;
        if (fr->flags & DECODE_FLAG_ACK) e->ack++;
        if (fr->flags & DECODE_FLAG_NAK) e->nak++;
        e->dlc = fr->data_len;
        memcpy(e->data, fr->data, fr->data_len > 8 ? 8 : fr->data_len);
    }
}

int can_bus_summary(const can_bus_stats_t *st, const can_bus_report_t *last,
                    char *out, uint32_t len)
{
    int healthy = (st->nak == 0 && st->crc == 0 && st->err == 0);
    int p = 0;
    if (!last || !last->signal) {
        p = snprintf(out, len, "CAN: no bus signal (%lu/%lu idle)",
                     (unsigned long)st->idle, (unsigned long)st->records);
        return 0;
    }
    if (last->bit_rate > 0.0f)
        p = snprintf(out, len, "CAN %luk", (unsigned long)(last->bit_rate / 1000.0f + 0.5f));
    else
    {
        unsigned t = (unsigned)(last->spb * 10.0f + 0.5f);   /* no %f on the target */
        p = snprintf(out, len, "CAN %u.%uspb", t / 10u, t % 10u);
    }
    for (uint8_t k = 0; k < st->nids && p > 0 && (uint32_t)p < len; k++) {
        const can_bus_id_t *e = &st->ids[k];
        p += snprintf(out + p, len - (uint32_t)p, e->ext ? " %lX%s" : " %03lX%s",
                      (unsigned long)e->id, e->nak ? "!" : "");
    }
    if (p > 0 && (uint32_t)p < len)
        p += snprintf(out + p, len - (uint32_t)p, healthy ? " ok %lu" : " %lufr",
                      (unsigned long)st->frames);
    if (!healthy && p > 0 && (uint32_t)p < len)
        snprintf(out + p, len - (uint32_t)p, " NAK%lu CRC%lu ERR%lu",
                 (unsigned long)st->nak, (unsigned long)st->crc, (unsigned long)st->err);
    return healthy;
}
