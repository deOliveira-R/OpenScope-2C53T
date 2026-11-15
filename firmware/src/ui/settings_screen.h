/*
 * OpenScope 2C53T - Settings (Home > Settings) as screens of the router
 * (screen.h; ARCH-1 B4 / B8)
 *
 * DEVICE settings only: a list (settings_item_t, ui.h) and its pages. The
 * scope's own settings are in its menu (osc_menu_screen.h). Three screens:
 *
 *   settings      SCR_SETTINGS   base    the list, and the question of "Reset to
 *                                        Defaults" drawn over it (settings_reset.h)
 *   set_sound     SCR_SET_SOUND  page    Sound and Light: sys.volume, sys.brightness
 *   set_about     SCR_SET_ABOUT  page    About
 *
 * A page is a layer the list pushes over itself (CHILD | OPAQUE: it covers
 * the list between the bars and takes the keys), so which page is shown is
 * the router's stack and nothing else. The question is not a page of its
 * own: it is the list with a box over it, and `ui screen` shows it as page 8.
 *
 * KEYS (input task). Every value is a property, set through prop_set_local
 * / prop_step_local (ORIGIN_KEY): the function the shell's `set` ends in.
 *
 *   the list
 *     UP / DOWN       move the row (stops at the ends)
 *     LEFT / RIGHT    a row with a value: one step, stopping at the ends
 *                     (Startup on Boot and Theme go round); else nothing
 *     OK              a row with a value: one step, round the ends
 *                     Sound and Light, About: the page
 *                     USB Sharing: the action settings_env_usb_share_toggle()
 *                     Firmware Update: settings_env_fw_update() (reboots)
 *                     Reset to Defaults: the question
 *     MENU            Home (sys.mode)
 *   the question      OK = settings_env_reset(), MENU = back, both to the list
 *   Sound and Light   UP / DOWN the row; LEFT / RIGHT one step (stops at the
 *                     ends), OK one step (round); MENU back
 *   About             OK, MENU back
 *   everywhere        CH1, CH2, TRIGGER, MOVE repaint the screen and do
 *                     nothing else (as they always did here); AUTO, PRM,
 *                     SELECT and every long key do nothing. POWER, SAVE and
 *                     the long mode keys are the router's globals.
 *
 *   row                property        values
 *   Auto Shutdown      sys.autooff     off / 15 / 30 / 60 min
 *   Startup on Boot    sys.startup     stock's row order, then "Last Used"
 *   Meter on Battery   meter.link      With Screen / Always
 *   Theme              sys.theme
 *   Sound              sys.volume      0..10 (a click at the new level)
 *   Brightness         sys.brightness  25..100 %
 *
 * USB SHARING HAS NO PROPERTY, on purpose: it is not a setting. It is the
 * USB device's enumeration (CDC, or CDC + the read-only gallery drive),
 * owned by the USB core (usb_debug.c / usb_mode_sm.c), not saved, and it
 * falls back by itself when the host does not configure the composite
 * device. Changing it re-enumerates the link the shell runs on, so a `set`
 * could not answer. It stays one named action, usb_share_toggle(), reached
 * from its row; the row shows usb_share_label().
 *
 * DISPLAY (display task). Each screen paints itself (settings_ui.c) and is
 * gated on its epoch: a key that changed nothing costs no LCD write.
 *
 * `ui screen`: settings=<page>/<row>/<sub>, page = settings_page_t (0 the
 * list, 2 About, 7 Sound and Light, 8 the question), row = settings_item_t,
 * sub = the row of Sound and Light. The numbers are the ones settings_depth
 * / settings_selected / settings_sub_selected had.
 *
 * Pure: no FreeRTOS, no driver header. Host test: make test-settings-screen.
 */
#ifndef SETTINGS_SCREEN_H
#define SETTINGS_SCREEN_H

#include <stdbool.h>
#include <stdint.h>
#include "screen.h"

extern const screen_t settings_screen;         /* SCR_SETTINGS  */
extern const screen_t settings_sound_screen;   /* SCR_SET_SOUND */
extern const screen_t settings_about_screen;   /* SCR_SET_ABOUT */

/* What is shown, in one look. Any task, read only. */
typedef struct {
    uint8_t page;       /* settings_page_t */
    uint8_t row;        /* settings_item_t: the row of the list */
    uint8_t sub;        /* the row of Sound and Light           */
} settings_view_t;
settings_view_t settings_screen_view(void);

/* ── the paint (settings_ui.c; fakes in the host test). Display task ── */
void settings_ui_draw_list(uint8_t row);
void settings_ui_draw_reset_question(uint8_t row);   /* the list and the box over it */
void settings_ui_draw_sound(uint8_t sub);
void settings_ui_draw_about(void);

/* ── the environment (input_handler.c; fakes in the host test). Input task ── */
void settings_env_click(void);              /* the key click, at the volume just set          */
void settings_env_page_left(void);          /* MENU out of a page: a deliberate boundary, the
                                             * settings are written now                       */
void settings_env_reset(void);              /* Reset to Defaults: the reset and its popup     */
void settings_env_usb_share_toggle(void);   /* the USB Sharing action (see above)             */
void settings_env_fw_update(void);          /* write the settings, reboot into the bootloader */

#endif /* SETTINGS_SCREEN_H */
