/* dsp/core_status.h: the status block of our capture core (op E0). */
#include <stdio.h>
#include <string.h>
#include "core_status.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void)
{
    /* as read on unit #2 (bench log, shell dump from byte 3): B5 D5 E3 0C 00 AB 01 0E 3E 5B 00 82 00 */
    const uint8_t ok[CORE_STATUS_REPLY_LEN] = { 0x80, 0x00, 0x00, 0xB5, 0xD5, 0xE3, 0x0C, 0x00, 0xAB, 0x01, 0x0E, 0x3E, 0x5B, 0x00, 0x82, 0x00 };
    core_status_t s = core_status_parse(ok);
    CHECK(s.valid && s.tag == 0xD5 && s.locked && s.cap_state == FPGA_CORE_ST_HELD);
    CHECK(s.tb == 0x0C && s.src == 0x00 && s.lvl == 0xAB && s.edge == 0x82 && s.nlost == 0);

    /* the positions are the package's */
    CHECK(FPGA_CORE_DBG_I_HEAD == 3 && FPGA_CORE_DBG_I_TAIL == 12 && FPGA_CORE_DBG_I_NLOST == 15);

    uint8_t b[CORE_STATUS_REPLY_LEN];
    memcpy(b, ok, sizeof b); b[FPGA_CORE_DBG_I_STATE] = 0x61;            /* not locked, armed */
    s = core_status_parse(b);
    CHECK(s.valid && !s.locked && s.cap_state == FPGA_CORE_ST_ARMED);
    memcpy(b, ok, sizeof b); b[FPGA_CORE_DBG_I_NLOST] = 3;
    CHECK(core_status_parse(b).nlost == 3);

    /* a dead line, a design that does not know the op, damaged markers */
    memset(b, 0xFF, sizeof b); CHECK(!core_status_parse(b).valid);
    memset(b, 0x00, sizeof b); CHECK(!core_status_parse(b).valid);
    memset(b, 0x00, sizeof b); b[0] = 0x80; CHECK(!core_status_parse(b).valid);
    for (unsigned i = 0; i < 3; i++) { memcpy(b, ok, sizeof b); b[i] ^= 0x01; CHECK(!core_status_parse(b).valid); }
    memcpy(b, ok, sizeof b); b[FPGA_CORE_DBG_I_HEAD] ^= 0x10; CHECK(!core_status_parse(b).valid);
    memcpy(b, ok, sizeof b); b[FPGA_CORE_DBG_I_TAIL] ^= 0x10; CHECK(!core_status_parse(b).valid);

    /* registers against the firmware's copies */
    s = core_status_parse(ok);
    core_regs_copy_t c = { 0x0C, 0x00, 0xAB, 0x82 };
    CHECK(core_status_reg_diff(&s, &c) == 0);
    c.tb = 0x0B;  CHECK(core_status_reg_diff(&s, &c) == CORE_REG_DIFF_TB);   c.tb = 0x0C;
    c.src = 0x01; CHECK(core_status_reg_diff(&s, &c) == CORE_REG_DIFF_SRC);  c.src = 0x00;
    c.lvl = 0xAC; CHECK(core_status_reg_diff(&s, &c) == CORE_REG_DIFF_LVL);  c.lvl = 0xAB;
    c.edge = 0x83; CHECK(core_status_reg_diff(&s, &c) == CORE_REG_DIFF_EDGE);
    c.edge = 0x42; CHECK(core_status_reg_diff(&s, &c) == CORE_REG_DIFF_EDGE);
    c.edge = 0x8E; CHECK(core_status_reg_diff(&s, &c) == 0);                 /* bits 3:2 are not kept by the core */
    c.edge = 0x00; c.lvl = 0; c.tb = 0; c.src = 1;
    CHECK(core_status_reg_diff(&s, &c) == 0x0F);

    /* only what differed twice running is rewritten */
    CHECK(core_status_to_fix(0x00, 0x04) == 0);
    CHECK(core_status_to_fix(0x04, 0x04) == 0x04);
    CHECK(core_status_to_fix(0x05, 0x06) == 0x04);
    CHECK(core_status_to_fix(0x0F, 0x00) == 0);

    if (fails) { printf("test_core_status: %d FAILED\n", fails); return 1; }
    printf("test_core_status: all passed\n");
    return 0;
}
