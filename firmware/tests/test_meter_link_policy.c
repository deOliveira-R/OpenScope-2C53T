/*
 * Host test for the meter link policy (src/model/meter_link_policy.c).
 *
 * The full truth table of the decision, and the debounce of the USB host
 * state: a re-enumeration shorter than the hold time never changes the
 * answer, so it never reaches PC11 (each rise restarts the meter SoC).
 *
 * Build/run:  make -C firmware test-meter-link-policy
 */
#include "meter_link_policy.h"

#include <stdio.h>
#include <string.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static void test_truth_table(void)
{
    /* screen, usb, policy -> want_on: all eight rows, spelled out. */
    static const struct { bool screen, usb; meter_link_policy_t pol; bool want; } rows[] = {
        { false, false, METER_LINK_SCREEN, false },   /* scope shown, on battery, default: off */
        { false, false, METER_LINK_ALWAYS, true  },   /* the user's "always"                    */
        { false, true,  METER_LINK_SCREEN, true  },   /* a host can take the data               */
        { false, true,  METER_LINK_ALWAYS, true  },
        { true,  false, METER_LINK_SCREEN, true  },   /* the meter screen is shown              */
        { true,  false, METER_LINK_ALWAYS, true  },
        { true,  true,  METER_LINK_SCREEN, true  },
        { true,  true,  METER_LINK_ALWAYS, true  },
    };
    CHECK(sizeof rows / sizeof rows[0] == 8);
    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        CHECK(meter_link_want_on(rows[i].screen, rows[i].usb, rows[i].pol) == rows[i].want);
        /* and against the formula itself */
        CHECK(rows[i].want == (rows[i].screen || rows[i].usb || rows[i].pol == METER_LINK_ALWAYS));
    }
    /* Exactly one row is off. */
    unsigned off = 0;
    for (int s = 0; s < 2; s++) for (int u = 0; u < 2; u++) for (int p = 0; p < METER_LINK_POLICY_COUNT; p++)
        if (!meter_link_want_on(s, u, (meter_link_policy_t)p)) off++;
    CHECK(off == 1);
    /* The record's values: 0 (what every old record holds) is "screen". */
    CHECK(METER_LINK_SCREEN == 0 && METER_LINK_ALWAYS == 1 && METER_LINK_POLICY_COUNT == 2);
}

static void test_debounce(void)
{
    meter_link_debounce_t d;
    memset(&d, 0, sizeof d);
    const uint32_t H = METER_LINK_USB_HOLD_MS;

    /* Starts at "no host", and stays there while there is none. */
    CHECK(!meter_link_debounce(&d, false, 0, H));
    CHECK(!meter_link_debounce(&d, false, 5000, H));

    /* A host appears: counted only once it has held for the hold time. */
    CHECK(!meter_link_debounce(&d, true, 6000, H));
    CHECK(!meter_link_debounce(&d, true, 6000 + H - 1, H));
    CHECK( meter_link_debounce(&d, true, 6000 + H, H));

    /* Re-enumeration: configured drops for 400 ms and comes back. No edge. */
    CHECK( meter_link_debounce(&d, false, 10000, H));
    CHECK( meter_link_debounce(&d, false, 10400, H));
    CHECK( meter_link_debounce(&d, true,  10401, H));
    CHECK( meter_link_debounce(&d, true,  20000, H));

    /* Several short drops in a row: still no edge (each restarts the hold). */
    for (uint32_t t = 21000; t < 26000; t += 600) {
        CHECK(meter_link_debounce(&d, false, t, H));
        CHECK(meter_link_debounce(&d, true,  t + 300, H));
    }

    /* The cable is pulled: gone after the hold time, not before. */
    CHECK( meter_link_debounce(&d, false, 30000, H));
    CHECK( meter_link_debounce(&d, false, 30000 + H - 1, H));
    CHECK(!meter_link_debounce(&d, false, 30000 + H, H));

    /* A blip of "configured" shorter than the hold never turns it on. */
    CHECK(!meter_link_debounce(&d, true,  40000, H));
    CHECK(!meter_link_debounce(&d, false, 40500, H));
    CHECK(!meter_link_debounce(&d, false, 45000, H));

    /* The millisecond counter wraps: the hold is still measured correctly. */
    memset(&d, 0, sizeof d);
    CHECK(!meter_link_debounce(&d, true, 0xFFFFFF00u, H));
    CHECK(!meter_link_debounce(&d, true, 0xFFFFFF00u + H - 1u, H));    /* wrapped */
    CHECK( meter_link_debounce(&d, true, 0xFFFFFF00u + H, H));

    /* First call with a host already there (a warm restart): still debounced. */
    memset(&d, 0, sizeof d);
    CHECK(!meter_link_debounce(&d, true, 123, H));
    CHECK( meter_link_debounce(&d, true, 123 + H, H));
}

int main(void)
{
    test_truth_table();
    test_debounce();
    printf("test_meter_link_policy: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
