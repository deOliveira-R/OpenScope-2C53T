/*
 * OpenScope 2C53T — fault capture
 *
 * Background (analysis_v120/hardfault_idle_task_2026-08-11.md): this unit
 * HardFaults ~55 s into every boot and has done for weeks. Diagnosing it was
 * needlessly hard because:
 *
 *   * MemManage / BusFault / UsageFault handlers were all DISABLED (SHCSR = 0),
 *     so every fault escalated to HardFault and the specific cause was lost.
 *   * CFSR is write-1-to-clear and nothing ever cleared it, so its bits
 *     ACCUMULATE across every fault since reset — the value describes a smear of
 *     events, not one event.
 *   * the default handlers are a bare `b .`, so nothing was recorded at all and
 *     the evidence had to be reconstructed by hand over SWD.
 *
 * This fixes all three. The record lives in .noinit so it SURVIVES a reset,
 * which matters for an intermittent fault: the device can die, reset, and still
 * tell you what happened on the previous boot.
 */

#ifndef FAULT_H
#define FAULT_H

#include <stdint.h>
#include <stdbool.h>

#define FAULT_MAGIC 0xFA17ED00u

/* Which handler ran. Recorded because with the configurable handlers enabled a
 * BusFault no longer masquerades as a HardFault. */
typedef enum {
    FAULT_KIND_NONE = 0,
    FAULT_KIND_HARD,
    FAULT_KIND_MEMMANAGE,
    FAULT_KIND_BUS,
    FAULT_KIND_USAGE,
    FAULT_KIND_STACK,      /* FreeRTOS stack overflow check; detail = task */
    FAULT_KIND_HEAP,       /* pvPortMalloc returned NULL */
    FAULT_KIND_ASSERT,     /* configASSERT; detail = file:line */
} fault_kind_t;

typedef struct {
    uint32_t magic;        /* FAULT_MAGIC once fully written */
    uint32_t kind;         /* fault_kind_t */
    uint32_t boot_count;   /* increments each boot; tells old records from new */

    /* SCB fault status, sampled before anything else can disturb it. */
    uint32_t cfsr, hfsr, dfsr, afsr, mmfar, bfar;

    /* The exception frame the core stacked. */
    uint32_t r0, r1, r2, r3, r12, lr, pc, xpsr;

    uint32_t exc_return;   /* bit2: 0 = frame on MSP, 1 = on PSP */
    uint32_t sp;           /* the stack the frame was found on */
    uint32_t cyccnt;       /* DWT cycle counter at fault. NOT a FreeRTOS tick:
                            * the handler must not call FreeRTOS (see fault.c). */
    char     task[16];     /* the detail of a software crash (task, file:line). Empty
                            * for a CPU fault: identify the task offline from
                            * `sp`, which lands in its stack. */
} fault_record_t;

/* Non-static so the address comes out of the ELF with nm and the record can be
 * read over SWD on a dead target — which is how this one has to be read. */
extern volatile fault_record_t g_fault;

/*
 * Enable the configurable fault handlers, clear stale CFSR/HFSR bits, and note
 * whether the PREVIOUS boot died. Call early in main(), before anything that
 * could fault. Safe before the scheduler starts.
 */
void fault_init(void);

/*
 * True when too many crash restarts came in a row (crash_policy.h). Call once,
 * right after fault_init(); the caller then stops in the bootloader.
 */
bool fault_restart_limit_reached(void);

/* A run that lasted CRASH_HEALTHY_MS: earlier crashes no longer count. */
void fault_note_uptime_ms(uint32_t uptime_ms);

/*
 * A crash found by software: record it, then end as a CPU fault does.
 *
 * How every crash ends:
 *   - from a task, or a handler with nothing beneath it: the CPU leaves the
 *     handler and restarts through the bootloader, power held, record kept;
 *   - inside a nested interrupt, which cannot be left safely: it stops, and
 *     a press of POWER switches the unit off. Never a chip reset.
 */
void fault_crash(uint32_t kind, const char *detail, uint32_t lr) __attribute__((noreturn));

/* What the previous run died of, or NULL; restarts in a row so far. */
const char *fault_last_summary(void);
uint32_t    fault_restarts_in_a_row(void);

/* Crash to the first watchdog feed of the restart, in ms; 0 if this boot was
 * not a crash restart. The watchdog is not fed in that time. */
uint32_t fault_last_restart_ms(void);

/* Bench test: the next button-scan interrupt executes an undefined instruction. */
extern volatile bool fault_test_in_irq;

/* Bench test: the next FAULT_TEST_BOOT_CRASHES boots crash during startup. */
void fault_test_boot_arm(void);
void fault_test_boot_point(void);

#endif /* FAULT_H */
