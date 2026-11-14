/*
 * OpenScope 2C53T - home screen model (stock's mode-selection page).
 *
 * Pure state machine: no LCD, no FreeRTOS, no GPIO, so tests/test_home_model.c
 * runs the very code the input task runs (make test-home-model). Drawing is
 * home_ui.c; the screen (keys, activation through the model) is
 * home_screen.c. Keeping the model apart is what lets it port to the softkey
 * UI redesign (docs/specs, "S1") unchanged.
 *
 * The entries are ONE TABLE (HOME_TILES below): a row gives the item's id,
 * its `ui screen` token, its label, its picture and what OK activates. A new
 * Home entry is one row (plus its picture in home_ui.c if it needs a new
 * one); the count, the page dots, the wrap and the neighbours follow.
 *
 * Stock V1.2.0 evidence (research/parity/home-screen.md has the detail):
 *   - Home is ms[0xF68] == 0; the selected item is ms[0xF69].
 *   - Four items, in this order: Multimeter, Oscilloscope, Signal Generator,
 *     Settings (label table 0x0804C924). OK enters F68 = item + 1
 *     (0x0800BD90), i.e. meter 1, scope 2, siggen 3, settings 4.
 *   - LEFT/RIGHT step the selection with wrap (0x0800B226 / 0x0800B7FA);
 *     UP/DOWN do nothing on home (their handlers exclude F68 == 0).
 *   - Long BACK (our long MENU, 0x0800D834) enters home from any mode with
 *     the item of the mode it left preselected; at boot with Startup "None"
 *     the preselection is Oscilloscope (0x0802DF80).
 *   - On home itself BACK, short or long, does nothing in stock (both
 *     handlers exit on F68 == 0). Ours: short MENU goes back to the mode
 *     home was entered from (HOME_KEY_BACK), see home_key().
 */
#ifndef HOME_MODEL_H
#define HOME_MODEL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ui.h"          /* device_mode_t */

/* The pictures home_ui.c can draw. */
typedef enum {
    HOME_ICON_METER = 0,
    HOME_ICON_SCOPE,
    HOME_ICON_SIGGEN,
    HOME_ICON_GEAR,
    HOME_ICON_COUNT
} home_icon_t;

/* A tile that is no instrument (Settings): OK sets sys.mode, not sys.instrument. */
#define HOME_NO_INSTRUMENT  0xFFu

/* X(id, token, label, icon, mode, instrument), in the order shown (stock's
 * ms[0xF69] values for the first four). `token` is the `ui screen` name
 * ("home=meter"); `mode` is the device mode the entry's view is (what long
 * MENU preselects and BACK returns to); `instrument` is its sys.instrument
 * value, or HOME_NO_INSTRUMENT for a page that is no instrument. */
#define HOME_TILES(X) \
    X(METER,    "meter",    "Multimeter",       HOME_ICON_METER,  MODE_MULTIMETER,   1) \
    X(SCOPE,    "scope",    "Oscilloscope",     HOME_ICON_SCOPE,  MODE_OSCILLOSCOPE, 0) \
    X(SIGGEN,   "siggen",   "Signal Generator", HOME_ICON_SIGGEN, MODE_SIGNAL_GEN,   2) \
    X(SETTINGS, "settings", "Settings",         HOME_ICON_GEAR,   MODE_SETTINGS,     HOME_NO_INSTRUMENT)

typedef enum {
#define X(id, token, label, icon, mode, inst) HOME_ITEM_##id,
    HOME_TILES(X)
#undef X
    HOME_ITEM_COUNT
} home_item_t;

typedef struct {
    const char *token;       /* `ui screen`: home=<token>                      */
    const char *label;       /* English, as stock's column of 0x0804C924       */
    uint8_t     icon;        /* home_icon_t                                    */
    uint8_t     mode;        /* device_mode_t of the entry's view              */
    uint8_t     instrument;  /* sys.instrument value, or HOME_NO_INSTRUMENT    */
} home_tile_t;

/* The row of item `it`, or NULL when it is out of range. */
const home_tile_t *home_tile(home_item_t it);

typedef struct {
    uint8_t sel;             /* home_item_t: the highlighted item */
    uint8_t back;            /* device_mode_t home was entered from, or HOME_NO_BACK */
} home_t;

#define HOME_NO_BACK   0xFFu
/* Boot state: Oscilloscope preselected (stock 0x0802DF80), nothing to go back to. */
#define HOME_INIT      { HOME_ITEM_SCOPE, HOME_NO_BACK }

typedef enum {
    HOME_KEY_LEFT = 0,
    HOME_KEY_RIGHT,
    HOME_KEY_UP,
    HOME_KEY_DOWN,
    HOME_KEY_OK,
    HOME_KEY_BACK,           /* short MENU */
    HOME_KEY_OTHER           /* any other key: ignored */
} home_key_t;

typedef enum {
    HOME_IGNORED = 0,        /* nothing changed */
    HOME_MOVED,              /* selection changed: repaint the selection */
    HOME_ENTER               /* leave home into home_result_t.mode */
} home_action_t;

typedef struct {
    home_action_t act;
    device_mode_t mode;      /* valid for HOME_ENTER */
} home_result_t;

void          home_init(home_t *h);
/* Entering home from `from`: preselect its item and remember it for BACK.
 * Entering from MODE_HOME (or anything unknown) keeps the selection. */
void          home_enter(home_t *h, device_mode_t from);
home_result_t home_key(home_t *h, home_key_t k);

/* Item <-> mode. home_item_for_mode() answers HOME_ITEM_COUNT for a mode
 * that is not on the home page (MODE_HOME itself). */
device_mode_t home_item_mode(home_item_t it);
home_item_t   home_item_for_mode(device_mode_t m);
/* The neighbour shown left (dir -1) or right (+1) of `sel`, with wrap. */
home_item_t   home_neighbour(uint8_t sel, int dir);
/* English label, as stock's English column of 0x0804C924. */
const char   *home_item_label(home_item_t it);

#endif /* HOME_MODEL_H */
