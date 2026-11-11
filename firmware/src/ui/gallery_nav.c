/*
 * gallery_nav.c — see gallery_nav.h.
 */
#include "gallery_nav.h"

#include <string.h>

void gallery_nav_open(gallery_nav_t *n)
{
    memset(n, 0, sizeof *n);
}

static gallery_nav_action_t move_to(gallery_nav_t *n, int target, uint8_t count)
{
    if (count == 0u) return GNA_NONE;
    if (target < 0 || target >= (int)count || target == (int)n->sel) return GNA_NONE;
    n->prev_sel = n->sel;
    n->sel = (uint8_t)target;
    return n->full ? GNA_SHOW_FULL : GNA_MOVE;
}

gallery_nav_action_t gallery_nav_key(gallery_nav_t *n, gallery_nav_key_t k, uint8_t count)
{
    if (count > 0u && n->sel >= count) n->sel = (uint8_t)(count - 1u);

    /* An armed delete takes exactly one more key: SAVE confirms, anything
     * else only disarms (it does not also act, so a stray arrow after the
     * prompt never moves the selection off the shot the prompt named). */
    if (n->armed) {
        n->armed = false;
        if (k == GNK_SAVE && count > 0u) return GNA_DELETE;
        if (k == GNK_MENU || k == GNK_SAVE_LONG) return GNA_CLOSE;
        return GNA_DISARM;
    }

    switch (k) {
    case GNK_MENU:
        if (n->full) { n->full = false; return GNA_SHOW_GRID; }
        return GNA_CLOSE;
    case GNK_SAVE_LONG:
        return GNA_CLOSE;
    case GNK_OK:
        if (n->full) { n->full = false; return GNA_SHOW_GRID; }
        if (count == 0u) return GNA_NONE;
        n->full = true;
        return GNA_SHOW_FULL;
    case GNK_SAVE:
        if (count == 0u) return GNA_NONE;
        n->armed = true;
        return GNA_ARM_DELETE;
    case GNK_LEFT:
        if (count == 0u) return GNA_NONE;
        return move_to(n, (n->sel == 0u) ? (int)count - 1 : (int)n->sel - 1, count);
    case GNK_RIGHT:
        if (count == 0u) return GNA_NONE;
        return move_to(n, (n->sel + 1u >= count) ? 0 : (int)n->sel + 1, count);
    case GNK_UP:
        if (n->full) return GNA_NONE;
        return move_to(n, (int)n->sel - (int)GALLERY_NAV_COLS, count);
    case GNK_DOWN:
        if (n->full) return GNA_NONE;
        return move_to(n, (int)n->sel + (int)GALLERY_NAV_COLS, count);
    case GNK_OTHER:
    default:
        return GNA_NONE;
    }
}

gallery_nav_action_t gallery_nav_deleted(gallery_nav_t *n, uint8_t count)
{
    n->armed = false;
    if (count == 0u) {
        n->sel = 0u;
        n->full = false;
        return GNA_SHOW_GRID;
    }
    if (n->sel >= count) n->sel = (uint8_t)(count - 1u);
    n->prev_sel = n->sel;
    return n->full ? GNA_SHOW_FULL : GNA_SHOW_GRID;
}
