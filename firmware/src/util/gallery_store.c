/*
 * gallery_store.c — see gallery_store.h for the layout and why it is not
 * stock's FAT volume.
 */
#include "gallery_store.h"
#include "cal_backup_core.h"   /* the project's one CRC32 (same parameters as flash_regions.c) */

#include <string.h>

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

static uint32_t slot_offset(uint32_t slot) { return slot * GALLERY_SLOT_SIZE; }

uint32_t gallery_slot_count(void)
{
    if (!flash_regions_ready()) return 0;
    const flash_region_t *r = flash_region_get(GALLERY_REGION);
    if (r == NULL) return 0;
    uint32_t n = r->length / GALLERY_SLOT_SIZE;
    return (n > GALLERY_MAX_SLOTS) ? GALLERY_MAX_SLOTS : n;
}

/* ── Names ────────────────────────────────────────────────────────────── */

uint32_t gallery_name(uint32_t seq, char *out, uint32_t out_size)
{
    char digits[11];
    uint32_t n = 0;
    do {
        digits[n++] = (char)('0' + (seq % 10u));
        seq /= 10u;
    } while (seq != 0u && n < sizeof digits);
    if (out_size < n + 5u) {            /* digits + ".bmp" + NUL */
        if (out_size) out[0] = '\0';
        return 0;
    }
    for (uint32_t i = 0; i < n; i++) out[i] = digits[n - 1u - i];
    memcpy(out + n, ".bmp", 5);
    return n + 4u;
}

bool gallery_parse_name(const char *s, uint32_t *seq)
{
    if (s == NULL || seq == NULL) return false;
    uint64_t v = 0;
    uint32_t nd = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (uint64_t)(*s - '0');
        if (v > 0xFFFFFFFFu) return false;
        s++;
        nd++;
    }
    if (nd == 0u || v == 0u) return false;
    if (*s == '.') {
        const char *e = s + 1;
        if (!((e[0] | 0x20) == 'b' && (e[1] | 0x20) == 'm' && (e[2] | 0x20) == 'p' && e[3] == '\0'))
            return false;
    } else if (*s != '\0') {
        return false;
    }
    *seq = (uint32_t)v;
    return true;
}

/* ── BMP header ───────────────────────────────────────────────────────── */

void gallery_bmp_header(uint8_t out[GALLERY_BMP_HDR_LEN], const uint16_t pal565[16])
{
    memset(out, 0, GALLERY_BMP_HDR_LEN);
    out[0] = 'B';
    out[1] = 'M';
    put32(out + 2, GALLERY_BMP_LEN);
    put32(out + 10, GALLERY_BMP_HDR_LEN);            /* pixel data offset */
    put32(out + 14, 40u);                            /* BITMAPINFOHEADER */
    put32(out + 18, GALLERY_IMG_W);
    put32(out + 22, (uint32_t)(-(int32_t)GALLERY_IMG_H));  /* top-down, like the shadow */
    put16(out + 26, 1u);                             /* planes */
    put16(out + 28, 4u);                             /* bpp */
    put32(out + 30, 0u);                             /* BI_RGB */
    put32(out + 34, GALLERY_PIXEL_BYTES);
    put32(out + 38, 2835u);                          /* 72 dpi, cosmetic */
    put32(out + 42, 2835u);
    put32(out + 46, 16u);                            /* colours used */
    put32(out + 50, 16u);
    for (uint32_t i = 0; i < 16u; i++) {
        uint16_t c = pal565[i];
        uint8_t r5 = (uint8_t)((c >> 11) & 0x1Fu);
        uint8_t g6 = (uint8_t)((c >> 5) & 0x3Fu);
        uint8_t b5 = (uint8_t)(c & 0x1Fu);
        uint8_t *q = out + 54u + i * 4u;
        q[0] = (uint8_t)((b5 << 3) | (b5 >> 2));     /* BMP palette is B, G, R, 0 */
        q[1] = (uint8_t)((g6 << 2) | (g6 >> 4));
        q[2] = (uint8_t)((r5 << 3) | (r5 >> 2));
        q[3] = 0u;
    }
}

/* ── Slot header ──────────────────────────────────────────────────────
 *   +0  magic   u32  "OSG1"; zeroed by delete
 *   +4  version u16, +6 header length u16 (24)
 *   +8  seq u32, +12 bmp_len u32, +16 bmp_crc u32
 *   +20 crc32 of bytes 4..19 — deliberately NOT covering the magic, so a
 *       deleted header still proves its seq and the numbering stays monotonic. */

static uint32_t body_crc(const uint8_t *hdr) { return cal_backup_crc32(hdr + 4, 16u); }

void gallery_write_slot_header(uint8_t out[GALLERY_HDR_LEN], uint32_t seq,
                               uint32_t bmp_len, uint32_t bmp_crc)
{
    put32(out + 0, GALLERY_MAGIC);
    put16(out + 4, GALLERY_VERSION);
    put16(out + 6, GALLERY_HDR_LEN);
    put32(out + 8, seq);
    put32(out + 12, bmp_len);
    put32(out + 16, bmp_crc);
    put32(out + 20, body_crc(out));
}

void gallery_classify_header(const uint8_t hdr[GALLERY_HDR_LEN], gallery_slot_info_t *info)
{
    memset(info, 0, sizeof *info);
    bool blank = true;
    for (uint32_t i = 0; i < GALLERY_HDR_LEN; i++) {
        if (hdr[i] != 0xFFu) { blank = false; break; }
    }
    if (blank) {
        info->state = GALLERY_SLOT_EMPTY;
        return;
    }
    const bool body_ok = get32(hdr + 20) == body_crc(hdr) &&
                         get16(hdr + 4) == GALLERY_VERSION &&
                         get16(hdr + 6) == GALLERY_HDR_LEN &&
                         get32(hdr + 12) == GALLERY_BMP_LEN &&
                         get32(hdr + 8) != 0u;
    if (body_ok) {
        info->seq = get32(hdr + 8);
        info->bmp_len = get32(hdr + 12);
        info->bmp_crc = get32(hdr + 16);
        info->seq_known = true;
    }
    const uint32_t magic = get32(hdr);
    if (body_ok && magic == GALLERY_MAGIC)
        info->state = GALLERY_SLOT_VALID;
    else if (body_ok && magic == 0u)
        info->state = GALLERY_SLOT_DELETED;
    else
        info->state = GALLERY_SLOT_DAMAGED;
}

gallery_status_t gallery_slot_info(uint32_t slot, gallery_slot_info_t *info)
{
    if (info == NULL) return GALLERY_ERR_BAD_ARG;
    const uint32_t n = gallery_slot_count();
    if (n == 0u) return GALLERY_ERR_NOT_READY;
    if (slot >= n) return GALLERY_ERR_BAD_ARG;
    uint8_t hdr[GALLERY_HDR_LEN];
    if (flash_region_read(GALLERY_REGION, slot_offset(slot), hdr, sizeof hdr) != FLASH_REGION_OK)
        return GALLERY_ERR_IO;
    gallery_classify_header(hdr, info);
    return GALLERY_OK;
}

gallery_status_t gallery_scan(gallery_index_t *idx)
{
    if (idx == NULL) return GALLERY_ERR_BAD_ARG;
    memset(idx, 0, sizeof *idx);
    idx->next_seq = 1u;
    const uint32_t n = gallery_slot_count();
    if (n == 0u) return GALLERY_ERR_NOT_READY;
    idx->slots_total = (uint8_t)n;
    uint32_t max_seq = 0;
    for (uint32_t s = 0; s < n; s++) {
        gallery_slot_info_t info;
        gallery_status_t st = gallery_slot_info(s, &info);
        if (st != GALLERY_OK) return st;
        if (info.seq_known && info.seq > max_seq) max_seq = info.seq;
        if (info.state != GALLERY_SLOT_VALID) continue;
        /* insertion, newest first */
        uint32_t k = idx->count;
        while (k > 0u && idx->seq[k - 1u] < info.seq) {
            idx->seq[k] = idx->seq[k - 1u];
            idx->slot[k] = idx->slot[k - 1u];
            k--;
        }
        idx->seq[k] = info.seq;
        idx->slot[k] = (uint8_t)s;
        idx->count++;
    }
    idx->next_seq = (max_seq == 0xFFFFFFFFu) ? max_seq : max_seq + 1u;
    return GALLERY_OK;
}

gallery_status_t gallery_find(uint32_t seq, uint32_t *slot)
{
    gallery_index_t idx;
    gallery_status_t st = gallery_scan(&idx);
    if (st != GALLERY_OK) return st;
    for (uint32_t i = 0; i < idx.count; i++) {
        if (idx.seq[i] == seq) {
            if (slot) *slot = idx.slot[i];
            return GALLERY_OK;
        }
    }
    return GALLERY_ERR_NOT_FOUND;
}

gallery_status_t gallery_pick_slot(uint32_t *slot)
{
    const uint32_t n = gallery_slot_count();
    if (n == 0u) return GALLERY_ERR_NOT_READY;
    uint32_t reuse = n;
    for (uint32_t s = 0; s < n; s++) {
        gallery_slot_info_t info;
        gallery_status_t st = gallery_slot_info(s, &info);
        if (st != GALLERY_OK) return st;
        if (info.state == GALLERY_SLOT_EMPTY) { *slot = s; return GALLERY_OK; }
        if (info.state != GALLERY_SLOT_VALID && reuse == n) reuse = s;
    }
    if (reuse == n) return GALLERY_ERR_FULL;
    *slot = reuse;
    return GALLERY_OK;
}

/* ── Save / delete ────────────────────────────────────────────────────── */

static void do_tick(void (*tick)(void)) { if (tick) tick(); }

static volatile uint32_t g_generation;

uint32_t gallery_generation(void) { return g_generation; }

gallery_status_t gallery_save(const uint8_t *pixels, const uint16_t pal565[16],
                              void (*tick)(void), uint32_t *seq_out)
{
    if (pixels == NULL || pal565 == NULL) return GALLERY_ERR_BAD_ARG;
    gallery_index_t idx;
    gallery_status_t st = gallery_scan(&idx);
    if (st != GALLERY_OK) return st;
    uint32_t slot;
    st = gallery_pick_slot(&slot);
    if (st != GALLERY_OK) return st;

    const uint32_t base = slot_offset(slot);
    g_generation++;   /* from here on the slot's bytes change, whatever the outcome */
    /* Erase the whole slot, sector by sector, so the caller's tick runs
     * between sector erases (a W25Q sector erase is up to 400 ms). The region
     * layer skips sectors that already read blank. */
    for (uint32_t i = 0; i < GALLERY_SLOT_SECTORS; i++) {
        if (flash_region_erase(GALLERY_REGION, base + i * FLASH_REGION_SECTOR_SIZE,
                               FLASH_REGION_SECTOR_SIZE) != FLASH_REGION_OK)
            return GALLERY_ERR_IO;
        do_tick(tick);
    }

    uint8_t bmp_hdr[GALLERY_BMP_HDR_LEN];
    gallery_bmp_header(bmp_hdr, pal565);
    uint32_t crc = cal_backup_crc32_update(CAL_BACKUP_CRC32_INIT, bmp_hdr, sizeof bmp_hdr);
    if (flash_region_write(GALLERY_REGION, base + GALLERY_BMP_OFFSET, bmp_hdr, sizeof bmp_hdr)
            != FLASH_REGION_OK)
        return GALLERY_ERR_IO;

    /* Pixels straight from the caller's buffer (the live LCD shadow on the
     * device: no copy). The CRC is taken over the same bytes that are written;
     * flash_region_write() reads every page back, so CRC and flash agree. */
    const uint32_t chunk = FLASH_REGION_SECTOR_SIZE;
    for (uint32_t off = 0; off < GALLERY_PIXEL_BYTES; off += chunk) {
        uint32_t n = GALLERY_PIXEL_BYTES - off;
        if (n > chunk) n = chunk;
        crc = cal_backup_crc32_update(crc, pixels + off, n);
        if (flash_region_write(GALLERY_REGION, base + GALLERY_BMP_OFFSET + GALLERY_BMP_HDR_LEN + off,
                               pixels + off, n) != FLASH_REGION_OK)
            return GALLERY_ERR_IO;
        do_tick(tick);
    }
    crc = cal_backup_crc32_final(crc);

    uint8_t hdr[GALLERY_HDR_LEN];
    gallery_write_slot_header(hdr, idx.next_seq, GALLERY_BMP_LEN, crc);
    if (flash_region_write(GALLERY_REGION, base, hdr, sizeof hdr) != FLASH_REGION_OK)
        return GALLERY_ERR_IO;
    if (seq_out) *seq_out = idx.next_seq;
    g_generation++;   /* the commit record is what makes the shot appear */
    return GALLERY_OK;
}

gallery_status_t gallery_delete(uint32_t seq)
{
    uint32_t slot;
    gallery_status_t st = gallery_find(seq, &slot);
    if (st != GALLERY_OK) return st;
    static const uint8_t zero[4] = { 0, 0, 0, 0 };
    g_generation++;
    if (flash_region_write(GALLERY_REGION, slot_offset(slot), zero, sizeof zero) != FLASH_REGION_OK)
        return GALLERY_ERR_IO;
    return GALLERY_OK;
}

/* ── Read back ────────────────────────────────────────────────────────── */

gallery_status_t gallery_read_bmp(uint32_t slot, uint32_t off, void *buf, uint32_t len)
{
    const uint32_t n = gallery_slot_count();
    if (n == 0u) return GALLERY_ERR_NOT_READY;
    if (buf == NULL || slot >= n || off > GALLERY_BMP_LEN || len > GALLERY_BMP_LEN - off)
        return GALLERY_ERR_BAD_ARG;
    if (len == 0u) return GALLERY_OK;
    if (flash_region_read(GALLERY_REGION, slot_offset(slot) + GALLERY_BMP_OFFSET + off, buf, len)
            != FLASH_REGION_OK)
        return GALLERY_ERR_IO;
    return GALLERY_OK;
}

gallery_status_t gallery_read_row(uint32_t slot, uint32_t row, uint8_t out[GALLERY_ROW_BYTES])
{
    if (row >= GALLERY_IMG_H) return GALLERY_ERR_BAD_ARG;
    return gallery_read_bmp(slot, GALLERY_BMP_HDR_LEN + row * GALLERY_ROW_BYTES, out, GALLERY_ROW_BYTES);
}

gallery_status_t gallery_read_palette(uint32_t slot, uint16_t pal565[16])
{
    uint8_t p[64];
    gallery_status_t st = gallery_read_bmp(slot, 54u, p, sizeof p);
    if (st != GALLERY_OK) return st;
    for (uint32_t i = 0; i < 16u; i++) {
        const uint8_t *q = p + i * 4u;
        pal565[i] = (uint16_t)(((q[2] >> 3) << 11) | ((q[1] >> 2) << 5) | (q[0] >> 3));
    }
    return GALLERY_OK;
}

gallery_status_t gallery_crc_bmp(uint32_t slot, uint32_t *crc)
{
    uint8_t buf[64];
    uint32_t c = CAL_BACKUP_CRC32_INIT;
    for (uint32_t off = 0; off < GALLERY_BMP_LEN; off += sizeof buf) {
        uint32_t n = GALLERY_BMP_LEN - off;
        if (n > sizeof buf) n = sizeof buf;
        gallery_status_t st = gallery_read_bmp(slot, off, buf, n);
        if (st != GALLERY_OK) return st;
        c = cal_backup_crc32_update(c, buf, n);
    }
    *crc = cal_backup_crc32_final(c);
    return GALLERY_OK;
}
