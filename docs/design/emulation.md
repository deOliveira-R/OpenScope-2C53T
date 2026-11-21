# Emulating the unit

Status: proposal (2026-11-21). Nothing of it is built.

## Why

Every look at a screen costs a flash, and needs the unit on the bench and
switched on. We write nearly all the logic the unit runs: the firmware, and
the FPGA's capture core. So most of the unit can run on the development
machine, and the unit is needed only for what a model cannot tell us.

What we want from it, in order of value:

1. Any screen as a picture, without the unit (to choose between designs, to
   review a change, to compare against a reference in the gate).
2. The firmware's logic run under a debugger and memory checkers.
3. The scope's whole path, from a signal to the trace, exercised without a
   bench signal: front end, capture core, record, display.
4. The real firmware image run on a model of the processor, for boot and
   driver problems.

## What exists

- **Host tests.** About 90 targets compile the pure modules for the
  development machine: the key model, the property model, measurements, the
  FFT, the canvas against a model of the glass (`firmware/tests`).
- **The capture core's source** (`fpga/core/rtl`, SystemVerilog), with a
  testbench and simulation cells (`fpga/core/tb`, `fpga/core/sim`), run with
  Icarus Verilog.
- **FreeRTOS's POSIX port** is in the vendor tree
  (`FreeRTOS/portable/ThirdParty/GCC/Posix`): the same tasks and queues as
  threads of a host process.
- **A Renode platform** (`emulator/renode`, inherited): it describes the
  factory unit, with an LCD model and button injection. Its own notes say the
  FPGA, the LCD bus and the converters are stubs and that the firmware stalls
  on them. `make emu` builds an image for it. It has not been run against the
  present firmware.
- **The shell.** Every bench script drives the unit through the shell
  (`ui press`, `get`/`set`, `screen dumpbin`, `fpga scope livecheck`). An
  emulated unit that offers the same shell is driven by the same scripts.

## The parts of the unit, and what replaces each

| Part of the unit | What the firmware sees | Replacement |
|---|---|---|
| LCD (ST7789 on the parallel bus) | `drivers/lcd.h`: a window, then a stream of pixels | A frame buffer that follows the same window rule; shown in a window, written as PNG |
| Keys | `button_scan` reading port pins | Key events from the keyboard or from the shell |
| FreeRTOS on the processor | tasks, queues, ticks | The POSIX port |
| USB serial (the shell, the stream) | a byte stream in and out | A local socket or the terminal |
| MCU flash pages (settings, calibration) and the W25Q chip | read, erase, write of regions | Files |
| FPGA (our capture core) on SPI3 | commands, registers, records (`fpga_acq`) | A model of the core: see below |
| Front end (relays, attenuators, coupling, the two converters) | relay pins and DAC values out, 8-bit codes in through the core | A signal source and a model of gain, offset, coupling and clipping |
| Meter chip on USART2 | frames | A model of the frames it sends for a given input |
| Generator DAC | samples out | A buffer the front end's model can take as its signal (a loop-back) |
| Battery, charger, power hold, buzzer, backlight | pins and one ADC channel | Fixed or scripted values |

Two of them need a decision on depth.

**The capture core** can be modelled at two levels, and we want both:

- *Behavioural*, in C: the registers, the time base, the trigger, the record
  and its header as the firmware reads them. Fast enough for interactive use.
  It is checked against the real core in simulation (the same stimulus to
  both, the records compared), so it cannot drift from the core unnoticed.
- *The real core*, compiled from its SystemVerilog by Verilator into a
  library with the same interface. Slower, and exact for logic: the trigger,
  the interleave, the roll mode are the code that runs in the chip.

**The front end** is where a model is weakest. Gain steps, offset, coupling,
bandwidth limit and clipping are simple to model, and the unit's calibration
data can be loaded into the model as into the unit. Noise, relay settling,
the converters' real bandwidth and their interleave errors are properties of
the hardware; the model has them only as parameters we choose.

## Two ways of running the firmware

### A. The firmware built for the development machine

The application is compiled natively. Below a boundary, the drivers are
replaced by the table above. This is the route for goals 1 to 3.

- Gives: pictures of every screen in milliseconds, a debugger, address and
  undefined-behaviour checkers on the whole application, tests that press
  keys and compare screens, the bench scripts run unchanged.
- Does not give: stack use, the real time a frame takes, the processor's
  interrupt behaviour, anything about the real drivers.

What it needs from the firmware is **one clean boundary**: today about 30
source files call the LCD driver and about 20 read the FPGA driver's
buffers or state. The work is to state that boundary as a small set of
headers (panel, keys, storage, capture, meter link, serial, time) and to
keep everything above it free of processor headers. The display
architecture already asks for the panel half of this (its step 9, "panel
driver in layers").

### B. The real image on a model of the processor

Renode runs the image that is flashed, instruction by instruction. This is
the route for goal 4.

- Gives: boot, clocks, interrupts, the real drivers, stack use.
- Needs models of the AT32's peripherals as the firmware uses them: the
  clock controller, the parallel bus with the LCD, SPI3 with the core's
  model behind it, USART2 with the meter's model, the flash controller, the
  timers, the power-hold pin, the watchdog. USB is the costly one; the shell
  would be given a UART in this build.
- The models of the FPGA, the front end and the meter are the same ones
  route A uses, behind a different connector.

Route B repeats much of route A's work at a lower level and answers fewer
of our daily questions, so it comes second and only when a problem at that
level justifies it.

## What stays on the unit

- Analogue truth: noise, bandwidth, settling, calibration against a
  reference.
- Real timing: the LCD bus, redraws per second, the FPGA's timing closure
  and its pad behaviour.
- The power path, charging, USB enumeration.
- The final check of anything before it is called done.

The self-checks that run on the unit (`fpga scope livecheck`) stay; the
emulated unit runs them too, which checks the emulation against the unit.

## Order of work

1. **The boundary and a host build that shows a screen.** Panel, keys, time
   and storage replaced; the capture source is a recorded record (the tests
   have fixtures). The shell on a socket. Outcome: `ui press`, screenshots
   and the menu screens work with no unit.
2. **Screens in the gate.** A set of reference screens compared pixel by
   pixel on every gate run.
3. **The behavioural core and the front end's model**, with a signal source
   (sine, square, a recorded waveform, the generator's loop-back). Checked
   against the core's simulation. Outcome: trigger, time base, autoset, roll
   and the measurements run end to end.
4. **The meter's model.**
5. **The real core through Verilator** as a second capture back end.
6. **Renode**, when needed.

Step 1 is the large one and is mostly a reorganisation of the firmware, so
it is done in small moves, each one keeping the unit's image unchanged
function by function where it can.

## Open points

- Where the host build lives: a `host` profile of the same makefile, or a
  directory of its own beside `firmware/`.
- The window for interactive use: SDL, or a small web page fed over the
  socket (the inherited `emulator/ui` is one).
- What to do with the inherited Renode platform until step 6: keep it,
  untested, or remove it and rebuild it then.
