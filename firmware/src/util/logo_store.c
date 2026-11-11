#include "logo_store.h"
#include "cal_backup_core.h"   /* the project's one CRC32 */
#include <string.h>

static void put32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static void put16(uint8_t *p, uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1]<<8; }

static void do_tick(void (*tick)(void)) { if (tick) tick(); }

const char *logo_status_str(logo_status_t st)
{
    switch (st) {
    case LOGO_OK:             return "ok";
    case LOGO_ERR_NOT_READY:  return "flash region layer not ready";
    case LOGO_ERR_IO:         return "flash io error or busy";
    case LOGO_ERR_BAD_ARG:    return "bad argument or no upload in progress";
    case LOGO_ERR_SEQUENCE:   return "chunk offset out of sequence";
    case LOGO_ERR_INCOMPLETE: return "upload incomplete";
    case LOGO_ERR_CRC:        return "CRC mismatch (nothing committed)";
    case LOGO_ERR_NONE:       return "no valid logo stored";
    }
    return "unknown";
}

void logo_write_header(uint8_t out[LOGO_HDR_LEN], uint32_t payload_crc)
{
    memset(out, 0, LOGO_HDR_LEN);
    put32(out + 0, LOGO_MAGIC);
    put16(out + 4, LOGO_VERSION);
    put16(out + 6, LOGO_FMT_RGB565);
    put16(out + 8, LOGO_W);
    put16(out + 10, LOGO_H);
    put32(out + 12, LOGO_PIXEL_BYTES);
    put32(out + 16, payload_crc);
    put32(out + 20, cal_backup_crc32(out, 20u));
}

void logo_classify_header(const uint8_t hdr[LOGO_HDR_LEN], logo_info_t *info)
{
    info->len = 0; info->crc = 0;
    bool blank = true;
    for (uint32_t i = 0; i < LOGO_HDR_LEN; i++) if (hdr[i] != 0xFFu) { blank = false; break; }
    if (blank) { info->state = LOGO_SLOT_EMPTY; return; }
    info->state = LOGO_SLOT_DAMAGED;
    if (get32(hdr) != LOGO_MAGIC) return;
    if (get32(hdr + 20) != cal_backup_crc32(hdr, 20u)) return;
    if (get16(hdr + 4) != LOGO_VERSION || get16(hdr + 6) != LOGO_FMT_RGB565) return;
    if (get16(hdr + 8) != LOGO_W || get16(hdr + 10) != LOGO_H) return;
    if (get32(hdr + 12) != LOGO_PIXEL_BYTES) return;
    info->state = LOGO_SLOT_VALID;
    info->len = get32(hdr + 12);
    info->crc = get32(hdr + 16);
}

logo_status_t logo_info(logo_info_t *info)
{
    if (!flash_regions_ready()) return LOGO_ERR_NOT_READY;
    uint8_t hdr[LOGO_HDR_LEN];
    if (flash_region_read(LOGO_REGION, 0, hdr, sizeof hdr) != FLASH_REGION_OK) return LOGO_ERR_IO;
    logo_classify_header(hdr, info);
    return LOGO_OK;
}

logo_status_t logo_put_begin(logo_upload_t *up, uint32_t declared_len,
                             uint32_t declared_crc, void (*tick)(void))
{
    if (up == NULL || declared_len != LOGO_PIXEL_BYTES) return LOGO_ERR_BAD_ARG;
    if (!flash_regions_ready()) return LOGO_ERR_NOT_READY;
    up->active = false;
    /* Header sector first: from here on the old logo is gone, a power cut
     * leaves EMPTY, never a header over half-replaced pixels. */
    for (uint32_t i = 0; i < LOGO_SECTORS; i++) {
        if (flash_region_erase(LOGO_REGION, i * FLASH_REGION_SECTOR_SIZE,
                               FLASH_REGION_SECTOR_SIZE) != FLASH_REGION_OK)
            return LOGO_ERR_IO;
        do_tick(tick);
    }
    up->declared_crc = declared_crc;
    up->received = 0;
    up->active = true;
    return LOGO_OK;
}

logo_status_t logo_put_data(logo_upload_t *up, uint32_t off, const void *data, uint32_t n)
{
    if (up == NULL || !up->active || data == NULL || n == 0u || n > LOGO_CHUNK_MAX)
        return LOGO_ERR_BAD_ARG;
    if (off != up->received) return LOGO_ERR_SEQUENCE;
    if (n > LOGO_PIXEL_BYTES - off) return LOGO_ERR_BAD_ARG;
    if (flash_region_write(LOGO_REGION, LOGO_DATA_OFFSET + off, data, n) != FLASH_REGION_OK)
        return LOGO_ERR_IO;
    up->received += n;
    return LOGO_OK;
}

static logo_status_t crc_payload(uint32_t *crc, void (*tick)(void))
{
    uint8_t buf[256];
    uint32_t c = CAL_BACKUP_CRC32_INIT;
    for (uint32_t off = 0; off < LOGO_PIXEL_BYTES; off += sizeof buf) {
        uint32_t n = LOGO_PIXEL_BYTES - off;
        if (n > sizeof buf) n = sizeof buf;
        if (flash_region_read(LOGO_REGION, LOGO_DATA_OFFSET + off, buf, n) != FLASH_REGION_OK)
            return LOGO_ERR_IO;
        c = cal_backup_crc32_update(c, buf, n);
        if ((off & 0x3FFFu) == 0u) do_tick(tick);
    }
    *crc = cal_backup_crc32_final(c);
    return LOGO_OK;
}

logo_status_t logo_crc_payload(uint32_t *crc, void (*tick)(void))
{
    if (!flash_regions_ready()) return LOGO_ERR_NOT_READY;
    return crc_payload(crc, tick);
}

logo_status_t logo_put_end(logo_upload_t *up, void (*tick)(void))
{
    if (up == NULL || !up->active) return LOGO_ERR_BAD_ARG;
    up->active = false;
    if (up->received != LOGO_PIXEL_BYTES) return LOGO_ERR_INCOMPLETE;
    uint32_t crc;
    logo_status_t st = crc_payload(&crc, tick);
    if (st != LOGO_OK) return st;
    if (crc != up->declared_crc) return LOGO_ERR_CRC;
    uint8_t hdr[LOGO_HDR_LEN];
    logo_write_header(hdr, crc);
    if (flash_region_write(LOGO_REGION, 0, hdr, sizeof hdr) != FLASH_REGION_OK)
        return LOGO_ERR_IO;
    return LOGO_OK;
}

logo_status_t logo_clear(void)
{
    if (!flash_regions_ready()) return LOGO_ERR_NOT_READY;
    if (flash_region_erase(LOGO_REGION, 0, FLASH_REGION_SECTOR_SIZE) != FLASH_REGION_OK)
        return LOGO_ERR_IO;
    return LOGO_OK;
}

logo_status_t logo_read(uint32_t off, void *buf, uint32_t len)
{
    if (buf == NULL || off > LOGO_PIXEL_BYTES || len > LOGO_PIXEL_BYTES - off) return LOGO_ERR_BAD_ARG;
    if (!flash_regions_ready()) return LOGO_ERR_NOT_READY;
    if (flash_region_read(LOGO_REGION, LOGO_DATA_OFFSET + off, buf, len) != FLASH_REGION_OK)
        return LOGO_ERR_IO;
    return LOGO_OK;
}

logo_status_t logo_stream(logo_pixel_sink_t sink, void *ctx)
{
    logo_info_t info;
    logo_status_t st = logo_info(&info);
    if (st != LOGO_OK) return st;
    if (info.state != LOGO_SLOT_VALID) return LOGO_ERR_NONE;
    uint16_t px[LOGO_STREAM_PX];   /* 128 B; flash bytes are little-endian RGB565 */
    for (uint32_t off = 0; off < LOGO_PIXEL_BYTES; off += 2u * LOGO_STREAM_PX) {
        uint8_t raw[2u * LOGO_STREAM_PX];
        st = logo_read(off, raw, sizeof raw);
        if (st != LOGO_OK) return st;
        for (uint32_t i = 0; i < LOGO_STREAM_PX; i++)
            px[i] = (uint16_t)(raw[2*i] | ((uint16_t)raw[2*i+1] << 8));
        sink(px, LOGO_STREAM_PX, ctx);
    }
    return LOGO_OK;
}
