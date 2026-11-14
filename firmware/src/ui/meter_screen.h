/*
 * OpenScope 2C53T - the multimeter screen as a screen of the router
 * (screen.h; ARCH-1 B6), on the meter.* properties
 *
 * The meter itself is the meter SoC behind USART2 (fpga.c owns the link and
 * the function change, meter_data.c the readings); the meter.* properties
 * are its setters. This screen is the view: every key that changes meter
 * state is one property set on the input task (prop_set_local /
 * prop_step_local), so a key press is what `get meter.*` answers at once,
 * and a `set meter.*` from the shell (or the old `meter` commands, which
 * write the same state) while the screen is shown is what the screen
 * paints: tick() sees its epoch move and asks for an in-place paint.
 *
 *   key                 Big / Graph / Stats      Limits            Fuse tester
 *   MOVE     (softkey)  meter.func +1 (wraps)    the same          lead cal (Cal leads);
 *                       Temperature: meter.degf                    Types page: meter.fusetype +1
 *   SELECT   (softkey)  min/max/mean reset       Low               meter.fuserating +1 (wraps);
 *                                                                  Scan page: meter.fusethresh,
 *                                                                  the next preset
 *   TRIGGER  (softkey)  meter.rel toggle         High              meter.fuseview +1 (wraps)
 *                       Continuity: meter.beep +1 (wraps)
 *   PRM      (softkey)  meter.layout, the next of Big, Graph, Stats, Limits, Fuse
 *   LEFT / RIGHT        meter.func -/+ 1 (wraps) the same          meter.fusetype -/+ 1 (wraps)
 *   UP / DOWN           nothing                  the active limit  meter.fuserating +/- 1 (stops);
 *                                                +/- one step      Scan page: meter.fusethresh
 *                                                                  +/- 0.1 mV (stops)
 *   OK                  meter.hold toggle (refused without a reading)
 *   AUTO                meter.func = auto (again: re-arms the SoC)
 *   MENU                Home (sys.mode)
 *   CH1 CH2             nothing but a full repaint (as before B6)
 *   long keys           nothing
 *   (SAVE, POWER and the long mode keys are the router's globals.)
 *
 * What a softkey does is the softkey bar's table (softkey.h): the bar shown
 * depends on the layout, the fuse page and the function, and a press runs
 * the slot's handler. The tables and their handlers are here; the values
 * the bar prints come from the properties, or from the paint module for the
 * three things below.
 *
 * Side effects of a set live in the setters (model/props_meter.c), not
 * here: entering the fuse tester forces DC V, entering Limits takes limits
 * from the reading, a fuse type change keeps the rating in amps, a function
 * change releases hold and relative. The key and the shell are one path.
 *
 * View state with no property (it is a float in the unit of the reading it
 * was taken from, or an action): the min/max/mean statistics (reset), the
 * Limits view's low/high and which of them the arrows move, and the fuse
 * tester's lead offset. The paint module keeps it (meter_ui.c, its heap
 * block); the keys reach it through the functions below.
 *
 * Pure: no FreeRTOS, no driver header. Host test: make test-meter-screen.
 */
#ifndef METER_SCREEN_H
#define METER_SCREEN_H

#include <stdbool.h>
#include <stdint.h>
#include "screen.h"
#include "softkey.h"

extern const screen_t meter_screen;

/* The softkey bar for a layout / fuse page / function, and the one shown
 * now. status_bar.c draws it (meter_softkeys_draw) and main.c repaints it
 * when meter_softkeys_epoch() moves. */
const softkey_bar_t *meter_softkey_bar(uint8_t layout, uint8_t fuse_page, uint8_t submode);
const softkey_bar_t *meter_screen_bar(void);
uint32_t meter_softkeys_epoch(void);

/* ── meter_ui.c: the paint module ──────────────────────────────────── */

/* Display task. FULL: the LCD was cleared (REDRAW_ALL, boot). RESTORE: an
 * overlay drawn over the page has gone. UPDATE: in place. */
void     meter_ui_draw(scr_paint_t how);
uint32_t meter_ui_reading_seq(void);      /* moves when the reading to show changed          */
uint32_t meter_ui_rendered_seq(void);     /* the reading_seq the last paint showed           */
bool     meter_ui_periodic(void);         /* the page animates (auto-hold wait, debug strip) */
bool     meter_ui_autoselect_running(void);
/* Moves whenever view state with no property changed (statistics reset,
 * limits, lead offset, the held text): the page's epoch mixes it. */
uint32_t meter_ui_gen(void);

/* Input task: the view state with no property. Each raises its own popup. */
void meter_ui_stats_reset(void);              /* Reset softkey                        */
void meter_ui_limit_press(uint8_t which);     /* Low (0) / High (1) softkey           */
void meter_ui_limits_nudge(int8_t dir);       /* UP / DOWN in Limits                  */
void meter_ui_fuse_cal(void);                 /* Cal leads softkey                    */
void meter_ui_autoselect_cancel(void);        /* a softkey ends a running `meter auto` */
/* Softkey values of that state. */
void meter_ui_limit_text(uint8_t which, char *b, uint8_t n);
bool meter_ui_limit_active(uint8_t which);
void meter_ui_cal_text(char *b, uint8_t n);
bool meter_ui_cal_set(void);

#endif /* METER_SCREEN_H */
