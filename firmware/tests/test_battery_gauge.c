/* Host test: battery gauge logic (src/drivers/battery_gauge.c). */
#include <stdio.h>
#include <string.h>
#include "../src/drivers/battery_gauge.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void run(batt_gauge_t *g, uint16_t mv, bool ext, int n)
{
    for (int i = 0; i < n; i++) batt_gauge_step(g, mv, ext);
}

int main(void)
{
    /* Curve: monotonic, ends at 0 and 100, unit #2 anchor points. */
    uint8_t prev = 0;
    for (uint16_t mv = 3000; mv <= 4400; mv++) {
        uint8_t p = batt_gauge_percent(mv);
        CHECK(p >= prev && p <= 100);
        prev = p;
    }
    CHECK(batt_gauge_percent(3299) == 0);
    CHECK(batt_gauge_percent(4131) == 100);          /* resting right after a full charge */
    CHECK(batt_gauge_percent(3874) == 54);           /* 59 min of use */

    batt_gauge_t g;

    /* Full on the cable (4.185 V) and just unplugged (4.131 V) read the same. */
    memset(&g, 0, sizeof g);
    run(&g, 4185, true, 20);
    CHECK(g.external && g.percent == 100 && g.rest_mv == 4135 && g.raw_mv == 4185);
    run(&g, 4131, false, 20);
    CHECK(!g.external && g.percent == 100);

    /* Plugging in at 3.874 V (reads 3.926 V on the cable): no jump at all. */
    memset(&g, 0, sizeof g);
    run(&g, 3874, false, 20);
    const uint8_t before = g.percent;
    for (int i = 0; i < 20; i++) {
        batt_gauge_step(&g, 3924, true);
        CHECK(g.percent == before);
    }
    CHECK(g.external);

    /* The symbol needs 3 readings; a 2-second glitch does not flip it. */
    memset(&g, 0, sizeof g);
    run(&g, 3900, false, 5);
    run(&g, 3900, true, 2);
    CHECK(!g.external);
    run(&g, 3900, false, 1);
    run(&g, 3900, true, 2);
    CHECK(!g.external);
    run(&g, 3900, true, 1);
    CHECK(g.external);

    /* First reading takes the pin as it is (boot on the cable). */
    memset(&g, 0, sizeof g);
    batt_gauge_step(&g, 4000, true);
    CHECK(g.external);

    /* Critical: 10 readings below 3.3 V on battery; never on the cable;
     * clears when the voltage recovers. */
    memset(&g, 0, sizeof g);
    run(&g, 3250, false, 9);
    CHECK(!g.critical);
    run(&g, 3250, false, 1);
    CHECK(g.critical);
    run(&g, 3250, true, 1);
    CHECK(!g.critical);
    run(&g, 3250, true, 30);
    CHECK(!g.critical);
    run(&g, 3250, false, 10);
    CHECK(g.critical);
    run(&g, 3500, false, 1);
    CHECK(!g.critical);

    if (fails) { printf("test_battery_gauge: %d FAILED\n", fails); return 1; }
    printf("test_battery_gauge: OK\n");
    return 0;
}
