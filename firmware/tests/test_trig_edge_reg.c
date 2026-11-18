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
    /* loaded by us: this firmware sent the upload and the design started */
    for (int sb = 0; sb < 2; sb++) for (int dn = 0; dn < 2; dn++)
        CHECK(trig_edge_reg_loaded_by_us(sb, dn) == (sb && dn));

    /* reg 07: rising only / falling only, hysteresis in the high nibble */
    CHECK(trig_edge_reg_code(false) == 0x82);
    CHECK(trig_edge_reg_code(true)  == 0x83);
    CHECK((trig_edge_reg_code(false) >> 4) == TRIG_EDGE_REG_HYST);
    CHECK((trig_edge_reg_code(false) & 0x0C) == 0);            /* bits 3:2 unused by the core */

    /* the seam in the reply header: only with V set */
    CHECK(trig_seam_from_hdr(0x04, 0x00) == 0);
    CHECK(trig_seam_from_hdr(0x07, 0xFF) == 1023);
    CHECK(trig_seam_from_hdr(0x06, 0x71) == 0x271);
    CHECK(trig_seam_from_hdr(0x00, 0x00) == -1);                /* no held record */
    CHECK(trig_seam_from_hdr(0x03, 0xFF) == -1);                /* a pointer without V */
    CHECK(trig_seam_from_hdr(0xFF, 0xFF) == -1);                /* a dead bus */
    for (int hi = 0; hi < 256; hi++)                            /* any other bit above the pointer and the forced bit */
        if ((hi & 0xF4) != 0x04) CHECK(trig_seam_from_hdr((uint8_t)hi, 0x55) == -1);
    CHECK(trig_seam_from_hdr(0x0E, 0x71) == 0x271 && trig_hdr_forced(0x0E));   /* a forced record */
    CHECK(!trig_hdr_forced(0x06));
    CHECK(trig_seam_from_hdr(0x08, 0x00) == -1);                /* forced without V: no seam */

    printf(fails ? "test_trig_edge_reg: %d FAILED\n" : "test_trig_edge_reg: all passed\n", fails);
    return fails ? 1 : 0;
}
