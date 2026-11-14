/*
 * OpenScope 2C53T - stream frame encoder (docs/stream_protocol.md)
 *
 * Binary frames that share the CDC channel with the text shell. Pure C: no
 * driver, no RTOS, no buffer beyond one 64-byte USB packet, so the firmware
 * sends a 2 KB record straight from the acquisition buffers and the host
 * test (tests/test_stream.c) runs the same code.
 *
 *   off  size  field
 *   0    1     0xFB            marker byte 0 (never valid in UTF-8 text)
 *   1    1     0x53 ('S')      marker byte 1
 *   2    1     type            SF_T_*; a host skips a type it does not know
 *   3    1     flags           0 for now
 *   4    2     seq             one counter over every frame begun (u16, wraps)
 *   6    2     len             payload length, <= SF_MAX_PAYLOAD
 *   8    2     hcrc            CRC-16/CCITT-FALSE of bytes 0..7
 *   10   len   payload         little-endian fields, IEEE-754 floats
 *   10+len 4   crc             CRC-32 (IEEE, zlib's) of bytes 2..9+len
 *   [1   0x00  pad]            only when the frame would end on a 64-byte
 *                              packet boundary; hosts drop NUL outside frames
 *
 * Payloads only ever grow at the end: a host reads the fields it knows and
 * ignores the rest (it never needs `len` to match its own idea of the size).
 */
#ifndef STREAM_FRAME_H
#define STREAM_FRAME_H

#include <stdbool.h>
#include <stdint.h>

#define SF_SOF0            0xFBu
#define SF_SOF1            0x53u
#define SF_HDR_LEN         10u
#define SF_CRC_LEN         4u
#define SF_OVERHEAD        (SF_HDR_LEN + SF_CRC_LEN)
#define SF_MAX_PAYLOAD     4096u
#define SF_PACKET          64u          /* USB full-speed bulk packet */

#define SF_PROTO_MAJOR     1u
#define SF_PROTO_MINOR     0u

/* Frame types. Codes are never reused. */
enum {
    SF_T_HELLO    = 0x01,
    SF_T_PROPS    = 0x02,
    SF_T_RECORD   = 0x03,
    SF_T_METER    = 0x04,
    SF_T_STATS    = 0x05,
    SF_T_PROPDESC = 0x06,
    /* reserved for later (docs/stream_protocol.md section 6) */
    SF_T_MEAS     = 0x07,
    SF_T_GEN      = 0x08,
    SF_T_CANFRAME = 0x09,
    SF_T_ROLL     = 0x0A,
    SF_T_COUNT    = 0x0B            /* one past the highest code in use */
};

/* HELLO capability / stream bits (what the firmware can send; what is on). */
enum {
    SF_CAP_SCOPE = 1u << 0,
    SF_CAP_METER = 1u << 1,
    SF_CAP_PROPS = 1u << 2,
};

/* RECORD flags. */
enum {
    SF_REC_INTERLEAVED  = 1u << 0,  /* CH1 is the 2-converter merge (2x rate)    */
    SF_REC_TIME_ORDERED = 1u << 1,  /* un-rotated: hardware trigger at trig_idx  */
    SF_REC_ROLL         = 1u << 2,  /* the scope is in roll (no records expected) */
    SF_REC_COHERENT     = 1u << 3,  /* CH1 and CH2 are one moment               */
    SF_REC_RUNNING      = 1u << 4,  /* acquisition running (not STOP/held)      */
    SF_REC_TRUE_SCALE   = 1u << 5,  /* the screen draws at true volts/div       */
};

/* METER flags. */
enum {
    SF_MTR_VALID     = 1u << 0,
    SF_MTR_NEGATIVE  = 1u << 1,
    SF_MTR_AC        = 1u << 2,
    SF_MTR_AUTORANGE = 1u << 3,
    SF_MTR_HOLD      = 1u << 4,
    SF_MTR_BEEP      = 1u << 5,
};

#define SF_TRIG_NONE       0xFFFFu
#define SF_REC_FIXED_LEN   24u     /* RECORD bytes before the channel table */
#define SF_REC_CH_LEN      16u     /* one channel table entry               */
#define SF_REC_TAIL_LEN    4u      /* gen_end                               */
#define SF_PROPS_PAIR_LEN  6u

/* ── checksums ──────────────────────────────────────────────────────── */
uint16_t sf_crc16(uint16_t crc, const uint8_t *p, uint32_t n);   /* seed 0xFFFF   */
uint32_t sf_crc32_update(uint32_t crc, const uint8_t *p, uint32_t n);   /* seed 0xFFFFFFFF, final ~ */

/* ── writer ─────────────────────────────────────────────────────────── */
/* The sink takes one packet (<= 64 bytes); false = it could not (the frame
 * is abandoned: later puts are swallowed and sf_end returns false). */
typedef bool (*sf_sink_fn)(const uint8_t *data, uint16_t len, void *ctx);

typedef struct {
    sf_sink_fn sink;
    void      *ctx;
    uint32_t   crc;
    uint32_t   sent;                /* bytes handed to the sink */
    uint16_t   left;                /* payload bytes still owed */
    uint16_t   len;
    uint8_t    n;                   /* bytes in pkt */
    bool       ok;
    uint8_t    pkt[SF_PACKET];
} sf_writer_t;

/* Total bytes on the wire for a payload of len (header, CRC, pad). */
uint32_t sf_wire_len(uint16_t len);

void sf_begin(sf_writer_t *w, sf_sink_fn sink, void *ctx, uint8_t type, uint8_t flags,
              uint16_t seq, uint16_t len);
void sf_put(sf_writer_t *w, const void *p, uint16_t n);
void sf_put_vol(sf_writer_t *w, const volatile uint8_t *p, uint16_t n);   /* live buffers */
void sf_u8(sf_writer_t *w, uint8_t v);
void sf_u16(sf_writer_t *w, uint16_t v);
void sf_u32(sf_writer_t *w, uint32_t v);
void sf_i32(sf_writer_t *w, int32_t v);
void sf_f32(sf_writer_t *w, float v);
/* Short string: u8 length (truncated to max) then the bytes. */
void sf_str(sf_writer_t *w, const char *s, uint8_t max);
uint16_t sf_str_len(const char *s, uint8_t max);    /* its payload bytes (1 + n) */
/* CRC, pad, last packet. False if the sink refused or the payload was short
 * (a short payload is zero-filled so the framing stays intact). */
bool sf_end(sf_writer_t *w);

/* ── payload descriptions (the encoders the firmware and the test share) ── */
typedef struct {
    uint8_t     caps, on;           /* SF_CAP_* */
    uint16_t    max_payload;
    uint16_t    rec_samples;
    uint16_t    n_props;
    uint32_t    table_crc;          /* sf_table_crc over the property table */
    const char *fw;                 /* firmware build text */
} sf_hello_t;

typedef struct {
    uint8_t  ch;                    /* 0 = CH1, 1 = CH2 */
    uint8_t  vdiv_idx, zero_code, coupling;
    int16_t  position;
    uint16_t probe_x;               /* 1 or 10 */
    float    volts_per_count;       /* at the probe tip; 0 = uncalibrated (codes) */
    float    volts_per_div;         /* at the probe tip */
    const volatile uint8_t *codes;  /* n samples */
} sf_rec_ch_t;

typedef struct {
    uint32_t gen, t_ms;
    float    fs_hz, s_per_div;
    uint16_t n, trig_idx;
    uint8_t  flags, kind, tb_code, nch;
    sf_rec_ch_t ch[2];
} sf_record_t;

typedef struct {
    uint32_t    update_count, t_ms;
    float       value;
    int32_t     bcd;
    uint8_t     func, auto_fn, result_class, status_code, decimal_pos, range_index, flags;
    const char *unit, *display, *range_label;
} sf_meter_t;

typedef struct {
    uint32_t t_ms, bytes, rec_missed, rec_torn, aborts;
    uint32_t sent[SF_T_COUNT], dropped[SF_T_COUNT];
} sf_stats_t;

typedef struct {
    uint16_t    id;
    uint8_t     type, unit, decimals, group;
    uint16_t    flags;
    int32_t     min, max, def;
    const char *name, *unit_sym;
} sf_propdesc_t;

typedef struct { uint16_t id; int32_t value; } sf_prop_t;

uint16_t sf_hello_len(const sf_hello_t *h);
uint16_t sf_record_len(const sf_record_t *r);
uint16_t sf_meter_len(const sf_meter_t *m);
uint16_t sf_stats_len(void);
uint16_t sf_propdesc_len(const sf_propdesc_t *d);
uint16_t sf_props_len(uint8_t n_groups, uint16_t n_props);

/* Each writes one whole frame through the sink; false = not (fully) sent.
 * The record trailer (gen_end) is read through gen_end_fn after the samples,
 * so a commit that landed during the send shows as gen_end != gen. */
bool sf_send_hello(sf_writer_t *w, sf_sink_fn s, void *ctx, uint16_t seq, const sf_hello_t *h);
bool sf_send_record(sf_writer_t *w, sf_sink_fn s, void *ctx, uint16_t seq, const sf_record_t *r,
                    uint32_t (*gen_end_fn)(void));
bool sf_send_meter(sf_writer_t *w, sf_sink_fn s, void *ctx, uint16_t seq, const sf_meter_t *m);
bool sf_send_stats(sf_writer_t *w, sf_sink_fn s, void *ctx, uint16_t seq, const sf_stats_t *st);
bool sf_send_propdesc(sf_writer_t *w, sf_sink_fn s, void *ctx, uint16_t seq, const sf_propdesc_t *d);
/* PROPS: the caller supplies the pairs through get(i) so the firmware reads
 * the model live, without a copy. */
bool sf_send_props(sf_writer_t *w, sf_sink_fn s, void *ctx, uint16_t seq, uint32_t table_crc,
                   uint8_t n_groups, const uint32_t *epochs, uint16_t n_props,
                   sf_prop_t (*get)(uint16_t i, void *gctx), void *gctx);

/* Property table identity: CRC-32 over (id u16, type, unit, decimals, group). */
uint32_t sf_table_crc_add(uint32_t crc, uint16_t id, uint8_t type, uint8_t unit, uint8_t decimals,
                          uint8_t group);

#endif /* STREAM_FRAME_H */
