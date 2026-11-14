/*
 * OpenScope 2C53T - the signal generator screen. See siggen_screen.h.
 */
#include "siggen_screen.h"
#include "siggen_edit.h"
#include "siggen_wave.h"     /* siggen_wave_uses_duty (pure) */
#include "model.h"
#include "theme.h"

#include <stdio.h>

/* UI state: the frequency editor's cursor and undo value (siggen_edit.h).
 * Written by the input task (key, open, close); the display task and the
 * shell's `siggen` status read it. */
static siggen_edit_t s_ed;

siggen_edit_t *siggen_ui_editor(void)
{
    return &s_ed;
}

/* ── input task ────────────────────────────────────────────────────── */

/* Leaving the generator with the editor open keeps the value (it is live
 * already) and drops the editor; it is closed on entry too. */
static void sg_open(screen_id_t from)  { (void)from; s_ed.active = false; }
static void sg_close(screen_id_t to)   { (void)to;   s_ed.active = false; }

static void sg_step(const prop_desc_t *p, int dir)
{
    (void)prop_step_local(p, dir, ORIGIN_KEY, NULL);
}

/* The frequency digit editor (parity G4). Closed: PRM opens it on the live
 * frequency. Open: the arrows move the digit cursor / change the digit, OK
 * or PRM accept, MENU undoes; every change is committed to gen.freq at once,
 * so the output follows the edit as in stock. false: not the editor's key. */
static bool sg_editor_key(button_id_t b)
{
    siggen_edit_key_t k;
    if (!s_ed.active) {
        if (b != BTN_PRM) return false;
        siggen_edit_begin(&s_ed, (uint32_t)prop_get(P(GEN_FREQ)));
        return true;
    }
    switch (b) {
    case BTN_LEFT:  k = SGE_KEY_LEFT;   break;
    case BTN_RIGHT: k = SGE_KEY_RIGHT;  break;
    case BTN_UP:    k = SGE_KEY_UP;     break;
    case BTN_DOWN:  k = SGE_KEY_DOWN;   break;
    case BTN_OK:
    case BTN_PRM:   k = SGE_KEY_ACCEPT; break;
    case BTN_MENU:  k = SGE_KEY_CANCEL; break;
    default:        return false;
    }
    /* Step from the live value: a `set gen.freq` may have moved it. */
    siggen_edit_track(&s_ed, (uint32_t)prop_get(P(GEN_FREQ)));
    const siggen_edit_result_t r = siggen_edit_key(&s_ed, k);
    if (r == SGE_CHANGED || r == SGE_CANCELLED)
        (void)prop_set_local(P(GEN_FREQ), (int32_t)s_ed.value, ORIGIN_KEY, NULL);
    return true;
}

static scr_res_t sg_key(scr_key_t k)
{
    if (k.is_long) {
        /* An open frequency edit is not interrupted: its value is live. */
        switch (k.btn) {
        case BTN_OK:   sg_step(P(GEN_OUT), +1);     return SCR_DRAW;   /* Run/Stop */
        case BTN_AUTO: sg_step(P(GEN_BOOTOUT), +1); return SCR_DRAW;   /* output at boot */
        default:       return SCR_HANDLED;
        }
    }

    if (sg_editor_key(k.btn)) return SCR_DRAW;

    switch (k.btn) {
    case BTN_MENU:
        /* Short MENU at the generator's top level opens Home (owner
         * decision D2; stock's BACK does nothing here). */
        (void)prop_set_local(P(SYS_MODE), (int32_t)MODE_HOME, ORIGIN_KEY, NULL);
        return SCR_HANDLED;                     /* the mode switch repaints */
    case BTN_SELECT:
        sg_step(P(GEN_WAVE), +1);
        return SCR_DRAW;
    case BTN_UP:
        sg_step(P(GEN_AMP), +1);
        return SCR_DRAW;
    case BTN_DOWN:
        sg_step(P(GEN_AMP), -1);
        return SCR_DRAW;
    case BTN_LEFT:
    case BTN_RIGHT:
        /* Duty is editable for Square and Sawtooth only (stock 0x0800A478). */
        if (siggen_wave_uses_duty((uint8_t)prop_get(P(GEN_WAVE))))
            sg_step(P(GEN_DUTY), k.btn == BTN_RIGHT ? +1 : -1);
        return SCR_DRAW;
    case BTN_OK:
        sg_step(P(GEN_OUT), +1);
        return SCR_DRAW;
    case BTN_CH1:
    case BTN_CH2:
    case BTN_TRIGGER:
    case BTN_MOVE:
        return SCR_DRAW_FULL;                   /* no meaning here: they only repaint, as before */
    default:
        return SCR_HANDLED;                     /* AUTO: nothing */
    }
}

/* ── display task ──────────────────────────────────────────────────── */

/* The page has no partial paint: FULL and UPDATE are the same picture. An
 * UPDATE is gated on epoch() by the display task, so a key that changed
 * nothing (a clamp) costs no LCD write. */
static void sg_draw(scr_paint_t how, uint32_t frame)
{
    (void)how; (void)frame;
    siggen_ui_draw();
}

/* Hash of everything siggen_ui_draw() renders: the generator's values
 * (whoever set them: a key, `set gen.*`, the old `siggen` command), the
 * editor's cursor and the theme. */
static uint32_t sg_epoch(void)
{
    uint32_t h = REDRAW_EPOCH_SEED;
    h = redraw_epoch_mix(h, (uint32_t)theme_get_id());
    h = redraw_epoch_mix(h, (uint32_t)prop_get(P(GEN_WAVE))
                            | ((uint32_t)prop_get(P(GEN_DUTY)) << 8)
                            | ((uint32_t)prop_get(P(GEN_OUT)) << 16)
                            | ((uint32_t)prop_get(P(GEN_AMP)) << 24));
    h = redraw_epoch_mix(h, (uint32_t)prop_get(P(GEN_FREQ)));
    /* Editor state and boot policy: a cursor move changes neither the
     * frequency nor the waveform, so without these it would not repaint. */
    h = redraw_epoch_mix(h, (uint32_t)s_ed.active | ((uint32_t)s_ed.digit << 1)
                            | ((uint32_t)prop_get(P(GEN_BOOTOUT)) << 8));
    return h;
}

/* ── any task ──────────────────────────────────────────────────────── */

/* `ui screen`: "edit=off", or "edit=<digit>" (0 = 1 Hz .. 4 = 10 kHz) while
 * the frequency editor is open. */
static void sg_describe(char *out, size_t n)
{
    if (s_ed.active) snprintf(out, n, "edit=%u", (unsigned)s_ed.digit);
    else             snprintf(out, n, "edit=off");
}

/* The page shows the generator; the bars add sys and ui (theme, mode). */
#define SG_GROUPS ((uint16_t)((1u << PG_GEN) | (1u << PG_SYS) | (1u << PG_UI)))

const screen_t siggen_screen = {
    .name = "siggen", .id = SCR_SIGGEN, .mode = MODE_SIGNAL_GEN, .instrument = MODE_SIGNAL_GEN,
    .flags = 0,
    .open = sg_open, .close = sg_close, .key = sg_key,
    .draw = sg_draw, .tick = NULL, .epoch = sg_epoch,
    .describe = sg_describe, .groups = SG_GROUPS,
};
