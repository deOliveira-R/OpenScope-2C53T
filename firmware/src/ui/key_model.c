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
    case KF_MASK:     return "UD/LR:Mask tol MOVE:stop";
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
    /* While the Oscilloscope Settings page is up the plot is hidden, so the
     * scope's own long presses (measurements, channel enable, baseline,
     * 50 %) would act on something the user cannot see: ignored. The global
     * ones (mode shortcuts, gallery: key_map_global) keep their meaning. */
    const bool page = (c->mode == MODE_OSCILLOSCOPE) && c->osc_menu_open;
    if (page && (b == BTN_PRM || b == BTN_CH1 || b == BTN_CH2 ||
                 b == BTN_OK || b == BTN_AUTO))
        return cmd(KA_NONE, 0);

    switch (b) {
    /* Long MENU/MOVE/SELECT/TRIGGER/SAVE are global (key_map_global). */
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

    default:
        return cmd(KA_NONE, 0);
    }
}

key_cmd_t key_map_global(const key_ctx_t *c, button_id_t btn, bool is_long)
{
    (void)c;
    if (is_long) {
        switch (btn) {
        /* The mode shortcuts work from ANY mode, the meter's softkeys, the
         * settings pages and the Oscilloscope Settings page included
         * (manual 1.1 / 2.1 / 3.1). */
        case BTN_MENU:    return cmd(KA_ENTER_HOME, 0);
        /* Owner, stock V1.2.0 on unit #2 (2026-11-11): long MOVE ->
         * multimeter, long SELECT -> scope, long TRIGGER -> generator. */
        case BTN_MOVE:    return cmd(KA_ENTER_METER, 0);
        case BTN_SELECT:  return cmd(KA_ENTER_SCOPE, 0);
        case BTN_TRIGGER: return cmd(KA_ENTER_SIGGEN, 0);
        /* Owner decision D1: the gallery from every mode (stock: scope only). */
        case BTN_SAVE:    return cmd(KA_GALLERY_OPEN, 0);
        default:          return cmd(KA_LEGACY, 0);
        }
    }
    switch (btn) {
    /* Owner decision D1: short SAVE is a whole-screen screenshot in every
     * mode and on every page (stock: the scope only; the meter's SAVE used
     * to toggle a debug overlay, now the shell's `meter overlay`). */
    case BTN_SAVE:  return cmd(KA_SCREENSHOT, 0);
    /* Shutdown must always work, whatever is up. */
    case BTN_POWER: return cmd(KA_POWER_PROMPT, 0);
    default:        return cmd(KA_LEGACY, 0);
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

/* The page's deep-link keys: the row each opens. MENU opens Display but is
 * not a deep link once the page is up (it is BACK there). */
static bool osc_deep_link_row(button_id_t b, osc_row_t *row)
{
    switch (b) {
    case BTN_PRM:     *row = OSC_ROW_PARAM;   return true;
    case BTN_TRIGGER: *row = OSC_ROW_TRIGGER; return true;
    case BTN_CH1:     *row = OSC_ROW_CH1;     return true;
    case BTN_CH2:     *row = OSC_ROW_CH2;     return true;
    default:          return false;
    }
}

/* The keys that open the page locked inside a pane, and the pane. */
static bool osc_pane_key_row(button_id_t b, osc_row_t *row)
{
    switch (b) {
    case BTN_PRM:     *row = OSC_ROW_PARAM;   return true;
    case BTN_TRIGGER: *row = OSC_ROW_TRIGGER; return true;
    default:          return false;
    }
}

/* Short keys while the page is up locked to one pane (PRM / TRIGGER). The
 * arrows and OK act in the pane only; MENU or the opener closes the page;
 * the other pane key moves the lock to its pane (as from the plot); the
 * rest, CH1/CH2 included, do nothing: no way to another row. */
static key_cmd_t map_osc_locked(const key_ctx_t *c, button_id_t b)
{
    osc_row_t row;
    switch (b) {
    case BTN_UP:    return cmd(KA_OSCMENU_KEY, OSC_KEY_UP);
    case BTN_DOWN:  return cmd(KA_OSCMENU_KEY, OSC_KEY_DOWN);
    case BTN_LEFT:  return cmd(KA_OSCMENU_KEY, OSC_KEY_LEFT);
    case BTN_RIGHT: return cmd(KA_OSCMENU_KEY, OSC_KEY_RIGHT);
    case BTN_OK:    return cmd(KA_OSCMENU_KEY, OSC_KEY_OK);
    case BTN_MENU:  return cmd(KA_OSCMENU_CLOSE, 0);
    case BTN_POWER: return cmd(KA_LEGACY, 0);                   /* shutdown always works */
    default: break;
    }
    if (osc_pane_key_row(b, &row)) {
        if ((uint8_t)b == c->osc_menu_opener)
            return cmd(KA_OSCMENU_CLOSE, 0);
        return cmd(KA_OSCMENU_PANE, (int8_t)row);
    }
    return cmd(KA_NONE, 0);
}

/* Short keys while the Oscilloscope Settings page is up (UI_STUDY 1.4). */
static key_cmd_t map_osc_menu(const key_ctx_t *c, button_id_t b)
{
    osc_row_t row;
    if (c->osc_menu_locked)
        return map_osc_locked(c, b);
    switch (b) {
    case BTN_UP:    return cmd(KA_OSCMENU_KEY, OSC_KEY_UP);
    case BTN_DOWN:  return cmd(KA_OSCMENU_KEY, OSC_KEY_DOWN);
    case BTN_LEFT:  return cmd(KA_OSCMENU_KEY, OSC_KEY_LEFT);
    case BTN_RIGHT: return cmd(KA_OSCMENU_KEY, OSC_KEY_RIGHT);
    case BTN_OK:    return cmd(KA_OSCMENU_KEY, OSC_KEY_OK);
    case BTN_MENU:  return cmd(KA_OSCMENU_KEY, OSC_KEY_BACK);   /* up a level, or close */
    case BTN_POWER: return cmd(KA_LEGACY, 0);                   /* shutdown always works */
    default: break;
    }
    if (osc_deep_link_row(b, &row)) {
        /* Stock: the same key that opened the page closes it (PRM, TRIGGER).
         * Ours: another deep-link key jumps to its row, and then owns the
         * close. */
        if ((uint8_t)b == c->osc_menu_opener)
            return cmd(KA_OSCMENU_CLOSE, 0);
        return cmd(KA_OSCMENU_OPEN, (int8_t)row);
    }
    /* MOVE / SELECT / AUTO: their meanings are the plot's, which is hidden. */
    return cmd(KA_NONE, 0);
}

key_cmd_t key_map(const key_ctx_t *c, button_id_t btn, bool is_long)
{
    /* The globals first (long mode keys, SAVE); POWER stays KA_LEGACY here. */
    if (btn != BTN_POWER) {
        const key_cmd_t g = key_map_global(c, btn, is_long);
        if (g.act != KA_LEGACY) return g;
    }
    if (is_long)
        return map_long(c, btn);

    if (c->mode != MODE_OSCILLOSCOPE)
        return cmd(KA_LEGACY, 0);

    if (c->osc_menu_open)
        return map_osc_menu(c, btn);

    switch (btn) {
    /* Stock: short BACK in the scope opens the Oscilloscope Settings page at
     * its first row (UI_STUDY 1.3), the full page. In every scope view: the
     * page is also the way back from the FFT views, which PRM no longer
     * cycles. */
    case BTN_MENU:
        return cmd(KA_OSCMENU_OPEN, OSC_ROW_DISPLAY);
    /* Stock V1.2.0 (owner, unit #2, 2026-11-12): PRM opens the page inside
     * Parameter Measurement and TRIGGER inside Trigger Settings, locked
     * there. Every scope view. TRIGGER's inline panel (parity C3, D3) and
     * the FFT views' trigger-mode cycle are gone. */
    case BTN_PRM:
    case BTN_TRIGGER: {
        osc_row_t row;
        (void)osc_pane_key_row(btn, &row);
        return cmd(KA_OSCMENU_PANE, (int8_t)row);
    }

    case BTN_MOVE:
        /* "Switch CH1/CH2 Adjustment" (m.p30), every scope view. The mask
         * stage has no channel to switch: there MOVE is stop-on-fail. */
        if (c->scope_time_view && c->focus == KF_MASK && c->mask_present)
            return cmd(KA_MASK_STOPFAIL, 0);
        return cmd(KA_ADJ_CH_SWITCH, 0);

    case BTN_SELECT:
        /* "Select Function for Directional Keys" (m.p30). FFT views keep
         * their window cycle on this key. */
        if (!c->scope_time_view)
            return cmd(KA_LEGACY, 0);
        return cmd(KA_FOCUS_NEXT, 0);

    case BTN_UP: case BTN_DOWN: case BTN_LEFT: case BTN_RIGHT:
        if (!c->scope_time_view)
            return cmd(KA_LEGACY, 0);       /* FFT reference level / zoom */
        return map_scope_arrow(c, btn);

    default:
        return cmd(KA_LEGACY, 0);
    }
}
