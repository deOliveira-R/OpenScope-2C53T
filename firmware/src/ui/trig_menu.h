/*
 * OpenScope 2C53T - the trigger panel (parity C3)
 *
 * Stock's TRIGGER key opens "Trigger Settings" (menu table 0x0804C824) with
 * three entries: "Mode:" Normal/Auto/Single (0x0804C7F0..F8), "Edge:" and
 * "Channel:" CH1/CH2 (0x0804C7FC/C800). Before C3 OpenScope had the mode and
 * edge only in Settings -> Oscilloscope and no way to pick the source; the
 * TRIGGER key cycled the cursors (stock reaches its cursors from the menu, so
 * they move to Settings -> Oscilloscope -> Cursors).
 *
 * This is the panel's model, pure (no LCD, no FPGA), so tests/test_key_model.c
 * runs the code the input task runs. scope_ui.c draws it; input_handler.c
 * applies the hardware side of a change (reg 0x06 for the source, the SINGLE
 * re-arm for the mode) from the bits trig_menu_change() returns.
 *
 * Keys while open (key_model.c): UP/DOWN pick the row, LEFT/RIGHT change it
 * (OK = RIGHT), TRIGGER or MENU close, MOVE/SELECT/PRM close too.
 */
#ifndef TRIG_MENU_H
#define TRIG_MENU_H

#include <stdint.h>
#include <stdbool.h>
#include "scope_state.h"

typedef enum {
    TM_ROW_MODE = 0,
    TM_ROW_EDGE,
    TM_ROW_SOURCE,
    TM_ROW_COUNT
} trig_menu_row_t;

typedef struct {
    bool    open;
    uint8_t row;            /* trig_menu_row_t */
} trig_menu_t;

#define TM_CHANGED_MODE    0x01u
#define TM_CHANGED_EDGE    0x02u
#define TM_CHANGED_SOURCE  0x04u
#define TM_REFUSED_OFF     0x80u    /* the source asked for is a channel that is off */

/* The one panel: the input task writes it, the display task reads it. */
extern trig_menu_t scope_trig_menu;

void trig_menu_open(trig_menu_t *m);       /* opens on the Mode row, as stock */
void trig_menu_close(trig_menu_t *m);
void trig_menu_row_step(trig_menu_t *m, int dir);    /* wraps */

/* Change the selected row by dir (+1/-1, wraps). Returns TM_CHANGED_* for
 * what moved, or TM_REFUSED_OFF (nothing changed) when the source would land
 * on a disabled channel: stock keeps the source on a live channel too, and
 * flips it when one is switched off (0x080235E8). */
uint8_t trig_menu_change(const trig_menu_t *m, scope_state_t *ss, int dir);

const char *trig_menu_row_label(uint8_t row);        /* "Mode:", "Edge:", "Channel:" */
const char *trig_menu_row_value(const scope_state_t *ss, uint8_t row);

#endif /* TRIG_MENU_H */
