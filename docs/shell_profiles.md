# Shell commands per build profile

The debug shell (USB CDC, `firmware/src/drivers/usb_debug.c`) has a fixed core and two optional sets of
commands, which are build features of `firmware/features.mk`:

| feature | HELLO bit | what | files |
|---|---|---|---|
| `benchshell` | 3 | reverse-engineering and bring-up commands: raw FPGA frames and wire words, SPI3, GPIO, USART, memory and W25Q pokes, the stock-state shadow, the acquisition experiment knobs, the pin log | `shell_bench.c`, `shell_bench.h` |
| `meterdebug` | 4 | DMM link diagnostics: trace, dump, frontend, streams, mux and timing replays, waveform sampler state, link experiments | `shell_meterdebug.c`, `shell_meterdebug.h` |

Both are on by default, so a plain `make` and the bench image (`make app-coldtrace-meter`, profile `bench`)
have every command. The profile `full` (`make app-full`: the bench image's hardware flags) leaves both out.
With a feature off its sources are not compiled, `help` does not list its commands and they answer
`Unknown command` like any typo; `features` shows the feature as off and its HELLO bit is clear.

| image | flash (text+data) | static RAM (bss) |
|---|---|---|
| `bench` (`app-coldtrace-meter`) | 609,988 B | 204,888 B |
| `full` (`app-full`) | 529,788 B | 203,032 B |

A/B on the bench image: `benchshell` 56,616 B flash and 1,296 B RAM, `meterdebug` 23,560 B flash and 560 B RAM
(`make features-ab FEATURE=benchshell`).

## How a command is placed

A command is **core** if a user, the host client, a host script used in normal operation or the flash and
recovery path needs it; otherwise it belongs to one of the two features. Some core commands are today the only
door to a real function of the instrument (marked below). They stay core until they have a property or a user
command of their own.

The handlers of the two features were moved out of `usb_debug.c` unchanged. What they need from the shell
core (print, argument parsing, the scratch buffer) they reach through `shell_internal.h`: next to each static
helper `usb_debug.c` has a thin accessor `usb_debug_sh_*`; the helpers themselves and the CDC core are untouched.
Dispatch is by longest match, so the place of a row in the table only decides where `help` prints it: the
core rows first, then the bench shell, then the meter diagnostics.

`make test-shell-profiles` reads the command table as each profile compiles it and checks it against
`scripts/shell_table_baseline.txt` (the table before the split): `bench` has exactly those rows, `full` has
every core command, no row is lost, duplicated or caught by a shorter one.

## Core (80 rows, in every image; `stream` only with the stream feature)

`help`, `?`, `version`, `features`, `status`, `fwload`, `fwstat`, `fwapply`, `fwswap`, `siggen`, `flash jedec`,
`flash read`, `flash dump`, `can decode`, `can stats`, `can screen`, `cal status`, `cal backup`, `cal restore`,
`cal page`, `cal pos`, `cal base`, `cal user`, `fpga scope autoset`, `fpga scope basecal`,
`fpga scope center50`, `fpga scope selfcal`, `screen dump`, `screen dumpbin`, `screenshot`, `logo`, `usb`,
`screen shadow`, `fpga scope range`, `fpga scope vdiv`, `fpga scope measure`, `fpga scope prm`,
`fpga scope cursor`, `fpga scope graticule`, `fpga scope softtrig`, `settings`, `get`, `set`, `list`,
`describe`, `stream`, `fpga scope hpos`, `fpga scope edge`, `btn`, `keys`, `ui press`, `ui long`, `ui seq`,
`ui screen`, `mask`, `fpga edgefilter`, `fpga cfgsettle`, `fpga scope ch`, `fpga scope probe`,
`fpga interleave`, `fpga unrotate`, `fpga scope trigmode`, `fpga scope trigger`, `fpga scope source`,
`fpga scope single`, `fpga scope level`, `fpga scope timebase`, `fpga roll`, `fpga scope center`, `meter func`,
`meter state`, `meter relays`, `meter degf`, `persist`, `batt`, `mode`, `meter auto`, `ui dump`,
`reboot bootloader`, `uptime`

Core today, but really a function of the instrument that wants a property or a user command:
`fpga scope autoset/basecal/center50/selfcal`, `fpga scope range/vdiv/ch/probe/timebase/hpos/center`,
`fpga scope trigmode/trigger/source/single/level/edge/softtrig`, `fpga scope measure/prm/cursor/graticule`,
`fpga interleave`, `fpga cfgsettle`, `fpga edgefilter`, `fpga unrotate`, `fpga roll`, `can decode/stats/screen`,
`mask`, `siggen`, `cal page/pos/base/user`, `meter func/relays/degf/auto`, `mode`, `logo`.

## benchshell (87 rows, plus `fpga dbgclk` and `fpga dbgarm` in an `FPGA_ALT_BITSTREAM` build)

`usart raw`, `usart tx`, `gpio set`, `gpio mode`, `bench snapshot`, `bench restore`, `gpio read`, `gpio scan`,
`buzzer test`, `mem read`, `mem write`, `trig2`, `trig`, `flash wtest`, `flash diag`, `fpga cmd`, `fpga frame`,
`fpga diag clear`, `fpga selftest`, `fpga busrelease`, `fpga busreacquire`, `fpga configbb`, `fpga stock diag`,
`fpga stock clear`, `fpga stock set`, `fpga stock preset`, `fpga stock base2`, `fpga stock state5`,
`fpga stock state6`, `fpga stock prev`, `fpga stock next`, `fpga stock select`, `fpga stock toggle`,
`fpga stock commit`, `fpga stock consume`, `fpga stock bridge fixed`, `fpga stock bridge dynamic`,
`fpga stock reenter`, `fpga wire words`, `fpga wire entry`, `fpga wire scope`, `fpga scope cal`,
`fpga scope freq`, `fpga scope zero`, `fpga postedge`, `fpga scope coupling`, `fpga pollgap`, `fpga autolive`,
`fpga holdlog`, `fpga holdread`, `fpga acqbr`, `fpga pairgap`, `fpga usart`, `fpga rearm`, `fpga autowait`,
`fpga rearmwait`, `fpga acqgate`, `fpga rate`, `fpga scope reinit`, `fpga meter reinit`, `fpga scope wake`,
`fpga scope acqmode`, `fpga scope beat`, `fpga scope entry`, `fpga scope timing`, `fpga scope trig`, `pinlog`,
`lcd readback`, `lcd`, `fpga acq`, `fpga reinit`, `spi3 xfer`, `spi3 seq`, `spi3 read`, `spi3 frame`,
`spi3 acqread`, `spi3 opread`, `spi3 opsweep`, `spi3 armtest`, `spi3 gowin`, `spi3 edgecap`, `spi3 scopetest`,
`spi3 acqtest`, `spi3 stock-readback`, `spi3 h2txdiag`, `spi3 h2verify`, `spi3 probe`

## meterdebug (17 rows)

`meter hdr`, `meter fast`, `meter keepalive`, `meter overlay`, `meter beep`, `meter dump`, `meter autoscan`,
`meter trace`, `meter frontend`, `meter probe-tail`, `meter boot-sequence`, `meter pc11-timing`,
`meter mux-arms`, `meter mux-stream`, `meter stream`, `meter adc-snapshot`, `meter wave`
