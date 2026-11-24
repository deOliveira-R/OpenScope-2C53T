/*
 * OpenScope 2C53T — fault capture. See fault.h for why this exists.
 */

#include "fault.h"
#include "crash_policy.h"
#include "dfu_boot.h"
#include "at32f403a_407.h"
/* Deliberately NOT including FreeRTOS headers: nothing in the fault path
 * may call a FreeRTOS API. See the note in fault_capture(). */

#include <stdio.h>
#include <string.h>

/* .noinit is NOLOAD in the linker script, so startup neither zeroes nor loads
 * it: the record survives a reset and can be reported on the next boot. */
volatile fault_record_t g_fault __attribute__((section(".noinit")));

static bool  prev_valid;
static char  prev_summary[80];

/* Captured before g_fault is overwritten, so the CURRENT boot can still report
 * what the PREVIOUS one did. */
static fault_record_t prev;

/* Survives the jump through the bootloader, like the record. */
static crash_policy_t crash_state __attribute__((section(".noinit")));

volatile bool fault_test_in_irq;

/* Set at a crash, consumed by the next fault_init: the restart time is only
 * meaningful for the boot right after a crash. */
static uint32_t restart_mark __attribute__((section(".noinit")));
static uint32_t restart_cycles;

/* From the bootloader's SystemInit until the app's clock setup the core runs
 * on the HICK, so nearly all of a restart is counted at this rate. The few
 * cycles at 240 MHz before it only make the figure slightly pessimistic. */
#define FAULT_RESTART_CLOCK_HZ     HICK_VALUE

/* Startup crash test. With the crash that arms it, this makes
 * CRASH_RESTART_LIMIT crashes in a row, so the loop limit is what ends it. */
#define FAULT_TEST_BOOT_CRASHES    (CRASH_RESTART_LIMIT - 1u)
#define FAULT_TEST_BOOT_MAGIC      0xB0070E00u
#define FAULT_TEST_BOOT_COUNT_MASK 0xFFu
static uint32_t boot_test __attribute__((section(".noinit")));

/* Room below the top of the main stack for the frame that leaves the handler
 * and for the restart code; the old stack is abandoned by then. Double-word
 * aligned, as an exception frame must be. */
#define FAULT_RESTART_STACK_BYTES  64u
#define FAULT_FRAME_WORDS          8u
#define FAULT_XPSR_THUMB                 0x01000000u
#define FAULT_EXC_RETURN_THREAD      0xFFFFFFF9u
/* PC8, the POWER key: active low. PC9 is the power latch. */
#define FAULT_POWER_KEY_MASK       (1u << 8)
#define FAULT_POWER_HOLD_MASK      (1u << 9)
/* Reads of the key in a row that count as a level: tens of ms at 240 MHz, past
 * contact bounce. */
#define FAULT_KEY_STABLE_READS     1000000u

extern uint32_t _estack;

static const char *kind_name(uint32_t k)
{
    switch (k) {
        case FAULT_KIND_HARD:       return "HARD";
        case FAULT_KIND_MEMMANAGE:  return "MEM";
        case FAULT_KIND_BUS:        return "BUS";
        case FAULT_KIND_USAGE:      return "USAGE";
        case FAULT_KIND_STACK:      return "STACK";
        case FAULT_KIND_HEAP:       return "HEAP";
        case FAULT_KIND_ASSERT:     return "ASSERT";
        case FAULT_KIND_STALL:      return "STALL";
        default:                    return "?";
    }
}

void fault_init(void)
{
    prev_valid = (g_fault.magic == FAULT_MAGIC);
    /* DWT keeps counting through the jump; it is restarted further down. */
    restart_cycles = (prev_valid && restart_mark == FAULT_MAGIC) ? DWT->CYCCNT - g_fault.cyccnt : 0u;
    restart_mark = 0u;
    if (prev_valid) {
        memcpy(&prev, (const void *)&g_fault, sizeof(prev));
        snprintf(prev_summary, sizeof(prev_summary),
                 "%s pc=%08lx lr=%08lx cfsr=%08lx t=%lu %s",
                 kind_name(prev.kind),
                 (unsigned long)prev.pc, (unsigned long)prev.lr,
                 (unsigned long)prev.cfsr, (unsigned long)prev.cyccnt,
                 prev.task);
    } else {
        prev_summary[0] = '\0';
    }

    /* Keep the record for inspection but bump the boot counter so a stale
     * record is distinguishable from one produced this boot. */
    uint32_t boots = prev_valid ? prev.boot_count : 0u;
    g_fault.boot_count = boots + 1u;
    /* The record stays valid until a healthy run (fault_note_uptime_ms): a
     * crash loop that stops in the bootloader must still be readable after. */

    /* Enable MemManage / BusFault / UsageFault. Without these every fault
     * escalates to HardFault and the specific cause is lost — which is exactly
     * what happened to the ~55 s fault. */
    SCB->SHCSR |= (SCB_SHCSR_MEMFAULTENA_Msk |
                   SCB_SHCSR_BUSFAULTENA_Msk |
                   SCB_SHCSR_USGFAULTENA_Msk);

    /* CFSR is write-1-to-clear and nothing ever cleared it, so its bits used to
     * accumulate across every fault since reset. Clear it so the next value
     * describes ONE event. */
    SCB->CFSR  = SCB->CFSR;
    SCB->HFSR  = SCB->HFSR;

    /* Precise bus faults are much easier to attribute than imprecise ones.
     * ACTLR.DISDEFWBUF forces the write buffer off so a faulting store reports
     * at the instruction that caused it rather than several instructions later.
     * Costs a little store throughput; worth it while this bug is open.
     *
     * ACTLR is at 0xE000E008 and is not a member of SCB_Type in this CMSIS
     * version, hence the direct access. */
    *(volatile uint32_t *)0xE000E008u |= (1u << 1);

    /* DWT cycle counter — the fault handler's only timestamp source, since it
     * must not call into FreeRTOS. At 240 MHz CYCCNT wraps every ~17.9 s, so it
     * gives sub-wrap precision rather than absolute uptime; pair it with the
     * boot_count and the observed ~55 s figure. */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL  |= 1u;            /* CYCCNTENA */
}

bool fault_restart_limit_reached(void)
{
    return crash_policy_boot(&crash_state) == CRASH_BOOT_STOP;
}

void fault_note_uptime_ms(uint32_t uptime_ms)
{
    if (uptime_ms >= CRASH_HEALTHY_MS) {
        crash_policy_healthy(&crash_state);
        g_fault.magic = 0u;      /* this run still reports it, from its own copy */
    }
}

const char *fault_last_summary(void)
{
    return prev_valid ? prev_summary : NULL;
}

uint32_t fault_last_restart_ms(void)
{
    return restart_cycles / (FAULT_RESTART_CLOCK_HZ / 1000u);
}

void fault_test_boot_arm(void)
{
    boot_test = FAULT_TEST_BOOT_MAGIC | FAULT_TEST_BOOT_CRASHES;
}

void fault_test_boot_point(void)
{
#if defined(FEATURE_BENCHSHELL) && FEATURE_BENCHSHELL
    if ((boot_test & ~FAULT_TEST_BOOT_COUNT_MASK) != FAULT_TEST_BOOT_MAGIC) return;
    const uint32_t left = boot_test & FAULT_TEST_BOOT_COUNT_MASK;
    boot_test = (left > 1u) ? FAULT_TEST_BOOT_MAGIC | (left - 1u) : 0u;
    if (left > 0u) __asm volatile ("udf #0");
#endif
}

uint32_t fault_restarts_in_a_row(void)
{
    return crash_policy_restarts(&crash_state);
}

/* Returns once the POWER key has read `pressed` for a stable stretch. */
static void fault_wait_key(bool pressed)
{
    for (uint32_t same = 0u; same < FAULT_KEY_STABLE_READS; ) {
        const bool now = (GPIOC->idt & FAULT_POWER_KEY_MASK) == 0u;
        same = (now == pressed) ? same + 1u : 0u;
    }
}

/* The nested case: no way back to thread mode. The screen stays as it was and
 * POWER still switches the unit off (release first: it may be held already). */
static void fault_stop(void) __attribute__((noreturn));
static void fault_stop(void)
{
    fault_wait_key(false);
    fault_wait_key(true);
    GPIOC->clr = FAULT_POWER_HOLD_MASK;
    for (;;) { }
}

/* Leave Handler mode: an exception return into the restart code, on a fresh
 * frame at the top of the main stack. The bootloader and the app must start in
 * thread mode with nothing active, or no interrupt would ever be taken. */
static void fault_return_to_restart(void) __attribute__((noreturn));
static void fault_return_to_restart(void)
{
    SCB->ICSR   = SCB_ICSR_PENDSVCLR_Msk | SCB_ICSR_PENDSTCLR_Msk;
    SCB->SHCSR &= ~(SCB_SHCSR_MEMFAULTPENDED_Msk | SCB_SHCSR_BUSFAULTPENDED_Msk |
                    SCB_SHCSR_USGFAULTPENDED_Msk);
    FPU->FPCCR &= ~FPU_FPCCR_LSPACT_Msk;   /* no lazy FP state to save any more */
    __set_BASEPRI(0u);

    /* volatile: the CPU reads this frame, which the compiler cannot see, so
     * plain stores would be dropped as dead and the return would go to 0. */
    volatile uint32_t *const frame =
        (volatile uint32_t *)(((uint32_t)&_estack - FAULT_RESTART_STACK_BYTES) & ~7u);
    for (unsigned i = 0u; i < FAULT_FRAME_WORDS; i++) frame[i] = 0u;
    frame[5] = 0xFFFFFFFFu;                                /* LR: nothing to return to */
    frame[6] = (uint32_t)(uintptr_t)dfu_restart_app & ~1u; /* PC */
    frame[7] = FAULT_XPSR_THUMB;

    __asm volatile (
        "msr msp, %0 \n"
        "dsb         \n"
        "isb         \n"
        "bx  %1      \n"
        :: "r" (frame), "r" (FAULT_EXC_RETURN_THREAD) : "memory");
    __builtin_unreachable();
}

/* Every crash ends here, with the record already written. */
static void fault_end(void) __attribute__((noreturn));
static void fault_end(void)
{
    __disable_irq();
    restart_mark = FAULT_MAGIC;
    crash_policy_note_crash(&crash_state);
    if (__get_IPSR() == 0u) dfu_restart_app();
    if (SCB->ICSR & SCB_ICSR_RETTOBASE_Msk) fault_return_to_restart();
    fault_stop();
}

void fault_crash(uint32_t kind, const char *detail, uint32_t lr)
{
    __disable_irq();
    g_fault.magic = 0u;
    g_fault.kind  = kind;
    g_fault.cfsr  = g_fault.hfsr = g_fault.dfsr = g_fault.afsr = 0u;
    g_fault.mmfar = g_fault.bfar = 0u;
    g_fault.r0 = g_fault.r1 = g_fault.r2 = g_fault.r3 = g_fault.r12 = 0u;
    g_fault.lr = lr;
    g_fault.pc = lr;
    g_fault.xpsr = __get_IPSR();
    g_fault.exc_return = 0u;
    g_fault.sp = __get_PSP();
    g_fault.cyccnt = DWT->CYCCNT;

    size_t n = 0u;
    while (detail != NULL && detail[n] != '\0' && n < sizeof(g_fault.task) - 1u) {
        g_fault.task[n] = detail[n];
        n++;
    }
    g_fault.task[n] = '\0';

    __DMB();
    g_fault.magic = FAULT_MAGIC;
    __DMB();
    fault_end();
}

/*
 * Record, then restart. Deliberately minimal and allocation-free: the record is
 * written FIRST, so if anything later in this function faults again (which is
 * how the core ends up in LOCKUP with pc=0xFFFFFFFE) the evidence is already
 * safe in .noinit.
 */
static void fault_capture(uint32_t *frame, uint32_t exc_return, uint32_t kind)
{
    g_fault.magic = 0;                    /* invalid while being written */

    g_fault.kind  = kind;
    g_fault.cfsr  = SCB->CFSR;
    g_fault.hfsr  = SCB->HFSR;
    g_fault.dfsr  = SCB->DFSR;
    g_fault.afsr  = SCB->AFSR;
    g_fault.mmfar = SCB->MMFAR;
    g_fault.bfar  = SCB->BFAR;

    g_fault.r0   = frame[0];
    g_fault.r1   = frame[1];
    g_fault.r2   = frame[2];
    g_fault.r3   = frame[3];
    g_fault.r12  = frame[4];
    g_fault.lr   = frame[5];
    g_fault.pc   = frame[6];
    g_fault.xpsr = frame[7];

    g_fault.exc_return = exc_return;
    g_fault.sp         = (uint32_t)frame;

    /* Timestamp from the DWT cycle counter, NOT from FreeRTOS.
     *
     * The first version of this called xTaskGetTickCountFromISR() here and it
     * destroyed the whole handler. That function runs
     * portASSERT_IF_INTERRUPT_PRIORITY_INVALID(), which ALWAYS fails in a fault
     * handler (priority -1/-2, above configMAX_SYSCALL_INTERRUPT_PRIORITY), and
     * this project's configASSERT then drove the LCD over EXMC from inside a
     * fault handler. That faulted again, escalated,
     * and locked the core up at 0xFFFFFFFE with the record already invalidated
     * by the magic=0 above. Measured: SHCSR showed USGFAULTACT set with
     * CFSR = UNDEFINSTR and HFSR = FORCED, and g_fault was never written.
     *
     * A fault handler must touch NOTHING but architectural state. No FreeRTOS
     * API, no LCD, no printf. DWT is a core peripheral: a plain register read
     * that cannot assert, block or fault. */
    g_fault.cyccnt = DWT->CYCCNT;

    /* The task is identified offline from `sp`: each task's stack occupies a
     * known range, so the stack the frame landed on names the task without
     * calling pcTaskGetName() — which asserts on a bad handle, and pxCurrentTCB
     * may be exactly what is corrupt. */
    g_fault.task[0] = '\0';

    __DMB();
    g_fault.magic = FAULT_MAGIC;
    __DMB();

    fault_end();
}

/*
 * EXC_RETURN bit 2 selects which stack the core pushed the frame onto: 0 = MSP
 * (fault in a handler), 1 = PSP (fault in a task). Getting this wrong means
 * reading eight words of unrelated stack and inventing a plausible, wrong
 * answer — so it is resolved here rather than guessed at afterwards.
 */
#define FAULT_ENTRY(name, kind)                     \
    __attribute__((naked)) void name(void)          \
    {                                               \
        __asm volatile (                            \
            "tst   lr, #4              \n"          \
            "ite   eq                  \n"          \
            "mrseq r0, msp             \n"          \
            "mrsne r0, psp             \n"          \
            "mov   r1, lr              \n"          \
            "mov   r2, %0              \n"          \
            "b     fault_capture_c     \n"          \
            :: "i" (kind) : "r0", "r1", "r2"        \
        );                                          \
    }

void fault_capture_c(uint32_t *frame, uint32_t exc_return, uint32_t kind);
void fault_capture_c(uint32_t *frame, uint32_t exc_return, uint32_t kind)
{
    fault_capture(frame, exc_return, kind);
}

FAULT_ENTRY(HardFault_Handler,  FAULT_KIND_HARD)
FAULT_ENTRY(MemManage_Handler,  FAULT_KIND_MEMMANAGE)
FAULT_ENTRY(BusFault_Handler,   FAULT_KIND_BUS)
FAULT_ENTRY(UsageFault_Handler, FAULT_KIND_USAGE)
