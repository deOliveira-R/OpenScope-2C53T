/* Host test for src/util/lcd_health.c. */
#include <stdio.h>
#include <string.h>
#include "lcd_health.h"

static int fails, checks;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; printf("  FAIL: %s\n", msg); } } while (0)

int main(void)
{
    lcd_health_t h;
    memset(&h, 0, sizeof h);

    CHECK(lcd_health_pm_ok(0x9C), "0x9C (unit #2 healthy) is ok");
    CHECK(!lcd_health_pm_ok(0x08), "0x08 (after the reset) is not");
    CHECK(!lcd_health_pm_ok(0x98), "display off is not");
    CHECK(!lcd_health_pm_ok(0x8C), "sleep in is not");

    CHECK(!lcd_health_step(&h, 0x9C, 1000, 0), "healthy: no re-init");
    CHECK(!lcd_health_step(&h, 0x08, 2000, 1), "one bad read: wait for a second");
    CHECK(!lcd_health_step(&h, 0x9C, 3000, 1), "a good read clears the streak");
    CHECK(!lcd_health_step(&h, 0x08, 4000, 1), "bad again: first of a new streak");
    CHECK(lcd_health_step(&h, 0x08, 5000, 1), "second bad read in a row: re-init");
    CHECK(h.events == 1 && h.last_event_ms == 5000 && h.event_pm == 0x08 && h.event_mode == 1,
          "the event is recorded");
    CHECK(!lcd_health_step(&h, 0x08, 6000, 1), "after a re-init the streak restarts");
    CHECK(lcd_health_step(&h, 0x08, 7000, 1), "a panel that stays dead re-inits again");
    CHECK(h.checks == 7 && h.bad_reads == 5 && h.events == 2, "counters");

    printf("test_lcd_health: %d/%d passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
