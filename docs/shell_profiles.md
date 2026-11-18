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
| `bench` (`app-coldtrace-meter`) | 603,372 B | 204,896 B |
| `full` (`app-full`) | 523,180 B | 203,040 B |

A/B on the bench image: `benchshell` 56,608 B flash and 1,296 B RAM, `meterdebug` 23,568 B flash and 560 B RAM
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
`scripts/shell_table_baseline.txt` (the table of `bench`; `test_shell_profiles.py --update` rewrites it after
a deliberate change): `bench` has exactly those rows, `full` has every core command, no row is lost,
duplicated or caught by a shorter one, and no image has a retired command (below).

## The interface to instrument state

Instrument state is the property model (`firmware/src/model/`), and the shell reaches it with four commands:

| command | what |
|---|---|
| `get <prop\|glob> [...]` | values: `get ch1.vdiv`, `get trig.*`, `get *`. One line per property, `name=value  # label` |
| `set <prop> <value>`, `set p=v [p=v ...]` | a change, applied by the input task exactly as a key applies it. A value is a label (`normal`, `10x`, `on`), a number in the property's unit (`gen.amp 1.5`), or a step (`+=1`, `-=5`). Up to 8 pairs, of which at most 4 change hardware (`list` marks them `HW`), applied in the order given; more answers `INVALID (more than 4 HW pairs); nothing applied` |
| `list [prefix\|glob]` | every property: name, type, range, value, flags |
| `list changed [prefix\|glob]`, `get changed` | only the properties whose value is not the built-in default (with the default; as `get` lines) |
| `describe <prop>` | range or labels, built-in default, unit, where it is saved, what a reset does to it, the owner call |
| `session [status]`, `session list`, `session save`, `session clear` | the session snapshot behind Startup on Boot = Restore: what the boot found and applied (with the properties it could not put in force), which properties are in it and which are left out and why; `save` writes what an orderly power-off writes, `clear` invalidates it |
| `reset settings CONFIRM` | "Reset to defaults": the built-in defaults over the user settings, applied through the setters and saved; never calibration, gallery or logo ([arch/settings_defaults.md](arch/settings_defaults.md)) |

and the keys with `ui press <KEY> [n]`, `ui long <KEY>`, `ui seq <tok> ...`; `ui screen` is one `key=value`
line that says where the UI is. `set` answers one line per pair:

```text
OK ch1.vdiv=4  # 2.00V
OK trig.level=30 px ; stored: scope FE off       (applied to the state; the hardware takes it on scope entry)
CLAMPED gen.freq=50000 Hz
INVALID tb.code=0x0A (bad value; 0..20); nothing applied
NOT_SET ch1.vdiv (relay apply failed); old value kept: 3  # 1.00V
REFUSED trig.source (CH2 is off); old value kept: CH1
BUSY ch1.vdiv (self-cal running)          BUSY (executor not up yet)          BUSY (request queue full)
TIMEOUT: input task busy (gallery/countdown/selfcal?); nothing applied
```

Anything but `OK` / `CLAMPED` means the pair was not applied (`scripts/bench.py` `Scope.set()` raises on it).
A change whose owner is another task is posted (`meter.func`, `mask.*`, `sys.mode`): the reply says
`; posted` and the value in force is read with `get` (or `meter state`, `mask`).

## Core (63 rows, in every image; `stream` only with the stream feature)

`help`, `?`, `version`, `features`, `status`, `fwload`, `fwstat`, `fwapply`, `fwswap`, `siggen`, `flash jedec`,
`flash read`, `flash dump`, `can decode`, `can stats`, `can screen`, `cal status`, `cal backup`, `cal restore`,
`cal page`, `cal base`, `cal user`, `fpga scope autoset`, `fpga scope basecal`, `fpga scope center50`, `fft auto`,
`fpga scope selfcal`, `screen dump`, `screen dumpbin`, `screenshot`, `logo`, `usb`, `screen shadow`,
`fpga scope range`, `fpga scope measure`, `fpga scope cursor`, `settings`, `reset settings`, `session`, `get`, `set`,
`list`, `describe`,
`stream`, `ui press`, `ui long`, `ui seq`, `ui screen`, `mask`, `fpga edgefilter`, `fpga cfgsettle`,
`fpga interleave`, `fpga unrotate`, `fpga scope trigger`, `fpga roll`, `fpga scope center`, `meter state`,
`meter relays`, `persist`, `batt`, `meter auto`, `ui dump`, `reboot bootloader`, `uptime`, `tasks`

Core today, but really a function of the instrument that wants a property or a user command:
`fpga scope autoset/basecal/center50/selfcal`, `fpga scope range/center`, `fpga scope measure/cursor`,
`fpga cfgsettle`, `fpga edgefilter`, `fpga unrotate`, `can decode/stats/screen`, `cal page/base/user`,
`meter relays/auto`, `logo`.

### To be restructured

Four commands keep their old names for what has no property yet. Their setting forms are retired: a setting
form prints a usage line that names the property and changes nothing.

| command | what is left | why it is not a property |
|---|---|---|
| `fpga roll` | status (step, ms/point, reg 0x01, fifo, last op03 frame, newest point per channel); `fpga roll code <hex>\|stock` | the status is a read-out of fpga.c; `code` is the entry byte override, a debug knob (when rolling it re-enters roll through `tb.rollstep`) |
| `fpga interleave` | status (enabled, active, record, B correction, knots, skew); `cal [piecewise]`, `tone`, `skew measure` | procedures: they drive the front end over many records and write the fit they find |
| `siggen` | state and the registers behind it (TMR7, DAC, DMA2 ch4, PA5, the cycle's extremes); `siggen dump` (the cycle in the DMA buffer) | register read-out for bench checks |
| `mask` | `status`, `teach [n] [ch1\|ch2\|both]`, `clear`, `reset`, `run`, `dump` | procedures on the acquisition task, counters, the bounds dump |

They are named after where the code used to be, not after the model: the procedures want to become actions
of their instrument and the read-outs diagnostics of it.

## benchshell

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

## Retired commands and their replacement

The commands that set instrument state from the shell were first made aliases of the property model and
are now retired: the property is the interface, and a second set of names for the same state would have to
be maintained for no gain. They answer `Unknown command`. Labels are matched without case.

| retired command | replacement | reply |
|---|---|---|
| `fpga scope vdiv <1\|2> <0-9>` | `set chN.vdiv <0-9>` (or the label `get` prints: `500mV`) | `OK ch1.vdiv=5  # 500mV` |
| `fpga scope ch <1\|2> on\|off` | `set chN.enabled on\|off` | `OK ch2.enabled=off`; the last channel left: `REFUSED ch1.enabled (Keep one CH on)` |
| `fpga scope ch <1\|2>` (read) | `get ch1.enabled ch2.enabled trig.source`; mask and bank in force: `cal page`; interleave active: `fpga interleave` | |
| `fpga scope probe <1\|2> 1x\|10x` | `set chN.probe 1x\|10x`; read: `get chN.probe` | `OK ch1.probe=10X` |
| `cal pos <1\|2> <-100..100>` | `set chN.pos <n>` | `OK ch1.pos=20` |
| `cal pos <1\|2>` (read) | `get chN.pos`; the offset code written, the reference read back and the record median: `cal page` (its `CH1: range .. pos .. -> DAC1 ..` lines) | |
| `fpga scope timebase <hex>` | `set tb.code=<decimal> tb.roll=off` (a Y-T code leaves roll; in this order it is one write of reg 0x01). The code is decimal; hex is not accepted (`INVALID tb.code=0x0A (bad value; 0..20)`) | `OK tb.code=16  # 2.00ms` + `OK tb.roll=off` |
| `fpga scope timebase` (read) | `get tb.code`; reg 0x01, the sample rate, the s/div label and the boot reconcile: `status` (the `boot reconcile:` and `timebase 0x.. (reg 0x01 = 0x..)  N S/s  .../div` lines) | |
| `fpga roll <0-8>` | `set tb.rollstep=<0-8> tb.roll=on` (the step first: one write) | two `OK` lines |
| `fpga roll off` | `set tb.roll off` | `OK tb.roll=off` |
| `fpga scope hpos <0..300>` | `set trig.x <0..300>` | `OK trig.x=160 px` |
| `fpga scope hpos` (read) | `get trig.x`; where the last window landed and the anchor: `fpga scope trigger` (`trigger position: asked column ..; last window landed at .. (anchor ..)`) | |
| `fpga scope edge rising\|falling` | `set trig.edge rising\|falling`; read: `get trig.edge`; the MCU edge filter: `fpga edgefilter` | `OK trig.edge=Falling` |
| `fpga scope trigmode auto\|normal\|single` | `set trig.mode auto\|normal\|single` | `OK trig.mode=Single ; Await Trigger...` |
| `fpga scope trigmode` (read) | `get trig.mode`; status, record kind, single armed/captured: `fpga scope trigger` | |
| `fpga scope source ch1\|ch2` | `set trig.source ch1\|ch2`; reg 0x06 in force: `fpga scope trigger` (`reg06 0x..`) | `OK trig.source=CH2`; a channel that is off: `REFUSED trig.source (CH2 is off)` |
| `fpga scope single` | `set trig.mode single` (a set of the mode it has re-arms) | `OK trig.mode=Single ; Await Trigger...` |
| `fpga scope level <-100..100>` | `set trig.level <n>` | `OK trig.level=-20 px` |
| `fpga scope level` (read) | `get trig.level`; the reg 0x08 code in force and the boot reconcile's: `fpga scope trigger` (`trigger level .. -> reg 0x08 code 0x.. in force (boot reconcile wrote 0x..)`) | |
| `fpga scope softtrig on\|off\|toggle` | `set trig.soft on\|off`, `set trig.soft +=1` (toggle) | `OK trig.soft=on` (the value after the step) |
| `fpga scope graticule true\|auto\|toggle` | `set view.truescale on\|off`, `set view.truescale +=1` | `OK view.truescale=on` |
| `fpga scope graticule` (read) | `get view.truescale`; which ranges honour it (`(no cal -> autofit)`): `ui dump` | |
| `fpga scope prm [ch1\|ch2] <mask>\|all\|none` | `set chN.meas <mask>\|all\|none`, or names: `set ch1.meas Freq,Vp-p` | `OK ch1.meas=Freq,Vp-p` (the names, `none` for 0) |
| `fpga scope prm [chN] default` | `set chN.meas 0x507` (`describe ch1.meas` gives the default) | |
| `fpga scope prm [chN] toggle <0-11>` | no step on a mask: `get chN.meas`, flip the bit on the host, `set chN.meas <mask>` | |
| `fpga scope prm hide\|show` | `set view.meashide on\|off` | `OK view.meashide=on` |
| `fpga scope prm` (read) | `get ch1.meas ch2.meas view.meashide`; what the strip shows and `(no slot)`: `ui dump` (`prm hidden=..`, `CHn mask=0x... shown: ...`) | |
| `fpga interleave on\|off` | `set il.enabled on\|off` | `OK il.enabled=on` |
| `fpga interleave corr <gain x1e4> <offset x100>` | `set il.knots=0 il.gain=<gain> il.offset=<offset>`, in the property's units: `corr 10123 -45` is `il.gain=1.0123 il.offset=-0.45`. `il.knots=0` first drops a piecewise map (a new gain or offset is refused while one is active) | three `OK` lines |
| `fpga interleave skew set <ppm>`, `skew off` | `set il.skew <ppm>`, `set il.skew 0` | `OK il.skew=-49000 ppm`; `get il.*` prints `il.gain=1.012300`, `il.offset=-0.45 counts` |
| `fpga interleave skew` (read) | `get il.skew`, or the status of `fpga interleave` | |
| `siggen wave <n>` | `set gen.wave <0-12>` or its name (`sine`) | `OK gen.wave=sine` |
| `siggen freq <Hz>` | `set gen.freq <Hz>` | `OK gen.freq=2000 Hz` |
| `siggen amp <0-30>` (tenths of a volt) | `set gen.amp <volts>`: `amp 15` is `gen.amp 1.5`. `gen.amp 15` is 15 V and answers `CLAMPED gen.amp=3.0 V` | `OK gen.amp=1.5 V` |
| `siggen duty <pct>` | `set gen.duty <pct>` | `OK gen.duty=50 %` (with a note when the waveform has no duty) |
| `siggen on\|off` | `set gen.out on\|off` | `OK gen.out=on` |
| `siggen boot on\|off` | `set gen.bootout on\|off` | `OK gen.bootout=off` |
| `siggen wave 3 amp 15 freq 2000 on` | `set gen.wave=3 gen.amp=1.5 gen.freq=2000 gen.out=on` (four hardware pairs is the most one `set` takes: a fifth, `gen.duty`, goes in a second `set`) | one line per pair |
| `mask tol <v> [h]` | `set mask.tolv=<v> mask.tolh=<h>` (v 0..64 record counts, h 0..16 samples; the old default of h was 2) | `OK` per pair, `; posted`: the acquisition task applies it. The two are two posts, each built from the tolerance in force: read them back (`get mask.tolv mask.tolh`, or `mask`) before relying on them, and send again if one did not land |
| `mask stop on\|off` | `set mask.stopfail on\|off` | `OK mask.stopfail=on ; posted` |
| `meter func <0-11>` | `set meter.func <0-11>` or its name (`dcv`, `ohm`, ...) | `OK meter.func=ohm ; posture posted`, or `; stored: meter FE off` when the meter link is off |
| `meter degf on\|off` | `set meter.degf on\|off`; read: `get meter.degf` (also `degF=` in `meter state`) | `OK meter.degf=on` |
| `mode scope`, `mode 0` | `set sys.mode scope` | `OK sys.mode=scope ; posted` |
| `mode meter [func] [layout]`, `mode 1 ...` | `set meter.func=<f> meter.layout=<l> sys.mode=meter` (the function first: on another screen it is stored and the meter entry applies it, one function change); without arguments `set sys.mode meter` | `OK meter.func=dca ; stored: meter FE off`, `OK meter.layout=stats`, `OK sys.mode=meter ; posted` |
| `mode` (read) | `get sys.mode sys.startup meter.func meter.layout`, or the first line of `ui dump` | |
| `mode startup scope\|meter\|home\|siggen\|restore` | `set sys.startup <name>`; read: `get sys.startup` | `OK sys.startup=meter` |
| `btn <key> [n]` | `ui press <KEY> [n]` | `OK 3 events 412 ms`, then the `ui screen` line |
| `btn <key> long` | `ui long <KEY>` (refused for a key without a long event; `btn` sent it and the handler ignored it) | as above |
| `keys` | `ui screen` (screen, `focus` = the arrow stage, `ach` = the adjusted channel); `get ch*.enabled ch*.pos trig.* view.meashide`; the long-press time: `ui dump` | |

### What the retired commands did that `set` does not

- **They waited.** `mode meter` and `meter func` answered when the meter chip had switched (up to 5 s);
  `mask tol` and `mask stop` when the acquisition task had taken the request (up to 400 ms, else
  `NOT APPLIED`). `set` answers when the input task has applied or posted the change. A script that needs
  the switch waits for it: `get meter.live`, `meter state` (`sequences=`, `soc_live=`, `reading: valid=`),
  `get mask.tolv mask.tolh mask.stopfail`.
- **`meter func` timed the switch** and named the path (`path=FAST(one word) dt_ms=..`). There is no such
  line now: `meter state` has the selector word, `fast_switches`, `fallbacks`, `echo_ok`, `tries` and
  `sequences`, so the path is the counter that moved and the time is measured on the host.
- **`meter func` refused with the meter link off.** `set meter.func` stores the function for the next meter
  entry and says so.
- **`mode startup` flushed the settings record and printed the outcome.** `set sys.startup` marks the record
  dirty like any saved property; `settings` shows the store's state, and `persist on` asks for a flush and
  prints its outcome.
- **`fpga scope timebase` took hex.** `tb.code` is decimal (`0x10` is `16`).

The replies in the table were read from a host build of the model (`make test-model-exec`'s fakes); those
of `il.*`, `mask.*` and `meter.degf` are from the setters' code, their owners are not in that build.

Read-outs that no property carries were moved, text unchanged, so a script's pattern still matches:
`status` (timebase in force, boot reconcile), `fpga scope trigger` (level code and reconcile code, trigger
column and anchor), `ui dump` (measurement strip, graticule per channel, long-press time), and `cal page`
already had the offset DAC read-back of `cal pos`.

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
