/*
 * OpenScope 2C53T - Input Handler
 *
 * All button-to-action mapping logic lives here.
 * The input task (in main.c) handles GPIO polling and debounce,
 * then calls input_handle_button() for the actual logic.
 *
 * Integrated features from 4 agent worktrees:
 *   - Cursor measurement mode (TRIGGER toggles, arrows move cursors)
 *   - Meter sub-modes (LEFT/RIGHT cycle, SELECT resets min/max/avg)
 *   - Signal gen: amplitude stepping, duty cycle, frequency presets
 *   - Math channel, persistence, component tester (settings sub-menus)
 */

#include "../ui/scope_mask.h"
#include "key_model.h"
#include "input_handler.h"
#include "ui.h"
#include "softkey.h"
#include "lcd.h"
#include "font.h"
#include "scope_state.h"
#include "scope_cal.h"
#include "../ui/scope_timebase.h"
#include "../ui/scope_roll.h"
#include "signal_gen.h"
#include "theme.h"
#include "math_channel.h"
#include "component_test.h"
#include "persistence.h"
#include "shared_mem.h"
#ifdef FEATURE_FFT
#include "fft_live.h"
#endif
#include "fpga.h"
#include "scope_vpos.h"
#include "meter_autoselect.h"
#include "at32f403a_407.h"
#include "button_scan.h"
#include "settings_store.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

/* MENU is bit 9 in the raw button-scan state (button_scan.c). */
#define BTN_SCAN_MENU_MASK  0x0200u

/* ── Settings persistence hooks ──────────────────────────────────────
 * The input task is the only thing that changes settings, so it is also what
 * decides when they are written. See settings_store.h for the policy; the
 * short version is "debounced settle, plus a flush at deliberate boundaries".
 * The FreeRTOS tick is 1 kHz (configTICK_RATE_HZ), so ticks are milliseconds. */
static uint32_t settings_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

#ifdef FEATURE_FFT
#include "fft.h"
#include "fft_test_signals.h"
#endif

/* ═══════════════════════════════════════════════════════════════════
 * Oscilloscope settings Left/Right handler
 * ═══════════════════════════════════════════════════════════════════ */

static void channel_cycle_coupling(uint8_t ch, channel_state_t *c, const char *name,
                                   char *pb, size_t n);

/* Stock's "Display Mode" item (menu table 0x0804C824: Y-T / Rolling / X-Y).
 * This item used to cycle the colour theme, which now has its own row.
 * Roll is the C11 MCU-paced mode (re-applied to the FPGA here, so the menu
 * and the timebase keys reach the same state); X-Y is the existing Lissajous
 * view, so it needs the FEATURE_FFT build that carries it and is skipped
 * otherwise. Leaving X-Y releases the FFT pool exactly as the PRM cycle does
 * on its way back to the time view (fft_deinit is a no-op if unclaimed). */
static void display_mode_step(int dir)
{
    scope_state_t *ss = scope_state_get();
    const scope_disp_mode_t cur = (scope_disp_mode_t)scope_display_mode();
    scope_disp_mode_t next = scope_disp_mode_cycle(cur, dir);
#ifndef FEATURE_FFT
    if (next == SCOPE_DISP_XY)
        next = scope_disp_mode_cycle(next, dir);
#else
    if (next == SCOPE_DISP_XY) {
        scope_view = SCOPE_VIEW_XY;
    } else if (scope_view != SCOPE_VIEW_TIME) {
        scope_view = SCOPE_VIEW_TIME;
        fft_deinit();
    }
#endif
    const bool want_roll = (next == SCOPE_DISP_ROLL);
    if (want_roll != ss->roll) {
        ss->roll = want_roll;
        if (!fpga_apply_roll())
            ss->roll = !want_roll;  /* nothing was written: keep the old mode */
    }
}

static void osc_settings_adjust(int dir)
{
    scope_state_t *ss = scope_state_get();
    switch (settings_sub_selected) {
    case 0: /* CH1 Coupling -- the relay, DC <-> AC; two states, so dir is moot */
        { char t[24]; channel_cycle_coupling(0, &ss->ch1, "CH1", t, sizeof(t)); }
        break;
    case 1: /* CH1 Probe */
        ss->ch1.probe = (probe_t)((ss->ch1.probe + PROBE_COUNT + dir) % PROBE_COUNT);
        break;
    case 2: /* CH1 20M Limit -- no hardware path known; see the menu row */
        ss->ch1.bw_limit = false;
        break;
    case 3: /* CH2 Coupling -- the relay, DC <-> AC; two states, so dir is moot */
        { char t[24]; channel_cycle_coupling(1, &ss->ch2, "CH2", t, sizeof(t)); }
        break;
    case 4: /* CH2 Probe */
        ss->ch2.probe = (probe_t)((ss->ch2.probe + PROBE_COUNT + dir) % PROBE_COUNT);
        break;
    case 5: /* CH2 20M Limit -- no hardware path known; see the menu row */
        ss->ch2.bw_limit = false;
        break;
    case 6: /* Trigger Mode */
        ss->trigger.mode = (trigger_mode_t)((ss->trigger.mode + TRIG_COUNT + dir) % TRIG_COUNT);
        break;
    case 7: /* Trigger Edge */
        ss->trigger.edge = (trigger_edge_t)((ss->trigger.edge + TRIG_EDGE_COUNT + dir) % TRIG_EDGE_COUNT);
        break;
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * Settings OK handler
 * ═══════════════════════════════════════════════════════════════════ */

void input_handle_settings_ok(void)
{
    scope_state_t *ss = scope_state_get();

    if (settings_depth == 0) {
        switch (settings_selected) {
        case 0: /* Oscilloscope Settings -- enter sub-menu */
            settings_depth = 1;
            settings_sub_selected = 0;
            break;
        case 3: /* Display Mode: Y-T / Roll / X-Y (stock menu 0x0804C824) */
            display_mode_step(+1);
            break;
        case 12: /* Theme -- moved off Display Mode, which is stock's Y-T/Roll/X-Y */
            theme_cycle();
            break;
        case 4: /* Math Channel -- enter math sub-menu */
            settings_depth = 3;
            settings_sub_selected = 0;
            break;
        case 5: /* Component Tester -- enter component test screen */
            settings_depth = 4;
            comp_test_init();
            break;
        case 6: /* Bode Plot -- enter bode screen */
            settings_depth = 6;
            break;
        case 7: /* Startup on Boot -- cycle target mode */
            startup_mode_adjust(1);
            break;
        case 8: /* About -- display info screen */
            settings_depth = 2;
            break;
        case 9: /* FPGA SPI Scanner -- brute-force activation sweep */
            {
                extern void fpga_scanner_run(void);
                fpga_scanner_run();
            }
            break;
        case 10: /* Firmware Update -- reboot into DFU bootloader */
            {
                extern void dfu_request_reboot(void);
                /* Deliberate boundary: we are about to reboot, so anything
                 * still inside the settle window would be lost. */
                (void)settings_store_flush(settings_now_ms());
                dfu_request_reboot();
            }
            break;
        default:
            break;
        }
    } else if (settings_depth == 1) {
        /* Oscilloscope settings sub-menu */
        switch (settings_sub_selected) {
        /* Coupling through the relay, as on the CH1/CH2 buttons; the popup
         * text is discarded here -- the menu redraw shows the label, which
         * only changes when the relay did. */
        case 0: { char t[24]; channel_cycle_coupling(0, &ss->ch1, "CH1", t, sizeof(t)); } break;
        case 1: scope_cycle_probe(&ss->ch1); break;
        /* 20M limit: refused. Its only consumer was an unobeyed USART bit,
         * so it was label-only in every build (EXP-70 audit). */
        case 2: ss->ch1.bw_limit = false; break;
        case 3: { char t[24]; channel_cycle_coupling(1, &ss->ch2, "CH2", t, sizeof(t)); } break;
        case 4: scope_cycle_probe(&ss->ch2); break;
        case 5: ss->ch2.bw_limit = false; break;
        case 6: scope_cycle_trigger_mode(ss); break;
        case 7: scope_cycle_trigger_edge(ss); break;
        default: break;
        }
    } else if (settings_depth == 2) {
        /* About screen -- OK goes back */
        settings_depth = 0;
    } else if (settings_depth == 3) {
        /* Math channel sub-menu */
        if (settings_sub_selected == 0) {
            /* Math Channel: cycle Off -> A+B -> A-B -> A*B -> ... -> Off */
            if (!math_enabled) {
                math_enabled = true;
                math_op = 0;
            } else if (math_op < MATH_COUNT - 1) {
                math_op++;
            } else {
                math_enabled = false;
                math_op = 0;
            }
        } else if (settings_sub_selected == 1) {
            /* Persistence: toggle on/off with pool lifecycle */
            persist_enabled = !persist_enabled;
            if (persist_enabled) {
                /* Persistence needs the pool — evicts FFT if active */
                if (fft_is_initialized())
                    fft_deinit();
                persist_init();
                persist_set_mode(PERSIST_MEDIUM);
            } else {
                persist_deinit();
            }
        } else if (settings_sub_selected == 2) {
            /* Back */
            settings_depth = 0;
        }
    } else if (settings_depth == 4) {
        /* Component tester: OK = enter resistor calculator */
        settings_depth = 5;
        resistor_calc_init();
    } else if (settings_depth == 5) {
        /* Resistor calculator: OK = back to component tester */
        settings_depth = 4;
    } else if (settings_depth == 6) {
        /* Bode plot: OK = back to settings */
        settings_depth = 0;
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * Helper: send a display command
 * ═══════════════════════════════════════════════════════════════════ */

static void send_cmd(QueueHandle_t q, uint8_t cmd)
{
    xQueueSend(q, &cmd, 0);
}

/* What the arrow keys adjust in the scope time view (key_model.h). MOVE cycles
 * it: zoom -> channel move -> trigger move -> [cursor] -> [mask]. The three
 * bools are derived from it (set_focus) because the renderer and the shell
 * read them. */
volatile key_focus_t scope_key_focus = KF_ZOOM;
volatile bool scope_trig_level_focus = false;
volatile bool scope_hpos_focus = false;
/* The mask stage, offered only while a mask exists: UP/DOWN = vertical
 * tolerance, LEFT/RIGHT = horizontal, SELECT = stop-on-fail. */
volatile bool scope_mask_focus = false;

static void set_focus(key_focus_t f)
{
    scope_key_focus        = f;
    scope_trig_level_focus = (f == KF_TRIGMOVE);
    scope_hpos_focus       = (f == KF_CHMOVE || f == KF_TRIGMOVE);
    scope_mask_focus       = (f == KF_MASK);
}

/* Helper: show popup and send redraw */
static void popup_and_redraw(QueueHandle_t q, const char *text)
{
    scope_show_popup(text);
    uint8_t cmd = DCMD_REDRAW_ALL;
    send_cmd(q, cmd);
}

/* CH1/CH2: DC <-> AC on the real relay. The label changes only if the relay
 * did (fpga_apply_coupling verifies by readback), so the popup and the info
 * bar can no longer say "AC" over a DC-coupled input -- which they did, on
 * every build, until 2026-10-03. GND is out of the cycle: no hardware ground
 * path is known, and a GND label over a live input would be the same lie. */
static void channel_cycle_coupling(uint8_t ch, channel_state_t *c, const char *name,
                                   char *pb, size_t n)
{
    coupling_t next = (c->coupling == COUPLING_DC) ? COUPLING_AC : COUPLING_DC;
    if (fpga_apply_coupling(ch, (uint8_t)next)) {
        c->coupling = next;
        snprintf(pb, n, "%s %s", name, coupling_labels[next]);
    } else {
        snprintf(pb, n, "%s %s NOT SET", name, coupling_labels[next]);
    }
}

/* ── Mask pass/fail from the buttons (docs/specs/scope/mask-pass-fail.md) ──
 *
 * Every change is a request the acquisition task applies (scope_mask.h). The
 * popup reports what the mask says AFTER the acknowledgement -- the applied
 * tolerance, not the one asked for -- and says NOT SET when no ack arrives
 * (the inert-controls lesson: report the wire, not the setter's intent).
 * The wait is bounded at ~100 ms; the acquisition loop services requests
 * every poll (~30 ms). */
static bool mask_req(scope_mask_req_t k, uint8_t a, uint8_t b, uint16_t n)
{
    uint32_t seq = scope_mask_post(k, a, b, n);
    for (int i = 0; i < 20; i++) {
        if (scope_mask_acked(seq)) return true;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}

/* tol_v steps of 2 counts (1/16 division), tol_h steps of 1 sample. The
 * popup shows the tolerance the mask reports after the ack. */
/* Consecutive presses build on the request still in flight, not on the
 * applied value (2026-10-06). The acquisition task can sleep fill + 100 ms
 * after a handover -- longer than mask_req's 100 ms wait -- so a second press
 * read the old tolerance and re-asked for the same step (mask_bench H: two
 * UPs from 8 landed on 10, 2 of 3 runs). The mailbox keeps only the newest
 * request, so posting on top of a pending one is exactly right. */
static uint32_t tol_pending_seq;
static uint8_t  tol_pending_v, tol_pending_h;

static void mask_adjust_tol(int dv, int dh, char *pb, size_t n)
{
    scope_mask_summary_t m;
    scope_mask_summary(&m);
    int base_v = m.tol_v, base_h = m.tol_h;
    if (tol_pending_seq && !scope_mask_acked(tol_pending_seq)) {
        base_v = tol_pending_v;
        base_h = tol_pending_h;
    }
    int v = base_v + dv, h = base_h + dh;
    if (v < 0) v = 0;
    if (v > 64) v = 64;
    if (h < 0) h = 0;
    if (h > (int)MASK_PF_HMAX) h = (int)MASK_PF_HMAX;
    tol_pending_v = (uint8_t)v;
    tol_pending_h = (uint8_t)h;
    tol_pending_seq = scope_mask_post(SCOPE_MASK_REQ_TOL, (uint8_t)v, (uint8_t)h, 0);
    for (int i = 0; i < 20 && !scope_mask_acked(tol_pending_seq); i++)
        vTaskDelay(pdMS_TO_TICKS(5));
    if (!scope_mask_acked(tol_pending_seq)) {
        /* Not lost: the acquisition task applies it on its next poll. */
        snprintf(pb, n, "Mask V%u H%u ...", (unsigned)v, (unsigned)h);
        return;
    }
    scope_mask_summary(&m);
    snprintf(pb, n, "Mask V%u H%u", (unsigned)m.tol_v, (unsigned)m.tol_h);
}

/* AUTO in the scope time view: teach when there is no mask, cancel a teach
 * in progress, clear a finished mask. AUTO did nothing in the time view
 * before 2026-10-03 (it only acts in the FFT views). */
static void mask_auto_key(char *pb, size_t n)
{
    mask_pf_state_t st = scope_mask_state();
    if (st == MASK_PF_EMPTY) {
        if (!mask_req(SCOPE_MASK_REQ_TEACH, 0, 0, 0)) {
            snprintf(pb, n, "MASK: NOT SET");
        } else if (scope_mask_teach_refusal() != NULL) {
            /* The refusals a user can cause, in words that fit 24 chars. */
            const char *why = scope_mask_teach_refusal();
            snprintf(pb, n, "%s", strstr(why, "CH2") ? "MASK: needs CH1 trig" :
                                  strstr(why, "heap") ? "MASK: no memory" :
                                  strstr(why, "channel") ? "MASK: no channel on" :
                                  "MASK: refused");
        } else {
            scope_mask_summary_t m;
            scope_mask_summary(&m);
            snprintf(pb, n, "MASK: teach %u", (unsigned)m.teach_target);
        }
    } else {
        bool ok = mask_req(SCOPE_MASK_REQ_CLEAR, 0, 0, 0);
        if (scope_key_focus == KF_MASK) set_focus(KF_ZOOM);   /* the stage went with the mask */
        snprintf(pb, n, "%s", !ok ? "MASK: NOT SET" :
                              st == MASK_PF_TEACHING ? "MASK: cancelled" : "MASK: off");
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * Mode entry, long-press shortcuts and the scope key model
 * ═══════════════════════════════════════════════════════════════════ */

/* Enter a device mode: the one path for MENU's cycle and the long-press
 * shortcuts, so a mode is never entered half-way (the FPGA posture, the
 * settings list, the persistence flush). */
static void enter_mode(device_mode_t m, QueueHandle_t dq)
{
    meter_autoselect_cancel();
    current_mode = m;
    if (current_mode == MODE_SETTINGS) {
        settings_selected = 0;
        settings_depth = 0;
    }
    /* Tell the FPGA which mode we're entering.
     * Each mode needs different FPGA commands and analog MUX config. */
    if (current_mode == MODE_MULTIMETER) {
        fpga_request_meter_mode(meter_submode);   /* background: no frozen buttons */
    } else if (current_mode == MODE_SIGNAL_GEN) {
        fpga_enter_siggen_mode();
    } else if (current_mode == MODE_OSCILLOSCOPE) {
        fpga_enter_scope_mode();
    } else {
        GPIOC->clr = (1U << 11);  /* PC11 LOW — meter disable */
    }
    /* Leaving a mode, or backing out of the settings menu, is a deliberate
     * boundary: commit now rather than waiting out the settle window. */
    (void)settings_store_flush(settings_now_ms());
    send_cmd(dq, DCMD_REDRAW_ALL);
}

/* Measurement selection (short PRM in the scope time view). The measurement
 * item implements this; the default says "not handled", and input falls back
 * to the older PRM meaning (the FFT view cycle). */
__attribute__((weak)) bool scope_param_select(void)
{
    return false;
}

static key_ctx_t key_ctx_now(void)
{
    const scope_state_t *ss = scope_state_get();
    key_ctx_t c;
    c.mode           = current_mode;
    c.settings_depth = settings_depth;
#ifdef FEATURE_FFT
    c.scope_time_view = (current_mode == MODE_OSCILLOSCOPE && scope_view == SCOPE_VIEW_TIME);
#else
    c.scope_time_view = (current_mode == MODE_OSCILLOSCOPE);
#endif
    c.cursors_on     = (ss->cursor.mode != CURSOR_OFF);
    c.mask_present   = (scope_mask_state() != MASK_PF_EMPTY);
    /* A stage that no longer exists (mask cleared from the shell, cursors
     * switched off) is dropped here, before it can steer an arrow press. */
    const key_focus_t f = key_focus_sanitize(scope_key_focus, c.cursors_on, c.mask_present);
    if (f != scope_key_focus) set_focus(f);
    c.focus          = f;
    c.active_channel = active_channel;
    return c;
}

/* Long AUTO/OK/SAVE: stock's baseline correction, 50 % centre and screenshot
 * gallery. Other parity items fill these in; until then say so. */
static const char *long_unused_text(button_id_t b)
{
    switch (b) {
    case BTN_AUTO: return "Long AUTO: not built";
    case BTN_OK:   return "Long OK: not built";
    default:       return "Long SAVE: not built";
    }
}

/* Execute one answer of key_map(). Returns the display command sent. */
static uint8_t exec_key_cmd(key_cmd_t k, QueueHandle_t dq)
{
    scope_state_t *ss = scope_state_get();
    uint8_t cmd = DCMD_REDRAW_ALL;
    char pb[24];
    channel_state_t *ach = (active_channel == 0) ? &ss->ch1 : &ss->ch2;
    const int dir = k.arg;

    switch (k.act) {
    case KA_NONE:
    case KA_LEGACY:
        break;

    /* -- long presses: mode shortcuts, from any mode -------------------- */
    case KA_ENTER_HOME:
        /* No home page exists in OpenScope; the settings list is the nearest
         * thing to stock's "function selection page". From inside a settings
         * page it goes back to the top of the list. */
        if (current_mode == MODE_SETTINGS) {
            settings_depth = 0;
            settings_sub_selected = 0;
            send_cmd(dq, cmd);
        } else {
            enter_mode(MODE_SETTINGS, dq);
        }
        break;
    case KA_ENTER_METER:
        if (current_mode != MODE_MULTIMETER) enter_mode(MODE_MULTIMETER, dq);
        break;
    case KA_ENTER_SCOPE:
        if (current_mode != MODE_OSCILLOSCOPE) enter_mode(MODE_OSCILLOSCOPE, dq);
        break;
    case KA_ENTER_SIGGEN:
        if (current_mode != MODE_SIGNAL_GEN) enter_mode(MODE_SIGNAL_GEN, dq);
        break;

    case KA_MEAS_HIDE_TOGGLE:
        ss->prm_hidden = !ss->prm_hidden;
        popup_and_redraw(dq, ss->prm_hidden ? "Measurements hidden" : "Measurements shown");
        break;

    case KA_CH_ENABLE_TOGGLE: {
        /* Same path as the shell's `fpga scope ch`: flip, apply the converter
         * routing, and put it back if the hardware refuses (both off). The
         * popup reports the routing in force, not the request: CH2 off at a
         * fast timebase is what gives the interleaved 250 MS/s record. */
        channel_state_t *c = (dir == 0) ? &ss->ch1 : &ss->ch2;
        const bool was = c->enabled;
        c->enabled = !was;
        if (!fpga_apply_channel_enables()) {
            c->enabled = was;
            popup_and_redraw(dq, "Keep one CH on");
            break;
        }
        if (!c->enabled && (active_channel == (uint8_t)dir))
            active_channel = (uint8_t)(1u - (uint8_t)dir);   /* the arrows follow the live one */
        snprintf(pb, sizeof(pb), "CH%d %s%s", dir + 1, c->enabled ? "ON" : "OFF",
                 fpga_interleave_active() ? " 250MS/s" : "");
        popup_and_redraw(dq, pb);
        break;
    }

    case KA_LONG_UNUSED:
        popup_and_redraw(dq, long_unused_text((button_id_t)dir));
        break;

    /* -- short presses -------------------------------------------------- */
    case KA_FOCUS_NEXT: {
        const key_focus_t f = key_focus_next(
            scope_key_focus, ss->cursor.mode != CURSOR_OFF,
            scope_mask_state() != MASK_PF_EMPTY);
        set_focus(f);
        popup_and_redraw(dq, key_focus_popup(f));
        break;
    }

    case KA_ADJ_CH_SWITCH: {
        /* "Switch CH1/CH2 Adjustment": which channel UP/DOWN act on. A
         * channel that is off is not offered: it has nothing to adjust. */
        const uint8_t other = (uint8_t)(1u - active_channel);
        const channel_state_t *oc = (other == 0) ? &ss->ch1 : &ss->ch2;
        if (oc->enabled) {
            active_channel = other;
            snprintf(pb, sizeof(pb), "Adjust CH%d", other + 1);
        } else {
            snprintf(pb, sizeof(pb), "CH%d is off", other + 1);
        }
        popup_and_redraw(dq, pb);
        break;
    }

    case KA_PARAM_SELECT:
        /* Only reached when scope_param_select() took it (see the caller). */
        send_cmd(dq, cmd);
        break;

    case KA_TB_STEP: {
        /* Past the last FPGA code the ladder continues into roll (C11,
         * stock 0x14..0x1C): a roll change re-applies roll, anything else
         * the Y-T code. Holding at the slowest roll step changes nothing. */
        const bool was_roll = ss->roll;
        const uint8_t was_step = ss->roll_step;
        const bool roll_changed = scope_adjust_timebase(ss, dir);
        /* Push it to the FPGA. Before 2026-08-19 this line did not exist
         * and the button only relabelled the axis while the hardware kept
         * sampling at whatever the arm block left in reg 0x01 — so the
         * derived s/div below was a confident, wrong number. If the write
         * fails the rate did NOT change, so say so rather than print a
         * label for a rate that is not in force. */
        const bool tb_ok = roll_changed ? fpga_apply_roll()
                         : (was_roll ? true : fpga_apply_timebase(ss->timebase_idx));
        if (roll_changed && !tb_ok) {
            /* Nothing was written. Unlike a Y-T code, a later press need not
             * re-apply (the slowest step holds), so put the state back. */
            ss->roll = was_roll;
            ss->roll_step = was_step;
        }
        char tb[12];
        if (ss->roll)
            scope_roll_label(ss->roll_step, tb, sizeof(tb));
        else
            scope_timebase_label(ss->timebase_idx, tb, sizeof(tb));
        /* Short on purpose: pb is 24 bytes and a truncated warning is
         * worse than a terse one. */
        snprintf(pb, sizeof(pb), tb_ok ? "H=%s/div" : "H=%s NOT SET", tb);
        popup_and_redraw(dq, pb);
        break;
    }

    case KA_VDIV_STEP: {
        scope_adjust_vdiv(ach, dir);
        /* Push it to the RELAYS. Before 2026-08-20 (EXP-19) this line did
         * not exist and the button only changed the label and the
         * counts->volts k while the frontend stayed at the boot range —
         * the decorative-timebase-button bug (EXP-17), on the axis where
         * it prints a confidently wrong VOLTAGE. */
        const bool vd_ok = fpga_apply_vdiv((uint8_t)(active_channel + 1), ach->vdiv_idx);
        /* Same derived label the status bar shows — see scope_cal.h.
         * Two different volts/div strings for one range would be worse
         * than the wrong one we replaced. */
        char vd[12];
        scope_cal_range_label((uint8_t)(active_channel + 1), ach->vdiv_idx, vd, sizeof(vd));
        snprintf(pb, sizeof(pb), vd_ok ? "CH%d %s/div" : "CH%d %s NOT SET",
                 active_channel + 1, vd);
        popup_and_redraw(dq, pb);
        break;
    }

    case KA_CHPOS_STEP:
        /* Stock's channel move: the vertical position drives the channel's
         * offset DAC from the calibration page (scope_vpos, parity C4). With no
         * usable page scope_vpos_set() is a no-op and only the ground marker
         * moves. fpga.c keeps the 0x1B/0x1D offset bytes pinned at 128. */
        scope_adjust_position(ach, dir);
        scope_vpos_set((uint8_t)(active_channel + 1), ach->position);
        snprintf(pb, sizeof(pb), "CH%d pos %+d", active_channel + 1, (int)ach->position);
        popup_and_redraw(dq, pb);
        break;

    case KA_TRIGCOL_STEP: {
        /* Move the trigger point across the screen, 16 px per press.
         * The popup reports the column it was ASKED for; the marker at
         * the top shows where it actually landed. */
        int tx = ss->trig_x + dir * 16;
        if (tx < 8) tx = 8;
        if (tx > 312) tx = 312;
        ss->trig_x = (int16_t)tx;
        snprintf(pb, sizeof(pb), "Trig at x=%d", tx);
        popup_and_redraw(dq, pb);
        break;
    }

    case KA_TRIGLVL_STEP: {
        scope_adjust_trigger_level(ss, dir);
        /* Through the ONE writer of reg 0x08, so the register, the
         * persisted level and the marker cannot diverge. The popup shows
         * the code the firmware records as in force, not a recomputation:
         * the inert-controls lesson is to report the wire, not the
         * setter's intent. */
        const bool lv_ok = fpga_apply_trigger_level(ss->trigger.level);
        snprintf(pb, sizeof(pb), lv_ok ? "Trig %+d  code %u" : "Trig %+d  NOT SET",
                 ss->trigger.level, (unsigned)fpga_acq_trig_code_get());
        popup_and_redraw(dq, pb);
        break;
    }

    case KA_CURSOR_MOVE:
        scope_cursor_move((int16_t)(dir * 4));
        cmd = DCMD_DRAW_SCOPE;
        send_cmd(dq, cmd);
        break;

    case KA_CURSOR_NEXT:
        scope_cursor_next_sel();
        cmd = DCMD_DRAW_SCOPE;
        send_cmd(dq, cmd);
        break;

    case KA_MASK_TOL_V:
        mask_adjust_tol(dir * 2, 0, pb, sizeof(pb));
        popup_and_redraw(dq, pb);
        break;

    case KA_MASK_TOL_H:
        mask_adjust_tol(0, dir, pb, sizeof(pb));
        popup_and_redraw(dq, pb);
        break;

    case KA_MASK_STOPFAIL: {
        scope_mask_summary_t m;
        scope_mask_summary(&m);
        bool ok = mask_req(SCOPE_MASK_REQ_STOPFAIL, m.stop_on_fail ? 0u : 1u, 0, 0);
        scope_mask_summary(&m);
        snprintf(pb, sizeof(pb), ok ? "Mask stop-on-fail %s" : "Mask stop NOT SET",
                 m.stop_on_fail ? "ON" : "OFF");
        popup_and_redraw(dq, pb);
        break;
    }
    }
    return cmd;
}

/* Settings persistence, once per button, after the handler has had its
 * effect. note_change() is a struct compare — no flash access at all when
 * nothing changed, which is the overwhelming majority of presses.
 * service() writes only once a change has been quiet for the settle
 * window, so holding an arrow key through ten values costs one record. */
static void settings_after_button(void)
{
    uint32_t now = settings_now_ms();
    settings_store_note_change(now);
    (void)settings_store_service(now);
}

/* ═══════════════════════════════════════════════════════════════════
 * Main button dispatcher
 * ═══════════════════════════════════════════════════════════════════ */

uint8_t input_handle_button(button_id_t button, QueueHandle_t dq)
{
    scope_state_t *ss = scope_state_get();
    uint8_t cmd = DCMD_REDRAW_ALL;
    char pb[24];

    /* A long press arrives as the id with BTN_LONG_FLAG set. It is handled
     * FIRST, before the meter's softkey catch below: long SELECT/MOVE/TRIGGER
     * are the quick mode switches and must work from the meter too. */
    if (((unsigned)button & BTN_LONG_FLAG) != 0u) {
        const key_ctx_t kc = key_ctx_now();
        const key_cmd_t k = key_map(&kc, (button_id_t)((unsigned)button & ~(unsigned)BTN_LONG_FLAG), true);
        cmd = exec_key_cmd(k, dq);
        settings_after_button();
        return cmd;
    }

    /* Softkeys (docs/specs/platform/softkey-ui.md): in the meter, the four
     * buttons under the screen do what the bar above them says. Caught here,
     * before the per-button cases, so the meter never falls into another
     * mode's meaning for MOVE/SELECT/TRIGGER/PRM. */
    if (current_mode == MODE_MULTIMETER) {
        int8_t sk = softkey_slot_for_button(button);
        if (sk >= 0) {
            (void)meter_softkey_press(sk);
            cmd = DCMD_DRAW_METER;
            send_cmd(dq, cmd);
            /* Same settings hook as the end of this function: a softkey
             * changes saved state (function, view) like any other button. */
            uint32_t now = settings_now_ms();
            settings_store_note_change(now);
            (void)settings_store_service(now);
            return cmd;
        }
    }

    /* The scope time view's key meanings (zoom/move stages, channel switch,
     * measurement selection): key_model.c decides, exec_key_cmd() acts.
     * Anything it does not claim falls through to the per-mode handlers. */
    {
        const key_ctx_t kc = key_ctx_now();
        key_cmd_t k = key_map(&kc, button, false);
        if (k.act == KA_PARAM_SELECT && !scope_param_select())
            k.act = KA_LEGACY;       /* nothing selects measurements yet: PRM keeps its old job */
        if (k.act != KA_LEGACY) {
            cmd = exec_key_cmd(k, dq);
            settings_after_button();
            return cmd;
        }
    }

    switch (button) {

    /* -- Mode / Navigation ---------------------------------------- */

    case BTN_MENU:
        if (current_mode == MODE_SETTINGS && settings_depth > 0) {
            meter_autoselect_cancel();
            settings_depth = 0;
            settings_sub_selected = 0;
            (void)settings_store_flush(settings_now_ms());
            send_cmd(dq, cmd);
        } else {
            enter_mode((device_mode_t)((current_mode + 1) % MODE_COUNT), dq);
        }
        break;

    /* -- Channel buttons ------------------------------------------ */

    case BTN_CH1:
        if (current_mode == MODE_OSCILLOSCOPE) {
            active_channel = 0;
            channel_cycle_coupling(0, &ss->ch1, "CH1", pb, sizeof(pb));
            popup_and_redraw(dq, pb);
        } else {
            send_cmd(dq, cmd);
        }
        break;

    case BTN_CH2:
        if (current_mode == MODE_OSCILLOSCOPE) {
            active_channel = 1;
            channel_cycle_coupling(1, &ss->ch2, "CH2", pb, sizeof(pb));
            popup_and_redraw(dq, pb);
        } else {
            send_cmd(dq, cmd);
        }
        break;

    /* -- Trigger -------------------------------------------------- */

    case BTN_TRIGGER:
        if (current_mode == MODE_OSCILLOSCOPE) {
#ifdef FEATURE_FFT
            if (scope_view == SCOPE_VIEW_TIME)
#endif
            {
                /* Toggle cursor mode in time view */
                scope_cursor_cycle_mode();
                /* Cursors on = the arrows move them, as before the key model;
                 * cursors off = back to zoom. */
                set_focus(key_focus_after_cursor_change(scope_key_focus,
                                                        ss->cursor.mode != CURSOR_OFF));
                send_cmd(dq, cmd);
                break;
            }
            /* Fall through for non-time views: cycle trigger mode */
            scope_cycle_trigger_mode(ss);
            snprintf(pb, sizeof(pb), "Trig: %s",
                     trigger_mode_labels[ss->trigger.mode]);
            popup_and_redraw(dq, pb);
        } else {
            send_cmd(dq, cmd);
        }
        break;

    case BTN_MOVE:
        /* Scope: key_model.c answers it (KA_FOCUS_NEXT, executed above). What
         * is left is the other modes, where it only repaints; the meter's
         * softkey meaning was caught at the top. */
        send_cmd(dq, cmd);
        break;

    /* -- Save (screenshot) ---------------------------------------- */

    case BTN_SAVE:
#if defined(FPGA_BENCH_SAVE_PC12) && FPGA_BENCH_SAVE_PC12
        /* BENCH IMAGES ONLY (guest-warmtest*). Until 2026-10-03 this was gated
         * on FPGA_WARM_HANDOFF_TEST, which the release image (guest-coldtrace)
         * also defines -- so in v0.4.0 SAVE silently flipped CH1's input PATH
         * relay (PC12: direct vs ~30x attenuated, set per range by the range
         * table), leaving the volts/div calibration wrong until the next range
         * change. It now needs its own flag.
         *
         * Warm-handoff bench build: SAVE toggles the input-routing relay
         * PC12 for a live A/B. Bench run 4 (2026-08-12) showed the input
         * path connected but AC-coupled — finger noise passes, DC blocked —
         * and PC12 (driven LOW by our approximate range table) is the
         * suspected coupling/routing select. Pure GPIO relay write (Exp C:
         * signal relay, not a config strap); costs nothing to flip live. */
        if (current_mode == MODE_OSCILLOSCOPE) {
            if (GPIOC->odt & (1U << 12)) {
                GPIOC->clr = (1U << 12);
                scope_show_popup("PC12: LOW");
            } else {
                GPIOC->scr = (1U << 12);
                scope_show_popup("PC12: HIGH");
            }
            send_cmd(dq, cmd);
            break;
        }
#endif
#if defined(FPGA_PIN_SWEEP_BUILD) && FPGA_PIN_SWEEP_BUILD
        /* Bench build: SAVE runs the RECONFIG_N candidate pin sweep instead of
         * its normal action. Gated to a button and run from here — NOT from
         * fpga_init — so a bad pulse costs a power-cycle rather than a failed
         * boot; three failed boots latch the bootloader into SAFE MODE, which
         * this unit has already done twice. Re-runnable without a reflash. */
        if (current_mode == MODE_OSCILLOSCOPE) {
            scope_show_popup("PIN SWEEP...");
            fpga_reconfig_pin_sweep();
            send_cmd(dq, cmd);
            break;
        }
#endif
        if (current_mode == MODE_MULTIMETER) {
            meter_toggle_debug_overlay();
            send_cmd(dq, cmd);
        } else {
            /* There is no on-device screenshot store: src/util/screenshot.c has
             * no caller, and writing the stock FAT volume is not implemented.
             * Until 2026-10-03 this printed "SAVED #n" and wrote nothing
             * (audit P3, dev plan 2.6) -- a confirmation of something that
             * never happened. Say so instead. Screenshots work over USB:
             * scripts/screenshot.py (CRC-verified, needs a held screen). */
            scope_show_popup("SAVE: not in this build");
            send_cmd(dq, cmd);
        }
        break;

    /* -- Auto ----------------------------------------------------- */

    case BTN_AUTO:
        if (current_mode == MODE_MULTIMETER) {
            meter_autoselect_status_t st;
            meter_autoselect_get_status(&st);
            if (st.state == METER_AUTOSELECT_RUNNING) {
                meter_autoselect_cancel();
                scope_show_popup("AUTO CANCEL");
            } else if (meter_autoselect_start(700U)) {
                scope_show_popup("AUTO DMM");
            } else {
                scope_show_popup("AUTO ERR");
            }
            cmd = DCMD_DRAW_METER;
            send_cmd(dq, cmd);
        } else if (current_mode == MODE_OSCILLOSCOPE) {
#ifdef FEATURE_FFT
            if (scope_view != SCOPE_VIEW_TIME) {
                /* Auto-configure the spectrum view on what it actually
                 * analyses: the live CH1 record (head skipped, as the views
                 * do) when there is one, the demo signal only otherwise —
                 * and never in the live-capture builds, where the demo is
                 * compiled out (scope_ui.c fft_prepare_input). */
                int16_t *sbuf = fft_get_sample_buf();
                const volatile uint8_t *b = fpga_get_ch1_buf();
                if (sbuf && fpga_data_ready() && b != NULL) {
                    uint16_t n = fft_live_prepare((const uint8_t *)b,
                                                  FPGA_ADC_BUF_SIZE,
                                                  sbuf, FFT_SIZE);
                    if (n > 0)
                        fft_auto_configure(sbuf, n);
                }
#if !(defined(FPGA_WARM_HANDOFF_TEST) && FPGA_WARM_HANDOFF_TEST)
                else if (sbuf) {
                    test_signal_generate(TEST_SIG_SQUARE, sbuf,
                                         FFT_SIZE, fft_get_config()->sample_rate_hz,
                                         1000.0f, 0.0f, 0.8f);
                    fft_auto_configure(sbuf, FFT_SIZE);
                }
#endif
                send_cmd(dq, cmd);
                break;
            }
#endif
            /* Time view: the mask key (teach / cancel / clear). */
            mask_auto_key(pb, sizeof(pb));
            popup_and_redraw(dq, pb);
        }
        break;

    /* -- PRM / SELECT --------------------------------------------- */

#ifdef FEATURE_FFT
    case BTN_PRM:
        if (current_mode == MODE_OSCILLOSCOPE) {
            scope_view_t prev_view = scope_view;
            scope_view = (scope_view_t)((scope_view + 1) % SCOPE_VIEW_COUNT);

            /* Pool lifecycle: claim FFT pool when entering FFT views,
             * release when returning to time-domain */
            if (prev_view == SCOPE_VIEW_TIME && scope_view != SCOPE_VIEW_TIME) {
                /* Entering FFT view — claim pool */
                fft_config_t fft_cfg;
                fft_cfg.window         = FFT_WINDOW_HANNING;
                fft_cfg.sample_rate_hz = 44100.0f;
                fft_cfg.ref_level_db   = 0.0f;
                fft_cfg.db_range       = 80.0f;
                fft_cfg.peak_count     = 4;
                fft_cfg.avg_count      = 0;
                fft_cfg.max_hold       = false;
                fft_cfg.zoom_start_bin = 1;
                fft_cfg.zoom_end_bin   = FFT_BINS - 1;
                fft_init(&fft_cfg);
            } else if (prev_view != SCOPE_VIEW_TIME && scope_view == SCOPE_VIEW_TIME) {
                /* Returning to time-domain — release pool */
                fft_deinit();
            }
            send_cmd(dq, cmd);
        } else if (current_mode == MODE_SIGNAL_GEN) {
            /* Cycle frequency presets up to stock's 50 kHz ceiling:
             * 1 -> 10 -> 100 -> 1k -> 10k -> 50k -> 1 Hz. Arbitrary values
             * go through the shell's `siggen freq` until the digit-cursor
             * editor (parity G4) lands. */
            const siggen_config_t *sc = siggen_get_config();
            uint32_t f = sc->frequency_hz;
            if (f < 10u) f = 10u;
            else if (f < 100u) f = 100u;
            else if (f < 1000u) f = 1000u;
            else if (f < 10000u) f = 10000u;
            else if (f < SIGGEN_FREQ_MAX_HZ) f = SIGGEN_FREQ_MAX_HZ;
            else f = 1u;
            siggen_set_frequency(f);
            /* DCMD_DRAW_SIGGEN, not the default DCMD_REDRAW_ALL: the
             * generator panel owns rows 16..223 and repaints them itself,
             * so a full-screen clear + status/info bar repaint on every
             * adjustment was pure flicker. The display task also gates
             * DCMD_DRAW_SIGGEN on the settings actually moving, which
             * REDRAW_ALL cannot be gated on (it is the hard invalidate —
             * see main.c). Applies to all seven siggen adjustments. */
            cmd = DCMD_DRAW_SIGGEN;
            send_cmd(dq, cmd);
        }
        break;

    case BTN_SELECT:
        if (current_mode == MODE_OSCILLOSCOPE &&
            scope_view != SCOPE_VIEW_TIME) {
            fft_cycle_window();
            send_cmd(dq, cmd);
        } else
#endif
        if (current_mode == MODE_SIGNAL_GEN) {
            siggen_cycle_waveform();
            cmd = DCMD_DRAW_SIGGEN;
            send_cmd(dq, cmd);
        } else if (current_mode == MODE_SETTINGS &&
                   settings_depth == 5) {
            /* Resistor calc: simulate measurement */
            resistor_calc_simulate_measure();
            cmd = DCMD_DRAW_SETTINGS;
            send_cmd(dq, cmd);
        } else if (current_mode == MODE_SETTINGS &&
                   settings_depth == 4) {
            /* Component tester: cycle component type */
            comp_test_cycle_type();
            cmd = DCMD_DRAW_SETTINGS;
            send_cmd(dq, cmd);
        }
        /* Scope time view: SELECT switches the adjusted channel -- key_model.c */
        break;

    /* -- UP / DOWN ------------------------------------------------ */

    case BTN_UP:
        if (current_mode == MODE_SETTINGS) {
            if (settings_depth == 5) {
                resistor_calc_change_color(1);
            } else if (settings_depth == 0) {
                if (settings_selected > 0) settings_selected--;
            } else if (settings_depth == 3) {
                if (settings_sub_selected > 0) settings_sub_selected--;
            } else if (settings_depth == 1) {
                if (settings_sub_selected > 0) settings_sub_selected--;
            }
            cmd = DCMD_DRAW_SETTINGS;
            send_cmd(dq, cmd);
        }
#ifdef FEATURE_FFT
        else if (current_mode == MODE_OSCILLOSCOPE &&
                 scope_view != SCOPE_VIEW_TIME) {
            fft_adjust_ref_level(5.0f);
            send_cmd(dq, cmd);
        }
#endif
        else if (current_mode == MODE_MULTIMETER &&
                 meter_layout == METER_LAYOUT_FUSE) {
            fuse_next_rating();   /* UP = larger */
            send_cmd(dq, cmd);
        }
        else if (current_mode == MODE_MULTIMETER &&
                 meter_layout == METER_LAYOUT_LIMITS) {
            meter_limits_nudge(+1);
            cmd = DCMD_DRAW_METER;
            send_cmd(dq, cmd);
        }
        else if (current_mode == MODE_SIGNAL_GEN) {
            siggen_amplitude_up();
            cmd = DCMD_DRAW_SIGGEN;
            send_cmd(dq, cmd);
        }
        break;

    case BTN_DOWN:
        if (current_mode == MODE_SETTINGS) {
            if (settings_depth == 5) {
                resistor_calc_change_color(-1);
            } else if (settings_depth == 0) {
                if (settings_selected < SETTINGS_ITEM_COUNT - 1)
                    settings_selected++;
            } else if (settings_depth == 3) {
                /* Math/persist sub-menu: 3 items (0,1,2) */
                if (settings_sub_selected < 2) settings_sub_selected++;
            } else if (settings_depth == 1) {
                if (settings_sub_selected < SETTINGS_OSC_ITEM_COUNT - 1)
                    settings_sub_selected++;
            }
            cmd = DCMD_DRAW_SETTINGS;
            send_cmd(dq, cmd);
        }
#ifdef FEATURE_FFT
        else if (current_mode == MODE_OSCILLOSCOPE &&
                 scope_view != SCOPE_VIEW_TIME) {
            fft_adjust_ref_level(-5.0f);
            send_cmd(dq, cmd);
        }
#endif
        else if (current_mode == MODE_MULTIMETER &&
                 meter_layout == METER_LAYOUT_FUSE) {
            fuse_prev_rating();
            send_cmd(dq, cmd);
        }
        else if (current_mode == MODE_MULTIMETER &&
                 meter_layout == METER_LAYOUT_LIMITS) {
            meter_limits_nudge(-1);
            cmd = DCMD_DRAW_METER;
            send_cmd(dq, cmd);
        }
        else if (current_mode == MODE_SIGNAL_GEN) {
            siggen_amplitude_down();
            cmd = DCMD_DRAW_SIGGEN;
            send_cmd(dq, cmd);
        }
        break;

    /* -- LEFT / RIGHT --------------------------------------------- */

    case BTN_LEFT:
        if (current_mode == MODE_SETTINGS && settings_depth == 0 &&
            (settings_selected == 3 || settings_selected == 7 || settings_selected == 12)) {
            if (settings_selected == 3)
                display_mode_step(-1);
            else if (settings_selected == 12)
                theme_cycle_reverse();
            else
                startup_mode_adjust(-1);
            cmd = DCMD_DRAW_SETTINGS;
            send_cmd(dq, cmd);
        }
        else if (current_mode == MODE_SETTINGS && settings_depth == 1) {
            osc_settings_adjust(-1);
            cmd = DCMD_DRAW_SETTINGS;
            send_cmd(dq, cmd);
        }
        else if (current_mode == MODE_SETTINGS && settings_depth == 5) {
            resistor_calc_move_band(-1);
            cmd = DCMD_DRAW_SETTINGS;
            send_cmd(dq, cmd);
        }
        /* Meter: fuse type or previous sub-mode */
        else if (current_mode == MODE_MULTIMETER) {
            if (meter_layout == METER_LAYOUT_FUSE) {
                fuse_prev_type();
            } else {
                meter_function_step(-1);
            }
            send_cmd(dq, cmd);
        }
#ifdef FEATURE_FFT
        else if (current_mode == MODE_OSCILLOSCOPE &&
            scope_view != SCOPE_VIEW_TIME) {
            fft_zoom_in();
            send_cmd(dq, cmd);
        }
#endif
        else if (current_mode == MODE_SIGNAL_GEN) {
            siggen_duty_cycle_down();
            cmd = DCMD_DRAW_SIGGEN;
            send_cmd(dq, cmd);
        }
        break;

    case BTN_RIGHT:
        if (current_mode == MODE_SETTINGS && settings_depth == 0 &&
            (settings_selected == 3 || settings_selected == 7 || settings_selected == 12)) {
            if (settings_selected == 3)
                display_mode_step(+1);
            else if (settings_selected == 12)
                theme_cycle();
            else
                startup_mode_adjust(1);
            cmd = DCMD_DRAW_SETTINGS;
            send_cmd(dq, cmd);
        }
        else if (current_mode == MODE_SETTINGS && settings_depth == 1) {
            osc_settings_adjust(+1);
            cmd = DCMD_DRAW_SETTINGS;
            send_cmd(dq, cmd);
        }
        else if (current_mode == MODE_SETTINGS && settings_depth == 5) {
            resistor_calc_move_band(1);
            cmd = DCMD_DRAW_SETTINGS;
            send_cmd(dq, cmd);
        }
        /* Meter: fuse type or next sub-mode */
        else if (current_mode == MODE_MULTIMETER) {
            if (meter_layout == METER_LAYOUT_FUSE) {
                fuse_next_type();
            } else {
                meter_function_step(+1);
            }
            send_cmd(dq, cmd);
        }
#ifdef FEATURE_FFT
        else if (current_mode == MODE_OSCILLOSCOPE &&
            scope_view != SCOPE_VIEW_TIME) {
            fft_zoom_out();
            send_cmd(dq, cmd);
        }
#endif
        else if (current_mode == MODE_SIGNAL_GEN) {
            siggen_duty_cycle_up();
            cmd = DCMD_DRAW_SIGGEN;
            send_cmd(dq, cmd);
        }
        break;

    /* -- OK ------------------------------------------------------- */

    case BTN_OK:
        if (current_mode == MODE_SETTINGS) {
            input_handle_settings_ok();
            send_cmd(dq, cmd);
        } else if (current_mode == MODE_SIGNAL_GEN) {
            const siggen_config_t *sc = siggen_get_config();
            siggen_enable(!sc->output_enabled);
            cmd = DCMD_DRAW_SIGGEN;
            send_cmd(dq, cmd);
        } else if (current_mode == MODE_MULTIMETER) {
            /* OK = Hold (softkey UI): freeze the reading on screen now. */
            meter_hold_toggle_now();
            cmd = DCMD_DRAW_METER;
            send_cmd(dq, cmd);
        } else if (current_mode == MODE_OSCILLOSCOPE) {
            if (scope_mask_hold_active()) {
                /* A mask failure is being held (stop-on-fail): OK resumes
                 * testing rather than toggling RUN/STOP, which would leave
                 * the acquisition frozen behind a STOP the user never set. */
                (void)scope_mask_post(SCOPE_MASK_REQ_RELEASE, 0, 0, 0);
                scope_show_popup("MASK: RESUME");
            } else {
                scope_toggle_running(ss);
                scope_show_popup(ss->running ? "RUN" : "STOP");
            }
            send_cmd(dq, cmd);
        }
        break;

    /* -- Power ---------------------------------------------------- */

    case BTN_POWER:
        {
            const theme_t *th = theme_get();
            uint16_t bg = 0x0010;  /* dark background */

            /* MENU + POWER → clean handoff to the factory IAP bootloader.
             * The FNIRSI bootloader at 0x08000000 checks the MENU key at
             * reset; the user is holding it, so a plain reset (NOT the DFU
             * magic-word path — that was for our retired HID bootloader)
             * lands in stock upgrade mode. Replaces the old behavior where
             * POWER alone hit the shutdown countdown and MENU+POWER froze.
             * The bootloader draws its own "firmware upgrade" screen, so no
             * delay/message here — reset immediately while MENU is still down. */
            if (button_scan_get_raw() & BTN_SCAN_MENU_MASK) {
                /* Reboot boundary — same reasoning as the DFU entry above. */
                (void)settings_store_flush(settings_now_ms());
                __DSB();
                NVIC_SystemReset();
                while (1) { }
            }

            /* Claim the screen. The display task keeps repainting live modes
             * every 50 ms tick, so an overlay drawn inline from this task was
             * overdrawn within one frame in scope mode ("flashing countdown",
             * 2026-08-20). ui_modal_active makes the display task drop all
             * rendering; the short delay lets a frame already in flight
             * finish before we paint over it. */
            ui_modal_active = true;
            vTaskDelay(pdMS_TO_TICKS(120));

            /* Countdown 3..2..1 while POWER button held. The full overlay is
             * repainted each second (cheap: one rect + two strings) so even a
             * frame that slipped past the modal claim heals within 1 s. */
            for (int countdown = 3; countdown > 0; countdown--) {
                char digit[2] = { '0' + countdown, '\0' };
                lcd_fill_rect(60, 80, 200, 80, bg);
                font_draw_string_center(160, 88, "Hold to power off",
                                        th->warning, bg, &font_medium);
                font_draw_string_center(160, 115, digit,
                                        th->text_primary, bg, &font_large);

                /* Wait 1 second, polling button every 50ms */
                for (int i = 0; i < 20; i++) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                    /* PC8 active LOW — if released, cancel */
                    if (GPIOC->idt & (1U << 8)) {
                        /* Button released — cancel, hand the screen back */
                        ui_modal_active = false;
                        send_cmd(dq, DCMD_REDRAW_ALL);
                        goto power_done;
                    }
                }
            }

            /* Still held after 3 seconds — power off */
            lcd_fill_rect(60, 80, 200, 80, bg);
            font_draw_string_center(160, 110, "Goodbye!",
                                    th->success, bg, &font_large);
            vTaskDelay(pdMS_TO_TICKS(500));

            /* Last chance to persist: an orderly power-off is the one place we
             * KNOW the settings are about to stop existing in RAM. Runs behind
             * the "Goodbye!" frame so its SPI2 traffic is invisible to the
             * user, and before the rail drops. */
            (void)settings_store_flush(settings_now_ms());

            /* Drop PC9 LOW — device powers off */
            GPIOC->clr = (1U << 9);

            /* Should never reach here */
            while (1);
        }
        power_done:
        break;

    default:
        break;
    }

    settings_after_button();

    return cmd;
}
