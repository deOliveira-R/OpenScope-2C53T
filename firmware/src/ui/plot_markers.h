/*
 * Plot-edge markers: channel ground (0 V) tags at the left edge, the trigger
 * level tag at the right edge, the trigger-position pointer at the top.
 *
 * Pure shapes (no LCD), host-tested in tests/test_measure_strip.c. Both
 * scope_ui.c renderers (the full path and the live column compositor) ask
 * plot_marker_px() for every pixel, so the two cannot draw different shapes.
 *
 * Stock (scope/IMG_0324.jpeg, lines.jpeg): ▶ in the channel colour at the left
 * edge on the channel's ground, ◀ in green at the right edge on the trigger
 * level, ▼ in green at the top over the trigger position. Ours (UI_STUDY S10):
 * a 9x9 tag - a 5 px body with a 4 px point - with the channel's number (or
 * "T") knocked out of the body, so two tags on top of each other still say
 * whose they are. When the level is above or below the band, the tag sits on
 * the band edge and points up or down instead: "off screen that way".
 */
#ifndef PLOT_MARKERS_H
#define PLOT_MARKERS_H

#include <stdbool.h>
#include <stdint.h>

#define PM_TAG_W   9
#define PM_TAG_H   9
#define PM_TPOS_W  9    /* trigger-position pointer */
#define PM_TPOS_H  5

typedef enum {
    PM_POINT_RIGHT = 0,   /* ▶ ground tag, at the left edge   */
    PM_POINT_LEFT,        /* ◀ trigger tag, at the right edge */
    PM_POINT_UP,          /* level above the band             */
    PM_POINT_DOWN,        /* level below the band             */
} pm_dir_t;

typedef enum { PM_PX_NONE = 0, PM_PX_FILL, PM_PX_KNOCK } pm_px_t;

/* Labels that can be knocked out of a tag body. */
typedef enum { PM_LABEL_NONE = 0, PM_LABEL_1, PM_LABEL_2, PM_LABEL_T } pm_label_t;

/* Pixel (dx, dy) of a tag, relative to its top-left; outside 9x9 = NONE. */
pm_px_t plot_marker_px(pm_dir_t dir, pm_label_t label, int dx, int dy);

/* The trigger-position pointer ▼, relative to its top-left; FILL or NONE. */
pm_px_t plot_marker_tpos_px(int dx, int dy);

/*
 * Where a tag goes. `row` is the level's row on the glass, unclamped (it may
 * lie outside [band_top, band_bot)). Edge x is the tag's left column. Returns
 * the tag's top-left and its direction: `horiz` (PM_POINT_RIGHT or
 * PM_POINT_LEFT) when the row is inside the band, PM_POINT_UP/DOWN at the band
 * edge otherwise. The tag always lies wholly inside [band_top, band_bot).
 */
pm_dir_t plot_marker_place(int row, int band_top, int band_bot, pm_dir_t horiz,
                           int *y0);

#endif /* PLOT_MARKERS_H */
