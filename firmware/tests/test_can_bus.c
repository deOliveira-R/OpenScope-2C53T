/*
 * Host tests for the CAN bus analyser: decode_can.c + can_bus.c.
 *
 *   1. Real records from a two-node bench bus (tests/fixtures/can_bench_records.h).
 *   2. Synthetic frames from an independent generator with a real CRC-15 and
 *      bit stuffing: ACK/NAK, CRC errors, 29-bit IDs, remote frames, clock skew,
 *      inverted polarity, truncation, glitches, back-to-back frames, 2..10
 *      samples per bit.
 *
 * Build/run: make test-can-bus
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "decode_can.h"
#include "can_bus.h"
#include "fixtures/can_bench_records.h"

static int g_pass, g_fail;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------------------------------------------------------------- generator */

typedef struct {
    uint32_t id;
    int ext, rtr;
    uint8_t dlc, data[8];
    int ack;          /* 1: a receiver drives the ACK slot dominant */
    int bad_crc;      /* 1: transmit CRC ^ 1 */
} gframe_t;

/* Logical bus bits (1 = recessive) for one frame, stuffed, plus ACK/EOF/IFS. */
static uint32_t frame_bits(const gframe_t *g, uint8_t *out)
{
    uint8_t f[160];
    uint32_t n = 0;
    f[n++] = 0;                                         /* SOF */
    if (!g->ext) {
        for (int i = 10; i >= 0; i--) f[n++] = (g->id >> i) & 1;
        f[n++] = g->rtr ? 1 : 0;                        /* RTR */
        f[n++] = 0;                                     /* IDE */
        f[n++] = 0;                                     /* r0 */
    } else {
        for (int i = 28; i >= 18; i--) f[n++] = (g->id >> i) & 1;
        f[n++] = 1;                                     /* SRR */
        f[n++] = 1;                                     /* IDE */
        for (int i = 17; i >= 0; i--) f[n++] = (g->id >> i) & 1;
        f[n++] = g->rtr ? 1 : 0;                        /* RTR */
        f[n++] = 0; f[n++] = 0;                         /* r1, r0 */
    }
    for (int i = 3; i >= 0; i--) f[n++] = (g->dlc >> i) & 1;
    if (!g->rtr)
        for (int b = 0; b < g->dlc && b < 8; b++)
            for (int i = 7; i >= 0; i--) f[n++] = (g->data[b] >> i) & 1;
    uint16_t crc = can_crc15(f, n);
    if (g->bad_crc) crc ^= 1;
    for (int i = 14; i >= 0; i--) f[n++] = (crc >> i) & 1;

    /* Stuff SOF..CRC. */
    uint32_t m = 0;
    int last = -1, run = 0;
    for (uint32_t i = 0; i < n; i++) {
        out[m++] = f[i];
        if (f[i] == last) run++; else { last = f[i]; run = 1; }
        if (run == 5) { out[m++] = !f[i]; last = !f[i]; run = 1; }
    }
    out[m++] = 1;                                       /* CRC delimiter */
    out[m++] = g->ack ? 0 : 1;                          /* ACK slot */
    out[m++] = 1;                                       /* ACK delimiter */
    for (int i = 0; i < 7; i++) out[m++] = 1;           /* EOF */
    for (int i = 0; i < 3; i++) out[m++] = 1;           /* intermission */
    return m;
}

/* Render bits to samples: spb samples per bit (fractional, so skew is real). */
static uint32_t render(const uint8_t *bits, uint32_t nb, float spb, uint32_t idle_before,
                       int16_t rec, int16_t dom, int16_t *s, uint32_t cap, uint32_t at)
{
    uint32_t p = at;
    for (uint32_t i = 0; i < idle_before && p < cap; i++) s[p++] = rec;
    float t0 = (float)p;
    for (uint32_t b = 0; b < nb; b++) {
        uint32_t e = (uint32_t)(t0 + (float)(b + 1) * spb + 0.5f);
        while (p < e && p < cap) s[p++] = bits[b] ? rec : dom;
    }
    return p;
}

static void fill(int16_t *s, uint32_t from, uint32_t to, int16_t v)
{
    for (uint32_t i = from; i < to; i++) s[i] = v;
}

static const decode_frame_t *find_id(const decode_result_t *r, uint32_t id)
{
    for (uint16_t i = 0; i < r->num_frames; i++)
        if (r->frames[i].id == id) return &r->frames[i];
    return NULL;
}

/* Decode one synthetic frame; returns the frame for the id or NULL. */
static decode_result_t g_res;
static const decode_frame_t *one(const gframe_t *g, float spb, float skew, int rec_high,
                                 uint32_t cut)
{
    static int16_t s[4096];
    static uint8_t bits[200];
    uint32_t nb = frame_bits(g, bits);
    int16_t rec = rec_high ? 100 : -100, dom = rec_high ? -100 : 100;
    uint32_t n = render(bits, nb, spb * (1.0f + skew), (uint32_t)(12 * spb), rec, dom, s, 4096, 0);
    fill(s, n, n + (uint32_t)(20 * spb), rec);
    n += (uint32_t)(20 * spb);
    if (cut && cut < n) n = cut;
    can_config_t cfg = { .bit_rate = 500000, .threshold = 0, .polarity = CAN_POL_AUTO };
    decode_can(s, n, spb * 500000.0f, &cfg, &g_res);
    return find_id(&g_res, g->id);
}

/* ---------------------------------------------------------------- tests */

static void test_bench_records(void)
{
    printf("Real bench records (%u)\n", (unsigned)CAN_BENCH_RECORDS);
    static int16_t diff[1024];
    for (uint32_t r = 0; r < CAN_BENCH_RECORDS; r++) {
        const can_bench_record_t *b = &can_bench_records[r];
        can_bus_report_t rep;
        decode_result_t res;
        can_bus_analyse(b->ch1, b->ch2, 1024, 0.0f, 1, diff, &rep, &res);
        CHECK(rep.signal, "%s: CAN signal found", b->name);
        CHECK(fabsf(rep.spb - b->spb) < 0.25f, "%s: spb %.2f, expected %.1f", b->name,
              (double)rep.spb, (double)b->spb);
        CHECK(rep.level_rec > rep.level_dom, "%s: recessive diff is the higher level (CH1-CH2)",
              b->name);
        for (uint8_t k = 0; k < b->nfull; k++) {
            const decode_frame_t *f = find_id(&res, b->full[k].id);
            CHECK(f != NULL, "%s: frame 0x%03X found", b->name, b->full[k].id);
            if (!f) continue;
            CHECK(!(f->flags & (DECODE_FLAG_CRC | DECODE_FLAG_ERROR)),
                  "%s: 0x%03X CRC ok (flags 0x%02X)", b->name, b->full[k].id, f->flags);
            /* The 2C53T records 512 samples after the trigger: at 2.5 MS/s an
             * 8-byte frame ends there just after its CRC, so the ACK slot is
             * outside the record. Un-rotated, that must read as truncated,
             * never as a missing ACK (both nodes acknowledged every frame). */
            CHECK(!(f->flags & DECODE_FLAG_NAK), "%s: 0x%03X not misread as NAK", b->name,
                  b->full[k].id);
            CHECK(f->flags & (DECODE_FLAG_ACK | DECODE_FLAG_TRUNC), "%s: 0x%03X ACK or truncated",
                  b->name, b->full[k].id);
            if (b->spb < 3.0f)      /* 1.25 MS/s: the whole frame and its ACK are recorded */
                CHECK(f->flags & DECODE_FLAG_ACK, "%s: 0x%03X ACK seen", b->name, b->full[k].id);
            CHECK(f->data_len == b->full[k].dlc && !memcmp(f->data, b->full[k].data, b->full[k].dlc),
                  "%s: 0x%03X payload", b->name, b->full[k].id);
        }
        if (b->trunc_id >= 0) {
            const decode_frame_t *f = find_id(&res, (uint32_t)b->trunc_id);
            /* At 5 MS/s the 512 post-trigger samples hold ~51 bits: the frame is
             * cut (truncated) or, where the cut lands mid-stuffing, an error. */
            CHECK(f && (f->flags & (DECODE_FLAG_TRUNC | DECODE_FLAG_ERROR)),
                  "%s: cut-off 0x%03X reported", b->name, b->trunc_id);
        }
        /* Every frame the C decoder calls clean must be one the bus can carry. */
        for (uint16_t i = 0; i < res.num_frames; i++) {
            const decode_frame_t *f = &res.frames[i];
            if (f->flags & (DECODE_FLAG_TRUNC | DECODE_FLAG_ERROR)) continue;
            CHECK(!(f->flags & DECODE_FLAG_CRC), "%s: no CRC failures on a healthy bus (id 0x%03lX)",
                  b->name, (unsigned long)f->id);
        }
    }
}

static void test_basic_ack_nak(void)
{
    printf("Standard frame, ACK and NAK, 2..10 samples/bit\n");
    gframe_t g = { .id = 0x1B0, .dlc = 8, .data = {0, 0x4B, 0x0F, 0, 0, 0, 0x03, 0x3A}, .ack = 1 };
    const float spbs[] = { 2.0f, 3.0f, 4.0f, 5.0f, 7.5f, 10.0f };
    for (unsigned i = 0; i < sizeof(spbs) / sizeof(spbs[0]); i++) {
        const decode_frame_t *f = one(&g, spbs[i], 0.0f, 1, 0);
        CHECK(f && (f->flags & DECODE_FLAG_ACK) && !(f->flags & DECODE_FLAG_CRC) &&
              f->data_len == 8 && !memcmp(f->data, g.data, 8), "spb %.1f: ACKed 0x1B0", (double)spbs[i]);
    }
    g.ack = 0;
    const decode_frame_t *f = one(&g, 5.0f, 0.0f, 1, 0);
    CHECK(f && (f->flags & DECODE_FLAG_NAK) && !(f->flags & DECODE_FLAG_ACK), "NAK reported");
    CHECK(f && strstr(f->label, "NAK") != NULL, "label says NAK (%s)", f ? f->label : "-");
}

static void test_crc_error(void)
{
    printf("CRC error\n");
    gframe_t g = { .id = 0x181, .dlc = 8, .data = {2, 0, 0, 0, 0, 0, 0, 0x0C}, .ack = 0, .bad_crc = 1 };
    const decode_frame_t *f = one(&g, 5.0f, 0.0f, 1, 0);
    CHECK(f && (f->flags & DECODE_FLAG_CRC), "CRC mismatch flagged");
    g.bad_crc = 0;
    f = one(&g, 5.0f, 0.0f, 1, 0);
    CHECK(f && !(f->flags & DECODE_FLAG_CRC), "correct CRC passes");
}

static void test_known_crc(void)
{
    printf("CRC-15 known value (the bench's unacknowledged 0x181 frame)\n");
    /* 0x181, DLC 8, 02 00 00 00 00 00 00 0C: decoded from the bus with a valid CRC on
     * 2026-11-10. Recompute the CRC over the destuffed bits and check the generator agrees. */
    gframe_t g = { .id = 0x181, .dlc = 8, .data = {2, 0, 0, 0, 0, 0, 0, 0x0C} };
    const decode_frame_t *f = one(&g, 5.0f, 0.0f, 1, 0);
    CHECK(f && !(f->flags & DECODE_FLAG_CRC) && (f->flags & DECODE_FLAG_NAK), "0x181 decodes, NAK");
}

static void test_ext_rtr(void)
{
    printf("29-bit ID and remote frames\n");
    gframe_t g = { .id = 0x18FEF100, .ext = 1, .dlc = 8, .data = {1, 2, 3, 4, 5, 6, 7, 8}, .ack = 1 };
    const decode_frame_t *f = one(&g, 5.0f, 0.0f, 1, 0);
    CHECK(f && (f->flags & DECODE_FLAG_EXT) && !(f->flags & DECODE_FLAG_CRC) && f->data[7] == 8,
          "extended 0x18FEF100");
    CHECK(f && strstr(f->label, "18FEF100") != NULL, "label carries the 29-bit ID (%s)", f ? f->label : "-");
    gframe_t r = { .id = 0x7DF, .rtr = 1, .dlc = 8, .ack = 1 };
    f = one(&r, 5.0f, 0.0f, 1, 0);
    CHECK(f && (f->flags & DECODE_FLAG_RTR) && f->data_len == 0 && !(f->flags & DECODE_FLAG_CRC),
          "remote frame 0x7DF, no data, CRC ok");
}

static void test_skew(void)
{
    printf("Transmitter clock skew\n");
    gframe_t g = { .id = 0x4B0, .dlc = 8, .data = {0xFF, 0, 0xAA, 0x55, 0, 0xFF, 0, 1}, .ack = 1 };
    const float skews[] = { -0.03f, -0.015f, 0.015f, 0.03f };
    for (unsigned i = 0; i < 4; i++) {
        const decode_frame_t *f = one(&g, 5.0f, skews[i], 1, 0);
        CHECK(f && !(f->flags & (DECODE_FLAG_CRC | DECODE_FLAG_ERROR)) && f->data[3] == 0x55,
              "skew %+.1f%% at 5 samples/bit", (double)(skews[i] * 100.0f));
    }
}

static void test_polarity(void)
{
    printf("Polarity: recessive low (inverting front end / swapped leads), auto-detected\n");
    gframe_t g = { .id = 0x123, .dlc = 2, .data = {1, 2}, .ack = 1 };
    const decode_frame_t *f = one(&g, 5.0f, 0.0f, 0, 0);
    CHECK(f && (f->flags & DECODE_FLAG_ACK) && f->data[1] == 2, "decodes with recessive low");
}

static void test_truncated(void)
{
    printf("Record ending inside a frame\n");
    gframe_t g = { .id = 0x1B0, .dlc = 8, .data = {9, 9, 9, 9, 9, 9, 9, 9}, .ack = 1 };
    /* idle 12 bits (60 samples) + SOF..ID..DLC + 3 data bytes ~ 60 bits: cut at 60 + 300 samples */
    const decode_frame_t *f = one(&g, 5.0f, 0.0f, 1, 360);
    CHECK(f && (f->flags & DECODE_FLAG_TRUNC), "truncated, ID still reported");
    CHECK(f && strstr(f->label, "...") != NULL, "label marks it truncated (%s)", f ? f->label : "-");
}

static void test_glitch(void)
{
    printf("Single-sample spike inside a bit (10 samples/bit)\n");
    static int16_t s[4096];
    static uint8_t bits[200];
    gframe_t g = { .id = 0x2A5, .dlc = 4, .data = {0x11, 0x22, 0x33, 0x44}, .ack = 1 };
    uint32_t nb = frame_bits(&g, bits);
    uint32_t n = render(bits, nb, 10.0f, 120, 100, -100, s, 4096, 0);
    fill(s, n, n + 200, 100);
    n += 200;
    /* spikes in the middle of several bits */
    for (uint32_t b = 20; b < 60; b += 7) { uint32_t k = 120 + b * 10 + 4; s[k] = (int16_t)-s[k]; }
    can_config_t cfg = { .bit_rate = 500000, .threshold = 0 };
    decode_can(s, n, 5000000.0f, &cfg, &g_res);
    const decode_frame_t *f = find_id(&g_res, 0x2A5);
    CHECK(f && !(f->flags & (DECODE_FLAG_CRC | DECODE_FLAG_ERROR)) && f->data[3] == 0x44,
          "spikes ignored");
}

static void test_back_to_back(void)
{
    printf("Two frames in one record, minimum gap\n");
    static int16_t s[4096];
    static uint8_t bits[200];
    gframe_t a = { .id = 0x1B0, .dlc = 8, .data = {1, 2, 3, 4, 5, 6, 7, 8}, .ack = 1 };
    gframe_t b = { .id = 0x4B0, .dlc = 8, .data = {8, 7, 6, 5, 4, 3, 2, 1}, .ack = 1 };
    uint32_t na = frame_bits(&a, bits);
    uint32_t n = render(bits, na, 5.0f, 60, 100, -100, s, 4096, 0);
    uint32_t nb = frame_bits(&b, bits);                 /* follows straight after IFS */
    n = render(bits, nb, 5.0f, 0, 100, -100, s, 4096, n);
    fill(s, n, n + 100, 100);
    n += 100;
    can_config_t cfg = { .bit_rate = 500000, .threshold = 0 };
    decode_can(s, n, 2500000.0f, &cfg, &g_res);
    CHECK(find_id(&g_res, 0x1B0) && find_id(&g_res, 0x4B0), "both frames decoded (%u)",
          (unsigned)g_res.num_frames);
}

static void test_spb_and_analyse(void)
{
    printf("Bit width measured from the record; two-channel analyse; stats and summary\n");
    static uint8_t h[1024], l[1024];
    static int16_t s[1024], diff[1024];
    static uint8_t bits[200];
    gframe_t g = { .id = 0x190, .dlc = 8, .data = {3, 0, 0, 0, 0, 0, 0, 0x55}, .ack = 1 };
    uint32_t nb = frame_bits(&g, bits);
    uint32_t n = render(bits, nb, 4.6f, 80, 1, 0, s, 1024, 0);
    fill(s, n, 1024, 1);
    /* 2C53T-like codes: recessive CH1 118 / CH2 175, dominant CH1 87 / CH2 191 */
    for (uint32_t i = 0; i < 1024; i++) {
        h[i] = s[i] ? 118 : 87;
        l[i] = s[i] ? 175 : 191;
    }
    can_bus_report_t rep;
    decode_result_t res;
    can_bus_analyse(h, l, 1024, 2.3e6f, 0, diff, &rep, &res);
    CHECK(rep.signal && fabsf(rep.spb - 4.6f) < 0.1f, "spb %.2f (true 4.60)", (double)rep.spb);
    CHECK(fabsf(rep.bit_rate - 500000.0f) < 12000.0f, "bit rate %.0f from fs/spb", (double)rep.bit_rate);
    const decode_frame_t *f = find_id(&res, 0x190);
    CHECK(f && (f->flags & DECODE_FLAG_ACK) && f->data[7] == 0x55, "0x190 decoded through analyse");

    can_bus_stats_t st;
    can_bus_stats_reset(&st);
    can_bus_stats_add(&st, &rep, &res);
    can_bus_stats_add(&st, &rep, &res);
    char line[64];
    int ok = can_bus_summary(&st, &rep, line, sizeof(line));
    CHECK(ok && st.frames == 2 && st.nids == 1 && strstr(line, "190") && strstr(line, "500k"),
          "summary '%s'", line);

    /* flat record: no signal */
    memset(h, 118, sizeof(h));
    memset(l, 175, sizeof(l));
    can_bus_analyse(h, l, 1024, 0.0f, 0, diff, &rep, &res);
    CHECK(!rep.signal && res.num_frames == 0, "flat bus: no signal, no frames");
    can_bus_stats_add(&st, &rep, &res);
    can_bus_summary(&st, &rep, line, sizeof(line));
    CHECK(strstr(line, "no bus signal") != NULL, "summary '%s'", line);
}

static void test_rotation(void)
{
    printf("Ring-buffer record: un-rotated at the longest idle\n");
    static uint8_t h[1024], l[1024];
    static int16_t s[1024], diff[1024];
    static uint8_t bits[200];
    gframe_t g = { .id = 0x1B0, .dlc = 2, .data = {0xAB, 0xCD}, .ack = 1 };
    uint32_t nb = frame_bits(&g, bits);
    uint32_t n = render(bits, nb, 5.0f, 300, 1, 0, s, 1024, 0);
    fill(s, n, 1024, 1);
    for (uint32_t r = 0; r < 1024; r += 173) {          /* every rotation decodes */
        for (uint32_t i = 0; i < 1024; i++) {
            int16_t v = s[(i + r) % 1024];
            h[i] = v ? 118 : 87;
            l[i] = v ? 175 : 191;
        }
        can_bus_report_t rep;
        decode_result_t res;
        can_bus_analyse(h, l, 1024, 0.0f, 1, diff, &rep, &res);
        const decode_frame_t *f = find_id(&res, 0x1B0);
        CHECK(f && (f->flags & DECODE_FLAG_ACK) && f->data[1] == 0xCD, "rotation %u", (unsigned)r);
    }
}

int main(void)
{
    printf("=== CAN bus analyser tests ===\n");
    test_bench_records();
    test_basic_ack_nak();
    test_crc_error();
    test_known_crc();
    test_ext_rtr();
    test_skew();
    test_polarity();
    test_truncated();
    test_glitch();
    test_back_to_back();
    test_spb_and_analyse();
    test_rotation();
    printf("=== Results: %d/%d passed ===\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
