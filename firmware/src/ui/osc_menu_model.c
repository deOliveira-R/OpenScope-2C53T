/*
 * OpenScope 2C53T - Oscilloscope Settings page model (see osc_menu_model.h)
 *
 * Pure: no LCD, no FPGA, no RTOS. The input task is the only writer.
 */
#include "osc_menu_model.h"
#include "meas_select.h"
#include "scope_measure.h"

static osc_menu_state_t g_m;

/* ── Tables ─────────────────────────────────────────────────────────── */

static const osc_pane_layout_t k_layout[OSC_ROW_COUNT] = {
    [OSC_ROW_DISPLAY] = { OSC_PANE_RADIO,  OSC_DISP_COUNT, 1, 0 },
    [OSC_ROW_CURSOR]  = { OSC_PANE_RADIO,  OSC_CUR_COUNT,  1, 0 },
    [OSC_ROW_PARAM]   = { OSC_PANE_CHIPS,  OSC_PRM_COUNT,  3, 0 },
    [OSC_ROW_CH1]     = { OSC_PANE_VALUES, OSC_CH_COUNT,   1, 0 },
    [OSC_ROW_CH2]     = { OSC_PANE_VALUES, OSC_CH_COUNT,   1, 0 },
    [OSC_ROW_TRIGGER] = { OSC_PANE_VALUES, OSC_TRIG_COUNT, 1, 0 },
    [OSC_ROW_PERSIST] = { OSC_PANE_RADIO,  OSC_PERS_COUNT, 1, 0 },
    [OSC_ROW_MATH]    = { OSC_PANE_CHIPS,  OSC_MATH_COUNT, 2, 1 },
};

static const char *const k_row_label[OSC_ROW_COUNT] = {
    "Display", "Cursors", "Measure", "CH1", "CH2", "Trigger", "Persist", "Math",
};
static const char *const k_row_title[OSC_ROW_COUNT] = {
    "Display Mode", "Cursor Measurement", "Parameter Measurement", "CH1 Settings",
    "CH2 Settings", "Trigger Settings", "Persistence", "Mathematics",
};

static const char *const k_disp[OSC_DISP_COUNT]  = { "Y-T", "Roll", "X-Y", "FFT", "Split", "Waterfall" };
static const char *const k_cur[OSC_CUR_COUNT]    = { "Off", "X1-X2  (time)", "Y1-Y2  (volts)", "Both" };
static const char *const k_ch[OSC_CH_COUNT]      = { "Channel:", "Probe:", "Coupling:" };
static const char *const k_trig[OSC_TRIG_COUNT]  = { "Mode:", "Edge:", "Channel:" };
static const char *const k_pers[OSC_PERS_COUNT]  = { "Off", "500 ms", "1 s", "2 s", "Infinite" };
static const char *const k_math[OSC_MATH_COUNT]  = {
    "Off", "CH1+CH2", "-CH1", "CH1-CH2", "-CH2", "CH1*CH2", "|CH1|", "CH1/CH2", "|CH2|",
};

/* Stock's 3x4 grid, row-major (parameter_measurement.jpeg). */
static const uint8_t k_chip_prm[12] = {
    SCOPE_PRM_FREQ,     SCOPE_PRM_WID_POS, SCOPE_PRM_VPP,
    SCOPE_PRM_PERI,     SCOPE_PRM_WID_NEG, SCOPE_PRM_AMP,
    SCOPE_PRM_DUTY_POS, SCOPE_PRM_VMAX,    SCOPE_PRM_VRMS,
    SCOPE_PRM_DUTY_NEG, SCOPE_PRM_VMIN,    SCOPE_PRM_AVG,
};

/* math_op_t values (math_channel.h: ADD SUB MUL DIV INV_A INV_B ABS_A ABS_B),
 * in stock's 2x4 order (mathmatics.jpeg). Numeric so this file needs no DSP
 * header; test_osc_menu.c checks them against math_channel.h. */
static const uint8_t k_math_op[OSC_MATH_COUNT] = { 0xFF, 0, 4, 1, 5, 2, 6, 3, 7 };

/* ── Lookups ────────────────────────────────────────────────────────── */

const osc_pane_layout_t *osc_menu_layout(osc_row_t row)
{
    return ((unsigned)row < OSC_ROW_COUNT) ? &k_layout[row] : &k_layout[OSC_ROW_DISPLAY];
}

const char *osc_menu_row_label(osc_row_t row)
{
    return ((unsigned)row < OSC_ROW_COUNT) ? k_row_label[row] : "";
}

const char *osc_menu_row_title(osc_row_t row)
{
    return ((unsigned)row < OSC_ROW_COUNT) ? k_row_title[row] : "";
}

/* view.mode per Display item (props_scope.c: yt roll xy fft split
 * waterfall); -1 = the item is not a view. test_osc_menu.c checks the pairs
 * against the property's own value names. */
static const int8_t k_disp_view_mode[OSC_DISP_COUNT] = {
    [OSC_DISP_YT] = 0, [OSC_DISP_ROLL] = 1, [OSC_DISP_XY] = 2,
    [OSC_DISP_FFT] = 3, [OSC_DISP_SPLIT] = 4, [OSC_DISP_WATERFALL] = 5,
};

int8_t osc_menu_disp_view_mode(uint8_t item)
{
    return (item < OSC_DISP_COUNT) ? k_disp_view_mode[item] : (int8_t)-1;
}

uint8_t osc_menu_disp_item(int32_t view_mode)
{
    if (view_mode >= 0)
        for (uint8_t i = 0; i < OSC_DISP_COUNT; i++)
            if (k_disp_view_mode[i] == view_mode) return i;
    return OSC_DISP_COUNT;
}

uint8_t osc_menu_prm_of_chip(uint8_t chip)
{
    return (chip < 12u) ? k_chip_prm[chip] : 0xFFu;
}

uint8_t osc_menu_math_op(uint8_t item)
{
    return (item < OSC_MATH_COUNT) ? k_math_op[item] : 0xFFu;
}

uint8_t osc_menu_math_item(uint8_t op)
{
    for (uint8_t i = 1; i < OSC_MATH_COUNT; i++)
        if (k_math_op[i] == op) return i;
    return OSC_MATH_OFF;
}

const char *osc_menu_item_label(osc_row_t row, uint8_t item, uint8_t ch)
{
    switch (row) {
    case OSC_ROW_DISPLAY: return item < OSC_DISP_COUNT ? k_disp[item] : "";
    case OSC_ROW_CURSOR:  return item < OSC_CUR_COUNT ? k_cur[item] : "";
    case OSC_ROW_CH1:
    case OSC_ROW_CH2:     return item < OSC_CH_COUNT ? k_ch[item] : "";
    case OSC_ROW_TRIGGER: return item < OSC_TRIG_COUNT ? k_trig[item] : "";
    case OSC_ROW_PERSIST: return item < OSC_PERS_COUNT ? k_pers[item] : "";
    case OSC_ROW_MATH:    return item < OSC_MATH_COUNT ? k_math[item] : "";
    case OSC_ROW_PARAM:
        if (item == OSC_PRM_TAB)  return ch ? "CH2" : "CH1";
        if (item == OSC_PRM_ALL)  return "All";
        if (item == OSC_PRM_NONE) return "None";
        if (item < OSC_PRM_COUNT) return scope_prm_label(k_chip_prm[item - OSC_PRM_CHIP0]);
        return "";
    default:              return "";
    }
}

void osc_menu_chip_pos(osc_row_t row, uint8_t item, uint8_t *r, uint8_t *c)
{
    const osc_pane_layout_t *l = osc_menu_layout(row);
    uint8_t cols = l->cols ? l->cols : 1u;
    if (l->span_first) {
        if (item == 0u) { *r = 0; *c = 0; return; }
        *r = (uint8_t)(1u + (item - 1u) / cols);
        *c = (uint8_t)((item - 1u) % cols);
    } else {
        *r = (uint8_t)(item / cols);
        *c = (uint8_t)(item % cols);
    }
}

/* Inverse of osc_menu_chip_pos; clamps the column into a short last row. */
static uint8_t chip_item(const osc_pane_layout_t *l, uint8_t r, uint8_t c)
{
    unsigned i;
    if (l->span_first) i = (r == 0u) ? 0u : 1u + (unsigned)(r - 1u) * l->cols + c;
    else               i = (unsigned)r * l->cols + c;
    if (i >= l->n_items) i = l->n_items - 1u;
    return (uint8_t)i;
}

static uint8_t chip_rows(const osc_pane_layout_t *l)
{
    uint8_t r, c;
    osc_menu_chip_pos((osc_row_t)(l - k_layout), (uint8_t)(l->n_items - 1u), &r, &c);
    return (uint8_t)(r + 1u);
}

bool osc_menu_select_closes(osc_row_t row, uint8_t item)
{
    return row == OSC_ROW_CURSOR && item != OSC_CUR_OFF && item < OSC_CUR_COUNT;
}

/* ── State ──────────────────────────────────────────────────────────── */

const osc_menu_state_t *osc_menu_state(void) { return &g_m; }

void osc_menu_open(osc_row_t row)
{
    if ((unsigned)row >= OSC_ROW_COUNT) row = OSC_ROW_DISPLAY;
    g_m.open    = true;
    g_m.row     = (uint8_t)row;
    g_m.in_pane = false;
    g_m.item    = 0;
    g_m.meas_ch = 0;
    g_m.locked  = false;
    g_m.opens++;
}

void osc_menu_open_locked(osc_row_t row)
{
    osc_menu_open(row);
    g_m.in_pane = true;
    g_m.locked  = true;
    if (g_m.row == OSC_ROW_PARAM) g_m.item = OSC_PRM_CHIP0;   /* on Freq, as entering from the rail */
}

bool osc_menu_is_open(void) { return g_m.open; }
bool osc_menu_is_locked(void) { return g_m.open && g_m.locked; }

void osc_menu_close(void)
{
    g_m.open    = false;
    g_m.in_pane = false;
    g_m.locked  = false;
}

void osc_menu_set_item(uint8_t item)
{
    const osc_pane_layout_t *l = osc_menu_layout((osc_row_t)g_m.row);
    g_m.item = (item < l->n_items) ? item : 0u;
}

static osc_result_t to_rail(void)
{
    g_m.in_pane = false;
    return OSC_RES_REDRAW;
}

/* BACK in a pane: up to the rail, or, locked, close the page. */
static osc_result_t pane_back(void)
{
    if (!g_m.locked) return to_rail();
    osc_menu_close();
    return OSC_RES_CLOSE;
}

/* LEFT off the pane's left edge: the rail, or, locked, nothing. */
static osc_result_t pane_left_edge(void)
{
    return g_m.locked ? OSC_RES_NONE : to_rail();
}

/* Measure pane OK: the tab flips the page, All/None and the chips edit the
 * selection (meas_select is pure, so the model applies it itself). */
static void param_ok(void)
{
    const uint8_t ch = g_m.meas_ch;
    if (g_m.item == OSC_PRM_TAB)       g_m.meas_ch = (uint8_t)(ch ^ 1u);
    else if (g_m.item == OSC_PRM_ALL)  meas_select_set(ch, (uint16_t)SCOPE_PRM_ALL_MASK);
    else if (g_m.item == OSC_PRM_NONE) meas_select_set(ch, 0u);
    else meas_select_toggle(ch, k_chip_prm[g_m.item - OSC_PRM_CHIP0]);
}

static osc_result_t chips_key(osc_key_t k, osc_action_t *act)
{
    const osc_pane_layout_t *l = &k_layout[g_m.row];
    const bool param = (g_m.row == OSC_ROW_PARAM);
    const uint8_t rows = chip_rows(l);
    uint8_t r, c;
    osc_menu_chip_pos((osc_row_t)g_m.row, g_m.item, &r, &c);

    switch (k) {
    case OSC_KEY_LEFT:
        if (c == 0u) return pane_left_edge();
        g_m.item = chip_item(l, r, (uint8_t)(c - 1u));
        return OSC_RES_REDRAW;
    case OSC_KEY_RIGHT:
        if ((l->span_first && r == 0u) || c + 1u >= l->cols) return OSC_RES_NONE;
        {
            uint8_t n = chip_item(l, r, (uint8_t)(c + 1u));
            if (n == g_m.item) return OSC_RES_NONE;   /* short last row */
            g_m.item = n;
        }
        return OSC_RES_REDRAW;
    case OSC_KEY_UP:
        if (r == 0u) {
            if (param && g_m.meas_ch == 1u) {        /* back up into CH1's panel */
                g_m.meas_ch = 0u;
                g_m.item = chip_item(l, (uint8_t)(rows - 1u), c);
                return OSC_RES_REDRAW;
            }
            return OSC_RES_NONE;
        }
        g_m.item = chip_item(l, (uint8_t)(r - 1u), (l->span_first && r == 1u) ? 0u : c);
        return OSC_RES_REDRAW;
    case OSC_KEY_DOWN:
        if (r + 1u >= rows) {
            if (param && g_m.meas_ch == 0u) {        /* scroll on into CH2's panel */
                g_m.meas_ch = 1u;
                g_m.item = chip_item(l, 0u, c);
                return OSC_RES_REDRAW;
            }
            return OSC_RES_NONE;
        }
        g_m.item = chip_item(l, (uint8_t)(r + 1u), (l->span_first && r == 0u) ? 0u : c);
        return OSC_RES_REDRAW;
    case OSC_KEY_OK:
        if (param) { param_ok(); return OSC_RES_REDRAW; }
        act->kind = OSC_ACT_SELECT;
        act->item = g_m.item;
        return OSC_RES_REDRAW;
    case OSC_KEY_BACK:
        return pane_back();
    default:
        return OSC_RES_NONE;
    }
}

static osc_result_t pane_key(osc_key_t k, osc_action_t *act)
{
    const osc_pane_layout_t *l = &k_layout[g_m.row];
    const uint8_t n = l->n_items;

    if (l->kind == OSC_PANE_CHIPS) return chips_key(k, act);

    switch (k) {
    case OSC_KEY_UP:
        g_m.item = (uint8_t)((g_m.item + n - 1u) % n);
        return OSC_RES_REDRAW;
    case OSC_KEY_DOWN:
        g_m.item = (uint8_t)((g_m.item + 1u) % n);
        return OSC_RES_REDRAW;
    case OSC_KEY_BACK:
        return pane_back();
    case OSC_KEY_LEFT:
        if (l->kind == OSC_PANE_RADIO) return pane_left_edge();
        act->kind = OSC_ACT_CHANGE; act->item = g_m.item; act->dir = -1;
        return OSC_RES_REDRAW;
    case OSC_KEY_RIGHT:
        if (l->kind == OSC_PANE_RADIO) return OSC_RES_NONE;
        act->kind = OSC_ACT_CHANGE; act->item = g_m.item; act->dir = +1;
        return OSC_RES_REDRAW;
    case OSC_KEY_OK:
        act->item = g_m.item;
        if (l->kind == OSC_PANE_RADIO) {
            act->kind = OSC_ACT_SELECT;
        } else {
            act->kind = OSC_ACT_CHANGE; act->dir = +1;
        }
        return OSC_RES_REDRAW;
    default:
        return OSC_RES_NONE;
    }
}

osc_result_t osc_menu_step(osc_key_t k, osc_action_t *act)
{
    act->kind = OSC_ACT_NONE;
    act->row  = g_m.row;
    act->item = 0;
    act->dir  = 0;
    if (!g_m.open) return OSC_RES_NONE;

    if (g_m.in_pane) {
        osc_result_t r = pane_key(k, act);
        if (act->kind == OSC_ACT_SELECT && osc_menu_select_closes((osc_row_t)g_m.row, act->item)) {
            osc_menu_close();
            return OSC_RES_CLOSE;
        }
        return r;
    }

    switch (k) {
    case OSC_KEY_UP:
        g_m.row = (uint8_t)((g_m.row + OSC_ROW_COUNT - 1u) % OSC_ROW_COUNT);
        return OSC_RES_REDRAW;
    case OSC_KEY_DOWN:
        g_m.row = (uint8_t)((g_m.row + 1u) % OSC_ROW_COUNT);
        return OSC_RES_REDRAW;
    case OSC_KEY_RIGHT:
    case OSC_KEY_OK:
        g_m.in_pane = true;
        g_m.item    = 0;
        g_m.meas_ch = 0;
        act->kind = OSC_ACT_ENTER;
        if (g_m.row == OSC_ROW_PARAM) g_m.item = OSC_PRM_CHIP0;   /* on Freq, past the tab row */
        return OSC_RES_REDRAW;
    case OSC_KEY_BACK:
        osc_menu_close();
        return OSC_RES_CLOSE;
    case OSC_KEY_LEFT:
    default:
        return OSC_RES_NONE;
    }
}
