/*
 * OpenScope 2C53T - key model. See key_model.h for the why.
 */

#include "key_model.h"

void key_scan_init(key_scan_t *s)
{
    for (int i = 0; i < KEY_NUM; i++) { s->held[i] = 0; s->rep[i] = 0; }
}

uint8_t key_scan_tick(key_scan_t *s, uint16_t raw, key_scan_event_t out[KEY_NUM])
{
    uint8_t n = 0;

    for (int i = 0; i < KEY_NUM; i++) {
        const uint16_t bit = (uint16_t)(1u << i);
        const bool has_long = (KEY_LONG_MASK & bit) != 0u;

        if (raw & bit) {
            if (s->held[i] < 0xFFFFu) s->held[i]++;

            if (has_long) {
                /* Long once, at the threshold. The short is held back until
                 * release (below), so exactly one of the two ever fires. */
                if (s->held[i] == KEY_LONG_TICKS) {
                    out[n].bit = (uint8_t)i; out[n].is_long = true; n++;
                }
            } else if (s->held[i] == KEY_CONFIRM_TICKS) {
                out[n].bit = (uint8_t)i; out[n].is_long = false; n++;
                s->rep[i] = 0;
            } else if (s->held[i] > KEY_CONFIRM_TICKS && (KEY_REPEAT_MASK & bit)) {
                if (s->rep[i] < 0xFFFEu) s->rep[i]++;
                if (s->rep[i] == KEY_REPEAT_INITIAL ||
                    (s->rep[i] > KEY_REPEAT_INITIAL &&
                     (s->rep[i] - KEY_REPEAT_INITIAL) % KEY_REPEAT_INTERVAL == 0)) {
                    out[n].bit = (uint8_t)i; out[n].is_long = false; n++;
                }
            }
        } else {
            /* Released. A long-capable key that was held past the confirm but
             * not to the long threshold is a short press, delivered now. A
             * shorter contact is a bounce, as for every key. */
            if (has_long && s->held[i] >= KEY_CONFIRM_TICKS &&
                s->held[i] < KEY_LONG_TICKS) {
                out[n].bit = (uint8_t)i; out[n].is_long = false; n++;
            }
            s->held[i] = 0;
            s->rep[i] = 0;
        }
    }
    return n;
}

/* ── Arrow stage ──────────────────────────────────────────────────── */

key_focus_t key_focus_sanitize(key_focus_t cur, bool cursors_on, bool mask_present)
{
    if (cur == KF_CURSOR && !cursors_on)  return KF_ZOOM;
    if (cur == KF_MASK && !mask_present)  return KF_ZOOM;
    if ((unsigned)cur >= (unsigned)KF_COUNT) return KF_ZOOM;
    return cur;
}

key_focus_t key_focus_next(key_focus_t cur, bool cursors_on, bool mask_present)
{
    cur = key_focus_sanitize(cur, cursors_on, mask_present);
    switch (cur) {
    case KF_ZOOM:     return KF_CHMOVE;
    case KF_CHMOVE:   return KF_TRIGMOVE;
    case KF_TRIGMOVE: return cursors_on ? KF_CURSOR : (mask_present ? KF_MASK : KF_ZOOM);
    case KF_CURSOR:   return mask_present ? KF_MASK : KF_ZOOM;
    case KF_MASK:
    default:          return KF_ZOOM;
    }
}

key_focus_t key_focus_after_cursor_change(key_focus_t cur, bool cursors_on)
{
    if (cursors_on) return KF_CURSOR;
    return (cur == KF_CURSOR) ? KF_ZOOM : cur;
}

const char *key_focus_name(key_focus_t f)
{
    switch (f) {
    case KF_ZOOM:     return "zoom";
    case KF_CHMOVE:   return "chmove";
    case KF_TRIGMOVE: return "trigmove";
    case KF_CURSOR:   return "cursor";
    case KF_MASK:     return "mask";
    default:          return "?";
    }
}

const char *key_focus_popup(key_focus_t f)
{
    switch (f) {
    case KF_ZOOM:     return "UP/DN: V/div  LT/RT: Time";
    case KF_CHMOVE:   return "UP/DN: CH pos  LT/RT: Trig x";
    case KF_TRIGMOVE: return "UP/DN: Trig level  LT/RT: x";
    case KF_CURSOR:   return "Arrows: Cursor";
    case KF_MASK:     return "UD/LR:Mask tol SEL:stop";
    default:          return "";
    }
}

/* ── Key -> action ────────────────────────────────────────────────── */

static key_cmd_t cmd(key_action_t a, int8_t arg)
{
    key_cmd_t r = { a, arg };
    return r;
}

static key_cmd_t map_long(const key_ctx_t *c, button_id_t b)
{
    switch (b) {
    /* The mode shortcuts work from ANY mode, the meter's softkeys and the
     * settings pages included (manual 1.1 / 2.1 / 3.1). */
    case BTN_MENU:    return cmd(KA_ENTER_HOME, 0);
    case BTN_SELECT:  return cmd(KA_ENTER_METER, 0);
    case BTN_MOVE:    return cmd(KA_ENTER_SCOPE, 0);
    case BTN_TRIGGER: return cmd(KA_ENTER_SIGGEN, 0);

    case BTN_PRM:
        return c->scope_time_view ? cmd(KA_MEAS_HIDE_TOGGLE, 0) : cmd(KA_NONE, 0);

    /* Stock has no long CH1/CH2, so this is OURS: the channel enable. */
    case BTN_CH1:
        return (c->mode == MODE_OSCILLOSCOPE) ? cmd(KA_CH_ENABLE_TOGGLE, 0) : cmd(KA_NONE, 0);
    case BTN_CH2:
        return (c->mode == MODE_OSCILLOSCOPE) ? cmd(KA_CH_ENABLE_TOGGLE, 1) : cmd(KA_NONE, 0);

    /* Long confirm is Run/Stop in the generator (manual 2.1; stock 0x0800D9E2
     * [H]); in the scope's time view it is stock's 50 % centre (code 0x17 ->
     * handler 22, 0x0800D954/0x0800DA3A, ms[0xF68] == 2). */
    case BTN_OK:
        if (c->mode == MODE_SIGNAL_GEN) return cmd(KA_SIGGEN_RUNSTOP, 0);
        return c->scope_time_view ? cmd(KA_CENTER50, 0) : cmd(KA_NONE, 0);

    /* Long AUTO: stock's baseline correction in the scope's time view (code
     * 0x10 -> handler 15, 0x0800DF58); in the generator (where stock has no
     * long AUTO) ours: the output-at-boot policy. */
    case BTN_AUTO:
        if (c->mode == MODE_SIGNAL_GEN) return cmd(KA_SIGGEN_BOOT_TOGGLE, 0);
        return c->scope_time_view ? cmd(KA_BASECAL, 0) : cmd(KA_NONE, 0);

    case BTN_SAVE:
        return (c->mode == MODE_OSCILLOSCOPE) ? cmd(KA_GALLERY_OPEN, 0) : cmd(KA_NONE, 0);

    default:
        return cmd(KA_NONE, 0);
    }
}

static key_cmd_t map_scope_arrow(const key_ctx_t *c, button_id_t b)
{
    switch (c->focus) {
    case KF_ZOOM:
        switch (b) {
        case BTN_UP:    return cmd(KA_VDIV_STEP, +1);
        case BTN_DOWN:  return cmd(KA_VDIV_STEP, -1);
        case BTN_LEFT:  return cmd(KA_TB_STEP, -1);
        case BTN_RIGHT: return cmd(KA_TB_STEP, +1);
        default: break;
        }
        break;
    case KF_CHMOVE:
        switch (b) {
        case BTN_UP:    return cmd(KA_CHPOS_STEP, +1);
        case BTN_DOWN:  return cmd(KA_CHPOS_STEP, -1);
        case BTN_LEFT:  return cmd(KA_TRIGCOL_STEP, -1);
        case BTN_RIGHT: return cmd(KA_TRIGCOL_STEP, +1);
        default: break;
        }
        break;
    case KF_TRIGMOVE:
        switch (b) {
        case BTN_UP:    return cmd(KA_TRIGLVL_STEP, +1);
        case BTN_DOWN:  return cmd(KA_TRIGLVL_STEP, -1);
        case BTN_LEFT:  return cmd(KA_TRIGCOL_STEP, -1);
        case BTN_RIGHT: return cmd(KA_TRIGCOL_STEP, +1);
        default: break;
        }
        break;
    case KF_CURSOR:
        switch (b) {
        case BTN_UP:    return cmd(KA_CURSOR_MOVE, -1);
        case BTN_DOWN:  return cmd(KA_CURSOR_MOVE, +1);
        case BTN_LEFT:
        case BTN_RIGHT: return cmd(KA_CURSOR_NEXT, 0);
        default: break;
        }
        break;
    case KF_MASK:
        switch (b) {
        case BTN_UP:    return cmd(KA_MASK_TOL_V, +1);
        case BTN_DOWN:  return cmd(KA_MASK_TOL_V, -1);
        case BTN_LEFT:  return cmd(KA_MASK_TOL_H, -1);
        case BTN_RIGHT: return cmd(KA_MASK_TOL_H, +1);
        default: break;
        }
        break;
    default:
        break;
    }
    return cmd(KA_LEGACY, 0);
}

key_cmd_t key_map(const key_ctx_t *c, button_id_t btn, bool is_long)
{
    if (is_long)
        return map_long(c, btn);

    if (c->mode != MODE_OSCILLOSCOPE)
        return cmd(KA_LEGACY, 0);

    /* The trigger panel owns the arrows and OK while it is up (parity C3).
     * The keys that would change what the arrows mean close it instead, so
     * the panel can never be left up under a different stage. CH1/CH2/AUTO/
     * SAVE keep their own meaning and leave it open. */
    if (c->trig_menu_open && c->scope_time_view) {
        switch (btn) {
        case BTN_UP:      return cmd(KA_TRIGMENU_ROW, -1);
        case BTN_DOWN:    return cmd(KA_TRIGMENU_ROW, +1);
        case BTN_LEFT:    return cmd(KA_TRIGMENU_CHANGE, -1);
        case BTN_RIGHT:
        case BTN_OK:      return cmd(KA_TRIGMENU_CHANGE, +1);
        case BTN_TRIGGER: case BTN_MENU:
        case BTN_MOVE: case BTN_SELECT: case BTN_PRM:
                          return cmd(KA_TRIGMENU_CLOSE, 0);
        default:          return cmd(KA_LEGACY, 0);
        }
    }

    switch (btn) {
    case BTN_TRIGGER:
        /* Stock's TRIGGER = "Trigger Settings" (manual key table). The
         * cursors it toggled before C3 are in Settings -> Oscilloscope, where
         * stock keeps them (menu "Cursor Measurement"). FFT views: legacy. */
        return c->scope_time_view ? cmd(KA_TRIGMENU_OPEN, 0) : cmd(KA_LEGACY, 0);

    case BTN_MOVE:
        return cmd(KA_FOCUS_NEXT, 0);       /* every scope view, as before */

    case BTN_SELECT:
        if (!c->scope_time_view)
            return cmd(KA_LEGACY, 0);       /* FFT window */
        if (c->focus == KF_MASK && c->mask_present)
            return cmd(KA_MASK_STOPFAIL, 0);
        return cmd(KA_ADJ_CH_SWITCH, 0);

    case BTN_PRM:
        return c->scope_time_view ? cmd(KA_PARAM_SELECT, 0) : cmd(KA_LEGACY, 0);

    case BTN_UP: case BTN_DOWN: case BTN_LEFT: case BTN_RIGHT:
        if (!c->scope_time_view)
            return cmd(KA_LEGACY, 0);       /* FFT reference level / zoom */
        return map_scope_arrow(c, btn);

    default:
        return cmd(KA_LEGACY, 0);
    }
}
