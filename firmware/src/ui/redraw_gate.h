/*
 * OpenScope 2C53T - redraw gate
 *
 * The display task wakes every 50 ms. The gate decides what a tick owes the
 * LCD for a screen: nothing, a live frame (only what changed is sent) or a
 * full repaint. The caller folds everything the screen renders out of
 * program state into an "epoch" (any cheap hash; see redraw_epoch_* below)
 * and the gate compares it with the epoch of the last completed draw. Same
 * epoch and no new samples: the next draw would give the same pixels, so it
 * is skipped.
 *
 * Deliberately not a dirty flag set by the UI: the screen's inputs are
 * spread over many owners, and a hash over observable state cannot go stale
 * when someone adds an input and forgets to mark it dirty in one of the
 * places that change it. The cost is one pass over about 20 scalars a tick.
 *
 * Pure C99, no firmware dependencies: tests/test_redraw_gate.c drives it on
 * the host (make -C firmware test-redraw-gate).
 */

#ifndef REDRAW_GATE_H
#define REDRAW_GATE_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════
 * Epoch helpers (FNV-1a). Any hash works; this one is 3 instructions
 * per field and has no table.
 * ═══════════════════════════════════════════════════════════════════ */

#define REDRAW_EPOCH_SEED  2166136261u

static inline uint32_t redraw_epoch_mix(uint32_t h, uint32_t v)
{
    h ^= v;
    h *= 16777619u;
    return h;
}

/* Floats are mixed by bit pattern: we only ever ask "is this the same
 * value as last frame", never "is it close to". memcpy keeps it legal
 * under strict aliasing and compiles to a single move. */
static inline uint32_t redraw_epoch_mix_f(uint32_t h, float v)
{
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    return redraw_epoch_mix(h, bits);
}

/* ═══════════════════════════════════════════════════════════════════
 * Gate
 * ═══════════════════════════════════════════════════════════════════ */

typedef enum {
    REDRAW_SKIP = 0,     /* nothing observable changed: do not touch the LCD */
    REDRAW_INCREMENTAL,  /* a live frame: only what changed is sent */
    REDRAW_FULL          /* everything the screen owns, whatever the glass holds */
} redraw_action_t;

typedef struct {
    uint32_t drawn_epoch;      /* epoch rendered by the last completed draw */
    uint32_t last_draw_frame;  /* tick index of the last completed draw */
    bool     primed;           /* this screen has been painted at least once */
} redraw_gate_t;

typedef struct {
    uint32_t frame;            /* display-task tick counter (50 ms/tick) */
    uint32_t epoch;            /* hash of everything this screen renders */
    bool     new_data;         /* a new capture landed since the last query */
    bool     force;            /* explicit invalidate */
    bool     animating;        /* content is a function of time (a fading ghost, the demo) */

    /* Both are in display-task ticks; 0 disables that arm. */
    uint16_t anim_tick_frames; /* cadence while `animating` */
    uint16_t idle_tick_frames; /* heartbeat when nothing else fires */
} redraw_query_t;

/* Force the next query to draw in full. Use on screen entry / hard invalidate. */
static inline void redraw_gate_invalidate(redraw_gate_t *g)
{
    g->primed = false;
}

/* Record a draw that happened outside the gate (a queued DCMD), so the
 * periodic path does not immediately repeat it. */
static inline void redraw_gate_mark(redraw_gate_t *g, uint32_t epoch,
                                    uint32_t frame, redraw_action_t action)
{
    if (action == REDRAW_SKIP) return;
    g->primed          = true;
    g->drawn_epoch     = epoch;
    g->last_draw_frame = frame;
}

/*
 * Decide what this tick owes the LCD, and latch the decision.
 *
 * Arms, in priority order:
 *   1. not primed / force  -> FULL. Screen entry, an explicit invalidate.
 *   2. epoch moved         -> FULL. A setting the screen shows changed;
 *                             badges, markers and labels must repaint.
 *   3. new data            -> INCREMENTAL, at the capture rate.
 *   4. animating           -> INCREMENTAL on anim_tick_frames.
 *   5. idle heartbeat      -> INCREMENTAL on idle_tick_frames: the slow
 *                             backstop for anything a hash cannot see.
 * Every screen that uses the gate has a live frame, so only 1 and 2 repaint
 * in full.
 * A tick with new data always draws (in full under 1 or 2, which shows the
 * new data too), so nothing has to be remembered between ticks.
 */
static inline redraw_action_t redraw_gate_step(redraw_gate_t *g,
                                               const redraw_query_t *q)
{
    const uint32_t since_draw = q->frame - g->last_draw_frame;

    redraw_action_t action = REDRAW_SKIP;

    if (!g->primed || q->force) {
        action = REDRAW_FULL;
    } else if (q->epoch != g->drawn_epoch) {
        action = REDRAW_FULL;
    } else if (q->new_data) {
        action = REDRAW_INCREMENTAL;
    } else if (q->animating && q->anim_tick_frames != 0 &&
               since_draw >= q->anim_tick_frames) {
        action = REDRAW_INCREMENTAL;
    } else if (q->idle_tick_frames != 0 && since_draw >= q->idle_tick_frames) {
        action = REDRAW_INCREMENTAL;
    }

    redraw_gate_mark(g, q->epoch, q->frame, action);
    return action;
}

#endif /* REDRAW_GATE_H */
