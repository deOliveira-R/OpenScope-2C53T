/* Trigger edge in the FPGA: register 0x07 of OpenScope's own capture core.
 *
 * The stock design is sent 07 00 once, in the arm burst, and never anything
 * else: what other values do to it is not known, so nothing else may reach it.
 * Our core (fpga_bitstream_core.h) reads the register as
 *   bits 1:0  00 either edge, 10 rising only, 11 falling only
 *   bits 7:4  hysteresis in codes: the signal must first be that far on the
 *             other side of the level
 * and then hands over only records of the edge the UI asks for. Before, it
 * fired on both and the MCU's edge filter threw the wrong-way records away:
 * more than half of them, and the badge flickered between Trig'd and Wait.
 * The MCU's filter stays; with the core doing the edge it drops nothing.
 *
 * Pure functions, so the rule "never to a design that is not ours" has a host
 * test (tests/test_trig_edge_reg.c). */
#ifndef TRIG_EDGE_REG_H
#define TRIG_EDGE_REG_H

#include <stdbool.h>
#include <stdint.h>
#include "fpga_core_regs.h"   /* generated from the capture core's package */

#define TRIG_EDGE_REG_HYST   8u      /* codes; unit #2: 0 wrong-way records at 8, 5 % at 4 */

/* Is the design running in the FPGA known to be our core?
 *   builtin_is_core  this image was built with our core as the built-in design
 *                    (then the slot holds our designs too; a stock-built-in
 *                    image never sends the register, whatever the slot holds)
 *   sent_by_us       this boot's firmware sent the upload itself (a design
 *                    that was already running and stayed is unknown)
 *   done             the FPGA reported DONE after that upload */
static inline bool trig_edge_reg_design_ours(bool builtin_is_core, bool sent_by_us, bool done)
{
    return builtin_is_core && sent_by_us && done;
}

/* The value register 0x07 must hold. 0x00 is what the arm burst wrote: for a
 * design that is not ours the answer never differs from it, so no write. */
static inline uint8_t trig_edge_reg_code(bool design_ours, bool falling)
{
    if (!design_ours) return FPGA_CORE_EDGE_EITHER;
    return (uint8_t)((TRIG_EDGE_REG_HYST << FPGA_CORE_EDGE_HYS_SHIFT) |
                     (falling ? FPGA_CORE_EDGE_FALLING : FPGA_CORE_EDGE_RISING));
}

/* Where a held record's oldest sample is: reply bytes 1-2 of op 04 / op 05.
 * Core v3 (tag D4) on sends  byte 1 = 00000 V s9 s8,  byte 2 = s7..s0  with
 * V = 1 only when the frame reads a held record and the pointer s is that
 * record's write pointer (the trigger is then at (s + 512) mod 1024). Core v2
 * sends 00 00, and what the stock design leaves in these bytes is not a
 * pointer, so the bytes are read only when the design is known to be ours.
 * Returns 0..1023, or -1 for "no pointer here": the caller then looks for the
 * seam in the samples as before. */
static inline int trig_seam_from_hdr(bool design_ours, uint8_t b1, uint8_t b2)
{
    if (!design_ours) return -1;
    if ((b1 & FPGA_CORE_SEAM_B1_MASK) != FPGA_CORE_SEAM_VALID) return -1;         /* V set, nothing else above the pointer */
    return (int)(((unsigned)(b1 & FPGA_CORE_SEAM_HI_MASK) << 8) | (unsigned)b2);
}

#endif /* TRIG_EDGE_REG_H */
