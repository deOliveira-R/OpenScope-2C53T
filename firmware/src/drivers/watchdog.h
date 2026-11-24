/*
 * OpenScope 2C53T - Watchdog & System Health Monitor
 *
 * Uses the GD32F307 FWDGT (Free Watchdog Timer) with a cooperative
 * health-check pattern: the watchdog is only fed when ALL registered
 * tasks have checked in within the deadline. If a task hangs, the monitor
 * records it and restarts through the crash path, power held. If the monitor
 * itself dies, the unfed watchdog resets the chip, which switches the unit off.
 *
 * This directly addresses community issues #1 (auto-measurement hang),
 * #2 (GUI lockup), and #7 (unexpected restarts/freezes).
 */

#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <stdint.h>
#include <stdbool.h>
#include "FreeRTOS.h"
#include "task.h"
#include "health_gap.h"

/* Images that run the hardware watchdog; the others only measure (dry run). */
#if !defined(GUEST_BUILD) && !defined(NO_WATCHDOG) && !defined(EMULATOR_BUILD)
#define WATCHDOG_ARMED
#endif

/* Maximum number of tasks that can register with the health monitor */
#define HEALTH_MAX_TASKS    6

/* Deadline: if a task hasn't checked in for this many ticks, it's stalled */
#define HEALTH_DEADLINE_MS  2000

/* ═══════════════════════════════════════════════════════════════════
 * Watchdog (FWDGT) — low-level
 * ═══════════════════════════════════════════════════════════════════ */

/* The hardware timeout: RELOAD x DIVIDER cycles of LICK, 3 s at a nominal 40 kHz.
 * LICK is imprecise: `health wdtclock` measures this unit's real timeout. */
#define WATCHDOG_DIVIDER    64u
#define WATCHDOG_RELOAD     1875u

/* Initialize FWDGT with ~3 second timeout (must be > HEALTH_DEADLINE_MS) */
void watchdog_init(void);

/* Feed the watchdog — only called by the health monitor, not by tasks */
void watchdog_feed(void);

/* main() before the scheduler: feed at the end of a boot stage, and record the
 * longest time between two feeds and the stage that took it. A watchdog left
 * running across a direct jump has to survive that. */
void watchdog_boot_feed(const char *stage_done);
uint32_t watchdog_boot_max_gap_ms(void);
const char *watchdog_boot_max_gap_stage(void);

/* ═══════════════════════════════════════════════════════════════════
 * Task Health Monitor — cooperative check-in pattern
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    const char    *name;               /* Task name (for diagnostics) */
    TaskHandle_t   handle;             /* FreeRTOS task handle */
    health_gap_t   gap;                 /* last check-in, longest gap (ticks) */
    UBaseType_t    stack_hwm;           /* Stack high water mark (words) */
    bool           registered;         /* Slot in use */
} task_health_t;

/* Register a task for health monitoring. Returns slot index or -1 on failure. */
int health_register(const char *name, TaskHandle_t handle);

/* Task check-in: call this periodically from each task's main loop */
void health_checkin(int slot);

/* Check in for whichever registered task is calling (no-op for an
 * unregistered one). For long operations in shared code that does not know
 * its caller's slot, e.g. a screenshot save from the key task or the shell. */
void health_checkin_self(void);

/* Run one health check cycle: update HWMs, check deadlines, then feed the WDT,
 * or restart through fault_crash() if a task is stalled. Thread mode only:
 * called from the health timer's callback. */
void health_check(void);

/* Get read-only pointer to task health array (for diagnostics display) */
const task_health_t *health_get_tasks(void);

/* Get count of registered tasks */
int health_get_count(void);

/* The health timer's own check-in gaps; clear all the recorded maxima. */
const health_gap_t *health_timer_gap(void);
void health_clear_gaps(void);

/* Check if a specific task is stalled */
bool health_is_stalled(int slot);

/* ═══════════════════════════════════════════════════════════════════
 * Failed assertions
 * ═══════════════════════════════════════════════════════════════════ */

/* configASSERT: record file:line and end as any crash does (fault.h). */
void fault_assert(const char *file, int line);

#endif /* WATCHDOG_H */
