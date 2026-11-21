# Display architecture

Status: agreed design (2026-11-20). Steps 1 to 5 of the order of work are
done (the canvas, and Y-T, roll and the spectrum as display types on it), and
the view table of step 8 exists (`firmware/src/ui/view_table.h`). X-Y, the
waterfall and the split view's upper half still have their own renderers.

## Why

Each view of the scope (Y-T, roll, X-Y, spectrum, split, waterfall) had its
own drawing code, and the Y-T view had two (a full redraw and a live update).
They composed pixels in different ways, so they could disagree (measured on
the unit before step 2: the full redraw drew a flat trace one row thick and
the live update two; they ordered the mask bounds and two crossing cursors
differently), a speed-up of one did not help the others (a live Y-T frame
took 35 ms, a full redraw 200 to 290 ms), and a new view starts from a copy
of an old one. Other instruments
that want a trace (the CAN scope, a meter trend) cannot reuse a view at all.

## The parts

```
UI scheme        theme roles, fonts, where the furniture goes
   |
Display type     axes + ordered list of elements + strip direction
   |    \
   |     Element library   grid, trace, cursor, marker, mask, label
   |
Canvas           one rectangle of the screen; the draw cycle
   |
Panel driver     bus, addressing
```

Beside them: **sources**, the data a display type shows.

### UI scheme

The theme, the fonts, and the layout of the furniture (status bar, badge
rows, soft keys, the rectangle each canvas gets). Elements ask the scheme for
a **role**, never for a colour: `trace.ch1`, `grid`, `grid.centre`, `trigger`,
`cursor`, `clip.converter`, `clip.screen`. A theme maps roles to colours.

### Display type

A display type is a small module with one interface. It states:

- **Axes.** What x and y mean and how a value maps to a pixel and back:
  time and volts (Y-T, roll), frequency and level (spectrum), volts and volts
  (X-Y), frequency and time (waterfall). Grid lines, cursors and readouts are
  computed from the axes, so they are right for every type that has them.
- **Elements, in drawing order.** Which elements exist is a property of the
  type. Y-T has grid, trigger level, trigger position, traces, math, mask,
  tags, cursors. A spectrum has a grid, traces and cursors, and no trigger
  line. A waterfall has an image and a frequency cursor. A type lists only
  what it has.
- **Strip direction.** Columns or rows (see Canvas).
- **Sources it needs** and what it does when one is missing.

Types are entries of a table; the display setting (`view.mode`) selects one.
A prototype type can sit in the table beside a stable one.

### Domains

The Display menu mixes three kinds of choice: how the data is taken (roll),
what is analysed (the time record, or the spectrum computed from it) and how
it is drawn (Y-T or X-Y of the record; spectrum or waterfall of the FFT;
split as a layout of two). The second kind is the **domain**: time or
frequency. Measurements, cursors and keys belong to the domain, not to the
picture:

- **Measurements.** Time: frequency, period, duty, the voltages. Frequency:
  the peak, its level, the harmonics.
- **Cursors.** Seconds and volts, or hertz and dB.
- **The arrows' jobs and AUTO.** Volts/div and time base, positions and the
  trigger, or reference level and span; autoset or the span fit.
- **Its settings.** The FFT window, averaging and max hold are settings of
  the analysis (`fft.window`, `fft.avg`, `fft.maxhold`; the settings page's
  Spectrum pane), as coupling is a setting of a channel.

A view says which domains it shows; `view_table.h` holds that per `view.mode`
value, with the jobs short SELECT steps through and what the view draws of
its domain (the measurement strip, cursors). The key model, the settings
page and the furniture ask the table, never "is this the FFT view". Split
shows both domains: its job list runs over both halves, and the job the
arrows are on says which half the keys, AUTO, the strip and the cursors
belong to. That half is drawn in a frame (the focus mark,
`focus_mark_px` in `view_table.h`), under the data: a trace, a label or a
cursor on the frame's pixels wins.

### Element library

Elements are written once, against the axes: a grid asks the axes where its
lines fall, a cursor asks what a pixel means, a trace asks where a sample
lands. An element paints into a strip: it is given the strip's index and
buffer and writes the pixels it owns. Elements that only one type has (the
trigger position pointer) live with that type.

**Clipping** is part of the trace element and uses two roles, because the two
cases ask the user for different things:

- `clip.converter`: the sample is at the converter's limit. The data is cut
  off; the range must change. This is a property of the data.
- `clip.screen`: the sample is valid but falls outside the plot. The position
  must change (or Auto pressed). This is a property of the picture only.

Only the first one matters to a host that reads records over USB; the stream
should carry it as a flag of the record, and never the second.

### Canvas

The canvas owns one rectangle and the draw cycle, and is the only caller of
the driver's pixel writes for that rectangle:

1. ask the display type to prepare a frame (read the sources once);
2. for each strip: clear the strip buffer, let each element paint into it in
   order, compare the result with what the strip held last time, send it if
   it changed;
3. keep the screenshot copy in step.

A **full redraw is the same cycle** with every strip marked as changed. A
strip is trusted as unchanged only while nobody else drew inside the
rectangle (the driver reports that).

A strip is a **column or a row**, chosen by the display type. The panel takes
a window of any shape, so both cost the same to send. The choice follows the
data: a trace is a function of x, so Y-T, roll and the spectrum use columns
(a column holds a few short spans); a waterfall adds one spectrum per row and
uses rows. The panel's hardware vertical scroll may reduce a waterfall update
to one new row; that is to be measured on the unit before it is relied on.
X-Y is a cloud of points and fits neither; it paints from an intensity buffer
(the persistence view has one) in strips.

Holes (a popup over the plot) are the canvas's business: it sends the parts
of a strip around the hole.

### Panel driver

The bus, the address window, a stream of pixels along a row or a column. The
screenshot copy is a separate part fed by the stream functions. Nothing
above the canvas calls the driver for a canvas's rectangle.

### Sources

The scope's record, math results, a spectrum, the lanes of the CAN scope, a
meter trend. A source has samples and the scales needed to build axes. An
**instrument** is a source, a display type and its furniture; this is how
another instrument gets a view without copying it.

## Order of work

Each step keeps the picture the same and is checked pixel by pixel against
the code it replaces (`fpga scope livecheck` on the unit; the plot read back
from the screenshot copy after a full redraw and after a live update).

1. Done. Canvas, column strips: the draw cycle (strip buffer, hash, send,
   holes) lifted out of the live Y-T frame. Checked by
   `firmware/tests/test_canvas.c` against a model of the glass.
2. Done. Y-T as the first display type: a frame built once, a list of
   elements painted in order into each column strip; the full redraw is the
   canvas with every strip sent (67 ms where it took 215 to 250 ms, and the
   plot is no longer blanked first). Where the two old renderers disagreed
   the live update's rule was kept. Its elements still read the Y-T maps
   directly; axes as an interface come with the second type (step 5).
3. Done. Clipping roles in the Y-T trace element
   (`clip_converter`, `clip_screen` in the theme): a column is at the
   converter's limit when the trace sits on a rail for at least three
   columns in a row, and off the plot when its sample lands beyond the map
   or under the measurement strip, where it is then drawn on the last rows
   that can be seen. Only the rows of the cut-off sample take the colour; the
   channel's badge in the info bar takes it too. The stream carries the
   converter case as `clip_mask` of the RECORD frame (protocol 1.3), never
   the off-plot one. Roll gets the colours with step 5.
4. Done. Cursors as a span element: the lines and labels are painted only
   on the rows they occupy (a frame with cursors takes 33 ms where it took
   80 to 89 ms, 20 redraws a second where it was 9.4). The per-pixel function
   stays as the rule `fpga scope livecheck` compares the painting with.
5. Roll: done. It is a display type on the canvas and shares the graticule
   and the trace element with Y-T (`firmware/src/ui/plot_elements.c`), which
   gave it the clip colours; its fitted band uses the Y-T map. The spectrum:
   done as a frame painted through the canvas, with the popup box as a hole
   (the split view's halves still send every column until they are two
   canvases, step 7). Its axes are an interface (`fft_axes_t`,
   `firmware/src/dsp/fft_live.h`): the bars, the carets and the cursors
   convert through it. It has the furniture of its domain: a measurement
   strip (peak, level, harmonics) and cursors on frequency and level, kept
   in bins and dB so they stay on what they mark, with the readout in the
   strip's AUX row as in Y-T. The cursor layer is the Y-T one, built from
   positions on the type's axes.
6. Row strips; the waterfall, then with the spectrum's strip and a
   frequency cursor; measure the hardware scroll.
7. X-Y from the intensity buffer; split as two canvases, each half then
   with its domain's strip and cursors (the keys already follow the job the
   arrows are on).
8. The view table: done for the keys (`view_table.h`: domains and jobs per
   `view.mode`). Open: the table also selects the renderer (today a switch
   on `scope_view` in `main.c`, with roll as a flag beside it), and the
   `FEATURE_FFT` build switch goes.
9. Panel driver in layers; the measurement strip computed outside the paint.
