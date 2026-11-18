/* Trigger edge in the FPGA: register 0x07 of OpenScope's capture core, and
 * the record header of ops 04/05.
 *
 * The core (fpga_bitstream_core.h) reads the register as
 *   bits 1:0  00 either edge, 10 rising only, 11 falling only
 *   bits 7:4  hysteresis in codes: the signal must first be that far on the
 *             other side of the level
 * and then hands over only records of the edge the UI asks for. Before, it
 * fired on both and the MCU's edge filter threw the wrong-way records away:
 * more than half of them, and the badge flickered between Trig'd and Wait.
 * The firmware once filtered records by their edge itself, for the factory
 * design; that filter had to guess the record's start from the samples and
 * threw good records away (unit #2: 84 of 84 of a decaying pulse). Gone.
 *
 * Pure functions, host tested (tests/test_trig_edge_reg.c). */
#ifndef TRIG_EDGE_REG_H
#define TRIG_EDGE_REG_H

#include <stdbool.h>
#include <stdint.h>
#include "fpga_core_regs.h"   /* generated from the capture core's package */

#define TRIG_EDGE_REG_HYST   8u      /* codes; unit #2: 0 wrong-way records at 8, 5 % at 4 */

/* Did this firmware put the design into the FPGA itself, and did it start?
 *   sent_by_us  this boot's firmware sent the upload (a design that was
 *               already running and stayed is unknown)
 *   done        the FPGA reported DONE after that upload
 * Only then is the design asked who it is (core_ident.h). */
static inline bool trig_edge_reg_loaded_by_us(bool sent_by_us, bool done)
{
    return sent_by_us && done;
}

/* The value register 0x07 must hold for the edge the UI asks for. */
static inline uint8_t trig_edge_reg_code(bool falling)
{
    return (uint8_t)((TRIG_EDGE_REG_HYST << FPGA_CORE_EDGE_HYS_SHIFT) |
                     (falling ? FPGA_CORE_EDGE_FALLING : FPGA_CORE_EDGE_RISING));
}

/* Where a held record's oldest sample is: reply bytes 1-2 of op 04 / op 05,
 *   byte 1 = 0000 F V s9 s8,  byte 2 = s7..s0
 * V = 1 only when the frame reads a held record; the pointer s is then that
 * record's write pointer (a trigger, if any, is at (s + 512) mod 1024).
 * F: see trig_hdr_forced. Returns 0..1023, or -1 when no record is held. */
static inline int trig_seam_from_hdr(uint8_t b1, uint8_t b2)
{
    if ((b1 & FPGA_CORE_SEAM_B1_MASK) != FPGA_CORE_SEAM_VALID) return -1;         /* V set, nothing else above the pointer */
    return (int)(((unsigned)(b1 & FPGA_CORE_SEAM_HI_MASK) << 8) | (unsigned)b2);
}

/* The record with that seam was held on request (op 09), not by a trigger:
 * it has no crossing at seam + 512. Meaningful only when trig_seam_from_hdr()
 * gave a seam and the core has CAP_FORCE. */
static inline bool trig_hdr_forced(uint8_t b1)
{
    return (b1 & FPGA_CORE_SEAM_FORCED) != 0u;
}

#endif /* TRIG_EDGE_REG_H */
