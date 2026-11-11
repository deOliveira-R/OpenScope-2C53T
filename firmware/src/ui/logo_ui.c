/*
 * logo_ui.c — boot logo drawing (see logo_store.h). No static buffers: the
 * image streams from the W25Q through a 128-byte stack buffer into one LCD
 * window, so a logo costs no RAM at all.
 *
 * Boot sequence (stock: 0x0802B420..0x0802C124): backlight off, draw,
 * backlight to the stored brightness, hold. Ours splits it in two because the
 * key matrix is only scanned once the key task has started:
 *   logo_ui_boot_show()  from main(), before the tasks: draws, claims the screen
 *   logo_ui_boot_hold()  first thing in the key task: waits, skippable, releases
 */
#include "logo_ui.h"

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
static QueueHandle_t s_dq;   /* display queue, remembered for the shell preview */

bool logo_ui_boot_show(void)
{
    logo_info_t info;
    if (logo_info(&info) != LOGO_OK || info.state != LOGO_SLOT_VALID) return false;
    sys_pwm_backlight_off();           /* no flash of the splash while the logo paints */
    const logo_status_t st = logo_ui_draw();
    sys_pwm_apply_brightness();
    if (st != LOGO_OK) return false;   /* partial draw: the display task repaints */
    ui_modal_active = true;            /* the display task idles until the hold ends */
    boot_pending = true;
    return true;
}

void logo_ui_boot_hold(QueueHandle_t keys, QueueHandle_t dq, uint32_t hold_ms)
{
    s_dq = dq;
    if (!boot_pending) return;
    boot_pending = false;
    for (uint32_t t = 0; t < hold_ms; t += 20u) {
        button_id_t key;
        if (xQueueReceive(keys, &key, pdMS_TO_TICKS(20)) == pdTRUE) break;   /* key = skip */
    }
    ui_modal_active = false;
    const uint8_t cmd = DCMD_REDRAW_ALL;
    xQueueSend(dq, &cmd, 0);
}

logo_status_t logo_ui_preview(uint32_t hold_ms)
{
    logo_info_t info;
    logo_status_t st = logo_info(&info);
    if (st != LOGO_OK) return st;
    if (info.state != LOGO_SLOT_VALID) return LOGO_ERR_NONE;
    const bool was = ui_modal_active;
    ui_modal_active = true;
    const uint32_t c0 = ui_modal_idle_count;   /* wait until the display task saw the claim */
    for (int i = 0; i < 50 && ui_modal_idle_count == c0; i++)
        vTaskDelay(pdMS_TO_TICKS(10));
    st = logo_ui_draw();
    vTaskDelay(pdMS_TO_TICKS(hold_ms));
    if (!was) ui_modal_active = false;
    const uint8_t cmd = DCMD_REDRAW_ALL;
    if (s_dq) xQueueSend(s_dq, &cmd, 0);
    return st;
}
