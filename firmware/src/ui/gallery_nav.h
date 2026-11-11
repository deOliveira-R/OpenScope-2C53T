/*
 * gallery_nav — key logic of the screenshot gallery (parity C16), pure.
 *
 * Stock: long SAVE opens a 3x3 "nine palace grid" of saved screenshots with
 * delete (manual; key handler for long SAVE in scope state ms[0xF68] == 2).
 * Our key meanings, chosen to match the rest of OpenScope's keys:
 *
 *   grid:  LEFT/RIGHT  previous/next cell (wraps)     UP/DOWN  row up/down
 *          OK          open the selected shot full screen
 *          SAVE        arm delete; SAVE again deletes, any other key disarms
 *          MENU, long SAVE   close the gallery
 *   full:  LEFT/RIGHT  previous/next shot             OK, MENU back to the grid
 *          SAVE        arm/confirm delete as in the grid
 *
 * POWER is never claimed (the power-off countdown must always work); the
 * caller closes the gallery and passes POWER on.
 *
 * Cells are numbered newest first (index 0 = the latest save), so the grid
 * reads like a camera roll.
 */
#ifndef GALLERY_NAV_H
#define GALLERY_NAV_H

#include <stdbool.h>
#include <stdint.h>

#define GALLERY_NAV_COLS  3u

/* Key ids the nav understands (the caller maps button_id_t onto these, so this
 * module does not depend on ui.h and its RTOS externs). */
typedef enum {
    GNK_LEFT, GNK_RIGHT, GNK_UP, GNK_DOWN, GNK_OK, GNK_SAVE, GNK_MENU,
    GNK_SAVE_LONG, GNK_OTHER
} gallery_nav_key_t;

typedef enum {
    GNA_NONE = 0,       /* nothing to do                                    */
    GNA_MOVE,           /* selection changed: redraw old + new cell (grid)  */
    GNA_SHOW_FULL,      /* draw shot `sel` full screen                      */
    GNA_SHOW_GRID,      /* draw the whole grid                              */
    GNA_ARM_DELETE,     /* show "SAVE again to delete"                     */
    GNA_DISARM,         /* remove that prompt (redraw current view)         */
    GNA_DELETE,         /* delete shot `sel`, then call gallery_nav_deleted */
    GNA_CLOSE,          /* leave the gallery                                */
} gallery_nav_action_t;

typedef struct {
    uint8_t sel;        /* selected cell, 0 = newest                         */
    uint8_t prev_sel;   /* cell to repaint after GNA_MOVE                    */
    bool    full;       /* full-screen view (else grid)                      */
    bool    armed;      /* delete armed                                      */
} gallery_nav_t;

void gallery_nav_open(gallery_nav_t *n);

/* One key; count = shots currently stored. */
gallery_nav_action_t gallery_nav_key(gallery_nav_t *n, gallery_nav_key_t k, uint8_t count);

/* After a delete: count = shots left. Keeps the selection on the shot that
 * took the deleted one's place (or the new last one); an empty gallery falls
 * back to the grid. Returns the view to draw (GNA_SHOW_GRID / GNA_SHOW_FULL). */
gallery_nav_action_t gallery_nav_deleted(gallery_nav_t *n, uint8_t count);

#endif /* GALLERY_NAV_H */
