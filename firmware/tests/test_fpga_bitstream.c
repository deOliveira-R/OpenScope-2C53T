/*
 * The stock FPGA configuration bitstream (src/drivers/fpga_bitstream_stock.c)
 * is the one copy in the image and holds the stock bytes: its size and CRC32
 * against the values read out of the image that had two copies (both
 * identical, sha256 5a0e7338...5c3b as the source states), 2026-11-14, and
 * the Gowin stream structure that says what it is: preamble, sync word,
 * IDCODE check, the SRAM write of 722 frames of 160 bytes, the trailer.
 *
 * Build/run:  make -C firmware test-fpga-bitstream
 */
#include <stdio.h>
#include <stdint.h>
#include "fpga_bitstream_stock.h"

static uint32_t crc32(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

int main(void)
{
    int fails = 0;
    const uint32_t crc = crc32(fpga_bitstream_stock, FPGA_BITSTREAM_STOCK_SIZE);
    if (FPGA_BITSTREAM_STOCK_SIZE != 115638u || sizeof fpga_bitstream_stock != 115638u) {
        printf("FAIL size %u\n", (unsigned)sizeof fpga_bitstream_stock); fails++;
    }
    if (crc != 0x3B789B6Du) { printf("FAIL crc32 %08X, expected 3B789B6D\n", (unsigned)crc); fails++; }
    /* the Gowin sync word and IDCODE where the stock payload has them */
    if (fpga_bitstream_stock[0x16] != 0xA5 || fpga_bitstream_stock[0x17] != 0xC3 ||
        fpga_bitstream_stock[0x1C] != 0x01 || fpga_bitstream_stock[0x1D] != 0x20 ||
        fpga_bitstream_stock[0x1E] != 0x68 || fpga_bitstream_stock[0x1F] != 0x1B) {
        printf("FAIL sync word / IDCODE\n"); fails++;
    }
    /* the stream's structure: 0xFF x 22, A5 C3, 0x06 + IDCODE, 0x3B 80 02 D2
     * (write SRAM, 722 frames) at 0x40, frames of 160 bytes from 0x44 each
     * ending FF x 6, then the trailer ending 08 00 00 00 + FF x 10 */
    {
        const uint8_t *b = fpga_bitstream_stock;
        unsigned bad = 0;
        for (unsigned i = 0; i < 22u; i++) bad += b[i] != 0xFF;
        bad += b[0x18] != 0x06;
        bad += !(b[0x40] == 0x3B && b[0x41] == 0x80 && b[0x42] == 0x02 && b[0x43] == 0xD2);
        const unsigned frames = ((unsigned)b[0x42] << 8) | b[0x43], start = 0x44u, fs = 160u;
        for (unsigned k = 0; k < frames; k++)
            for (unsigned j = fs - 6u; j < fs; j++) bad += b[start + k * fs + j] != 0xFF;
        const unsigned end = start + frames * fs;     /* 115,588 */
        const unsigned tail = FPGA_BITSTREAM_STOCK_SIZE - end;
        bad += tail != 50u;
        bad += !(b[end + 20] == 0x0A);                 /* user code (0) */
        bad += !(b[end + 36] == 0x08 && b[end + 37] == 0 && b[end + 38] == 0 && b[end + 39] == 0);
        for (unsigned i = end + 40u; i < FPGA_BITSTREAM_STOCK_SIZE; i++) bad += b[i] != 0xFF;
        if (bad) { printf("FAIL Gowin stream structure: %u mismatches\n", bad); fails++; }
    }
    printf("test_fpga_bitstream: %u bytes, crc32 %08X, %d failures\n",
           (unsigned)FPGA_BITSTREAM_STOCK_SIZE, (unsigned)crc, fails);
    return fails ? 1 : 0;
}
