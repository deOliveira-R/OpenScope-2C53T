/*
 * OpenScope 2C53T - how long something went without checking in.
 *
 * Pure bookkeeping on a wrapping tick counter, so tests/test_health_gap.c runs
 * it on the host. The watchdog's health monitor (drivers/watchdog.c) keeps one
 * per task; the numbers say what timeout the watchdog could safely use.
 */
#ifndef HEALTH_GAP_H
#define HEALTH_GAP_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     started;
    uint32_t last;          /* tick of the last check-in */
    uint32_t max;           /* longest gap, ticks */
    uint32_t max_at;        /* tick at which that gap ended */
    uint32_t over;          /* gaps longer than the deadline */
} health_gap_t;

void     health_gap_start(health_gap_t *g, uint32_t now);
void     health_gap_mark(health_gap_t *g, uint32_t now, uint32_t deadline);
uint32_t health_gap_quiet(const health_gap_t *g, uint32_t now);
void     health_gap_clear(health_gap_t *g);

#endif /* HEALTH_GAP_H */
