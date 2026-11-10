#include "decode_can.h"
#include <string.h>

/*
 * Classic CAN decoder (see decode_can.h).
 *
 * Bit timing: a frame is hard-synchronised on its start-of-frame edge; after
 * that every edge found within half a bit of an expected bit boundary
 * re-synchronises the grid (like a controller's resync), so drift from a node
 * whose clock differs from the assumed rate never accumulates over more than a
 * few bits (stuffing guarantees an edge at least every 6 bits). Each bit is read
 * at its centre.
 *
 * Start of frame: a recessive-to-dominant edge after at least 6 recessive bits.
 * Inside a frame, stuffing limits runs to 5 equal bits, so 6 recessive bits can
 * only be end-of-frame, intermission or idle: a SOF found this way is never a
 * mid-frame edge.
 */

static const char hex_chars[] = "0123456789ABCDEF";

#define DOM 0
#define REC 1

typedef struct {
    const int16_t *s;
    uint32_t n;
    int16_t  thr;
    int      rec_high;        /* 1: recessive is >= thr */
    float    spb;             /* samples per bit */
    float    edge;            /* sample index of the last synchronising edge */
    uint32_t k;               /* bit times since that edge */
    int      run_v;           /* destuffing: value of the current run, -1 none */
    uint32_t run_n;           /* destuffing: length of the current run */
    int      crc_on;
    uint16_t crc;
} cctx_t;

#define R_TRUNC  (-1)
#define R_STUFF  (-2)

static inline int lvl(const cctx_t *c, uint32_t i)
{
    int hi = c->s[i] >= c->thr;
    return (hi == c->rec_high) ? REC : DOM;
}

static void crc_bit(uint16_t *crc, int b)
{
    int nxt = b ^ ((*crc >> 14) & 1);
    *crc = (uint16_t)((*crc << 1) & 0x7FFF);
    if (nxt) *crc ^= 0x4599;
}

uint16_t can_crc15(const uint8_t *bits, uint32_t nbits)
{
    uint16_t crc = 0;
    for (uint32_t i = 0; i < nbits; i++) crc_bit(&crc, bits[i] ? 1 : 0);
    return crc;
}

/* One bus bit (stuff bits included). Resynchronises on an edge near the
 * expected start of the bit. With at least 4 samples per bit an edge needs two
 * stable samples on each side, so a one-sample spike cannot move the grid. */
static int raw_bit(cctx_t *c)
{
    float start = c->edge + (float)c->k * c->spb;
    int32_t lo = (int32_t)(start - 0.5f * c->spb + 0.5f);
    int32_t hi = (int32_t)(start + 0.5f * c->spb);
    if (lo < 1) lo = 1;
    if (hi > (int32_t)c->n - 2) hi = (int32_t)c->n - 2;
    if (c->k > 0) {   /* k == 0: just synchronised on this very edge */
        for (int32_t t = lo; t <= hi; t++) {
            int now = lvl(c, (uint32_t)t), before = lvl(c, (uint32_t)t - 1u);
            if (now == before) continue;
            /* With >= 4 samples per bit, a real edge has two stable samples on
             * each side; a one-sample spike has neither. */
            if (c->spb >= 4.0f && (t < 2 || lvl(c, (uint32_t)t + 1u) != now ||
                                   lvl(c, (uint32_t)t - 2u) != before))
                continue;
            {
                c->edge = (float)t;
                c->k = 0;
                start = (float)t;
                break;
            }
        }
    }
    float centre = start + 0.5f * c->spb;
    if (centre >= (float)c->n) return R_TRUNC;
    c->k++;
    return lvl(c, (uint32_t)centre);
}

/* One frame bit with de-stuffing; feeds the CRC while crc_on. */
static int bit_d(cctx_t *c)
{
    int b = raw_bit(c);
    if (b < 0) return b;
    if (c->run_n == 5) {                 /* this bus bit must be a stuff bit */
        if (b == c->run_v) return R_STUFF;
        c->run_v = b;
        c->run_n = 1;
        b = raw_bit(c);
        if (b < 0) return b;
    }
    if (b == c->run_v) c->run_n++;
    else { c->run_v = b; c->run_n = 1; }
    if (c->crc_on) crc_bit(&c->crc, b);
    return b;
}

static int field(cctx_t *c, uint32_t nbits, uint32_t *v)
{
    uint32_t x = 0;
    for (uint32_t i = 0; i < nbits; i++) {
        int b = bit_d(c);
        if (b < 0) return b;
        x = (x << 1) | (uint32_t)b;
    }
    *v = x;
    return 0;
}

static void make_label(decode_frame_t *f, int have_id)
{
    char *p = f->label, *end = f->label + sizeof(f->label) - 1;
    const char *tag = (f->flags & DECODE_FLAG_ERROR) ? "ERR" :
                      (f->flags & DECODE_FLAG_TRUNC) ? "..." :
                      (f->flags & DECODE_FLAG_CRC)   ? "CRC" :
                      (f->flags & DECODE_FLAG_ACK)   ? "ACK" :
                      (f->flags & DECODE_FLAG_NAK)   ? "NAK" : "";
    *p++ = 'I'; *p++ = 'D'; *p++ = ':';
    if (have_id) {
        int nd = (f->flags & DECODE_FLAG_EXT) ? 8 : 3;
        for (int i = nd - 1; i >= 0 && p < end; i--)
            *p++ = hex_chars[(f->id >> (4 * i)) & 0xF];
    } else {
        *p++ = '?';
    }
    if (*tag && p + 1 + (int)strlen(tag) <= end) {
        *p++ = ' ';
        while (*tag) *p++ = *tag++;
    }
    *p = '\0';
}

/* The level holding the longest run is the idle (recessive) level. */
static int auto_rec_high(const int16_t *s, uint32_t n, int16_t thr)
{
    uint32_t best = 0, run = 0;
    int best_hi = 1, cur = -1;
    for (uint32_t i = 0; i < n; i++) {
        int hi = s[i] >= thr;
        if (hi == cur) run++;
        else { cur = hi; run = 1; }
        if (run > best) { best = run; best_hi = hi; }
    }
    return best_hi;
}

void decode_can(const int16_t *samples, uint32_t num_samples,
                float sample_rate, const can_config_t *cfg,
                decode_result_t *result)
{
    memset(result, 0, sizeof(*result));
    result->type = DECODE_CAN;
    if (!samples || num_samples < 4 || !cfg || cfg->bit_rate == 0) return;

    cctx_t c;
    memset(&c, 0, sizeof(c));
    c.s = samples;
    c.n = num_samples;
    c.thr = cfg->threshold;
    c.spb = sample_rate / (float)cfg->bit_rate;
    if (c.spb < 2.0f) return;           /* cannot resolve bits */
    c.rec_high = (cfg->polarity == CAN_POL_RECESSIVE_HIGH) ? 1 :
                 (cfg->polarity == CAN_POL_RECESSIVE_LOW)  ? 0 :
                 auto_rec_high(samples, num_samples, cfg->threshold);

    const uint32_t min_idle = (uint32_t)(6.0f * c.spb);
    uint32_t i = 1, rec_run = (lvl(&c, 0) == REC) ? 1u : 0u;

    while (i < num_samples && result->num_frames < DECODE_MAX_FRAMES) {
        int l = lvl(&c, i);
        if (l == REC) { rec_run++; i++; continue; }
        if (rec_run < min_idle) { rec_run = 0; i++; continue; }

        /* Start of frame at sample i. */
        decode_frame_t *f = &result->frames[result->num_frames];
        memset(f, 0, sizeof(*f));
        f->start_sample = i;
        c.edge = (float)i;
        c.k = 0;
        c.run_v = -1;
        c.run_n = 0;
        c.crc_on = 1;
        c.crc = 0;

        int have_id = 0, r = 0;
        uint32_t id = 0, v = 0, dlc = 0;

        r = bit_d(&c);                                   /* SOF */
        if (r == DOM) r = field(&c, 11, &id);
        else if (r == REC) r = R_STUFF;                  /* not a SOF after all */
        if (r >= 0) {
            have_id = 1;
            int srr = bit_d(&c);                         /* RTR, or SRR */
            int ide = (srr >= 0) ? bit_d(&c) : srr;
            if (srr < 0) r = srr;
            else if (ide < 0) r = ide;
            else if (ide == REC) {                       /* 29-bit identifier */
                f->flags |= DECODE_FLAG_EXT;
                r = field(&c, 18, &v);
                if (r >= 0) {
                    id = (id << 18) | v;
                    int rtr = bit_d(&c);
                    if (rtr < 0) r = rtr;
                    else {
                        if (rtr == REC) f->flags |= DECODE_FLAG_RTR;
                        int r1 = bit_d(&c);
                        if (r1 < 0) r = r1;
                    }
                }
            } else if (srr == REC) {
                f->flags |= DECODE_FLAG_RTR;
            }
            if (r >= 0) {
                int r0 = bit_d(&c);
                if (r0 < 0) r = r0;
                else r = field(&c, 4, &dlc);
            }
        }
        f->id = id;

        if (r >= 0) {
            uint32_t nd = (f->flags & DECODE_FLAG_RTR) ? 0 : (dlc > 8 ? 8 : dlc);
            f->data_len = (uint8_t)nd;
            for (uint32_t d = 0; d < nd && r >= 0; d++) {
                r = field(&c, 8, &v);
                if (r >= 0) f->data[d] = (uint8_t)v;
            }
        }
        if (r >= 0) {
            uint16_t calc = c.crc;
            c.crc_on = 0;
            r = field(&c, 15, &v);
            if (r >= 0 && (uint16_t)v != calc) f->flags |= DECODE_FLAG_CRC;
        }
        if (r >= 0 && c.run_n == 5) {                    /* stuff bit after the CRC */
            int sb = raw_bit(&c);
            if (sb < 0) r = sb;
            else if (sb == c.run_v) r = R_STUFF;
        }
        if (r >= 0) {
            int del = raw_bit(&c);                       /* CRC delimiter */
            if (del < 0) r = del;
            else if (del != REC) f->flags |= DECODE_FLAG_ERROR;
        }
        if (r >= 0) {
            int ack = raw_bit(&c);                       /* ACK slot */
            if (ack < 0) r = ack;
            else f->flags |= (ack == DOM) ? DECODE_FLAG_ACK : DECODE_FLAG_NAK;
        }
        if (r >= 0) {
            int adel = raw_bit(&c);                      /* ACK delimiter */
            if (adel < 0) r = adel;
            else if (adel != REC) f->flags |= DECODE_FLAG_ERROR;
        }
        int eof_clean = 0;
        if (r >= 0) {                                    /* EOF: 7 recessive */
            eof_clean = 1;
            for (int e = 0; e < 7; e++) {
                int eb = raw_bit(&c);
                if (eb < 0) break;                       /* record ended in EOF: fine */
                if (eb != REC) { f->flags |= DECODE_FLAG_ERROR; eof_clean = 0; break; }
            }
        }

        if (r == R_TRUNC) f->flags |= DECODE_FLAG_TRUNC;
        if (r == R_STUFF) f->flags |= DECODE_FLAG_ERROR;

        uint32_t endpos = (uint32_t)(c.edge + (float)c.k * c.spb);
        if (endpos >= num_samples) endpos = num_samples - 1;
        if (endpos <= i) endpos = i + 1;
        f->end_sample = endpos;

        if (have_id) {
            make_label(f, have_id);
            result->num_frames++;
        }
        if (r == R_TRUNC) break;

        /* Resume after the frame. After a clean EOF the bus is known to be
         * recessive, and the next SOF may come after the 3-bit intermission;
         * after an error, idle must be seen again before trusting an edge. */
        i = endpos;
        rec_run = eof_clean ? min_idle : 0;
    }
}
