/*
 * OpenScope 2C53T - how many crash restarts in a row are allowed.
 *
 * A crash restarts the app through the bootloader, and that keeps the power
 * latch (PC9) held. A crash early in every boot would therefore loop for ever
 * with the POWER key dead, so the count of restarts without a healthy run is
 * kept in RAM that survives the jump, and the limit ends the loop.
 *
 * Pure logic, host-tested: make test-crash-policy
 */
#ifndef CRASH_POLICY_H
#define CRASH_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/* Restarts in a row before the unit stops in the bootloader's update screen. */
#define CRASH_RESTART_LIMIT  3u
/* Uptime that counts as a healthy run and clears the count. Longer than a boot
 * (about 5 s) plus the time to reach any screen. */
#define CRASH_HEALTHY_MS     30000u

typedef struct {
    uint32_t magic;
    uint32_t restarts;
    uint32_t check;     /* tells a real count from power-on RAM garbage */
} crash_policy_t;

typedef enum { CRASH_BOOT_RUN, CRASH_BOOT_STOP } crash_boot_t;

/* At boot: garbage reads as zero restarts; at the limit the count is cleared
 * and the answer is STOP, so the next boot starts afresh. */
crash_boot_t crash_policy_boot(crash_policy_t *p);
void         crash_policy_note_crash(crash_policy_t *p);
void         crash_policy_healthy(crash_policy_t *p);
uint32_t     crash_policy_restarts(const crash_policy_t *p);

#endif /* CRASH_POLICY_H */
