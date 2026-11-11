/*
 * raw_block.c — see raw_block.h.
 */
#include "raw_block.h"
#include "cal_backup_core.h"     /* CRC-32 (zlib) */

#include <string.h>

bool raw_block_begin(raw_block_t *b, uint8_t *buf, uint32_t cap, uint32_t len,
                     uint32_t crc, uint32_t tag, uint32_t now_ms)
{
    b->state = RAW_IDLE;
    if (buf == NULL || len == 0u || len > cap) return false;
    b->buf = buf;
    b->cap = cap;
    b->want = len;
    b->got = 0;
    b->crc = crc;
    b->tag = tag;
    b->last_ms = now_ms;
    b->state = RAW_RECEIVING;
    return true;
}

uint32_t raw_block_feed(raw_block_t *b, const uint8_t *data, uint32_t n, uint32_t now_ms)
{
    if (b->state != RAW_RECEIVING || n == 0u) return 0;
    uint32_t take = b->want - b->got;
    if (take > n) take = n;
    memcpy(b->buf + b->got, data, take);
    b->got += take;
    b->last_ms = now_ms;
    if (b->got == b->want)
        b->state = cal_backup_crc32(b->buf, b->want) == b->crc ? RAW_DONE_OK : RAW_DONE_CRC;
    return take;
}

void raw_block_poll(raw_block_t *b, uint32_t now_ms)
{
    if (b->state == RAW_RECEIVING && now_ms - b->last_ms >= RAW_BLOCK_IDLE_MS)
        b->state = RAW_TIMEOUT;
}
