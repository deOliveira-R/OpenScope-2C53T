/*
 * Host test for the stream frame encoder (src/stream/stream_frame.c,
 * docs/stream_protocol.md).
 *
 *   - CRC-16/CCITT-FALSE and CRC-32 check values;
 *   - header fields, header CRC, frame CRC, packet chunking (every packet
 *     64 bytes but the last), the pad rule, sf_wire_len;
 *   - a sink that refuses mid-frame (abandoned, never called again), a short
 *     payload (zero-filled, framing intact, reported), an over-long put;
 *   - every payload's declared length equals what is written, and the
 *     RECORD / PROPS fields decode back;
 *   - a torn record: gen_end is read after the samples.
 *
 * Cross-check: it writes a stream of text and frames to
 * tests/fixtures/stream/vectors.bin and, from the INPUT structures (not the
 * encoded bytes), the expected decode to vectors_expect.txt. The committed
 * files must match byte for byte (STREAM_VECTORS_UPDATE=1 rewrites them);
 * `python3 scripts/scope_stream.py --selftest` decodes the same files with
 * the host's independent decoder.
 *
 * Build/run:  make -C firmware test-stream
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/stream/stream_frame.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ── capture sink ───────────────────────────────────────────────────── */
typedef struct {
    uint8_t  buf[16384];
    uint32_t n;
    uint16_t pkt_len[512];
    uint32_t pkts;
    int      refuse_at;            /* packet index to refuse, -1 = never */
    uint32_t calls_after_refuse;
    int      refused;
} cap_t;

static bool cap_sink(const uint8_t *d, uint16_t len, void *ctx)
{
    cap_t *c = (cap_t *)ctx;
    if (c->refused) { c->calls_after_refuse++; return false; }
    if (c->refuse_at >= 0 && (int)c->pkts == c->refuse_at) { c->refused = 1; return false; }
    if (len > SF_PACKET || c->n + len > sizeof c->buf) return false;
    memcpy(c->buf + c->n, d, len);
    c->n += len;
    c->pkt_len[c->pkts++] = len;
    return true;
}

static void cap_init(cap_t *c) { memset(c, 0, sizeof *c); c->refuse_at = -1; }

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static float rdf(const uint8_t *p) { uint32_t u = rd32(p); float f; memcpy(&f, &u, 4); return f; }

/* Validate one frame at buf[0..]: returns its payload pointer, or NULL. */
static const uint8_t *frame_ok(const uint8_t *b, uint32_t avail, uint8_t *type, uint16_t *seq,
                               uint16_t *len, uint32_t *wire)
{
    if (avail < SF_OVERHEAD || b[0] != SF_SOF0 || b[1] != SF_SOF1) return NULL;
    if (sf_crc16(0xFFFFu, b, 8u) != rd16(b + 8)) return NULL;
    *type = b[2];
    *seq  = rd16(b + 4);
    *len  = rd16(b + 6);
    if (avail < (uint32_t)SF_OVERHEAD + *len) return NULL;
    const uint32_t c = ~sf_crc32_update(0xFFFFFFFFu, b + 2, 8u + *len);
    if (c != rd32(b + SF_HDR_LEN + *len)) return NULL;
    *wire = sf_wire_len(*len);
    return b + SF_HDR_LEN;
}

static void check_chunks(const cap_t *c)
{
    for (uint32_t i = 0; i + 1u < c->pkts; i++) CHECK(c->pkt_len[i] == SF_PACKET);
    if (c->pkts) CHECK(c->pkt_len[c->pkts - 1u] < SF_PACKET);   /* ends on a short packet */
}

/* ── checksums ──────────────────────────────────────────────────────── */
static void test_crc(void)
{
    const uint8_t s[] = "123456789";
    CHECK(sf_crc16(0xFFFFu, s, 9u) == 0x29B1u);
    CHECK(~sf_crc32_update(0xFFFFFFFFu, s, 9u) == 0xCBF43926u);
}

/* ── writer ─────────────────────────────────────────────────────────── */
static void test_writer_basic(void)
{
    for (uint16_t len = 0; len < 300; len++) {
        static cap_t c;
        cap_init(&c);
        sf_writer_t w;
        uint8_t pl[300];
        for (uint16_t i = 0; i < len; i++) pl[i] = (uint8_t)(i * 7u + len);
        sf_begin(&w, cap_sink, &c, 0x42u, 0u, (uint16_t)(1000u + len), len);
        sf_put(&w, pl, len);
        CHECK(sf_end(&w));
        CHECK(c.n == sf_wire_len(len));
        CHECK(w.sent == c.n);
        check_chunks(&c);
        uint8_t t; uint16_t sq, l; uint32_t wire;
        const uint8_t *p = frame_ok(c.buf, c.n, &t, &sq, &l, &wire);
        CHECK(p != NULL);
        if (!p) continue;
        CHECK(t == 0x42u && sq == 1000u + len && l == len && wire == c.n);
        CHECK(memcmp(p, pl, len) == 0);
        const bool padded = ((SF_OVERHEAD + len) % SF_PACKET) == 0u;
        CHECK(padded == (c.n == SF_OVERHEAD + len + 1u));
        if (padded) CHECK(c.buf[c.n - 1u] == 0u);
    }
}

static void test_writer_refuse(void)
{
    static cap_t c;
    cap_init(&c);
    c.refuse_at = 3;
    uint8_t pl[1000];
    memset(pl, 0x5A, sizeof pl);
    sf_writer_t w;
    sf_begin(&w, cap_sink, &c, SF_T_RECORD, 0u, 7u, sizeof pl);
    sf_put(&w, pl, sizeof pl);
    CHECK(!sf_end(&w));
    CHECK(c.calls_after_refuse == 0u);            /* never called again */
    CHECK(c.n == 3u * SF_PACKET && w.sent == c.n);
}

static void test_writer_short_long(void)
{
    static cap_t c;
    cap_init(&c);
    sf_writer_t w;
    sf_begin(&w, cap_sink, &c, 0x10u, 0u, 1u, 10u);
    sf_u32(&w, 0x11223344u);
    CHECK(!sf_end(&w));                           /* short: reported */
    uint8_t t; uint16_t sq, l; uint32_t wire;
    const uint8_t *p = frame_ok(c.buf, c.n, &t, &sq, &l, &wire);
    CHECK(p != NULL && l == 10u && rd32(p) == 0x11223344u && p[4] == 0 && p[9] == 0);

    cap_init(&c);
    sf_begin(&w, cap_sink, &c, 0x10u, 0u, 2u, 3u);
    sf_u32(&w, 0xAABBCCDDu);                      /* one byte too many: truncated */
    CHECK(sf_end(&w));
    p = frame_ok(c.buf, c.n, &t, &sq, &l, &wire);
    CHECK(p != NULL && l == 3u && p[0] == 0xDDu && p[2] == 0xBBu && c.n == SF_OVERHEAD + 3u);
}

/* ── payloads ───────────────────────────────────────────────────────── */
static uint8_t ch1_codes[1024], ch2_codes[1024];
static uint32_t g_gen_end;
static uint32_t gen_end_fn(void) { return g_gen_end; }

static void fill_codes(void)
{
    for (int i = 0; i < 1024; i++) {
        ch1_codes[i] = (uint8_t)(100 + (i % 50));
        ch2_codes[i] = (uint8_t)(255 - (i & 0xFF));
    }
}

static sf_record_t make_record(uint8_t nch, uint32_t gen)
{
    sf_record_t r;
    memset(&r, 0, sizeof r);
    r.gen = gen; r.t_ms = 123456u; r.fs_hz = 5.0e6f; r.s_per_div = 5.0e-6f;
    r.n = 1024u; r.trig_idx = 512u;
    r.flags = SF_REC_TIME_ORDERED | SF_REC_COHERENT | SF_REC_RUNNING | SF_REC_TRUE_SCALE;
    r.kind = 1u; r.tb_code = 10u; r.nch = nch;
    r.ch[0] = (sf_rec_ch_t){ 0u, 3u, 100u, 0u, 0, 1u, 0.0390625f, 1.0f, ch1_codes };
    r.ch[1] = (sf_rec_ch_t){ 1u, 5u, 105u, 1u, 5, 10u, 0.75f, 20.0f, ch2_codes };
    r.trig_level = -25; r.trig_edge = 1u; r.trig_src = 0u; r.trig_mode = 0u; r.cap_mask = nch == 2u ? 3u : 1u;
    return r;
}

static void test_record(void)
{
    static cap_t c;
    for (uint8_t nch = 1; nch <= 2; nch++) {
        cap_init(&c);
        sf_record_t r = make_record(nch, 40u);
        g_gen_end = 42u;                              /* a commit during the send */
        sf_writer_t w;
        CHECK(sf_send_record(&w, cap_sink, &c, 9u, &r, gen_end_fn));
        uint8_t t; uint16_t sq, l; uint32_t wire;
        const uint8_t *p = frame_ok(c.buf, c.n, &t, &sq, &l, &wire);
        CHECK(p && t == SF_T_RECORD && l == sf_record_len(&r));
        CHECK(sf_wire_len(l) % SF_PACKET != 0u);       /* both sizes: no pad needed */
        CHECK(((uint32_t)SF_OVERHEAD + l) % SF_PACKET != 0u);
        if (!p) continue;
        CHECK(rd32(p) == 40u && rd32(p + 4) == 123456u && rdf(p + 8) == 5.0e6f);
        CHECK(rdf(p + 12) == 5.0e-6f && rd16(p + 16) == 1024u && rd16(p + 18) == 512u);
        CHECK(p[23] == nch);
        const uint8_t *ct = p + SF_REC_FIXED_LEN;
        CHECK(ct[0] == 0u && ct[1] == 3u && ct[2] == 100u && rdf(ct + 8) == 0.0390625f);
        if (nch == 2) CHECK(ct[16] == 1u && (int16_t)rd16(ct + 20) == 5 && rd16(ct + 22) == 10u);
        const uint8_t *s = ct + nch * SF_REC_CH_LEN;
        CHECK(memcmp(s, ch1_codes, 1024) == 0);
        if (nch == 2) CHECK(memcmp(s + 1024, ch2_codes, 1024) == 0);
        CHECK(rd32(s + nch * 1024u) == 42u);           /* gen_end after the samples */
        const uint8_t *cb = s + nch * 1024u + 4u;      /* 1.1 capture block */
        CHECK((int16_t)rd16(cb) == -25 && cb[2] == 1u && cb[3] == 0u && cb[4] == 0u);
        CHECK(cb[5] == (nch == 2u ? 3u : 1u) && cb[6] == SF_REC_CAP_VERSION);
        CHECK(cb + SF_REC_CAP_LEN == p + l);
    }
}

static const sf_prop_t g_props[] = {
    { 0x0101, 1 }, { 0x0102, 3 }, { 0x0201, 0 }, { 0x0301, 10 }, { 0x0404, -25 }, { 0x0602, 50000 },
};
static sf_prop_t props_get(uint16_t i, void *ctx) { (void)ctx; return g_props[i]; }
static const uint32_t g_epochs[3] = { 1u, 0u, 0xDEADBEEFu };

static void test_props(void)
{
    static cap_t c;
    cap_init(&c);
    sf_writer_t w;
    const uint16_t n = (uint16_t)(sizeof g_props / sizeof g_props[0]);
    CHECK(sf_send_props(&w, cap_sink, &c, 3u, 0xCAFEF00Du, 3u, g_epochs, n, props_get, NULL));
    uint8_t t; uint16_t sq, l; uint32_t wire;
    const uint8_t *p = frame_ok(c.buf, c.n, &t, &sq, &l, &wire);
    CHECK(p && t == SF_T_PROPS && l == sf_props_len(3u, n));
    if (!p) return;
    CHECK(rd32(p) == 0xCAFEF00Du && p[4] == 3u && rd32(p + 5 + 8) == 0xDEADBEEFu);
    const uint8_t *q = p + 5 + 12;
    CHECK(rd16(q) == n);
    for (uint16_t i = 0; i < n; i++) {
        CHECK(rd16(q + 2 + 6 * i) == g_props[i].id);
        CHECK((int32_t)rd32(q + 4 + 6 * i) == g_props[i].value);
    }
    /* a 62-property snapshot fits one frame */
    CHECK(sf_props_len(12u, 62u) <= SF_MAX_PAYLOAD);
    /* table identity: order and every field count */
    uint32_t a = sf_table_crc_add(0xFFFFFFFFu, 0x0101, 0, 0, 0, 0);
    uint32_t b = sf_table_crc_add(0xFFFFFFFFu, 0x0101, 0, 0, 1, 0);
    CHECK(a != b);
}

static void test_lengths(void)
{
    static cap_t c;
    sf_writer_t w;
    const sf_hello_t h = { SF_CAP_SCOPE | SF_CAP_METER | SF_CAP_PROPS, SF_CAP_SCOPE, SF_MAX_PAYLOAD,
                           1024u, 62u, 0x12345678u, "OpenScope test", 0x00000005u };
    cap_init(&c);
    CHECK(sf_send_hello(&w, cap_sink, &c, 0u, &h) && c.n == sf_wire_len(sf_hello_len(&h)));
    const sf_meter_t m = { 7u, 99u, -1.234f, -1234, 1u, 0u, 1u, 0u, 3u, 2u, SF_MTR_VALID | SF_MTR_NEGATIVE,
                           "V", "-1.234", "6V" };
    cap_init(&c);
    CHECK(sf_send_meter(&w, cap_sink, &c, 1u, &m) && c.n == sf_wire_len(sf_meter_len(&m)));
    sf_stats_t st;
    memset(&st, 0, sizeof st);
    cap_init(&c);
    CHECK(sf_send_stats(&w, cap_sink, &c, 2u, &st) && c.n == sf_wire_len(sf_stats_len()));
    const sf_propdesc_t d = { 0x0102, 1u, 0u, 0u, 0u, 0x000A, 0, 9, 3, "ch1.vdiv", "" };
    cap_init(&c);
    CHECK(sf_send_propdesc(&w, cap_sink, &c, 3u, &d) && c.n == sf_wire_len(sf_propdesc_len(&d)));
    /* a long string is cut at its field's maximum */
    const sf_meter_t m2 = { 1u, 1u, 0.0f, 0, 0u, 0u, 0u, 0u, 0u, 0u, 0u, "abcdefghijklmnopqrstuvwxyz", "", NULL };
    CHECK(sf_meter_len(&m2) == 4 + 4 + 4 + 4 + 7 + 16 + 1 + 1);
}

/* ── cross-check vectors ─────────────────────────────────────────────── */
static cap_t vec;
static FILE *vexp;
static uint16_t vseq;

static void vtext(const char *s)
{
    const size_t n = strlen(s);
    memcpy(vec.buf + vec.n, s, n);
    vec.n += (uint32_t)n;
    fprintf(vexp, "TEXT ");
    for (size_t i = 0; i < n; i++) fprintf(vexp, "%02x", (uint8_t)s[i]);
    fprintf(vexp, "\n");
}

static uint32_t sum_codes(const uint8_t *c, uint16_t n)
{
    uint32_t s = 0;
    for (uint16_t i = 0; i < n; i++) s += c[i];
    return s;
}

static void vrecord(sf_record_t *r, uint32_t gen_end)
{
    sf_writer_t w;
    g_gen_end = gen_end;
    CHECK(sf_send_record(&w, cap_sink, &vec, vseq, r, gen_end_fn));
    fprintf(vexp, "FRAME type=3 seq=%u gen=%u gen_end=%u t_ms=%u fs_hz=%.9g s_per_div=%.9g n=%u trig_idx=%u "
                  "flags=%u kind=%u tb_code=%u nch=%u",
            vseq, r->gen, gen_end, r->t_ms, (double)r->fs_hz, (double)r->s_per_div, r->n, r->trig_idx,
            r->flags, r->kind, r->tb_code, r->nch);
    fprintf(vexp, " trig_level=%d trig_edge=%u trig_src=%u trig_mode=%u cap_mask=%u cap_version=%u",
            r->trig_level, r->trig_edge, r->trig_src, r->trig_mode, r->cap_mask, SF_REC_CAP_VERSION);
    for (uint8_t i = 0; i < r->nch; i++) {
        const sf_rec_ch_t *c = &r->ch[i];
        fprintf(vexp, " ch%u.id=%u ch%u.vdiv_idx=%u ch%u.zero_code=%u ch%u.coupling=%u ch%u.position=%d "
                      "ch%u.probe_x=%u ch%u.volts_per_count=%.9g ch%u.volts_per_div=%.9g ch%u.sum=%u "
                      "ch%u.first=%u ch%u.last=%u",
                i, c->ch, i, c->vdiv_idx, i, c->zero_code, i, c->coupling, i, c->position, i, c->probe_x,
                i, (double)c->volts_per_count, i, (double)c->volts_per_div,
                i, sum_codes((const uint8_t *)c->codes, r->n), i, c->codes[0], i, c->codes[r->n - 1]);
    }
    fprintf(vexp, "\n");
    vseq++;
}

static void vstr(const char *k, const char *s)
{
    fprintf(vexp, " %s=", k);
    for (const char *p = s ? s : ""; *p; p++) fprintf(vexp, "%02x", (uint8_t)*p);
}

static int write_vectors(const char *bin_path, const char *exp_path, const char *tmp_bin, const char *tmp_exp)
{
    cap_init(&vec);
    vexp = fopen(tmp_exp, "w");
    if (!vexp) { printf("FAIL cannot write %s\n", tmp_exp); fails++; return 1; }
    vseq = 65530u;                                /* wraps inside the stream */
    sf_writer_t w;

    vtext("\r\n> stream on\r\nstream: on scope meter props\r\n> ");

    const sf_hello_t h = { SF_CAP_SCOPE | SF_CAP_METER | SF_CAP_PROPS,
                           SF_CAP_SCOPE | SF_CAP_METER | SF_CAP_PROPS, SF_MAX_PAYLOAD, 1024u, 6u,
                           0x0BADF00Du, "OpenScope 2C53T vectors", 0x80000005u };
    CHECK(sf_send_hello(&w, cap_sink, &vec, vseq, &h));
    fprintf(vexp, "FRAME type=1 seq=%u proto_major=%u proto_minor=%u caps=%u on=%u max_payload=%u "
                  "rec_samples=%u n_props=%u table_crc=%u", vseq, SF_PROTO_MAJOR, SF_PROTO_MINOR, h.caps,
            h.on, h.max_payload, h.rec_samples, h.n_props, h.table_crc);
    vstr("fw", h.fw);
    fprintf(vexp, " features=%u\n", (unsigned)h.features);
    vseq++;

    static const sf_propdesc_t descs[2] = {
        { 0x0102, 1u, 0u, 0u, 0u, 0x000A, 0, 9, 3, "ch1.vdiv", "" },
        { 0x0603, 2u, 1u, 1u, 5u, 0x000A, 0, 30, 30, "gen.amp", "V" },
    };
    for (int i = 0; i < 2; i++) {
        const sf_propdesc_t *d = &descs[i];
        CHECK(sf_send_propdesc(&w, cap_sink, &vec, vseq, d));
        fprintf(vexp, "FRAME type=6 seq=%u id=%u type_=%u unit=%u decimals=%u group=%u flags=%u min=%d max=%d def=%d",
                vseq, d->id, d->type, d->unit, d->decimals, d->group, d->flags, d->min, d->max, d->def);
        vstr("name", d->name);
        vstr("unit_sym", d->unit_sym);
        fprintf(vexp, "\n");
        vseq++;
    }

    const uint16_t n = (uint16_t)(sizeof g_props / sizeof g_props[0]);
    CHECK(sf_send_props(&w, cap_sink, &vec, vseq, 0x0BADF00Du, 3u, g_epochs, n, props_get, NULL));
    fprintf(vexp, "FRAME type=2 seq=%u table_crc=%u n_groups=3 epochs=%u,%u,%u n_props=%u props=",
            vseq, 0x0BADF00Du, g_epochs[0], g_epochs[1], g_epochs[2], n);
    for (uint16_t i = 0; i < n; i++) fprintf(vexp, "%s%u:%d", i ? "," : "", g_props[i].id, g_props[i].value);
    fprintf(vexp, "\n");
    vseq++;

    sf_record_t r2 = make_record(2u, 1000u);
    vrecord(&r2, 1000u);                          /* clean */
    vtext("version\r\nOpenScope 2C53T\r\nBuild: test \xe2\x80\x94 vectors\r\n> ");
    sf_record_t r1 = make_record(1u, 1004u);
    r1.flags = SF_REC_INTERLEAVED | SF_REC_RUNNING | SF_REC_STALE;
    r1.trig_idx = SF_TRIG_NONE;
    r1.fs_hz = 2.5e8f;
    vrecord(&r1, 1006u);                          /* torn: a commit during the send */

    const sf_meter_t m = { 77u, 5000u, -12.5f, -1250, 1u, 0u, 1u, 0u, 2u, 1u,
                           SF_MTR_VALID | SF_MTR_NEGATIVE | SF_MTR_AUTORANGE, "V", "-12.50", "60V" };
    CHECK(sf_send_meter(&w, cap_sink, &vec, vseq, &m));
    fprintf(vexp, "FRAME type=4 seq=%u update_count=%u t_ms=%u value=%.9g bcd=%d func=%u auto_fn=%u "
                  "result_class=%u status_code=%u decimal_pos=%u range_index=%u flags=%u",
            vseq, m.update_count, m.t_ms, (double)m.value, m.bcd, m.func, m.auto_fn, m.result_class,
            m.status_code, m.decimal_pos, m.range_index, m.flags);
    vstr("unit", m.unit); vstr("display", m.display); vstr("range_label", m.range_label);
    fprintf(vexp, "\n");
    vseq++;

    /* an unknown type the host must skip by length, with a payload that
     * ends the frame on a packet boundary (so the pad byte appears) */
    {
        const uint16_t len = (uint16_t)(2u * SF_PACKET - SF_OVERHEAD);
        sf_begin(&w, cap_sink, &vec, 0x7Eu, 0u, vseq, len);
        for (uint16_t i = 0; i < len; i++) sf_u8(&w, (uint8_t)(i ^ 0xA5u));
        CHECK(sf_end(&w));
        fprintf(vexp, "FRAME type=126 seq=%u len=%u\n", vseq, len);
        vseq++;
    }

    sf_stats_t st;
    memset(&st, 0, sizeof st);
    st.t_ms = 6000u; st.bytes = 123456u; st.rec_missed = 3u; st.rec_torn = 1u; st.aborts = 2u;
    for (uint8_t t = 1; t < SF_T_COUNT; t++) { st.sent[t] = 10u * t; st.dropped[t] = t; }
    CHECK(sf_send_stats(&w, cap_sink, &vec, vseq, &st));
    fprintf(vexp, "FRAME type=5 seq=%u t_ms=%u bytes=%u rec_missed=%u rec_torn=%u aborts=%u", vseq, st.t_ms,
            st.bytes, st.rec_missed, st.rec_torn, st.aborts);
    for (uint8_t t = 1; t < SF_T_COUNT; t++) fprintf(vexp, " sent%u=%u dropped%u=%u", t, st.sent[t], t, st.dropped[t]);
    fprintf(vexp, "\n");
    vseq++;

    vtext("stream off\r\nstream: on=0\r\n> ");
    fclose(vexp);

    FILE *f = fopen(tmp_bin, "wb");
    if (!f) { printf("FAIL cannot write %s\n", tmp_bin); fails++; return 1; }
    fwrite(vec.buf, 1, vec.n, f);
    fclose(f);

    /* compare with (or update) the committed files */
    const char *upd = getenv("STREAM_VECTORS_UPDATE");
    const char *pairs[2][2] = { { tmp_bin, bin_path }, { tmp_exp, exp_path } };
    for (int i = 0; i < 2; i++) {
        FILE *a = fopen(pairs[i][0], "rb");
        FILE *b = fopen(pairs[i][1], "rb");
        int same = (a && b);
        while (same) {
            const int ca = fgetc(a), cb = fgetc(b);
            if (ca != cb) same = 0;
            if (ca == EOF || cb == EOF) break;
        }
        if (a) fclose(a);
        if (b) fclose(b);
        if (!same) {
            if (upd && upd[0] == '1') {
                if (rename(pairs[i][0], pairs[i][1]) != 0) { printf("FAIL rename to %s\n", pairs[i][1]); fails++; }
                else printf("updated %s\n", pairs[i][1]);
            } else {
                printf("FAIL %s differs from the encoder's output (%s); rerun with STREAM_VECTORS_UPDATE=1 "
                       "if the change is intended\n", pairs[i][1], pairs[i][0]);
                fails++;
            }
        }
    }
    return 0;
}

int main(void)
{
    fill_codes();
    test_crc();
    test_writer_basic();
    test_writer_refuse();
    test_writer_short_long();
    test_record();
    test_props();
    test_lengths();
    write_vectors("tests/fixtures/stream/vectors.bin", "tests/fixtures/stream/vectors_expect.txt",
                  "build/test_stream_vectors.bin", "build/test_stream_vectors_expect.txt");
    printf("test_stream: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
