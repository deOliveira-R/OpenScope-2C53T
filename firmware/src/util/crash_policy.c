/* See crash_policy.h. */
#include "crash_policy.h"

#define CRASH_POLICY_MAGIC 0xC4A5B007u

static uint32_t check_of(uint32_t restarts)
{
    return ~CRASH_POLICY_MAGIC ^ restarts;
}

static void set_restarts(crash_policy_t *p, uint32_t restarts)
{
    p->magic    = CRASH_POLICY_MAGIC;
    p->restarts = restarts;
    p->check    = check_of(restarts);
}

uint32_t crash_policy_restarts(const crash_policy_t *p)
{
    const bool valid = p->magic == CRASH_POLICY_MAGIC && p->check == check_of(p->restarts);
    return valid ? p->restarts : 0u;
}

crash_boot_t crash_policy_boot(crash_policy_t *p)
{
    const uint32_t n = crash_policy_restarts(p);
    if (n >= CRASH_RESTART_LIMIT) {
        set_restarts(p, 0u);
        return CRASH_BOOT_STOP;
    }
    set_restarts(p, n);
    return CRASH_BOOT_RUN;
}

void crash_policy_note_crash(crash_policy_t *p)
{
    const uint32_t n = crash_policy_restarts(p);
    set_restarts(p, n < CRASH_RESTART_LIMIT ? n + 1u : n);
}

void crash_policy_healthy(crash_policy_t *p)
{
    set_restarts(p, 0u);
}
