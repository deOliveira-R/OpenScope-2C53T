/* FPGA configuration bitstream (Gowin GW1N-2 family, IDCODE 0x0120681B)
 * extracted from stock firmware V1.2.0: the declaration. The bytes are in
 * fpga_cal_table.c, the only copy in the image (see there for provenance,
 * the sha256 and the alternate-payload build).
 * Uploaded via SPI3 opcodes 0x3B (start) / 0x3A (end).
 */
#ifndef FPGA_CAL_TABLE_H
#define FPGA_CAL_TABLE_H

#include <stdint.h>

#define FPGA_H2_CAL_TABLE_SIZE  115638u

extern const uint8_t fpga_h2_cal_table[FPGA_H2_CAL_TABLE_SIZE];

#endif /* FPGA_CAL_TABLE_H */
