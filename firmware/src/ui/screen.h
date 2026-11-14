/*
 * OpenScope 2C53T - screen core (the screen interface and the key router)
 *
 * A screen is a table entry: a name, flags and a handful of callbacks. One
 * base screen is always current (today one per device mode); overlays, modals
 * and settings children are pushed on top of it. The stack is at most
 * SCR_STACK_MAX deep, base included.
 *
 * Base screens are the views of instruments (a scope, a meter, a generator
 * or a bus analyser is a named set of properties plus one view). A base screen
 * therefore carries an `instrument` id next to its legacy `mode`, and can be
 * found by either (screen_base_for_mode / screen_base_for_instrument). No
 * instrument table exists yet; SCR_INSTRUMENT_NONE means "not owned".
 *
 * Contexts (the contract the firmware wiring will keep):
 *   open() close() key()     input task: change UI state only.
 *   draw() tick() epoch()    display task: read only.
 *   describe()               any task, read only.
 * Pure core: no FreeRTOS, no hardware headers. The lock, the activity hooks
 * and the "stack changed" post are injected (screen_hooks_t). The lock is
 * never held while a screen callback runs, so a callback may push or pop.
 *
 * Host test: make test-screen-router (tests/test_screen_router.c).
 */

#ifndef SCREEN_H
#define SCREEN_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ui.h"             /* button_id_t, device_mode_t, BTN_LONG_FLAG */
#include "redraw_gate.h"    /* redraw_action_t */

typedef enum {
    SCR_HOME, SCR_SCOPE, SCR_METER, SCR_SIGGEN, SCR_SETTINGS,          /* bases (one per mode) */
    SCR_SET_ABOUT, SCR_SET_SOUND,                                       /* settings children    */
    SCR_OSC_MENU, SCR_GALLERY, SCR_LOGO, SCR_POWER_PROMPT,              /* overlays             */
    SCR_COUNT
} screen_id_t;

enum {
    SCR_F_OPAQUE  = 1u << 0,    /* hides everything below: drawing and ticks start here */
    SCR_F_GRAB    = 1u << 1,    /* takes every key but POWER; leftovers go to the globals only,
                                 * unless the layer closed itself under the key: then the key
                                 * routes as if it had not been there */
    SCR_F_CHILD   = 1u << 2,    /* a page of its base (settings pages) */
    SCR_F_NO_BARS = 1u << 3     /* the status/title bars are not drawn */
};

#define SCR_STACK_MAX        4
#define SCR_INSTRUMENT_NONE  0xFFu

typedef struct { button_id_t btn; bool is_long; } scr_key_t;
typedef enum { SCR_UNHANDLED = 0, SCR_HANDLED, SCR_DRAW, SCR_DRAW_FULL, SCR_CLOSE } scr_res_t;
typedef enum { SCR_PAINT_FULL, SCR_PAINT_UPDATE } scr_paint_t;

typedef struct screen {
    const char     *name;            /* `ui screen` and logs */
    screen_id_t     id;
    device_mode_t   mode;            /* base screens: their MODE_*; others MODE_COUNT */
    uint8_t         instrument;      /* base screens: owning instrument id, else SCR_INSTRUMENT_NONE */
    uint8_t         flags;
    /* input task */
    void          (*open)(screen_id_t from);     /* from == SCR_COUNT: nothing before (init) */
    void          (*close)(screen_id_t to);
    scr_res_t     (*key)(scr_key_t k);
    /* display task */
    void          (*draw)(scr_paint_t how, uint32_t frame);
    redraw_action_t (*tick)(uint32_t frame);
    uint32_t      (*epoch)(void);              /* mixes the epochs of the property groups it shows */
    /* any task, read-only: for `ui screen` (row/pane/item of a menu); NULL = name only */
    void          (*describe)(char *out, size_t n);
    uint16_t        groups;                    /* property group bits it shows */
} screen_t;

/* Injected environment. Every member may be NULL. */
typedef struct {
    void (*lock)(void);                 /* guards the stack arrays only */
    void (*unlock)(void);
    void (*touch)(void);                /* every real key: auto-off restart */
    void (*click)(void);                /* every real key: key click */
    /* Self-cal lock (router step 3): return true when the key is consumed
     * (AUTO/MENU cancel, the rest "Busy"). Never asked about POWER. */
    bool (*locked)(button_id_t btn, bool is_long);
    /* Globals (step 4): long MENU/SELECT/MOVE/TRIGGER/SAVE, short SAVE, POWER.
     * Return true when the key was taken. */
    bool (*globals)(button_id_t btn, bool is_long);
    /* After any handled key (step 7): settings capture and store service. */
    void (*after_key)(scr_res_t res);
    /* After the stack changed (push, pop, close, switch): post DCMD_STACK. */
    void (*stack_changed)(void);
} screen_hooks_t;

typedef struct { uint8_t n; screen_id_t id[SCR_STACK_MAX]; uint32_t gen; } screen_stack_t;

/* Setup. screen_register keeps the pointer (entries must outlive the router);
 * it refuses a NULL, an id out of range or a second entry for an id. */
bool          screen_register(const screen_t *s);
void          screen_set_hooks(const screen_hooks_t *h);   /* NULL clears */
void          screen_reset(void);                          /* forget screens, hooks and the stack (tests) */
bool          screen_init(screen_id_t base);               /* stack = {base}; calls base open(SCR_COUNT) */

/* Input task. */
uint8_t       screen_route_key(button_id_t raw);   /* raw may carry BTN_LONG_FLAG; returns a scr_res_t */
void          screen_switch(screen_id_t base);     /* the sys.mode setter calls this; closes top-down */
bool          screen_push(screen_id_t layer);      /* false: not registered, a base, already open, or full */
void          screen_pop(void);                    /* the top layer; the base never pops */
bool          screen_close(screen_id_t layer);     /* that layer and any above it; false if it was not open */

/* Any task. */
screen_id_t   screen_top(void);
screen_id_t   screen_base(void);
screen_stack_t screen_snapshot(void);              /* under the injected lock */
const screen_t *screen_get(screen_id_t id);        /* NULL if not registered */
screen_id_t   screen_base_for_mode(device_mode_t m);          /* SCR_COUNT if none */
screen_id_t   screen_base_for_instrument(uint8_t instrument); /* SCR_COUNT if none */
/* `ui screen`: "<base> > <layer>... gen=N" and, if the top has describe(), " | <its text>". */
size_t        screen_snapshot_text(char *out, size_t n);

/* Display task. Drawing and ticks start at the highest OPAQUE layer (or the
 * base) and run up to the top. */
void          screen_draw(scr_paint_t how, uint32_t frame);   /* UPDATE: the top only */
redraw_action_t screen_tick(uint32_t frame);                  /* the largest request of the visible layers */
bool          screen_popups_allowed(void);         /* false while an overlay is OPAQUE */
bool          screen_bars_wanted(void);            /* false if a visible layer has SCR_F_NO_BARS */
uint32_t      screen_epoch(void);                  /* mix of the visible layers' epochs */
uint16_t      screen_groups(void);                 /* union of the visible layers' groups */

#endif /* SCREEN_H */
