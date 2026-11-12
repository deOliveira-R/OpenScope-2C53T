/*
 * Plot-edge marker shapes. See plot_markers.h. Pure: no LCD.
 */
#include "plot_markers.h"

/* 3x5 knock-out glyphs, one row per byte, bit 2 = left column. */
static const uint8_t k_glyph[4][5] = {
    { 0, 0, 0, 0, 0 },                          /* none */
    { 0x2, 0x6, 0x2, 0x2, 0x7 },                /* 1 */
    { 0x6, 0x1, 0x2, 0x4, 0x7 },                /* 2 */
    { 0x7, 0x2, 0x2, 0x2, 0x2 },                /* T */
};

static int iabs(int v) { return v < 0 ? -v : v; }

pm_px_t plot_marker_px(pm_dir_t dir, pm_label_t label, int dx, int dy)
{
    if (dx < 0 || dy < 0 || dx >= PM_TAG_W || dy >= PM_TAG_H)
        return PM_PX_NONE;

    switch (dir) {
    case PM_POINT_RIGHT:
    case PM_POINT_LEFT: {
        /* RIGHT: body cols 0-4, point cols 5-8. LEFT is its mirror, but the
         * glyph is not mirrored: it is read in screen columns. */
        const int s = (dir == PM_POINT_LEFT) ? PM_TAG_W - 1 - dx : dx;
        if (s > 4 && (s - 4) + iabs(dy - 4) > 4)
            return PM_PX_NONE;                  /* outside the point */
        const int g0 = (dir == PM_POINT_LEFT) ? 5 : 1;   /* glyph's left column */
        const int gx = dx - g0;
        if ((unsigned)label < 4u && gx >= 0 && gx <= 2 && dy >= 2 && dy <= 6) {
            const uint8_t row = k_glyph[label][dy - 2];
            if (row & (1u << (2 - gx)))
                return PM_PX_KNOCK;
        }
        return PM_PX_FILL;
    }
    case PM_POINT_DOWN:
        dy = PM_TAG_H - 1 - dy;                 /* mirror of UP */
        /* fall through */
    case PM_POINT_UP:
        /* body: rows 4-8 full width; point above. No label: the body is
         * only 5 rows, and the colour says whose it is. */
        if (dy < 4 && (4 - dy) + iabs(dx - 4) > 4)
            return PM_PX_NONE;
        return PM_PX_FILL;
    default:
        return PM_PX_NONE;
    }
}

pm_px_t plot_marker_tpos_px(int dx, int dy)
{
    if (dx < 0 || dy < 0 || dx >= PM_TPOS_W || dy >= PM_TPOS_H)
        return PM_PX_NONE;
    return (iabs(dx - 4) <= 4 - dy) ? PM_PX_FILL : PM_PX_NONE;
}

pm_dir_t plot_marker_place(int row, int band_top, int band_bot, pm_dir_t horiz,
                           int *y0)
{
    int y;
    pm_dir_t d = horiz;
    if (row < band_top) {
        d = PM_POINT_UP;
        y = band_top;
    } else if (row >= band_bot) {
        d = PM_POINT_DOWN;
        y = band_bot - PM_TAG_H;
    } else {
        y = row - PM_TAG_H / 2;                 /* centred on the level */
        if (y < band_top) y = band_top;
        if (y > band_bot - PM_TAG_H) y = band_bot - PM_TAG_H;
    }
    if (y < band_top) y = band_top;             /* band shorter than a tag */
    if (y0) *y0 = y;
    return d;
}
