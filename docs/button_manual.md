# OpenScope 2C53T - Button & Navigation Manual

Physical buttons: **MENU**, **AUTO**, **SAVE**, **CH1**, **CH2**, **PRM**, **SELECT**, **UP**, **DOWN**, **LEFT**, **RIGHT**, **OK**, **MOVE**

Emulator keys: `M`=MENU, `A`=AUTO, `S`=SAVE, `1`=CH1, `2`=CH2, `P`=PRM, `Space`=SELECT, Arrows, `Enter`=OK

---

## Mode Switching

**MENU** opens the **home page** (stock's mode-selection screen): four items in
a row, Multimeter, Oscilloscope, Signal Generator, Settings, with the one you
came from highlighted.

| On the home page | Action |
|--------|--------|
| **LEFT / RIGHT** | Move the selection (wraps around) |
| **OK** | Enter the selected mode |
| **MENU** | Back to the mode you opened home from (ours; stock does nothing) |
| **POWER** | As everywhere (hold to power off) |

Elsewhere: inside a level (a Settings sub-page, the trigger panel, the
generator's frequency editor) **MENU** goes back one level; at the top of a
mode it opens home. **Long MENU** opens home from anywhere, as stock's long
BACK does. Long MOVE / SELECT / TRIGGER still jump straight to meter / scope /
generator. Stock's short BACK in the scope opens "Oscilloscope Settings"; here
that page is Home -> Settings -> Oscilloscope Settings.

Settings -> **Startup on Boot** offers stock's four choices: Home (stock calls
it "None"), Meter, Scope, Signal Gen.

---

## Oscilloscope Mode

### Channel Controls

| Button | Action |
|--------|--------|
| **CH1** | Select CH1 as active channel + toggle coupling (DC <-> AC, on the relay) |
| **CH2** | Select CH2 as active channel + toggle coupling (DC <-> AC, on the relay) |
| **CH1 / CH2, long** | Channel on/off. At least one stays on. CH2 off at a fast timebase (code 3 and below) gives the single-channel 250 MS/s record; the popup says `250MS/s` when that routing is in force |
| **SELECT** | **Switch CH1/CH2 adjustment**: which channel UP/DOWN act on (stock's key meaning; a channel that is off is skipped). Mask stage: stop-on-fail. FFT views: window function |

The probe (1X/10X) is in Settings -> Oscilloscope Settings, and on the bench
shell as `fpga scope probe <1|2> [1x|10x]`. SELECT used to cycle it; stock's
SELECT switches the adjusted channel, so it does that now. A 10X probe shows
as `10X` after the coupling in the bottom bar.

### Vertical / Horizontal

In the Zoom stage (the default, see MOVE below):

| Button | Action |
|--------|--------|
| **UP** | Increase V/div for active channel (zoom out vertically) |
| **DOWN** | Decrease V/div for active channel (zoom in vertically) |
| **LEFT** | Decrease timebase (faster sweep, zoom in horizontally) |
| **RIGHT** | Increase timebase (slower sweep, zoom out horizontally) |

V/div steps: 5mV, 10mV, 20mV, 50mV, 100mV, 200mV, 500mV, 1V, 2V, 5V

Timebase steps: 5ns, 10ns, 20ns, 50ns, 100ns, 200ns, 500ns, 1us, 2us, 5us, 10us, 20us, 50us, 100us, 200us, 500us, 1ms, 2ms, 5ms, 10ms, 20ms

### What the arrow keys adjust (MOVE)

**MOVE** cycles the arrow keys' function, as stock's "Select Function for
Directional Keys" does. A popup names the stage:

| Stage | UP/DOWN | LEFT/RIGHT |
|-------|---------|------------|
| Zoom (default) | V/div of the adjusted channel | Timebase |
| Channel move | Vertical position of the adjusted channel (+-100 px, 5 per press) | Trigger point across the screen |
| Trigger move | Trigger level (the dotted line) | Trigger point across the screen |
| Cursor (only while cursors are on) | Move the active cursor | Switch cursor |
| Mask (only while a mask exists) | Mask vertical tolerance +-2 counts | Mask horizontal tolerance +-1 sample (SELECT = stop-on-fail) |

Order: Zoom -> Channel move -> Trigger move -> [Cursor] -> [Mask] -> Zoom.
Switching cursors on (Settings -> Oscilloscope -> Cursors) selects the Cursor stage straight away; MOVE
leaves it. Channel position moves only the channel's ground-reference marker
for now: the live trace is autoscaled and ignores it, and it drives neither the
offset DAC nor the FPGA offset bytes (the calibration item adds that). Until 2026-11 the stages were V/div -> Trig level ->
Position -> Mask, and cursors took the arrows whenever they were on.

### Trigger (TRIGGER)

**TRIGGER** opens the trigger panel, as stock's TRIGGER opens "Trigger
Settings": three rows, `Mode:` Auto / Normal / Single, `Edge:` Rising /
Falling, `Channel:` CH1 / CH2 (the source; a channel that is off is refused).

| Key (panel open) | Action |
|------------------|--------|
| **UP/DOWN** | Pick the row |
| **LEFT/RIGHT**, **OK** | Change it (OK = RIGHT) |
| **TRIGGER**, **MENU**, **MOVE**, **SELECT**, **PRM** | Close the panel |

The same three are in Settings -> Oscilloscope Settings (Trigger Mode / Edge /
Source), and on the shell (`fpga scope trigmode`, `fpga scope edge`,
`fpga scope source`, `fpga scope trigger` for the status). The trigger level
is the Trigger move stage (MOVE, then UP/DOWN); the dotted line and the arrow
at the right edge show where the hardware fires, the small triangle at the
top the column where the trigger point landed.

The source drives the FPGA (reg 0x06) since parity C3: before, CH2 as the
source moved the label and the soft trigger while the hardware kept firing on
CH1.

**SINGLE** is stock's one-shot. Selecting it arms it (`Await Trigger...`, badge
`Await`); the first triggered record after the arm is captured and held (badge
`STOP`); **OK** then re-arms it (`fpga scope single` does the same). A record
the FPGA was holding from before the arm is dropped, not taken.

The badge at the top right of the trace says what the acquisition did, not
what the mode is called:

| Badge | Meaning |
|-------|---------|
| `Trig'd` | the record on screen was handed over by a trigger, recently |
| `Wait` | NORMAL (or AUTO holding): no trigger lately, the trace is held |
| `Auto` | AUTO free-running (no trigger for a whole budget), one channel on |
| `Auto*` (warning colour) | AUTO free-running with **both** channels on: CH1 and CH2 were read ~140 us apart while the FPGA kept writing, so they are two moments, not one capture (upstream #84). Trigger the signal, or turn a channel off, for a coherent two-channel record |
| `Await` | SINGLE armed |
| `STOP` | stopped, or SINGLE captured |

While edges occur, AUTO shows only triggered records; it falls back to the
free-running buffer only after a whole budget without one, so a quiet input
still moves.

The top bar shows `<mode> <edge> <source>` for the trigger (`Auto R C1`,
`Norm F C2`, `Sngl R C1`: R/F = rising/falling).

| Button | Action |
|--------|--------|
| **OK** | Toggle Run / Stop (or, while a mask failure is held, resume testing) |

### Long presses

A long press is a hold of about 1 s (`KEY_LONG_TICKS`, key_model.h). Keys that
have a long action deliver their short press on **release**, so a long press
never fires the short action first; AUTO, OK and the arrows are unchanged
where they have no long action. POWER is unchanged (its own countdown).

| Long press | Action |
|------------|--------|
| **MENU** (stock: BACK) | Home: the mode-selection page, from any mode |
| **MOVE** | Multimeter (from any mode) |
| **SELECT** | Oscilloscope (from any mode) |
| **TRIGGER** | Signal generator (from any mode) |
| **PRM** (scope time view) | Hide / show the measurement badges |
| **CH1 / CH2** (scope) | Channel on/off (see above; stock has no long CH1/CH2) |
| **AUTO / OK / SAVE** (scope) | Popup `not built`: stock's baseline correction, 50 % centre and gallery are later parity items |

The shell's `btn <name> long` injects the same event, and `keys` prints the
mode, the arrow stage, the adjusted channel and the channel enables.

### Mask pass/fail (time view)

| Button | Action |
|--------|--------|
| **AUTO** | No mask: teach one from the next 8 good captures. Teaching: cancel. Mask ready: turn it off |
| **MOVE** -> *Mask* | Adjust tolerances and stop-on-fail (table above) |
| **OK** | Resume after a held failure |

Every capture is then judged PASS / FAIL, or skipped with a reason (for example,
a setting changed since teaching). Dotted lines show the mask. Columns that
broke it turn red, and a strip along the bottom shows the verdict. The badge
row shows `P<n> F<n>`, `MASK skip: <why>`, or `FAIL ... HOLD-OK` while a
failure is held. Full design: `docs/specs/scope/mask-pass-fail.md`.

### Cursors

| Button | Action |
|--------|--------|
| Settings -> Oscilloscope -> **Cursors** | Cycle cursor mode: Off -> Vertical -> Horizontal -> Both -> Off (stock keeps them in its menu, "Cursor Measurement"; TRIGGER cycled them until parity C3) |
| **UP/DOWN** | Move active cursor (Cursor stage) |
| **LEFT/RIGHT** | Switch between cursor pairs (V1/V2 or H1/H2) (Cursor stage) |

Switching cursors on puts the arrow keys in the Cursor stage; MOVE steps on to the others. The cursor readout shows delta-time (dt), frequency (1/dt), and delta-voltage (dV).

### FFT / Spectrum Views

| Button | Action |
|--------|--------|
| **PRM** | Cycle view: Time -> FFT -> Split -> Waterfall -> Time (from the time view only until the measurement-selection item takes short PRM there; long PRM hides the measurement badges) |
| **SELECT** | Cycle FFT window function (in FFT/Split/Waterfall views) |
| **UP/DOWN** | Adjust reference level (+/- 5dB, in FFT views) |
| **LEFT/RIGHT** | Zoom in/out frequency range (in FFT views) |
| **AUTO** | Auto-configure FFT (in FFT views; in the time view AUTO is the mask key) |

Note: Entering an FFT view claims the shared memory pool (88KB). Returning to time view releases it.

### Run Control

| Button | Action |
|--------|--------|
| **OK** | Toggle acquisition Run / Stop (shows popup) |

---

## Multimeter Mode

The four buttons directly under the screen are **softkeys**: the bar along the
bottom edge of the screen says what each one does, right above it, and its
current value. Softkeys arrived with the softkey UI (2026-10-06; spec
`docs/specs/platform/softkey-ui.md`).

### Softkeys

| Button | Big / Graph / Stats | Limits | Fuse |
|--------|---------------------|--------|------|
| **MOVE** | **Function**: next function | Function | **Cal leads**: hold the tips together and press, to take the lead offset off every reading. On the Types page: **Fuse type** |
| **SELECT** | **Reset**: min/max/avg, chart, histogram | **Low**: arrows move it; press again = set to the reading | **Rating** (next, wrapping; arrows step). In Scan: **Draw if >** presets 0.1 / 0.2 / 0.5 / 1 / 2 mV |
| **TRIGGER** | **Relative** on/off (zeros at the current reading). In Continuity: **Beep** threshold 10 / 30 / 50 / 100 Ohm | **High**, likewise | **Show**: Detail -> Table -> Scan -> Types |
| **PRM** | **View**: Big -> Graph -> Stats -> Limits -> Fuse | View | View |

The four softkeys act when you **release** them (a long press of MOVE, SELECT,
TRIGGER is the quick switch to the oscilloscope, multimeter, signal generator,
and a long press of MENU goes Home; see *Long presses* above).

### Other buttons

| Button | Action |
|--------|--------|
| **OK** | **Hold**: freeze the reading on screen now (HOLD badge); OK again = live |
| **LEFT / RIGHT** | Previous / next function (Fuse view: fuse type) |
| **UP / DOWN** | Limits view: move the highlighted limit. Fuse view: rating up/down (Scan: threshold ±0.1 mV) |
| **AUTO** | Stock's Auto function: the meter chip picks the function itself (V or Ohm, AC or DC, continuity) and the screen follows (`Auto DCV`, `Auto ACV`, `Auto Ohm`, `Auto Cont`). A second press re-arms it. The old software scan is the shell command `meter auto` |
| **SAVE** | Raw-frame debug overlay |

Changing function is instant: the reading shows dashes while the meter chip
switches in the background (~1.5 s in the `-meter` image). In the plain
`coldtrace` image only DC volts is measured; other functions show `---`.

### 11 Functions (and Auto)

```
0: DC Voltage       6: Resistance
1: AC Voltage       7: Continuity  (short = below the Beep threshold, default 30 Ohm)
2: DC Current (mA)  8: Diode Test
3: DC Current (A)   9: Capacitance
4: AC Current (mA) 10: Temperature
5: AC Current (A)
```

### 5 Views

- **Big** — the reading in 70 px digits, unit underneath, one line of min/max (or the relative reference, or SHORT/OPEN). Readable across the bench.
- **Graph** — compact reading, scrolling strip chart with an auto-scaled Y axis.
- **Stats** — min/max/avg/peak-to-peak and a histogram of the readings.
- **Limits** — pass/fail: a big PASS / FAIL box, the reading's position on a Low–High bar, pass and fail counts, and the last failing value. Entering it with a live reading sets Low/High to the reading ±5 %.
- **Fuse** — fuse current tester: probe the two metal tips on top of a fuse
  left in place, and it estimates the current from the voltage drop and the
  fuse's typical resistance (about ±10 %). Entering it switches to DC V.
  **Calibrate the leads first:** touch the probe tips together and press
  **Cal leads**. The meter reads about −1 mV with nothing connected, which
  would otherwise show as a ~120 mA draw on a 10 A fuse. The value is kept
  until power-off. No verdict is shown until the reading has been steady for
  about a second ("unsteady: probes on?", or WAIT in Scan).
  - **Detail**: a picture of the selected fuse, the current, a 0–150 mA bar
    with the 50 mA parasitic-draw mark, the drop and the fuse resistance.
  - **Table**: every rating of the type, largest at the top, with the current
    each would mean.
  - **Scan**: DRAW / NO DRAW against the threshold, for walking a fuse box
    with the engine off, plus the current if the fuse is 10/15/20/30 A
    (20/30/40/60 A for Maxi and J-Case).
  - **Types**: "which fuse is this?" — the five types to scale, front and top,
    and the common rating colours. The number on top of the fuse is what
    counts; colours vary.

---

## Signal Generator Mode

### Waveform & Output

| Button | Action |
|--------|--------|
| **SELECT** | Cycle waveform type |
| **OK** | Toggle output ON / OFF |

### Waveforms

```
Sine -> Square -> Triangle -> Sawtooth -> Full-Rect Sine -> Half-Rect Sine -> Pulse -> Noise
```

### Parameters

| Button | Action |
|--------|--------|
| **UP** | Increase amplitude (+0.1 Vpp) |
| **DOWN** | Decrease amplitude (-0.1 Vpp) |
| **LEFT** | Decrease duty cycle (-5%) |
| **RIGHT** | Increase duty cycle (+5%) |
| **PRM** | Cycle frequency presets: 1 -> 10 -> 100 -> 1k -> 10k -> 25k -> 1 Hz |

---

## Settings Mode

### Top-Level Menu

```
> Oscilloscope Settings    [sub-menu]
  Sound and Light
  Auto Shutdown
  Display Mode             [cycles theme]
  Math / Persist           [sub-menu]
  Component Tester         [sub-menu]
  Bode Plot                [sub-menu]
  Startup on Boot
  About                    [info screen]
  FPGA SPI Scanner
  Firmware Update          [USB HID bootloader]
  Factory Reset
```

### Navigation

| Button | Action |
|--------|--------|
| **UP** | Move selection up |
| **DOWN** | Move selection down |
| **OK** | Enter sub-menu or toggle selected option |
| **MENU** | Back to top-level (from sub-menu), or to the home page from the top level |

### Display Mode (OK to cycle)

```
Dark Blue -> Classic Green -> High Contrast -> Night Red -> Dark Blue
```

### Startup on Boot (OK/LEFT/RIGHT to cycle)

```
Scope <-> Meter
```

Fresh or erased settings default to `Meter`: the application starts in
Multimeter mode and configures the DMM frontend immediately after FPGA
initialization. Selecting `Scope` keeps the normal oscilloscope startup path. The choice and the
last meter function (including Auto) are saved with the other settings and
restored at boot, in the plain image and in the `-meter` image alike (the other
warm-handoff images are scope-only and always start in the scope). The `mode startup`
shell command sets it, and it is saved with the next key press.

### Oscilloscope Settings Sub-menu (depth 1)

```
> CH1 Coupling      [DC / AC / GND]
  CH1 Probe         [1X / 10X]
  CH2 Coupling      [DC / AC / GND]
  CH2 Probe         [1X / 10X]
  Trigger Mode      [Auto / Normal / Single]
  Trigger Edge      [Rising / Falling]
  Trigger Source    [CH1 / CH2]
  Cursors           [Off / Vertical / Horizontal / Both]
```

There is no "20M Limit" row: stock V1.2.0 has one in its tables but its menu
never reaches it and it drives no hardware (docs/re/parity2_static.md, C13).

Press **OK** on any item to cycle its value. **MENU** to go back.

### Math / Persist Sub-menu (depth 3)

```
> Math Channel      [Off / A+B / A-B / A*B / A/B / Max / Min]
  Persistence       [On / Off]
  Back
```

- **Math Channel**: OK cycles through math operations, then back to Off
- **Persistence**: OK toggles on/off. Enabling persistence claims the shared memory pool (evicts FFT if active)

### Component Tester (depth 4)

| Button | Action |
|--------|--------|
| **SELECT** | Cycle component type |
| **OK** | Enter Resistor Calculator |

### Resistor Color Band Calculator (depth 5)

Interactive graphical resistor with 4 color bands (Digit 1, Digit 2, Multiplier, Tolerance).

| Button | Action |
|--------|--------|
| **LEFT/RIGHT** | Move between bands |
| **UP/DOWN** | Change selected band's color |
| **SELECT** | Simulate measurement and show pass/fail |
| **OK** | Back to Component Tester |

Calculates expected resistance from color bands, then compares against measured value showing deviation percentage and PASS/FAIL against the selected tolerance.

### About Screen (depth 2)

Shows device info, pool status, heap free, license. **MENU** or **OK** to go back.

---

## Quick Reference Card

```
+--------+------------------+------------------+------------------+------------------+
|  Key   |   Oscilloscope   |    Multimeter    |    Signal Gen    |     Settings     |
+--------+------------------+------------------+------------------+------------------+
| MENU   | -> Multimeter    | -> Signal Gen    | -> Settings      | Back / -> Scope  |
| CH1    | Coupling DC/AC   |        -         |        -         |        -         |
| CH2    | Coupling DC/AC   |        -         |        -         |        -         |
| UP     | V/div up         |        -         | Amplitude up     | Selection up     |
| DOWN   | V/div down       |        -         | Amplitude down   | Selection down   |
| LEFT   | Timebase down    | Prev function    | Duty cycle down  |        -         |
| RIGHT  | Timebase up      | Next function    | Duty cycle up    |        -         |
| OK     | Run/Stop         | Hold             | Output on/off    | Enter / Toggle   |
| PRM    | Cycle FFT views  | Softkey: View    | Freq presets     |        -         |
| SELECT | Probe 1X/10X     | Softkey (Reset)  | Cycle waveform   | Cycle comp type  |
| MOVE   | Trigger edge     | Softkey (Func.)  |        -         |        -         |
| TRIGGER| Trigger panel    | Softkey (Rel.)   |        -         |        -         |
| AUTO   | FFT auto-config  | Auto function    |        -         |        -         |
| SAVE   | Screenshot       | Debug overlay    | Screenshot       | Screenshot       |
+--------+------------------+------------------+------------------+------------------+

Note: When cursors are active in Oscilloscope mode, UP/DOWN moves the cursor
and LEFT/RIGHT switches cursor selection (overrides V/div and timebase).
```
