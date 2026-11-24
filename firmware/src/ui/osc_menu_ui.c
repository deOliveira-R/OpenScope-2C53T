/*
 * OpenScope 2C53T - Oscilloscope Settings page: the paint (UI round 1, R1-A)
 *
 * The model is osc_menu_model.c (pure, host-tested) and the page is a screen
 * of the router (osc_menu_screen.c): its table binds every pane and row to a
 * property, a key is one property set there, and this file is the paint
 * only. It runs on the display task and reads: the model's cursor, the bound
 * properties (osc_menu_bind_*), the measurement selection and the note.
 *
 * DRAWN AREA: y 16..223 (the status bar above and the info bar below stay
 * theirs), x 0..319: rail 96 px with 9 rows of 23 px, a 6 px notch column,
 * the pane x 102..319.
 */
#include "osc_menu_model.h"
#include "osc_menu_screen.h"
#include "menu_frame.h"
#include "meas_select.h"
#include "key_model.h"       /* the active channel: the one a spectrum view analyses */
#include "measure_strip.h"   /* MSTRIP_MAX_PER_CH */
#include "ui.h"
#include "lcd.h"
#include "font.h"
#include "theme.h"
#include "scope_measure.h"
#include <stdio.h>

#define PAGE_Y        16u
#define PAGE_H       208u
#define RAIL_W        96u
#define RAIL_ROW_H    23u
_Static_assert(OSC_ROW_COUNT * RAIL_ROW_H <= PAGE_H, "every rail row is on the page");
#define CONTENT_Y    (PAGE_Y + MENU_FRAME_TITLE_H + 2u)

static const char *rail_label(uint8_t row) { return osc_menu_row_label((osc_row_t)row); }

static const menu_frame_t k_frame = {
    .x = 0, .y = PAGE_Y, .w = LCD_WIDTH, .h = PAGE_H,
    .rail_w = RAIL_W, .row_h = RAIL_ROW_H, .n_rows = OSC_ROW_COUNT,
    .label = rail_label, .icon = NULL,   /* 12x12 icons: R1-B's icons.c, at the merge */
};

/* ═══════════════════════════════════════════════════════════════════
 * Live values: the properties the page is bound to (osc_menu_screen.h)
 * ═══════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════
 * Draw
 * ═══════════════════════════════════════════════════════════════════ */

#define ITEM_X     (RAIL_W + MENU_FRAME_NOTCH_W + 4u)   /* 106: panel left  */
#define ITEM_W     (LCD_WIDTH - ITEM_X - 4u)            /* 210: panel width */

#define RADIO_PITCH  27u
#define RADIO_ROWS   6u       /* the longest radio pane: Display */
_Static_assert(CONTENT_Y + RADIO_ROWS * RADIO_PITCH <= PAGE_Y + PAGE_H - MENU_FRAME_FOOTER_H,
               "the radio rows end above the footer");
_Static_assert(OSC_DISP_COUNT <= RADIO_ROWS && OSC_CUR_COUNT <= RADIO_ROWS &&
               OSC_PERS_COUNT <= RADIO_ROWS, "every radio pane fits");

static void draw_radio(const osc_menu_state_t *s)
{
    const osc_row_t row = (osc_row_t)s->row;
    const uint8_t n = osc_menu_layout(row)->n_items;
    const uint8_t cur = osc_menu_bind_current(row);
    for (uint8_t i = 0; i < n; i++) {
        menu_frame_radio(&k_frame, (uint16_t)(CONTENT_Y + i * RADIO_PITCH), 24, osc_menu_item_label(row, i, 0),
                         s->in_pane && s->item == i, cur == i, true);
    }
}

static void draw_values(const osc_menu_state_t *s)
{
    const osc_row_t row = (osc_row_t)s->row;
    for (uint8_t i = 0; i < osc_menu_layout(row)->n_items; i++) {
        char v[16];
        osc_menu_bind_text(row, i, v, sizeof v);
        menu_frame_value_row(&k_frame, (uint16_t)(CONTENT_Y + i * 30u), 26,
                             osc_menu_item_label(row, i, 0), v, s->in_pane && s->item == i, true);
    }
}

/* Chip grids (Measure: 3 columns, Math: 2 with a full-width OFF row) inside a
 * panel outlined in the channel's colour (Measure) or grey (Math). */
static void draw_chips(const osc_menu_state_t *s)
{
    const theme_t *th = theme_get();
    const osc_row_t row = (osc_row_t)s->row;
    const osc_pane_layout_t *l = osc_menu_layout(row);
    const bool param = (row == OSC_ROW_PARAM);
    const uint8_t set = s->meas_set;
    /* A panel is in its source's colour: the channel's, and for the spectrum
     * the channel it analyses (the active one). */
    const bool ch2 = (set == MEAS_SET_FFT) ? key_active_channel_get() != 0u : set == MEAS_SET_CH2;
    const uint16_t pc = param ? (ch2 ? th->ch2 : th->ch1) : th->text_secondary;
    const uint16_t gap = 4u, ch_h = 22u, pitch = 26u;
    const uint16_t in_x = ITEM_X + 4u, in_w = ITEM_W - 8u;
    const uint16_t cw = (uint16_t)((in_w - (l->cols - 1u) * gap) / l->cols);
    const uint16_t top = CONTENT_Y + 4u;
    const uint16_t panel_h = (uint16_t)(5u * pitch + 4u);

    /* Panel outline only (1 px), so the chips are not painted twice. */
    lcd_fill_rect(ITEM_X, CONTENT_Y, ITEM_W, 1, pc);
    lcd_fill_rect(ITEM_X, (uint16_t)(CONTENT_Y + panel_h - 1u), ITEM_W, 1, pc);
    lcd_fill_rect(ITEM_X, CONTENT_Y, 1, panel_h, pc);
    lcd_fill_rect((uint16_t)(ITEM_X + ITEM_W - 1u), CONTENT_Y, 1, panel_h, pc);

    const uint16_t mask = param ? meas_select_get(set) : 0u;
    const uint8_t cur_math = param ? 0u : osc_menu_bind_current(row);
    for (uint8_t i = 0; i < l->n_items; i++) {
        uint8_t r, c;
        osc_menu_chip_pos(row, i, &r, &c);
        const bool full = (l->span_first && i == 0u);
        const uint16_t x = (uint16_t)(in_x + c * (cw + gap));
        const uint16_t y = (uint16_t)(top + r * pitch);
        const uint16_t w = full ? in_w : cw;
        const bool focused = s->in_pane && s->item == i;
        const char *lab = osc_menu_item_label(row, i, set);
        if (param && i == OSC_PRM_TAB) {
            /* The panel's tab: filled in its colour, as stock. */
            menu_frame_box(x, y, w, ch_h, pc, focused ? th->text_primary : pc, focused ? 2u : 0u);
            font_draw_string_box((uint16_t)(x + 2u), (uint16_t)(y + (ch_h - font_medium.height) / 2u),
                                 (uint16_t)(w - 4u), lab, th->background, pc, &font_medium, FONT_ALIGN_CENTER);
            continue;
        }
        bool on = false;
        if (param && i >= OSC_PRM_CHIP0)
            on = (mask >> osc_menu_meas_of_chip(set, (uint8_t)(i - OSC_PRM_CHIP0))) & 1u;
        else if (!param)
            on = (i == cur_math);
        menu_frame_chip(x, y, w, ch_h, lab, param ? pc : th->highlight, on, focused, true);
    }
}

static void draw_footer(const osc_menu_state_t *s)
{
    char buf[48];
    const osc_pane_layout_t *l = osc_menu_layout((osc_row_t)s->row);
    const char *const note = osc_menu_screen_note();
    const char *t;
    if (note[0]) {
        t = note;
    } else if (!s->in_pane) {
        t = "OK: open   MENU: close";
    } else if (s->row == OSC_ROW_PARAM) {
        /* The strip shows at most MSTRIP_MAX_PER_CH of a set, picked by
         * measure_strip.c's priority table (levels first); the selection
         * itself keeps all of them (All = A-on), so say how many show. */
        const unsigned on = meas_select_count(s->meas_set);
        const char *const tab = osc_menu_item_label(OSC_ROW_PARAM, OSC_PRM_TAB, s->meas_set);
        if (on > MSTRIP_MAX_PER_CH)
            snprintf(buf, sizeof buf, "%s: %u on, %u shown", tab, on, (unsigned)MSTRIP_MAX_PER_CH);
        else
            snprintf(buf, sizeof buf, "%s: %u of %u on", tab, on, meas_select_size(s->meas_set));
        t = buf;
    } else if (l->kind == OSC_PANE_VALUES) {
        /* Locked (TRIGGER's pane): MENU closes, there is no rail to go back to. */
        t = s->locked ? "LEFT/RIGHT: change   MENU: close" : "LEFT/RIGHT: change   MENU: back";
    } else {
        t = s->locked ? "OK: select   MENU: close" : "OK: select   MENU: back";
    }
    menu_frame_draw_footer(&k_frame, t);
}

static uint8_t drawn_opens = 0xFFu, drawn_row = 0xFFu, drawn_set = 0xFFu;

void osc_menu_invalidate(void)
{
    drawn_row = 0xFFu;      /* the next osc_menu_draw() repaints the pane background */
}

void osc_menu_draw(void)
{
    static uint8_t first = 0;
    const osc_menu_state_t s = *osc_menu_state();   /* the input task writes it */
    if (!s.open) return;

    /* Repaint the pane background only when its content changes. */
    if (s.opens != drawn_opens || s.row != drawn_row ||
        (s.row == OSC_ROW_PARAM && s.meas_set != drawn_set)) {
        menu_frame_clear_pane(&k_frame);
        drawn_opens = s.opens;
        drawn_row = s.row;
        drawn_set = s.meas_set;
    }

    menu_frame_draw_rail(&k_frame, s.row, !s.in_pane, &first);
    menu_frame_draw_title(&k_frame, osc_menu_row_title((osc_row_t)s.row));
    switch (osc_menu_layout((osc_row_t)s.row)->kind) {
    case OSC_PANE_RADIO:  draw_radio(&s);  break;
    case OSC_PANE_VALUES: draw_values(&s); break;
    default:              draw_chips(&s);  break;
    }
    draw_footer(&s);
}
