/*
 * The scope state mutators as they were before ARCH-1 B7 (src/ui/
 * scope_state.c at fd20895), for the host tests that compare the property
 * setters and the scope screen against them. Not firmware code: nothing in
 * src/ calls these any more.
 */
#ifndef SCOPE_STATE_PRE_B7_H
#define SCOPE_STATE_PRE_B7_H

#include "scope_state.h"
#include "scope_roll.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

/* ═══════════════════════════════════════════════════════════════════
 * Mutators (cycle/adjust with bounds checking)
 * ═══════════════════════════════════════════════════════════════════ */

static inline void scope_cycle_trigger_mode(scope_state_t *s)
{
    s->trigger.mode = (trigger_mode_t)((s->trigger.mode + 1) % TRIG_COUNT);
}

static inline void scope_cycle_trigger_edge(scope_state_t *s)
{
    s->trigger.edge = (trigger_edge_t)((s->trigger.edge + 1) % TRIG_EDGE_COUNT);
}

static inline void scope_cycle_trigger_source(scope_state_t *s)
{
    s->trigger.source = (trigger_source_t)((s->trigger.source + 1) % TRIG_SRC_COUNT);
}

static inline void scope_cycle_coupling(channel_state_t *ch)
{
    ch->coupling = (coupling_t)((ch->coupling + 1) % COUPLING_COUNT);
}

static inline void scope_cycle_probe(channel_state_t *ch)
{
    ch->probe = (probe_t)((ch->probe + 1) % PROBE_COUNT);
}

static inline void scope_toggle_bw_limit(channel_state_t *ch)
{
    ch->bw_limit = !ch->bw_limit;
}

static inline void scope_toggle_channel(channel_state_t *ch)
{
    ch->enabled = !ch->enabled;
}

static inline void scope_adjust_vdiv(channel_state_t *ch, int direction)
{
    int idx = (int)ch->vdiv_idx + direction;
    if (idx < 0) idx = 0;
    if (idx >= VDIV_COUNT) idx = VDIV_COUNT - 1;
    ch->vdiv_idx = (uint8_t)idx;
}

static inline bool scope_adjust_timebase(scope_state_t *s, int direction, bool roll_offered)
{
    /* Stock's ladder continues past the last FPGA record into roll
     * (ms[0x2D] 0x14..0x1C); ours continues past code TIMEBASE_COUNT-1, in
     * a view that takes the roll source. */
    return scope_roll_tb_step(&s->timebase_idx, &s->roll, &s->roll_step,
                              direction, TIMEBASE_COUNT, roll_offered);
}

static inline void scope_adjust_trigger_level(scope_state_t *s, int direction)
{
    s->trigger.level += direction * 5;
    if (s->trigger.level < -100) s->trigger.level = -100;
    if (s->trigger.level > 100) s->trigger.level = 100;
}

static inline void scope_adjust_position(channel_state_t *ch, int direction)
{
    int p = (int)ch->position + direction * SCOPE_POSITION_STEP;
    if (p < -SCOPE_POSITION_MAX) p = -SCOPE_POSITION_MAX;
    if (p >  SCOPE_POSITION_MAX) p =  SCOPE_POSITION_MAX;
    ch->position = (int16_t)p;
}

static inline void scope_toggle_running(scope_state_t *s)
{
    s->running = !s->running;
}

/* ═══════════════════════════════════════════════════════════════════
 * Cursor operations
 * ═══════════════════════════════════════════════════════════════════ */

static inline void scope_cursor_cycle_mode(void)
{
    cursor_state_t *c = &scope_state_get()->cursor;
    c->mode = (cursor_mode_t)((c->mode + 1) % CURSOR_MODE_COUNT);

    switch (c->mode) {
    case CURSOR_VERTICAL:    c->active = CURSOR_SEL_V1; break;
    case CURSOR_HORIZONTAL:  c->active = CURSOR_SEL_H1; break;
    case CURSOR_BOTH:        c->active = CURSOR_SEL_V1; break;
    default:                 c->active = CURSOR_SEL_V1; break;
    }
}

static inline void scope_cursor_next_sel(void)
{
    cursor_state_t *c = &scope_state_get()->cursor;

    switch (c->mode) {
    case CURSOR_VERTICAL:
        c->active = (c->active == CURSOR_SEL_V1) ? CURSOR_SEL_V2 : CURSOR_SEL_V1;
        break;
    case CURSOR_HORIZONTAL:
        c->active = (c->active == CURSOR_SEL_H1) ? CURSOR_SEL_H2 : CURSOR_SEL_H1;
        break;
    case CURSOR_BOTH:
        if (c->active == CURSOR_SEL_V1) c->active = CURSOR_SEL_V2;
        else if (c->active == CURSOR_SEL_V2) c->active = CURSOR_SEL_H1;
        else if (c->active == CURSOR_SEL_H1) c->active = CURSOR_SEL_H2;
        else c->active = CURSOR_SEL_V1;
        break;
    default:
        break;
    }
}



#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#endif /* SCOPE_STATE_PRE_B7_H */
