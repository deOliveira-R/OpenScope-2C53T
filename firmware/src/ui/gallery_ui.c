/*
 * gallery_ui.c — screenshot capture and the 3x3 gallery (parity C16).
 *
 * Stock V1.2.0: SAVE writes `2:/Screenshot file/%d.bmp` (string 0x080BC1B4)
 * with the "Saving..."/"Saved"/"Save failed" popups (ms[0xE10] = 1/2/3,
 * 0x0801C7B0..0x0801CA08); long SAVE opens the "nine palace grid" gallery with
 * delete (manual p. 31). Ours keeps the keys and the grid, stores into our own
 * scratch region instead of stock's FAT (gallery_store.h says why).
 *
 * RAM: no static buffers. Rows are read from the W25Q into small stack
 * buffers and pushed to the LCD; the only state kept between key presses is
 * the 4-byte nav struct and two flags. Runs in the key task (384-word stack):
 * the deepest path here is ~400 B.
 */
#include "gallery_ui.h"
#include "gallery_nav.h"

#include "ui.h"
#include "lcd.h"
#include "font.h"
#include "theme.h"
#include "watchdog.h"
#include "task.h"

#include <string.h>
#include <stdio.h>

static gallery_nav_t nav;
static bool          is_open;
static bool          modal_was_set;     /* modal claimed before we opened (do not drop it) */
static volatile bool capture_busy;

/* ── Screen claim ─────────────────────────────────────────────────────── */

/* Claim the screen and wait until the display task has SEEN the claim (it
 * counts ui_modal_idle_count only on its modal branch), so no frame is half
 * drawn when we read the shadow or paint over the LCD. The POWER countdown
 * uses a fixed 120 ms delay for the same purpose; a scope frame can take
 * longer than that, and for a screenshot a torn frame is the defect. */
static bool claim_screen(void)
{
    const bool was = ui_modal_active;
    ui_modal_active = true;
    const uint32_t c0 = ui_modal_idle_count;
    for (int i = 0; i < 50 && ui_modal_idle_count == c0; i++)
        vTaskDelay(pdMS_TO_TICKS(10));
    return was;
}

/* Between flash operations: check in with the health monitor and yield, so
 * the display task (priority 1) still checks in while the key task (4)
 * busy-polls the W25Q. A sector erase is up to 400 ms; ten in a row would
 * otherwise starve it past HEALTH_DEADLINE_MS. */
static void save_tick(void)
{
    health_checkin_self();
    vTaskDelay(1);
}

gallery_status_t gallery_ui_capture(uint32_t *seq_out)
{
    taskENTER_CRITICAL();
    const bool busy = capture_busy;
    capture_busy = true;
    taskEXIT_CRITICAL();
    if (busy) return GALLERY_ERR_IO;

    const bool was = claim_screen();
    gallery_status_t st = gallery_save(lcd_shadow_bits(), lcd_shadow_palette_get(), save_tick, seq_out);
    if (!was) ui_modal_active = false;
    capture_busy = false;
    return st;
}

void gallery_ui_result_text(gallery_status_t st, uint32_t seq, char *out, uint32_t out_size)
{
    char name[16];
    switch (st) {
    case GALLERY_OK:
        gallery_name(seq, name, sizeof name);
        snprintf(out, out_size, "Saved %s", name);
        break;
    case GALLERY_ERR_FULL:
        snprintf(out, out_size, "Gallery full: delete");
        break;
    case GALLERY_ERR_NOT_READY:
        snprintf(out, out_size, "Save failed: no flash");
        break;
    default:
        snprintf(out, out_size, "Save failed");
        break;
    }
}

/* ── Drawing ──────────────────────────────────────────────────────────── */

#define TITLE_H   18u
#define CELL_W    106u
#define CELL_H    74u
#define THUMB_W   80u
#define THUMB_H   60u
#define THUMB_DX  ((CELL_W - THUMB_W) / 2u)
#define THUMB_DY  2u

/* lcd_write_data(), not lcd_write_pixels(): it also feeds the LCD's indexed
 * shadow, so `screen dumpbin` / scripts/screenshot.py (and a SAVE from the
 * shell while the gallery is open) see the gallery as drawn. The stored
 * colours are the shadow's own palette, so the shadow copy is exact. */
static void push(const uint16_t *px, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) lcd_write_data(px[i]);
}

static uint8_t pix(const uint8_t *row, uint32_t x)
{
    const uint8_t b = row[x >> 1];
    return (x & 1u) ? (uint8_t)(b & 0x0Fu) : (uint8_t)(b >> 4);
}

static void cell_origin(uint32_t cell, uint16_t *x, uint16_t *y)
{
    *x = (uint16_t)((cell % GALLERY_NAV_COLS) * CELL_W);
    *y = (uint16_t)(TITLE_H + (cell / GALLERY_NAV_COLS) * CELL_H);
}

/* 1/4 nearest-neighbour thumbnail straight from flash: 60 row reads. Source
 * pixel x = 4*tx + 1 is the low nibble of row byte 2*tx, so the row is read
 * into the pixel buffer itself and expanded in place, front to back: step tx
 * reads byte 2*tx before writing bytes 2*tx..2*tx+1, and no later step needs
 * an earlier byte. Saves a 160-byte row buffer on the key task's stack. */
_Static_assert(THUMB_W * 2u == GALLERY_ROW_BYTES, "in-place expansion needs a 1/4 thumb");
static bool draw_thumb(uint32_t slot, uint16_t x0, uint16_t y0)
{
    uint16_t pal[16];
    uint16_t px[THUMB_W];
    uint8_t *raw = (uint8_t *)px;
    if (gallery_read_palette(slot, pal) != GALLERY_OK) return false;
    for (uint32_t ty = 0; ty < THUMB_H; ty++) {
        if (gallery_read_row(slot, ty * 4u + 1u, raw) != GALLERY_OK) return false;
        for (uint32_t tx = 0; tx < THUMB_W; tx++)
            px[tx] = pal[raw[2u * tx] & 0x0Fu];
        lcd_set_window(x0, (uint16_t)(y0 + ty), THUMB_W, 1);
        push(px, THUMB_W);
    }
    return true;
}

static void draw_frame(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t c)
{
    lcd_fill_rect(x, y, w, 2, c);
    lcd_fill_rect(x, (uint16_t)(y + h - 2u), w, 2, c);
    lcd_fill_rect(x, y, 2, h, c);
    lcd_fill_rect((uint16_t)(x + w - 2u), y, 2, h, c);
}

/* with_thumb = false repaints only the selection frame and the name, which is
 * all a cursor move changes. */
static void draw_cell(const gallery_index_t *idx, uint32_t cell, bool with_thumb)
{
    const theme_t *th = theme_get();
    uint16_t x, y;
    cell_origin(cell, &x, &y);
    const uint16_t tx = (uint16_t)(x + THUMB_DX), ty = (uint16_t)(y + THUMB_DY);
    const bool sel = (cell == nav.sel);
    draw_frame((uint16_t)(tx - 3u), (uint16_t)(ty - 2u), THUMB_W + 6u, THUMB_H + 4u,
               sel ? th->highlight : th->background);
    if (cell >= idx->count) {
        if (with_thumb) lcd_fill_rect(tx, ty, THUMB_W, THUMB_H, th->status_bar_bg);
        return;
    }
    if (with_thumb && !draw_thumb(idx->slot[cell], tx, ty)) {
        lcd_fill_rect(tx, ty, THUMB_W, THUMB_H, th->background);
        font_draw_string_center((uint16_t)(tx + THUMB_W / 2u), (uint16_t)(ty + 24u), "read error",
                                th->warning, th->background, &font_small);
    }
    char name[16];
    gallery_name(idx->seq[cell], name, sizeof name);
    font_draw_string_center((uint16_t)(x + CELL_W / 2u), (uint16_t)(ty + THUMB_H + 1u), name,
                            sel ? th->highlight : th->text_secondary, th->background, &font_small);
}

static void draw_title(const gallery_index_t *idx, const char *msg)
{
    const theme_t *th = theme_get();
    char t[40];
    lcd_fill_rect(0, 0, LCD_WIDTH, TITLE_H, th->status_bar_bg);
    if (msg) {
        font_draw_string(4, 3, msg, th->warning, th->status_bar_bg, &font_small);
        return;
    }
    snprintf(t, sizeof t, "Gallery  %u/%u", (unsigned)idx->count, (unsigned)idx->slots_total);
    font_draw_string(4, 3, t, th->text_primary, th->status_bar_bg, &font_small);
    font_draw_string_right(LCD_WIDTH - 4u, 3, "OK view  SAVE del  MENU exit",
                           th->text_secondary, th->status_bar_bg, &font_small);
}

static void draw_grid(const gallery_index_t *idx)
{
    const theme_t *th = theme_get();
    lcd_fill_rect(0, TITLE_H, LCD_WIDTH, LCD_HEIGHT - TITLE_H, th->background);
    draw_title(idx, NULL);
    if (idx->count == 0u) {
        font_draw_string_center(LCD_WIDTH / 2u, 110, "No screenshots: SAVE takes one",
                                th->text_secondary, th->background, &font_medium);
        return;
    }
    for (uint32_t c = 0; c < GALLERY_MAX_SLOTS && c < idx->slots_total; c++)
        draw_cell(idx, c, true);
}

static void draw_full(const gallery_index_t *idx)
{
    const theme_t *th = theme_get();
    if (nav.sel >= idx->count) return;
    const uint32_t slot = idx->slot[nav.sel];
    uint16_t pal[16];
    uint8_t  row[GALLERY_ROW_BYTES];
    uint16_t px[64];
    if (gallery_read_palette(slot, pal) != GALLERY_OK) goto fail;
    lcd_set_window(0, 0, LCD_WIDTH, LCD_HEIGHT);
    for (uint32_t y = 0; y < GALLERY_IMG_H; y++) {
        if (gallery_read_row(slot, y, row) != GALLERY_OK) goto fail;
        for (uint32_t x0 = 0; x0 < GALLERY_IMG_W; x0 += 64u) {
            for (uint32_t i = 0; i < 64u; i++) px[i] = pal[pix(row, x0 + i)];
            push(px, 64u);
        }
    }
    return;
fail:
    lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, th->background);
    font_draw_string_center(LCD_WIDTH / 2u, 110, "read error", th->warning, th->background, &font_medium);
}

static void draw_delete_prompt(const gallery_index_t *idx)
{
    const theme_t *th = theme_get();
    char name[16], t[40];
    if (nav.sel >= idx->count) return;
    gallery_name(idx->seq[nav.sel], name, sizeof name);
    snprintf(t, sizeof t, "SAVE again: delete %s", name);
    if (nav.full) {
        lcd_fill_rect(0, LCD_HEIGHT - TITLE_H, LCD_WIDTH, TITLE_H, th->status_bar_bg);
        font_draw_string(4, LCD_HEIGHT - TITLE_H + 3u, t, th->warning, th->status_bar_bg, &font_small);
    } else {
        draw_title(idx, t);
    }
}

/* ── Open / keys ──────────────────────────────────────────────────────── */

bool gallery_ui_is_open(void) { return is_open; }

static void close_gallery(QueueHandle_t dq)
{
    is_open = false;
    if (!modal_was_set) ui_modal_active = false;
    const uint8_t cmd = DCMD_REDRAW_ALL;
    xQueueSend(dq, &cmd, 0);
}

void gallery_ui_open(QueueHandle_t dq)
{
    (void)dq;
    if (is_open) return;
    gallery_nav_open(&nav);
    modal_was_set = claim_screen();
    is_open = true;
    gallery_index_t idx;
    if (gallery_scan(&idx) != GALLERY_OK) {
        memset(&idx, 0, sizeof idx);
        lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, theme_get()->background);
        draw_title(&idx, "Gallery: flash not ready  (MENU exit)");
        return;
    }
    draw_grid(&idx);
}

static gallery_nav_key_t map_key(uint8_t button)
{
    const bool lng = (button & BTN_LONG_FLAG) != 0u;
    switch ((button_id_t)(button & (uint8_t)~BTN_LONG_FLAG)) {
    case BTN_LEFT:  return GNK_LEFT;
    case BTN_RIGHT: return GNK_RIGHT;
    case BTN_UP:    return GNK_UP;
    case BTN_DOWN:  return GNK_DOWN;
    case BTN_OK:    return lng ? GNK_OTHER : GNK_OK;
    case BTN_SAVE:  return lng ? GNK_SAVE_LONG : GNK_SAVE;
    case BTN_MENU:  return GNK_MENU;
    default:        return GNK_OTHER;
    }
}

bool gallery_ui_key(uint8_t button, QueueHandle_t dq)
{
    if (!is_open) return false;
    if ((button & (uint8_t)~BTN_LONG_FLAG) == BTN_POWER) {
        close_gallery(dq);
        return false;
    }
    gallery_index_t idx;
    if (gallery_scan(&idx) != GALLERY_OK) {
        /* nothing to browse; any key leaves */
        close_gallery(dq);
        return true;
    }
    switch (gallery_nav_key(&nav, map_key(button), idx.count)) {
    case GNA_NONE:
        break;
    case GNA_MOVE:
        draw_cell(&idx, nav.prev_sel, false);
        draw_cell(&idx, nav.sel, false);
        break;
    case GNA_SHOW_FULL:
        draw_full(&idx);
        break;
    case GNA_SHOW_GRID:
        draw_grid(&idx);
        break;
    case GNA_ARM_DELETE:
        draw_delete_prompt(&idx);
        break;
    case GNA_DISARM:
        if (nav.full) draw_full(&idx); else draw_title(&idx, NULL);
        break;
    case GNA_DELETE: {
        const uint32_t seq = idx.seq[nav.sel];
        const gallery_status_t st = gallery_delete(seq);
        gallery_index_t after;
        if (gallery_scan(&after) != GALLERY_OK) { close_gallery(dq); break; }
        if (gallery_nav_deleted(&nav, after.count) == GNA_SHOW_FULL) draw_full(&after);
        else draw_grid(&after);
        if (st != GALLERY_OK && !nav.full) draw_title(&after, "Delete failed");
        break;
    }
    case GNA_CLOSE:
        close_gallery(dq);
        break;
    }
    return true;
}
