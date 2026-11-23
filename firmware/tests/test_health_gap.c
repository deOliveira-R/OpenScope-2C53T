/*
 * The check-in gap bookkeeping (src/util/health_gap.c).
 * Build and run: make test-health-gap
 */
#include <stdio.h>

#include "health_gap.h"

#define DEADLINE 2000u

static int run, failed;
#define CHECK(cond) do { run++; if (!(cond)) { failed++; printf("  FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

int main(void)
{
    health_gap_t g = { 0 };

    CHECK(health_gap_quiet(&g, 500u) == 0u);    /* not started: nothing to report */
    health_gap_mark(&g, 1000u, DEADLINE);       /* the first mark only starts it */
    CHECK(g.started && g.last == 1000u && g.max == 0u && g.over == 0u);

    health_gap_mark(&g, 1100u, DEADLINE);
    CHECK(g.max == 100u && g.max_at == 1100u);
    health_gap_mark(&g, 1150u, DEADLINE);       /* a shorter gap keeps the longest */
    CHECK(g.max == 100u && g.max_at == 1100u);
    CHECK(health_gap_quiet(&g, 1400u) == 250u);

    health_gap_mark(&g, 1150u + DEADLINE, DEADLINE);    /* exactly the deadline is not over it */
    CHECK(g.over == 0u && g.max == DEADLINE);
    health_gap_mark(&g, 1151u + 2u * DEADLINE, DEADLINE);
    CHECK(g.over == 1u && g.max == DEADLINE + 1u);

    health_gap_clear(&g);
    CHECK(g.max == 0u && g.over == 0u && g.last == 1151u + 2u * DEADLINE);
    CHECK(health_gap_quiet(&g, g.last + 7u) == 7u);     /* the gap in progress survives */

    health_gap_start(&g, 0xFFFFFFF0u);          /* across the tick wrap */
    health_gap_mark(&g, 0x10u, DEADLINE);
    CHECK(g.max == 0x20u && g.over == 0u);
    CHECK(health_gap_quiet(&g, 0x18u) == 8u);

    printf("Results: %d/%d passed\n", run - failed, run);
    return failed ? 1 : 0;
}
