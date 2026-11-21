/*
 * OpenScope 2C53T - the oscilloscope as a screen (ARCH-1 B7)
 *
 * The scope's base screen of the router (screen.h).
 * The Oscilloscope Settings page stays a layer of its own over it
 * (osc_menu_screen.h).
 *
 * KEYS (input task). key_model.c says what a key means in the scope
 * (key_map: view, arrow stage, cursors, mask); this file executes every
 * answer, and every change of scope state is ONE property set
 * (prop_set_local / prop_step_local, ORIGIN_KEY), the function `set` on the
 * direct route ends in. Nothing here calls scope_apply.c or a driver:
 *
 *   key (stage / view)            action              property
 *   UP/DOWN   (zoom)              KA_VDIV_STEP        chN.vdiv of the active channel
 *   LEFT/RIGHT (zoom)             KA_TB_STEP          tb.code, past its end tb.roll / tb.rollstep
 *   UP/DOWN   (channel move)      KA_CHPOS_STEP       chN.pos
 *   LEFT/RIGHT (channel / trigger move)  KA_TRIGCOL_STEP  trig.x
 *   UP/DOWN   (trigger move)      KA_TRIGLVL_STEP     trig.level
 *   UP/DOWN   (cursor)            KA_CURSOR_MOVE      view.curv1 / curv2 / curh1 / curh2 (the selected one)
 *   LEFT/RIGHT (cursor)           KA_CURSOR_NEXT      view.cursel
 *   UP/DOWN, LEFT/RIGHT (mask)    KA_MASK_TOL_V / _H  mask.tolv / mask.tolh
 *   MOVE      (mask)              KA_MASK_STOPFAIL    mask.stopfail
 *   CH1 / CH2                     KA_COUPLING_TOGGLE  chN.coupling (and the active channel)
 *   long CH1 / CH2                KA_CH_ENABLE_TOGGLE chN.enabled
 *   long PRM                      KA_MEAS_HIDE_TOGGLE view.meashide
 *   OK                            KA_RUNSTOP          view.run (the setter latches at STOP and
 *                                                     re-arms SINGLE)
 *
 * An arrow at the end of its range still sets the value in force, as the
 * key always re-applied it (the setter runs for a set of the same value).
 *
 * UI state, not settings (model.h 2.9: screens own it; ui.focus / ui.ach
 * report it read-only): the arrow stage (SELECT), the active channel (MOVE,
 * CH1/CH2), opening the Oscilloscope Settings page (MENU, PRM, TRIGGER).
 *
 * Procedures: one named request each, the one the shell's row makes too:
 *   AUTO (time record) scope_selfcal_request_autoset()        `fpga scope autoset`
 *   long AUTO          scope_selfcal_request_basecal(0x03)    `fpga scope basecal`
 *   long OK            scope_selfcal_request_center50()       `fpga scope center50`
 *   AUTO, a mask or its stage up   scope_mask_key_auto()      `mask teach` / `mask clear`
 *   OK, a mask failure held        SCOPE_MASK_REQ_RELEASE     `mask run`
 *   AUTO (spectrum)                fft_auto_request()         `fft auto`
 *   OK in SINGLE, stopped or its record taken: `set view.run on` (re-arm)
 *
 * State with no property: the spectrum's reference level and span (the
 * arrows in the spectrum job). Its window, averaging and max hold are the
 * fft.* properties (the settings page's Spectrum pane).
 *
 * DISPLAY (display task). draw / tick / epoch are the scope's paint module
 * (scope_view_*, main.c in the firmware): the full and live frames, the
 * heartbeat and the redraw gate, behind the screen's hooks. scope_screen_epochs() says what changed for it: see there.
 *
 * `ui screen`: no describe() of its own; focus=, ach=, run=, view= are the
 * properties ui.focus, ui.ach, view.run, view.mode, as before.
 */
#ifndef SCOPE_SCREEN_H
#define SCOPE_SCREEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "screen.h"
#include "key_model.h"
#include "redraw_gate.h"

extern const screen_t scope_screen;

/* The job the arrows were last put on (view_table.h KF_*); the scope screen
 * is its one writer. Readers want key_focus_get(), the job in force. */
extern volatile key_focus_t scope_key_focus;
void scope_screen_view_changed(void);    /* the arrows to the new view's first job */

/* The scope's key context as it stands (cursors, mask, view, page), mode
 * MODE_OSCILLOSCOPE. Input task. It also drops an arrow stage that no
 * longer exists (the mask cleared or the cursors switched off from the
 * shell) before a key can be steered by it. */
key_ctx_t scope_screen_ctx(void);

/* What the scope shows, hashed in two parts, for the display task:
 *   *layout  what moves or resizes something between the bars (channel on /
 *            off, the cursor mode, the measurement rows, math, persistence,
 *            the view, roll, RUN/STOP, the theme, the FFT configuration):
 *            a change repaints the plot area once;
 *   *soft    values the per-column live frame, the measurement strip's
 *            in-place refresh and the bars' own epochs repaint by
 *            themselves (ranges, positions, coupling, probe, the timebase
 *            code, the trigger settings, the active channel): a change
 *            costs one live frame, no clear.
 * Cursor positions and the selection are in neither: the live frame draws
 * them (scope_ui_request_live). */
void scope_screen_epochs(uint32_t *layout, uint32_t *soft);
/* The cursor lines' own hash: a change wants one live frame. */
uint32_t scope_screen_cursor_epoch(void);

/* What the scope shows now (the hashes above and the kind of view: Y-T,
 * roll, X-Y, FFT, split, waterfall), and what the display task does about
 * the difference to what it last painted:
 *   bar     the top bar's slots (the timebase, RUN): repaint the changed ones;
 *   whole   another kind of view: the whole screen once (as a key's change);
 *   layout  the layout part moved: the redraw gate repaints the view in full
 *           (its epoch is `layout`);
 *   live    a soft change in the Y-T view, or a cursor line: one live frame
 *           (and the strip's cells at once when `strip`);
 *   force   a soft change in another view: the view in full.
 * `primed`: the scope was painted since it was entered (else nothing is
 * known to differ: the entry paints it). Pure; display task. */
typedef struct { uint32_t layout, soft, cursor; uint8_t kind; } scope_shown_t;
typedef struct { bool bar, whole, layout, live, strip, force; } scope_change_t;
void           scope_screen_shown(scope_shown_t *now);
scope_change_t scope_screen_changed(const scope_shown_t *seen, const scope_shown_t *now,
                                    bool primed);

/* A `set` from the direct route changed `groups` (bit = PG_x) while the
 * scope is the top screen: true when every one of them is the scope's own
 * (ch1, ch2, tb, trig, view, fft, il, mask), i.e. the screen repaints the
 * change from its epochs without a clear. A change of the bars' other
 * groups (the generator slot, sys: the theme) still repaints everything. */
bool scope_screen_paints(uint16_t groups);

/* ── environment: the paint module (main.c; a fake in the host test) ── */
void            scope_view_draw(scr_paint_t how, uint32_t frame);
redraw_action_t scope_view_tick(uint32_t frame);
uint32_t        scope_view_fft_epoch(void);     /* the live FFT configuration, 0 without one */

/* ── environment: the procedures (their owners; fakes in the host test) ── */
bool scope_selfcal_request_autoset(void);
bool scope_selfcal_request_basecal(uint8_t banks);
bool scope_selfcal_request_center50(void);

#endif /* SCOPE_SCREEN_H */
