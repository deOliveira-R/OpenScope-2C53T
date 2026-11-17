# OpenScope capture core for the FNIRSI 2C53T

The FPGA design OpenScope loads into the scope's GW1N-UV2 (QN48). It is written from scratch, builds with the open
tools (Yosys, nextpnr-himbaechel, Apicula) and with the vendor's, and answers the SPI commands the firmware
already used, so one firmware image drives either design.

`firmware/src/drivers/fpga_bitstream_core.h` is the build of this folder (tag `D4`), built into the firmware.

| File | What it is |
|---|---|
| `top.v` | The whole design, one module. Its header comment is the specification of the SPI protocol. |
| `tb.v` | Testbench: the firmware's frames at 60 MHz against a model of the two converters. |
| `pins.cst` | Pins and pad settings. |
| `clocks.sdc` | The three clocks: 125 MHz core, 62.5 MHz SPI, 25 MHz reference. |
| `pllo_bb.v` | Port and parameter declaration of the GW1N-2 `PLLO` cell, for a Yosys that does not know it yet. |
| `build.sh` | Build and simulation recipe. |

## What the core does

- Clocks the two 8-bit converters at 125 MHz in antiphase (250 MS/s when the firmware merges them), or both at a
  real 50 MHz for the 50 MS/s timebase, and divides down to 500 S/s for the slow ones.
- Keeps writing both converters into one 1024-word ring. A trigger (level crossing on the chosen converter:
  either direction, rising only or falling only, with hysteresis) stops it 512 samples later, so the crossing
  sits in the middle of the record.
- Hands the record over on ops 04 and 05 with a header that says where the oldest sample is (the seam), so the
  firmware does not have to search for it. Re-arms by itself after the pair has been read.
- Pin 38 (PC0 on the MCU) pulses low when a held record starts going out.

The register map and frame formats are in the header of `top.v`.

## Build

    sh build.sh          # -> build/top.fs
    sh build.sh sim      # testbench; needs Icarus Verilog and GOWIN_SIMLIB=<vendor>/simlib/gw1n/prim_sim.v

`YOSYS`, `NEXTPNR` and `GOWIN_PACK` choose the tools (default: from `PATH`).

GW1N-2 support for the PLL, the clock network and this package is not in the released tools yet. The build
that is in the firmware was made with:

| Tool | Version |
|---|---|
| Yosys | 0.71 (stock release; `pllo_bb.v` supplies the PLL declaration) |
| nextpnr | `deOliveira-R/nextpnr`, branch `gw1n2-pllo`, commit `3b8ebf91` |
| Apicula | `deOliveira-R/apicula`, branch `gw1n2-clocks-pllo`, commit `7b483c4` |

With those, `build/top.fs` has sha256 `16c65965cea354b7261daf0ea655b47e395f9fbe42dd27377bfe62aa0a41428b`
(clk 151 MHz, SCLK 70 MHz after routing). A different nextpnr or Yosys version places the design differently:
the bitstream then differs but must behave the same.

To put a build into the firmware:

    python3 scripts/fs2header.py --symbol fpga_bitstream_core --size-macro FPGA_BITSTREAM_CORE_SIZE \
        --name capcore_d4 --guard FPGA_BITSTREAM_CORE_H fpga/core/build/top.fs \
        firmware/src/drivers/fpga_bitstream_core.h

or load it into the unit's bitstream slot without reflashing: `scripts/fpga_put.py build/top.fs --name NAME --load`.

## Rules for any design on this board

- **`PCI_CLAMP=OFF` on every input.** The MCU reloads the FPGA by switching its supply off (PC6). With the clamp
  diodes on, the converters' outputs and the SPI lines keep the FPGA powered and it never restarts.
- MISO changes on the rising SCLK edge (SPI mode 3) and the reply's first byte is `80`.
- The debug ops `E0` and `E1` exist only in this core. `E0` returns `B5`, the tag, state and registers, `5B`:
  the bench scripts use it to check which build is running. Do not send them to the factory design.

## Verification

- Simulation: `sh build.sh sim` ends with `ALL CHECKS PASSED` (records, seam header against the data, trigger
  direction and hysteresis, 50 MS/s, and a 700-pair sweep of the hand-over race).
- On the unit: 3-hour runs with reloads, cold boot onto the built-in copy, trigger position and direction on both
  channels, all timebases, 250 MS/s image at about -50 dBc.

## Changing the core

Keep `TAG` in step: a new behaviour gets a new tag so the firmware and the bench scripts can tell builds apart.
