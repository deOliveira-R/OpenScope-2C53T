/* dsp/trig_edge_reg.h: the trigger edge register 0x07 of our capture core.
 * The rule under test: the value differs from the arm burst's 0x00 only when
 * the running design is known to be ours, so nothing else is ever written to
 * the stock design or to a design that was already running at boot. */
#include <stdio.h>
#include "trig_edge_reg.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void)
{
    /* ours = built-in is the core AND we sent the upload AND it ended with DONE */
    for (int b = 0; b < 2; b++)
        for (int s = 0; s < 2; s++)
            for (int d = 0; d < 2; d++)
                CHECK(trig_edge_reg_design_ours(b, s, d) == (b && s && d));

    /* not ours: 0x00 for either edge, i.e. the acquisition task never writes */
    CHECK(trig_edge_reg_code(false, false) == 0x00);
    CHECK(trig_edge_reg_code(false, true)  == 0x00);

    /* ours: rising only / falling only, hysteresis in the high nibble */
    CHECK(trig_edge_reg_code(true, false) == 0x82);
    CHECK(trig_edge_reg_code(true, true)  == 0x83);
    CHECK((trig_edge_reg_code(true, false) >> 4) == TRIG_EDGE_REG_HYST);
    CHECK((trig_edge_reg_code(true, false) & 0x0C) == 0);      /* bits 3:2 unused by the core */

    /* a stock-built-in image never sends it, whatever else is true */
    CHECK(trig_edge_reg_code(trig_edge_reg_design_ours(false, true, true), true) == 0x00);
    /* a design that stayed through the boot (not sent by us) is unknown */
    CHECK(trig_edge_reg_code(trig_edge_reg_design_ours(true, false, true), false) == 0x00);
    /* an upload that did not reach DONE */
    CHECK(trig_edge_reg_code(trig_edge_reg_design_ours(true, true, false), false) == 0x00);

    printf(fails ? "test_trig_edge_reg: %d FAILED\n" : "test_trig_edge_reg: all passed\n", fails);
    return fails ? 1 : 0;
}
