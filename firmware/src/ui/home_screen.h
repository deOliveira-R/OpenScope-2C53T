/*
 * OpenScope 2C53T - the home screen (stock's mode-selection page) as a
 * screen of the router (screen.h; ARCH-1 B2)
 *
 * home_model.c is its core (the entry table, the selection, the wrap),
 * home_ui.c its painting, and this is the screen: the callbacks of
 * screen.h, in their contexts.
 *
 *   open()   input task    entering Home preselects the entry of the screen
 *                          it was opened from and remembers it for BACK
 *                          (stock 0x0800D834)
 *   key()    input task    LEFT/RIGHT move the selection (UI state, kept
 *                          here); OK activates the selected entry and short
 *                          MENU the one Home was opened from, both as a set
 *                          of sys.instrument (an instrument) or sys.mode
 *                          (Settings) through prop_set_local: the path the
 *                          long mode keys and the shell's `set` take. It
 *                          writes no mode and calls no driver.
 *   draw()   display task  FULL: the page; UPDATE: the parts that follow
 *                          the selection. Read only.
 *
 * The answer of key() says what to repaint (SCR_DRAW: an UPDATE paint of
 * this screen); the wiring (legacy_screens.c) posts it and runs the settings
 * hook, as for every key.
 *
 * Pure: no FreeRTOS, no driver header. Host test: make test-home-screen.
 */
#ifndef HOME_SCREEN_H
#define HOME_SCREEN_H

#include <stdint.h>
#include "screen.h"

extern const screen_t home_screen;

/* The selected entry (a home_item_t). Any task, read only. */
uint8_t home_screen_selected(void);

/* home_ui.c (display task): the whole page, and the parts that follow the
 * selection (dots, icon, neighbours, label). */
void home_ui_draw(uint8_t sel);
void home_ui_draw_selection(uint8_t sel);

#endif /* HOME_SCREEN_H */
