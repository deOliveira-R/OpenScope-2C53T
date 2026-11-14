/*
 * The FPGA configuration payload (src/drivers/fpga_cal_table.c) is the one
 * copy in the image and holds the stock bytes: its size and CRC32 against
 * the values read out of the image that had two copies (both identical,
 * sha256 5a0e7338...5c3b as the source header states), 2026-11-14.
 *
 * Build/run:  make -C firmware test-fpga-cal-table
 */
#include <stdio.h>
#include <stdint.h>
#include "fpga_cal_table.h"

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
    const uint32_t crc = crc32(fpga_h2_cal_table, FPGA_H2_CAL_TABLE_SIZE);
    if (FPGA_H2_CAL_TABLE_SIZE != 115638u || sizeof fpga_h2_cal_table != 115638u) {
        printf("FAIL size %u\n", (unsigned)sizeof fpga_h2_cal_table); fails++;
    }
    if (crc != 0x3B789B6Du) { printf("FAIL crc32 %08X, expected 3B789B6D\n", (unsigned)crc); fails++; }
    /* the Gowin sync word and IDCODE where the stock payload has them */
    if (fpga_h2_cal_table[0x16] != 0xA5 || fpga_h2_cal_table[0x17] != 0xC3 ||
        fpga_h2_cal_table[0x1C] != 0x01 || fpga_h2_cal_table[0x1D] != 0x20 ||
        fpga_h2_cal_table[0x1E] != 0x68 || fpga_h2_cal_table[0x1F] != 0x1B) {
        printf("FAIL sync word / IDCODE\n"); fails++;
    }
    printf("test_fpga_cal_table: %u bytes, crc32 %08X, %d failures\n",
           (unsigned)FPGA_H2_CAL_TABLE_SIZE, (unsigned)crc, fails);
    return fails ? 1 : 0;
}
