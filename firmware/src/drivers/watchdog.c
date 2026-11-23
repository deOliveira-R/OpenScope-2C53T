/*
 * OpenScope 2C53T - Watchdog & System Health Monitor
 *
 * Cooperative watchdog pattern:
 *   1. Each task registers with health_register() at startup
 *   2. Each task calls health_checkin() in its main loop
 *   3. A timer periodically calls health_check(), which:
 *      - Updates stack high water marks
 *      - Checks if any task has missed its deadline
 *      - Feeds the FWDGT only if all tasks are healthy
 *   4. If any task is stalled, the FWDGT starves and resets the MCU
 *
 * The FWDGT timeout (~3s) is deliberately longer than the health check
 * interval so the monitor has time to detect and report the problem.
 */

#include "watchdog.h"
#include "fault.h"
#include <string.h>

#ifndef EMULATOR_BUILD
#include "at32f403a_407_wdt.h"
#endif

/* ═══════════════════════════════════════════════════════════════════
 * Health monitor state
 * ═══════════════════════════════════════════════════════════════════ */

static task_health_t health_tasks[HEALTH_MAX_TASKS];
static int health_count = 0;
static health_gap_t health_timer;      /* the monitor itself, one mark per health_check() */

#define HEALTH_DEADLINE_TICKS  ((uint32_t)pdMS_TO_TICKS(HEALTH_DEADLINE_MS))

/* Decimal digits of the largest source line an assert can name. */
#define ASSERT_LINE_DIGITS     5u

/* ═══════════════════════════════════════════════════════════════════
 * Watchdog (FWDGT) — low-level
 * ═══════════════════════════════════════════════════════════════════ */

void watchdog_init(void)
{
#ifndef EMULATOR_BUILD
    /*
     * FWDGT clock = IRC40K (~40kHz).
     * Prescaler DIV64: 40000 / 64 = 625 Hz per tick.
     * Reload 1875: 1875 / 625 = 3.0 seconds timeout.
     *
     * This gives us a 3-second window. The health monitor runs every
     * 500ms and feeds the WDT if all tasks are healthy, so under
     * normal operation the WDT is fed ~6 times per timeout period.
     */
    wdt_register_write_enable(TRUE);
    wdt_divider_set(WDT_CLK_DIV_64);
    wdt_reload_value_set(1875);
    wdt_counter_reload();
    wdt_enable();
#endif
}

void watchdog_feed(void)
{
#ifndef EMULATOR_BUILD
    wdt_counter_reload();
#endif
}

/* Timed by the DWT cycle counter (started by fault_init), as no tick runs yet.
 * Its 17.9 s wrap at 240 MHz is far above any gap that could be survived. */
#ifndef EMULATOR_BUILD
static bool     boot_fed;
static uint32_t boot_last_cycles, boot_max_cycles;
static const char *boot_max_stage = "none";
#endif

void watchdog_boot_feed(const char *stage_done)
{
#ifndef EMULATOR_BUILD
    wdt_counter_reload();
    const uint32_t now = DWT->CYCCNT;
    if (boot_fed && now - boot_last_cycles > boot_max_cycles) {
        boot_max_cycles = now - boot_last_cycles;
        boot_max_stage = stage_done;
    }
    boot_last_cycles = now;
    boot_fed = true;
#else
    (void)stage_done;
#endif
}

const char *watchdog_boot_max_gap_stage(void)
{
#ifndef EMULATOR_BUILD
    return boot_max_stage;
#else
    return "none";
#endif
}

uint32_t watchdog_boot_max_gap_ms(void)
{
#ifndef EMULATOR_BUILD
    return boot_max_cycles / (system_core_clock / 1000u);
#else
    return 0u;
#endif
}

/* ═══════════════════════════════════════════════════════════════════
 * Task Health Monitor
 * ═══════════════════════════════════════════════════════════════════ */

int health_register(const char *name, TaskHandle_t handle)
{
    if (health_count >= HEALTH_MAX_TASKS)
        return -1;

    int slot = health_count;
    health_tasks[slot].name         = name;
    health_tasks[slot].handle       = handle;
    health_gap_start(&health_tasks[slot].gap, xTaskGetTickCount());
    health_tasks[slot].stack_hwm    = 0;
    health_tasks[slot].registered   = true;
    health_count++;
    return slot;
}

void health_checkin(int slot)
{
    if (slot >= 0 && slot < HEALTH_MAX_TASKS && health_tasks[slot].registered) {
        health_gap_mark(&health_tasks[slot].gap, xTaskGetTickCount(), HEALTH_DEADLINE_TICKS);
    }
}

void health_checkin_self(void)
{
    const TaskHandle_t me = xTaskGetCurrentTaskHandle();
    for (int i = 0; i < HEALTH_MAX_TASKS; i++) {
        if (health_tasks[i].registered && health_tasks[i].handle == me) {
            health_gap_mark(&health_tasks[i].gap, xTaskGetTickCount(), HEALTH_DEADLINE_TICKS);
            return;
        }
    }
}

bool health_is_stalled(int slot)
{
    if (slot < 0 || slot >= HEALTH_MAX_TASKS || !health_tasks[slot].registered)
        return false;

    return health_gap_quiet(&health_tasks[slot].gap, xTaskGetTickCount()) > HEALTH_DEADLINE_TICKS;
}

void health_check(void)
{
    bool all_healthy = true;

    health_gap_mark(&health_timer, xTaskGetTickCount(), HEALTH_DEADLINE_TICKS);
#ifndef EMULATOR_BUILD
    fault_note_uptime_ms(xTaskGetTickCount() * portTICK_PERIOD_MS);
#endif

    for (int i = 0; i < health_count; i++) {
        if (!health_tasks[i].registered)
            continue;

        /* Update stack high water mark */
        health_tasks[i].stack_hwm = uxTaskGetStackHighWaterMark(
            health_tasks[i].handle);

        /* Check deadline */
        if (health_is_stalled(i)) {
            all_healthy = false;
        }
    }

    if (all_healthy) {
        watchdog_feed();
    }
    /* If not all healthy, we deliberately don't feed the watchdog.
     * The FWDGT will reset the MCU after its timeout expires. */
}

const task_health_t *health_get_tasks(void)
{
    return health_tasks;
}

int health_get_count(void)
{
    return health_count;
}

const health_gap_t *health_timer_gap(void)
{
    return &health_timer;
}

void health_clear_gaps(void)
{
    for (int i = 0; i < health_count; i++)
        health_gap_clear(&health_tasks[i].gap);
    health_gap_clear(&health_timer);
}

/* ═══════════════════════════════════════════════════════════════════
 * configASSERT
 * ═══════════════════════════════════════════════════════════════════ */

void fault_assert(const char *file, int line)
{
#ifndef EMULATOR_BUILD
    const char *base = file;
    for (const char *p = file; p != NULL && *p != '\0'; p++)
        if (*p == '/' || *p == '\\') base = p + 1;

    /* file:line, keeping the line when the name is long. No printf: this can
     * run in any context. */
    char detail[sizeof(g_fault.task)];
    char digits[ASSERT_LINE_DIGITS];
    unsigned nd = 0u;
    for (unsigned v = (unsigned)line; nd < sizeof digits; v /= 10u) {
        digits[nd++] = (char)('0' + v % 10u);
        if (v < 10u) break;
    }
    const size_t room = sizeof detail - 2u - nd;    /* ':' and the terminator */
    size_t n = 0u;
    while (base != NULL && base[n] != '\0' && n < room) { detail[n] = base[n]; n++; }
    detail[n++] = ':';
    while (nd > 0u) detail[n++] = digits[--nd];
    detail[n] = '\0';
    fault_crash(FAULT_KIND_ASSERT, detail, (uint32_t)__builtin_return_address(0));
#else
    (void)file;
    (void)line;
    for (;;) { }    /* the emulator stops here, for inspection */
#endif
}
