/*
 * logo_ui.c — boot logo drawing (see logo_store.h). No static buffers: the
 * image streams from the W25Q through a 128-byte stack buffer into one LCD
 * window, so a logo costs no RAM at all.
 *
 * The logo is a layer of the screen router (ui/overlay_screens.h, SCR_LOGO):
 * while it is up the display task draws nothing else. Who paints it:
 *   boot:    main(), before the scheduler runs (logo_ui_boot_show), with the
 *            backlight off, then on (stock: 0x0802B420..0x0802C124); the key
 *            task holds it (logo_ui_boot_hold), any key skips.
 *   preview: the display task (logo_ui_paint), for the shell's `logo show`,
 *            which only raises the layer and waits (logo_ui_preview).
 */
#include "logo_ui.h"
#include "overlay_screens.h"
#include "input_handler.h"     /* input_call_sync: the layer is the input task's */

#include "ui.h"
#include "lcd.h"
#include "sys_pwm.h"
#include "task.h"
#include "queue.h"

static void sink(const uint16_t *px, uint32_t count, void *ctx)
{
    (void)ctx;
    lcd_write_pixels(px, count);
}

logo_status_t logo_ui_draw(void)
{
    logo_info_t info;
    logo_status_t st = logo_info(&info);
    if (st != LOGO_OK) return st;
    if (info.state != LOGO_SLOT_VALID) return LOGO_ERR_NONE;
    lcd_set_window(0, 0, LOGO_W, LOGO_H);
    return logo_stream(sink, NULL);
}

static bool boot_pending;

bool logo_ui_boot_show(void)
{
    logo_info_t info;
    if (logo_info(&info) != LOGO_OK || info.state != LOGO_SLOT_VALID) return false;
    sys_pwm_backlight_off();           /* no flash of the splash while the logo paints */
    const logo_status_t st = logo_ui_draw();
    sys_pwm_apply_brightness();
    if (st != LOGO_OK) return false;   /* partial draw: the display task repaints */
    boot_pending = true;               /* its layer is pushed by the key task (logo_ui_boot_hold) */
    return true;
}

void logo_ui_boot_hold(QueueHandle_t keys, uint32_t hold_ms)
{
    if (!boot_pending) return;
    boot_pending = false;
    (void)logo_screen_show(true);      /* it is on the LCD: the display task leaves it there */
    for (uint32_t t = 0; t < hold_ms; t += 20u) {
        button_id_t key;
        if (xQueueReceive(keys, &key, pdMS_TO_TICKS(20)) == pdTRUE) break;   /* key = skip */
    }
    logo_screen_hide();                /* the display task repaints what is under it */
}

/* The preview's result, from the display task's paint to the shell. */
static volatile logo_status_t paint_status;

void logo_ui_paint(void)
{
    paint_status = logo_ui_draw();
    logo_screen_mark_painted();
}

/* `logo show` runs on the shell task; the layer is the input task's to push
 * and close (input_call_sync). */
static void preview_show(void *ok) { *(bool *)ok = logo_screen_show(false); }
static void preview_hide(void *unused) { (void)unused; logo_screen_hide(); }

static void preview_drop(void)
{
    /* The input task may be inside a POWER countdown (3.6 s): keep asking. */
    for (int i = 0; i < 5 && !input_call_sync(preview_hide, NULL, 2000u); i++) { }
}

logo_status_t logo_ui_preview(uint32_t hold_ms)
{
    logo_info_t info;
    logo_status_t st = logo_info(&info);
    if (st != LOGO_OK) return st;
    if (info.state != LOGO_SLOT_VALID) return LOGO_ERR_NONE;
    if (logo_screen_up()) return LOGO_ERR_IO;             /* one at a time */
    bool shown = false;
    if (!input_call_sync(preview_show, &shown, 1000u) || !shown)
        return LOGO_ERR_IO;                               /* the key task is busy, or the stack is full */
    /* Until the display task has painted it (it finishes the frame it is in
     * first); 2 s is far more than a frame and the stream. */
    for (int i = 0; i < 200 && !logo_screen_painted(); i++)
        vTaskDelay(pdMS_TO_TICKS(10));
    if (!logo_screen_painted()) {
        preview_drop();
        return LOGO_ERR_IO;
    }
    st = paint_status;
    vTaskDelay(pdMS_TO_TICKS(hold_ms));
    preview_drop();
    return st;
}
