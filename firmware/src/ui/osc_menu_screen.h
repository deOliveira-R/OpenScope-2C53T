/*
 * OpenScope 2C53T - the Oscilloscope Settings page as a screen of the router
 * (screen.h; ARCH-1 B5), table-bound to the scope's properties
 *
 * The page's state machine is osc_menu_model.c (rail, panes, cursor; pure)
 * and its paint is osc_menu_ui.c. This file is what joins them to the
 * property model: a table says which property each pane or row shows and
 * sets, so a change made on the page is ONE prop_set_local / prop_step_local
 * on the input task, the same setter the shell's `set` and the plot's keys
 * reach, and what the page shows is what `get` answers.
 *
 *   pane      kind    property                          item -> value
 *   Display   RADIO   view.mode                         item i = value i (yt roll xy fft split waterfall)
 *   Cursors   RADIO   view.cursor                       item i = value i (off time volt both)
 *   Persist   RADIO   view.persist                      item i = value i (off 500ms 1s 2s inf)
 *   Math      CHIPS   view.math                         Off = 0, else 1 + math_op_t in stock's 2x4 order
 *   CH1/CH2   VALUES  chN.enabled, chN.probe, chN.coupling    LEFT/RIGHT/OK = one step
 *   Trigger   VALUES  trig.mode, trig.edge, trig.source       LEFT/RIGHT/OK = one step
 *   Measure   CHIPS   chN.meas (shown)                  explicit: see below
 *
 * OSC_ACT_ENTER  = prop_get (the pane cursor starts on the value in force),
 * OSC_ACT_SELECT = prop_set_local, OSC_ACT_CHANGE = prop_step_local. The
 * rules a change obeys (keep one channel on, a refused relay keeps the old
 * coupling, Single arms the one-shot, a spectrum view claims the pool, ...)
 * are the setters' (model/props_scope.c); nothing is applied here.
 *
 * Captions are the page's own (osc_menu_model.c: "Y-T", "500 ms",
 * "CH1+CH2"): a property's labels are the shell's tokens. The VALUE a
 * VALUES row shows is the property's text (prop_format: "DC", "10X",
 * "Single"), a bool as ON / OFF.
 *
 * EXPLICIT ACTIONS (not one property set):
 *   - Measure: the channel tab (a page flip, UI state), All, None and the 12
 *     chips. The pure model edits the selection itself (meas_select.c, the
 *     backing store of chN.meas); this screen then tells the model layer the
 *     group changed (model_touch), so the strip and a `get chN.meas` follow.
 *   - picking a cursor mode closes the page (the model's rule), and a cursor
 *     mode change moves the arrows' focus (osc_menu_env_cursors).
 *   - the rail, BACK, the deep-link keys and the opener key: UI state.
 *
 * The footer note is the last change's result when the row cannot show it
 * (a refusal, NOT SET, the Single arm), kept for this visit only.
 *
 * Contexts: open/close/key on the input task, draw/epoch on the display
 * task, describe anywhere. The page redraws from its epoch (every value it
 * shows, the cursor, the note), so a `set` from the shell while it is open
 * is painted at once and an idle page costs no LCD write.
 *
 * Pure: no FreeRTOS, no driver header. Host test: make test-osc-menu-screen.
 */
#ifndef OSC_MENU_SCREEN_H
#define OSC_MENU_SCREEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "screen.h"
#include "osc_menu_model.h"
#include "model.h"

extern const screen_t osc_menu_screen;

/* ── input task ────────────────────────────────────────────────────── */

/* Open the page on `row` (the full page, rail focus), or locked inside its
 * pane; while open it jumps there. `opener` is the key that did it (a
 * button_id_t): pressed again it closes the page (key_model.c). The layer
 * itself is pushed by the stack mirror (legacy_screens_sync). */
void    osc_menu_screen_open(osc_row_t row, bool locked, uint8_t opener);
uint8_t osc_menu_screen_opener(void);          /* BTN_NONE while closed */

/* ── the binding (any task, read only) ─────────────────────────────── */

/* The property a pane (RADIO, Math, Measure: `item` ignored, Measure's is
 * CH1's) or a VALUES row shows; NULL out of range. */
const prop_desc_t *osc_menu_bind_prop(osc_row_t row, uint8_t item);
/* RADIO / Math: the property value item `item` sets; -1 for no such item. */
int32_t osc_menu_bind_value(osc_row_t row, uint8_t item);
/* RADIO / Math: the item showing the property's value now; the pane's item
 * count when none does. */
uint8_t osc_menu_bind_current(osc_row_t row);
/* VALUES: the text of row `item`'s value now ("ON", "10X", "Single"). */
void    osc_menu_bind_text(osc_row_t row, uint8_t item, char *out, size_t n);
/* The footer note of this visit, "" when there is none. */
const char *osc_menu_screen_note(void);
/* A change of these property groups (bit = PG_x) while the page is on top
 * is one the page itself shows in full: an in-place paint is enough. false:
 * something else it does not draw changed too (the theme, the timebase in
 * the bars): the caller repaints everything. */
bool    osc_menu_screen_paints(uint16_t groups);

/* ── the environment (input_handler.c; fakes in the host test) ─────── */

/* The page changed the cursor mode (on = some cursor is shown): the arrows'
 * focus follows, as TRIGGER's old cursor cycle did. Input task. */
void osc_menu_env_cursors(bool on);

/* The paint is osc_menu_draw() / osc_menu_invalidate() (osc_menu_model.h;
 * osc_menu_ui.c, fakes in the host test): display task only. */

#endif /* OSC_MENU_SCREEN_H */
