/*
 * The pin sweep's board data (src/re/pin_sweep_table.h): no candidate is a
 * pin that must never be pulsed. The power-hold pin PC9 is the one that
 * matters most: low switches the unit off. The sweep also checks at run
 * time; this fails the build before an image with a bad table exists.
 *
 * Build/run:  make -C firmware test-re-pin-sweep
 */
#include <stdio.h>
#include "../src/re/pin_sweep_table.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void)
{
    CHECK(RE_SWEEP_N > 0u);
    for (unsigned i = 0; i < RE_SWEEP_N; i++) {
        const re_pin_t *p = &RE_SWEEP_CANDIDATES[i];
        if (!re_pin_sweepable(p->port, p->pin)) {
            printf("FAIL candidate P%c%u: %s\n", p->port, (unsigned)p->pin,
                   re_pin_excluded(p->port, p->pin) ? re_pin_excluded(p->port, p->pin) : "not a pin");
            fails++;
        }
        for (unsigned j = 0; j < i; j++)
            CHECK(!(RE_SWEEP_CANDIDATES[j].port == p->port && RE_SWEEP_CANDIDATES[j].pin == p->pin));
        CHECK(p->why && p->why[0]);
    }
    /* the pins whose loss or misuse costs the most are on the list, by name */
    CHECK(re_pin_excluded('C', 9) != NULL);    /* power hold */
    CHECK(re_pin_excluded('C', 6) != NULL);    /* FPGA reload */
    CHECK(re_pin_excluded('B', 3) && re_pin_excluded('B', 4) && re_pin_excluded('B', 5) && re_pin_excluded('B', 6));
    CHECK(re_pin_excluded('A', 13) && re_pin_excluded('A', 14));
    CHECK(!re_pin_sweepable('C', 9) && !re_pin_sweepable('F', 1) && !re_pin_sweepable('A', 16));
    CHECK(re_pin_sweepable('C', 1));
    printf("test_re_pin_sweep: %u candidates, %u excluded pins, %d failures\n",
           (unsigned)RE_SWEEP_N, (unsigned)RE_SWEEP_EXCLUDED_N, fails);
    return fails ? 1 : 0;
}
