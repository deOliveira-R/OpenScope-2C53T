/*
 * OpenScope 2C53T - the POWER prompt's paint (overlay_screens.h). Display
 * task only.
 *
 * The box is what the input task's POWER loop and the auto power-off drew
 * inline until ARCH-1 B5: the same rectangle, texts, fonts and colours.
 */
#include "overlay_screens.h"
#include "lcd.h"
#include "font.h"
#include "theme.h"

void power_prompt_ui_paint(power_prompt_view_t v)
{
    const theme_t *th = theme_get();
    const uint16_t bg = 0x0010;  /* dark background */

    lcd_fill_rect(60, 80, 200, 80, bg);
    switch ((power_prompt_kind_t)v.kind) {
    case POWER_PROMPT_HOLD: {
        const char digit[2] = { (char)('0' + v.count), '\0' };
        font_draw_string_center(160, 88, "Hold to power off", th->warning, bg, &font_medium);
        font_draw_string_center(160, 115, digit, th->text_primary, bg, &font_large);
        break;
    }
    case POWER_PROMPT_GOODBYE:
        font_draw_string_center(160, 110, "Goodbye!", th->success, bg, &font_large);
        break;
    case POWER_PROMPT_AUTOOFF:
        font_draw_string_center(160, 100, "Auto power off", th->warning, bg, &font_medium);
        font_draw_string_center(160, 126, "Goodbye!", th->success, bg, &font_large);
        break;
    case POWER_PROMPT_UPDATE:
        font_draw_string_center(160, 100, "Firmware update", th->warning, bg, &font_medium);
        font_draw_string_center(160, 126, "Release POWER", th->text_primary, bg, &font_medium);
        break;
    case POWER_PROMPT_BATTERY:
        font_draw_string_center(160, 100, "Battery critical", th->warning, bg, &font_medium);
        font_draw_string_center(160, 126, "Shutting down", th->text_primary, bg, &font_medium);
        break;
    default:
        break;
    }
}
