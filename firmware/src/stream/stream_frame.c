/*
 * OpenScope 2C53T - stream frame encoder (stream_frame.h)
 *
 * Built only with FEATURE_STREAM (the host test defines it). The whole frame
 * passes through one 64-byte packet buffer: the CRC is taken over the bytes
 * as they are copied into it, so what the CRC covers is exactly what left,
 * even when the source (a live record buffer) changes during the send.
 */
#if FEATURE_STREAM

#include "stream_frame.h"

#include <string.h>

/* ── checksums ──────────────────────────────────────────────────────── */
uint16_t sf_crc16(uint16_t crc, const uint8_t *p, uint32_t n)
{
    while (n--) {
        crc ^= (uint16_t)((uint16_t)*p++ << 8);
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return crc;
}

uint32_t sf_crc32_update(uint32_t crc, const uint8_t *p, uint32_t n)
{
    while (n--) {
        crc ^= *p++;
        for (int b = 0; b < 8; b++) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc;
}

uint32_t sf_table_crc_add(uint32_t crc, uint16_t id, uint8_t type, uint8_t unit, uint8_t decimals,
                          uint8_t group)
{
    const uint8_t b[6] = { (uint8_t)id, (uint8_t)(id >> 8), type, unit, decimals, group };
    return sf_crc32_update(crc, b, sizeof b);
}

/* ── writer ─────────────────────────────────────────────────────────── */
uint32_t sf_wire_len(uint16_t len)
{
    const uint32_t t = (uint32_t)SF_OVERHEAD + len;
    return (t % SF_PACKET) == 0u ? t + 1u : t;
}

static void flush(sf_writer_t *w)
{
    if (w->n == 0u) return;
    if (w->ok) {
        if (w->sink(w->pkt, w->n, w->ctx)) w->sent += w->n;
        else w->ok = false;
    }
    w->n = 0;
}

/* One byte onto the wire; crc says whether it is covered by the CRC-32. */
static inline void put_byte(sf_writer_t *w, uint8_t b, bool crc)
{
    if (crc) w->crc = sf_crc32_update(w->crc, &b, 1u);
    w->pkt[w->n++] = b;
    if (w->n == SF_PACKET) flush(w);
}

void sf_begin(sf_writer_t *w, sf_sink_fn sink, void *ctx, uint8_t type, uint8_t flags,
              uint16_t seq, uint16_t len)
{
    w->sink = sink;
    w->ctx  = ctx;
    w->crc  = 0xFFFFFFFFu;
    w->sent = 0;
    w->n    = 0;
    w->ok   = true;
    w->len  = len;
    w->left = len;
    uint8_t h[SF_HDR_LEN] = { SF_SOF0, SF_SOF1, type, flags, (uint8_t)seq, (uint8_t)(seq >> 8),
                              (uint8_t)len, (uint8_t)(len >> 8), 0, 0 };
    const uint16_t hc = sf_crc16(0xFFFFu, h, 8u);
    h[8] = (uint8_t)hc;
    h[9] = (uint8_t)(hc >> 8);
    for (unsigned i = 0; i < SF_HDR_LEN; i++) put_byte(w, h[i], i >= 2u);
}

void sf_put(sf_writer_t *w, const void *p, uint16_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    if (n > w->left) n = w->left;            /* never write past the declared length */
    w->left = (uint16_t)(w->left - n);
    while (n--) put_byte(w, *b++, true);
}

void sf_put_vol(sf_writer_t *w, const volatile uint8_t *p, uint16_t n)
{
    if (n > w->left) n = w->left;
    w->left = (uint16_t)(w->left - n);
    while (n--) put_byte(w, *p++, true);     /* each source byte read once */
}

void sf_u8(sf_writer_t *w, uint8_t v)   { sf_put(w, &v, 1u); }
void sf_u16(sf_writer_t *w, uint16_t v)
{
    const uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    sf_put(w, b, 2u);
}
void sf_u32(sf_writer_t *w, uint32_t v)
{
    const uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    sf_put(w, b, 4u);
}
void sf_i32(sf_writer_t *w, int32_t v) { sf_u32(w, (uint32_t)v); }
void sf_f32(sf_writer_t *w, float v)
{
    uint32_t u;
    memcpy(&u, &v, sizeof u);                /* IEEE-754 single on both targets */
    sf_u32(w, u);
}

uint16_t sf_str_len(const char *s, uint8_t max)
{
    size_t l = s ? strlen(s) : 0u;
    if (l > max) l = max;
    return (uint16_t)(1u + l);
}

void sf_str(sf_writer_t *w, const char *s, uint8_t max)
{
    const uint16_t l = (uint16_t)(sf_str_len(s, max) - 1u);
    sf_u8(w, (uint8_t)l);
    if (l) sf_put(w, s, l);
}

bool sf_end(sf_writer_t *w)
{
    bool full = (w->left == 0u);
    while (w->left) {                        /* keep the framing: zero-fill */
        w->left--;
        put_byte(w, 0u, true);
    }
    const uint32_t c = ~w->crc;
    put_byte(w, (uint8_t)c, false);
    put_byte(w, (uint8_t)(c >> 8), false);
    put_byte(w, (uint8_t)(c >> 16), false);
    put_byte(w, (uint8_t)(c >> 24), false);
    if (((uint32_t)SF_OVERHEAD + w->len) % SF_PACKET == 0u)
        put_byte(w, 0u, false);              /* end on a short packet */
    flush(w);
    return w->ok && full;
}

/* ── payloads ───────────────────────────────────────────────────────── */
#define FW_MAX 48u
#define STR_MAX 15u

uint16_t sf_hello_len(const sf_hello_t *h)
{
    return (uint16_t)(2u + 1u + 1u + 2u + 2u + 2u + 4u + sf_str_len(h->fw, FW_MAX));
}

bool sf_send_hello(sf_writer_t *w, sf_sink_fn s, void *ctx, uint16_t seq, const sf_hello_t *h)
{
    sf_begin(w, s, ctx, SF_T_HELLO, 0u, seq, sf_hello_len(h));
    sf_u8(w, SF_PROTO_MAJOR);
    sf_u8(w, SF_PROTO_MINOR);
    sf_u8(w, h->caps);
    sf_u8(w, h->on);
    sf_u16(w, h->max_payload);
    sf_u16(w, h->rec_samples);
    sf_u16(w, h->n_props);
    sf_u32(w, h->table_crc);
    sf_str(w, h->fw, FW_MAX);
    return sf_end(w);
}

uint16_t sf_record_len(const sf_record_t *r)
{
    const uint8_t nch = r->nch > 2u ? 2u : r->nch;
    return (uint16_t)(SF_REC_FIXED_LEN + nch * (SF_REC_CH_LEN + (uint32_t)r->n) + SF_REC_TAIL_LEN);
}

bool sf_send_record(sf_writer_t *w, sf_sink_fn s, void *ctx, uint16_t seq, const sf_record_t *r,
                    uint32_t (*gen_end_fn)(void))
{
    const uint8_t nch = r->nch > 2u ? 2u : r->nch;
    sf_begin(w, s, ctx, SF_T_RECORD, 0u, seq, sf_record_len(r));
    sf_u32(w, r->gen);
    sf_u32(w, r->t_ms);
    sf_f32(w, r->fs_hz);
    sf_f32(w, r->s_per_div);
    sf_u16(w, r->n);
    sf_u16(w, r->trig_idx);
    sf_u8(w, r->flags);
    sf_u8(w, r->kind);
    sf_u8(w, r->tb_code);
    sf_u8(w, nch);
    for (uint8_t i = 0; i < nch; i++) {
        const sf_rec_ch_t *c = &r->ch[i];
        sf_u8(w, c->ch);
        sf_u8(w, c->vdiv_idx);
        sf_u8(w, c->zero_code);
        sf_u8(w, c->coupling);
        sf_u16(w, (uint16_t)c->position);
        sf_u16(w, c->probe_x);
        sf_f32(w, c->volts_per_count);
        sf_f32(w, c->volts_per_div);
    }
    for (uint8_t i = 0; i < nch && w->ok; i++)
        sf_put_vol(w, r->ch[i].codes, r->n);
    sf_u32(w, gen_end_fn ? gen_end_fn() : r->gen);
    return sf_end(w);
}

uint16_t sf_meter_len(const sf_meter_t *m)
{
    return (uint16_t)(4u + 4u + 4u + 4u + 7u + sf_str_len(m->unit, STR_MAX) +
                      sf_str_len(m->display, STR_MAX) + sf_str_len(m->range_label, STR_MAX));
}

bool sf_send_meter(sf_writer_t *w, sf_sink_fn s, void *ctx, uint16_t seq, const sf_meter_t *m)
{
    sf_begin(w, s, ctx, SF_T_METER, 0u, seq, sf_meter_len(m));
    sf_u32(w, m->update_count);
    sf_u32(w, m->t_ms);
    sf_f32(w, m->value);
    sf_i32(w, m->bcd);
    sf_u8(w, m->func);
    sf_u8(w, m->auto_fn);
    sf_u8(w, m->result_class);
    sf_u8(w, m->status_code);
    sf_u8(w, m->decimal_pos);
    sf_u8(w, m->range_index);
    sf_u8(w, m->flags);
    sf_str(w, m->unit, STR_MAX);
    sf_str(w, m->display, STR_MAX);
    sf_str(w, m->range_label, STR_MAX);
    return sf_end(w);
}

uint16_t sf_stats_len(void)
{
    return (uint16_t)(5u * 4u + 1u + (SF_T_COUNT - 1u) * 9u);
}

bool sf_send_stats(sf_writer_t *w, sf_sink_fn s, void *ctx, uint16_t seq, const sf_stats_t *st)
{
    sf_begin(w, s, ctx, SF_T_STATS, 0u, seq, sf_stats_len());
    sf_u32(w, st->t_ms);
    sf_u32(w, st->bytes);
    sf_u32(w, st->rec_missed);
    sf_u32(w, st->rec_torn);
    sf_u32(w, st->aborts);
    sf_u8(w, (uint8_t)(SF_T_COUNT - 1u));
    for (uint8_t t = 1; t < SF_T_COUNT; t++) {
        sf_u8(w, t);
        sf_u32(w, st->sent[t]);
        sf_u32(w, st->dropped[t]);
    }
    return sf_end(w);
}

uint16_t sf_propdesc_len(const sf_propdesc_t *d)
{
    return (uint16_t)(2u + 4u + 2u + 12u + sf_str_len(d->name, 31u) + sf_str_len(d->unit_sym, 7u));
}

bool sf_send_propdesc(sf_writer_t *w, sf_sink_fn s, void *ctx, uint16_t seq, const sf_propdesc_t *d)
{
    sf_begin(w, s, ctx, SF_T_PROPDESC, 0u, seq, sf_propdesc_len(d));
    sf_u16(w, d->id);
    sf_u8(w, d->type);
    sf_u8(w, d->unit);
    sf_u8(w, d->decimals);
    sf_u8(w, d->group);
    sf_u16(w, d->flags);
    sf_i32(w, d->min);
    sf_i32(w, d->max);
    sf_i32(w, d->def);
    sf_str(w, d->name, 31u);
    sf_str(w, d->unit_sym, 7u);
    return sf_end(w);
}

uint16_t sf_props_len(uint8_t n_groups, uint16_t n_props)
{
    return (uint16_t)(4u + 1u + 4u * (uint32_t)n_groups + 2u + SF_PROPS_PAIR_LEN * (uint32_t)n_props);
}

bool sf_send_props(sf_writer_t *w, sf_sink_fn s, void *ctx, uint16_t seq, uint32_t table_crc,
                   uint8_t n_groups, const uint32_t *epochs, uint16_t n_props,
                   sf_prop_t (*get)(uint16_t i, void *gctx), void *gctx)
{
    sf_begin(w, s, ctx, SF_T_PROPS, 0u, seq, sf_props_len(n_groups, n_props));
    sf_u32(w, table_crc);
    sf_u8(w, n_groups);
    for (uint8_t g = 0; g < n_groups; g++) sf_u32(w, epochs[g]);
    sf_u16(w, n_props);
    for (uint16_t i = 0; i < n_props && w->ok; i++) {
        const sf_prop_t p = get(i, gctx);
        sf_u16(w, p.id);
        sf_i32(w, p.value);
    }
    return sf_end(w);
}

#endif /* FEATURE_STREAM */
