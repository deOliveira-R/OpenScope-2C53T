/*
 * The FPGA design built into the image: OpenScope's own capture core
 * (fpga/core). The bytes live in the generated header fpga_bitstream_core.h
 * (scripts/fs2header.py) and are defined here, once. The firmware uploads
 * them at boot when the flash slot holds no design marked for boot.
 */
#define FPGA_BITSTREAM_CORE_H_DEFINE
#include "fpga_bitstream_core.h"
