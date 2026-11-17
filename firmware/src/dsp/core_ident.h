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

#define CORE_IDENT_REPLY_LEN 9u      /* bytes 0..8 of the reply */

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
    const uint8_t check = (uint8_t)(reply[2] ^ reply[3] ^ reply[4] ^ reply[5] ^ reply[6] ^ reply[7]);
    if (reply[0] != FPGA_CORE_REPLY_FIRST || reply[1] != 0x00u) return id;
    if (reply[2] != FPGA_CORE_ID_MAGIC0 || reply[3] != FPGA_CORE_ID_MAGIC1) return id;
    if (reply[8] != check) return id;
    id.proto      = reply[4];
    id.tag        = reply[5];
    id.caps       = reply[6];
    id.depth_log2 = reply[7];
    /* a protocol this firmware was not written for: report it, do not trust it */
    id.valid      = (id.proto == FPGA_CORE_PROTO_VER);
    return id;
}

static inline bool core_ident_has(const core_ident_t *id, uint8_t cap)
{
    return id->valid && (id->caps & cap) == cap;
}

#endif
