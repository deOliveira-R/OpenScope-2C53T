/* Status block of OpenScope's capture core (op E0, fpga/core/rtl/cmd.sv).
 *
 * What the core itself says about the registers the firmware wrote, its PLL
 * and its capture: the firmware keeps a copy of every register it writes, and
 * this is the only way to know the copy and the register agree.
 *
 * Reply: byte 0 = 80, bytes 1-2 = 00, then the block at the positions the
 * core's package gives (FPGA_CORE_DBG_I_*). The op belongs to our core only:
 * the caller sends it only to a core that has identified itself
 * (core_ident.h). Pure, host tested (tests/test_core_status.c). */
#ifndef CORE_STATUS_H
#define CORE_STATUS_H

#include <stdbool.h>
#include <stdint.h>
#include "fpga_core_regs.h"

#define CORE_STATUS_REPLY_LEN 16u    /* bytes 0..15 of the reply */

typedef struct {
    bool    valid;        /* first byte, head and tail markers as expected */
    uint8_t tag;          /* build tag */
    bool    locked;       /* PLL locked */
    uint8_t cap_state;    /* FPGA_CORE_ST_* */
    uint8_t tb;           /* reg 01 */
    uint8_t src;          /* reg 06 */
    uint8_t lvl;          /* reg 08 */
    uint8_t edge;         /* reg 07 as written: hysteresis << 4 | edge mode */
    uint8_t nlost;        /* times the PLL lost lock since configuration (stops at 255) */
} core_status_t;

static inline core_status_t core_status_parse(const uint8_t *r)
{
    core_status_t s = { false, 0u, false, 0u, 0u, 0u, 0u, 0u, 0u };
    if (r[0] != FPGA_CORE_REPLY_FIRST || r[1] != 0x00u || r[2] != 0x00u) return s;
    if (r[FPGA_CORE_DBG_I_HEAD] != FPGA_CORE_DBG_HEAD || r[FPGA_CORE_DBG_I_TAIL] != FPGA_CORE_DBG_TAIL) return s;
    s.tag       = r[FPGA_CORE_DBG_I_TAG];
    s.locked    = (r[FPGA_CORE_DBG_I_STATE] & FPGA_CORE_DBG_STATE_LOCK) != 0u;
    s.cap_state = (uint8_t)(r[FPGA_CORE_DBG_I_STATE] & FPGA_CORE_DBG_STATE_CAP_MASK);
    s.tb        = r[FPGA_CORE_DBG_I_TB];
    s.src       = r[FPGA_CORE_DBG_I_SRC];
    s.lvl       = r[FPGA_CORE_DBG_I_LVL];
    s.edge      = r[FPGA_CORE_DBG_I_EDGE];
    s.nlost     = r[FPGA_CORE_DBG_I_NLOST];
    s.valid     = true;
    return s;
}

/* The registers as the firmware believes them. */
typedef struct { uint8_t tb, src, lvl, edge; } core_regs_copy_t;

/* Which registers differ: bit 0 timebase, 1 source, 2 level, 3 edge. The core
 * keeps only the bits it uses of the source (bit 0) and of the edge register
 * (bits 7:4 and 1:0) -- compare those. */
#define CORE_REG_DIFF_TB    0x01u
#define CORE_REG_DIFF_SRC   0x02u
#define CORE_REG_DIFF_LVL   0x04u
#define CORE_REG_DIFF_EDGE  0x08u
static inline uint8_t core_status_reg_diff(const core_status_t *s, const core_regs_copy_t *c)
{
    uint8_t d = 0u;
    if (s->tb != c->tb) d |= CORE_REG_DIFF_TB;
    if ((s->src & 0x01u) != (c->src & 0x01u)) d |= CORE_REG_DIFF_SRC;
    if (s->lvl != c->lvl) d |= CORE_REG_DIFF_LVL;
    if ((s->edge & 0xF3u) != (c->edge & 0xF3u)) d |= CORE_REG_DIFF_EDGE;
    return d;
}

/* What to rewrite: only what differed in two successive readings (one
 * reading may be a disturbed transfer), and only with a valid reply both
 * times. prev_diff = the previous reading's result (0 when it was invalid). */
static inline uint8_t core_status_to_fix(uint8_t prev_diff, uint8_t now_diff)
{
    return (uint8_t)(prev_diff & now_diff);
}

#endif
