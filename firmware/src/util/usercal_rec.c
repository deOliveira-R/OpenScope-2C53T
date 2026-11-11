/*
 * usercal_rec.c — record format and storage for OpenScope's own calibration
 * corrections. See usercal_rec.h for the layout and the reasons behind it.
 */
#include "usercal_rec.h"

#include <string.h>

#include "cal_backup_core.h"   /* cal_backup_crc32: the project's one CRC32 */

#define OFF_MAGIC     0u
#define OFF_VERSION   4u
#define OFF_LEN       6u
#define OFF_TABLE     8u
#define OFF_FLAGS     12u
#define OFF_IL_GAIN   16u
#define OFF_IL_OFF    20u
#define OFF_VALID     24u
#define OFF_CORR      32u
#define OFF_CRC       (OFF_CORR + CALPAGE_CHANNELS * CALPAGE_BANKS * CALPAGE_RANGES * 2u * 2u)

/* The layout constants above and the header's total must agree. */
typedef char usercal_len_check[(OFF_CRC + 4u == USERCAL_REC_LEN) ? 1 : -1];

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

const char *usercal_status_str(usercal_status_t st)
{
    switch (st) {
    case USERCAL_ST_OK:         return "ok";
    case USERCAL_ST_EMPTY:      return "no record";
    case USERCAL_ST_NO_STORAGE: return "no storage";
    case USERCAL_ST_IO:         return "flash io error";
    case USERCAL_ST_SIZE:       return "wrong record size";
    case USERCAL_ST_MAGIC:      return "bad magic";
    case USERCAL_ST_VERSION:    return "unknown version";
    case USERCAL_ST_CRC:        return "bad crc";
    case USERCAL_ST_RANGE:      return "implausible value";
    }
    return "?";
}

void usercal_clear(usercal_t *u)
{
    if (u) memset(u, 0, sizeof(*u));
}

uint32_t usercal_table_crc(const uint8_t *page)
{
    return page ? cal_backup_crc32(page + USERCAL_TABLE_OFF, USERCAL_TABLE_LEN) : 0u;
}

unsigned usercal_count(const usercal_t *u)
{
    unsigned n = 0;
    if (!u) return 0;
    for (unsigned c = 0; c < CALPAGE_CHANNELS; c++)
        for (uint32_t v = u->valid[c]; v; v &= v - 1u) n++;
    return n;
}

size_t usercal_encode(const usercal_t *u, uint8_t *buf, size_t cap)
{
    if (!u || !buf || cap < USERCAL_REC_LEN) return 0;
    memset(buf, 0, USERCAL_REC_LEN);
    put32(buf + OFF_MAGIC, USERCAL_MAGIC);
    put16(buf + OFF_VERSION, USERCAL_VERSION);
    put16(buf + OFF_LEN, USERCAL_REC_LEN);
    put32(buf + OFF_TABLE, u->table_crc);
    put32(buf + OFF_FLAGS, u->il_present ? USERCAL_F_IL : 0u);
    /* An absent block is stored as identity so two "no interleave" records
     * are byte-identical (and the layer elides the second). */
    put32(buf + OFF_IL_GAIN, (uint32_t)(u->il_present ? u->il_gain_ppm : 1000000));
    put32(buf + OFF_IL_OFF,  (uint32_t)(u->il_present ? u->il_off_e4 : 0));
    uint8_t *p = buf + OFF_CORR;
    for (unsigned c = 0; c < CALPAGE_CHANNELS; c++) {
        put32(buf + OFF_VALID + 4u * c, u->valid[c]);
        for (unsigned b = 0; b < CALPAGE_BANKS; b++)
            for (unsigned r = 0; r < CALPAGE_RANGES; r++) {
                /* Deltas without a valid bit are zeroed, for the same reason. */
                const bool v = (u->valid[c] >> (b * CALPAGE_RANGES + r)) & 1u;
                put16(p,      (uint16_t)(v ? u->corr[c][b][r][0] : 0));
                put16(p + 2u, (uint16_t)(v ? u->corr[c][b][r][1] : 0));
                p += 4u;
            }
    }
    put32(buf + OFF_CRC, cal_backup_crc32(buf, OFF_CRC));
    return USERCAL_REC_LEN;
}

usercal_status_t usercal_decode(const uint8_t *buf, size_t len, usercal_t *out)
{
    if (!buf || !out || len < 8u) return USERCAL_ST_SIZE;
    if (get32(buf + OFF_MAGIC) != USERCAL_MAGIC) return USERCAL_ST_MAGIC;
    /* Version before length: a later layout is allowed to change the size,
     * and "unknown version" is the truthful report for it. */
    if (get16(buf + OFF_VERSION) != USERCAL_VERSION) return USERCAL_ST_VERSION;
    if (len != USERCAL_REC_LEN || get16(buf + OFF_LEN) != USERCAL_REC_LEN) return USERCAL_ST_SIZE;
    if (get32(buf + OFF_CRC) != cal_backup_crc32(buf, OFF_CRC)) return USERCAL_ST_CRC;

    /* Validate everything from the bytes first, then fill *out: a rejected
     * record never reaches the caller's struct, and no second 280-byte copy
     * sits on the boot stack (main() runs this before the scheduler, on the
     * ~1.2 KB MSP stack). */
    const uint32_t flags = get32(buf + OFF_FLAGS);
    if (flags & ~USERCAL_F_IL) return USERCAL_ST_RANGE;   /* bits no v1 writer sets */
    const bool il = (flags & USERCAL_F_IL) != 0u;
    const int32_t gain = (int32_t)get32(buf + OFF_IL_GAIN);
    const int32_t off  = (int32_t)get32(buf + OFF_IL_OFF);
    if (il && (gain < USERCAL_IL_GAIN_MIN || gain > USERCAL_IL_GAIN_MAX ||
               off < -USERCAL_IL_OFF_MAX || off > USERCAL_IL_OFF_MAX))
        return USERCAL_ST_RANGE;
    const uint32_t used = (1u << (CALPAGE_BANKS * CALPAGE_RANGES)) - 1u;
    for (unsigned c = 0; c < CALPAGE_CHANNELS; c++) {
        const uint32_t v = get32(buf + OFF_VALID + 4u * c);
        if (v & ~used) return USERCAL_ST_RANGE;
        const uint8_t *p = buf + OFF_CORR + c * (CALPAGE_BANKS * CALPAGE_RANGES * 4u);
        for (unsigned k = 0; k < CALPAGE_BANKS * CALPAGE_RANGES; k++, p += 4u) {
            if (!((v >> k) & 1u)) continue;
            const int16_t db = (int16_t)get16(p), du = (int16_t)get16(p + 2u);
            if (db < -USERCAL_DELTA_MAX || db > USERCAL_DELTA_MAX ||
                du < -USERCAL_DELTA_MAX || du > USERCAL_DELTA_MAX)
                return USERCAL_ST_RANGE;
        }
    }

    usercal_clear(out);
    out->table_crc   = get32(buf + OFF_TABLE);
    out->il_present  = il;
    out->il_gain_ppm = gain;
    out->il_off_e4   = off;
    const uint8_t *p = buf + OFF_CORR;
    for (unsigned c = 0; c < CALPAGE_CHANNELS; c++) {
        out->valid[c] = get32(buf + OFF_VALID + 4u * c);
        for (unsigned b = 0; b < CALPAGE_BANKS; b++)
            for (unsigned r = 0; r < CALPAGE_RANGES; r++, p += 4u) {
                if (!((out->valid[c] >> (b * CALPAGE_RANGES + r)) & 1u)) continue;
                out->corr[c][b][r][0] = (int16_t)get16(p);
                out->corr[c][b][r][1] = (int16_t)get16(p + 2u);
            }
    }
    return USERCAL_ST_OK;
}

flash_region_status_t usercal_store_save(const usercal_t *u)
{
    uint8_t buf[USERCAL_REC_LEN];
    if (!u || usercal_encode(u, buf, sizeof(buf)) != USERCAL_REC_LEN) return FLASH_REGION_ERR_BAD_ARG;
    if (!flash_regions_ready()) return FLASH_REGION_ERR_NOT_INIT;
    flash_region_status_t st = flash_region_append(USERCAL_REGION, buf, sizeof(buf));
    if (st == FLASH_REGION_ERR_FULL || st == FLASH_REGION_ERR_LOG_DAMAGED) {
        /* Same way out as config_save(): the log takes no more records
         * (issue #57), so reset our own region and re-append the newest. A
         * power cut in between loses the saved corrections — the unit then
         * boots on the page alone, the safe failure — never anything else. */
        const flash_region_status_t rst = flash_region_reset(USERCAL_REGION);
        if (rst != FLASH_REGION_OK) return rst;
        st = flash_region_append(USERCAL_REGION, buf, sizeof(buf));
    }
    return st;
}

usercal_status_t usercal_store_load(usercal_t *out)
{
    uint8_t buf[USERCAL_REC_LEN];
    uint32_t len = 0;
    if (!out) return USERCAL_ST_IO;
    if (!flash_regions_ready()) return USERCAL_ST_NO_STORAGE;
    const flash_region_status_t st = flash_region_read_latest(USERCAL_REGION, buf, sizeof(buf), &len);
    if (st == FLASH_REGION_ERR_NOT_FOUND) return USERCAL_ST_EMPTY;
    if (st == FLASH_REGION_ERR_BOUNDS) return USERCAL_ST_SIZE;   /* longer than a v1 record */
    if (st != FLASH_REGION_OK) return USERCAL_ST_IO;
    return usercal_decode(buf, len, out);
}
