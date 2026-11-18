/*
 * The FPGA design built into the image (src/drivers/fpga_bitstream_core.h,
 * generated from fpga/core's .fs): is it a complete Gowin configuration
 * stream for THIS chip? A stream for another device is refused by the FPGA
 * (IDCODE check) and the unit would boot with no capture core, so the test
 * fails the build first. The layout is Gowin's for the GW1N family:
 * 0xFF padding, sync word, IDCODE check, "write SRAM" with the frame count,
 * the frames, a fixed trailer.
 *
 * Build/run:  make -C firmware test-fpga-bitstream
 */
#include <stdio.h>
#include <stdint.h>
#include "fpga_bitstream_core.h"

#define GW_SYNC_OFF      0x16u   /* A5 C3 after 22 bytes of 0xFF */
#define GW_IDCODE_OFF    0x18u   /* 06 00 00 00 + the IDCODE */
#define GW_WRITE_OFF     0x40u   /* 3B 80 + frame count (big endian) */
#define GW_FRAMES_OFF    0x44u
#define GW_FRAME_BYTES   160u    /* GW1N-2: 1,216 bits of data + CRC16 + 6 x 0xFF */
#define GW_FRAME_PAD     6u
#define GW_TRAILER_BYTES 50u
static const uint8_t GW1N2_IDCODE[4] = { 0x01, 0x20, 0x68, 0x1B };

int main(void)
{
    const uint8_t *b = fpga_bitstream_core;
    unsigned bad = 0;
    int fails = 0;

    if (sizeof fpga_bitstream_core != FPGA_BITSTREAM_CORE_SIZE) { printf("FAIL size macro\n"); fails++; }
    for (unsigned i = 0; i < GW_SYNC_OFF; i++) bad += b[i] != 0xFF;
    bad += !(b[GW_SYNC_OFF] == 0xA5 && b[GW_SYNC_OFF + 1u] == 0xC3);
    if (bad) { printf("FAIL padding / sync word\n"); fails++; }

    bad = b[GW_IDCODE_OFF] != 0x06;
    for (unsigned i = 0; i < 4u; i++) bad += b[GW_IDCODE_OFF + 4u + i] != GW1N2_IDCODE[i];
    if (bad) { printf("FAIL IDCODE: not a GW1N-2 stream\n"); fails++; }

    bad = !(b[GW_WRITE_OFF] == 0x3B && b[GW_WRITE_OFF + 1u] == 0x80);
    const unsigned frames = ((unsigned)b[GW_WRITE_OFF + 2u] << 8) | b[GW_WRITE_OFF + 3u];
    const unsigned end = GW_FRAMES_OFF + frames * GW_FRAME_BYTES;
    if (bad || end + GW_TRAILER_BYTES != FPGA_BITSTREAM_CORE_SIZE) {
        printf("FAIL write-SRAM header: %u frames do not fill %u bytes\n", frames,
               (unsigned)FPGA_BITSTREAM_CORE_SIZE);
        fails++;
    } else {
        bad = 0;
        for (unsigned k = 0; k < frames; k++)
            for (unsigned j = GW_FRAME_BYTES - GW_FRAME_PAD; j < GW_FRAME_BYTES; j++)
                bad += b[GW_FRAMES_OFF + k * GW_FRAME_BYTES + j] != 0xFF;
        /* trailer: ... 08 00 00 00 (write disable) then 0xFF to the end */
        bad += !(b[end + 36u] == 0x08 && b[end + 37u] == 0 && b[end + 38u] == 0 && b[end + 39u] == 0);
        for (unsigned i = end + 40u; i < FPGA_BITSTREAM_CORE_SIZE; i++) bad += b[i] != 0xFF;
        if (bad) { printf("FAIL frame padding / trailer: %u mismatches\n", bad); fails++; }
    }
    printf("test_fpga_bitstream: %s, %u bytes, %u frames, %d failures\n", FPGA_BITSTREAM_NAME,
           (unsigned)FPGA_BITSTREAM_CORE_SIZE, frames, fails);
    return fails ? 1 : 0;
}
