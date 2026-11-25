/*
 * Shared Memory Pool — Lifecycle-Managed
 *
 * Lifecycle-managed BSS buffer shared between on-demand features.
 * Tracks ownership, transition count, and provides diagnostics.
 */

#include "shared_mem.h"
#include <string.h>

/* The shared buffer lives in .bss. */
static uint8_t pool[SHMEM_POOL_SIZE] __attribute__((aligned(4)));
static shmem_owner_t current_owner = SHMEM_OWNER_NONE;
static uint32_t transitions = 0;
/* Who holds the FFT pool's slot. One byte, written by the display task's
 * take only when it differs and by a release only when it is the releaser:
 * a release is never lost to a take on another task. */
static volatile uint8_t slot_tenant = SHMEM_SLOT_NONE;

/* Owner names for diagnostics */
static const char *owner_names[SHMEM_OWNER_COUNT] = {
    "Free",
    "FFT",
    "Persistence",
    "Decode",
    "Bode",
    "Module",
    "Display",
};

/* RAM needs per owner */
static const uint32_t owner_needs[SHMEM_OWNER_COUNT] = {
    0,
    SHMEM_NEED_FFT_TOTAL,   /* FFT proper + the waterfall sub-tenant */
    SHMEM_NEED_PERSISTENCE,
    SHMEM_NEED_DECODE,
    SHMEM_NEED_BODE,
    0, /* Module — variable, not known at compile time */
    SHMEM_NEED_DISPLAY,
};

/* ═══════════════════════════════════════════════════════════════════
 * Core API
 * ═══════════════════════════════════════════════════════════════════ */

uint8_t *shared_mem_acquire(shmem_owner_t owner)
{
    if (current_owner != owner) {
        /* New owner — zero the pool so stale data doesn't cause issues */
        memset(pool, 0, SHMEM_POOL_SIZE);
        current_owner = owner;
        slot_tenant = SHMEM_SLOT_NONE;
        transitions++;
    }
    return pool;
}

void shared_mem_release(void)
{
    current_owner = SHMEM_OWNER_NONE;
}

uint8_t *shared_mem_get(shmem_owner_t expected_owner)
{
    if (current_owner == expected_owner)
        return pool;
    return (uint8_t *)0;
}

uint32_t shared_mem_size(void)
{
    return SHMEM_POOL_SIZE;
}

uint8_t *shared_mem_fft_slot(shmem_slot_tenant_t t, bool *changed)
{
    *changed = slot_tenant != (uint8_t)t;
    if (current_owner != SHMEM_OWNER_FFT) return (uint8_t *)0;
    if (*changed) slot_tenant = (uint8_t)t;
    return pool + SHMEM_FFT_SLOT_OFFSET;
}

void shared_mem_fft_slot_release(shmem_slot_tenant_t t)
{
    if (slot_tenant == (uint8_t)t) slot_tenant = SHMEM_SLOT_NONE;
}

/* ═══════════════════════════════════════════════════════════════════
 * Lifecycle helpers
 * ═══════════════════════════════════════════════════════════════════ */

const char *shared_mem_owner_name(void)
{
    if (current_owner < SHMEM_OWNER_COUNT)
        return owner_names[current_owner];
    return "Unknown";
}

uint32_t shared_mem_owner_need(void)
{
    if (current_owner < SHMEM_OWNER_COUNT)
        return owner_needs[current_owner];
    return 0;
}

uint32_t shared_mem_transition_count(void)
{
    return transitions;
}
