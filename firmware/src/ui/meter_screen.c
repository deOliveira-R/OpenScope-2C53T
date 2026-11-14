/*
 * OpenScope 2C53T - the multimeter screen. See meter_screen.h.
 */
#include "meter_screen.h"
#include "fuse_model.h"
#include "fuse_table.h"
#include "model.h"
#include "theme.h"

#include <stdio.h>

static int32_t G(const prop_desc_t *p) { return prop_get(p); }

/* ── softkey handlers (input task) ─────────────────────────────────── */

/* The names the bar prints (the status bar's longer ones are meter_ui.c's). */
static const char *const k_func_names[METER_SUBMODE_COUNT] = {
    "DC V", "AC V", "DC mA", "DC A", "AC mA", "AC A",
    "Ohms", "Contin.", "Diode", "Cap.", "Temp", "Auto",
};
static const char *const k_view_names[METER_LAYOUT_COUNT] = {
    "Big", "Graph", "Stats", "Fuse", "Limits",
};
/* The View softkey's order: Limits before the fuse tester. */
static const uint8_t k_view_order[METER_LAYOUT_COUNT] = {
    METER_LAYOUT_BIG, METER_LAYOUT_CHART, METER_LAYOUT_STATS,
    METER_LAYOUT_LIMITS, METER_LAYOUT_FUSE,
};

static uint8_t func_now(void)   { const int32_t m = G(P(METER_FUNC));   return (uint8_t)(m < METER_SUBMODE_COUNT ? m : 0); }
static uint8_t layout_now(void) { const int32_t l = G(P(METER_LAYOUT)); return (uint8_t)(l < METER_LAYOUT_COUNT ? l : 0); }
static const fuse_table_t *fuse_table_now(void)
{
    const int32_t t = G(P(METER_FUSETYPE));
    return &fuse_tables[t < FUSE_TYPE_COUNT ? t : 0];
}

static void step(const prop_desc_t *p, int dir) { (void)prop_step_local(p, dir, ORIGIN_KEY, NULL); }

static void skv_function(char *b, uint8_t n) { snprintf(b, n, "%s", k_func_names[func_now()]); }
static void skp_function(void) { step(P(METER_FUNC), +1); }

static void skv_reset(char *b, uint8_t n) { snprintf(b, n, "Min/Max"); }
static void skp_reset(void) { meter_ui_stats_reset(); }

static void skv_rel(char *b, uint8_t n) { snprintf(b, n, "%s", G(P(METER_REL)) ? "On" : "Off"); }
static bool ska_rel(void) { return G(P(METER_REL)) != 0; }
static void skp_rel(void)
{
    step(P(METER_REL), +1);
    scope_show_popup(G(P(METER_REL)) ? "Relative: On" : "Relative: Off");
}

/* MOVE in Temperature: degC <-> degF, as stock (0x0800D2DC flips ms[0xF39];
 * the parser converts, 0x0803E180). Stock resets min/max/mean on it (the
 * setter's op does). The bar keeps its Function key on LEFT/RIGHT. */
static void skv_tunit(char *b, uint8_t n) { snprintf(b, n, "%s", G(P(METER_DEGF)) ? "degF" : "degC"); }
static void skp_tunit(void)
{
    step(P(METER_DEGF), +1);
    scope_show_popup(G(P(METER_DEGF)) ? "Temperature: degF" : "Temperature: degC");
}

static void skv_view(char *b, uint8_t n) { snprintf(b, n, "%s", k_view_names[layout_now()]); }
static void skp_view(void)
{
    uint8_t i = 0;
    const uint8_t cur = layout_now();
    while (i < METER_LAYOUT_COUNT && k_view_order[i] != cur) i++;
    prop_note_t note;
    (void)prop_set_local(P(METER_LAYOUT), k_view_order[(i + 1) % METER_LAYOUT_COUNT],
                         ORIGIN_KEY, &note);
    /* What the setter did on the way in ("Fuse tester: DC V"), or why not. */
    if (note.text[0]) scope_show_popup(note.text);
}

static void skv_low(char *b, uint8_t n)  { meter_ui_limit_text(0, b, n); }
static void skv_high(char *b, uint8_t n) { meter_ui_limit_text(1, b, n); }
static bool ska_low(void)  { return meter_ui_limit_active(0); }
static bool ska_high(void) { return meter_ui_limit_active(1); }
static void skp_low(void)  { meter_ui_limit_press(0); }
static void skp_high(void) { meter_ui_limit_press(1); }

static void skv_fuse_type(char *b, uint8_t n)
{
    const int32_t t = G(P(METER_FUSETYPE));
    snprintf(b, n, "%s", fuse_type_names[t < FUSE_TYPE_COUNT ? t : 0]);
}
static void skp_fuse_type(void) { step(P(METER_FUSETYPE), +1); }
static void skv_fuse_show(char *b, uint8_t n)
{
    static const char *const v[FUSE_VIEW_COUNT] = { "Detail", "Table", "Scan", "Types" };
    const int32_t f = G(P(METER_FUSEVIEW));
    snprintf(b, n, "%s", v[f < FUSE_VIEW_COUNT ? f : 0]);
}
static void skp_fuse_show(void) { step(P(METER_FUSEVIEW), +1); }
static void skv_fuse_rating(char *b, uint8_t n)
{
    const fuse_table_t *t = fuse_table_now();
    const int32_t i = G(P(METER_FUSERATING));
    char l[8];
    fuse_rating_label(t->entries[i < t->count ? i : 0].rating_amps, l, sizeof l);
    snprintf(b, n, "%s A", l);
}
static bool ska_arrows(void) { return true; }          /* UP/DOWN adjust this key */
/* Rating softkey: the next rating of this type, wrapping (the arrows stop). */
static void skp_fuse_rating(void)
{
    const uint8_t count = fuse_table_now()->count;
    if (!count) return;
    (void)prop_set_local(P(METER_FUSERATING), (G(P(METER_FUSERATING)) + 1) % count, ORIGIN_KEY, NULL);
}
static void skv_fuse_thresh(char *b, uint8_t n)
{
    const int t = (int)G(P(METER_FUSETHRESH));
    snprintf(b, n, "%d.%d mV", t / 10, t % 10);
}
/* Draw-if softkey: the next preset. */
static void skp_fuse_thresh(void)
{
    const float next = fuse_scan_threshold_next_preset((float)G(P(METER_FUSETHRESH)) / 10.0f);
    (void)prop_set_local(P(METER_FUSETHRESH), (int32_t)(next * 10.0f + 0.5f), ORIGIN_KEY, NULL);
}

static void skv_fuse_cal(char *b, uint8_t n) { meter_ui_cal_text(b, n); }
static bool ska_fuse_cal(void) { return meter_ui_cal_set(); }
static void skp_fuse_cal(void) { meter_ui_fuse_cal(); }

static void skv_cont(char *b, uint8_t n) { snprintf(b, n, "< %u Ohm", (unsigned)meter_beep_ohms()); }
static void skp_cont(void)
{
    char t[24];
    step(P(METER_BEEP), +1);
    snprintf(t, sizeof t, "Beep below %u Ohm", (unsigned)meter_beep_ohms());
    scope_show_popup(t);
}

/* ── softkey tables ────────────────────────────────────────────────── */

static const softkey_bar_t meter_bar_cont = {{
    { "Function", skv_function,  skp_function, NULL },
    { "Reset",    skv_reset,     skp_reset,    NULL },
    { "Beep",     skv_cont,      skp_cont,     NULL },
    { "View",     skv_view,      skp_view,     NULL },
}};
static const softkey_bar_t meter_bar_temp = {{
    { "Unit",     skv_tunit,     skp_tunit,    NULL },
    { "Reset",    skv_reset,     skp_reset,    NULL },
    { "Relative", skv_rel,       skp_rel,      ska_rel },
    { "View",     skv_view,      skp_view,     NULL },
}};
static const softkey_bar_t meter_bar_main = {{
    { "Function", skv_function,  skp_function, NULL },
    { "Reset",    skv_reset,     skp_reset,    NULL },
    { "Relative", skv_rel,       skp_rel,      ska_rel },
    { "View",     skv_view,      skp_view,     NULL },
}};
static const softkey_bar_t meter_bar_limits = {{
    { "Function", skv_function,  skp_function, NULL },
    { "Low",      skv_low,       skp_low,      ska_low },
    { "High",     skv_high,      skp_high,     ska_high },
    { "View",     skv_view,      skp_view,     NULL },
}};
/* Fuse type is also on LEFT/RIGHT; the Types page keeps it on a key. */
static const softkey_bar_t meter_bar_fuse = {{
    { "Cal leads", skv_fuse_cal,    skp_fuse_cal,    ska_fuse_cal },
    { "Rating",    skv_fuse_rating, skp_fuse_rating, ska_arrows },
    { "Show",      skv_fuse_show,   skp_fuse_show,   NULL },
    { "View",      skv_view,        skp_view,        NULL },
}};
static const softkey_bar_t meter_bar_fuse_types = {{
    { "Fuse type", skv_fuse_type,   skp_fuse_type,   NULL },
    { "Rating",    skv_fuse_rating, skp_fuse_rating, ska_arrows },
    { "Show",      skv_fuse_show,   skp_fuse_show,   NULL },
    { "View",      skv_view,        skp_view,        NULL },
}};
static const softkey_bar_t meter_bar_fuse_scan = {{
    { "Cal leads", skv_fuse_cal,    skp_fuse_cal,    ska_fuse_cal },
    { "Draw if >", skv_fuse_thresh, skp_fuse_thresh, ska_arrows },
    { "Show",      skv_fuse_show,   skp_fuse_show,   NULL },
    { "View",      skv_view,        skp_view,        NULL },
}};

const softkey_bar_t *meter_softkey_bar(uint8_t layout, uint8_t fuse_page, uint8_t submode)
{
    if (layout == METER_LAYOUT_LIMITS) return &meter_bar_limits;
    if (layout == METER_LAYOUT_FUSE)
        return fuse_page == FUSE_VIEW_SCAN  ? &meter_bar_fuse_scan
             : fuse_page == FUSE_VIEW_TYPES ? &meter_bar_fuse_types : &meter_bar_fuse;
    if (submode == 7)                  return &meter_bar_cont;   /* continuity: Beep threshold */
    if (submode == 10)                 return &meter_bar_temp;   /* temperature: degC/degF on MOVE */
    return &meter_bar_main;
}

const softkey_bar_t *meter_screen_bar(void)
{
    return meter_softkey_bar((uint8_t)G(P(METER_LAYOUT)), (uint8_t)G(P(METER_FUSEVIEW)),
                             (uint8_t)G(P(METER_FUNC)));
}

uint32_t meter_softkeys_epoch(void) { return softkey_bar_epoch(meter_screen_bar()); }

/* ── input task ────────────────────────────────────────────────────── */

static scr_res_t m_key(scr_key_t k)
{
    if (k.is_long) return SCR_HANDLED;          /* the long mode keys and long SAVE are globals */

    /* The four buttons under the screen do what the bar above them says.
     * Asked first, so the meter never falls into another meaning for them.
     * An empty slot still repaints (as before). */
    const int8_t slot = softkey_slot_for_button(k.btn);
    if (slot >= 0) {
        meter_ui_autoselect_cancel();
        (void)softkey_bar_press(meter_screen_bar(), slot);
        return SCR_DRAW;
    }

    const uint8_t layout = (uint8_t)G(P(METER_LAYOUT));
    switch (k.btn) {
    case BTN_MENU:
        /* Short MENU at the meter's top level opens Home (owner decision
         * D2; stock's BACK does nothing here). */
        (void)prop_set_local(P(SYS_MODE), (int32_t)MODE_HOME, ORIGIN_KEY, NULL);
        return SCR_HANDLED;                     /* the mode switch repaints */
    case BTN_AUTO:
        /* Stock's AUTO key in the meter (0x0800CB50) sends word 0x0514 and
         * lets the SoC pick the function (V or Ohm, AC or DC); it resets
         * hold/REL/min/max like any function change. Pressed again it
         * re-arms the SoC (a set of the same value still runs the setter).
         * The software autoscan stays reachable as the shell's `meter auto`. */
        (void)prop_set_local(P(METER_FUNC), METER_SUBMODE_COUNT - 1, ORIGIN_KEY, NULL);
        scope_show_popup("AUTO");
        return SCR_DRAW;
    case BTN_OK: {
        /* OK = Hold: freeze the reading on screen now, or back to live. */
        prop_note_t note;
        const bool was = G(P(METER_HOLD)) != 0;
        const prop_status_t s = prop_set_local(P(METER_HOLD), !was, ORIGIN_KEY, &note);
        scope_show_popup(s != PS_OK ? note.text : was ? "Live" : "HOLD");
        return SCR_DRAW;
    }
    case BTN_UP:
    case BTN_DOWN: {
        const int dir = (k.btn == BTN_UP) ? +1 : -1;      /* UP = larger */
        if (layout == METER_LAYOUT_FUSE) {
            /* The rating, or the draw threshold on the Scan page. Both stop
             * at their ends. */
            step(G(P(METER_FUSEVIEW)) == FUSE_VIEW_SCAN ? P(METER_FUSETHRESH) : P(METER_FUSERATING), dir);
            return SCR_DRAW_FULL;
        }
        if (layout == METER_LAYOUT_LIMITS) {
            meter_ui_limits_nudge((int8_t)dir);
            return SCR_DRAW;
        }
        return SCR_HANDLED;
    }
    case BTN_LEFT:
    case BTN_RIGHT: {
        const int dir = (k.btn == BTN_RIGHT) ? +1 : -1;
        /* The fuse type in the fuse tester, else the previous / next function. */
        step(layout == METER_LAYOUT_FUSE ? P(METER_FUSETYPE) : P(METER_FUNC), dir);
        return SCR_DRAW_FULL;
    }
    case BTN_CH1:
    case BTN_CH2:
        return SCR_DRAW_FULL;                   /* no meaning here: they only repaint, as before */
    default:
        return SCR_HANDLED;
    }
}

/* ── display task ──────────────────────────────────────────────────── */

/* Hash of everything the page shows apart from the reading itself: the
 * meter's settings (whoever set them: a key, `set meter.*`, the old `meter`
 * commands), the view state with no property, the softkey values, the
 * theme. */
static uint32_t m_epoch(void)
{
    uint32_t h = REDRAW_EPOCH_SEED;
    h = redraw_epoch_mix(h, (uint32_t)theme_get_id());
    h = redraw_epoch_mix(h, (uint32_t)G(P(METER_FUNC)) | ((uint32_t)G(P(METER_LAYOUT)) << 8)
                            | ((uint32_t)G(P(METER_HOLD)) << 16) | ((uint32_t)G(P(METER_REL)) << 17)
                            | ((uint32_t)G(P(METER_DEGF)) << 18) | ((uint32_t)G(P(METER_BEEP)) << 20));
    h = redraw_epoch_mix(h, (uint32_t)G(P(METER_FUSETYPE)) | ((uint32_t)G(P(METER_FUSERATING)) << 8)
                            | ((uint32_t)G(P(METER_FUSEVIEW)) << 16) | ((uint32_t)G(P(METER_FUSETHRESH)) << 24));
    h = redraw_epoch_mix(h, meter_ui_gen());
    h = redraw_epoch_mix(h, meter_softkeys_epoch());
    return h;
}

/* What the last paint showed, and when tick() last asked for one. Display
 * task only. */
static uint32_t s_painted_epoch;
static bool     s_painted;
static uint32_t s_tick_frame;

static void m_draw(scr_paint_t how, uint32_t frame)
{
    (void)frame;
    /* Before the paint: a change that lands while it runs is seen by the
     * next tick. */
    s_painted_epoch = m_epoch();
    s_painted = true;
    meter_ui_draw(how);
}

/* Repaint in place when the reading to show changed (at most 4 times a
 * second), at once when anything else the page shows changed (a set from
 * the shell, a key), and once a second while the page animates. Nothing
 * else: a steady reading costs no LCD write. The software autoselect holds
 * the page still while it steps through its candidates. */
#define METER_MIN_FRAMES       5    /* 20 Hz loop -> at most 4 Hz for readings */
#define METER_PERIODIC_FRAMES  20

static redraw_action_t m_tick(uint32_t frame)
{
    if (meter_ui_autoselect_running()) return REDRAW_SKIP;
    const uint32_t since = frame - s_tick_frame;
    const bool changed  = !s_painted || m_epoch() != s_painted_epoch;
    const bool reading  = meter_ui_reading_seq() != meter_ui_rendered_seq() && since >= METER_MIN_FRAMES;
    const bool periodic = meter_ui_periodic() && since >= METER_PERIODIC_FRAMES;
    if (!changed && !reading && !periodic) return REDRAW_SKIP;
    s_tick_frame = frame;
    return REDRAW_INCREMENTAL;
}

/* Entering the page: the first tick paints whatever the last visit left. */
static void m_open(screen_id_t from) { (void)from; }

/* ── any task ──────────────────────────────────────────────────────── */

/* `ui screen`: "mview=<layout>", in the fuse tester "mview=fuse/<page>". */
static void m_describe(char *out, size_t n)
{
    char l[12], f[12];
    prop_format(P(METER_LAYOUT), G(P(METER_LAYOUT)), l, sizeof l);
    if (G(P(METER_LAYOUT)) == METER_LAYOUT_FUSE) {
        prop_format(P(METER_FUSEVIEW), G(P(METER_FUSEVIEW)), f, sizeof f);
        snprintf(out, n, "mview=%s/%s", l, f);
    } else {
        snprintf(out, n, "mview=%s", l);
    }
}

/* The page shows the meter; the bars add the generator slot, sys and ui. */
#define M_GROUPS ((uint16_t)((1u << PG_METER) | (1u << PG_GEN) | (1u << PG_SYS) | (1u << PG_UI)))

const screen_t meter_screen = {
    .name = "meter", .id = SCR_METER, .mode = MODE_MULTIMETER, .instrument = MODE_MULTIMETER,
    .flags = 0,
    .open = m_open, .close = NULL, .key = m_key,
    .draw = m_draw, .tick = m_tick, .epoch = m_epoch,
    .describe = m_describe, .groups = M_GROUPS,
};
