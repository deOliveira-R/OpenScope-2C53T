/*
 * OpenScope 2C53T - Oscilloscope Settings page: apply and draw (UI round 1, R1-A)
 *
 * The model is osc_menu_model.c (pure, host-tested). This file is the other
 * half of the fixed interface: osc_menu_key() runs a key through the model and
 * applies what it asks through the scope code that already exists, and
 * osc_menu_draw() paints the page with the menu_frame widget.
 *
 * WHAT IS APPLIED, AND HOW (nothing reimplemented: the glue the keys and the
 * Settings list use is shared, src/tasks/scope_apply.c, since the merge):
 *   Display   the view.mode property (model: the view, the FFT pool and
 *             Roll in one setter, the old view kept if a step is refused)
 *   Cursors   ss->cursor.mode / .active (scope_state.c's per-mode rule)
 *   Measure   meas_select.c (in the model)
 *   CH1/CH2   fpga_apply_channel_enables() (reverted when both would be off),
 *             probe, fpga_apply_coupling() (label changes only if the relay
 *             did). No FFT row: the spectrum views are Display items and
 *             analyse the active channel (dsp/fft_source.h)
 *   Trigger   trig_menu_change() + fpga_apply_trigger_source() / SINGLE re-arm
 *   Persist   persist_init/deinit/set_mode (the pool is shared with the FFT)
 *   Math      math_enabled / math_op
 *
 * DRAWN AREA: y 16..223 (the status bar above and the info bar below stay
 * theirs), x 0..319: rail 96 px with 8 rows of 26 px, a 6 px notch column,
 * the pane x 102..319.
 */
#include "osc_menu_model.h"
#include "menu_frame.h"
#include "meas_select.h"
#include "measure_strip.h"   /* MSTRIP_MAX_PER_CH */
#include "ui.h"
#include "lcd.h"
#include "font.h"
#include "theme.h"
#include "scope_state.h"
#include "scope_measure.h"
#include "trig_menu.h"
#include "persistence.h"
#include "math_channel.h"
#include "fpga.h"
#include "scope_apply.h"
#include "model.h"
#include <stdio.h>

#define PAGE_Y        16u
#define PAGE_H       208u
#define RAIL_W        96u
#define RAIL_ROW_H    26u
#define CONTENT_Y    (PAGE_Y + MENU_FRAME_TITLE_H + 2u)

static const char *rail_label(uint8_t row) { return osc_menu_row_label((osc_row_t)row); }

static const menu_frame_t k_frame = {
    .x = 0, .y = PAGE_Y, .w = LCD_WIDTH, .h = PAGE_H,
    .rail_w = RAIL_W, .row_h = RAIL_ROW_H, .n_rows = OSC_ROW_COUNT,
    .label = rail_label, .icon = NULL,   /* 12x12 icons: R1-B's icons.c, at the merge */
};

/* A one-line result of the last key ("Keep one CH on"); "" = show the hint.
 * g_note_opens: the osc_menu_state()->opens it was written under. A note
 * from an earlier visit (or from before a deep-link jump) is not shown: the
 * input task writes both, the display task only compares. */
static char    g_note[28];
static uint8_t g_note_opens;

/* ═══════════════════════════════════════════════════════════════════
 * Live values
 * ═══════════════════════════════════════════════════════════════════ */

/* The item of the view in force: view.mode, through the model. */
static uint8_t display_current(void)
{
    return osc_menu_disp_item(prop_get(P(VIEW_MODE)));
}

static bool display_available(uint8_t item)
{
#ifdef FEATURE_FFT
    (void)item;
    return true;
#else
    return item == OSC_DISP_YT || item == OSC_DISP_ROLL;
#endif
}

static uint8_t cursor_current(void)
{
    switch (scope_state_get()->cursor.mode) {
    case CURSOR_VERTICAL:   return OSC_CUR_TIME;    /* X1/X2: vertical lines, time */
    case CURSOR_HORIZONTAL: return OSC_CUR_VOLT;    /* Y1/Y2: horizontal lines, volts */
    case CURSOR_BOTH:       return OSC_CUR_BOTH;
    default:                return OSC_CUR_OFF;
    }
}

static const persist_mode_t k_pers_mode[OSC_PERS_COUNT] = {
    PERSIST_OFF, PERSIST_LOW, PERSIST_MEDIUM, PERSIST_HIGH, PERSIST_INFINITE,
};

static uint8_t persist_current(void)
{
    if (!persist_enabled) return OSC_PERS_OFF;
    const persist_mode_t m = persist_get_mode();
    for (uint8_t i = 1; i < OSC_PERS_COUNT; i++)
        if (k_pers_mode[i] == m) return i;
    return OSC_PERS_OFF;
}

static uint8_t math_current(void)
{
    return math_enabled ? osc_menu_math_item(math_op) : (uint8_t)OSC_MATH_OFF;
}

static uint8_t radio_current(osc_row_t row)
{
    switch (row) {
    case OSC_ROW_DISPLAY: return display_current();
    case OSC_ROW_CURSOR:  return cursor_current();
    case OSC_ROW_PERSIST: return persist_current();
    case OSC_ROW_MATH:    return math_current();
    default:              return 0;
    }
}

static channel_state_t *chan(uint8_t ch)
{
    scope_state_t *ss = scope_state_get();
    return ch ? &ss->ch2 : &ss->ch1;
}

/* ═══════════════════════════════════════════════════════════════════
 * Apply
 * ═══════════════════════════════════════════════════════════════════ */

/* The view, the FFT pool and Roll are one property, view.mode (this is the
 * executor: the key, the shell's `set view.mode` and a host are one path).
 * Its setter leaves the old view when a step is refused and says why. */
static void display_select(uint8_t item)
{
    if (!display_available(item)) {
        snprintf(g_note, sizeof g_note, "Not in this build");
        return;
    }
    const int8_t vm = osc_menu_disp_view_mode(item);
    if (vm < 0) return;
    prop_note_t n;
    const prop_status_t st = prop_set_local(P(VIEW_MODE), vm, ORIGIN_KEY, &n);
    if (st != PS_OK && st != PS_CLAMPED)
        snprintf(g_note, sizeof g_note, "%s", n.text[0] ? n.text : "view NOT SET");
}

static void cursor_select(uint8_t item)
{
    static const cursor_mode_t k_mode[OSC_CUR_COUNT] = {
        CURSOR_OFF, CURSOR_VERTICAL, CURSOR_HORIZONTAL, CURSOR_BOTH,
    };
    cursor_state_t *c = &scope_state_get()->cursor;
    c->mode   = k_mode[item];
    c->active = (c->mode == CURSOR_HORIZONTAL) ? CURSOR_SEL_H1 : CURSOR_SEL_V1;
}

static void persist_select(uint8_t item)
{
    if (item == OSC_PERS_OFF) {
        scope_apply_persist_off();
        return;
    }
    if (!scope_apply_persist_on(k_pers_mode[item]))   /* leaves a spectrum view first */
        snprintf(g_note, sizeof g_note, "No memory for persistence");
}

static void math_select(uint8_t item)
{
    const uint8_t op = osc_menu_math_op(item);
    if (op >= MATH_COUNT) {
        math_enabled = false;
        return;
    }
    math_op = op;
    math_enabled = true;
}

static void channel_change(uint8_t ch, uint8_t item, int dir)
{
    channel_state_t *c = chan(ch);
    switch (item) {
    case OSC_CH_ENABLE: {
        const bool was = c->enabled;
        c->enabled = !was;
        if (!fpga_apply_channel_enables()) {
            c->enabled = was;
            snprintf(g_note, sizeof g_note, "Keep one CH on");
            break;
        }
        if (!c->enabled && active_channel == ch)
            active_channel = (uint8_t)(1u - ch);       /* the arrows follow the live one */
        if (fpga_interleave_active())
            snprintf(g_note, sizeof g_note, "CH%u alone: 250 MS/s", (unsigned)(2u - ch));
        break;
    }
    case OSC_CH_PROBE:
        c->probe = (probe_t)((c->probe + PROBE_COUNT + dir) % PROBE_COUNT);
        break;
    case OSC_CH_COUPLING: {
        /* DC <-> AC on the relay; GND is not offered (no ground path known). */
        char t[sizeof g_note];
        if (!scope_apply_coupling_toggle(ch, t, sizeof t))
            snprintf(g_note, sizeof g_note, "%s", t);
        break;
    }
    default:
        break;
    }
}

static void trigger_change(uint8_t item, int dir)
{
    trig_menu_t m = { item };
    char t[sizeof g_note];
    /* The row shows the new value; the note only what it cannot (a refusal,
     * NOT SET, the SINGLE arm). */
    if (scope_apply_trigger_change(trig_menu_change(&m, scope_state_get(), dir), t, sizeof t))
        snprintf(g_note, sizeof g_note, "%s", t);
}

osc_result_t osc_menu_key(osc_key_t k)
{
    osc_action_t a;
    const osc_result_t r = osc_menu_step(k, &a);
    /* A key that redraws nothing keeps the note on screen (this visit's). */
    if (r != OSC_RES_NONE || g_note_opens != osc_menu_state()->opens) g_note[0] = '\0';
    g_note_opens = osc_menu_state()->opens;
    const osc_row_t row = (osc_row_t)a.row;

    switch (a.kind) {
    case OSC_ACT_ENTER:
        if (osc_menu_layout(row)->kind == OSC_PANE_RADIO || row == OSC_ROW_MATH)
            osc_menu_set_item(radio_current(row));
        break;
    case OSC_ACT_SELECT:
        if (row == OSC_ROW_DISPLAY)      display_select(a.item);
        else if (row == OSC_ROW_CURSOR)  cursor_select(a.item);
        else if (row == OSC_ROW_PERSIST) persist_select(a.item);
        else if (row == OSC_ROW_MATH)    math_select(a.item);
        break;
    case OSC_ACT_CHANGE:
        if (row == OSC_ROW_CH1 || row == OSC_ROW_CH2)
            channel_change(row == OSC_ROW_CH2 ? 1u : 0u, a.item, a.dir);
        else if (row == OSC_ROW_TRIGGER)
            trigger_change(a.item, a.dir);
        break;
    default:
        break;
    }
    return r;
}

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
    const uint8_t cur = radio_current(row);
    for (uint8_t i = 0; i < n; i++) {
        const bool ok = (row != OSC_ROW_DISPLAY) || display_available(i);
        menu_frame_radio(&k_frame, (uint16_t)(CONTENT_Y + i * RADIO_PITCH), 24, osc_menu_item_label(row, i, 0),
                         s->in_pane && s->item == i, cur == i, ok);
    }
}

static void draw_values(const osc_menu_state_t *s)
{
    const osc_row_t row = (osc_row_t)s->row;
    const scope_state_t *ss = scope_state_get();
    for (uint8_t i = 0; i < osc_menu_layout(row)->n_items; i++) {
        const char *v = "";
        bool ok = true;
        if (row == OSC_ROW_TRIGGER) {
            v = trig_menu_row_value(ss, i);
        } else {
            const uint8_t ch = (row == OSC_ROW_CH2) ? 1u : 0u;
            const channel_state_t *c = ch ? &ss->ch2 : &ss->ch1;
            switch (i) {
            case OSC_CH_ENABLE:   v = c->enabled ? "ON" : "OFF"; break;
            case OSC_CH_PROBE:    v = probe_labels[c->probe % PROBE_COUNT]; break;
            case OSC_CH_COUPLING: v = coupling_labels[c->coupling % COUPLING_COUNT]; break;
            default: break;
            }
        }
        menu_frame_value_row(&k_frame, (uint16_t)(CONTENT_Y + i * 30u), 26,
                             osc_menu_item_label(row, i, 0), v, s->in_pane && s->item == i, ok);
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
    const uint8_t ch = s->meas_ch;
    const uint16_t pc = param ? (ch ? th->ch2 : th->ch1) : th->text_secondary;
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

    const uint16_t mask = param ? meas_select_get(ch) : 0u;
    const uint8_t cur_math = param ? 0u : math_current();
    for (uint8_t i = 0; i < l->n_items; i++) {
        uint8_t r, c;
        osc_menu_chip_pos(row, i, &r, &c);
        const bool full = (l->span_first && i == 0u);
        const uint16_t x = (uint16_t)(in_x + c * (cw + gap));
        const uint16_t y = (uint16_t)(top + r * pitch);
        const uint16_t w = full ? in_w : cw;
        const bool focused = s->in_pane && s->item == i;
        const char *lab = osc_menu_item_label(row, i, ch);
        if (param && i == OSC_PRM_TAB) {
            /* The channel tab: filled in the channel colour, as stock. */
            menu_frame_box(x, y, w, ch_h, pc, focused ? th->text_primary : pc, focused ? 2u : 0u);
            font_draw_string_box((uint16_t)(x + 2u), (uint16_t)(y + (ch_h - font_medium.height) / 2u),
                                 (uint16_t)(w - 4u), lab, th->background, pc, &font_medium, FONT_ALIGN_CENTER);
            continue;
        }
        bool on = false;
        if (param && i >= OSC_PRM_CHIP0)
            on = (mask >> osc_menu_prm_of_chip((uint8_t)(i - OSC_PRM_CHIP0))) & 1u;
        else if (!param)
            on = (i == cur_math);
        menu_frame_chip(x, y, w, ch_h, lab, param ? pc : th->highlight, on, focused, true);
    }
}

static void draw_footer(const osc_menu_state_t *s)
{
    char buf[48];
    const osc_pane_layout_t *l = osc_menu_layout((osc_row_t)s->row);
    const char *t;
    if (g_note[0] && g_note_opens == s->opens) {
        t = g_note;
    } else if (!s->in_pane) {
        t = "OK: open   MENU: close";
    } else if (s->row == OSC_ROW_PARAM) {
        /* The strip shows at most MSTRIP_MAX_PER_CH per channel, picked by
         * measure_strip.c's priority table (levels first); the selection
         * itself keeps all 12 (All = A-on), so say how many show. */
        const unsigned on = meas_select_count(s->meas_ch);
        if (on > MSTRIP_MAX_PER_CH)
            snprintf(buf, sizeof buf, "CH%u: %u on, %u shown   %s", (unsigned)s->meas_ch + 1u,
                     on, (unsigned)MSTRIP_MAX_PER_CH, s->meas_ch ? "UP: CH1" : "DOWN: CH2");
        else
            snprintf(buf, sizeof buf, "CH%u: %u of 12 on   %s", (unsigned)s->meas_ch + 1u,
                     on, s->meas_ch ? "UP: CH1" : "DOWN: CH2");
        t = buf;
    } else if (l->kind == OSC_PANE_VALUES) {
        /* Locked (TRIGGER's pane): MENU closes, there is no rail to go back to. */
        t = s->locked ? "LEFT/RIGHT: change   MENU: close" : "LEFT/RIGHT: change   MENU: back";
    } else {
        t = s->locked ? "OK: select   MENU: close" : "OK: select   MENU: back";
    }
    menu_frame_draw_footer(&k_frame, t);
}

static uint8_t drawn_opens = 0xFFu, drawn_row = 0xFFu, drawn_ch = 0xFFu;

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
        (s.row == OSC_ROW_PARAM && s.meas_ch != drawn_ch)) {
        menu_frame_clear_pane(&k_frame);
        drawn_opens = s.opens;
        drawn_row = s.row;
        drawn_ch = s.meas_ch;
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
