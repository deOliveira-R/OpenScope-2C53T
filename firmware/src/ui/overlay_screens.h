/*
 * OpenScope 2C53T - the small layers as screens of the router (screen.h;
 * ARCH-1 B5): the POWER countdown prompt and the logo
 *
 * Both used to be painted by the task that wanted them (the input task's
 * POWER loop and auto power-off, the shell's `logo show`) under a global
 * "nobody else draw" flag. They are layers now: the task that wants one only
 * says what is to be shown (a few bytes of state), and the display task
 * paints it and, when it goes, what was under it.
 *
 * THE POWER PROMPT (SCR_POWER_PROMPT). The input task keeps everything that
 * decides: the POWER pin poll, the three-second count, the settings flush
 * and the power-off itself are where they were (input_handler.c); only the
 * drawing moved. The prompt is a box over the screen as it stands, which is
 * frozen under it. It has no keys: POWER is never a layer's key (the router
 * gives it to the globals), and no other key is routed while the input task
 * counts.
 *
 * THE LOGO (SCR_LOGO). At boot main() still paints it before the scheduler
 * runs, and the key task holds it ~1 s (any key skips). `logo show` raises
 * the same layer from the shell.
 *
 * Both are OPAQUE | NO_BARS: nothing under them is drawn or ticked, the
 * bars included. Each is up while its layer is on the router's stack: show
 * pushes it and hide closes it, on the input task (the shell's `logo show`
 * asks the input task: logo_ui_preview).
 *
 * Pure: no FreeRTOS, no driver header. Host test: make test-overlay-screens.
 */
#ifndef OVERLAY_SCREENS_H
#define OVERLAY_SCREENS_H

#include <stdbool.h>
#include <stdint.h>
#include "screen.h"

/* ── the POWER prompt ──────────────────────────────────────────────── */

typedef enum {
    POWER_PROMPT_NONE = 0,
    POWER_PROMPT_HOLD,       /* "Hold to power off" and the count 3..1 */
    POWER_PROMPT_GOODBYE,    /* "Goodbye!" (POWER held to the end)     */
    POWER_PROMPT_AUTOOFF,    /* "Auto power off" / "Goodbye!"          */
    POWER_PROMPT_UPDATE,     /* "Firmware update" / "Release POWER"    */
} power_prompt_kind_t;

typedef struct {
    uint8_t kind;            /* power_prompt_kind_t */
    uint8_t count;           /* HOLD: the digit shown */
    uint8_t shows;           /* bumped by every show: each one is painted */
} power_prompt_view_t;

extern const screen_t power_prompt_screen;

/* Input task. show says what the box shows and pushes its layer if it is
 * not up; hide closes the layer. Each show is painted once by the display
 * task (the box is repainted every second of the count, so a stray frame
 * heals). Neither waits or posts anything. */
void power_prompt_show(power_prompt_kind_t kind, uint8_t count);
void power_prompt_hide(void);
bool power_prompt_up(void);                          /* any task: its layer is on the stack */
power_prompt_view_t power_prompt_view(void);         /* any task */

/* The paint (overlay_ui.c; a fake in the host test). Display task only. */
void power_prompt_ui_paint(power_prompt_view_t v);

/* ── the logo ──────────────────────────────────────────────────────── */

extern const screen_t logo_screen;

/* Input task: push / close the layer. painted = it is on the LCD already
 * (main() drew it before the scheduler): the display task does not paint it
 * again. false: it is up already, or the stack is full. */
bool    logo_screen_show(bool painted);
void    logo_screen_hide(void);
bool    logo_screen_up(void);                        /* any task: its layer is on the stack */
uint8_t logo_screen_shows(void);                     /* bumped by every show */
bool    logo_screen_painted(void);                   /* this show is on the LCD */
/* Display task: this show has been painted (logo_ui.c reports its result). */
void    logo_screen_mark_painted(void);

/* The paint (logo_ui.c; a fake in the host test). Display task only: the
 * stored logo, full screen. */
void logo_ui_paint(void);

/* ── the display task's bookkeeping of the layers that own the LCD ──── */

/* The POWER prompt, the logo and the gallery own the LCD while they are up
 * (main.c modal_layer). This is what has to be repainted as they come and
 * go, as a pure step so the rule is host-tested:
 *   - a layer that follows another one is painted whole (the box over the
 *     gallery's last picture; the logo again when the box goes);
 *   - when the POWER prompt goes, what its box covered is repainted by a
 *     RESTORE paint (no clear, as under an expired popup) of the screen
 *     under it: the page if one is up, else the base (main.c); the full
 *     redraw only if there is no screen to ask;
 *   - when a layer that covered the whole LCD goes, the full redraw; it
 *     also wins over a restore that is pending with it.
 * Nothing is repainted while a layer is up. */
typedef struct {
    const screen_t *was;          /* the layer of the last step (NULL: none) */
    bool            redraw;       /* a full redraw is owed                   */
    bool            restore;      /* a restore of the box's area is owed     */
    bool            whole;        /* the layer up now is owed a whole paint  */
} layer_track_t;

typedef enum { LAYER_REPAINT_NONE = 0, LAYER_REPAINT_RESTORE, LAYER_REPAINT_ALL } layer_repaint_t;

/* Once per frame, with the layer up now (NULL: none). true: the layer
 * changed (the display task's paint gate starts over). */
bool layer_track_step(layer_track_t *t, const screen_t *now);
/* With a layer up: is it owed a whole paint (FULL)? Taken once. */
bool layer_track_take_whole(layer_track_t *t);
/* With no layer up: what is owed, taken once. base_restores: there is a
 * screen to repaint under the box without a clear (every screen can; false
 * only before the router is up). */
layer_repaint_t layer_track_take(layer_track_t *t, bool base_restores);
/* A full redraw was made (DCMD_REDRAW_ALL): nothing is owed any more. */
void layer_track_redrawn(layer_track_t *t);

#endif /* OVERLAY_SCREENS_H */
