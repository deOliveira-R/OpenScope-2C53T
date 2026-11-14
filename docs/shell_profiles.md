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
| `bench` (`app-coldtrace-meter`) | 616,764 B | 204,912 B |
| `full` (`app-full`) | 536,596 B | 203,056 B |

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

## Core (81 rows, in every image; `stream` only with the stream feature)

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
`reboot bootloader`, `uptime`, `tasks`

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

## Old state commands are aliases of the property model (ARCH-1 M4)

The commands that set instrument state no longer write it. Each parses its old syntax, asks the input task
for the change through the executor (`prop_request`, exactly as `set` does, with the same timeouts) and
prints its old text, so scripts and transcripts keep working (`firmware/src/drivers/shell_alias.c`; host
test `make test-shell-alias`, which compares the output with the text of the handlers before the change).
What a command printed that is not a property (register read-backs, counters, the status blocks) is still
printed, read only.

| old command | property |
|---|---|
| `fpga scope vdiv <1\|2> <0-9>` | `chN.vdiv` |
| `fpga scope ch <1\|2> [on\|off]` | `chN.enabled` |
| `fpga scope probe <1\|2> [1x\|10x]` | `chN.probe` |
| `cal pos <1\|2> [pos]` | `chN.pos` |
| `fpga scope timebase [hex]` | `tb.code`, then `tb.roll=off` (a Y-T code leaves roll) |
| `fpga roll [off\|<step>]` | `tb.roll`; `tb.rollstep`, then `tb.roll=on` |
| `fpga scope hpos`, `edge`, `trigmode`, `source`, `level`, `softtrig` | `trig.x`, `trig.edge`, `trig.mode`, `trig.source`, `trig.level`, `trig.soft` |
| `fpga scope single` | `trig.mode=single` (the setter arms it) |
| `fpga scope graticule [auto\|true\|toggle]` | `view.truescale` |
| `fpga scope prm [ch1\|ch2] <mask\|all\|default\|none\|toggle N>`, `hide\|show` | `chN.meas`, `view.meashide` |
| `fpga interleave on\|off`, `corr <g> <o>`, `skew set <ppm>\|off` | `il.enabled`; `il.knots=0`, `il.gain`, `il.offset`; `il.skew` |
| `siggen wave\|freq\|amp\|duty\|on\|off\|boot` | `gen.wave`, `gen.freq`, `gen.amp`, `gen.duty`, `gen.out`, `gen.bootout` (one request per token) |
| `mask tol <v> [h]`, `mask stop on\|off` | `mask.tolv`, `mask.tolh`; `mask.stopfail` |
| `meter func <n>`, `meter degf [on\|off]` | `meter.func`, `meter.degf` |
| `mode scope\|0`, `mode meter\|1 [func] [layout]` | `sys.mode` (after `meter.func`, `meter.layout`) |
| `mode startup [scope\|meter\|home\|siggen\|restore]` | `sys.startup`, then the settings flush it reports |

Not aliases, on purpose: `fpga roll code` (the entry byte override is a debug knob of fpga.c; when rolling
it re-applies through `tb.rollstep`), the procedures `fpga interleave cal`, `tone` and `skew measure` (they
drive the front end over many records and write the fit they find), `mask teach/clear/reset/run`, and the
bench shell's `meter autoscan` and `fpga meter reinit`.

Replies that are new (the old handlers could not give them):

- `TIMEOUT: input task busy (gallery/countdown/selfcal?); nothing applied` (or `; still applying: check with
  get`): the input task did not answer in 600 ms (1500 ms for a hardware change).
- `BUSY <prop> (self-cal running)`: autoset or the baseline correction is running.
- `BUSY (executor not up yet)`: the boot logo is still shown (the input task starts the executor after
  it); `BUSY (request queue full)`: four requests are waiting for an input task that does not answer.
  In all three cases nothing was changed: a script that does not read the reply no longer gets the change
  it got from the old handler, which acted whatever the input task was doing.
- after the old reply, `  (<prop> stored: <instrument> FE off; applied when it is next entered)`: the
  hardware path was not live (for example a range change on the meter screen with `meter relays on`).
- `mask tol: v <n> is past the model's range, set to 64 (mask.tolv 0..64)`.
- `mode startup restore` is accepted; the usage line names it.

What changed besides the text:

- A failed hardware apply leaves the state as it was (`fpga scope level`, `timebase` and `roll` used to
  keep the new value).
- A change that changes nothing is not re-applied: `fpga scope ch` with the state it has, `fpga roll off`
  out of roll, `mode scope` in scope mode (use `fpga scope reinit` to re-enter the scope posture).
- `fpga scope ch <n> off` moves the active channel to the one left on, as the key does.
- `meter func`, `mode meter` and `meter degf` have the key's side effects: the software autoselect is
  cancelled and hold and relative are released; `mode meter <f> 3` (the fuse tester) forces DC V. The
  function change runs on the meter poll task; the commands wait for it (up to 5 s) before they answer.
- `mode` runs the keys' mode switch: the oscilloscope menu is closed, the settings are flushed and the
  screen is repainted. Every alias repaints the screen that shows what it changed.

## `tasks`

One line per FreeRTOS task, in creation order, then the heap:

```text
tasks: 11 (stack and min-free in words of 4 bytes; min-free = the least ever left)
  name             prio state  stack min-free  free%
  display             1 ready    768      301    39%
  key                 4 block    384      112    29%  <- key (input) task
  ...
heap: free 5120 B, min ever 4800 B, of 31744 B
key (input) task: stack 384 words, min-free 112 words (448 B)
```

(The numbers above are placeholders for the format.) `stack` is read from the task's control block
(`configRECORD_STACK_HIGH_ADDRESS`, 4 bytes of heap per task); `min-free` is
`uxTaskGetStackHighWaterMark`. The key (input) task is the one named `key` (main.c creates it with 384
words): every key, every screen and every property setter runs on it.
