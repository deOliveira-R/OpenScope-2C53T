/*
 * OpenScope 2C53T - meter properties (meter.*)
 *
 * meter.func is meter_function_set (meter_ui.c, input task) split in two:
 * model_hw()->meter_func does what it does to other state (cancel the
 * autoselect unless the change came from it, reset min/max/hold/rel, post
 * the meter posture for fpga_service_requests) and the setter stores
 * meter_submode once that was posted. meter.layout and the fuse tester's
 * settings are plain state (main.c globals, fuse_ui.c's rules).
 */
#define MODEL_INTERNAL
#include "model.h"
#include "ui.h"
#include "fuse_model.h"
#include "fuse_table.h"

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

int32_t meter_func_get(const prop_desc_t *p) { (void)p; return meter_submode; }
void    meter_func_store(const prop_desc_t *p, int32_t v) { (void)p; meter_submode = (uint8_t)v; }
prop_status_t meter_func_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    /* meter_auto posts its pick as ORIGIN_AUTO: that must not cancel itself. */
    if (HW->meter_func && !HW->meter_func((uint8_t)v, origin == ORIGIN_AUTO)) {
        model_note(note, "meter posture NOT posted");
        return PS_NOT_SET;
    }
    meter_func_store(p, v);
    model_note(note, "posture posted");
    return PS_OK;
}

int32_t meter_layout_get(const prop_desc_t *p) { (void)p; return meter_layout; }
void    meter_layout_store(const prop_desc_t *p, int32_t v) { (void)p; meter_layout = (uint8_t)v; }
prop_status_t meter_layout_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin; (void)note;
    meter_layout_store(p, v);
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
/* A new type keeps the rating index when the type has it, else the first. */
prop_status_t meter_fusetype_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin; (void)note;
    meter_fusetype_store(p, v);
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
