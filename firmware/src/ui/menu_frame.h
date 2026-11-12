/*
 * OpenScope 2C53T - two-pane menu frame (UI round 1, R1-A; reused by R2-D)
 *
 * Stock's settings pages are a left rail of categories and a right pane of
 * options, the selected rail row filled and notched toward the pane
 * (research/ui/UI_STUDY.md 2.3, 4). This is that frame, generic: the caller
 * gives the geometry, the row labels and (optionally) an icon painter, and
 * draws its own pane with the element painters below. Oscilloscope Settings
 * (osc_menu_ui.c) is the first user; System Settings is the next.
 *
 * Every painter writes each pixel of its element once and opaque (no
 * clear-then-draw), so a repaint on a key press does not flash. Colours come
 * from the theme (theme.h): rail = status_bar_bg, pane = background, selection
 * = menu_selected_bg with a highlight accent.
 *
 * The scroll rule (menu_frame_first) is pure and header-only, so host tests
 * can check it without linking the LCD.
 */
#ifndef MENU_FRAME_H
#define MENU_FRAME_H

#include <stdint.h>
#include <stdbool.h>

#define MENU_FRAME_NOTCH_W   6u    /* the selection's triangular notch */
#define MENU_FRAME_ICON_W   12u    /* icon box, when an icon painter is given */

/* Paints row `row`'s 12x12 icon at (x, y) in fg over bg. */
typedef void (*menu_frame_icon_fn)(uint8_t row, uint16_t x, uint16_t y, uint16_t fg, uint16_t bg);
typedef const char *(*menu_frame_label_fn)(uint8_t row);

typedef struct {
    uint16_t x, y, w, h;          /* the whole frame */
    uint16_t rail_w;              /* rail width; the pane starts after the notch */
    uint8_t  row_h;               /* rail row pitch */
    uint8_t  n_rows;
    menu_frame_label_fn label;    /* one line each: labels never wrap */
    menu_frame_icon_fn  icon;     /* NULL: no icon column */
} menu_frame_t;

/* Rows that fit the rail. */
static inline uint8_t menu_frame_visible(const menu_frame_t *f)
{
    return (uint8_t)(f->row_h ? f->h / f->row_h : 0u);
}

/* First rail row to show so `sel` is visible, moving the window as little as
 * possible from `first` (no jump when the selection is already on screen). */
static inline uint8_t menu_frame_first(uint8_t sel, uint8_t n_rows, uint8_t visible, uint8_t first)
{
    if (visible == 0u || n_rows <= visible) return 0u;
    if (sel >= n_rows) sel = (uint8_t)(n_rows - 1u);
    if (first > n_rows - visible) first = (uint8_t)(n_rows - visible);
    if (sel < first) return sel;
    if (sel >= first + visible) return (uint8_t)(sel - visible + 1u);
    return first;
}

/* Pane rectangle (right of the notch). */
static inline uint16_t menu_frame_pane_x(const menu_frame_t *f) { return (uint16_t)(f->x + f->rail_w + MENU_FRAME_NOTCH_W); }
static inline uint16_t menu_frame_pane_w(const menu_frame_t *f) { return (uint16_t)(f->w - f->rail_w - MENU_FRAME_NOTCH_W); }

/* The rail and the notch column. `rail_focus` = the arrows are on the rail
 * (accent bar lit); otherwise the selection stays filled but quiet. *first is
 * the scroll window, updated with menu_frame_first. */
void menu_frame_draw_rail(const menu_frame_t *f, uint8_t sel, bool rail_focus, uint8_t *first);

/* Paint the whole pane background. Call when the pane's content changes
 * (another row), not on every key: the element painters are opaque. */
void menu_frame_clear_pane(const menu_frame_t *f);

/* Pane title (font_medium, highlight colour) on the pane's first 20 px. */
#define MENU_FRAME_TITLE_H  20u
void menu_frame_draw_title(const menu_frame_t *f, const char *title);

/* Footer line (font_small, secondary) on the pane's last 14 px: hints,
 * refusals ("Keep one CH on"). NULL or "" paints it empty. */
#define MENU_FRAME_FOOTER_H 14u
void menu_frame_draw_footer(const menu_frame_t *f, const char *text);

/* ── Pane elements ──────────────────────────────────────────────────── */

/* Opaque rectangle with a `bw`-pixel border (bw 0: plain fill). */
void menu_frame_box(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                    uint16_t fill, uint16_t border, uint8_t bw);

/* Radio row across the pane: a disc (filled when `checked`), the label.
 * `focused` = under the pane cursor; `enabled` false dims it (not built). */
void menu_frame_radio(const menu_frame_t *f, uint16_t y, uint16_t h, const char *label,
                      bool focused, bool checked, bool enabled);

/* "Label:" on the left, value in an outlined box on the right (stock's
 * CH1 Settings rows). */
void menu_frame_value_row(const menu_frame_t *f, uint16_t y, uint16_t h, const char *label,
                          const char *value, bool focused, bool enabled);

/* Chip: label centred in a rounded-look box. `on` = selected (2 px border
 * and text in `on_color`); `focused` = under the cursor (filled). */
void menu_frame_chip(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const char *label,
                     uint16_t on_color, bool on, bool focused, bool enabled);

#endif /* MENU_FRAME_H */
