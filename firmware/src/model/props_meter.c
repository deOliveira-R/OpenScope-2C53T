/*
 * OpenScope 2C53T - meter properties (meter.*)
 *
 * meter.func: the setter stores meter_submode and model_hw()->meter_func
 * does the rest of a function change (cancel the software autoselect unless
 * the change is its own, reset min/max, release hold and relative, post the
 * meter posture for the poll task). A refusal puts the old function back.
 *
 * meter.layout is the View softkey's setter (B6): entering the fuse tester
 * forces DC V through meter.func's setter (the tester reads a DC drop) and
 * says so in the note; entering Limits lets the paint module take limits
 * from the reading (model_hw()->meter_layout). meter.fusetype keeps the
 * rating in amps, as the TYPE keys always did. So `set meter.layout fuse`
 * and the key are one path. The fuse tester's other settings and the
 * beeper threshold are plain state.
 *
 * meter.hold, meter.rel and meter.degf are flags their owners keep
 * (meter_ui.c, meter_data.c: the old shell commands still write them
 * there), read and set through model_hw()->meter_flag_get/set.
 *
 * meter.link is the link policy without a USB host (meter_link_policy.h):
 * plain state here, read by the display task, which owns the decision and
 * turns the link on or off within one frame of a set. meter.live reports
 * what the link is doing now.
 */
#define MODEL_INTERNAL
#include "model.h"
#include "ui.h"
#include "fuse_model.h"
#include "fuse_table.h"
#include "meter_link_policy.h"

#include <stdio.h>

#define HW model_hw()

_Static_assert(METER_SUBMODE_COUNT == 12, "meter.func 0..11 (11 = Auto)");
_Static_assert(METER_LAYOUT_COUNT == 5 && METER_LAYOUT_BIG == 0 && METER_LAYOUT_CHART == 1 &&
               METER_LAYOUT_STATS == 2 && METER_LAYOUT_FUSE == 3 && METER_LAYOUT_LIMITS == 4,
               "meter.layout values = METER_LAYOUT_*");
_Static_assert(FUSE_TYPE_COUNT == 5, "meter.fusetype 0..4");

/* meter_ui.c's short names, token-safe, same order (meter_submode). */
const char *const lbl_meter_func[METER_SUBMODE_COUNT] = {
    "dcv", "acv", "dcma", "dca", "acma", "aca", "ohm", "cont", "diode", "cap", "temp", "auto",
};
const char *const lbl_meter_layout[METER_LAYOUT_COUNT] = { "big", "graph", "stats", "fuse", "limits" };
/* fuse_type_names, token-safe. */
const char *const lbl_fuse_type[FUSE_TYPE_COUNT] = { "ato", "mini", "micro2", "maxi", "jcase" };

_Static_assert(METER_LINK_POLICY_COUNT == 2 && METER_LINK_SCREEN == 0 && METER_LINK_ALWAYS == 1,
               "meter.link 0..1; 0 = screen is what a record without the field holds");
const char *const lbl_meter_link[METER_LINK_POLICY_COUNT] = { "screen", "always" };

int32_t meter_func_get(const prop_desc_t *p) { (void)p; return meter_submode; }
void    meter_func_store(const prop_desc_t *p, int32_t v) { (void)p; meter_submode = (uint8_t)v; }
prop_status_t meter_func_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    /* Stored first: the poll task that serves the posted request reads
     * meter_submode (the order meter_function_set had). meter_auto posts its
     * pick as ORIGIN_AUTO: that must not cancel itself. */
    const uint8_t old = meter_submode;
    meter_func_store(p, v);
    if (HW->meter_func && !HW->meter_func((uint8_t)v, origin == ORIGIN_AUTO)) {
        meter_func_store(p, old);
        model_note(note, "meter posture NOT posted");
        return PS_NOT_SET;
    }
    model_note(note, "posture posted");
    return PS_OK;
}

int32_t meter_layout_get(const prop_desc_t *p) { (void)p; return meter_layout; }
void    meter_layout_store(const prop_desc_t *p, int32_t v) { (void)p; meter_layout = (uint8_t)v; }
prop_status_t meter_layout_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    const bool enter_fuse = (v == METER_LAYOUT_FUSE && meter_layout != METER_LAYOUT_FUSE);
    bool forced = false;
    if (enter_fuse && meter_submode != 0) {
        /* The fuse tester reads a DC drop. Before the layout is stored: a
         * refusal leaves the view where it was. */
        if (meter_func_set(P(METER_FUNC), 0, origin, NULL) != PS_OK) {
            model_note(note, "DC V NOT set");
            return PS_NOT_SET;
        }
        forced = true;
    }
    meter_layout_store(p, v);
    if (HW->meter_layout) HW->meter_layout((uint8_t)v);
    if (forced) model_note(note, "Fuse tester: DC V");
    return PS_OK;
}

static uint8_t rating_count(uint8_t type)
{
    return type < FUSE_TYPE_COUNT ? fuse_tables[type].count : 0u;
}

int32_t meter_fusetype_get(const prop_desc_t *p) { (void)p; return fuse_type; }
void    meter_fusetype_store(const prop_desc_t *p, int32_t v)
{
    (void)p;
    fuse_type = (uint8_t)v;
    if (fuse_rating_idx >= rating_count(fuse_type)) fuse_rating_idx = 0;
}
/* The load hook above keeps the index (a snapshot restores the rating right
 * after the type). A set is a user's type change: like the TYPE keys
 * (fuse_ui.c keep_rating, lifted) it keeps the rating in amps when the new
 * type has it, else the largest below it, else the smallest. */
prop_status_t meter_fusetype_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)p; (void)origin; (void)note;
    const fuse_table_t *ot = &fuse_tables[fuse_type < FUSE_TYPE_COUNT ? fuse_type : 0];
    const uint8_t amps = ot->entries[fuse_rating_idx < ot->count ? fuse_rating_idx : 0].rating_amps;
    const fuse_table_t *nt = &fuse_tables[(uint8_t)v < FUSE_TYPE_COUNT ? (uint8_t)v : 0];
    uint8_t best = 0;
    for (uint8_t i = 0; i < nt->count; i++)
        if (nt->entries[i].rating_amps <= amps) best = i;
    fuse_type = (uint8_t)v;
    fuse_rating_idx = best;
    return PS_OK;
}

int32_t meter_fuserating_get(const prop_desc_t *p) { (void)p; return fuse_rating_idx; }
void    meter_fuserating_store(const prop_desc_t *p, int32_t v)
{
    (void)p;
    const uint8_t n = rating_count(fuse_type);
    fuse_rating_idx = (uint8_t)((v < n) ? v : (n ? n - 1 : 0));
}
/* The table's max is the longest type's; a shorter one clamps (fuse_next_rating stops at its end). */
prop_status_t meter_fuserating_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin;
    const uint8_t n = rating_count(fuse_type);
    meter_fuserating_store(p, v);
    if (v >= n) { model_note(note, "past this type's last rating"); return PS_CLAMPED; }
    return PS_OK;
}

bool meter_fuserating_label(const prop_desc_t *p, int32_t v, char *out, size_t n)
{
    (void)p;
    if (v < 0 || v >= rating_count(fuse_type) || !n) return false;
    snprintf(out, n, "%uA", (unsigned)fuse_tables[fuse_type].entries[v].rating_amps);
    return true;
}

/* fuse_scan_threshold_mv is a float in 0.1 mV steps (0.1 .. 5.0 mV). */
/* (tests/test_model.c checks the table range against FUSE_SCAN_THRESH_*.) */
int32_t meter_fusethr_get(const prop_desc_t *p)
{
    (void)p;
    return (int32_t)(fuse_scan_threshold_mv * 10.0f + 0.5f);
}
void meter_fusethr_store(const prop_desc_t *p, int32_t v) { (void)p; fuse_scan_threshold_mv = (float)v / 10.0f; }
prop_status_t meter_fusethr_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin; (void)note;
    meter_fusethr_store(p, v);
    return PS_OK;
}

/* meter.fuseview: the fuse tester's page (ui.h FUSE_VIEW_*). */
_Static_assert(FUSE_VIEW_COUNT == 4 && FUSE_VIEW_DETAIL == 0 && FUSE_VIEW_MULTI == 1 &&
               FUSE_VIEW_SCAN == 2 && FUSE_VIEW_TYPES == 3, "meter.fuseview values = FUSE_VIEW_*");
const char *const lbl_fuse_view[FUSE_VIEW_COUNT] = { "detail", "table", "scan", "types" };
int32_t meter_fuseview_get(const prop_desc_t *p) { (void)p; return fuse_view; }
void    meter_fuseview_store(const prop_desc_t *p, int32_t v) { (void)p; fuse_view = (uint8_t)v; }
prop_status_t meter_fuseview_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin; (void)note;
    meter_fuseview_store(p, v);
    return PS_OK;
}

/* meter.beep: continuity beeps below this many ohms (2026-10-06). Shorted
 * leads read ~0.14 Ohm as a plain resistance, so the chip's own continuity
 * flag alone never fired. Common DMM practice is 10-50 Ohm. One byte,
 * written on the executor, read by the display and buzzer contexts. */
static const uint16_t k_beep_ohms[METER_BEEP_COUNT] = { 10, 30, 50, 100 };
const char *const lbl_meter_beep[METER_BEEP_COUNT] = { "10ohm", "30ohm", "50ohm", "100ohm" };
static volatile uint8_t g_meter_beep = 1;      /* 30 Ohm */
uint16_t meter_beep_ohms(void) { return k_beep_ohms[g_meter_beep < METER_BEEP_COUNT ? g_meter_beep : 1]; }
int32_t meter_beep_get(const prop_desc_t *p) { (void)p; return g_meter_beep; }
void    meter_beep_store(const prop_desc_t *p, int32_t v)
{
    (void)p;
    g_meter_beep = (uint8_t)((v >= 0 && v < METER_BEEP_COUNT) ? v : 1);
}
prop_status_t meter_beep_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin; (void)note;
    meter_beep_store(p, v);
    return PS_OK;
}

/* meter.hold / meter.rel / meter.degf. Without the ops (host tests of the
 * model alone): off, and a set is refused. */
static int32_t flag_get(uint8_t which)
{
    return (HW->meter_flag_get && HW->meter_flag_get(which)) ? 1 : 0;
}
static prop_status_t flag_set(uint8_t which, int32_t v, prop_note_t *note, const char *refused)
{
    if (!HW->meter_flag_set || !HW->meter_flag_set(which, v != 0, true)) {
        model_note(note, refused);
        return PS_NOT_SET;
    }
    return PS_OK;
}
static void flag_store(uint8_t which, int32_t v)
{
    if (HW->meter_flag_set) (void)HW->meter_flag_set(which, v != 0, false);
}

int32_t meter_hold_get(const prop_desc_t *p) { (void)p; return flag_get(METER_FLAG_HOLD); }
void    meter_hold_store(const prop_desc_t *p, int32_t v) { (void)p; flag_store(METER_FLAG_HOLD, v); }
prop_status_t meter_hold_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)p; (void)origin;
    return flag_set(METER_FLAG_HOLD, v, note, "HOLD: no reading");
}

int32_t meter_rel_get(const prop_desc_t *p) { (void)p; return flag_get(METER_FLAG_REL); }
void    meter_rel_store(const prop_desc_t *p, int32_t v) { (void)p; flag_store(METER_FLAG_REL, v); }
prop_status_t meter_rel_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)p; (void)origin;
    return flag_set(METER_FLAG_REL, v, note, "relative NOT set");
}

int32_t meter_degf_get(const prop_desc_t *p) { (void)p; return flag_get(METER_FLAG_DEGF); }
void    meter_degf_store(const prop_desc_t *p, int32_t v) { (void)p; flag_store(METER_FLAG_DEGF, v); }
prop_status_t meter_degf_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)p; (void)origin;
    return flag_set(METER_FLAG_DEGF, v, note, "unit NOT set");
}

/* meter.link: no hardware in the setter or the store hook (a load runs
 * before the ops are bound). One byte, written on the executor or by a load
 * before the scheduler, read by the display task. */
static volatile uint8_t g_meter_link = METER_LINK_SCREEN;

int32_t meter_link_get(const prop_desc_t *p) { (void)p; return g_meter_link; }
void    meter_link_store(const prop_desc_t *p, int32_t v)
{
    (void)p;
    g_meter_link = (v == METER_LINK_ALWAYS) ? METER_LINK_ALWAYS : METER_LINK_SCREEN;
}
prop_status_t meter_link_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin; (void)note;
    meter_link_store(p, v);
    return PS_OK;
}

/* meter.live: whether the link is on now. Without the op (host tests): off. */
int32_t meter_live_get(const prop_desc_t *p)
{
    (void)p;
    return (HW->meter_link_on && HW->meter_link_on()) ? 1 : 0;
}
