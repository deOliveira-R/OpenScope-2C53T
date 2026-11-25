/*
 * OpenScope 2C53T - the session snapshot: membership and codec (model_session.h)
 */
#include "model_session.h"

#include <string.h>

/* ═══ what is NOT in the snapshot, and why ═══════════════════════════ */

const model_session_exclusion_t model_session_exclusions[] = {
    /* Calibration is never a session's: its own record, written by `cal user save` only. */
    { PROP_ID_IL_GAIN,        "user calibration (usercal record)" },
    { PROP_ID_IL_OFFSET,      "user calibration (usercal record)" },
    { PROP_ID_IL_SKEW,        "user calibration (usercal record)" },
    { PROP_ID_IL_KNOTS,       "user calibration: its only set drops the level map" },
    /* Another name for state the snapshot already holds, or does not want. */
    { PROP_ID_SYS_MODE,       "alias of sys.instrument" },
    /* The screen: the settings record's last-screen byte brings it back, before the scheduler. */
    { PROP_ID_SYS_INSTRUMENT, "the screen binding: the record's last screen restores it" },
    /* A restore never turns a signal on unasked: the output keeps its own boot policy. */
    { PROP_ID_GEN_OUT,        "boot policy gen.bootout (settings record)" },
    /* The scope always comes back running. */
    { PROP_ID_VIEW_RUN,       "the scope always comes back running" },
    /* Momentary state: it belongs to the reading that was on screen. */
    { PROP_ID_METER_HOLD,     "momentary: a held reading" },
    { PROP_ID_METER_REL,      "momentary: a relative reference" },
};
const uint8_t model_session_exclusion_count =
    (uint8_t)(sizeof model_session_exclusions / sizeof model_session_exclusions[0]);

const char *model_session_excluded(const prop_desc_t *p)
{
    if (!p) return "no such property";
    if (!prop_is_settable(p)) return "read only";
    for (uint8_t i = 0; i < model_session_exclusion_count; i++)
        if (model_session_exclusions[i].id == p->id) return model_session_exclusions[i].why;
    return NULL;
}

uint16_t model_session_member_count(void)
{
    uint16_t n = 0;
    for (uint16_t i = 0; i < prop_count(); i++)
        if (!model_session_excluded(prop_at(i))) n++;
    return n;
}

/* ═══ bytes ══════════════════════════════════════════════════════════ */

uint32_t model_session_crc32(const void *data, uint32_t len)
{
    const uint8_t *d = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return ~crc;
}

static void wr16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *b, uint32_t v) { wr16(b, (uint16_t)v); wr16(b + 2, (uint16_t)(v >> 16)); }
static uint16_t rd16(const uint8_t *b) { return (uint16_t)(b[0] | ((uint16_t)b[1] << 8)); }
static uint32_t rd32(const uint8_t *b) { return (uint32_t)rd16(b) | ((uint32_t)rd16(b + 2) << 16); }

/* Zigzag LEB128: small magnitudes of either sign are short. <= 5 bytes. */
static uint16_t put_varint(uint8_t *b, int32_t v)
{
    uint32_t z = ((uint32_t)v << 1) ^ (uint32_t)(v >> 31);
    uint16_t n = 0;
    do {
        uint8_t c = (uint8_t)(z & 0x7Fu);
        z >>= 7;
        b[n++] = (uint8_t)(c | (z ? 0x80u : 0u));
    } while (z);
    return n;
}

/* 0: truncated or longer than 5 bytes. */
static uint16_t get_varint(const uint8_t *b, uint16_t avail, int32_t *v)
{
    uint32_t z = 0;
    for (uint16_t n = 0; n < avail && n < 5u; n++) {
        z |= (uint32_t)(b[n] & 0x7Fu) << (7u * n);
        if (!(b[n] & 0x80u)) {
            *v = (int32_t)((z >> 1) ^ (uint32_t)-(int32_t)(z & 1u));
            return (uint16_t)(n + 1u);
        }
    }
    return 0;
}

static void header(uint8_t *buf, uint8_t flags, uint16_t seq, uint16_t count, uint32_t stamp)
{
    wr16(buf, MODEL_SESSION_MAGIC);
    buf[2] = MODEL_SESSION_VERSION;
    buf[3] = flags;
    wr16(buf + 4, seq);
    wr16(buf + 6, count);
    wr32(buf + 8, stamp);
}

uint16_t model_session_encode(uint8_t *buf, uint16_t cap, uint16_t seq, uint32_t stamp)
{
    if (!buf || cap < MODEL_SESSION_HDR_BYTES + 4u) return 0;
    uint16_t n = MODEL_SESSION_HDR_BYTES, count = 0;
    for (uint16_t i = 0; i < prop_count(); i++) {
        const prop_desc_t *p = prop_at(i);
        if (model_session_excluded(p)) continue;
        if ((uint32_t)n + 2u + 5u + 4u > cap) return 0;      /* the widest entry and the CRC */
        int32_t v;
        if (!model_boot_kept(p, &v)) v = prop_get(p);
        wr16(buf + n, p->id);
        n = (uint16_t)(n + 2u);
        n = (uint16_t)(n + put_varint(buf + n, v));
        count++;
    }
    header(buf, 0u, seq, count, stamp);
    wr32(buf + n, model_session_crc32(buf, n));
    return (uint16_t)(n + 4u);
}

uint16_t model_session_encode_cleared(uint8_t *buf, uint16_t cap, uint16_t seq)
{
    if (!buf || cap < MODEL_SESSION_HDR_BYTES + 4u) return 0;
    header(buf, MODEL_SESSION_F_CLEARED, seq, 0u, 0u);
    wr32(buf + MODEL_SESSION_HDR_BYTES, model_session_crc32(buf, MODEL_SESSION_HDR_BYTES));
    return (uint16_t)(MODEL_SESSION_HDR_BYTES + 4u);
}

/* One walk for both: keep = false only checks that every entry is whole. */
static model_session_parse_t walk(const uint8_t *buf, uint16_t len, model_session_info_t *info, bool keep)
{
    model_session_info_t scratch;
    if (!info) info = &scratch;
    memset(info, 0, sizeof *info);
    if (!buf || len < MODEL_SESSION_HDR_BYTES + 4u || len > MODEL_SESSION_MAX_BYTES) return MODEL_SESSION_BAD;
    if (rd16(buf) != MODEL_SESSION_MAGIC) return MODEL_SESSION_BAD;
    const uint16_t body = (uint16_t)(len - 4u);
    if (rd32(buf + body) != model_session_crc32(buf, body)) return MODEL_SESSION_BAD;
    info->version = buf[2];
    info->flags   = buf[3];
    info->seq     = rd16(buf + 4);
    info->count   = rd16(buf + 6);
    info->stamp   = rd32(buf + 8);
    if (info->version != MODEL_SESSION_VERSION) return MODEL_SESSION_VERSION_;
    if (info->flags & MODEL_SESSION_F_CLEARED) return MODEL_SESSION_CLEARED;

    uint16_t n = MODEL_SESSION_HDR_BYTES;
    for (uint16_t e = 0; e < info->count; e++) {
        int32_t v;
        if ((uint32_t)n + 3u > body) return MODEL_SESSION_BAD;
        const uint16_t id = rd16(buf + n);
        const uint16_t used = get_varint(buf + n + 2u, (uint16_t)(body - n - 2u), &v);
        if (!used) return MODEL_SESSION_BAD;
        n = (uint16_t)(n + 2u + used);
        if (!keep) continue;
        const prop_desc_t *p = prop_by_id(id);
        if (!p)                                { if (info->unknown < 0xFFu) info->unknown++; continue; }
        if (model_session_excluded(p))         { if (info->excluded < 0xFFu) info->excluded++; continue; }
        if (!prop_value_in_range(p, v))        { if (info->out_of_range < 0xFFu) info->out_of_range++; continue; }
        model_boot_keep(p, v, false);
        if (info->kept < 0xFFu) info->kept++;
    }
    return (n == body) ? MODEL_SESSION_OK : MODEL_SESSION_BAD;
}

model_session_parse_t model_session_parse(const uint8_t *buf, uint16_t len, model_session_info_t *info)
{
    return walk(buf, len, info, false);
}

model_session_parse_t model_session_keep(const uint8_t *buf, uint16_t len, model_session_info_t *info)
{
    /* Whole first: a snapshot that turns out to be damaged keeps nothing. */
    const model_session_parse_t r = walk(buf, len, info, false);
    return r == MODEL_SESSION_OK ? walk(buf, len, info, true) : r;
}
