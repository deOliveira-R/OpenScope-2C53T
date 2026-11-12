/*
 * OpenScope 2C53T - weak stand-in for the Oscilloscope Settings page.
 *
 * UI round 1 builds the page (R1-A: osc_menu_model.c / osc_menu_ui.c) and its
 * key wiring (R1-K: key_model.c, input_handler.c) in parallel. These weak
 * definitions let the key wiring build and run on its own: MENU/PRM open a
 * placeholder page, UP/DOWN walk its rows, OK on CH1 toggles the FFT view
 * (so the FFT views stay reachable now that PRM no longer cycles them, and
 * the FFT code is not dropped by --gc-sections), MENU closes it. R1-A's strong
 * definitions replace every one of them at link time once both are merged;
 * then this file can be deleted.
 */
#include "osc_menu_model.h"
#include "lcd.h"
#include "font.h"
#include "theme.h"
#include "ui.h"
#include "input_handler.h"   /* scope_view_select() */

static bool    stub_open;
static uint8_t stub_row;

static const char *const stub_rows[OSC_ROW_COUNT] = {
    "Display", "Cursors", "Measure", "CH1", "CH2", "Trigger", "Persist", "Math"
};

__attribute__((weak)) void osc_menu_open(osc_row_t row)
{
    stub_open = true;
    stub_row = ((unsigned)row < (unsigned)OSC_ROW_COUNT) ? (uint8_t)row : 0u;
}

__attribute__((weak)) bool osc_menu_is_open(void)
{
    return stub_open;
}

__attribute__((weak)) void osc_menu_close(void)
{
    stub_open = false;
}

__attribute__((weak)) osc_result_t osc_menu_key(osc_key_t k)
{
    if (!stub_open) return OSC_RES_NONE;
    switch (k) {
    case OSC_KEY_BACK:
        stub_open = false;
        return OSC_RES_CLOSE;
    case OSC_KEY_UP:
        stub_row = (uint8_t)((stub_row + OSC_ROW_COUNT - 1u) % OSC_ROW_COUNT);
        return OSC_RES_REDRAW;
    case OSC_KEY_DOWN:
        stub_row = (uint8_t)((stub_row + 1u) % OSC_ROW_COUNT);
        return OSC_RES_REDRAW;
#ifdef FEATURE_FFT
    case OSC_KEY_OK:
        if (stub_row != OSC_ROW_CH1) return OSC_RES_NONE;
        scope_view_select(scope_view == SCOPE_VIEW_TIME ? SCOPE_VIEW_FFT : SCOPE_VIEW_TIME);
        stub_open = false;
        return OSC_RES_CLOSE;
#endif
    default:
        return OSC_RES_NONE;
    }
}

/* Everything below the 16 px status bar. */
__attribute__((weak)) void osc_menu_draw(void)
{
    const theme_t *th = theme_get();
    lcd_fill_rect(0, 16, LCD_WIDTH, LCD_HEIGHT - 16, th->background);
    font_draw_string(8, 24, "Oscilloscope Settings (coming)", th->text_primary,
                     th->background, &font_medium);
    for (uint8_t i = 0; i < OSC_ROW_COUNT; i++) {
        const bool sel = (i == stub_row);
        const uint16_t y = (uint16_t)(48 + i * 20);
        const uint16_t bg = sel ? th->menu_selected_bg : th->background;
        lcd_fill_rect(8, y, 96, 18, bg);
        font_draw_string(12, (uint16_t)(y + 3), stub_rows[i],
                         sel ? th->highlight : th->text_secondary, bg, &font_small);
    }
    font_draw_string(112, 200, "CH1 + OK: FFT on/off", th->text_secondary, th->background, &font_small);
    font_draw_string(112, 216, "MENU: back", th->text_secondary, th->background, &font_small);
}
