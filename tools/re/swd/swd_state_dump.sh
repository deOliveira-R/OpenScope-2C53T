#!/usr/bin/env bash
# SWD peripheral state dump of a parked target, for a mechanical diff of two
# firmwares at the same instant (docs/re/README.md, step 3).
#
# Park each firmware at the instant of interest with a branch-to-self:
#   factory : patch `b .` (bytes FE E7) over the instruction to stop at, in a
#             copy of the factory image, and flash that copy. On the 2C53T
#             V1.2.0 the instant studied was flash 0x0802DA42: FPGA prelude
#             (05/12) sent, CS low, config-enable (0x15) not yet clocked.
#   ours    : a `for (;;) {}` at the matching line of our own firmware.
#
# Dumps every register that could plausibly differ, so the two runs can be
# diffed:  ./swd_state_dump.sh stock   then   ./swd_state_dump.sh ours
#   diff -u /tmp/swd_stock.txt /tmp/swd_ours.txt
#
# Board data: the register list below is the AT32F403A's (GPIOA-E, AFIO, the
# clock tree, SPI3, USART2). Replace it for another MCU.
#
# The OpenOCD config is at32_minimal.cfg next to this script (override with
# SWD_CFG=path). This script was last run with a stm32f1x-based config that
# was later found to freeze the target on attach; at32_minimal.cfg is the
# replacement and has not been run with this script.
#
# NOTES (from docs/SWD_GOLDEN_REFERENCE_2026_06_09.md):
#  - Unplug the ST-Link before any IAP flash (it browns out the USB-powered
#    bootloader and causes bad writes). Replug only once the image is running.
#  - Flash reads return 0xFF and OpenOCD may report a bogus "HardFault" — that
#    is a read-protection artifact. Peripheral (0x4000xxxx) reads are live and
#    are all this script uses.
#  - Wire SWDIO / SWCLK / GND only. Do NOT wire 3V3.
set -u

LABEL="${1:-dump}"
OUT="/tmp/swd_${LABEL}.txt"
CFG="${SWD_CFG:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/at32_minimal.cfg}"

[ -f "$CFG" ] || { echo "missing $CFG"; exit 1; }

# base+offset groups: GPIO CRL/CRH/IDR/ODR = 4 words from base
read -r -d '' CMDS <<'EOF'
echo "=== GPIOA (CRL CRH IDR ODR) ==="
mdw 0x40010800 4
echo "=== GPIOB (CRL CRH IDR ODR) ==="
mdw 0x40010C00 4
echo "=== GPIOC (CRL CRH IDR ODR) ==="
mdw 0x40011000 4
echo "=== GPIOD (CRL CRH IDR ODR) ==="
mdw 0x40011400 4
echo "=== GPIOE (CRL CRH IDR ODR) ==="
mdw 0x40011800 4
echo "=== AFIO (EVCR MAPR EXTICR1-4 .. MAPR2) ==="
mdw 0x40010000 8
echo "=== SPI3 (CTRL1 CTRL2 STS) ==="
mdw 0x40003C00 3
echo "=== CRM (0x40021000 .. +0x2C) ==="
mdw 0x40021000 12
echo "=== USART2 (STS CTRL1@+0x0C) ==="
mdw 0x40004400 5
EOF

ARGS=(-f "$CFG" -c "init" -c "halt")
while IFS= read -r line; do
  [ -z "$line" ] && continue
  ARGS+=(-c "$line")
done <<< "$CMDS"
ARGS+=(-c "resume" -c "shutdown")

echo "# Experiment E SWD dump — label=${LABEL}  $(date -Is)" > "$OUT"
sudo timeout 60 openocd "${ARGS[@]}" 2>&1 | tee -a "$OUT"

echo
echo "wrote $OUT"
echo "when both are captured:  diff -u /tmp/swd_stock.txt /tmp/swd_ours.txt"
