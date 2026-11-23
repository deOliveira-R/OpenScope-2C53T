# Reverse-engineering toolkit

How to open up an instrument that has an MCU and an FPGA, starting with
nothing but the unit and the vendor's firmware update file. The steps are in
the order one would take them. Each step names the tool that does it and how
to call it.

The tools live in `tools/re/`. A few steps use scripts that are also in daily
use and therefore stay in `scripts/`; those are named with their full path.

The tools were written for the FNIRSI 2C53T (MCU AT32F403A, FPGA Gowin GW1N-2,
external flash W25Q128). The method carries over to other boards. The
addresses, pin names and register tables do not: each tool below has a
"board data" line that says what to replace.

Rules that apply to every step:

- No bytes of the vendor's firmware or bitstream go into the repository. The
  tools take the image as an argument. Keep images outside the tree or in a
  path that git ignores.
- Back up before writing. Take the flash backup of step 7 before any firmware
  of your own writes to the external flash.
- A measurement needs a known answer. Read something whose value you know
  first (the FPGA's IDCODE, the flash's JEDEC ID) and stop if it is wrong.

## What each tool needs

| Tool | Python packages | Other programs |
|---|---|---|
| `firmware-image/trace_peripheral_writes.py` | capstone | |
| `firmware-image/find_gpio_pulses.py` | none | `arm-none-eabi-objdump` |
| `firmware-image/find_high_flash_refs.py` | none | `arm-none-eabi-objdump` |
| `firmware-image/dump_factory_bootloader.py` | pyserial | |
| `emulation/unicorn_init_trace.py` | unicorn | |
| `emulation/unicorn_stock_flash_trace.py` | unicorn | |
| `bus-capture/decode_spi_csv.py` | none | a logic analyser's CSV export |
| `flash/flash_capture.py` | pyserial | |
| `flash/extract_spi_flash_fat.py` | none | |
| `stock-caplog/build_caplog_image.py` | none | `arm-none-eabi-gcc`, `-ld`, `-objcopy` |
| `stock-caplog/caplog_dump.py` | pyserial (hidapi for `--hid`) | |
| `swd/*.sh` | | OpenOCD, an ST-Link |
| `board-probes/` | | `arm-none-eabi-gcc`, `make` |
| `fpga-config/` | | Arduino IDE, an ESP32 |

Every Python tool answers `--help`, except `flash_capture.py`, which prints
its usage when called with no arguments.

## 1. Get the firmware image

The vendor's update file is the image. For the 2C53T it is a raw binary that
the factory bootloader writes to `0x08007000`.

If there is no update file, or to get what the update file does not hold
(the bootloader), read the chip from the inside. Read protection blocks a
debug probe but not code that runs on the chip:

```
python3 tools/re/firmware-image/dump_factory_bootloader.py bootloader.bin
```

It reads internal flash through the `mem read` command of our debug shell,
256 bytes per call. Board data: `BASE` and `SIZE` (the bootloader's region).
It needs a firmware of our own running, so it comes after step 3 on a new
board.

`scripts/hid_mem_read.py <address> <size> --out file` does the same through
our HID bootloader, with no application running.

## 2. Read the image

Load it in Ghidra. [re_guide.md](re_guide.md) has the project settings and the
headless scripts (`reverse_engineering/ghidra_scripts/`). Start from the
strings and the vector table.

Two address conventions are in use for the 2C53T image, and the tools do not
agree on one. The image runs at `0x08007000`, so a run-time address is the
file offset plus `0x08007000`. The Ghidra project and the older analysis
files load it at `0x08000000`, so their addresses are `0x7000` lower
([freertos_tasks.md](freertos_tasks.md)). `trace_peripheral_writes.py` takes
file offsets and defaults to the Ghidra base; `find_gpio_pulses.py` defaults
to the run-time base. Check which one a number is in before using it.

Then ask the image three direct questions.

Which peripheral registers does one function write?

```
python3 tools/re/firmware-image/trace_peripheral_writes.py image.bin <file offset> <length> [--base 0x08000000]
```

It disassembles the region and lists every store to `0x40000000`-`0x5FFFFFFF`
with the register name. Run it on the init function: the list tells which
SPI, USART, timers and pins the board uses. It resolves constants only; a
value computed at run time shows as unknown (step 6 resolves those). Board
data: the register name table.

Which GPIO pins does the firmware drive before a given instruction?

```
python3 tools/re/firmware-image/find_gpio_pulses.py image.bin --link-base 0x08007000 --hi <address>
```

It resolves the base register of every store by backward dataflow, so a
timer is not mistaken for a GPIO port. It was written to look for a
reconfiguration pulse to the FPGA before the configuration starts. Board
data: the GPIO base addresses and the default of `--hi`.

Does the firmware refer to flash beyond the end of the update file?

```
python3 tools/re/firmware-image/find_high_flash_refs.py image.bin --base 0x08007000
```

It pairs `movw`/`movt` and reports absolute flash addresses that the file
does not cover. Such addresses hold data that the factory programmed and the
update never replaces: fonts, tables, calibration. Board data: `FLASH_START`
and `FLASH_END`.

## 3. Get onto the board

Attach a debug probe without disturbing the target:

```
openocd -f tools/re/swd/at32_minimal.cfg -c init -c targets -c shutdown
```

The file defines the target and nothing else. A generic STM32 config froze
this MCU on attach; the header of the file has the details. Wire SWDIO, SWCLK
and GND only.

Then run small programs of your own. `tools/re/board-probes/` holds three,
each a single C file with no RTOS:

```
make -C tools/re/board-probes                   # hwtest: power latch, backlight, LCD
make -C tools/re/board-probes PROBE=btntest     # every GPIO input register on the LCD
make -C tools/re/board-probes PROBE=uartsniff   # every USART as a receiver, bytes on the LCD
```

`hwtest` is the first thing to get running: a battery instrument turns
itself off unless the firmware holds a power latch. `btntest` finds buttons:
press one and see which bit moves. `uartsniff` finds a serial link from
another chip. Board data: the power latch pin (PC9), the backlight pin (PB8),
the LCD bus (EXMC, addresses in each file).

The Makefile links at `0x08000000` and has no flash rule. Writing there
replaces the bootloader. Decide where the probe goes on your unit, link for
that address (`LDSCRIPT=`), and flash it by the route you would use for any
image, with its checks. `LDSCRIPT=../../../firmware/ld/at32f403a_app.ld`
links, but that is not enough to run under a bootloader: the vendor's
system init sets the vector table to `0x08000000` (`VECT_TAB_OFFSET` 0), so a
probe that uses interrupts needs that changed too. Not tried on a unit.

## 4. Find the MCU-FPGA buses and how the FPGA is configured

The trace of step 2 names the candidates: the SPI and USART that the init
function sets up and the pins it switches to alternate functions. Confirm on
the unit:

- `uartsniff` (step 3) shows whether a USART carries traffic.
- The shell of our firmware reads the FPGA's configuration port: `spi3 gowin`
  (a toolkit command: the `app-re` image, step 9) reads IDCODE, USERCODE and
  STATUS; `fpga selftest` (every bench image) checks the clock, the IDCODE
  and every driven pin. On the factory design of the 2C53T, reading
  STATUS while it captures desynchronises it until the next power cycle; our
  capture core ignores configuration-port opcodes, so there these reads (and
  `re pinsweep`, step 9) are safe while it runs.

To see the state of the factory firmware at one instant, park it there and
read its peripherals over SWD:

```
tools/re/swd/swd_state_dump.sh stock      # then the same with our firmware: ... ours
diff -u /tmp/swd_stock.txt /tmp/swd_ours.txt
```

Parking means a branch-to-self patched over one instruction of a copy of the
factory image. The header of the script says how. The diff of the two dumps
is the list of what our firmware sets up differently. Board data: the
register list.

```
tools/re/swd/swd_fpga_status.sh stock
```

drives the MCU's SPI from the host while the CPU is halted and reads the
FPGA's IDCODE and STATUS. It refuses to report anything if the IDCODE is
wrong. Board data: the SPI base address, the chip-select pin, the Gowin
opcodes, the IDCODE.

Both scripts default to `at32_minimal.cfg`. They were last run with another
config, so the first run with this one is a test of the pair.

`tools/re/fpga-config/` is an ESP32 sketch that acts as an external master on
the FPGA's configuration port, after `fpga busrelease` (toolkit, `app-re`
image) has taken the MCU off the bus. It tries a handshake in seconds where a firmware build takes
minutes. It has not been run on hardware. Board data: the opcodes and the
wiring table in its README.

Where the bitstream is: the factory firmware of the 2C53T sends it over SPI
at every boot. The emulator trace of step 6 shows the upload as a long run of
writes to the SPI data register; the address the loop reads from is the
bitstream inside the image. There is no tool for this step beyond that trace.

## 5. Capture and decode bus traffic

From outside, with a logic analyser on the bus:

```
python3 tools/re/bus-capture/decode_spi_csv.py digital.csv
python3 tools/re/bus-capture/decode_spi_csv.py digital.csv --pin-stats
```

Input is the transition CSV that Saleae Logic 2 exports. It decodes the SPI
bytes per chip-select frame and prints a time line of the control pins.
`--pin-stats` counts edges per channel, which tells quickly whether a probe
is on the right pad. Board data: the column order and the SPI mode at the top
of the file.

From inside, when the pads cannot be reached:

```
python3 tools/re/stock-caplog/build_caplog_image.py --stock <factory image>
python3 scripts/iap_flash.py flash <the image it wrote>
#   use the instrument, then MENU + Power without removing power, flash a reader firmware
python3 tools/re/stock-caplog/caplog_dump.py --port /dev/ttyACM0 --save run.caplog
python3 tools/re/stock-caplog/caplog_dump.py --decode run.caplog
```

It patches two hooks and a small logger into a copy of the factory image.
The logger records every frame the firmware sends on one USART and every
GPIO change, in RAM that survives the switch to another firmware as long as
power stays on. The reader must leave that RAM block alone. The current
OpenScope image does not (its `.bss` covers the block and startup zeroes it);
[stock_caplog.md](../stock_caplog.md) names the reader that was used and has
the procedure and the limits. Board data: the two hook addresses and the
bytes they replace (checked by SHA-256 of the image), the address of the
free flash and of the RAM block. `scripts/test_stock_caplog.py` tests the
patch encoder and the decoder.

## 6. Run the factory firmware in an emulator

```
python3 tools/re/emulation/unicorn_init_trace.py <factory image>
```

It runs the factory init function in Unicorn and logs every write to a
peripheral register with its final value, in order. This is the reference
for a bring-up of our own: the same registers, the same values, the same
order. It resolves what the static trace of step 2 cannot. Board data: the
entry and stop addresses, the calls it skips, the memory map, the register
names.

## 7. Dump the external flash

Check the link first: `flash jedec` in the debug shell must print the chip's
JEDEC ID.

```
uv run python scripts/flash_backup.py backup.bin                      # the whole chip
python3 tools/re/flash/flash_capture.py 0x200000 0x10000 region.bin   # one region
```

Both read through the shell's `flash dump` command, 4096 bytes per call.
Keep the full backup of each unit; it is the only copy of its factory data.

```
python3 tools/re/flash/extract_spi_flash_fat.py backup.bin --out extracted/ --bases 0x0 0x200000
```

It extracts the FAT12 volumes in the image. Board data: the volume offsets
and the 4 KB sector layout.

## 8. Locate calibration and settings

Three ways, from cheapest to surest.

Change one setting and compare:

```
python3 scripts/stock_settings_diff.py before.bin after.bin
```

Take a dump, change exactly one setting in the factory firmware, take
another dump. The script groups the bytes that changed. `scripts/stock_settings.py
inspect <dump>` decodes the settings block of the 2C53T once its layout is
known.

See what the factory firmware reads at boot:

```
python3 tools/re/emulation/unicorn_stock_flash_trace.py --binary <factory image> --dump backup.bin --out trace.txt
```

It runs the factory init in Unicorn with the flash backup behind the SPI
flash interface and logs every flash transaction. The addresses it reads
before the first screen are where settings and calibration live. Board data:
every address in the file.

Read the MCU's own flash from the inside: `make -C firmware guest-caldump`
builds an image that prints one sector of internal flash and its CRC and
writes nothing. Read protection does not stop it (step 1).

## 9. Probe an FPGA design from the shell

Once our firmware talks to the FPGA, the shell is the fastest probe. The
bench image has the commands that are also in daily use: `spi3 xfer`,
`spi3 seq` and `spi3 opread` send bytes and read replies; `gpio set`,
`gpio mode` and `gpio read` drive and read pins; `mem read` and `mem write`
reach any register; `usart tx` sends one two-byte word on the USART.
`scripts/bench.py` wraps them with a signal generator and with statistics
that refuse a result that has no control ([README-bench.md](../../scripts/README-bench.md)).

The commands that exist only to find out how a board works are a feature of
their own, off in every product and bench image (there they answer
`Unknown command`):

```
make -C firmware app-re        # the bench image plus the toolkit commands (profile `re`)
```

| Command | The question it answers |
|---|---|
| `re pinsweep [run]` | Which MCU pin is wired to the FPGA's RECONFIG_N? (below) |
| `spi3 probe` | Does the FPGA drive MISO at all, idle and with chip select low? |
| `spi3 gowin` | What do the configuration port's IDCODE, USERCODE and STATUS say? Read at two clock rates, the IDCODE as the known answer: a STATUS whose IDCODE is wrong is not decoded |
| `spi3 edgecap [reps]` | Does a frame from the SPI peripheral differ on the wire from the same frame bit-banged on the same pins? It sends both bursts for a logic analyser (step 5) |
| `spi3 opsweep [a b [len]]` | Which opcodes of the running design return data? One read window per opcode, a known read between them as the control; opcodes known to write are skipped |
| `fpga busrelease`, `fpga busreacquire` | Can an external master configure the FPGA? The first takes the MCU off the SPI bus (`tools/re/fpga-config/`, step 4), the second takes the bus back |
| `fpga reinit [...]` | Which step of the configuration handshake through the SPI peripheral matters? It replays the handshake with one parameter changed per run (clock divider, gaps, frame shape, a reset pulse on a chosen pin, trailing clocks) and prints the STATUS |
| `pinlog [start]` | Which pin follows an event? It stores every port's input word and the battery ADC once a second, on a change only: start it, plug a cable or press a key, print the rows |
| `usart raw <10 hex bytes>` | What does the chip on the USART answer to an arbitrary frame? It prints what the link's counters did and the frame that came back |

Board data: the SPI3 register addresses and the chip-select pin (PB6) in
`re_spi3.c`, the opcodes `spi3 opsweep` skips, the IDCODE, the pins
`pinlog` masks as busy, the 10-byte frame of `usart raw`. The files are
`firmware/src/re/re_spi3.c`, `re_fpga_config.c` and `re_board.c`; the bench
shell (`src/drivers/shell_bench.c`) keeps the bus helpers they call. Run
`fpga selftest` first: it says whether the bus reads right.

`re pinsweep` lists the candidate pins; `re pinsweep run` looks for the MCU
pin wired to the FPGA's RECONFIG_N. It pulses one candidate low at a time and
watches the configuration port's STATUS register, each read checked against
the IDCODE. A hit can leave the FPGA without a design (`fpga reload builtin`
loads ours again). Board data: `firmware/src/re/pin_sweep_table.h` holds the
candidates, the pins that must never be pulsed (the power latch first) and
the IDCODE; `make -C firmware test-re-pin-sweep` fails the build if a
candidate is on the forbidden list. Not run on the 2C53T since it became a
shell command: the pin is known here (PC6).

Build knobs for bench work on the configuration path:

| Build | What it changes |
|---|---|
| `make guest-coldtrace-la` | `FPGA_BB_HALF_DELAY=60`: the bit-bang configuration clock slowed to 1-2 MHz, so a 24 MS/s logic analyser decodes the whole upload |
| `make app-rtt` | `DEBUG_SHELL_RTT_ONLY`: the shell on RTT only, for a board whose USB does not enumerate yet (needs a debug probe) |
| `make guest-caldump` | `CAL_DUMP_MODE`: step 8's read-only dump of one internal flash sector |

## Other documents in this directory

| File | What it holds |
|---|---|
| [re_guide.md](re_guide.md) | Ghidra project settings, headless scripts, where to start in the image |
| [reference_projects.md](reference_projects.md) | Other projects and forums on FNIRSI instruments |
| [firmware_analysis.md](firmware_analysis.md) | Factory firmware versions and structure |
| [function_map.md](function_map.md) | Named functions and variables of the factory firmware |
| [freertos_tasks.md](freertos_tasks.md), [rtos_analysis.md](rtos_analysis.md) | The factory firmware's RTOS and tasks |
| [acq_seam_root_cause.md](acq_seam_root_cause.md) | Analysis of the record seam of the factory FPGA design |
| [parity2_static.md](parity2_static.md) | Static reading of several factory features |

The analysis output of the factory firmware is under `reverse_engineering/`.
