/*
 * logo_ui — draw the stored boot logo (logo_store.h) on the LCD.
 * Stock draws its logo with the backlight off, then raises the backlight
 * (0x0802C04A..0x0802C06C); boot_show() does the same and holds ~1 s.
 */
#ifndef LOGO_UI_H
#define LOGO_UI_H

#include <stdbool.h>
#include "FreeRTOS.h"
#include "queue.h"
#include "logo_store.h"

/* Paint the stored logo full screen. LOGO_ERR_NONE if none is stored. */
logo_status_t logo_ui_draw(void);

/* Boot, from main() before the tasks start (after sys_pwm_init): if a valid
 * logo is stored, backlight off, draw it, backlight on, claim the screen
 * (ui_modal_active). Returns true if shown. */
bool logo_ui_boot_show(void);

/* First thing in the key task, after button_scan_start(): hold the logo for
 * hold_ms (any key press ends the hold early, and is consumed), then release
 * the screen with a full redraw. No-op if boot_show() did not show a logo. */
void logo_ui_boot_hold(QueueHandle_t keys, QueueHandle_t dq, uint32_t hold_ms);

/* Shell `logo show`: take the screen from the display task, draw, hold
 * hold_ms, hand it back with a full redraw. */
logo_status_t logo_ui_preview(uint32_t hold_ms);

#endif
