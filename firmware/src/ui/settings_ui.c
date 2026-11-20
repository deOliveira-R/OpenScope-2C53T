/*
 * OpenScope 2C53T - Settings UI
 *
 * The paint of Home > Settings (ui/settings_screen.h): the list, the
 * question of "Reset to Defaults" over it, and the two pages, Sound and
 * Light and About. Paint only, display task: the screens say what is
 * selected, and the values are read through the property model (prop_get):
 * sys.volume, sys.brightness, sys.autooff, sys.startup, sys.theme,
 * meter.link. USB Sharing is not a property (settings_screen.h): its row
 * shows usb_share_label().
 */

#include "ui.h"
#include "settings_screen.h"
#include "lcd.h"
#include "font.h"
#include "theme.h"
#include "watchdog.h"
#include "shared_mem.h"
#include "sys_prefs.h"
#include "model.h"
#include "meter_link_policy.h"
#if FEATURE_USBDRIVE
#include "usb_debug.h"
#endif
#include <stdio.h>

/* The list's labels, by name (ui.h settings_item_t is the display order). */
static const char *const settings_items[SETTINGS_ITEM_COUNT] = {
    [SET_ITEM_SOUND_LIGHT]   = "Sound and Light",
    [SET_ITEM_AUTOOFF]       = "Auto Shutdown",
    [SET_ITEM_STARTUP]       = "Startup on Boot",
    [SET_ITEM_METER_LINK]    = "Meter on Battery",   /* without a USB host: With Screen / Always */
    [SET_ITEM_THEME]         = "Theme",
    [SET_ITEM_USB_SHARE]     = "USB Sharing",        /* stock's settings item (OK handler 0x0800BF08) */
    [SET_ITEM_FW_UPDATE]     = "Firmware Update",
    [SET_ITEM_RESET_DEFAULTS] = "Reset to Defaults",  /* asks first (settings_reset.h) */
    [SET_ITEM_ABOUT]         = "About",
};
_Static_assert(SET_ITEM_ABOUT == SETTINGS_ITEM_COUNT - 1 && SETTINGS_ITEM_COUNT == 9,
               "nine device settings, About last");

/* Layout constants */
#define MENU_TOP        42
#define MENU_ITEM_H     24
#define MENU_LEFT       16
#define MENU_WIDTH      288

/* ═══════════════════════════════════════════════════════════════════
 * Helper: draw a generic menu list
 * ═══════════════════════════════════════════════════════════════════ */

/* Maximum visible items given screen geometry */
#define MENU_VISIBLE    ((LCD_HEIGHT - 20 - MENU_TOP) / MENU_ITEM_H)

static void draw_menu_item(int i, int scroll_offset, int selected_idx,
                           const char *label, const char *value,
                           const theme_t *th)
{
    int row = i - scroll_offset;
    if (row < 0 || row >= MENU_VISIBLE) return;

    uint16_t y = MENU_TOP + row * MENU_ITEM_H;
    bool selected = (i == selected_idx);

    uint16_t bg = selected ? th->menu_selected_bg : th->background;

    if (selected)
        lcd_fill_rect(MENU_LEFT - 4, y, MENU_WIDTH + 8, MENU_ITEM_H - 2, bg);

    if (selected)
        font_draw_string(MENU_LEFT, y + 4, ">",
                         th->highlight, bg, &font_medium);

    uint16_t fg = selected ? th->text_primary : th->text_secondary;
    font_draw_string(MENU_LEFT + 16, y + 4, label, fg, bg, &font_medium);

    if (value)
        font_draw_string_right(MENU_LEFT + MENU_WIDTH - 8, y + 4,
                               value, th->highlight, bg, &font_medium);
}

/* Sound and Light page: stock's two rows, volume 0..10 and brightness
 * 25..100 % in steps of 5 (sys.volume, sys.brightness). */
void settings_ui_draw_sound(uint8_t sub)
{
    const theme_t *th = theme_get();
    char vol[8], bri[8];

    lcd_fill_rect(0, 16, LCD_WIDTH, LCD_HEIGHT - 32, th->background);
    font_draw_string_center(LCD_WIDTH / 2, 20, "Sound and Light",
                            th->text_primary, th->background, &font_large);

    const unsigned v = (unsigned)prop_get(P(SYS_VOLUME));
    if (v == 0u) snprintf(vol, sizeof(vol), "Off");
    else         snprintf(vol, sizeof(vol), "%u", v);
    snprintf(bri, sizeof(bri), "%u%%", (unsigned)prop_get(P(SYS_BRIGHTNESS)));

    draw_menu_item(SET_SOUND_ROW_VOLUME, 0, sub, "Sound", vol, th);
    draw_menu_item(SET_SOUND_ROW_BRIGHTNESS, 0, sub, "Brightness", bri, th);
}

void settings_ui_draw_list(uint8_t row)
{
    const int selected = row;
    const theme_t *th = theme_get();

    lcd_fill_rect(0, 16, LCD_WIDTH, LCD_HEIGHT - 32, th->background);

    font_draw_string_center(LCD_WIDTH / 2, 20, "Settings",
                            th->text_primary, th->background, &font_large);

    /* Compute scroll offset so selected item is always visible */
    int scroll = 0;
    if (selected >= MENU_VISIBLE)
        scroll = selected - MENU_VISIBLE + 1;

    for (int i = 0; i < SETTINGS_ITEM_COUNT; i++) {
        const char *value = NULL;

        switch ((settings_item_t)i) {
        case SET_ITEM_SOUND_LIGHT:
        case SET_ITEM_ABOUT:        value = ">"; break;                 /* a page */
        case SET_ITEM_AUTOOFF:      value = sys_autooff_label(sys_autooff_mins_at((unsigned)prop_get(P(SYS_AUTOOFF)))); break;
        case SET_ITEM_STARTUP:      value = startup_mode_name((startup_mode_t)prop_get(P(SYS_STARTUP))); break;
        /* With a USB host the meter runs whatever this says (meter_link_policy.h). */
        case SET_ITEM_METER_LINK:   value = prop_get(P(METER_LINK)) == METER_LINK_ALWAYS ? "Always" : "With Screen"; break;
        case SET_ITEM_THEME:        value = th->name; break;                /* sys.theme: the theme in force */
#if FEATURE_USBDRIVE
        case SET_ITEM_USB_SHARE:    value = usb_share_label(); break;   /* CDC + read-only gallery drive */
#else
        case SET_ITEM_USB_SHARE:    value = "n/a"; break;               /* built CDC-only (USB_MSC=0) */
#endif
        case SET_ITEM_FW_UPDATE:    value = "DFU"; break;               /* reboot to the bootloader */
        case SET_ITEM_RESET_DEFAULTS:                                   /* an action: no value */
        default: break;
        }

        draw_menu_item(i, scroll, selected, settings_items[i], value, th);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * Reset to Defaults: the question, a box over the list
 * ═══════════════════════════════════════════════════════════════════ */

void settings_ui_draw_reset_question(uint8_t row)
{
    const theme_t *th = theme_get();
    const uint16_t x = 30, y = 76, w = 260, h = 92;
    const uint16_t bg = th->background;

    settings_ui_draw_list(row);               /* the list stays readable behind it */
    lcd_fill_rect(x, y, w, h, bg);
    lcd_fill_rect(x, y, w, 1, th->highlight);
    lcd_fill_rect(x, y + h - 1, w, 1, th->highlight);
    lcd_fill_rect(x, y + 1, 1, h - 2, th->highlight);
    lcd_fill_rect(x + w - 1, y + 1, 1, h - 2, th->highlight);
    font_draw_string_center(LCD_WIDTH / 2, y + 10, "Reset all settings?",
                            th->text_primary, bg, &font_large);
    font_draw_string_center(LCD_WIDTH / 2, y + 42, "OK = yes   MENU = no",
                            th->highlight, bg, &font_medium);
    font_draw_string_center(LCD_WIDTH / 2, y + 68, "Calibration, gallery, logo are kept",
                            th->text_secondary, bg, &font_small);
}

/* ═══════════════════════════════════════════════════════════════════
 * About page
 * ═══════════════════════════════════════════════════════════════════ */

void settings_ui_draw_about(void)
{
    const theme_t *th = theme_get();

    lcd_fill_rect(0, 16, LCD_WIDTH, LCD_HEIGHT - 32, th->background);

    font_draw_string_center(LCD_WIDTH / 2, 20, "About",
                            th->text_primary, th->background, &font_large);

    uint16_t y = 55;
    uint16_t fg = th->text_secondary;
    uint16_t bg = th->background;

    font_draw_string(MENU_LEFT, y, "OpenScope 2C53T", th->text_primary, bg, &font_medium);
    y += 22;
    font_draw_string(MENU_LEFT, y, "Custom Firmware v0.2", fg, bg, &font_medium);
    y += 22;
    font_draw_string(MENU_LEFT, y, "MCU: AT32F403A @240MHz", fg, bg, &font_medium);
    y += 22;
    font_draw_string(MENU_LEFT, y, "LCD: ST7789V 320x240", fg, bg, &font_medium);
    y += 22;
    font_draw_string(MENU_LEFT, y, "RTOS: FreeRTOS", fg, bg, &font_medium);
    y += 26;

    /* Pool status */
    {
        char pbuf[40];
        snprintf(pbuf, sizeof(pbuf), "Pool: %s (%luKB)",
                 shared_mem_owner_name(),
                 (unsigned long)(shared_mem_owner_need() / 1024));
        font_draw_string(MENU_LEFT, y, pbuf, th->highlight, bg, &font_small);
        y += 14;
        snprintf(pbuf, sizeof(pbuf), "Heap free: %luB  Transitions: %lu",
                 (unsigned long)xPortGetFreeHeapSize(),
                 (unsigned long)shared_mem_transition_count());
        font_draw_string(MENU_LEFT, y, pbuf, fg, bg, &font_small);
    }
    y += 20;

    font_draw_string(MENU_LEFT, y, "License: GPL v3",
                     th->text_secondary, bg, &font_small);

    y += 24;
    font_draw_string_center(LCD_WIDTH / 2, y, "MENU to go back",
                            th->text_secondary, bg, &font_small);
}

/* ═══════════════════════════════════════════════════════════════════
 * Task health diagnostics panel
 * ═══════════════════════════════════════════════════════════════════ */
