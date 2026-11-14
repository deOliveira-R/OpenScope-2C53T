/*
 * gallery_ui.c — screenshot capture and the paint of the 3x3 gallery
 * (parity C16).
 *
 * Stock V1.2.0: SAVE writes `2:/Screenshot file/%d.bmp` (string 0x080BC1B4)
 * with the "Saving..."/"Saved"/"Save failed" popups (ms[0xE10] = 1/2/3,
 * 0x0801C7B0..0x0801CA08); long SAVE opens the "nine palace grid" gallery with
 * delete (manual p. 31). Ours keeps the keys and the grid, stores into our own
 * scratch region instead of stock's FAT (gallery_store.h says why).
 *
 * EVERYTHING HERE THAT TOUCHES THE LCD RUNS ON THE DISPLAY TASK (ARCH-1 B5).
 * The gallery is a screen of the router (gallery_screen.c: its state and its
 * keys, on the input task); gallery_ui_paint() is its draw(). A capture is a
 * display command (DCMD_CAPTURE): the display task saves the LCD shadow
 * between two of its own frames, so the shot is one whole frame with no
 * claim and no handshake, and the caller only waits for the result.
 *
 * RAM: no static buffers. Rows are read from the W25Q into small stack
 * buffers and pushed to the LCD; what is kept between paints is the view
 * last drawn (8 bytes). The deepest path here is ~400 B of the display
 * task's stack (768 words).
 */
#include "gallery_ui.h"
#include "gallery_screen.h"
#include "gallery_nav.h"      /* GALLERY_NAV_COLS */

#include "ui.h"
#include "lcd.h"
#include "font.h"
#include "theme.h"
#include "watchdog.h"
#include "task.h"

#include <string.h>
#include <stdio.h>

/* ── Capture (short SAVE, the shell's `screenshot save`) ──────────────── */

static QueueHandle_t      cap_dq;          /* the display queue (gallery_ui_bind) */
static volatile bool      capture_busy;    /* one at a time                        */
static volatile uint32_t  cap_req, cap_done;
static volatile gallery_status_t cap_status;
static volatile uint32_t  cap_seq;

void gallery_ui_bind(QueueHandle_t display_queue) { cap_dq = display_queue; }

/* Between flash operations: check in with the health monitor and yield, so
 * the other tasks run while this one busy-polls the W25Q. A sector erase is
 * up to 400 ms; ten in a row would otherwise run past HEALTH_DEADLINE_MS. */
static void save_tick(void)
{
    health_checkin_self();
    vTaskDelay(1);
}

/* Display task, on DCMD_CAPTURE: the LCD shadow as it stands, no frame in
 * flight (this task is the drawer). */
void gallery_ui_capture_run(void)
{
    if (cap_done == cap_req) return;           /* no request behind this command */
    const uint32_t req = cap_req;
    uint32_t seq = 0;
    cap_status = gallery_save(lcd_shadow_bits(), lcd_shadow_palette_get(), save_tick, &seq);
    cap_seq = seq;
    cap_done = req;
    capture_busy = false;
}

/* Any task but the display task: post the capture and wait for its result.
 * The screen is still for the ~0.5 s of the write, as it always was. */
gallery_status_t gallery_ui_capture(uint32_t *seq_out)
{
    taskENTER_CRITICAL();
    const bool busy = capture_busy;
    capture_busy = true;
    taskEXIT_CRITICAL();
    if (busy) return GALLERY_ERR_IO;

    const uint32_t req = ++cap_req;
    const uint8_t cmd = DCMD_CAPTURE;
    if (cap_dq == NULL || xQueueSend(cap_dq, &cmd, pdMS_TO_TICKS(500)) != pdTRUE) {
        cap_done = req;                        /* never posted: nothing will run */
        capture_busy = false;
        return GALLERY_ERR_IO;
    }
    /* A full slot is ten sector erases and ten sector programs: seconds at
     * the worst. The display task clears capture_busy when it is done, also
     * after this wait has given up. */
    for (uint32_t waited = 0; cap_done != req; waited += 10u) {
        if (waited >= 30000u) return GALLERY_ERR_IO;
        health_checkin_self();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (seq_out) *seq_out = cap_seq;
    return cap_status;
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

/* Through lcd_write_data(), which feeds the LCD's indexed shadow (so does
 * lcd_write_pixels() since 2026-11-13), so `screen dumpbin` /
 * scripts/screenshot.py (and a SAVE from the shell while the gallery is open)
 * see the gallery as drawn. The stored colours are the shadow's own palette,
 * so the shadow copy is exact. */
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
static uint8_t g_sel;        /* the selection being painted (the view's) */

static void draw_cell(const gallery_index_t *idx, uint32_t cell, bool with_thumb)
{
    const theme_t *th = theme_get();
    uint16_t x, y;
    cell_origin(cell, &x, &y);
    const uint16_t tx = (uint16_t)(x + THUMB_DX), ty = (uint16_t)(y + THUMB_DY);
    const bool sel = (cell == g_sel);
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
    if (g_sel >= idx->count) return;
    const uint32_t slot = idx->slot[g_sel];
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

static void draw_delete_prompt(const gallery_index_t *idx, bool full)
{
    const theme_t *th = theme_get();
    char name[16], t[40];
    if (g_sel >= idx->count) return;
    gallery_name(idx->seq[g_sel], name, sizeof name);
    snprintf(t, sizeof t, "SAVE again: delete %s", name);
    if (full) {
        lcd_fill_rect(0, LCD_HEIGHT - TITLE_H, LCD_WIDTH, TITLE_H, th->status_bar_bg);
        font_draw_string(4, LCD_HEIGHT - TITLE_H + 3u, t, th->warning, th->status_bar_bg, &font_small);
    } else {
        draw_title(idx, t);
    }
}

/* ── The paint (display task): the view against the one last drawn ─────── */

static gallery_view_t drawn;        /* the view on the LCD */
static bool           drawn_valid;

void gallery_ui_paint(scr_paint_t how)
{
    const gallery_view_t v = gallery_screen_view();     /* one look: the input task writes it */
    if (!v.open) { drawn_valid = false; return; }
    g_sel = v.sel;

    gallery_index_t idx;
    if (gallery_scan(&idx) != GALLERY_OK) {
        memset(&idx, 0, sizeof idx);
        lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, theme_get()->background);
        draw_title(&idx, "Gallery: flash not ready  (MENU exit)");
        drawn_valid = false;
        return;
    }

    const uint8_t plan = gallery_paint_plan(drawn_valid ? &drawn : NULL, &v, how);
    const char *const msg = (v.msg == GALLERY_MSG_DELETE_FAILED) ? "Delete failed" : NULL;
    if (plan & GALLERY_PAINT_ALL) {
        if (v.full) draw_full(&idx); else draw_grid(&idx);
    }
    if (plan & GALLERY_PAINT_CELLS) {
        draw_cell(&idx, drawn.sel, false);
        draw_cell(&idx, v.sel, false);
    }
    if (plan & GALLERY_PAINT_SHOT)   draw_full(&idx);
    if (plan & GALLERY_PAINT_PROMPT) draw_delete_prompt(&idx, v.full);
    if (plan & GALLERY_PAINT_TITLE)  draw_title(&idx, msg);
    drawn = v;
    drawn_valid = true;
}
