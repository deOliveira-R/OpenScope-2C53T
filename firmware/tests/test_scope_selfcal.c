/*
 * Host tests for autoset, baseline correction and 50 % centre (parity C9/C10,
 * src/ui/scope_selfcal.c). The engines run against simulated channels that
 * produce whole 1024-byte records the way the device does: clamp(raw - 28),
 * 0 V at record 100 + pos with a page, invalid head, noise, hum, clipping.
 *
 * Baseline: each (channel, bank, range) has a TRUE zero code z and slope s
 * (codes per count); the page pair handed to the servo is the truth plus an
 * error. A converged run must hand back page + delta = truth, within a code
 * or two, and refuse everything a quiet-input calibration must not accept.
 *
 * Autoset: the input is volts (DC + sine/square); the frontend is the
 * nominal ladder (scope_cal_nominal_mv) and the measured sample rates
 * (scope_timebase.c), so the decisions are checked against the real tables.
 */
#include "../src/ui/scope_selfcal.h"
#include "../src/ui/scope_cal.h"
#include "../src/ui/scope_timebase.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
            failures++;                                                       \
        }                                                                     \
    } while (0)

#define REC_LEN   1024u
#define HEAD      128u

/* Deterministic noise. */
static uint32_t rng = 12345u;
static float urand(void)   /* -1..1 */
{
    rng = rng * 1664525u + 1013904223u;
    return ((float)(rng >> 8) / 8388608.0f) - 1.0f;
}

static uint8_t clamp_rec(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 227.0f) v = 227.0f;
    return (uint8_t)lroundf(v);
}

/* ═══════════════════════════════════════════════════════════════════
 * Record statistics and the small helpers
 * ═══════════════════════════════════════════════════════════════════ */
static void test_stats(void)
{
    uint8_t r[REC_LEN];
    sc_rec_stats_t s;
    for (unsigned i = 0; i < REC_LEN; i++) r[i] = (i < HEAD) ? 0u : 100u;
    CHECK(sc_rec_stats(r, REC_LEN, HEAD, &s), "stats on a full record");
    CHECK(s.min == 100 && s.max == 100 && fabsf(s.mean - 100.0f) < 1e-4f && !s.clipped,
          "flat 100: min %u max %u mean %.2f clipped %d (head zeros must not count)",
          s.min, s.max, s.mean, s.clipped);
    CHECK(s.n_rise == 0 && s.period == 0.0f, "flat: no crossings");

    /* A square of period 64 samples: 14 periods in the 896-sample body. */
    for (unsigned i = 0; i < REC_LEN; i++) r[i] = ((i / 32u) & 1u) ? 150u : 50u;
    sc_rec_stats(r, REC_LEN, HEAD, &s);
    CHECK(s.n_rise >= 13 && s.n_rise <= 14, "square: %u rising crossings", s.n_rise);
    CHECK(fabsf(s.period - 64.0f) < 0.5f, "square: period %.2f, want 64", s.period);

    r[500] = 0;
    sc_rec_stats(r, REC_LEN, HEAD, &s);
    CHECK(s.clipped, "a body sample at record 0 is the clamp rail");
    r[500] = 227;
    sc_rec_stats(r, REC_LEN, HEAD, &s);
    CHECK(s.clipped, "a body sample at 227 is the upper rail");
    CHECK(!sc_rec_stats(r, 600, HEAD, &s), "a body shorter than 500 is refused");

    CHECK(sc_level_for_raw(128.0f) == 0, "raw 128 = level 0");
    CHECK(sc_level_for_raw(178.0f) == 40, "raw 178 = level 40 (50 * 206 / 256)");
    CHECK(sc_level_for_raw(0.0f) == -100 && sc_level_for_raw(255.0f) == 100, "level clamps");

    int16_t pos[2];
    int lvl = sc_center50(true, true, 0, pos);
    CHECK(pos[0] == 50 && pos[1] == -50 && lvl == 40,
          "both on: +50/-50, trigger on CH1's 0 V (got %d/%d level %d)", pos[0], pos[1], lvl);
    lvl = sc_center50(true, true, 1, pos);
    CHECK(lvl == -40, "both on, CH2 source: level on CH2's 0 V (%d)", lvl);
    lvl = sc_center50(true, false, 0, pos);
    CHECK(pos[0] == 0 && pos[1] == 0 && lvl == 0, "one channel: both 0");

    uint16_t cb, cu;
    CHECK(sc_corr_pair(1000, 2580, -10, 12, &cb, &cu) && cb == 990 && cu == 2592, "corr pair");
    CHECK(!sc_corr_pair(10, 2580, -11, 0, &cb, &cu), "corr pair below 0 refused");
    CHECK(!sc_corr_pair(1000, 4090, 0, 6, &cb, &cu), "corr pair above 4095 refused");
}

/* Glitch-trimmed spread, mean and rails (the ilcal_window recipe). */
static void test_trimmed_stats(void)
{
    uint8_t r[REC_LEN];
    sc_rec_stats_t s;
    for (unsigned i = 0; i < REC_LEN; i++) r[i] = (i < HEAD) ? 0u : (uint8_t)(100u + (i % 5u));  /* 100..104 */
    for (unsigned i = 500; i < 540; i++) r[i] = (i & 1u) ? 0u : 220u;   /* a 40-sample burst, on both rails */
    CHECK(sc_rec_stats(r, REC_LEN, HEAD, &s), "stats");
    CHECK(s.spread == 4 && !s.tclip, "burst trimmed: spread %u, tclip %u", s.spread, s.tclip);
    CHECK(s.max - s.min > 200 && s.clipped, "the raw view still sees the burst");
    CHECK(s.n_out == 40, "40 dropped, got %u", s.n_out);
    CHECK(fabsf(s.tmean - 102.0f) < 0.1f, "trimmed mean %.2f", s.tmean);

    /* More than 15 % wild: not a glitch, spread 255. */
    for (unsigned i = 300; i < 300 + 140; i++) r[i] = 200u;   /* 180/896 = 20 % outside */
    sc_rec_stats(r, REC_LEN, HEAD, &s);
    CHECK(s.spread == 255, "20 %% outliers: spread %u", s.spread);

    /* A record that really sits on the rail stays clipped. */
    for (unsigned i = 0; i < REC_LEN; i++) r[i] = (i < HEAD) ? 0u : (uint8_t)(i % 3u);
    sc_rec_stats(r, REC_LEN, HEAD, &s);
    CHECK(s.tclip, "pegged at the low rail: tclip");
}

/* ═══════════════════════════════════════════════════════════════════
 * Baseline correction on a simulated channel
 * ═══════════════════════════════════════════════════════════════════ */
typedef struct {
    float z[2][3][10];        /* true code for record 100 */
    float s[2][3][10];        /* true codes per count */
    int   err_b[2][3][10], err_u[2][3][10];   /* page = truth + err */
    bool  no_pair[2][3][10];
    /* input */
    float noise;              /* +- counts, per sample */
    float hum;                /* +- counts, per record */
    float sine_amp_mv;        /* a signal left on the input */
    float dc_mv;              /* a DC level left on the input */
    float noise_cnt[2][10];   /* per channel and range: uniform +- counts (shorted-input noise) */
    float open_mv[2];         /* per channel: floating-input pickup, uniform +- mV (counts = mV / ladder) */
    unsigned burst_len;       /* interference burst per record: this many wild samples ... */
    float burst_amp;          /*  ... uniform +- this many counts */
    /* results */
    int   commits;
    bool  got[2][3][10];
    int   cb[2][3][10], cu[2][3][10];
    int   records;
} bc_sim_t;

static bool sim_pair(void *ctx, uint8_t ch, uint8_t bank, uint8_t range,
                     uint16_t *base, uint16_t *upper)
{
    bc_sim_t *s = ctx;
    if (s->no_pair[ch][bank][range]) return false;
    const float z = s->z[ch][bank][range], k = s->s[ch][bank][range];
    *base = (uint16_t)lroundf(z - 100.0f * k) + (uint16_t)s->err_b[ch][bank][range];
    *upper = (uint16_t)lroundf(z + 100.0f * k) + (uint16_t)s->err_u[ch][bank][range];
    return true;
}

static void sim_commit(void *ctx, uint8_t ch, uint8_t bank, uint8_t range,
                       int16_t d_base, int16_t d_upper)
{
    bc_sim_t *s = ctx;
    uint16_t b, u;
    sim_pair(ctx, ch, bank, range, &b, &u);
    s->commits++;
    s->got[ch][bank][range] = true;
    s->cb[ch][bank][range] = (int)b + d_base;
    s->cu[ch][bank][range] = (int)u + d_upper;
}

static void bc_sim_default(bc_sim_t *s)
{
    memset(s, 0, sizeof(*s));
    for (unsigned c = 0; c < 2; c++)
        for (unsigned b = 0; b < 3; b++)
            for (unsigned r = 0; r < 10; r++) {
                s->z[c][b][r] = 2300.0f + 37.0f * (float)r - 11.0f * (float)b + 5.0f * (float)c;
                s->s[c][b][r] = 7.6f + 0.05f * (float)r + 0.1f * (float)c;
                /* page errors of up to ~5 counts, different at each end */
                s->err_b[c][b][r] = (int)(((r * 7u + b * 13u + c * 5u) % 81u)) - 40;
                s->err_u[c][b][r] = (int)(((r * 11u + b * 3u + c * 17u) % 61u)) - 30;
            }
    s->noise = 2.0f;
}

static void bc_make_obs(bc_sim_t *s, const sc_basecal_t *b, sc_rec_stats_t obs[2])
{
    uint8_t rec[REC_LEN];
    const uint8_t bank = sc_bc_phases[b->phase].bank;
    const float mv = scope_cal_nominal_mv(b->range);
    s->records++;
    for (unsigned c = 0; c < 2; c++) {
        const float z = s->z[c][bank][b->range], k = s->s[c][bank][b->range];
        const float level = 100.0f + ((float)b->dac[c] - z) / k + s->hum * urand()
                            + s->dc_mv / mv;
        for (unsigned i = 0; i < REC_LEN; i++) {
            float v = level + s->noise * urand() + s->noise_cnt[c][b->range] * urand()
                      + (s->open_mv[c] / mv) * urand();
            if (s->sine_amp_mv > 0.0f)
                v += s->sine_amp_mv / mv * sinf(6.2831853f * (float)i / 97.0f);
            rec[i] = (i < 40u) ? 0u : clamp_rec(v);   /* invalid head, as on the device */
        }
        if (s->burst_len) {   /* one burst somewhere in the body */
            const unsigned at = 200u + (unsigned)((urand() + 1.0f) * 0.5f * (float)(REC_LEN - 300u - s->burst_len));
            for (unsigned i = at; i < at + s->burst_len; i++)
                rec[i] = clamp_rec((float)rec[i] + s->burst_amp * urand());
        }
        sc_rec_stats(rec, REC_LEN, HEAD, &obs[c]);
    }
}

static sc_bc_status_t bc_run(bc_sim_t *s, uint8_t banks, sc_basecal_t *b)
{
    sc_bc_cfg_t cfg = { banks, sim_pair, sim_commit, s };
    sc_basecal_begin(b, &cfg);
    sc_bc_status_t st = SC_BC_RUNNING;
    for (int i = 0; i < 20000 && st == SC_BC_RUNNING; i++) {
        sc_rec_stats_t obs[2];
        bool fresh;
        bc_make_obs(s, b, obs);
        st = sc_basecal_step(b, obs, &fresh);
    }
    return st;
}

static void test_basecal_converges(void)
{
    static bc_sim_t s;
    sc_basecal_t b;
    bc_sim_default(&s);
    sc_bc_status_t st = bc_run(&s, 0x03u, &b);
    CHECK(st == SC_BC_DONE, "banks 0+1: status %s", sc_bc_status_str(st));
    CHECK(s.commits == 40 && b.n_ok == 40 && b.n_fail == 0,
          "banks 0+1: 2 ch x 2 banks x 10 ranges corrected, got %d commits, %u fail (last %s)",
          s.commits, b.n_fail, sc_bc_fail_str(b.last_fail));
    int worst = 0;
    for (unsigned c = 0; c < 2; c++)
        for (unsigned k = 0; k < 2; k++)
            for (unsigned r = 0; r < 10; r++) {
                if (!s.got[c][k][r]) continue;
                const int tb = (int)lroundf(s.z[c][k][r] - 100.0f * s.s[c][k][r]);
                const int tu = (int)lroundf(s.z[c][k][r] + 100.0f * s.s[c][k][r]);
                const int e1 = abs(s.cb[c][k][r] - tb), e2 = abs(s.cu[c][k][r] - tu);
                if (e1 > worst) worst = e1;
                if (e2 > worst) worst = e2;
            }
    /* +-0.5 count of tolerance at two points 100 positions apart, times a
     * 1.5 lever to the endpoints: ~1.5 counts x ~8 codes. */
    CHECK(worst <= 14, "corrected endpoints within 14 codes (~1.7 counts) of truth, worst %d", worst);
    printf("  basecal banks 0+1: %d records, worst endpoint error %d codes\n", s.records, worst);

    bc_sim_default(&s);
    st = bc_run(&s, 0x07u, &b);
    CHECK(st == SC_BC_DONE && s.commits == 60,
          "all banks: bank 2 per channel adds 20 (got %d, %s)", s.commits, sc_bc_status_str(st));

    /* Hum within the agreement window and coarse noise still converge. */
    bc_sim_default(&s);
    s.hum = 0.6f;
    st = bc_run(&s, 0x01u, &b);
    CHECK(st == SC_BC_DONE && s.commits == 20, "hum 0.6: %d commits (%s)", s.commits,
          sc_bc_status_str(st));
}

/* Shorted-input noise in counts per range (grows with the gain: ~2 mV p-p
 * at 0.4 mV/count is 5 counts, but the front end adds more at the top
 * sensitivities), worst pp = 2x these, always inside the range's limit. */
static const float noisy_cnt[10] = { 10, 9, 8, 7, 6, 4, 3, 2, 2, 2 };

static void test_basecal_noisy_ranges(void)
{
    static bc_sim_t s;
    sc_basecal_t b;

    /* Noise at every range plus a 5 % interference burst in every record:
     * the old fixed limits (12 / 36 below r4) and the raw p-p/rails refused
     * these ("27 ok / 13 not flat", unit #2). All 40 entries now correct. */
    bc_sim_default(&s);
    for (unsigned c = 0; c < 2; c++)
        for (unsigned r = 0; r < 10; r++) s.noise_cnt[c][r] = noisy_cnt[r];
    s.noise = 0.5f;
    s.burst_len = 40;
    s.burst_amp = 150.0f;
    sc_bc_status_t st = bc_run(&s, 0x03u, &b);
    CHECK(st == SC_BC_DONE && s.commits == 40 && b.n_fail == 0,
          "noisy ranges + bursts: %s, %d commits, %u refused (last %s r%u)", sc_bc_status_str(st),
          s.commits, b.n_fail, sc_bc_fail_str(b.last_fail), b.last_fail_range);
    uint8_t es, sp;
    CHECK(sc_basecal_entry(&b, 0, 1, 4, &es, &sp, NULL) && es == SC_BC_LOG_OK && sp >= 6 && sp <= sc_bc_flat_limit(4),
          "log: CH1 b1 r4 ok, spread %u within its limit %u", sp, sc_bc_flat_limit(4));

    /* Limits widen with the gain and never fall below the quiet limit. */
    for (unsigned r = 1; r < 10; r++)
        CHECK(sc_bc_flat_limit((uint8_t)r) <= sc_bc_flat_limit((uint8_t)(r - 1)) &&
              sc_bc_flat_limit((uint8_t)r) >= SC_BC_FLAT_PP, "limit monotonic at r%u", r);
    CHECK(sc_bc_flat_limit(9) == SC_BC_FLAT_PP, "coarse end keeps the quiet limit");

    /* Noise beyond a range's limit is still refused, with the number behind it. */
    bc_sim_default(&s);
    s.noise_cnt[0][5] = 20.0f;     /* +-20 -> p-p ~40 on r5, limit 20 */
    st = bc_run(&s, 0x01u, &b);
    CHECK(st == SC_BC_DONE && !s.got[0][0][5] && s.commits == 19, "r5 too noisy refused: %d commits", s.commits);
    CHECK(sc_basecal_entry(&b, 0, 0, 5, &es, &sp, NULL) && es == SC_BC_LOG_FAIL0 + SC_BCF_NOT_FLAT &&
          sp > sc_bc_flat_limit(5), "log: reason not flat, spread %u > %u", sp, sc_bc_flat_limit(5));
    CHECK(!b.open_ch[0] && !b.open_ch[1], "one noisy entry does not make the input open");
}

static void test_basecal_open_input(void)
{
    static bc_sim_t s;
    sc_basecal_t b;
    uint8_t es;

    /* CH1 shorted, CH2 floating (hum pickup of +-60 mV, invisible at the
     * coarse ranges, hundreds of counts at the sensitive ones): CH1 is
     * corrected, CH2 is refused as open and never corrected at r4..r9. */
    bc_sim_default(&s);
    s.open_mv[1] = 60.0f;
    for (unsigned r = 0; r < 10; r++) s.noise_cnt[0][r] = noisy_cnt[r];
    sc_bc_status_t st = bc_run(&s, 0x03u, &b);
    CHECK(st == SC_BC_DONE, "CH2 open, CH1 shorted: %s", sc_bc_status_str(st));
    unsigned ch1 = 0, ch2 = 0;
    for (unsigned k = 0; k < 3; k++)
        for (unsigned r = 0; r < 10; r++) { ch1 += s.got[0][k][r]; ch2 += s.got[1][k][r]; }
    CHECK(ch1 == 20 && ch2 == 0, "CH1 20 corrected, CH2 none: %u / %u", ch1, ch2);
    CHECK(b.open_ch[1] && !b.open_ch[0], "CH2 declared open/noisy, CH1 not");
    CHECK(sc_basecal_entry(&b, 1, 0, 0, &es, NULL, NULL) && es == SC_BC_LOG_FAIL0 + SC_BCF_NOT_FLAT,
          "log: CH2 b0 r0 not flat");
    CHECK(sc_basecal_entry(&b, 1, 0, 9, &es, NULL, NULL) && es == SC_BC_LOG_FAIL0 + SC_BCF_OPEN,
          "log: CH2 b0 r9 skipped because open (state %u)", es);
    CHECK(sc_basecal_entry(&b, 1, 1, 6, &es, NULL, NULL) && es == SC_BC_LOG_FAIL0 + SC_BCF_OPEN,
          "log: CH2 b1 r6 skipped because open too (the next bank)");
    CHECK(b.n_ok == 20 && b.n_fail == 20, "ok %u fail %u", b.n_ok, b.n_fail);
    CHECK(strstr(sc_bc_fail_str(SC_BCF_OPEN), "open") != NULL, "message names the open input");

    /* Both floating: nothing corrected, the status says why. */
    bc_sim_default(&s);
    s.open_mv[0] = s.open_mv[1] = 60.0f;
    st = bc_run(&s, 0x03u, &b);
    CHECK(st == SC_BC_OPEN && s.commits == 0, "both open: %s, %d commits", sc_bc_status_str(st), s.commits);
    CHECK(strstr(sc_bc_status_str(SC_BC_OPEN), "open/noisy") != NULL, "status text");

    /* Open channel on bank 2 (one channel per phase) only: the other one is unaffected. */
    bc_sim_default(&s);
    s.open_mv[1] = 60.0f;
    st = bc_run(&s, 0x04u, &b);
    CHECK(st == SC_BC_DONE && s.commits == 10, "bank 2: CH1 corrected, CH2 refused: %d", s.commits);
}

static void test_basecal_refusals(void)
{
    static bc_sim_t s;
    sc_basecal_t b;

    /* A probe left on a 1 V signal: refused before any entry. */
    bc_sim_default(&s);
    s.sine_amp_mv = 1000.0f;
    sc_bc_status_t st = bc_run(&s, 0x03u, &b);
    CHECK(st == SC_BC_NOT_QUIET && s.commits == 0, "signal on the input: %s, %d commits",
          sc_bc_status_str(st), s.commits);

    /* A DC level of 1 V: 2.5 counts at r9, 25 at r6. Flat, unclipped, and
     * would otherwise be "corrected" into the table. */
    bc_sim_default(&s);
    s.dc_mv = 1000.0f;
    st = bc_run(&s, 0x03u, &b);
    CHECK(st == SC_BC_DC_INPUT && s.commits == 0, "DC on the input: %s, %d commits",
          sc_bc_status_str(st), s.commits);

    /* Hum far beyond the agreement window: no entry may be accepted. */
    bc_sim_default(&s);
    s.hum = 6.0f;
    st = bc_run(&s, 0x01u, &b);
    CHECK(st != SC_BC_DONE && s.commits == 0, "hum 6: %s, %d commits", sc_bc_status_str(st),
          s.commits);

    /* One page pair wildly off (the CH1 0xDC5B kind, but in range): the
     * record sits on a rail at the start point. Never stepped on a clipped
     * mean: that entry fails, the rest are corrected. */
    bc_sim_default(&s);
    s.err_b[0][0][3] = 1400;
    s.err_u[0][0][3] = 1400;
    st = bc_run(&s, 0x01u, &b);
    CHECK(st == SC_BC_DONE && s.commits == 19 && !s.got[0][0][3],
          "clipped entry refused, others kept: %d commits", s.commits);
    CHECK(b.last_fail == SC_BCF_CLIPPED, "reason %s", sc_bc_fail_str(b.last_fail));

    /* A truth that needs a code above 4095: refused, no wrap. */
    bc_sim_default(&s);
    s.z[1][0][5] = 3700.0f;
    s.s[1][0][5] = 8.0f;
    s.err_b[1][0][5] = (int)(2900 - lroundf(3700.0f - 800.0f));
    s.err_u[1][0][5] = (int)(4090 - lroundf(3700.0f + 800.0f));
    st = bc_run(&s, 0x01u, &b);
    CHECK(st == SC_BC_DONE && !s.got[1][0][5] && s.commits == 19,
          "out-of-range truth refused: got %d, commits %d", s.got[1][0][5], s.commits);
    CHECK(b.last_fail == SC_BCF_DAC_RANGE, "reason %s", sc_bc_fail_str(b.last_fail));

    /* An inverted channel (code up = record down): diverges, never committed. */
    bc_sim_default(&s);
    s.s[0][0][7] = -7.8f;
    s.err_b[0][0][7] = -1560 + 20;   /* page pair still base < upper */
    s.err_u[0][0][7] = 1560 + 20;
    st = bc_run(&s, 0x01u, &b);
    CHECK(!s.got[0][0][7], "inverted channel never committed");

    /* No page pair for a range: reported, not invented. */
    bc_sim_default(&s);
    s.no_pair[1][0][2] = true;
    st = bc_run(&s, 0x01u, &b);
    CHECK(st == SC_BC_DONE && s.commits == 19 && !s.got[1][0][2], "missing pair skipped");

    /* No page at all: nothing to do. */
    bc_sim_default(&s);
    for (unsigned c = 0; c < 2; c++)
        for (unsigned k = 0; k < 3; k++)
            for (unsigned r = 0; r < 10; r++) s.no_pair[c][k][r] = true;
    st = bc_run(&s, 0x07u, &b);
    CHECK(st == SC_BC_FAILED && s.commits == 0, "no page: %s", sc_bc_status_str(st));
}

/* ═══════════════════════════════════════════════════════════════════
 * Autoset on a simulated scope
 * ═══════════════════════════════════════════════════════════════════ */
typedef struct {
    float dc_v, amp_v, freq;     /* per channel */
    bool  square;
} as_sig_t;

typedef struct {
    as_sig_t sig[2];
    bool     page;               /* 0 V = record 100 + pos */
    float    nopage_offset[2];   /* record level of 0 V without a page */
    float    noise;
    int      records;
} as_sim_t;

static float rates[SC_TB_CODES];
static float mvs[SC_RANGES];

static void as_make_obs(as_sim_t *s, const sc_autoset_t *a, sc_rec_stats_t obs[2])
{
    uint8_t rec[REC_LEN];
    const float fs = rates[a->tb];
    s->records++;
    for (unsigned c = 0; c < 2; c++) {
        const as_sig_t *g = &s->sig[c];
        const float cpv = 1000.0f / mvs[a->range[c]];   /* counts per volt */
        const float zero = s->page ? 100.0f + (float)a->pos[c] : s->nopage_offset[c];
        const float ph = 0.37f + (float)s->records * 0.61f;
        for (unsigned i = 0; i < REC_LEN; i++) {
            const float t = (float)i / fs;
            float w = sinf(6.2831853f * g->freq * t + ph);
            if (g->square) w = (w >= 0.0f) ? 1.0f : -1.0f;
            const float v = g->dc_v + g->amp_v * w;
            rec[i] = (i < 40u) ? 0u : clamp_rec(zero + v * cpv + s->noise * urand());
        }
        sc_rec_stats(rec, REC_LEN, HEAD, &obs[c]);
    }
}

static sc_as_status_t as_run(as_sim_t *s, sc_autoset_t *a, bool on1, bool on2,
                             uint8_t r1, uint8_t r2, uint8_t tb, uint8_t trig_src,
                             sc_rec_stats_t final_obs[2])
{
    sc_as_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.ch_on[0] = on1;
    cfg.ch_on[1] = on2;
    cfg.pos_volts = s->page;
    cfg.range_min = s->page ? 0u : 4u;
    cfg.mv = mvs;
    cfg.rate = rates;
    cfg.tb_min = 0x04;
    cfg.tb_max = 0x13;
    cfg.tb_probe = 0x10;
    cfg.window = 320;
    cfg.trig_src = trig_src;
    const uint8_t rr[2] = { r1, r2 };
    const int16_t pp[2] = { 0, 0 };
    sc_autoset_begin(a, &cfg, rr, pp, tb);
    s->records = 0;
    sc_as_status_t st = SC_AS_RUNNING;
    for (int i = 0; i < 100 && st == SC_AS_RUNNING; i++) {
        as_make_obs(s, a, final_obs);
        st = sc_autoset_step(a, final_obs);
    }
    as_make_obs(s, a, final_obs);   /* what the screen shows afterwards */
    return st;
}

static float periods_on_screen(const sc_autoset_t *a, float f)
{
    return 320.0f * f / rates[a->tb];
}

/* The code nearest 3 periods (in ratio) over 0x04..0x13: where the ladder
 * has a hole (0x0B/0x0C have no rate) 2..4 periods may not exist at all. */
static float best_framing_err(float f)
{
    float best = 1e30f;
    for (uint8_t c = 0x04; c <= 0x13; c++) {
        if (rates[c] <= 0.0f) continue;
        const float e = fabsf(logf(320.0f * f / rates[c] / 3.0f));
        if (e < best) best = e;
    }
    return best;
}

static bool framed_ok(const sc_autoset_t *a, float f)
{
    const float e = fabsf(logf(periods_on_screen(a, f) / 3.0f));
    return e <= best_framing_err(f) + 0.05f;
}

static void check_framed(const char *what, const sc_autoset_t *a, unsigned c,
                         const sc_rec_stats_t *o, bool centred)
{
    const int pp = (int)o->max - (int)o->min;
    CHECK(!o->clipped, "%s: CH%u clipped after autoset (r%u)", what, c + 1u, a->range[c]);
    CHECK(pp <= SC_AS_PP_MAX + 8 && pp >= SC_AS_PP_MAX * 2 / 5 - 8,
          "%s: CH%u spans %d counts (%.1f div) on r%u, want ~2.4..6 div",
          what, c + 1u, pp, pp / 32.0f, a->range[c]);
    if (centred) {
        const float mid = 0.5f * ((float)o->min + (float)o->max);
        CHECK(fabsf(mid - 100.0f) <= 8.0f, "%s: CH%u midline at %.1f, want ~100 (pos %d)",
              what, c + 1u, mid, a->pos[c]);
    }
}

static void test_autoset(void)
{
    for (uint8_t c = 0; c < SC_TB_CODES; c++) rates[c] = scope_timebase_sample_rate(c);
    for (uint8_t r = 0; r < SC_RANGES; r++) mvs[r] = scope_cal_nominal_mv(r);

    static as_sim_t s;
    sc_autoset_t a;
    sc_rec_stats_t o[2];
    sc_as_status_t st;

    /* 1 kHz, 2 Vpp sine on CH1, from the least and the most sensitive range
     * (the latter starts clipped) and from a far-off timebase. */
    const uint8_t starts[][2] = { { 9u, 0x06u }, { 0u, 0x13u }, { 5u, 0x0Au } };
    for (unsigned k = 0; k < 3; k++) {
        memset(&s, 0, sizeof(s));
        s.page = true;
        s.noise = 1.0f;
        s.sig[0] = (as_sig_t){ 0.0f, 1.0f, 1000.0f, false };
        st = as_run(&s, &a, true, false, starts[k][0], 9u, starts[k][1], 0u, o);
        char what[48];
        snprintf(what, sizeof(what), "1 kHz 2 Vpp from r%u tb 0x%02X", starts[k][0], starts[k][1]);
        CHECK(st == SC_AS_DONE, "%s: %s", what, sc_as_status_str(st));
        check_framed(what, &a, 0, &o[0], true);
        const float p = periods_on_screen(&a, 1000.0f);
        CHECK(p >= 1.5f && p <= 5.0f, "%s: %.2f periods on screen at tb 0x%02X", what, p, a.tb);
        CHECK(abs(a.trig_level) <= 8, "%s: trigger level %d, want ~0 (centred midline)",
              what, a.trig_level);
        CHECK(a.trig_ch == 0, "%s: trigger on CH1", what);
        printf("  %s -> r%u pos %d tb 0x%02X (%.2f periods), %d records\n",
               what, a.range[0], a.pos[0], a.tb, p, s.records);
    }

    /* Offset signal: 0.4 Vpp riding on +0.3 V. Centred by the position:
     * r3 (4 mV/count) puts the DC 75 counts up, within the +-100 reach. */
    memset(&s, 0, sizeof(s));
    s.page = true;
    s.noise = 1.0f;
    s.sig[0] = (as_sig_t){ 0.3f, 0.2f, 5000.0f, true };
    st = as_run(&s, &a, true, false, 9u, 9u, 0x0E, 0u, o);
    CHECK(st == SC_AS_DONE, "offset square: %s", sc_as_status_str(st));
    check_framed("offset square", &a, 0, &o[0], true);
    CHECK(a.range[0] == 3 && a.pos[0] < -60, "offset square: r3, position pulls the +0.3 V "
          "down (r%u pos %d)", a.range[0], a.pos[0]);

    /* Ripple on a big DC level (0.2 Vpp on +2 V): no range can centre it
     * within 6 div, so it is framed off centre but on screen, never clipped. */
    memset(&s, 0, sizeof(s));
    s.page = true;
    s.noise = 1.0f;
    s.sig[0] = (as_sig_t){ 2.0f, 0.1f, 5000.0f, false };
    st = as_run(&s, &a, true, false, 9u, 9u, 0x0E, 0u, o);
    CHECK(st == SC_AS_DONE && !o[0].clipped && a.pos[0] == -100,
          "ripple on 2 V: %s r%u pos %d clipped %d", sc_as_status_str(st), a.range[0], a.pos[0],
          o[0].clipped);

    /* Slow 50 Hz mains-like sine seen first at 50 MS/s: a record is flat
     * there, so the probe look finds it. */
    memset(&s, 0, sizeof(s));
    s.page = true;
    s.noise = 1.0f;
    s.sig[0] = (as_sig_t){ 0.0f, 5.0f, 50.0f, false };
    st = as_run(&s, &a, true, false, 9u, 9u, 0x05, 0u, o);
    CHECK(st == SC_AS_DONE, "50 Hz from tb 5: %s", sc_as_status_str(st));
    check_framed("50 Hz", &a, 0, &o[0], true);
    {
        const float p = periods_on_screen(&a, 50.0f);
        CHECK(p >= 1.5f && p <= 5.0f, "50 Hz: %.2f periods on screen at tb 0x%02X", p, a.tb);
    }

    /* Two channels: CH1 1 kHz 2 Vpp, CH2 10 kHz 0.2 Vpp, trigger on CH1. */
    memset(&s, 0, sizeof(s));
    s.page = true;
    s.noise = 1.0f;
    s.sig[0] = (as_sig_t){ 0.0f, 1.0f, 1000.0f, false };
    s.sig[1] = (as_sig_t){ 0.0f, 0.1f, 10000.0f, true };
    st = as_run(&s, &a, true, true, 9u, 9u, 0x0E, 0u, o);
    CHECK(st == SC_AS_DONE, "two channels: %s", sc_as_status_str(st));
    check_framed("two channels", &a, 0, &o[0], true);
    check_framed("two channels", &a, 1, &o[1], true);
    CHECK(a.range[1] < a.range[0], "CH2's smaller signal gets a more sensitive range (%u vs %u)",
          a.range[1], a.range[0]);
    CHECK(a.trig_ch == 0, "trigger stays on CH1");
    {
        const float p = periods_on_screen(&a, 1000.0f);
        CHECK(p >= 1.5f && p <= 5.0f, "two channels: timebase from CH1 (%.2f periods)", p);
    }

    /* Trigger source on a dead CH2: the trigger moves to CH1's signal. */
    memset(&s, 0, sizeof(s));
    s.page = true;
    s.noise = 1.0f;
    s.sig[0] = (as_sig_t){ 0.0f, 1.0f, 1000.0f, false };
    st = as_run(&s, &a, true, true, 9u, 7u, 0x0E, 1u, o);
    CHECK(st == SC_AS_DONE && a.trig_ch == 0, "dead CH2 source: trigger on CH1 (%u, %s)",
          a.trig_ch, sc_as_status_str(st));
    CHECK(a.range[1] == 7u && a.pos[1] == 0, "dead CH2 restored to r7 pos 0 (r%u pos %d)",
          a.range[1], a.pos[1]);

    /* DC only: +2 V. Shown off centre with the 0 V marker on screen; the
     * timebase is left where it was. */
    memset(&s, 0, sizeof(s));
    s.page = true;
    s.noise = 1.0f;
    s.sig[0] = (as_sig_t){ 2.0f, 0.0f, 0.0f, false };
    st = as_run(&s, &a, true, false, 9u, 9u, 0x0E, 0u, o);
    CHECK(st == SC_AS_DC, "DC 2 V: %s", sc_as_status_str(st));
    CHECK(a.tb == 0x0E, "DC: timebase kept (0x%02X)", a.tb);
    {
        const float off = o[0].mean - 100.0f;
        CHECK(!o[0].clipped && off > 20.0f && off <= 3.0f * 32.0f + 4.0f && a.pos[0] == 0,
              "DC: level %.1f counts above 0 V on r%u, pos %d", off, a.range[0], a.pos[0]);
    }

    /* Nothing connected: everything as it was. */
    memset(&s, 0, sizeof(s));
    s.page = true;
    s.noise = 1.0f;
    st = as_run(&s, &a, true, true, 6u, 8u, 0x0E, 0u, o);
    CHECK(st == SC_AS_NO_SIGNAL, "no signal: %s", sc_as_status_str(st));
    CHECK(a.range[0] == 6 && a.range[1] == 8 && a.tb == 0x0E && a.pos[0] == 0,
          "no signal: restored (r%u r%u tb 0x%02X)", a.range[0], a.range[1], a.tb);

    /* Far over range: clipped on r9, reported, not looping. */
    memset(&s, 0, sizeof(s));
    s.page = true;
    s.noise = 1.0f;
    s.sig[0] = (as_sig_t){ 0.0f, 80.0f, 1000.0f, false };
    st = as_run(&s, &a, true, false, 3u, 9u, 0x0E, 0u, o);
    CHECK(st == SC_AS_DONE && a.range[0] == 9 && a.res[0] == SC_ASC_OVER,
          "160 Vpp: r9 and OVER (r%u res %d)", a.range[0], (int)a.res[0]);

    /* Without a page: ranges r4..r9 only, the position is not touched. */
    memset(&s, 0, sizeof(s));
    s.page = false;
    s.noise = 1.0f;
    s.nopage_offset[0] = 110.0f;
    s.sig[0] = (as_sig_t){ 0.0f, 1.0f, 1000.0f, false };
    st = as_run(&s, &a, true, false, 9u, 9u, 0x0E, 0u, o);
    CHECK(st == SC_AS_DONE && a.range[0] >= 4 && a.pos[0] == 0, "no page: %s r%u pos %d",
          sc_as_status_str(st), a.range[0], a.pos[0]);
    check_framed("no page", &a, 0, &o[0], false);

    /* Torture: frequencies across the usable span always end framed. */
    const float fl[] = { 20.0f, 200.0f, 3000.0f, 40000.0f, 300000.0f, 2.0e6f, 9.7e6f };
    for (unsigned k = 0; k < sizeof(fl) / sizeof(fl[0]); k++) {
        memset(&s, 0, sizeof(s));
        s.page = true;
        s.noise = 1.0f;
        s.sig[0] = (as_sig_t){ 0.0f, 0.5f, fl[k], (k & 1u) != 0u };
        st = as_run(&s, &a, true, false, 9u, 9u, 0x0E, 0u, o);
        const float p = periods_on_screen(&a, fl[k]);
        CHECK(st == SC_AS_DONE && framed_ok(&a, fl[k]),
              "%.0f Hz: %s, %.2f periods at tb 0x%02X", fl[k], sc_as_status_str(st), p, a.tb);
    }
}

int main(void)
{
    test_stats();
    test_basecal_converges();
    test_basecal_refusals();
    test_basecal_noisy_ranges();
    test_basecal_open_input();
    test_trimmed_stats();
    test_autoset();
    if (failures) {
        printf("test_scope_selfcal: %d FAILED\n", failures);
        return 1;
    }
    printf("test_scope_selfcal: all passed\n");
    return 0;
}
