# Display architecture

Status: agreed design (2026-11-20). Steps 1 to 5 of the order of work are
done (the canvas, and Y-T, roll and the spectrum as display types on it), and
the view table of step 8 exists (`firmware/src/ui/view_table.h`). Step 7's
port is done: X-Y and both halves of split draw through canvases. The
waterfall still has its own renderer.

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

The scope's choices are of three kinds: how the data is taken (roll, a
source of the time-base: `tb.roll`, which only a view that takes it draws),
what is analysed (the time record, or the spectrum computed from it) and how
it is drawn (Y-T or X-Y of the record; spectrum or waterfall of the FFT;
split as a layout of two; the Display menu's choice). The second kind is the **domain**: time or
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
   gave it the clip colours; its fitted band uses the Y-T map. Since step
   10 (b) it is no longer a type of its own but a source of the Y-T type. The spectrum:
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
   arrows are on). The port is done: each canvas has its own memory and its
   own watch of its rows (`canvas_id_t`, `LCD_WATCH_SLOTS`), so split's halves
   each skip what did not change; X-Y paints its whole band (the margins and
   title too) from the trail; both keep the popup box as a hole. Open: the
   strips and cursors of the halves and of X-Y, which change the picture.
8. The view table: done. `view_table.h` holds the domains and jobs per
   `view.mode`, and `main.c` selects the renderer from a table indexed by
   `view.mode`. The FFT build switch (`make WITHOUT=fft`) is gone
   (2026-11-24): the spectrum views and X-Y are in every image, and the
   feature's bit 1 in `features.mk` is retired.
9. Panel driver in layers; the measurement strip computed outside the paint.
10. Panes, so split can pair any two views and each half has the furniture
    of its view. (a) Done: the Y-T type draws into a pane it is given
    (`firmware/src/ui/plot_pane.h`: a full-width band of rows, its canvas,
    its graticule division derived from its height), and its grid, maps,
    tags, trigger line, math, persistence and cursor lines take their rows
    from it. Cursors keep their rows on the full plot (`view.curh1`/`2`) and
    are put on a pane's graticule at the same place in divisions. Checked at
    full size only: 11 states read back from the screen before and after,
    every difference a trace or a reading that moves with the signal;
    livecheck 0 of 1.8 M pixels with cursors and math on; 18.6 frames a
    second before and after. (b) Roll as a source of the Y-T type: done
    for the drawing. `view_build_roll()` lays the FIFO out in the Y-T window
    (newest at the right), the frame says which elements the source has,
    and the fill counter is a Y-T element; roll's own renderer is gone.
    Roll points are drawn as stored (`as_stored` on the map), never
    rescaled like a held record. The picture is the old roll picture: 6
    roll states read back, every difference a trace, a reading or a setting
    the capture order left different; livecheck 0 in roll. The measurement
    cells now refresh at the Y-T rate (twice a second) instead of every 8
    points, and roll draws 18.6 frames a second where it drew 15.6. Then roll
    gained what the source can support: the trigger line and the tags (the
    roll trigger compares against the same level), and cursors with their
    readout, on the roll point rate (`view_time_axis()`: one point per
    column, time counted back from the newest point; 100 columns at
    50 ms/div read 200 ms). The ground tags and the cursors' absolute volts
    use the source's 0 V code (`chan_zero_code` in roll, the record's own
    otherwise). Not in roll, by the source: the trigger pointer and the mask
    (no placement on a trigger), persistence (roll moves every point along
    the screen columns the ghosts are kept in), math (its inputs are the
    record's own scale; to do). Roll is out of `view.mode` (2026-11-24): the
    views are yt, xy, fft, split and waterfall, and roll is only the
    time-base property `tb.roll` (with `tb.rollstep`), a source the views
    that take it draw (`view_desc_t.roll` in `view_table.h`: Y-T only).
    `tb.roll` on is refused in another view, a change to another view
    switches it off first, and the timebase ladder stops at the last FPGA
    code there. The Display menu has no Roll row and `main.c` no roll
    renderer. `view.mode` took a new id (0x0500; 0x0501 is released, so a
    snapshot written before is read without its view). The settings record's
    byte 23 keeps its encoding (0 yt, 1 yt with roll, 2 xy, 3 fft, 4 split,
    5 waterfall) as the pair (`view.mode`, `tb.roll`), so roll still
    comes back across a power cycle and an older firmware reads the same
    byte; `tb.roll` is saved in the settings record and in the session
    snapshot. (c) Split as a pair of views in two panes.
