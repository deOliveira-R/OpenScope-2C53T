/*
 * OpenScope 2C53T - top status bar and bottom info bar (UI round 1, R1-B)
 *
 * Top bar, y 0..15, every mode (UI_STUDY.md 2.1 S1-S4, S8, S11, B1):
 *
 *   x 0   23 26              113 116    157 160    197 200 208 211    262 264     318
 *   [gen icon] [H=100ns          ] [RUN tab ] [focus   ] [dot] [view word] [batt  94%]
 *
 *   gen icon   the generator's current waveform (S8), on a green tab while the
 *              output is on, grey on the bar when it is off. Every mode.
 *   H=         the timebase the screen shows; while a held record is viewed
 *              at another timebase (zoom while paused, C14) it turns amber
 *              with the zoom: "H=50ns x2" / "H=1us /5" (2.4).
 *   RUN tab    sheared tab: RUN green, STOP red, ARM amber (SINGLE armed).
 *   focus      what the arrows adjust, stock's glyph: "V H" (V in the active
 *              channel's colour), "> v" channel position (channel colour),
 *              "< v" trigger (trigger green), "X1 Y1" cursors, "M" mask.
 *   dot        trigger state: green triggered, amber Auto free-run, grey
 *              waiting, armed or stopped (S11).
 *   view word  the scope view (Y-T / ROLL / X-Y / FFT / SPLIT / WFALL), or
 *              the title set with status_bar_set_title().
 *   battery    22x10 glyph with 4 bars + "%" (B1); bolt while charging;
 *              red blinking + "LOW" when critical.
 *
 *   Other modes: [gen icon] [ title centred in x 26..263 ] [battery].
 *
 * Bottom bar, y 224..239 (scope time view; S5, S7):
 *
 *   x 1                 102 104                          215 217                318
 *   [1] 1.37V X1 DC         (T) Auto [edge] CH1 +20          [2] 627mV X10 AC
 *
 *   CH badges: a channel chip with its number, then volts/div (measured,
 *   scope_cal), probe factor and coupling, 3 px apart, in the channel colour
 *   (grey when the channel is off). CH2 is right-aligned at x 318. A badge
 *   that would not fit (only GND coupling with the widest label, and GND is
 *   not reachable from the keys today) drops its chip rather than overlap.
 *   Trigger badge, centred: green T disc, mode, the edge glyph and the source
 *   in the source's colour, and the level when it is not 0, in the
 *   firmware's own units (-100..100, the "Trig +20" popup's number; it is an
 *   offset from the signal's midline, NOT volts, so no unit is printed).
 *   The FFT view keeps its own bar; the generator has none (G1).
 */
#ifndef STATUS_BAR_H
#define STATUS_BAR_H

#include <stdint.h>

/* ── Geometry (tests/test_icons.c checks it fits and does not overlap) ── */
#define SB_H            16u     /* both bars */
#define SB_TEXT_Y       2u      /* font_small inside the top bar */
#define IB_Y            (240u - SB_H)
#define IB_TEXT_Y       (IB_Y + 2u)

#define SB_GEN_X        0u
#define SB_GEN_W        24u
#define SB_TB_X         26u
#define SB_TB_W         88u
#define SB_RUN_X        116u
#define SB_RUN_W        42u
#define SB_FOCUS_X      160u
#define SB_FOCUS_W      38u
#define SB_DOT_X        200u
#define SB_DOT_W        9u
#define SB_VIEW_X       211u
#define SB_VIEW_W       52u
#define SB_BATT_X       264u
#define SB_BATT_W       56u     /* glyph 22 + gap + "100%", right-aligned at x 318 */
#define SB_TITLE_X      SB_TB_X                          /* non-scope modes */
#define SB_TITLE_W      (SB_VIEW_X + SB_VIEW_W - SB_TB_X)

#define IB_CH1_X        1u
#define IB_CH_W         102u
#define IB_TRIG_X       104u
#define IB_TRIG_W       112u
#define IB_CH2_X        217u
#define IB_CHIP_W       9u      /* the channel-number chip */
#define IB_CHIP_GAP     3u
#define IB_FIELD_GAP    3u      /* between volts/div, probe and coupling (a space is 5) */

#define SB_TITLE_MAX    24u     /* bytes kept by status_bar_set_title, incl. NUL */

/* Draw / invalidate (also declared in ui.h, where main.c finds them). */
void draw_status_bar(void);
void status_bar_invalidate(void);
void draw_info_bar(void);

/* The title for the CURRENT mode (copied, at most SB_TITLE_MAX-1 chars): the
 * meter's function name, a page name. It applies only while the mode it was
 * set in stays current; NULL or "" goes back to the default (the mode's name;
 * the scope view word in the scope). Repaints on the next draw_status_bar. */
void status_bar_set_title(const char *title);

/* Hash of everything draw_info_bar() prints in the scope time view (more than
 * main.c's info_bar_epoch hashes today: the trigger level and the measured
 * labels). draw_status_bar() repaints the info bar when it changes, so a
 * shell-driven change shows within a second; main.c may poll it per tick. */
uint32_t status_bar_info_epoch(void);

/* What the RUN/STOP/ARM tab and the trigger dot show (fpga_trigger_status()
 * and the run state), as one key. main.c's display loop calls
 * draw_status_bar() when it changes, so they follow at the loop's 50 ms, not
 * the 1 s timer's pace; the bar repaints only the slots that changed. */
uint32_t status_bar_live_key(void);

/* Once a second, from main.c's 1 s timer: counts the seconds of critical
 * battery. draw_status_bar() (display task: the LCD is its alone) shows the
 * shutdown and powers off at 30, so drawing the bar more often than once a
 * second does not shorten the countdown. */
void status_bar_second_tick(void);

#endif /* STATUS_BAR_H */
