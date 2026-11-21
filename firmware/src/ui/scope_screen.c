/*
 * OpenScope 2C53T - the oscilloscope as a screen. See scope_screen.h.
 */
#include "scope_screen.h"
#include "osc_menu_screen.h"
#include "osc_menu_model.h"
#include "scope_state.h"
#include "scope_roll.h"
#include "scope_mask.h"
#include "meas_select.h"
#include "math_channel.h"
#include "persistence.h"
#include "model.h"
#include "theme.h"
#include "ui.h"
#if FEATURE_FFT
#include "fft.h"
#include "fft_live.h"        /* the spectrum's axes: what a cursor step is worth */
void scope_ui_fft_axes(fft_axes_t *out);     /* scope_spectrum.c: the axes on screen */
#endif

#include <stdio.h>
#include <string.h>

static int32_t G(const prop_desc_t *p) { return prop_get(p); }

/* ── UI state: the arrow stage and the active channel ──────────────── */

/* The job the arrow keys were last put on (view_table.h); short SELECT
 * steps it through the view's list. The job in force is key_focus_get():
 * this one while the view offers it, else the view's first. */
volatile key_focus_t scope_key_focus = KF_ZOOM;

static void set_focus(key_focus_t f)
{
    if (f != scope_key_focus) model_touch(PG_UI);        /* ui.focus */
    scope_key_focus = f;
}

static void set_active_channel(uint8_t ch)
{
    if (ch != active_channel) model_touch(PG_UI);        /* ui.ach */
    active_channel = ch;
}

/* key_model.h: read-only views for the status bar and ui.focus / ui.ach. */
uint8_t key_focus_get(void)
{
    return (uint8_t)key_focus_sanitize((uint8_t)G(P(VIEW_MODE)), scope_key_focus,
                                       G(P(VIEW_CURSOR)) != CURSOR_OFF,
                                       G(P(MASK_STATE)) != MASK_PF_EMPTY);
}
uint8_t key_active_channel_get(void) { return active_channel; }

/* Another view is on the screen (the view's owner, on the input task): the
 * arrows start on its first job, the one the view is mostly used with. */
void scope_screen_view_changed(void)
{
    set_focus((key_focus_t)view_desc((uint8_t)G(P(VIEW_MODE)))->jobs[0]);
}

/* The Oscilloscope Settings page changed the cursor mode by a key
 * (osc_menu_screen.h): cursors switched on from its Cursors row put the
 * arrows on the cursor stage (stock behaviour [inferred], UI_STUDY 2.3). A
 * `set view.cursor` leaves the stage alone: the next key drops a stage that
 * no longer exists (scope_screen_ctx). */
void osc_menu_env_cursors(bool on)
{
    set_focus(key_focus_after_cursor_change((uint8_t)G(P(VIEW_MODE)), scope_key_focus, on));
}

key_ctx_t scope_screen_ctx(void)
{
    key_ctx_t c;
    memset(&c, 0, sizeof c);
    c.mode            = MODE_OSCILLOSCOPE;
    c.view            = (uint8_t)G(P(VIEW_MODE));
    c.cursors_on      = (G(P(VIEW_CURSOR)) != CURSOR_OFF);
    c.mask_present    = (G(P(MASK_STATE)) != MASK_PF_EMPTY);
    c.focus           = (key_focus_t)key_focus_get();
    if (c.focus != scope_key_focus) set_focus(c.focus);   /* the job that went away is forgotten */
    c.active_channel  = active_channel;
    c.osc_menu_open   = screen_is_open(SCR_OSC_MENU);
    c.osc_menu_locked = c.osc_menu_open && osc_menu_is_locked();
    c.osc_menu_opener = osc_menu_screen_opener();
    return c;
}

/* ── one key action ────────────────────────────────────────────────── */

static const prop_desc_t *ch_prop(uint8_t ch, const prop_desc_t *ch1, const prop_desc_t *ch2)
{
    return ch ? ch2 : ch1;
}
#define CHP(ch, sym) ch_prop((ch), P(CH1_##sym), P(CH2_##sym))

static bool set_ok(prop_status_t s) { return s == PS_OK || s == PS_CLAMPED; }

/* The value one arrow step asks for: saturating at the property's ends. The
 * key sets it even when it is the value in force (prop_set_local runs the
 * setter): an arrow at the end of a range re-applied the relay or the
 * register before B7 too. */
static int32_t stepped(const prop_desc_t *p, int delta)
{
    int32_t v = G(p) + delta;
    if (v < p->min) v = p->min;
    if (v > p->max) v = p->max;
    return v;
}

static scr_res_t popup_full(const char *text)
{
    scope_show_popup(text);
    return SCR_DRAW_FULL;
}

/* LEFT / RIGHT in the zoom stage. Past the last FPGA code the ladder
 * continues into roll (C11, stock 0x14..0x1C): scope_roll_tb_step() says
 * where a step lands, and the one property that moved is set: tb.code in
 * Y-T, tb.roll on the way in or out of roll, tb.rollstep inside it. Holding
 * at the slowest roll step changes nothing. The status bar's H= shows the
 * result; only a failed write, which the bar cannot show, gets a popup. */
static scr_res_t tb_step(int dir)
{
    uint8_t tb = (uint8_t)G(P(TB_CODE)), step = (uint8_t)G(P(TB_ROLLSTEP));
    bool roll = G(P(TB_ROLL)) != 0;
    const bool was_roll = roll;
    const uint8_t was_step = step;
    const bool roll_changed = scope_roll_tb_step(&tb, &roll, &step, dir, (uint8_t)(P(TB_CODE)->max + 1));
    prop_status_t st = PS_OK;
    int32_t want_code = tb;

    if (!roll_changed) {
        if (was_roll) return SCR_DRAW_FULL;              /* the slowest step holds */
        st = prop_set_local(P(TB_CODE), tb, ORIGIN_KEY, NULL);
    } else if (roll != was_roll) {
        /* Into roll at step 0, or out of it onto the last Y-T code: both
         * are stored without a write (roll is off / still on) and the roll
         * set applies once. */
        if (roll) (void)prop_set_local(P(TB_ROLLSTEP), step, ORIGIN_KEY, NULL);
        else      (void)prop_set_local(P(TB_CODE), tb, ORIGIN_KEY, NULL);
        st = prop_set_local(P(TB_ROLL), roll, ORIGIN_KEY, NULL);
        if (!set_ok(st) && roll) (void)prop_set_local(P(TB_ROLLSTEP), was_step, ORIGIN_KEY, NULL);
        want_code = G(P(TB_CODE));
    } else {
        st = prop_set_local(P(TB_ROLLSTEP), step, ORIGIN_KEY, NULL);
        want_code = G(P(TB_CODE));
    }
    if (set_ok(st)) return SCR_DRAW_FULL;

    /* The rate did NOT change: say so rather than leave a label for a rate
     * that is not in force. A refused roll change names the mode kept. */
    char tbl[12], pb[24];
    if (G(P(TB_ROLL))) scope_roll_label((uint8_t)G(P(TB_ROLLSTEP)), tbl, sizeof tbl);
    else               prop_format(P(TB_CODE), want_code, tbl, sizeof tbl);
    snprintf(pb, sizeof pb, "H=%s NOT SET", tbl);
    return popup_full(pb);
}

static const prop_desc_t *cursor_prop(int32_t sel)
{
    switch (sel) {
    case CURSOR_SEL_V1: return P(VIEW_CURV1);
    case CURSOR_SEL_V2: return P(VIEW_CURV2);
    case CURSOR_SEL_H1: return P(VIEW_CURH1);
    default:            return P(VIEW_CURH2);
    }
}

#if FEATURE_FFT
/* UP / DOWN in the spectrum's cursor job: the selected line by what
 * SPECTRUM_CURSOR_STEP_PX pixels are worth on the spectrum's axes as they
 * stand, so a step looks the same at every span and zooming in makes it
 * finer. UP moves a frequency line down in frequency and a level line up, as
 * the time cursors move left and up. A line that is off the plot comes to
 * its nearest edge first. Nothing moves while no spectrum is drawn. */
static void spectrum_cursor_move(int32_t sel, int dir)
{
    static const prop_desc_t *const k_prop[4] = {
        [CURSOR_SEL_V1] = P(VIEW_CURF1), [CURSOR_SEL_V2] = P(VIEW_CURF2),
        [CURSOR_SEL_H1] = P(VIEW_CURL1), [CURSOR_SEL_H2] = P(VIEW_CURL2),
    };
    fft_axes_t ax;
    scope_ui_fft_axes(&ax);
    if (!ax.on || sel < 0 || sel > CURSOR_SEL_H2) return;
    const prop_desc_t *p = k_prop[sel];
    int32_t v = G(p), lo, hi, step;
    if (sel == CURSOR_SEL_V1 || sel == CURSOR_SEL_V2) {
        lo = ax.bin0;
        hi = ax.bin0 + ax.bins - 1;
        step = dir * (int32_t)fft_axes_bins_per_cols(&ax, SPECTRUM_CURSOR_STEP_PX);
    } else {
        const float unit = (float)SPECTRUM_CURSOR_LEVEL_UNIT;
        hi = (int32_t)(ax.ref_db * unit);
        lo = (int32_t)((ax.ref_db - ax.range_db) * unit) + 1;
        const int32_t s = (int32_t)(fft_axes_db_per_rows(&ax, SPECTRUM_CURSOR_STEP_PX) * unit + 0.5f);
        step = -dir * (s ? s : 1);
    }
    if (v < lo) v = lo; else if (v > hi) v = hi; else v += step;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    (void)prop_set_local(p, v, ORIGIN_KEY, NULL);
}
#else
static void spectrum_cursor_move(int32_t sel, int dir) { (void)sel; (void)dir; }
#endif

/* LEFT / RIGHT in the cursor stage: the next line of the pair shown. */
static int32_t cursor_next(int32_t mode, int32_t sel)
{
    switch (mode) {
    case CURSOR_VERTICAL:   return (sel == CURSOR_SEL_V1) ? CURSOR_SEL_V2 : CURSOR_SEL_V1;
    case CURSOR_HORIZONTAL: return (sel == CURSOR_SEL_H1) ? CURSOR_SEL_H2 : CURSOR_SEL_H1;
    case CURSOR_BOTH:
        return sel == CURSOR_SEL_V1 ? CURSOR_SEL_V2 : sel == CURSOR_SEL_V2 ? CURSOR_SEL_H1 :
               sel == CURSOR_SEL_H1 ? CURSOR_SEL_H2 : CURSOR_SEL_V1;
    default:                return sel;
    }
}

/* The mask tolerances: a request the acquisition task applies. The popup
 * shows what the mask reports once it has acknowledged, and the pair asked
 * for with "..." while the request is still in flight (not lost: applied on
 * its next poll). */
static scr_res_t mask_tol(const prop_desc_t *p, int delta)
{
    char pb[24];
    (void)prop_set_local(p, stepped(p, delta), ORIGIN_KEY, NULL);
    snprintf(pb, sizeof pb, "Mask V%u H%u%s", (unsigned)G(P(MASK_TOLV)), (unsigned)G(P(MASK_TOLH)),
             G(P(MASK_PENDING)) ? " ..." : "");
    return popup_full(pb);
}

static scr_res_t exec(key_cmd_t k, button_id_t btn)
{
    char pb[24];
    prop_note_t note;
    const int dir = k.arg;
    const uint8_t ach = active_channel ? 1u : 0u;

    switch (k.act) {
    /* -- long presses -------------------------------------------------- */
    case KA_MEAS_HIDE_TOGGLE:
        (void)prop_step_local(P(VIEW_MEASHIDE), +1, ORIGIN_KEY, NULL);
        return popup_full(G(P(VIEW_MEASHIDE)) ? "Measurements hidden" : "Measurements shown");

    case KA_CH_ENABLE_TOGGLE: {
        /* The setter routes the converters and refuses both off; when the
         * active channel goes off the arrows follow the live one. The popup
         * reports the routing in force, not the request: CH2 off at a fast
         * timebase is what gives the interleaved 250 MS/s record (the
         * setter's note says so). */
        const prop_desc_t *p = CHP(dir ? 1u : 0u, ENABLED);
        if (!set_ok(prop_step_local(p, +1, ORIGIN_KEY, &note)))
            return popup_full("Keep one CH on");
        snprintf(pb, sizeof pb, "CH%d %s%s", dir + 1, G(p) ? "ON" : "OFF", note.text[0] ? " 250MS/s" : "");
        return popup_full(pb);
    }

    /* Long AUTO / long OK (parity C10): queued for the input task's selfcal
     * service, which runs right after this key and shows its own popups.
     * Baseline correction walks banks 0 and 1 (stock's order); bank 2, the
     * interleaved single-channel regime, is `fpga scope basecal 7`. */
    case KA_BASECAL:
        return scope_selfcal_request_basecal(0x03u) ? SCR_HANDLED : popup_full("Busy");
    case KA_CENTER50:
        return scope_selfcal_request_center50() ? SCR_HANDLED : popup_full("Busy");

    /* -- short presses: UI state --------------------------------------- */
    case KA_FOCUS_NEXT: {
        const key_focus_t f = key_focus_next((uint8_t)G(P(VIEW_MODE)), scope_key_focus,
                                             G(P(VIEW_CURSOR)) != CURSOR_OFF,
                                             G(P(MASK_STATE)) != MASK_PF_EMPTY);
        set_focus(f);
        return popup_full(key_focus_popup(f));
    }

    case KA_ADJ_CH_SWITCH: {
        /* "Switch CH1/CH2 Adjustment": which channel UP/DOWN act on. A
         * channel that is off is not offered: it has nothing to adjust. */
        const uint8_t other = (uint8_t)(1u - ach);
        if (G(CHP(other, ENABLED))) {
            set_active_channel(other);
            snprintf(pb, sizeof pb, "Adjust CH%d", other + 1);
        } else {
            snprintf(pb, sizeof pb, "CH%d is off", other + 1);
        }
        return popup_full(pb);
    }

    case KA_OSCMENU_OPEN:        /* MENU or a deep-link key: the full page (osc_menu_screen_open pushes its layer) */
        osc_menu_screen_open((osc_row_t)dir, false, (uint8_t)btn);
        return SCR_DRAW;
    case KA_OSCMENU_PANE:        /* PRM / TRIGGER: the page locked inside their pane (stock V1.2.0) */
        osc_menu_screen_open((osc_row_t)dir, true, (uint8_t)btn);
        return SCR_DRAW;

    /* -- short presses: settings, one property set each ---------------- */
    case KA_TB_STEP:
        return tb_step(dir);

    case KA_VDIV_STEP: {
        /* The relays first, the state on success (the setter). The bottom
         * bar shows the new volts/div; only a failed relay write, which the
         * bar cannot show, gets a popup. */
        const prop_desc_t *p = CHP(ach, VDIV);
        const int32_t want = stepped(p, dir);
        if (set_ok(prop_set_local(p, want, ORIGIN_KEY, NULL))) return SCR_DRAW_FULL;
        char vd[12];
        prop_format(p, want, vd, sizeof vd);
        snprintf(pb, sizeof pb, "CH%d %s NOT SET", ach + 1, vd);
        return popup_full(pb);
    }

    case KA_CHPOS_STEP: {
        /* Stock's channel move: the vertical position drives the channel's
         * offset DAC from the calibration page (scope_vpos, parity C4); with
         * no usable page only the ground marker moves. */
        const prop_desc_t *p = CHP(ach, POS);
        (void)prop_set_local(p, stepped(p, dir * (int)p->step), ORIGIN_KEY, NULL);
        snprintf(pb, sizeof pb, "CH%d pos %+d", ach + 1, (int)G(p));
        return popup_full(pb);
    }

    case KA_TRIGCOL_STEP: {
        /* The trigger point across the screen, 16 px per press, over
         * stock's 150 + hpos, hpos +-150 (parity C5). The popup reports the
         * column asked for; the marker at the top shows where it landed. */
        const int32_t want = stepped(P(TRIG_X), dir * (int)P(TRIG_X)->step);
        (void)prop_set_local(P(TRIG_X), want, ORIGIN_KEY, NULL);
        snprintf(pb, sizeof pb, "Trig at x=%d", (int)want);
        return popup_full(pb);
    }

    case KA_TRIGLVL_STEP: {
        /* Through the ONE writer of reg 0x08 (the setter), so the register,
         * the persisted level and the marker cannot diverge. The popup shows
         * the code the firmware records as in force (trig.code), not a
         * recomputation. */
        const int32_t want = stepped(P(TRIG_LEVEL), dir * (int)P(TRIG_LEVEL)->step);
        if (set_ok(prop_set_local(P(TRIG_LEVEL), want, ORIGIN_KEY, NULL)))
            snprintf(pb, sizeof pb, "Trig %+d  code %u", (int)G(P(TRIG_LEVEL)), (unsigned)G(P(TRIG_CODE)));
        else
            snprintf(pb, sizeof pb, "Trig %+d  NOT SET", (int)want);
        return popup_full(pb);
    }

    /* The live frame draws the cursors and the AUX row their readout, so a
     * move is one live frame, not a full repaint (scope_yt.c). */
    case KA_CURSOR_MOVE:
        (void)prop_step_local(cursor_prop(G(P(VIEW_CURSEL))), dir, ORIGIN_KEY, NULL);
        scope_ui_request_live();
        return SCR_HANDLED;
    case KA_SPECTRUM_CURSOR_MOVE:
        spectrum_cursor_move(G(P(VIEW_CURSEL)), dir);
        scope_ui_request_live();
        return SCR_HANDLED;
    case KA_CURSOR_NEXT:
        (void)prop_set_local(P(VIEW_CURSEL), cursor_next(G(P(VIEW_CURSOR)), G(P(VIEW_CURSEL))),
                             ORIGIN_KEY, NULL);
        scope_ui_request_live();
        return SCR_HANDLED;

    case KA_MASK_TOL_V: return mask_tol(P(MASK_TOLV), dir * (int)P(MASK_TOLV)->step);
    case KA_MASK_TOL_H: return mask_tol(P(MASK_TOLH), dir * (int)P(MASK_TOLH)->step);

    case KA_MASK_STOPFAIL:
        (void)prop_step_local(P(MASK_STOPFAIL), +1, ORIGIN_KEY, NULL);
        if (G(P(MASK_PENDING))) snprintf(pb, sizeof pb, "Mask stop NOT SET");
        else snprintf(pb, sizeof pb, "Mask stop-on-fail %s", G(P(MASK_STOPFAIL)) ? "ON" : "OFF");
        return popup_full(pb);

    case KA_COUPLING_TOGGLE: {
        /* The channel becomes the active one; DC <-> AC on its relay. The
         * label changes only if the relay did (the setter verifies by
         * readback). GND is out of the cycle: no ground path is known. */
        const uint8_t ch = dir ? 1u : 0u;
        const prop_desc_t *p = CHP(ch, COUPLING);
        const int32_t want = (G(p) == COUPLING_DC) ? COUPLING_AC : COUPLING_DC;
        char lab[8];
        set_active_channel(ch);
        const bool ok = set_ok(prop_set_local(p, want, ORIGIN_KEY, NULL));
        prop_format(p, want, lab, sizeof lab);
        snprintf(pb, sizeof pb, ok ? "CH%u %s" : "CH%u %s NOT SET", (unsigned)ch + 1u, lab);
        return popup_full(pb);
    }

    case KA_RUNSTOP:
        if (G(P(MASK_HOLD))) {
            /* A mask failure is being held (stop-on-fail): OK resumes
             * testing rather than toggling RUN/STOP, which would leave the
             * acquisition frozen behind a STOP the user never set. */
            (void)scope_mask_post(SCOPE_MASK_REQ_RELEASE, 0, 0, 0);
            scope_show_popup("MASK: RESUME");
        } else if (G(P(TRIG_MODE)) == TRIG_SINGLE && (!G(P(VIEW_RUN)) || G(P(TRIG_HELD)))) {
            /* Stock's SINGLE: after the capture the scope is STOPPED
             * (0x0802163E) and RUN re-arms it; a STOP the user pressed
             * while armed re-arms the same way. The view.run setter does it
             * ("Await Trigger..."), for `set view.run on` too. */
            (void)prop_set_local(P(VIEW_RUN), 1, ORIGIN_KEY, &note);
            if (note.text[0]) scope_show_popup(note.text);
        } else {
            /* No popup: the status bar's RUN/STOP tab says it. STOP latches
             * the ranges for the zoom (the setter). */
            (void)prop_step_local(P(VIEW_RUN), +1, ORIGIN_KEY, NULL);
        }
        return SCR_DRAW_FULL;

    /* -- short presses: procedures ------------------------------------- */
    case KA_AUTOSET:
        /* Autoset (parity C9; stock 0x0800CB50). While a mask exists, or
         * the arrows are on its stage, AUTO keeps its mask meaning (teach /
         * cancel a teach / clear): the mask is ours, and an autoset would
         * move the picture it was taught on. */
        if (G(P(MASK_STATE)) != MASK_PF_EMPTY || scope_key_focus == KF_MASK) {
            const bool gone = scope_mask_key_auto(pb, sizeof pb);
            if (gone && scope_key_focus == KF_MASK) set_focus(KF_ZOOM);   /* the stage went with the mask */
            return popup_full(pb);
        }
        return scope_selfcal_request_autoset() ? SCR_HANDLED : popup_full("Busy");

#if FEATURE_FFT
    /* The spectrum job's keys: the reference level and the span, which have
     * no property (scope_screen.h). AUTO fits the span to the record the
     * view analyses: a request the display task serves on its next frame. */
    case KA_FFT_AUTO:
        fft_auto_request();
        return SCR_DRAW_FULL;
    case KA_FFT_REF:
        (void)fft_adjust_ref_level(dir > 0 ? 5.0f : -5.0f);
        return SCR_DRAW_FULL;
    case KA_FFT_ZOOM:
        if (dir < 0) fft_zoom_in(); else fft_zoom_out();
        return SCR_DRAW_FULL;
#endif

    /* Claimed with nothing to do (KA_NONE: a long key the view does not
     * use), an arrow in a stage that is gone (KA_LEGACY), and the answers
     * of screens that take their keys before this one. */
    default:
        return SCR_HANDLED;
    }
}

/* ── input task ────────────────────────────────────────────────────── */

/* POWER, SAVE and the long mode keys are the router's globals and never get
 * here; the Oscilloscope Settings page, while up, is asked first. */
static scr_res_t s_key(scr_key_t k)
{
    const key_ctx_t c = scope_screen_ctx();
    return exec(key_map(&c, k.btn, k.is_long), k.btn);
}

/* ── display task ──────────────────────────────────────────────────── */

void scope_screen_epochs(uint32_t *layout, uint32_t *soft)
{
    const scope_state_t *s = scope_state_get();
    uint32_t h = REDRAW_EPOCH_SEED;

    h = redraw_epoch_mix(h, (uint32_t)theme_get_id());
    h = redraw_epoch_mix(h, ((uint32_t)s->running << 8) | ((uint32_t)s->roll << 9)
                            | ((uint32_t)s->roll_step << 12));
    h = redraw_epoch_mix(h, (uint32_t)s->ch1.enabled | ((uint32_t)s->ch2.enabled << 1));
    /* The measurement selection, every set; long PRM hides the rows. */
    h = redraw_epoch_mix(h, (uint32_t)meas_select_get(MEAS_SET_CH1) |
                            ((uint32_t)meas_select_get(MEAS_SET_CH2) << 16));
    h = redraw_epoch_mix(h, (uint32_t)meas_select_get(MEAS_SET_FFT));
    h = redraw_epoch_mix(h, (uint32_t)s->prm_hidden);
    /* Cursor mode only: it moves the strip (the AUX row). */
    h = redraw_epoch_mix(h, (uint32_t)s->cursor.mode);
    h = redraw_epoch_mix(h, (uint32_t)math_enabled | ((uint32_t)math_op << 1)
                            | ((uint32_t)persist_enabled << 8)
                            | ((uint32_t)persist_get_mode() << 9));
#if FEATURE_FFT
    h = redraw_epoch_mix(h, (uint32_t)scope_view);
    h = redraw_epoch_mix(h, scope_view_fft_epoch());
#endif
    *layout = h;

    h = REDRAW_EPOCH_SEED;
    h = redraw_epoch_mix(h, (uint32_t)active_channel);
    h = redraw_epoch_mix(h, (uint32_t)s->timebase_idx);
    h = redraw_epoch_mix(h, ((uint32_t)s->ch1.vdiv_idx << 1) | ((uint32_t)s->ch1.coupling << 8)
                            | ((uint32_t)s->ch1.probe << 12) | ((uint32_t)s->ch1.bw_limit << 16));
    h = redraw_epoch_mix(h, (uint32_t)(uint16_t)s->ch1.position);
    h = redraw_epoch_mix(h, ((uint32_t)s->ch2.vdiv_idx << 1) | ((uint32_t)s->ch2.coupling << 8)
                            | ((uint32_t)s->ch2.probe << 12) | ((uint32_t)s->ch2.bw_limit << 16));
    h = redraw_epoch_mix(h, (uint32_t)(uint16_t)s->ch2.position);
    h = redraw_epoch_mix(h, (uint32_t)s->trigger.mode | ((uint32_t)s->trigger.edge << 4)
                            | ((uint32_t)s->trigger.source << 8));
    h = redraw_epoch_mix(h, (uint32_t)(uint16_t)s->trigger.level);
    h = redraw_epoch_mix(h, (uint32_t)(uint16_t)s->trig_x);
    *soft = h;
}

uint32_t scope_screen_cursor_epoch(void)
{
    const cursor_state_t *c = &scope_state_get()->cursor;
    uint32_t h = REDRAW_EPOCH_SEED;
    h = redraw_epoch_mix(h, (uint32_t)c->active);
    h = redraw_epoch_mix(h, (uint32_t)c->v1_x | ((uint32_t)c->v2_x << 16));
    h = redraw_epoch_mix(h, (uint32_t)c->h1_y | ((uint32_t)c->h2_y << 16));
    /* The same lines on a spectrum. */
    const spectrum_cursor_t *sc = &scope_state_get()->spectrum_cursor;
    h = redraw_epoch_mix(h, (uint32_t)sc->bin[0] | ((uint32_t)sc->bin[1] << 16));
    h = redraw_epoch_mix(h, (uint32_t)(uint16_t)sc->level[0] | ((uint32_t)(uint16_t)sc->level[1] << 16));
    return h;
}

static uint32_t s_epoch(void)
{
    uint32_t layout, soft;
    scope_screen_epochs(&layout, &soft);
    return redraw_epoch_mix(redraw_epoch_mix(layout, soft), scope_screen_cursor_epoch());
}

#if FEATURE_FFT
_Static_assert(SCOPE_VIEW_TIME == 0, "scope_shown_t.kind 0 is the Y-T view (scope_screen_changed)");
#endif
void scope_screen_shown(scope_shown_t *now)
{
    const scope_state_t *s = scope_state_get();
    scope_screen_epochs(&now->layout, &now->soft);
    now->cursor = scope_screen_cursor_epoch();
#if FEATURE_FFT
    now->kind = (uint8_t)(((uint8_t)scope_view << 1) | (s->roll ? 1u : 0u));
#else
    now->kind = s->roll ? 1u : 0u;
#endif
}

scope_change_t scope_screen_changed(const scope_shown_t *seen, const scope_shown_t *now,
                                    bool primed)
{
    scope_change_t c = { false, false, false, false, false, false };
    const bool soft_moved = now->soft != seen->soft;
    /* The bar follows whoever changed a value, painted or not yet. */
    c.bar = soft_moved || now->layout != seen->layout;
    if (!primed) return c;
    if (now->kind != seen->kind) { c.whole = true; return c; }
    c.layout = now->layout != seen->layout;
    /* Soft in the Y-T view (kind 0): its live frame repaints everything a
     * soft change moves. */
    const bool soft_ok = now->kind == 0u;
    if (soft_moved && soft_ok) c.live = c.strip = true;
    if (soft_moved && !soft_ok) c.force = true;
    if (now->cursor != seen->cursor) c.live = true;
    return c;
}

bool scope_screen_paints(uint16_t groups)
{
    const uint16_t own = (uint16_t)((1u << PG_CH1) | (1u << PG_CH2) | (1u << PG_TB) | (1u << PG_TRIG) |
                                    (1u << PG_VIEW) | (1u << PG_FFT) | (1u << PG_IL) | (1u << PG_MASK));
    return groups != 0u && (groups & (uint16_t)~own) == 0u;
}

static void            s_draw(scr_paint_t how, uint32_t frame) { scope_view_draw(how, frame); }
static redraw_action_t s_tick(uint32_t frame)                  { return scope_view_tick(frame); }

/* The property groups the scope shows (screen_t.groups): the bars add the
 * generator slot, sys and ui. */
#define GB(g) ((uint16_t)(1u << (g)))
#define S_GROUPS (GB(PG_GEN) | GB(PG_SYS) | GB(PG_UI) | GB(PG_CH1) | GB(PG_CH2) | GB(PG_TB) | GB(PG_TRIG) | \
                  GB(PG_VIEW) | GB(PG_FFT) | GB(PG_IL) | GB(PG_MASK))

const screen_t scope_screen = {
    .name = "scope", .id = SCR_SCOPE, .mode = MODE_OSCILLOSCOPE, .instrument = MODE_OSCILLOSCOPE,
    .flags = 0,
    .open = NULL, .close = NULL, .key = s_key,
    .draw = s_draw, .tick = s_tick, .epoch = s_epoch,
    .describe = NULL, .groups = S_GROUPS,
};
