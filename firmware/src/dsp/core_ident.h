/* Identity of OpenScope's capture core (op E2, fpga/core/rtl/cmd.sv).
 *
 * The reply to E2: byte 0 = 80, byte 1 = 00, then 'O', 'S', protocol version,
 * build tag, capability bits, log2 of the record length, and the XOR of
 * those six. A core older than D5 does not know the op and answers zeros.
 *
 * The op belongs to our core only (E0..EF): the caller sends it only to a
 * design it loaded itself and knows to be ours (trig_edge_reg.h). Pure, host
 * tested (tests/test_core_ident.c). */
#ifndef CORE_IDENT_H
#define CORE_IDENT_H

#include <stdbool.h>
#include <stdint.h>
#include "fpga_core_regs.h"

#define CORE_IDENT_REPLY_LEN (FPGA_CORE_ID_I_CHECK + 1u)   /* bytes 0..8 of the reply */

typedef struct {
    bool    valid;        /* magic, version and check byte all as expected */
    uint8_t proto;        /* protocol version */
    uint8_t tag;          /* build tag (D5, ...) */
    uint8_t caps;         /* FPGA_CORE_CAP_* */
    uint8_t depth_log2;   /* record length = 1 << depth_log2 */
} core_ident_t;

/* reply = the 9 bytes read back while sending E2 and eight fill bytes. */
static inline core_ident_t core_ident_parse(const uint8_t *reply)
{
    core_ident_t id = { false, 0u, 0u, 0u, 0u };
    uint8_t check = 0u;
    for (unsigned i = FPGA_CORE_ID_I_MAGIC0; i <= FPGA_CORE_ID_I_DEPTH; i++) check ^= reply[i];
    if (reply[0] != FPGA_CORE_REPLY_FIRST || reply[1] != 0x00u) return id;
    if (reply[FPGA_CORE_ID_I_MAGIC0] != FPGA_CORE_ID_MAGIC0 || reply[FPGA_CORE_ID_I_MAGIC1] != FPGA_CORE_ID_MAGIC1) return id;
    if (reply[FPGA_CORE_ID_I_CHECK] != check) return id;
    id.proto      = reply[FPGA_CORE_ID_I_PROTO];
    id.tag        = reply[FPGA_CORE_ID_I_TAG];
    id.caps       = reply[FPGA_CORE_ID_I_CAPS];
    id.depth_log2 = reply[FPGA_CORE_ID_I_DEPTH];
    /* a protocol this firmware was not written for: report it, do not trust it */
    id.valid      = (id.proto == FPGA_CORE_PROTO_VER);
    return id;
}

static inline bool core_ident_has(const core_ident_t *id, uint8_t cap)
{
    return id->valid && (id->caps & cap) == cap;
}

/* The capability bits the firmware may act on: the core's own, and only when
 * the firmware loaded the design itself (design_ours, trig_edge_reg.h) and
 * the reply is a valid identity. Anything else is 0: the design is then
 * treated like the factory's (no edge register, no seam in the header). */
static inline uint8_t core_ident_trusted_caps(bool design_ours, const uint8_t *reply)
{
    if (!design_ours) return 0u;
    const core_ident_t id = core_ident_parse(reply);
    return id.valid ? id.caps : 0u;
}

#endif
