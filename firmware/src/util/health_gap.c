/*
 * OpenScope 2C53T - how long something went without checking in (health_gap.h)
 */
#include "health_gap.h"

void health_gap_start(health_gap_t *g, uint32_t now)
{
    g->started = true;
    g->last = now;
    g->max = 0u;
    g->max_at = now;
    g->over = 0u;
}

void health_gap_mark(health_gap_t *g, uint32_t now, uint32_t deadline)
{
    if (!g->started) {
        health_gap_start(g, now);
        return;
    }
    const uint32_t gap = now - g->last;     /* unsigned: right across a tick wrap */
    if (gap > g->max) {
        g->max = gap;
        g->max_at = now;
    }
    if (gap > deadline) g->over++;
    g->last = now;
}

uint32_t health_gap_quiet(const health_gap_t *g, uint32_t now)
{
    return g->started ? now - g->last : 0u;
}

/* Keeps `last`: the gap in progress still counts when it ends. */
void health_gap_clear(health_gap_t *g)
{
    g->max = 0u;
    g->max_at = g->last;
    g->over = 0u;
}
