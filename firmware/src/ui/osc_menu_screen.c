/*
 * OpenScope 2C53T - the Oscilloscope Settings page as a screen. See
 * osc_menu_screen.h.
 */
#include "osc_menu_screen.h"
#include "key_model.h"
#include "meas_select.h"
#include "scope_state.h"
#include "persistence.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>

/* ── the binding table ─────────────────────────────────────────────── */

#define B_NONE PROP_IX_COUNT

typedef struct {
    uint8_t prop[3];     /* VALUES: the property of each row. Others: [0] the pane's
                          * (Measure: [0] CH1's page, [1] CH2's). PROP_IX_x, B_NONE = none */
} osc_bind_t;

static const osc_bind_t k_bind[OSC_ROW_COUNT] = {
    [OSC_ROW_DISPLAY] = { { PROP_IX_VIEW_MODE,    B_NONE,            B_NONE } },
    [OSC_ROW_CURSOR]  = { { PROP_IX_VIEW_CURSOR,  B_NONE,            B_NONE } },
    [OSC_ROW_PARAM]   = { { PROP_IX_CH1_MEAS,     PROP_IX_CH2_MEAS,  B_NONE } },
    [OSC_ROW_CH1]     = { { PROP_IX_CH1_ENABLED,  PROP_IX_CH1_PROBE, PROP_IX_CH1_COUPLING } },
    [OSC_ROW_CH2]     = { { PROP_IX_CH2_ENABLED,  PROP_IX_CH2_PROBE, PROP_IX_CH2_COUPLING } },
    [OSC_ROW_TRIGGER] = { { PROP_IX_TRIG_MODE,    PROP_IX_TRIG_EDGE, PROP_IX_TRIG_SOURCE } },
    [OSC_ROW_PERSIST] = { { PROP_IX_VIEW_PERSIST, B_NONE,            B_NONE } },
    [OSC_ROW_MATH]    = { { PROP_IX_VIEW_MATH,    B_NONE,            B_NONE } },
};
_Static_assert(PROP_IX_COUNT < 256, "a table index fits the binding's byte");
_Static_assert(OSC_CH_ENABLE == 0 && OSC_CH_PROBE == 1 && OSC_CH_COUPLING == 2 && OSC_CH_COUNT == 3,
               "the CH panes' rows are enabled, probe, coupling");
_Static_assert(OSC_TRIG_MODE == 0 && OSC_TRIG_EDGE == 1 && OSC_TRIG_SOURCE == 2 && OSC_TRIG_COUNT == 3,
               "the Trigger pane's rows are mode, edge, source");
/* item i = value i for the three RADIO panes. */
#define SAME(a, b) ((int)(a) == (int)(b))
_Static_assert(SAME(OSC_CUR_OFF, CURSOR_OFF) && SAME(OSC_CUR_TIME, CURSOR_VERTICAL) &&
               SAME(OSC_CUR_VOLT, CURSOR_HORIZONTAL) && SAME(OSC_CUR_BOTH, CURSOR_BOTH),
               "Cursors item = view.cursor value");
_Static_assert(SAME(OSC_PERS_OFF, PERSIST_OFF) && SAME(OSC_PERS_500MS, PERSIST_LOW) &&
               SAME(OSC_PERS_1S, PERSIST_MEDIUM) && SAME(OSC_PERS_2S, PERSIST_HIGH) &&
               SAME(OSC_PERS_INF, PERSIST_INFINITE), "Persist item = view.persist value");

static bool is_values(osc_row_t row)
{
    return (unsigned)row < OSC_ROW_COUNT && osc_menu_layout(row)->kind == OSC_PANE_VALUES;
}

const prop_desc_t *osc_menu_bind_prop(osc_row_t row, uint8_t item)
{
    if ((unsigned)row >= OSC_ROW_COUNT) return NULL;
    const uint8_t i = is_values(row) ? item : 0u;
    if (i >= 3u || k_bind[row].prop[i] == B_NONE) return NULL;
    return &prop_table[k_bind[row].prop[i]];
}

int32_t osc_menu_bind_value(osc_row_t row, uint8_t item)
{
    if ((unsigned)row >= OSC_ROW_COUNT || item >= osc_menu_layout(row)->n_items) return -1;
    switch (row) {
    case OSC_ROW_DISPLAY: return osc_menu_disp_view_mode(item);
    case OSC_ROW_CURSOR:
    case OSC_ROW_PERSIST: return item;
    case OSC_ROW_MATH: {
        if (item == OSC_MATH_OFF) return 0;
        const uint8_t op = osc_menu_math_op(item);
        return op == 0xFFu ? -1 : 1 + (int32_t)op;
    }
    default:              return -1;
    }
}

uint8_t osc_menu_bind_current(osc_row_t row)
{
    const prop_desc_t *p = osc_menu_bind_prop(row, 0);
    const uint8_t n = ((unsigned)row < OSC_ROW_COUNT) ? osc_menu_layout(row)->n_items : 0u;
    if (!p) return n;
    const int32_t v = prop_get(p);
    for (uint8_t i = 0; i < n; i++)
        if (osc_menu_bind_value(row, i) == v) return i;
    return n;
}

void osc_menu_bind_text(osc_row_t row, uint8_t item, char *out, size_t n)
{
    if (!out || !n) return;
    out[0] = '\0';
    const prop_desc_t *p = is_values(row) ? osc_menu_bind_prop(row, item) : NULL;
    if (!p) return;
    const int32_t v = prop_get(p);
    if (p->type == PT_BOOL) snprintf(out, n, "%s", v ? "ON" : "OFF");
    else                    prop_format(p, v, out, n);
}

bool osc_menu_screen_paints(uint16_t groups)
{
    const uint16_t bound = (uint16_t)((1u << PG_CH1) | (1u << PG_CH2) | (1u << PG_TRIG) | (1u << PG_VIEW));
    return groups != 0u && (groups & (uint16_t)~bound) == 0u;
}

/* ── UI state (input task writes; the display task and the shell read) ── */

/* The key that opened the page (or last jumped in it): pressed again it
 * closes the page (key_model.c map_osc_menu). */
static uint8_t s_opener = BTN_NONE;
/* A one-line result of the last key ("Keep one CH on"); "" = show the hint.
 * s_note_opens: the osc_menu_state()->opens it was written under. A note
 * from an earlier visit (or from before a deep-link jump) is not shown. */
static char    s_note[28];
static uint8_t s_note_opens;

uint8_t osc_menu_screen_opener(void) { return s_opener; }

const char *osc_menu_screen_note(void)
{
    return (s_note[0] && s_note_opens == osc_menu_state()->opens) ? s_note : "";
}

static void note(const char *t) { snprintf(s_note, sizeof s_note, "%s", t); }

void osc_menu_screen_open(osc_row_t row, bool locked, uint8_t opener)
{
    if (locked) osc_menu_open_locked(row);
    else        osc_menu_open(row);
    s_opener = opener;
    (void)screen_push(SCR_OSC_MENU);    /* refused while it is up: one layer */
}

/* ── apply: one property set per change ────────────────────────────── */

/* What the footer says after a set of `p` to `want` answered `st`. On
 * success the setter's own note, if it has one ("CH2 alone: 250 MS/s",
 * "Await Trigger..."); the row shows the rest. On a refusal the texts the
 * page always used. */
static void note_result(const prop_desc_t *p, int32_t want, prop_status_t st, const prop_note_t *n)
{
    if (st == PS_OK || st == PS_CLAMPED) {
        if (n->text[0]) note(n->text);
        return;
    }
    char lab[8];      /* "DC", "AC", "CH1", "CH2" */
    switch (p->id) {
    case PROP_ID_VIEW_PERSIST:
        note("No memory for persistence");
        return;
    case PROP_ID_CH1_COUPLING:
    case PROP_ID_CH2_COUPLING:
        if (st != PS_NOT_SET) break;
        prop_format(p, want, lab, sizeof lab);
        snprintf(s_note, sizeof s_note, "CH%u %s NOT SET", (unsigned)p->arg + 1u, lab);
        return;
    case PROP_ID_TRIG_SOURCE:
        if (st != PS_NOT_SET) break;
        prop_format(p, want, lab, sizeof lab);
        snprintf(s_note, sizeof s_note, "Trig %s NOT SET", lab);
        return;
    case PROP_ID_VIEW_MODE:
        note(n->text[0] ? n->text : "view NOT SET");
        return;
    default:
        break;
    }
    note(n->text[0] ? n->text : prop_status_name((uint8_t)st));
}

/* The value one step of `dir` asks for (the failure text names it). */
static int32_t step_target(const prop_desc_t *p, int dir)
{
    const int32_t v = prop_get(p);
    if (p->type == PT_BOOL) return !v;
    int32_t w = v + (dir < 0 ? -(int32_t)p->step : (int32_t)p->step);
    if (w > p->max) w = (p->flags & PF_WRAP) ? p->min : p->max;
    if (w < p->min) w = (p->flags & PF_WRAP) ? p->max : p->min;
    return w;
}

static void apply(const osc_action_t *a)
{
    const osc_row_t row = (osc_row_t)a->row;
    prop_note_t n;
    n.text[0] = '\0';

    switch (a->kind) {
    case OSC_ACT_ENTER:
        /* The pane cursor starts on the value in force. */
        if (osc_menu_layout(row)->kind == OSC_PANE_RADIO || row == OSC_ROW_MATH)
            osc_menu_set_item(osc_menu_bind_current(row));
        break;
    case OSC_ACT_SELECT: {
        const prop_desc_t *p = osc_menu_bind_prop(row, a->item);
        const int32_t v = osc_menu_bind_value(row, a->item);
        if (!p || v < 0) break;
        note_result(p, v, prop_set_local(p, v, ORIGIN_KEY, &n), &n);
        break;
    }
    case OSC_ACT_CHANGE: {
        const prop_desc_t *p = is_values(row) ? osc_menu_bind_prop(row, a->item) : NULL;
        if (!p) break;
        const int32_t want = step_target(p, a->dir);
        note_result(p, want, prop_step_local(p, a->dir, ORIGIN_KEY, &n), &n);
        break;
    }
    default:
        break;
    }
}

/* One key of the page through the model, then the change it asks for. */
static osc_result_t page_key(osc_key_t k)
{
    const int32_t cur_was = prop_get(P(VIEW_CURSOR));
    const uint16_t m1 = meas_select_get(0), m2 = meas_select_get(1);

    osc_action_t a;
    const osc_result_t r = osc_menu_step(k, &a);
    /* A key that redraws nothing keeps the note on screen (this visit's). */
    if (r != OSC_RES_NONE || s_note_opens != osc_menu_state()->opens) s_note[0] = '\0';
    s_note_opens = osc_menu_state()->opens;
    apply(&a);

    /* The Measure pane's selection is edited by the model (explicit action,
     * osc_menu_screen.h): say which group moved. */
    if (meas_select_get(0) != m1) model_touch(PG_CH1);
    if (meas_select_get(1) != m2) model_touch(PG_CH2);
    const int32_t cur = prop_get(P(VIEW_CURSOR));
    if (cur != cur_was) osc_menu_env_cursors(cur != CURSOR_OFF);
    return r;
}

/* ── input task ────────────────────────────────────────────────────── */

/* The page's keys are key_model.c's (map_osc_menu, map_osc_locked): the
 * arrows, OK and MENU act in it, a deep-link key jumps or closes, the
 * plot's long keys do nothing. POWER, SAVE and the long mode keys are the
 * router's globals and never get here. */
static scr_res_t oms_key(scr_key_t k)
{
    if (!osc_menu_is_open()) return SCR_CLOSE;          /* the page's model is shut: so is its layer */
    key_ctx_t c;
    memset(&c, 0, sizeof c);
    c.mode            = MODE_OSCILLOSCOPE;
    c.osc_menu_open   = true;
    c.osc_menu_locked = osc_menu_is_locked();
    c.osc_menu_opener = s_opener;
    const key_cmd_t m = key_map(&c, k.btn, k.is_long);

    switch (m.act) {
    case KA_OSCMENU_OPEN:                    /* a deep-link key: jump to its row */
        osc_menu_screen_open((osc_row_t)m.arg, false, (uint8_t)k.btn);
        return SCR_DRAW;
    case KA_OSCMENU_PANE:                    /* the other pane key: the lock moves */
        osc_menu_screen_open((osc_row_t)m.arg, true, (uint8_t)k.btn);
        return SCR_DRAW;
    case KA_OSCMENU_KEY:
        switch (page_key((osc_key_t)m.arg)) {
        case OSC_RES_REDRAW: return SCR_DRAW;
        case OSC_RES_CLOSE:  osc_menu_close(); s_opener = BTN_NONE; return SCR_CLOSE;
        default:             return SCR_HANDLED;     /* nothing changed on screen */
        }
    case KA_OSCMENU_CLOSE:
        osc_menu_close();
        s_opener = BTN_NONE;
        return SCR_CLOSE;
    case KA_NONE:
        return SCR_HANDLED;                  /* MOVE / SELECT / AUTO and the plot's long keys */
    default:
        return SCR_UNHANDLED;                /* POWER (KA_LEGACY): the globals' */
    }
}

/* The layer went (the page closed itself, or the mode changed under it):
 * the page never reappears over another screen's return. */
static void oms_close(screen_id_t to)
{
    (void)to;
    osc_menu_close();
    s_opener = BTN_NONE;
}

/* ── display task ──────────────────────────────────────────────────── */

static void oms_draw(scr_paint_t how, uint32_t frame)
{
    (void)frame;
    if (how != SCR_PAINT_UPDATE) osc_menu_invalidate();   /* cleared, or a popup box went */
    osc_menu_draw();
}

/* Hash of everything osc_menu_draw() renders: the page's cursor, every
 * bound value (whoever set it), both measurement selections, the note and
 * the theme. */
static uint32_t oms_epoch(void)
{
    const osc_menu_state_t s = *osc_menu_state();
    uint32_t h = REDRAW_EPOCH_SEED;
    h = redraw_epoch_mix(h, (uint32_t)theme_get_id());
    h = redraw_epoch_mix(h, (uint32_t)s.open | ((uint32_t)s.in_pane << 1) | ((uint32_t)s.locked << 2)
                            | ((uint32_t)s.meas_ch << 3) | ((uint32_t)s.row << 8)
                            | ((uint32_t)s.item << 16) | ((uint32_t)s.opens << 24));
    for (unsigned r = 0; r < OSC_ROW_COUNT; r++)
        for (unsigned i = 0; i < 3u; i++)
            if (k_bind[r].prop[i] != B_NONE)
                h = redraw_epoch_mix(h, (uint32_t)prop_get(&prop_table[k_bind[r].prop[i]]));
    for (const char *t = osc_menu_screen_note(); *t; t++)
        h = redraw_epoch_mix(h, (uint32_t)(unsigned char)*t);
    return h;
}

/* ── any task ──────────────────────────────────────────────────────── */

static const char *const k_row_name[OSC_ROW_COUNT] = {
    "display", "cursor", "param", "ch1", "ch2", "trigger", "persist", "math",
};

/* `ui screen`: "row=ch1 pane=pane item=2". */
static void oms_describe(char *out, size_t n)
{
    const osc_menu_state_t *om = osc_menu_state();
    snprintf(out, n, "row=%s pane=%s item=%u",
             om->row < OSC_ROW_COUNT ? k_row_name[om->row] : "?",
             om->locked ? "locked" : om->in_pane ? "pane" : "rail", (unsigned)om->item);
}

/* The page is the scope's: it shows the scope's groups (the bars above and
 * below it show the rest of them). OPAQUE, not GRAB: a grabbing layer is
 * asked before the self-cal lock, and the lock comes first here (a key on
 * the page during a shell `autoset` says "Busy: AUTO cancels"). */
#define G(g) ((uint16_t)(1u << (g)))
#define OMS_GROUPS (G(PG_GEN) | G(PG_SYS) | G(PG_UI) | G(PG_CH1) | G(PG_CH2) | G(PG_TB) | G(PG_TRIG) | \
                    G(PG_VIEW) | G(PG_FFT) | G(PG_IL) | G(PG_MASK))

const screen_t osc_menu_screen = {
    .name = "osc_menu", .id = SCR_OSC_MENU, .mode = MODE_COUNT, .instrument = SCR_INSTRUMENT_NONE,
    .flags = SCR_F_OPAQUE,
    .open = NULL, .close = oms_close, .key = oms_key,
    .draw = oms_draw, .tick = NULL, .epoch = oms_epoch,
    .describe = oms_describe, .groups = OMS_GROUPS,
};
