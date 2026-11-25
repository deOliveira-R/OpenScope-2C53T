/*
 * OpenScope 2C53T - shell commands: screen
 *
 * Screen dumps, screenshots, the boot logo and the UI state dump.
 */

#include "shell_parts.h"

/* `screen dump [shadow] [x y w h]`: the LCD shadow (4-bit palette indices)
 * as text, one hex digit per pixel, a "ROW n" line per row after a
 * SCREENDUMP header. The region defaults to the whole screen; rows outside
 * the shadow's page read as index 0. The `shadow` keyword is accepted and
 * changes nothing. */
void cmd_screen_dump(const char *args)
{
    char buf[64];
    char *saveptr = NULL;
    char *tok;
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t w = LCD_WIDTH;
    uint32_t h = LCD_HEIGHT;

    if (args && *args) {
        if (strlen(args) >= sizeof(buf)) {
            usb_send_str("Usage: screen dump [shadow] [x y w h]\r\n");
            return;
        }

        strcpy(buf, args);
        tok = strtok_r(buf, " \t", &saveptr);
        if (tok && strcmp(tok, "shadow") == 0) {
            tok = strtok_r(NULL, " \t", &saveptr);
            if (tok == NULL) goto parsed_screen_args;
        }
        if (tok == NULL || parse_int(tok, &x) != 0) {
            usb_send_str("Usage: screen dump [shadow] [x y w h]\r\n");
            return;
        }
        tok = strtok_r(NULL, " \t", &saveptr);
        if (tok == NULL || parse_int(tok, &y) != 0) {
            usb_send_str("Usage: screen dump [shadow] [x y w h]\r\n");
            return;
        }
        tok = strtok_r(NULL, " \t", &saveptr);
        if (tok == NULL || parse_int(tok, &w) != 0) {
            usb_send_str("Usage: screen dump [shadow] [x y w h]\r\n");
            return;
        }
        tok = strtok_r(NULL, " \t", &saveptr);
        if (tok == NULL || parse_int(tok, &h) != 0) {
            usb_send_str("Usage: screen dump [shadow] [x y w h]\r\n");
            return;
        }
        if (strtok_r(NULL, " \t", &saveptr) != NULL) {
            usb_send_str("Usage: screen dump [shadow] [x y w h]\r\n");
            return;
        }
    }

parsed_screen_args:
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT ||
        w == 0 || h == 0 ||
        w > LCD_WIDTH || h > LCD_HEIGHT ||
        x > LCD_WIDTH - w || y > LCD_HEIGHT - h) {
        usb_send_str("Usage: screen dump [shadow] [x y w h]\r\n");
        usb_debug_printf("  bounds: x<%u y<%u x+w<=%u y+h<=%u\r\n",
                         (unsigned)LCD_WIDTH,
                         (unsigned)LCD_HEIGHT,
                         (unsigned)LCD_WIDTH,
                         (unsigned)LCD_HEIGHT);
        return;
    }

    usb_debug_printf("SCREENDUMP x=%lu y=%lu w=%lu h=%lu format=%s\r\n",
                     x, y, w, h, "indexed4");

    const uint8_t *bits = lcd_shadow_bits();
    static const char hexdigits[] = "0123456789ABCDEF";
    uint16_t page_y = lcd_shadow_page_y();
    for (uint32_t row = 0; row < h; row++) {
        usb_debug_printf("ROW %lu ", row);
        for (uint32_t col = 0; col < w; col++) {
            uint32_t sx = x + col;
            uint32_t sy = y + row;
            uint8_t idx = 0;
            if (sy >= page_y && sy < (uint32_t)page_y + LCD_SHADOW_HEIGHT) {
                uint32_t shadow_y = sy - page_y;
                uint8_t byte = bits[shadow_y * LCD_SHADOW_STRIDE + (sx >> 1)];
                idx = (sx & 1U) ? (byte & 0x0FU) : (byte >> 4);
            }
            char c = hexdigits[idx & 0x0F];
            usb_send_bytes((const uint8_t *)&c, 1);
        }
        usb_send_str("\r\n");
    }
}

/* Parse "x y w h" into the outputs, defaulting to the whole screen when args
 * is empty. Returns false on a malformed argument or a region off the
 * screen. */
static bool parse_screen_region_args(const char *args,
                                     uint32_t *x,
                                     uint32_t *y,
                                     uint32_t *w,
                                     uint32_t *h)
{
    char buf[64];
    char *saveptr = NULL;
    char *tok;

    *x = 0;
    *y = 0;
    *w = LCD_WIDTH;
    *h = LCD_HEIGHT;

    if (args && *args) {
        if (strlen(args) >= sizeof(buf)) return false;
        strcpy(buf, args);

        tok = strtok_r(buf, " \t", &saveptr);
        if (tok == NULL || parse_int(tok, x) != 0) return false;
        tok = strtok_r(NULL, " \t", &saveptr);
        if (tok == NULL || parse_int(tok, y) != 0) return false;
        tok = strtok_r(NULL, " \t", &saveptr);
        if (tok == NULL || parse_int(tok, w) != 0) return false;
        tok = strtok_r(NULL, " \t", &saveptr);
        if (tok == NULL || parse_int(tok, h) != 0) return false;
        if (strtok_r(NULL, " \t", &saveptr) != NULL) return false;
    }

    return *x < LCD_WIDTH && *y < LCD_HEIGHT &&
           *w > 0 && *h > 0 &&
           *w <= LCD_WIDTH && *h <= LCD_HEIGHT &&
           *x <= LCD_WIDTH - *w && *y <= LCD_HEIGHT - *h;
}

/* `screen dumpbin [x y w h]`: the LCD shadow region as raw bytes, two 4-bit
 * pixels per byte, high nibble first, each row padded to a whole byte. The
 * rows are packed twice: once for the CRC32 in the SCREENBIN header line,
 * then again to send, then an END line. */
void cmd_screen_dumpbin(const char *args)
{
    uint32_t x, y, w, h;
    uint8_t out_row[LCD_SHADOW_STRIDE];

    if (!parse_screen_region_args(args, &x, &y, &w, &h)) {
        usb_send_str("Usage: screen dumpbin [x y w h]\r\n");
        usb_debug_printf("  bounds: x<%u y<%u x+w<=%u y+h<=%u\r\n",
                         (unsigned)LCD_WIDTH,
                         (unsigned)LCD_HEIGHT,
                         (unsigned)LCD_WIDTH,
                         (unsigned)LCD_HEIGHT);
        return;
    }

    uint32_t row_len = (w + 1U) / 2U;
    uint32_t len = row_len * h;
    uint32_t crc = 0;
    const uint8_t *bits = lcd_shadow_bits();

    for (uint32_t row = 0; row < h; row++) {
        uint32_t sy = y + row;
        memset(out_row, 0, row_len);
        for (uint32_t col = 0; col < w; col++) {
            uint32_t sx = x + col;
            uint8_t src = bits[sy * LCD_SHADOW_STRIDE + (sx >> 1)];
            uint8_t idx = (sx & 1U) ? (src & 0x0FU) : (src >> 4);
            if ((col & 1U) == 0) {
                out_row[col >> 1] = (uint8_t)(idx << 4);
            } else {
                out_row[col >> 1] |= idx;
            }
        }
        crc = crc32_update(crc, out_row, row_len);
    }

    usb_debug_printf("SCREENBIN x=%lu y=%lu w=%lu h=%lu format=indexed4 len=%lu crc32=%08lX\r\n",
                     x, y, w, h, len, crc);

    for (uint32_t row = 0; row < h; row++) {
        uint32_t sy = y + row;
        memset(out_row, 0, row_len);
        for (uint32_t col = 0; col < w; col++) {
            uint32_t sx = x + col;
            uint8_t src = bits[sy * LCD_SHADOW_STRIDE + (sx >> 1)];
            uint8_t idx = (sx & 1U) ? (src & 0x0FU) : (src >> 4);
            if ((col & 1U) == 0) {
                out_row[col >> 1] = (uint8_t)(idx << 4);
            } else {
                out_row[col >> 1] |= idx;
            }
        }
        usb_send_bytes(out_row, (uint16_t)row_len);
    }

    usb_send_str("\r\nSCREENBIN END\r\n");
}

/* ---- screenshot: the on-device gallery over the shell ----
 * Same store as SAVE / long SAVE (gallery_store.h: 9 slots in our W25Q
 * scratch region, never stock's FAT). `get` frames a raw BMP the way
 * `screen dumpbin` frames its bytes: a header line with len + the CRC32 the
 * save recorded, the raw file, then an END line with the CRC recomputed from
 * what was actually sent, so a host can check both. */
static const char *gallery_status_str(gallery_status_t st)
{
    switch (st) {
    case GALLERY_OK:            return "ok";
    case GALLERY_ERR_NOT_READY: return "flash region layer not ready";
    case GALLERY_ERR_FULL:      return "gallery full (delete one)";
    case GALLERY_ERR_IO:        return "flash io error or busy";
    case GALLERY_ERR_NOT_FOUND: return "no such screenshot";
    case GALLERY_ERR_BAD_ARG:   return "bad argument";
    }
    return "?";
}

/* `screenshot list | save | get <n> | delete <n> | check <n>`. No argument
 * lists. `check` compares the stored CRC with one recomputed from flash. */
void cmd_screenshot(const char *args)
{
    static const char usage[] =
        "Usage: screenshot list | save | get <n> | delete <n> | check <n>\r\n";
    char name[16];
    uint32_t seq = 0, slot = 0;
    gallery_status_t st;

    if (args == NULL || *args == '\0' || strcmp(args, "list") == 0) {
        gallery_index_t idx;
        st = gallery_scan(&idx);
        if (st != GALLERY_OK) { usb_debug_printf("screenshot: %s\r\n", gallery_status_str(st)); return; }
        const flash_region_t *r = flash_region_get(GALLERY_REGION);
        usb_debug_printf("SHOTS count=%u slots=%u next=%lu region=%s 0x%06lX+0x%lX slot=0x%lX\r\n",
                         (unsigned)idx.count, (unsigned)idx.slots_total, (unsigned long)idx.next_seq,
                         r->name, (unsigned long)r->start, (unsigned long)r->length,
                         (unsigned long)GALLERY_SLOT_SIZE);
        for (uint32_t i = 0; i < idx.count; i++) {
            gallery_slot_info_t info;
            (void)gallery_slot_info(idx.slot[i], &info);
            gallery_name(idx.seq[i], name, sizeof name);
            usb_debug_printf("  %-14s slot %u @0x%06lX len=%lu crc32=%08lX\r\n", name,
                             (unsigned)idx.slot[i],
                             (unsigned long)(r->start + idx.slot[i] * GALLERY_SLOT_SIZE + GALLERY_BMP_OFFSET),
                             (unsigned long)info.bmp_len, (unsigned long)info.bmp_crc);
        }
        return;
    }
    if (strcmp(args, "save") == 0) {
        st = gallery_ui_capture(&seq);
        if (st == GALLERY_OK) {
            gallery_name(seq, name, sizeof name);
            usb_debug_printf("screenshot: saved %s\r\n", name);
        } else {
            usb_debug_printf("screenshot: save failed: %s\r\n", gallery_status_str(st));
        }
        return;
    }

    const bool is_get = strncmp(args, "get ", 4) == 0;
    const bool is_del = strncmp(args, "delete ", 7) == 0;
    const bool is_chk = strncmp(args, "check ", 6) == 0;
    if (!is_get && !is_del && !is_chk) { usb_send_str(usage); return; }
    const char *arg = args + (is_get ? 4 : is_del ? 7 : 6);
    while (*arg == ' ') arg++;
    if (!gallery_parse_name(arg, &seq)) { usb_send_str(usage); return; }
    gallery_name(seq, name, sizeof name);

    if (is_del) {
        st = gallery_delete(seq);
        usb_debug_printf("screenshot: delete %s: %s\r\n", name, gallery_status_str(st));
        return;
    }
    st = gallery_find(seq, &slot);
    if (st != GALLERY_OK) { usb_debug_printf("screenshot: %s: %s\r\n", name, gallery_status_str(st)); return; }
    gallery_slot_info_t info;
    (void)gallery_slot_info(slot, &info);
    if (is_chk) {
        uint32_t crc = 0;
        st = gallery_crc_bmp(slot, &crc);
        usb_debug_printf("screenshot: %s stored=%08lX read=%08lX %s\r\n", name,
                         (unsigned long)info.bmp_crc, (unsigned long)crc,
                         (st == GALLERY_OK && crc == info.bmp_crc) ? "OK" : "MISMATCH");
        return;
    }
    usb_debug_printf("SHOTBIN name=%s len=%lu crc32=%08lX\r\n", name,
                     (unsigned long)info.bmp_len, (unsigned long)info.bmp_crc);
    uint8_t buf[64];
    uint32_t crc = CAL_BACKUP_CRC32_INIT;
    for (uint32_t off = 0; off < GALLERY_BMP_LEN; off += sizeof buf) {
        uint32_t n = GALLERY_BMP_LEN - off;
        if (n > sizeof buf) n = sizeof buf;
        if (gallery_read_bmp(slot, off, buf, n) != GALLERY_OK) memset(buf, 0, n);
        crc = cal_backup_crc32_update(crc, buf, n);
        usb_send_bytes(buf, (uint16_t)n);
    }
    usb_debug_printf("\r\nSHOTBIN END crc32=%08lX\r\n", (unsigned long)cal_backup_crc32_final(crc));
}

/*
 * Boot logo (logo_store.h): upload a 320x240 RGB565 image over the shell.
 *   logo put <len> <crc32hex>        erase the slot, start an upload (len = 153600)
 *   logo data <off> <hex>            up to LOGO_CHUNK_MAX bytes at the next expected offset
 *   logo blk <off> <len> <crc32hex>  up to LOGO_BLK_MAX raw bytes, CRC-checked
 *   logo pos                         the next expected offset
 *   logo end                         verify CRC from flash, commit
 *   logo info | show [ms] | clear    state, a preview, erase
 * scripts/logo_put.py drives this; every reply starts "logo:" and a failure
 * says "err". The upload session lives in static RAM.
 */
logo_upload_t s_logo_up;

/* Raw block receive for `logo blk` (raw_block.h): armed by the command,
 * fed by the shell task loop before the line editor sees anything. The
 * block lands in shell_bus_scratch, which fw_loader and some spi3 and
 * interleave commands also borrow; all run on the shell task, never nested. */
raw_block_t s_raw;


#define LOGO_BLK_MAX 1024u   /* largest `logo blk` block, bytes */

/* A raw block finished (complete, CRC failed or timed out): on success write
 * it to the upload in LOGO_CHUNK_MAX pieces; answer either way, followed by
 * the prompt. */
void logo_blk_finish(void)
{
    const raw_block_state_t st = s_raw.state;
    const uint32_t off = s_raw.tag, n = s_raw.want;
    raw_block_reset(&s_raw);
    if (st == RAW_TIMEOUT) {
        usb_debug_printf("logo: err timeout (block @%lu: %lu of %lu bytes; expect %lu)\r\n> ",
                         (unsigned long)off, (unsigned long)s_raw.got, (unsigned long)n,
                         (unsigned long)s_logo_up.received);
        return;
    }
    if (st == RAW_DONE_CRC) {
        usb_debug_printf("logo: err crc (block @%lu; expect %lu)\r\n> ",
                         (unsigned long)off, (unsigned long)s_logo_up.received);
        return;
    }
    logo_status_t ls = LOGO_OK;
    for (uint32_t k = 0; k < n && ls == LOGO_OK; k += LOGO_CHUNK_MAX) {
        const uint32_t m = (n - k) < LOGO_CHUNK_MAX ? (n - k) : LOGO_CHUNK_MAX;
        ls = logo_put_data(&s_logo_up, off + k, shell_bus_scratch + k, m);
    }
    if (ls == LOGO_OK) usb_debug_printf("logo: ok %lu\r\n> ", (unsigned long)s_logo_up.received);
    else usb_debug_printf("logo: err %s (expect %lu)\r\n> ", logo_status_str(ls),
                          (unsigned long)s_logo_up.received);
}

/* Called by the logo store during long flash work: keep the shell task's
 * health check-in alive and yield. */
static void logo_tick(void)
{
    health_checkin_self();
    vTaskDelay(1);
}

/* Two hex digits at p as a byte, or -1 if either is not a hex digit. */
static int logo_hexbyte(const char *p)
{
    int v = 0;
    for (int i = 0; i < 2; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

void cmd_logo(const char *args)
{
    static const char usage[] =
        "Usage: logo info | put <len> <crc32hex> | blk <off> <len> <crc32hex> | data <off> <hex> | pos | end | show | clear\r\n";
    if (args == NULL || *args == '\0' || strcmp(args, "info") == 0) {
        logo_info_t info;
        logo_status_t st = logo_info(&info);
        if (st != LOGO_OK) { usb_debug_printf("logo: err %s\r\n", logo_status_str(st)); return; }
        const flash_region_t *r = flash_region_get(LOGO_REGION);
        if (info.state == LOGO_SLOT_VALID) {
            uint32_t crc = 0;
            st = logo_crc_payload(&crc, logo_tick);
            usb_debug_printf("logo: valid %lux%lu rgb565 %lu B crc %08lX payload %s @0x%06lX\r\n",
                             (unsigned long)LOGO_W, (unsigned long)LOGO_H, (unsigned long)info.len,
                             (unsigned long)info.crc,
                             (st == LOGO_OK && crc == info.crc) ? "OK" : "MISMATCH",
                             (unsigned long)(r ? r->start : 0));
        } else {
            usb_debug_printf("logo: %s (built-in splash)\r\n",
                             info.state == LOGO_SLOT_EMPTY ? "none stored" : "damaged slot");
        }
        return;
    }
    if (strncmp(args, "put ", 4) == 0) {
        uint32_t len, crc;
        const char *p = args + 4;
        char *e;
        len = strtoul(p, &e, 10);
        if (e == p || *e != ' ' || parse_hex32(e + 1, &crc) != 0) { usb_send_str(usage); return; }
        logo_status_t st = logo_put_begin(&s_logo_up, len, crc, logo_tick);
        if (st == LOGO_OK) usb_debug_printf("logo: ok put %lu\r\n", (unsigned long)len);
        else usb_debug_printf("logo: err %s\r\n", logo_status_str(st));
        return;
    }
    if (strncmp(args, "data ", 5) == 0) {
        uint32_t off;
        const char *p = args + 5;
        char *e;
        off = strtoul(p, &e, 10);
        if (e == p || *e != ' ') { usb_send_str(usage); return; }
        p = e + 1;
        uint8_t buf[LOGO_CHUNK_MAX];
        uint32_t n = 0;
        while (p[0] != '\0' && p[1] != '\0') {
            int b = logo_hexbyte(p);
            if (b < 0 || n >= sizeof buf) { usb_send_str("logo: err bad hex\r\n"); return; }
            buf[n++] = (uint8_t)b;
            p += 2;
        }
        if (n == 0u || *p != '\0') { usb_send_str("logo: err bad hex\r\n"); return; }
        logo_status_t st = logo_put_data(&s_logo_up, off, buf, n);
        if (st == LOGO_OK) usb_debug_printf("logo: ok %lu\r\n", (unsigned long)s_logo_up.received);
        else usb_debug_printf("logo: err %s (expect %lu)\r\n", logo_status_str(st),
                              (unsigned long)s_logo_up.received);
        return;
    }
    if (strncmp(args, "blk ", 4) == 0) {
        /* logo blk <off> <len> <crc32hex>: the next <len> raw bytes on this
         * transport are pixel data for <off>; answered "logo: ready", then
         * "logo: ok <received>" once they are in, CRC-checked and written. */
        uint32_t off, n, crc;
        const char *p = args + 4;
        char *e;
        off = strtoul(p, &e, 10);
        if (e == p || *e != ' ') { usb_send_str(usage); return; }
        p = e + 1;
        n = strtoul(p, &e, 10);
        if (e == p || *e != ' ' || parse_hex32(e + 1, &crc) != 0) { usb_send_str(usage); return; }
        if (!s_logo_up.active) { usb_send_str("logo: err no upload in progress (logo put first)\r\n"); return; }
        if (off != s_logo_up.received) {
            usb_debug_printf("logo: err sequence (expect %lu)\r\n", (unsigned long)s_logo_up.received);
            return;
        }
        if (n == 0u || n > LOGO_BLK_MAX || n > LOGO_PIXEL_BYTES - off) {
            usb_debug_printf("logo: err bad length (1..%u)\r\n", (unsigned)LOGO_BLK_MAX);
            return;
        }
        (void)raw_block_begin(&s_raw, shell_bus_scratch, LOGO_BLK_MAX, n, crc, off, shell_now_ms());
        usb_send_str("logo: ready\r\n");
        return;
    }
    if (strcmp(args, "pos") == 0) {
        usb_debug_printf("logo: pos %lu %s\r\n", (unsigned long)s_logo_up.received,
                         s_logo_up.active ? "active" : "idle");
        return;
    }
    if (strcmp(args, "end") == 0) {
        logo_status_t st = logo_put_end(&s_logo_up, logo_tick);
        usb_debug_printf(st == LOGO_OK ? "logo: ok committed\r\n" : "logo: err %s\r\n",
                         logo_status_str(st));
        return;
    }
    if (strcmp(args, "clear") == 0) {
        logo_status_t st = logo_clear();
        usb_debug_printf("logo: %s\r\n", st == LOGO_OK ? "ok cleared" : logo_status_str(st));
        return;
    }
    if (strncmp(args, "show", 4) == 0) {
        uint32_t ms = 2000u;
        if (args[4] == ' ') parse_int(args + 5, &ms);
        if (ms > 10000u) ms = 10000u;
        logo_status_t st = logo_ui_preview(ms);
        usb_debug_printf("logo: %s\r\n", st == LOGO_OK ? "ok shown" : logo_status_str(st));
        return;
    }
    usb_send_str(usage);
}

/* `screen shadow [page [y]]`: print the shadow's page origin and height, or
 * set the origin (y, default 0) first. */
void cmd_screen_shadow(const char *args)
{
    if (args == NULL || *args == '\0') {
        usb_debug_printf("shadow_page_y=%u height=%u\r\n",
                         (unsigned)lcd_shadow_page_y(),
                         (unsigned)LCD_SHADOW_HEIGHT);
        return;
    }

    if (strncmp(args, "page", 4) == 0 &&
        (args[4] == '\0' || args[4] == ' ' || args[4] == '\t')) {
        const char *value = args + 4;
        uint32_t y = 0;
        while (*value == ' ' || *value == '\t') value++;
        if (*value != '\0' && parse_int(value, &y) != 0) {
            usb_send_str("Usage: screen shadow page [y]\r\n");
            return;
        }
        lcd_shadow_set_page((uint16_t)y);
        usb_debug_printf("shadow_page_y=%u height=%u\r\n",
                         (unsigned)lcd_shadow_page_y(),
                         (unsigned)LCD_SHADOW_HEIGHT);
        return;
    }

    usb_send_str("Usage: screen shadow page [y]\r\n");
}

/* `ui dump`: the UI state and its counters: mode and settings selection,
 * scope draw counts and timings, the FFT pool and engine, the
 * meter screen and autoselect, the measurement strip, the graticule per
 * channel and the long-press timing. */
void cmd_ui_dump(void)
{
    meter_autoselect_status_t auto_st;
    meter_autoselect_get_status(&auto_st);

    usb_debug_printf("mode=%lu startup=%s meter_submode=%u (%s) layout=%u (%s) "
                     "settings_depth=%d selected=%d sub_selected=%d\r\n",
                     (uint32_t)current_mode,
                     startup_mode_name(startup_mode),
                     (unsigned)meter_submode,
                     meter_submode_name(meter_submode),
                     (unsigned)meter_layout,
                     meter_layout_name(meter_layout),
                     (int)settings_screen_view().page,
                     (int)settings_screen_view().row,
                     (int)settings_screen_view().sub);
    {
        extern volatile uint32_t ui_scope_full_draws, ui_scope_live_draws, ui_redraw_all_count;
        usb_debug_printf("draws scope_full=%lu scope_live=%lu redraw_all=%lu\r\n",
                         (unsigned long)ui_scope_full_draws, (unsigned long)ui_scope_live_draws,
                         (unsigned long)ui_redraw_all_count);
        {
            extern volatile uint32_t ui_scope_live_us_last, ui_scope_live_us_max, ui_scope_view_us_last;
            usb_debug_printf("scope live frame: last %lu us (view_build %lu us), longest %lu us\r\n",
                             (unsigned long)ui_scope_live_us_last, (unsigned long)ui_scope_view_us_last,
                             (unsigned long)ui_scope_live_us_max);
            extern volatile uint16_t ui_scope_live_cols_sent, ui_scope_live_cols_skipped;
            extern volatile uint32_t ui_scope_live_send_us, ui_scope_live_px_sent;
            extern volatile uint32_t lcd_perf_window_cycles, lcd_perf_palette_searches;
            usb_debug_printf("lcd column writes since boot: window set-up %lu ms, palette searches %lu\r\n",
                             (unsigned long)(lcd_perf_window_cycles / (system_core_clock / 1000u)),
                             (unsigned long)lcd_perf_palette_searches);
            usb_debug_printf("scope live frame columns: %u sent, %u unchanged and skipped; %lu pixels; "
                             "sending took %lu us\r\n",
                             (unsigned)ui_scope_live_cols_sent, (unsigned)ui_scope_live_cols_skipped,
                             (unsigned long)ui_scope_live_px_sent, (unsigned long)ui_scope_live_send_us);
            typedef struct { uint32_t n, last_us, max_us; } ui_perf_t;
            extern volatile ui_perf_t ui_perf[];
            extern const char *const ui_perf_name[];
            extern const uint8_t ui_perf_count;
            for (uint8_t i = 0; i < ui_perf_count; i++)
                usb_debug_printf("scope %s: %lu times, last %lu us, longest %lu us\r\n", ui_perf_name[i],
                                 (unsigned long)ui_perf[i].n, (unsigned long)ui_perf[i].last_us,
                                 (unsigned long)ui_perf[i].max_us);
        }
        usb_debug_printf("lcd foreign_calls=%lu\r\n", (unsigned long)lcd_foreign_calls);
        static const char *const wfs[] = { "never drawn", "drawn", "no FFT", "waiting for capture",
                                           "pool not FFT's" };
        const uint8_t w = scope_ui_waterfall_status();
        usb_debug_printf("pool size=%lu owner=%s transitions=%lu | waterfall rows %u x %u at %lu..%lu "
                         "(%s), last frame: %s, newest row %u\r\n",
                         (unsigned long)shared_mem_size(), shared_mem_owner_name(),
                         (unsigned long)shared_mem_transition_count(),
                         (unsigned)SHMEM_FFT_WATERFALL_ROWS, (unsigned)SHMEM_FFT_WATERFALL_COLS,
                         (unsigned long)SHMEM_FFT_SLOT_OFFSET,
                         (unsigned long)(SHMEM_FFT_SLOT_OFFSET + SHMEM_FFT_SLOT_SIZE),
                         (SHMEM_FFT_SLOT_OFFSET + SHMEM_FFT_SLOT_SIZE <= shared_mem_size())
                             ? "fits" : "DOES NOT FIT",
                         (w < 5u) ? wfs[w] : "?", (unsigned)scope_ui_waterfall_row());
        /* The transform's own setup (the CMSIS tables are selected at build
         * time for the one size): a refusal must be seen, not drawn. */
        usb_debug_printf("fft engine: %s; last live spectrum from %u record samples\r\n",
                         !fft_is_initialized() ? "not initialized (enter the FFT view)"
                         : fft_engine_ok() ? "ok" : "INIT FAILED (no tables for this FFT size)",
                         (unsigned)scope_ui_fft_last_n());
    }
    usb_debug_printf("meter_ui draws=%lu full_clears=%lu partial_clears=%lu draw_us=%lu max_draw_us=%lu over_budget=%lu rendered_update=%lu last_full=%u live=%u continuity_flash=%u reading_valid=%u "
                     "reading_submode=%u updates=%lu display_updates=%lu\r\n",
                     meter_screen_draw_count,
                     meter_screen_full_clear_count,
                     meter_screen_partial_clear_count,
                     meter_screen_last_draw_us,
                     meter_screen_max_draw_us,
                     meter_screen_over_budget_count,
                     meter_screen_last_reading_display_update,
                     (unsigned)meter_screen_last_full_clear,
                     (unsigned)meter_screen_last_live,
                     (unsigned)meter_screen_last_continuity_flash,
                     meter_reading.valid ? 1U : 0U,
                     (unsigned)meter_reading.submode,
                     meter_reading.update_count,
                     meter_reading.display_update_count);
    usb_debug_printf("autoselect state=%s current=%u (%s) index=%u/%u best=%u (%s) best_score=%u last_score=%u cancel=%u\r\n",
                     meter_autoselect_state_name(auto_st.state),
                     (unsigned)auto_st.current_submode,
                     meter_submode_name(auto_st.current_submode),
                     (unsigned)auto_st.current_index,
                     (unsigned)auto_st.candidate_count,
                     (unsigned)auto_st.best_submode,
                     meter_submode_name(auto_st.best_submode),
                     (unsigned)auto_st.best_score,
                     (unsigned)auto_st.last_score,
                     auto_st.cancel_pending ? 1U : 0U);
    /* What the strip and the graticule do with the properties (chN.meas,
     * view.meashide, view.truescale), and the key timing. */
    prm_print_status();
    {
        const scope_state_t *ss = scope_state_get();
        usb_debug_printf("graticule: %s\r\n",
                         ss->true_scale ? "TRUE SCALE (grid == volts/div)"
                                        : "autofit (grid == position only)");
        for (uint8_t ch = 1u; ch <= 2u; ch++) {
            const uint8_t r = (ch == 1u) ? ss->ch1.vdiv_idx : ss->ch2.vdiv_idx;
            char l[12];
            scope_cal_range_label(ch, r, l, sizeof(l));
            usb_debug_printf("  CH%u rng %u -> %s/div %s\r\n", (unsigned)ch, (unsigned)r, l,
                             scope_cal_true_scale_ok(ch, r) ? "" : "(no cal -> autofit)");
        }
    }
    usb_debug_printf("long press %u ms (%u ticks), short on release for the long-capable keys\r\n",
                     (unsigned)(KEY_LONG_TICKS * 2u), (unsigned)KEY_LONG_TICKS);
}
