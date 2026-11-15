#include "fpga_slot.h"
#include "cal_backup_core.h"   /* the project's one CRC32 */
#include <string.h>

static void put32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static void put16(uint8_t *p, uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1]<<8; }

static void do_tick(void (*tick)(void)) { if (tick) tick(); }

const char *fslot_status_str(fslot_status_t st)
{
    switch (st) {
    case FSLOT_OK:             return "ok";
    case FSLOT_ERR_NOT_READY:  return "flash region layer not ready";
    case FSLOT_ERR_IO:         return "flash io error or busy";
    case FSLOT_ERR_BAD_ARG:    return "bad argument or no upload in progress";
    case FSLOT_ERR_SEQUENCE:   return "chunk offset out of sequence";
    case FSLOT_ERR_INCOMPLETE: return "upload incomplete";
    case FSLOT_ERR_CRC:        return "CRC mismatch";
    case FSLOT_ERR_NONE:       return "no valid design stored";
    }
    return "unknown";
}

void fslot_write_header(uint8_t out[FSLOT_HDR_LEN], uint32_t len, uint32_t payload_crc, const char *name,
                        uint32_t flags)
{
    memset(out, 0, FSLOT_HDR_LEN);
    put32(out + 0, FSLOT_MAGIC);
    put16(out + 4, FSLOT_VERSION);
    put16(out + 6, flags);
    put32(out + 8, len);
    put32(out + 12, payload_crc);
    if (name != NULL) {
        for (uint32_t i = 0; i < FSLOT_NAME_LEN && name[i] != '\0'; i++) out[16 + i] = (uint8_t)name[i];
    }
    put32(out + 60, cal_backup_crc32(out, 60u));
}

void fslot_classify_header(const uint8_t hdr[FSLOT_HDR_LEN], fslot_info_t *info)
{
    info->len = 0; info->crc = 0; info->name[0] = '\0'; info->boot = false;
    bool blank = true;
    for (uint32_t i = 0; i < FSLOT_HDR_LEN; i++) if (hdr[i] != 0xFFu) { blank = false; break; }
    if (blank) { info->state = FSLOT_EMPTY; return; }
    info->state = FSLOT_DAMAGED;
    if (get32(hdr) != FSLOT_MAGIC) return;
    if (get32(hdr + 60) != cal_backup_crc32(hdr, 60u)) return;
    if (get16(hdr + 4) != FSLOT_VERSION) return;
    const uint32_t len = get32(hdr + 8);
    if (len == 0u || len > FSLOT_MAX_LEN) return;
    info->state = FSLOT_VALID;
    info->len = len;
    info->crc = get32(hdr + 12);
    info->boot = (get16(hdr + 6) & FSLOT_FLAG_BOOT) != 0u;
    memcpy(info->name, hdr + 16, FSLOT_NAME_LEN);
    info->name[FSLOT_NAME_LEN] = '\0';
}

fslot_status_t fslot_info(fslot_info_t *info)
{
    if (!flash_regions_ready()) return FSLOT_ERR_NOT_READY;
    uint8_t hdr[FSLOT_HDR_LEN];
    if (flash_region_read(FSLOT_REGION, 0, hdr, sizeof hdr) != FLASH_REGION_OK) return FSLOT_ERR_IO;
    fslot_classify_header(hdr, info);
    return FSLOT_OK;
}

fslot_status_t fslot_span_blank(bool *blank, void (*tick)(void))
{
    if (blank == NULL) return FSLOT_ERR_BAD_ARG;
    if (!flash_regions_ready()) return FSLOT_ERR_NOT_READY;
    uint8_t buf[256];
    *blank = true;
    for (uint32_t off = 0; off < FSLOT_SPAN; off += sizeof buf) {
        if (flash_region_read(FSLOT_REGION, off, buf, sizeof buf) != FLASH_REGION_OK) return FSLOT_ERR_IO;
        for (uint32_t i = 0; i < sizeof buf; i++) if (buf[i] != 0xFFu) { *blank = false; return FSLOT_OK; }
        if ((off & 0x3FFFu) == 0u) do_tick(tick);
    }
    return FSLOT_OK;
}

fslot_status_t fslot_put_begin(fslot_upload_t *up, uint32_t declared_len, uint32_t declared_crc,
                               const char *name, void (*tick)(void))
{
    if (up == NULL || declared_len == 0u || declared_len > FSLOT_MAX_LEN) return FSLOT_ERR_BAD_ARG;
    if (!flash_regions_ready()) return FSLOT_ERR_NOT_READY;
    up->active = false;
    /* Header sector first: from here on the old design is gone, a power cut
     * leaves EMPTY, never a header over a half-replaced payload. */
    const uint32_t sectors = (FSLOT_DATA_OFFSET + declared_len + FLASH_REGION_SECTOR_SIZE - 1u)
                             / FLASH_REGION_SECTOR_SIZE;
    for (uint32_t i = 0; i < sectors; i++) {
        if (flash_region_erase(FSLOT_REGION, i * FLASH_REGION_SECTOR_SIZE,
                               FLASH_REGION_SECTOR_SIZE) != FLASH_REGION_OK)
            return FSLOT_ERR_IO;
        do_tick(tick);
    }
    up->declared_len = declared_len;
    up->declared_crc = declared_crc;
    up->received = 0;
    memset(up->name, 0, sizeof up->name);
    if (name != NULL) strncpy(up->name, name, FSLOT_NAME_LEN);
    up->active = true;
    return FSLOT_OK;
}

fslot_status_t fslot_put_data(fslot_upload_t *up, uint32_t off, const void *data, uint32_t n)
{
    if (up == NULL || !up->active || data == NULL || n == 0u || n > FSLOT_CHUNK_MAX)
        return FSLOT_ERR_BAD_ARG;
    if (off != up->received) return FSLOT_ERR_SEQUENCE;
    if (n > up->declared_len - off) return FSLOT_ERR_BAD_ARG;
    if (flash_region_write(FSLOT_REGION, FSLOT_DATA_OFFSET + off, data, n) != FLASH_REGION_OK)
        return FSLOT_ERR_IO;
    up->received += n;
    return FSLOT_OK;
}

static fslot_status_t crc_payload(uint32_t len, uint32_t *crc, void (*tick)(void))
{
    uint8_t buf[256];
    uint32_t c = CAL_BACKUP_CRC32_INIT;
    for (uint32_t off = 0; off < len; off += sizeof buf) {
        uint32_t n = len - off;
        if (n > sizeof buf) n = sizeof buf;
        if (flash_region_read(FSLOT_REGION, FSLOT_DATA_OFFSET + off, buf, n) != FLASH_REGION_OK)
            return FSLOT_ERR_IO;
        c = cal_backup_crc32_update(c, buf, n);
        if ((off & 0x3FFFu) == 0u) do_tick(tick);
    }
    *crc = cal_backup_crc32_final(c);
    return FSLOT_OK;
}

fslot_status_t fslot_put_end(fslot_upload_t *up, void (*tick)(void))
{
    if (up == NULL || !up->active) return FSLOT_ERR_BAD_ARG;
    up->active = false;
    if (up->received != up->declared_len) return FSLOT_ERR_INCOMPLETE;
    uint32_t crc;
    fslot_status_t st = crc_payload(up->declared_len, &crc, tick);
    if (st != FSLOT_OK) return st;
    if (crc != up->declared_crc) return FSLOT_ERR_CRC;
    uint8_t hdr[FSLOT_HDR_LEN];
    fslot_write_header(hdr, up->declared_len, crc, up->name, 0u);
    if (flash_region_write(FSLOT_REGION, 0, hdr, sizeof hdr) != FLASH_REGION_OK)
        return FSLOT_ERR_IO;
    return FSLOT_OK;
}

fslot_status_t fslot_set_boot(bool boot)
{
    fslot_info_t info;
    fslot_status_t st = fslot_info(&info);
    if (st != FSLOT_OK) return st;
    if (info.state != FSLOT_VALID) return FSLOT_ERR_NONE;
    if (info.boot == boot) return FSLOT_OK;
    uint8_t hdr[FSLOT_HDR_LEN];
    fslot_write_header(hdr, info.len, info.crc, info.name, boot ? FSLOT_FLAG_BOOT : 0u);
    if (flash_region_erase(FSLOT_REGION, 0, FLASH_REGION_SECTOR_SIZE) != FLASH_REGION_OK) return FSLOT_ERR_IO;
    if (flash_region_write(FSLOT_REGION, 0, hdr, sizeof hdr) != FLASH_REGION_OK) return FSLOT_ERR_IO;
    return FSLOT_OK;
}

fslot_status_t fslot_clear(void)
{
    if (!flash_regions_ready()) return FSLOT_ERR_NOT_READY;
    if (flash_region_erase(FSLOT_REGION, 0, FLASH_REGION_SECTOR_SIZE) != FLASH_REGION_OK)
        return FSLOT_ERR_IO;
    return FSLOT_OK;
}

fslot_status_t fslot_read(uint32_t off, void *buf, uint32_t len)
{
    if (buf == NULL || off > FSLOT_MAX_LEN || len > FSLOT_MAX_LEN - off) return FSLOT_ERR_BAD_ARG;
    if (!flash_regions_ready()) return FSLOT_ERR_NOT_READY;
    if (flash_region_read(FSLOT_REGION, FSLOT_DATA_OFFSET + off, buf, len) != FLASH_REGION_OK)
        return FSLOT_ERR_IO;
    return FSLOT_OK;
}

fslot_status_t fslot_verify(fslot_info_t *info, void (*tick)(void))
{
    fslot_status_t st = fslot_info(info);
    if (st != FSLOT_OK) return st;
    if (info->state != FSLOT_VALID) return FSLOT_ERR_NONE;
    uint32_t crc;
    st = crc_payload(info->len, &crc, tick);
    if (st != FSLOT_OK) return st;
    return (crc == info->crc) ? FSLOT_OK : FSLOT_ERR_CRC;
}
