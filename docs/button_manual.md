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

Elsewhere: inside a level (a Settings sub-page, the Oscilloscope Settings
page, the generator's frequency editor) **MENU** goes back one level (in the
page's locked Measure / Trigger pane it closes the page). At the top of the meter, generator and settings it opens home
(ours; owner decision D2). In the scope, short MENU opens the **Oscilloscope
Settings** page, as stock's short BACK does (see *Oscilloscope Settings page*
below). **Long MENU** opens home from anywhere, as stock's long BACK does.
Long MOVE / SELECT / TRIGGER jump straight to meter / scope / generator.

**SAVE** works the same in every mode (owner decision D1): short SAVE saves a
screenshot of the whole screen to the gallery (`Saved <n>.bmp`), long SAVE
opens the gallery. Stock does both in the scope only.

Settings -> **Startup on Boot** offers stock's four choices: Home (stock calls
it "None"), Meter, Scope, Signal Gen, and ours: Last Used (the instrument that
was on screen when the device was last switched off; the default).

---

## Oscilloscope Mode

### Channel Controls

| Button | Action |
|--------|--------|
| **CH1** | Select CH1 as active channel + toggle coupling (DC <-> AC, on the relay) |
| **CH2** | Select CH2 as active channel + toggle coupling (DC <-> AC, on the relay) |
| **CH1 / CH2, long** | Channel on/off. At least one stays on. CH2 off at a fast timebase (code 3 and below) gives the single-channel 250 MS/s record; the popup says `250MS/s` when that routing is in force |
| **MOVE** | **Switch CH1/CH2 adjustment**: which channel UP/DOWN act on (stock's "Switch CH1/CH2 Adjustment", manual p.30; a channel that is off is skipped). Mask stage: stop-on-fail |

The probe (1X/10X) is in Settings -> Oscilloscope Settings, and on the bench
shell as `set ch1.probe 1x|10x` (`ch2.probe`). A 10X probe shows as `10X` after
the coupling in the bottom bar. Until UI round 1 (2026-11) the short meanings
of MOVE and SELECT were the other way round from stock's; the long ones were
already right.

### Vertical / Horizontal

In the Zoom stage (the default, see SELECT below):

| Button | Action |
|--------|--------|
| **UP** | Increase V/div for active channel (zoom out vertically) |
| **DOWN** | Decrease V/div for active channel (zoom in vertically) |
| **LEFT** | Decrease timebase (faster sweep, zoom in horizontally) |
| **RIGHT** | Increase timebase (slower sweep, zoom out horizontally) |

V/div steps: 5mV, 10mV, 20mV, 50mV, 100mV, 200mV, 500mV, 1V, 2V, 5V

Timebase steps: 5ns, 10ns, 20ns, 50ns, 100ns, 200ns, 500ns, 1us, 2us, 5us, 10us, 20us, 50us, 100us, 200us, 500us, 1ms, 2ms, 5ms, 10ms, 20ms

### What the arrow keys adjust (SELECT)

**SELECT** cycles the arrow keys' function, as stock's "Select Function for
Directional Keys" does (manual p.30), in every scope view. A popup names the
stage and the status bar shows it. Each view offers the stages that make
sense for what it shows (table below).

| Stage | UP/DOWN | LEFT/RIGHT |
|-------|---------|------------|
| Zoom | V/div of the adjusted channel | Timebase |
| Channel move | Vertical position of the adjusted channel (+-100 px, 5 per press) | Trigger point across the screen |
| Trigger move | Trigger level (the dotted line) | Trigger point across the screen |
| Cursor (only while cursors are on) | Move the active cursor | Switch cursor |
| Mask (only while a mask exists) | Mask vertical tolerance +-2 counts | Mask horizontal tolerance +-1 sample (MOVE = stop-on-fail) |
| Spectrum | Reference level, 5 dB per press | Span: LEFT zooms in, RIGHT out |

| View | Stages, in SELECT's order (the first is where the view starts) |
|------|------|
| Y-T, Roll | Zoom, Channel move, Trigger move, [Cursor], [Mask] |
| X-Y | Zoom, Channel move, Trigger move |
| FFT, Waterfall | Spectrum, Zoom (the timebase sets the spectrum's range, V/div its level) |
| Split | Zoom, Channel move, Trigger move, Spectrum: one list over both halves |

In Split a thin frame surrounds the half the keys are on: the upper half
(the traces) in the first three stages, the lower half (the spectrum) in the
Spectrum stage. AUTO acts on the framed half.

Switching cursors on (Settings -> Oscilloscope -> Cursors, or the Oscilloscope
Settings page) selects the Cursor stage straight away; SELECT leaves it. Channel position moves only the channel's ground-reference marker
for now: the live trace is autoscaled and ignores it, and it drives neither the
offset DAC nor the FPGA offset bytes (the calibration item adds that). Until 2026-11 the stages were V/div -> Trig level ->
Position -> Mask, and cursors took the arrows whenever they were on.

### Trigger (TRIGGER)

**TRIGGER** opens the Oscilloscope Settings page directly inside its
**Trigger Settings** pane, locked there, as stock V1.2.0 does (owner on unit
#2, 2026-11-12; in every scope view): three rows, `Mode:` Auto / Normal /
Single, `Edge:` Rising / Falling, `Channel:` CH1 / CH2 (the source; a channel
that is off is refused).

| Key (Trigger pane, locked) | Action |
|------------------|--------|
| **UP/DOWN** | Pick the row |
| **LEFT/RIGHT**, **OK** | Change it (OK = RIGHT) |
| **TRIGGER** or **MENU** | Close the page, back to the plot |
| **PRM** | Switch to the locked Measure pane |
| **CH1 / CH2 / MOVE / SELECT / AUTO** | Nothing (no way to the rail or other rows) |

Until 2026-11-12 TRIGGER opened an inline panel over the plot (parity C3); it
is gone (it kept the plot on the full clear-and-repaint path, which flashed).

The same three are in Settings -> Oscilloscope Settings (Trigger Mode / Edge /
Source), and on the shell (`set trig.mode auto|normal|single`,
`set trig.edge rising|falling`, `set trig.source ch1|ch2`; `get trig.*` reads
them, `fpga scope trigger` prints the registers and the status). The trigger level
is the Trigger move stage (SELECT, then UP/DOWN); the dotted line and the arrow
at the right edge show where the hardware fires, the small triangle at the
top the column where the trigger point landed.

The source drives the FPGA (reg 0x06) since parity C3: before, CH2 as the
source moved the label and the soft trigger while the hardware kept firing on
CH1.

**SINGLE** is stock's one-shot. Selecting it arms it (`Await Trigger...`, badge
`Await`); the first triggered record after the arm is captured and held (badge
`STOP`); **OK** then re-arms it (`set trig.mode single` does the same). A record
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
| **AUTO** (scope views of the time record: Y-T, with roll too, X-Y, Split) | Baseline correction (stock) |
| **OK** (the same views) | 50 % centre (stock) |
| **SAVE** (every mode) | Screenshot gallery (stock: scope only) |

While the Oscilloscope Settings page is up, the scope's own long presses
(PRM, CH1, CH2, AUTO, OK) do nothing: they act on the plot, which the page
hides. Long MENU / MOVE / SELECT / TRIGGER / SAVE keep their meaning.

The shell's `ui long <KEY>` injects the same event (`ui press <KEY> [n]` a short
press, `ui seq` a sequence), and `ui screen` prints where the UI is: the screen,
the arrow stage (`focus`), the adjusted channel (`ach`). The channel enables are
`get ch1.enabled ch2.enabled`.

### Mask pass/fail (time view)

| Button | Action |
|--------|--------|
| **AUTO** | No mask: teach one from the next 8 good captures. Teaching: cancel. Mask ready: turn it off |
| **SELECT** -> *Mask* | Adjust tolerances; MOVE = stop-on-fail (table above) |
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
| **UP/DOWN** | Move active cursor 4 pixels (Cursor stage) |
| **LEFT/RIGHT** | Switch between cursor pairs (V1/V2 or H1/H2) (Cursor stage) |

Switching cursors on puts the arrow keys in the Cursor stage; SELECT steps on to the others. The cursor readout shows delta-time (dt), frequency (1/dt), and delta-voltage (dV).

Each time-view line is kept at its time or level, not at a place on the
screen: a vertical line at its time from the trigger point (in roll, back
from the newest point; roll and Y-T keep a pair each), a horizontal line at
its voltage at the active channel's probe tip. Change the time base, the
range or the position and the lines move with what they mark; one that
leaves the plot is not drawn, and its readout stays. UP on a vertical line
moves it left, UP on a horizontal line moves it up, 4 pixels of the plot at
the scale in force; a line off the plot comes to the edge first. Lines off
the plot when the cursors are switched on start at a third and two thirds of
the plot. With a range that has no volts (uncalibrated) the horizontal lines
are not drawn and dV reads "--". The shell sets them as `view.curt1`/`2`
(ticks of 0.4 ns), `view.curr1`/`2` (ms) and `view.cury1`/`2` (volts).

**On a spectrum (FFT view)** the same four lines are cursors on frequency
(V1, V2) and level (H1, H2), with the readout in the same row above the
measurements: the two frequencies and their difference, and the level
difference (with the level lines alone, both levels too). Each line is kept
at its frequency or level, not at a place on the screen: zoom the span and a
frequency line stays on the spectral line it marks (and is not drawn while
that frequency is off the screen; its readout stays). UP on a frequency line
moves it down in frequency, UP on a level line moves it up; one step is
4 pixels of the plot at the span and level range in force, so zooming in
makes the steps finer. Levels are dB relative to one count of the
transform's input, the unit of the plot's own scale.

### FFT / Spectrum Views

| Button | Action |
|--------|--------|
| Oscilloscope Settings page | The views are chosen on the page (CH panes "FFT: ON/OFF", Display), as on stock. Short PRM cycled them until UI round 1; it now opens the page |
| **MENU** / **PRM** | Open the Oscilloscope Settings page, as in the time view (the way back to it) |
| **SELECT** | Next arrow stage of the view (see "What the arrow keys adjust") |
| **UP/DOWN**, **LEFT/RIGHT** | In the Spectrum stage: reference level and span |
| **AUTO** | Fit the spectrum's span (FFT, Waterfall; Split while the arrows are in the Spectrum stage, else autoset) |
| Settings page, **Spectrum** row | Window (Rect, Hanning, Hamming, Blackman-Harris, Flat top), averaging (off, x2 to x32), max hold. Saved with the settings |
| **PRM** | The Measure pane, on the spectrum's own measurements (FFT view; long PRM hides them) |

The spectrum's measurements, shown in a strip under the plot as the time
views show theirs: **Peak** (frequency of the strongest bin; it is the
bin's centre, so good to half a bin width), **Level** (that bin's level),
**H2** and **H3** (the level at twice and three times that frequency,
relative to the peak, dBc). Peak and Level are on until you choose. The
spectrum half of Split and the Waterfall do not draw the strip or cursors
yet.

Note: Entering an FFT view claims the shared memory pool (88KB). Returning to time view releases it.

### Run Control

| Button | Action |
|--------|--------|
| **OK** | Toggle acquisition Run / Stop (shows popup) |

### Oscilloscope Settings page

Stock's settings page for the scope (stock state 9): a rail of rows on the
left (Display, Cursors, Measure, CH1, CH2, Trigger, Persist, Math, Spectrum), the
selected row's settings on the right. The plot is hidden while it is up; the
acquisition keeps running behind it.

| Key | Action |
|-----|--------|
| **MENU** (short, scope) | Open the full page at Display (rail focus: every row) |
| **PRM** (short, scope) | Open the page inside Measure ("Parameter Measurement"), **locked** to that pane |
| **TRIGGER** (short, scope) | Open the page inside Trigger ("Trigger Settings"), **locked** to that pane |
| **UP / DOWN / LEFT / RIGHT / OK** (page up) | Move and change on the page (locked: in the pane only; LEFT at its edge does nothing) |
| **MENU** (page up) | Full page: back one level; from the rail, close. Locked pane: close |
| the key that opened it (PRM, TRIGGER, ...) | Close the page (stock: PRM again goes back) |
| **PRM / TRIGGER** (locked pane, not the opener) | Switch to the other locked pane |
| **PRM / TRIGGER / CH1 / CH2** (full page, not the opener) | Jump to Measure / Trigger / CH1 / CH2 (ours); that key then closes it |
| **CH1 / CH2** (locked pane) | Nothing |
| **MOVE / SELECT / AUTO** (page up) | Nothing |
| **SAVE** | Screenshot of the page; long SAVE = gallery |
| long **MENU / MOVE / SELECT / TRIGGER** | Home / meter / scope / generator, as everywhere (the page closes) |

Measure pane: one panel per source, of the sources the view on screen shows:
CH1 and CH2 in the views of the time record, FFT in the spectrum views, all
three in Split. OK on the panel's tab goes to the next panel; DOWN past a
panel's last row scrolls into the next. Each channel picks any of stock's 12
measurements (both default to Freq, Peri, Duty+, Vp-p, Vrms); the spectrum
picks among its four. The plot's strip shows at most 6 per
channel; with more on, the 6 shown are chosen by priority, levels first:
Vp-p, Vmax, Vmin, Freq, Duty+, Vrms, then the rest in stock order
(`mstrip_show_priority[]`, firmware/src/ui/measure_strip.c). They are drawn
in stock order.

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
| **SAVE** | Screenshot (long SAVE: gallery), as in every mode. The raw-frame debug overlay it used to toggle is the shell's `meter overlay` |

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
  Reset to Defaults        [asks first: OK = yes, MENU = no]
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
Home -> Meter -> Scope -> Signal Gen -> Last Used -> Home
```

`Last Used` (`sys.startup = restore`) boots into the instrument that was on
screen when the device was last switched off (POWER held, or the auto
power-off): the scope, the meter or the generator. Only a power-off records
the screen, so powering off on the meter boots the meter, and changing
screens writes nothing. Opening the update screen (MENU + POWER) does not
record it. If the device was switched off on Home or in Settings, or the
saved record is older than this choice, it boots to Home.

Fresh or erased settings default to `Last Used` (until 2026-11-14 the default
was stock's `Meter`), so a unit without saved settings boots to Home. A unit
with saved settings keeps the choice it stored. The choice and the last meter
function (including Auto) are saved with the other settings and restored at
boot, in the plain image and in the `-meter` image alike (the other
warm-handoff images are scope-only and always start in the scope).
`set sys.startup scope|meter|home|siggen|restore` sets it from the shell
(saved within 100 ms, unless `persist off`); `get sys.lastscreen` shows the screen the record held when
it was loaded at boot.

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
| MENU   | Osc Settings page| -> Home          | -> Home          | Back / -> Home   |
| CH1    | Coupling DC/AC   |        -         |        -         |        -         |
| CH2    | Coupling DC/AC   |        -         |        -         |        -         |
| UP     | V/div up         |        -         | Amplitude up     | Selection up     |
| DOWN   | V/div down       |        -         | Amplitude down   | Selection down   |
| LEFT   | Timebase down    | Prev function    | Duty cycle down  |        -         |
| RIGHT  | Timebase up      | Next function    | Duty cycle up    |        -         |
| OK     | Run/Stop         | Hold             | Output on/off    | Enter / Toggle   |
| PRM    | Measure pane     | Softkey: View    | Freq editor      |        -         |
| SELECT | Arrow stage      | Softkey (Reset)  | Cycle waveform   | Cycle comp type  |
| MOVE   | CH1/CH2 adjust   | Softkey (Func.)  |        -         |        -         |
| TRIGGER| Trigger pane     | Softkey (Rel.)   |        -         |        -         |
| AUTO   | Autoset          | Auto function    |        -         |        -         |
| SAVE   | Screenshot       | Screenshot       | Screenshot       | Screenshot       |
| SAVE L | Gallery          | Gallery          | Gallery          | Gallery          |
+--------+------------------+------------------+------------------+------------------+

Note: in the scope the arrows follow the stage SELECT picked (Zoom by
default: V/div and timebase); in the Cursor stage UP/DOWN moves the cursor and
LEFT/RIGHT switches it. Home: SAVE is a screenshot there too.
```
