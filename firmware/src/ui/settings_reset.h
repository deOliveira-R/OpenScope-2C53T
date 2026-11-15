/*
 * OpenScope 2C53T - Settings > Reset to Defaults: the confirm step
 *
 * "Reset to Defaults" copies the built-in defaults over the user settings
 * (settings_store_reset_to_defaults). It never happens on a single press:
 *
 *   OK on the row "Reset to Defaults"   -> the question (SET_PAGE_RESET):
 *                                          "Reset all settings?
 *                                           OK = yes   MENU = no"
 *   OK on the question                  -> the reset, a popup, back to the list
 *   MENU on the question                -> back to the list, nothing changed
 *                                          (the list's MENU: a page goes up one)
 *   any other key on the question       -> nothing (the question stays)
 *
 * The decision is this pure function, so tests/test_settings_reset.c walks
 * it without the input task. input_handler.c acts on the answer.
 */
#ifndef SETTINGS_RESET_H
#define SETTINGS_RESET_H

#include "ui.h"

typedef enum {
    SET_RESET_NOT_MINE = 0,   /* OK means something else here                */
    SET_RESET_ASK,            /* show the question; nothing is reset         */
    SET_RESET_DO,             /* the question was up and OK said yes: reset  */
} settings_reset_step_t;

/* What OK means for the reset, given the Settings page shown and the row
 * selected in the list. */
static inline settings_reset_step_t settings_reset_on_ok(int8_t depth, int8_t selected)
{
    if (depth == SET_PAGE_RESET) return SET_RESET_DO;
    if (depth == SET_PAGE_LIST && selected == SET_ITEM_RESET_DEFAULTS) return SET_RESET_ASK;
    return SET_RESET_NOT_MINE;
}

#endif /* SETTINGS_RESET_H */
