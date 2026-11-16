/* The FPGA configuration bitstream (Gowin GW1N-2, IDCODE 0x0120681B) the
 * firmware uploads at every boot: the declaration. The bytes and the evidence
 * for what they are: fpga_bitstream_stock.c, the only copy in the image.
 *
 * fpga_bitstream / FPGA_BITSTREAM_SIZE are the slot the upload paths read:
 * the stock design (extracted from stock V1.2.0), or with
 * -DFPGA_ALT_BITSTREAM=1 the alternate design of fpga_bitstream_alt.h, or with
 * -DFPGA_CORE_BITSTREAM=1 OpenScope's own capture core (fpga_bitstream_core.h;
 * the Makefile's FPGA_BUILTIN=core). One design per image.
 */
#ifndef FPGA_BITSTREAM_STOCK_H
#define FPGA_BITSTREAM_STOCK_H

#include <stdint.h>

#if defined(FPGA_ALT_BITSTREAM)
#include "fpga_bitstream_alt.h"   /* FPGA_BITSTREAM_NAME, _ALT_SIZE, the declaration */
#define fpga_bitstream       fpga_bitstream_alt
#define FPGA_BITSTREAM_SIZE  FPGA_BITSTREAM_ALT_SIZE
#elif defined(FPGA_CORE_BITSTREAM)
#include "fpga_bitstream_core.h"  /* FPGA_BITSTREAM_NAME, _CORE_SIZE, the declaration */
#define fpga_bitstream       fpga_bitstream_core
#define FPGA_BITSTREAM_SIZE  FPGA_BITSTREAM_CORE_SIZE
#else
#define FPGA_BITSTREAM_STOCK_SIZE  115638u
extern const uint8_t fpga_bitstream_stock[FPGA_BITSTREAM_STOCK_SIZE];
#define fpga_bitstream       fpga_bitstream_stock
#define FPGA_BITSTREAM_SIZE  FPGA_BITSTREAM_STOCK_SIZE
#endif

#endif /* FPGA_BITSTREAM_STOCK_H */
