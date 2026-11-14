/*
 * OpenScope 2C53T - the screenshot gallery as a screen of the router
 * (screen.h; ARCH-1 B5)
 *
 * Long SAVE, from every screen: stock's 3x3 "nine palace grid" of the saved
 * screenshots, with a full-screen view and delete (parity C16). Storage is
 * gallery_store.h, the key logic gallery_nav.h (both pure). This file is the
 * screen: it owns the gallery's UI state (open, the selection, grid or full
 * view, an armed delete, a message) and its keys, on the input task. It
 * draws nothing: the paint is gallery_ui.c, on the display task, from a
 * snapshot of this state (gallery_screen_view), so two keys before a frame
 * are one paint of the state they left.
 *
 *   grid:  LEFT/RIGHT  previous/next shot (wraps)     UP/DOWN  row up/down
 *          OK          the selected shot full screen
 *          SAVE        arm delete; SAVE again deletes, any other key disarms
 *          MENU, long SAVE   close the gallery
 *   full:  LEFT/RIGHT  previous/next shot             OK, MENU back to the grid
 *          SAVE        arm / confirm delete as in the grid
 *
 * The layer is OPAQUE | GRAB | NO_BARS: it covers the whole LCD, takes every
 * key but POWER (the router never gives POWER to a layer; the POWER global
 * closes the gallery first) and nothing under it is drawn or ticked. A
 * delete is an action, not a setting: it goes to gallery_store.c from here.
 *
 * The open flag is the authority (the stack mirrors it, legacy_screens.c):
 * gallery_ui_close() from any task is a flag write, and the input task pops
 * the layer on its next pass.
 *
 * Pure: no FreeRTOS, no driver header. Host test: make test-gallery-screen.
 */
#ifndef GALLERY_SCREEN_H
#define GALLERY_SCREEN_H

#include <stdbool.h>
#include <stdint.h>
#include "screen.h"

extern const screen_t gallery_screen;

/* A message in the grid's title bar until the title is next repainted. */
typedef enum { GALLERY_MSG_NONE = 0, GALLERY_MSG_DELETE_FAILED } gallery_msg_t;

/* What the paint needs, taken in one look (the input task writes it). */
typedef struct {
    bool     open;
    uint8_t  opens;      /* bumped by every open: the next paint is a whole one   */
    uint8_t  sel;        /* selected shot, 0 = newest                             */
    bool     full;       /* full-screen view (else the grid)                      */
    bool     armed;      /* "SAVE again: delete <name>" is shown                  */
    uint8_t  msg;        /* gallery_msg_t                                         */
    uint32_t content;    /* the stored shots changed (a delete here, a save or
                          * delete elsewhere): the thumbnails are stale           */
} gallery_view_t;

gallery_view_t gallery_screen_view(void);          /* any task */

/* Input task: open it (long SAVE). No-op while open. The layer is pushed by
 * the stack mirror. */
void gallery_screen_open(void);

bool gallery_ui_is_open(void);                     /* any task */
/* Close it (if open), from any task: a mode change from the shell must not
 * leave the gallery owning the screen. The display task repaints what was
 * under it when it sees it gone. */
void gallery_ui_close(void);

/* What a paint has to draw to turn the view last drawn into `v`
 * (`drawn` NULL: nothing of the gallery is on the LCD). 0: nothing. */
enum {
    GALLERY_PAINT_ALL    = 1u << 0,   /* the whole view: the grid with its thumbnails, or the shot */
    GALLERY_PAINT_CELLS  = 1u << 1,   /* grid: the frames and names of drawn->sel and v->sel       */
    GALLERY_PAINT_PROMPT = 1u << 2,   /* "SAVE again: delete <name>" (title bar; full view: a strip) */
    GALLERY_PAINT_TITLE  = 1u << 3,   /* grid: the title bar again (the prompt went, or a message)  */
    GALLERY_PAINT_SHOT   = 1u << 4,   /* full view: the shot again (the prompt strip went)          */
};
uint8_t gallery_paint_plan(const gallery_view_t *drawn, const gallery_view_t *v, scr_paint_t how);

/* The paint (gallery_ui.c; a fake in the host test). Display task only.
 * UPDATE repaints what differs from the last paint (two cells for a move,
 * the title for a prompt, everything for a new view); FULL and RESTORE
 * repaint everything. */
void gallery_ui_paint(scr_paint_t how);

#endif /* GALLERY_SCREEN_H */
