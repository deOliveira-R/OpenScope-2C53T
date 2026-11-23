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
#include "lcd.h"
#include <string.h>
#include <stdio.h>

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
#endif

void watchdog_boot_feed(void)
{
#ifndef EMULATOR_BUILD
    wdt_counter_reload();
    const uint32_t now = DWT->CYCCNT;
    if (boot_fed && now - boot_last_cycles > boot_max_cycles)
        boot_max_cycles = now - boot_last_cycles;
    boot_last_cycles = now;
    boot_fed = true;
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
 * Fault display — red screen of death
 * ═══════════════════════════════════════════════════════════════════ */

void fault_display(const char *title, const char *detail)
{
    /*
     * This runs outside FreeRTOS (interrupts disabled or from a hook).
     * Write directly to LCD — no queues, no RTOS calls.
     */
    lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, COLOR_RED);

    /* Title bar */
    lcd_fill_rect(0, 0, LCD_WIDTH, 30, 0x8000); /* Dark red */
    lcd_draw_string(10, 8, "SYSTEM FAULT", COLOR_WHITE, 0x8000);

    /* Error details */
    lcd_draw_string(10, 50, title, COLOR_WHITE, COLOR_RED);

    if (detail != NULL) {
        lcd_draw_string(10, 80, detail, COLOR_YELLOW, COLOR_RED);
    }

    lcd_draw_string(10, 130, "The system will restart", COLOR_WHITE, COLOR_RED);
    lcd_draw_string(10, 155, "automatically in ~3 seconds.", COLOR_WHITE, COLOR_RED);

    lcd_draw_string(10, 200, "If this persists, hold POWER+PRM",
                    COLOR_GRAY, COLOR_RED);
    lcd_draw_string(10, 218, "at boot for recovery.",
                    COLOR_GRAY, COLOR_RED);

    /*
     * Don't loop forever here — let the watchdog do the reset.
     * The FWDGT is running on its own clock and will fire regardless.
     * Spin just long enough for the user to see the screen.
     */
    volatile uint32_t count = 120000 * 2000; /* ~2 seconds at 120MHz */
    while (count--) {
        __asm volatile("nop");
    }
}

void fault_assert(const char *file, int line)
{
    char detail[64];
    const char *base = file;
    const char *p = file;

    while (p != NULL && *p != '\0') {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
        p++;
    }

    snprintf(detail, sizeof(detail), "%s:%d", base, line);
    fault_display("ASSERT FAIL", detail);
#ifndef EMULATOR_BUILD
    /* Reset rather than hang forever on the red screen (PR #13). The emulator
     * build has no CMSIS core header and therefore no NVIC_SystemReset; halting
     * there is also more useful than resetting, since the assert text stays on
     * screen for inspection. */
    NVIC_SystemReset();
#endif
    for (;;) { }
}
