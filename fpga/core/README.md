# OpenScope capture core for the FNIRSI 2C53T

The FPGA design OpenScope loads into the scope's GW1N-UV2 (QN48). Written from scratch in SystemVerilog, it builds
with the open tools (Yosys, nextpnr-himbaechel, Apicula) and with the vendor's, and answers the SPI commands the
firmware already used, so one firmware image drives either design.

`firmware/src/drivers/fpga_bitstream_core.h` is the build of this folder (tag `D5`), built into the firmware.
`firmware/src/drivers/fpga_core_regs.h` is generated from `rtl/core_pkg.sv` (opcodes, bit fields, reply byte
positions, record geometry, the timebase divider table); the firmware names every protocol number through it and
`make test-core-regs-header` fails when the two disagree.

## What the core does

- Clocks the two 8-bit converters at 125 MHz in antiphase (250 MS/s when the firmware merges them), or both at a
  real 50 MHz for the 50 MS/s timebase, and divides down to 500 S/s for the slow ones.
- Keeps writing both converters into one 1024-word ring. A trigger (level crossing on the chosen converter:
  either direction, rising only or falling only, with hysteresis) stops it 512 samples later, so the crossing
  sits in the middle of the record.
- Hands the record over on ops 04 and 05 with a header that says where the oldest sample is (the seam), so the
  firmware does not have to search for it. Re-arms by itself after the pair has been read.
- Pin 38 (PC0 on the MCU) pulses low when a held record starts going out.
- Captures nothing until the PLL has locked, and counts the times lock was lost.
- Says what it is: op E2 returns a magic, the protocol version, the build tag and capability bits.

The protocol is specified in the header of `rtl/cmd.sv`; its numbers are in `rtl/core_pkg.sv`.

## Structure

    clkgen ── clk (125 MHz) ──┬─ adc_front ── sample pair ─┬─ trigger ── fire ─┐
                              ├─ timebase ── tick ─────────┴───────────────────┴─ capture ── we, wp ── memory
                              └─ cmd_sync ══ crossing ══ cmd ── spi_slave ── MCU          (read side: SCLK)

| File | Clock | Job |
|---|---|---|
| `rtl/core_pkg.sv` | - | Opcodes, reset values, record geometry, capture states, the register bundle, the timebase table. The protocol's numbers live here and nowhere else. |
| `rtl/clkgen.sv` | - | PLL: 25 MHz -> 125 MHz, and the reset that holds the capture until the PLL has locked. |
| `rtl/adc_front.sv` | clk | Converter clocks out, data in, the pair of samples that belongs together, the 50 MS/s phasing. |
| `rtl/timebase.sv` | clk | One tick per stored sample. |
| `rtl/trigger.sv` | clk | Level, direction, hysteresis -> "this sample is the crossing". |
| `rtl/capture.sv` | clk | The ring and its four states. |
| `rtl/spi_slave.sv` | SCLK | Bits and bytes; knows no commands. |
| `rtl/cmd.sv` | SCLK | Registers, record read-out, reply bytes. |
| `rtl/cmd_sync.sv` | clk | Every signal that crosses between the two clocks, the PC0 pulse, the re-arm. |
| `rtl/top.sv` | - | Wiring and the record memory. |
| `rtl/pllo_bb.v` | - | Port and parameter declaration of the GW1N-2 `PLLO` cell, for a Yosys that does not know it yet. |
| `pins.cst`, `clocks.sdc` | - | Pins and pad settings; the clocks (125 MHz core, 62.5 MHz SPI, 25 MHz reference). |
| `sim/cells_sim.sv` | - | Our own simulation models of the four vendor cells used (PLL, DDR in, DDR out, RAM). |
| `tb/tb.sv` | - | Directed testbench: the firmware's frames at 60 MHz against a model of the two converters. |
| `tb/lockstep.sv` | - | This core and core D4 (`ref/capcore_d4.v`) on the same pins under random traffic, every output compared at every change. |
| `tb/cells_tb.sv` | - | Our cell models against the vendor's simulation library. |
| `tools/gen_c_header.py` | - | Makes the firmware's `fpga_core_regs.h` from the package. |
| `ref/capcore_d4.v` | - | Core D4, the one-file Verilog this core was restructured from. Kept as the reference for the lockstep. |

Names follow the firmware: converter A = op 04 = pins 39..46 (clock pin 47), converter B = op 05 = pins 18..27
(clock pin 17).

## Build and test

    sh build.sh             # -> build/top.fs
    sh build.sh sim         # directed testbench (Icarus Verilog only)
    sh build.sh lockstep    # against core D4; SEEDS="1 2 3" FRAMES=400
    GOWIN_SIMLIB=<vendor>/simlib/gw1n/prim_sim.v sh build.sh cells     # our cell models against the vendor's
    GOWIN_SIMLIB=... sh build.sh sim                                   # the testbench on the vendor's models
    python3 tools/gen_c_header.py [--check] ../../firmware/src/drivers/fpga_core_regs.h

`YOSYS`, `NEXTPNR`, `GOWIN_PACK`, `IVERILOG` choose the tools (default: from `PATH`); `SEED` the placement seed.

GW1N-2 support for the PLL, the clock network and this package is not in the released tools yet. The build
that is in the firmware was made with:

| Tool | Version |
|---|---|
| Yosys | 0.71 (stock release; `rtl/pllo_bb.v` supplies the PLL declaration) |
| nextpnr | `deOliveira-R/nextpnr`, branch `gw1n2-pllo`, commit `3b8ebf91` |
| Apicula | `deOliveira-R/apicula`, branch `gw1n2-clocks-pllo`, commit `7b483c4` |

With those and seed 7, `build/top.fs` has sha256 `7d039e88c8918dcc3252f265386ad87d020cf5fe505dc488ff9a52844ac72fbd`
(clk 168 MHz, SCLK 78.9 MHz after routing; the vendor tool gives 138.9 and 93.0). nextpnr's result depends
strongly on the seed (clk 128..168 MHz over eight seeds), which is why it is pinned; a build that misses
125 / 62.5 MHz fails. `build.sh` also refuses a clock constraint that matches no net (nextpnr only warns).

To put a build into the firmware:

    python3 scripts/fs2header.py --symbol fpga_bitstream_core --size-macro FPGA_BITSTREAM_CORE_SIZE \
        --name capcore_d5 --guard FPGA_BITSTREAM_CORE_H fpga/core/build/top.fs \
        firmware/src/drivers/fpga_bitstream_core.h

or load it into the unit's bitstream slot without reflashing: `scripts/fpga_put.py build/top.fs --name NAME --load`.

## SystemVerilog subset

What Yosys 0.71 (`read_verilog -sv`, no plugin), Icarus Verilog 13 and Gowin 1.9.12 all take:
- yes: `package` with `localparam`, `typedef enum`, `typedef struct packed`, functions; `logic`; `always_ff`,
  `always_comb`; typed parameters; scoped names (`core_pkg::X`);
- no: `import` (Yosys rejects it), `return` in functions (Yosys), `unique`/`priority` (Yosys acts on them,
  Icarus ignores them: simulation and hardware could differ), interfaces (Yosys);
- every file starts with `` `default_nettype none `` and ends with `` `default_nettype wire `` (the vendor's
  simulation library needs the default).

## Rules for any design on this board

- **`PCI_CLAMP=OFF` on every input.** The MCU reloads the FPGA by switching its supply off (PC6). With the clamp
  diodes on, the converters' outputs and the SPI lines keep the FPGA powered and it never restarts.
- MISO changes on the rising SCLK edge (SPI mode 3) and the reply's first byte is `80`.
- Bit 7 of byte 1 of every reply is 0: that bit leaves before the opcode has been taken in.
- Ops `E0`..`EF` exist only in this core (`E0` status, `E1` bench tuning, `E2` identity). Do not send them to the
  factory design.
- A change of behaviour gets a new `TAG`, a change a firmware could misread a new `PROTO_VER`.

## Verification

- **Against D4, in simulation:** `tb/lockstep_runs_d5.log`: six seeds on the source (26 million comparisons) and
  three on the Yosys netlist (10 million), no mismatch. D4 is brought to this core's state once, after the PLL has
  locked (this core starts later by design); the netlist equals the source with no alignment.
- **Directed testbench:** records, seam header against the data, trigger direction and hysteresis, 50 MS/s, a
  700-pair sweep of the hand-over race, identity, loss of lock. Passes on our cell models and on the vendor's.
- **Cell models:** DDR cells and RAM equal to the vendor's at every instant, with the same number of output
  changes; PLL equal in frequency and lock (phase not compared; CLEAR not exercised).
- **On the unit:** the D4 regression set repeated on D5 (trigger position and direction on both channels, all
  timebases, seam header against the search, roll, 250 MS/s image at about -51 dBc), a 3-hour run with reloads, and
  a cold boot onto the built-in copy.
