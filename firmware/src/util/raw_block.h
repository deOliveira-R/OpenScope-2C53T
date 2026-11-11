/*
 * raw_block — receive exactly N raw bytes from the shell transport into a
 * caller buffer, check their CRC-32, with an idle timeout (pure logic).
 *
 * Used by `logo blk <off> <len> <crc>` (usb_debug.c): the command line arms
 * a block, the shell then routes the next `len` bytes here instead of into
 * the line editor (no echo, no parsing), and the command completes when the
 * block is in and verified. A host that stalls mid-block costs one
 * RAW_BLOCK_IDLE_MS: the block is abandoned, the shell listens again, and
 * the host resynchronises with `logo pos`. Bytes a confused host keeps
 * sending after an abandoned block land in the shell as an unknown command,
 * which is harmless.
 */
#ifndef RAW_BLOCK_H
#define RAW_BLOCK_H

#include <stdbool.h>
#include <stdint.h>

#define RAW_BLOCK_IDLE_MS 2000u

typedef enum {
    RAW_IDLE = 0,       /* not receiving                         */
    RAW_RECEIVING,      /* waiting for more bytes                */
    RAW_DONE_OK,        /* all bytes in, CRC matches             */
    RAW_DONE_CRC,       /* all bytes in, CRC differs             */
    RAW_TIMEOUT,        /* the host went quiet mid-block         */
} raw_block_state_t;

typedef struct {
    uint8_t *buf;
    uint32_t cap;
    uint32_t want;
    uint32_t got;
    uint32_t crc;           /* declared CRC-32 (zlib)            */
    uint32_t tag;           /* caller's: e.g. the target offset  */
    uint32_t last_ms;
    raw_block_state_t state;
} raw_block_t;

/* Arm a block. False (and stays idle) if len is 0 or exceeds cap. */
bool raw_block_begin(raw_block_t *b, uint8_t *buf, uint32_t cap, uint32_t len,
                     uint32_t crc, uint32_t tag, uint32_t now_ms);

/* Feed transport bytes. Returns how many were consumed (the rest, if any,
 * belong to the shell). The state turns DONE_* when the last byte lands. */
uint32_t raw_block_feed(raw_block_t *b, const uint8_t *data, uint32_t n, uint32_t now_ms);

/* Age the idle timeout; RAW_TIMEOUT once the host has been quiet too long. */
void raw_block_poll(raw_block_t *b, uint32_t now_ms);

static inline bool raw_block_active(const raw_block_t *b) { return b->state == RAW_RECEIVING; }

/* Back to idle after the caller consumed a DONE_* / TIMEOUT state. */
static inline void raw_block_reset(raw_block_t *b) { b->state = RAW_IDLE; }

#endif /* RAW_BLOCK_H */
