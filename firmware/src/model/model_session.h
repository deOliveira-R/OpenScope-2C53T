/*
 * OpenScope 2C53T - the session snapshot (codec and membership)
 *
 * "Startup on Boot = Restore" brings each instrument back in the state it was
 * left in. The settings record (60 bytes, v5, model_persist_v5.c) holds the
 * USER SETTINGS (PF_PERSIST) and is what every Startup choice loads. The
 * session snapshot holds, next to it and in a flash area of its own
 * (util/session_store.h), the value of EVERY settable property an orderly
 * power-off left: the PF_PERSIST ones again and the PF_SESSION ones, which
 * nothing else saves. Restore alone reads it.
 *
 * MEMBERSHIP. A settable property is in the snapshot unless
 * model_session_excluded() names a reason; the list is data
 * (model_session_exclusions) and tests/test_session.c holds "every settable
 * property is in the snapshot or in the list, never both". A new property
 * is therefore in the snapshot unless someone writes down why not.
 *
 * FORMAT (version 1), little-endian, at most MODEL_SESSION_MAX_BYTES:
 *   +0   u16  magic 0x4E53 ("SN")
 *   +2   u8   format version (1)
 *   +3   u8   flags: bit 0 = CLEARED (a tombstone: "no snapshot", count 0)
 *   +4   u16  sequence number (the save before it + 1; wraps)
 *   +6   u16  count of entries
 *   +8   u32  settings stamp: CRC-32 of the 60-byte settings record image
 *             this snapshot was saved with (session_store.h, STALE)
 *   +12  count x { u16 property id ; value as a zigzag LEB128 varint }
 *   end  u32  CRC-32 (the zlib polynomial) of every byte before it
 * A value is 1 byte for -64..63, 2 for -8192..8191, 3 below 2^20, 5 at most:
 * about 3.2 bytes an entry with the id, ~210 bytes for today's table.
 *
 * ACROSS FIRMWARE UPDATES. Values are keyed by the property id (the table's
 * 16-bit ids are stable and never reused), not by position:
 *   - an id this build does not know is skipped (a property that left);
 *   - an id this build excludes or cannot set is skipped;
 *   - a value outside the property's range is skipped (never clamped: the
 *     property stays where the settings record or its default put it);
 *   - a property the snapshot does not name stays where it is (a new one);
 *   - a different format version, a bad magic, length or checksum: the whole
 *     snapshot is not used.
 * So a table that gained or lost a property loads what both tables share.
 *
 * No hardware and no flash here: this file is the model's (host-tested with
 * the fake ops). A decode only KEEPS values (model_boot_keep); the input
 * task sets them through the setters (model.h, model_boot_apply).
 */
#ifndef MODEL_SESSION_H
#define MODEL_SESSION_H

#include "model.h"

#define MODEL_SESSION_MAGIC      0x4E53u
#define MODEL_SESSION_VERSION    1u
#define MODEL_SESSION_F_CLEARED  0x01u
#define MODEL_SESSION_HDR_BYTES  12u
/* The buffer a snapshot is built in and read into. An encode that does not
 * fit fails (nothing is written); tests/test_session.c holds that today's
 * table fits with every value at its widest. */
#define MODEL_SESSION_MAX_BYTES  512u

/* Why a settable property is NOT in the snapshot. */
typedef struct { uint16_t id; const char *why; } model_session_exclusion_t;
extern const model_session_exclusion_t model_session_exclusions[];
extern const uint8_t model_session_exclusion_count;

/* NULL: p is in the snapshot. Else the reason it is not: its entry in the
 * list, or "read only" for a property that cannot be set at all. */
const char *model_session_excluded(const prop_desc_t *p);
uint16_t    model_session_member_count(void);

uint32_t model_session_crc32(const void *data, uint32_t len);

/* The live state as a snapshot: every member's value, in table order. A
 * value a load kept that is not in force yet (model_boot_kept: a view the
 * boot could not show) is written instead of the live one, as the settings
 * record does. Returns the length, or 0 when it does not fit in cap. */
uint16_t model_session_encode(uint8_t *buf, uint16_t cap, uint16_t seq, uint32_t stamp);
/* A tombstone: "there is no snapshot". Returns its length (16), 0 if cap is short. */
uint16_t model_session_encode_cleared(uint8_t *buf, uint16_t cap, uint16_t seq);

typedef enum {
    MODEL_SESSION_OK = 0,      /* a snapshot                                    */
    MODEL_SESSION_CLEARED,     /* a tombstone                                   */
    MODEL_SESSION_BAD,         /* magic, length, checksum or a truncated entry  */
    MODEL_SESSION_VERSION_,    /* a format this build does not read             */
} model_session_parse_t;

typedef struct {
    uint16_t seq;
    uint16_t count;            /* entries in the snapshot                        */
    uint32_t stamp;
    uint8_t  version, flags;
    /* model_session_keep only: */
    uint8_t  kept;             /* handed to model_boot_keep                      */
    uint8_t  unknown;          /* skipped: no property has that id               */
    uint8_t  excluded;         /* skipped: this build does not restore it        */
    uint8_t  out_of_range;     /* skipped: not a value the property can hold     */
} model_session_info_t;

/* Check the bytes and read the header; nothing is kept. */
model_session_parse_t model_session_parse(const uint8_t *buf, uint16_t len, model_session_info_t *info);
/* The same, and when it is MODEL_SESSION_OK every usable entry is kept for
 * the boot apply (model_boot_keep, not sticky). */
model_session_parse_t model_session_keep(const uint8_t *buf, uint16_t len, model_session_info_t *info);

#endif /* MODEL_SESSION_H */
