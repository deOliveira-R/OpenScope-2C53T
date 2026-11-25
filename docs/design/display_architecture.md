# Display architecture

Status: agreed design (2026-11-20). Steps 1 to 5 of the order of work are
done (the canvas, and Y-T, roll and the spectrum as display types on it), and
the view table of step 8 exists (`firmware/src/ui/view_table.h`). Step 7's
port is done: X-Y and both halves of split draw through canvases. Since step
10 (c) split is a pair of views in two panes, each drawn by its view's own
drawer: any two of Y-T, X-Y and the spectrum. The waterfall still has its own renderer.

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
is a pair of views (`view.upper`, `view.lower`) and shows the domains of
both: its entry is composed from theirs (`view_desc_of`), its job list the
upper half's jobs then the lower half's, each job in the half of its domain.
The job the arrows are on says which half the keys, AUTO, the strip and the
cursor readout belong to. That half is drawn in a frame (the focus mark,
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

**Base and overlays.** A display type may split its elements into a base
(what holds still: background, graticule, focus mark) and overlays (what
moves with the record: traces, trigger line, mask bounds, tags, cursors).
The canvas paints the base, keeps a copy and its hash, paints the overlays
over it on the same strip, and notes the rows where the strip differs from
the base (at most four runs a column; more are joined across the smallest
gap). A changed column whose base is the one on the glass sends only the
rows the overlays left and the rows they arrived on, joined where the gap is
shorter than a window's set-up (11 bus writes). A changed base, `all`, a
hole that changed, another painter on the rows, or a last frame without a
base sends the whole column. Y-T (and so roll and split's Y-T half) uses it;
with persistence on the ghosts change the base every frame, so its columns
go whole as before. `ui dump` counts the pixels sent.

The partial send is a build feature, `canvasruns` (`firmware/features.mk`),
on in every image. Its core is one file, `firmware/src/ui/canvas_runs.c`: the
copy of the base strip, the base hash and runs of every column of each
canvas, and the windows a changed column sends. `canvas.c` calls it through
`canvas_runs.h` with no `#if`; everything else (the base and the overlays,
the strip hash, skipping unchanged columns, holes, the trust rule) is the
same in both builds. It costs 8,216 B of RAM and 896 B of flash (`make features-ab
FEATURE=canvasruns`, 2026-11-25, bench image). Built
without it (`make WITHOUT=canvasruns`), a changed column is sent whole: the
picture is the same, the plot's send is slower (a moving trace sends its
whole columns instead of the overlays' rows).

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
   done as a frame painted through the canvas, with the popup box as a hole.
   Its axes are an interface (`fft_axes_t`,
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
   title too) from the trail; both keep the popup box as a hole. The strips
   and cursors of split's halves came with step 10 (c). Open: the strip and
   cursors of X-Y, which change the picture.
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
    from it. Cursors are kept in the units of what they mark (Y-T times in
    0.4 ns ticks from the trigger point, `view.curt1`/`2`; roll's in ms back
    from the newest point, `view.curr1`/`2`; levels in microvolts at the
    active channel's probe tip, `view.cury1`/`2`) and are put on the pane
    they are drawn in through its time axis and the active channel's map as
    drawn there, so a line stays on its time and its level when the time
    base, the range or the position moves. Checked at
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
    50 ms/div read 200 ms). The ground tags use the source's 0 V code
    (`chan_zero_code` in roll, the record's own otherwise), and the level
    cursors the 0 V of the codes their band draws. Not in roll, by the source: the trigger pointer and the mask
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
    snapshot. (c) Split as a pair of views in two panes: implemented for Y-T
    and the spectrum, checked on the unit (both pairs, the keys on each half,
    cursors on; livecheck 0 over both halves; full Y-T, roll, the spectrum,
    X-Y and the waterfall unchanged otherwise). The user picks
    the two views on the Display pane ("Upper half", "Lower half"; properties
    `view.upper`, `view.lower`, session settings, defaults Y-T over FFT). Which
    views can be a half is a column of the view table (`view_desc_t.half`: a
    drawer for a pane of any height, listed in `firmware/src/ui/split_halves.h`,
    which a host test holds against the table); the waterfall cannot be one
    (its scratch line shares memory with the Y-T window, and its history is
    the memory an X-Y half's trail takes). The two halves
    never show the same view: a view keeps one set of state for what it drew
    (the bands the cursors read, the trigger placement, the window, the popup
    hole), so a half set to the other half's view swaps them, and either pair
    is one set away. The band from `SCOPE_TOP` to the top of the taller of the
    two halves' strips (`view_band_bot`), whichever half has the keys, is
    split into two panes with a divider row between them
    (`plot_pane_split`; with the results hidden and the cursors off, 103 and
    102 rows of a 206-row band, a division of 13 rows each), on a canvas per half (`CANVAS_SPLIT_UPPER`; the lower half
    shares the full plot's memory, `CANVAS_SPLIT_LOWER` = `CANVAS_PLOT`, since
    they are never on screen together: no third watch slot or 1.3 kB hash
    table). Each half is drawn by its view's pane drawer (`scope_yt_draw`,
    `fft_pane_draw`, `firmware/src/ui/scope_split.c`) with its own cursor
    lines; the keys' half wears the focus mark and owns its domain's strip and,
    with cursors on, their readout, at the bottom of the reserved space (the
    rest of it is background). Split has its own results setting,
    `view.splithide` (0x0519, session, hidden by default): long PRM in split
    toggles it, and `view.meashide` stays the full views' (the view table says
    which one a view uses, `view_desc_t.results`). With the results hidden the
    cursor readout row stays while the cursors are on, so the band ends one
    strip row (13) above the info bar: 96 and 96 rows, a division of 12. A mask
    or the CAN line keeps the time strip's AUX row too, so then the band ends
    there whichever half has the keys. The divider row is in neither canvas: it is
    painted with every frame, around the popup box while a pane keeps a hole
    for it. Moving the keys to the other half is a layout change: a full
    frame of the panes at the size they had (the focus mark crosses every
    column), with the strip and the readout of the other domain; the panes'
    heights do not move. `main.c` runs
    the Y-T furniture (strip refresh, cursor row, mask slot, CAN row) when the
    view's strip is the time record's (`view_strip_domain`), in split when the
    keys are on the Y-T half. `fpga scope livecheck` is to check both halves
    against their views' rules (the focus mark is in the Y-T rule) and counts
    one frame per screen frame (X-Y and waterfall frames count with no pixel
    compared). Split still does not take roll (`view_desc_t.roll`), though a
    Y-T half could. With the results shown and the default selection (two strip
    rows per channel, 52 rows; the spectrum's one row) the halves are 77 and 76
    rows, a division of 10, the keys on either half (70 and 70, a division of
    9, with the cursors on);
    the spectrum draws its scale on any pane of `FFT_SCALE_LINES` font lines
    (48 rows) or more, where it took a fixed 81 before (which a half under the
    strip would have lost).
    (d) X-Y as a half (implemented 2026-11-25, to be checked on the unit).
    One drawer for a pane of any height (`xy_pane_draw` in
    `firmware/src/ui/scope_xy.c`): a square as tall as the pane, centred,
    the graticule at the pane's division (26 rows full, 13 on a half with
    the results hidden), the title in the left margin (a line that does not
    fit the pane is left out), the focus mark under the trail when the half
    has the keys. Full X-Y is that drawer on the whole band with its trail in
    the persistence buffer, as before. A half's trail cannot be on the heap
    (31,744 bytes) or in the persistence buffer (claiming it would take the
    pool from split's spectrum): it is the FFT pool's slot
    (`SHMEM_FFT_SLOT_OFFSET`, 20,480 bytes above the FFT layout), a slot with
    tenants (the waterfall's history, the X-Y half's trail) that are never on
    screen together; a tenant takes it every frame and starts it afresh when
    the take reports that the bytes are not its own (another tenant, another
    pool owner, or its hold released). The tallest half is split's upper
    pane on the whole band, 103 rows: 103 x 103 = 10,609 bytes, checked at
    build time. The half's trail starts afresh when the half enters split
    and when its pane's height changes (the strips shown or hidden, the
    cursors on or off). Both trails fade on wall time (`persist_fade`, the persistence
    fade over any buffer with its own remainder) at the 500 ms step X-Y
    enters with, so the tail is as long in a half as full; full X-Y still
    follows `view.persist` while persistence is on in it. Full X-Y used to
    fade 16 steps a frame: about 0.8 s at 20 frames a second running, 3.2 s
    at the 5 a second of the animation when stopped. Entering a half does X-Y's bring-up that is not
    about the trail (the offset outputs without the calibration page,
    `scope_view_half_enter`), on the input task, from the one path both
    ways a half arrives share (`view.mode` to split, and `view.upper` /
    `view.lower` while split is on). The keys: a job is on the half that has
    it (the job listed and its domain shown); with Y-T and X-Y both halves of
    the time record, the zoom, channel and trigger moves are on the upper
    half, the cursor and mask jobs on Y-T's. The strip is the keys' domain's
    when a half draws that domain with a strip (`view_strip_domains`): X-Y
    has none, so beside Y-T the time strip is Y-T's whichever time half has
    the keys, and beside the spectrum the time jobs show no strip and the
    band reserves only the spectrum's (panes of 96 rows with the results
    shown and the cursors off). The keys' half is in the layout epoch with
    the pair, so moving the keys between two time halves moves the focus
    mark. Split animates when a half's view does (an X-Y half's trail fades
    without new data).
