/*
 * The crash restart limit (src/util/crash_policy.c).
 * Build and run: make test-crash-policy
 */
#include <stdio.h>
#include <string.h>

#include "crash_policy.h"

static int run, failed;
#define CHECK(cond) do { run++; if (!(cond)) { failed++; printf("  FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

int main(void)
{
    crash_policy_t p;

    memset(&p, 0xA5, sizeof p);                     /* power-on garbage */
    CHECK(crash_policy_restarts(&p) == 0u);
    CHECK(crash_policy_boot(&p) == CRASH_BOOT_RUN);
    CHECK(crash_policy_restarts(&p) == 0u);

    for (uint32_t i = 1u; i < CRASH_RESTART_LIMIT; i++) {
        crash_policy_note_crash(&p);
        CHECK(crash_policy_restarts(&p) == i);
        CHECK(crash_policy_boot(&p) == CRASH_BOOT_RUN);
    }
    crash_policy_note_crash(&p);                    /* the last one allowed */
    CHECK(crash_policy_restarts(&p) == CRASH_RESTART_LIMIT);
    crash_policy_note_crash(&p);                    /* a crash at the limit does not overflow */
    CHECK(crash_policy_restarts(&p) == CRASH_RESTART_LIMIT);
    CHECK(crash_policy_boot(&p) == CRASH_BOOT_STOP);
    CHECK(crash_policy_restarts(&p) == 0u);         /* the stop clears it: next boot runs */
    CHECK(crash_policy_boot(&p) == CRASH_BOOT_RUN);

    crash_policy_note_crash(&p);
    crash_policy_healthy(&p);                       /* a healthy run forgets earlier crashes */
    CHECK(crash_policy_restarts(&p) == 0u);

    crash_policy_note_crash(&p);
    p.restarts ^= 2u;                               /* a damaged count is not believed */
    CHECK(crash_policy_restarts(&p) == 0u);

    printf("crash policy: %d/%d passed\n", run - failed, run);
    return failed ? 1 : 0;
}
