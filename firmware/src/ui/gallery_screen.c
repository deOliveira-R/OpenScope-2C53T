/*
 * OpenScope 2C53T - the screenshot gallery as a screen. See gallery_screen.h.
 */
#include "gallery_screen.h"
#include "gallery_nav.h"
#include "gallery_store.h"
#include "theme.h"

#include <stdio.h>

/* UI state. Written by the input task (open, key); the display task and the
 * shell read it. Whether the gallery is up is the router's stack. */
static gallery_nav_t nav;
static uint8_t       opens;
static uint8_t       msg;
static uint32_t      deletes;      /* deletes made here (the store counts them too) */

bool gallery_ui_is_open(void) { return screen_is_open(SCR_GALLERY); }

gallery_view_t gallery_screen_view(void)
{
    gallery_view_t v;
    v.open    = gallery_ui_is_open();
    v.opens   = opens;
    v.sel     = nav.sel;
    v.full    = nav.full;
    v.armed   = nav.armed;
    v.msg     = msg;
    v.content = gallery_generation() + deletes;
    return v;
}

void gallery_screen_open(void)
{
    if (gallery_ui_is_open()) return;
    gallery_nav_open(&nav);
    msg = GALLERY_MSG_NONE;
    opens++;
    (void)screen_push(SCR_GALLERY);
}

/* ── input task ────────────────────────────────────────────────────── */

static gallery_nav_key_t map_key(scr_key_t k)
{
    switch (k.btn) {
    case BTN_LEFT:  return GNK_LEFT;
    case BTN_RIGHT: return GNK_RIGHT;
    case BTN_UP:    return GNK_UP;
    case BTN_DOWN:  return GNK_DOWN;
    case BTN_OK:    return k.is_long ? GNK_OTHER : GNK_OK;
    case BTN_SAVE:  return k.is_long ? GNK_SAVE_LONG : GNK_SAVE;
    case BTN_MENU:  return GNK_MENU;
    default:        return GNK_OTHER;
    }
}

/* Every key but POWER while the gallery is up. */
static scr_res_t gs_key(scr_key_t k)
{
    gallery_index_t idx;
    if (gallery_scan(&idx) != GALLERY_OK)
        return SCR_CLOSE;                      /* nothing to browse; any key leaves */
    switch (gallery_nav_key(&nav, map_key(k), idx.count)) {
    case GNA_NONE:
        return SCR_HANDLED;
    case GNA_MOVE:                             /* the title is not repainted: a message stays */
        return SCR_DRAW;
    case GNA_SHOW_FULL:
    case GNA_SHOW_GRID:
    case GNA_ARM_DELETE:
    case GNA_DISARM:
        msg = GALLERY_MSG_NONE;
        return SCR_DRAW;
    case GNA_DELETE: {
        const gallery_status_t st = gallery_delete(idx.seq[nav.sel]);
        gallery_index_t after;
        if (gallery_scan(&after) != GALLERY_OK)
            return SCR_CLOSE;
        (void)gallery_nav_deleted(&nav, after.count);
        deletes++;
        msg = (st != GALLERY_OK && !nav.full) ? GALLERY_MSG_DELETE_FAILED : GALLERY_MSG_NONE;
        return SCR_DRAW;
    }
    case GNA_CLOSE:
    default:
        return SCR_CLOSE;
    }
}

/* ── display task ──────────────────────────────────────────────────── */

uint8_t gallery_paint_plan(const gallery_view_t *drawn, const gallery_view_t *v, scr_paint_t how)
{
    if (!v->open) return 0u;
    /* A whole paint: the LCD is not the gallery's yet, another view, other
     * shots, or (full view) another shot. */
    if (how != SCR_PAINT_UPDATE || drawn == NULL || !drawn->open || v->opens != drawn->opens ||
        v->content != drawn->content || v->full != drawn->full || (v->full && v->sel != drawn->sel)) {
        uint8_t all = GALLERY_PAINT_ALL;
        if (v->armed) all |= GALLERY_PAINT_PROMPT;
        else if (v->msg != GALLERY_MSG_NONE && !v->full) all |= GALLERY_PAINT_TITLE;
        return all;
    }
    uint8_t plan = 0u;
    if (v->sel != drawn->sel) plan |= GALLERY_PAINT_CELLS;         /* the grid's selection moved */
    if (v->armed != drawn->armed || v->msg != drawn->msg) {
        if (v->armed)     plan |= GALLERY_PAINT_PROMPT;
        else if (v->full) plan |= GALLERY_PAINT_SHOT;
        else              plan |= GALLERY_PAINT_TITLE;
    }
    return plan;
}

static void gs_draw(scr_paint_t how, uint32_t frame)
{
    (void)frame;
    gallery_ui_paint(how);
}

/* Hash of what gallery_ui_paint() renders from: the view and the theme. */
static uint32_t gs_epoch(void)
{
    const gallery_view_t v = gallery_screen_view();
    uint32_t h = REDRAW_EPOCH_SEED;
    h = redraw_epoch_mix(h, (uint32_t)theme_get_id());
    h = redraw_epoch_mix(h, (uint32_t)v.open | ((uint32_t)v.full << 1) | ((uint32_t)v.armed << 2)
                            | ((uint32_t)v.msg << 8) | ((uint32_t)v.sel << 16) | ((uint32_t)v.opens << 24));
    h = redraw_epoch_mix(h, v.content);
    return h;
}

/* ── any task ──────────────────────────────────────────────────────── */

/* `ui screen`: "gal=grid/0", "gal=full/2", with "/del" while a delete is
 * armed. */
static void gs_describe(char *out, size_t n)
{
    const gallery_view_t v = gallery_screen_view();
    snprintf(out, n, "gal=%s/%u%s", v.full ? "full" : "grid", (unsigned)v.sel, v.armed ? "/del" : "");
}

const screen_t gallery_screen = {
    .name = "gallery", .id = SCR_GALLERY, .mode = MODE_COUNT, .instrument = SCR_INSTRUMENT_NONE,
    .flags = SCR_F_OPAQUE | SCR_F_GRAB | SCR_F_NO_BARS,
    .open = NULL, .close = NULL, .key = gs_key,
    .draw = gs_draw, .tick = NULL, .epoch = gs_epoch,
    .describe = gs_describe, .groups = 0,
};
