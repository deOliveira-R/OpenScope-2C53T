/*
 * OpenScope 2C53T - Input Handler
 *
 * All button-to-action mapping logic lives here.
 * The input task (in main.c) handles GPIO polling and debounce,
 * then calls input_handle_button() for the actual logic.
 *
 * Integrated features from 4 agent worktrees:
 *   - Cursor measurement mode (the Oscilloscope Settings page's Cursors
 *     row; TRIGGER opens its Trigger pane, osc_menu_model.h)
 *   - Meter sub-modes (LEFT/RIGHT cycle, SELECT resets min/max/avg)
 *   - Signal gen: amplitude stepping, duty cycle, frequency presets
 *   - Math channel, persistence, component tester (settings sub-menus)
 */

#include "../ui/scope_mask.h"
#include "key_model.h"
#include "trig_menu.h"
#include "osc_menu_model.h"
#include "input_handler.h"
#include "scope_apply.h"
#include "ui.h"
#include "softkey.h"
#include "lcd.h"
#include "font.h"
#include "scope_state.h"
#include "scope_cal.h"
#include "../ui/scope_timebase.h"
#include "../ui/scope_roll.h"
#include "../ui/scope_geom.h"
#include "signal_gen.h"
#include "../ui/siggen_edit.h"
#include "../ui/home_model.h"
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
#include "scope_selfcal_run.h"
#include "meter_autoselect.h"
#include "fpga_meter_plan.h"
#include "at32f403a_407.h"
#include "button_scan.h"
#include "settings_store.h"
#include "sys_prefs.h"
#include "sys_pwm.h"
#include "../ui/gallery_ui.h"
#include "../ui/screen.h"
#include "../ui/legacy_screens.h"
#include "model.h"           /* the long mode keys set sys.mode */
#if defined(USB_MSC) && USB_MSC
#include "usb_debug.h"      /* Settings "USB Sharing" */
#endif
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

/* ── System preferences (parity C15) ─────────────────────────────────
 * Auto power-off: stock resets its idle counter on every handled key event
 * (ms[0xF6C], 0x08040038) and at 15/30/60 min saves the settings and drops
 * PC9. The count is wall time here (stock's tick period is unproven [L]).
 * The pure timer is sys_prefs.c; this file owns the keys and the actual
 * power-off, because only it may flush settings (see settings_store.h). */
static sys_autooff_t g_autooff;
static bool          g_autooff_ready;

static void autooff_touch(void)
{
    uint32_t now = settings_now_ms();
    if (!g_autooff_ready) {
        sys_autooff_init(&g_autooff, sys_prefs_autooff_mins(), now);
        g_autooff_ready = true;
    }
    sys_autooff_activity(&g_autooff, now);
}

/* The one orderly power-off: flush settings FIRST (the rail is about to stop),
 * backlight to 0 like stock's saver (0x080293EE), then release the PC9 hold. */
static void power_off_now(void)
{
    (void)settings_store_flush(settings_now_ms());
    sys_pwm_backlight_off();
    GPIOC->clr = (1U << 9);
    while (1) { }
}

/* A setting changed from the shell (e.g. `mode startup`) has no key press
 * behind it, so settings_after_button() never captures it and a reflash right
 * after loses it. The shell only raises this flag; the input task stays the
 * one writer of the settings record (settings_store.h), and flushes on its
 * next pass (<= 100 ms). */
static volatile bool g_settings_flush_req;
/* Outcome hand-back for input_settings_flush_sync(): the input task bumps
 * g_flush_done_seq AFTER storing the outcome, the shell waits for the bump. */
static volatile uint32_t g_flush_req_seq;
static volatile uint32_t g_flush_done_seq;
static volatile settings_flush_outcome_t g_flush_outcome;

void input_request_settings_flush(void)
{
    g_flush_req_seq++;
    g_settings_flush_req = true;
}

bool input_settings_flush_sync(uint32_t timeout_ms, settings_flush_outcome_t *out)
{
    const uint32_t want = ++g_flush_req_seq;
    g_settings_flush_req = true;
    for (uint32_t waited = 0; waited <= timeout_ms; waited += 10u) {
        /* >= in modular arithmetic: a later async request may be served first
         * and cover this one (the flush captures everything pending). */
        if ((int32_t)(g_flush_done_seq - want) >= 0) {
            if (out != NULL) *out = g_flush_outcome;
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

/* Called every pass of the input task (<= 100 ms). */
void input_autooff_poll(void)
{
    uint32_t now = settings_now_ms();
    /* A shell `autoset` / `basecal` / `center50` only QUEUES a run, served by
     * scope_selfcal_service() later in this loop over many passes: flushing
     * now would write the pre-run state (center50) or a mid-run snapshot
     * (autoset forces AUTO and walks timebase/ranges), and the run's own
     * settings_changed() only starts the settle window, which nothing
     * services without a key. Hold the request until the run has ended
     * (or been cancelled and restored) so the flush records the final state. */
    if (g_settings_flush_req && scope_selfcal_quiet()) {
        /* Clear the flag BEFORE reading the sequence: a request landing in
         * between is then either covered by `seq` (its change precedes the
         * flush below) or re-raises the flag for the next pass. */
        g_settings_flush_req = false;
        const uint32_t seq = g_flush_req_seq;
        g_flush_outcome = settings_store_flush_ex(now);
        g_flush_done_seq = seq;
    }
    if (!g_autooff_ready) {
        sys_autooff_init(&g_autooff, sys_prefs_autooff_mins(), now);
        g_autooff_ready = true;
    }
    if (!sys_autooff_poll(&g_autooff, now)) return;

    const theme_t *th = theme_get();
    uint16_t bg = 0x0010;
    ui_modal_active = true;          /* same screen claim as the POWER countdown */
    vTaskDelay(pdMS_TO_TICKS(120));
    lcd_fill_rect(60, 80, 200, 80, bg);
    font_draw_string_center(160, 100, "Auto power off",
                            th->warning, bg, &font_medium);
    font_draw_string_center(160, 126, "Goodbye!", th->success, bg, &font_large);
    vTaskDelay(pdMS_TO_TICKS(800));
    power_off_now();
}

/* Settings > Auto Shutdown value (top-level row 2). */
static void autooff_adjust(int dir, bool wrap)
{
    uint8_t m = sys_autooff_step(sys_prefs_autooff_mins(), dir, wrap);
    sys_prefs_set_autooff_mins(m);
    sys_autooff_set_minutes(&g_autooff, m, settings_now_ms());
    g_autooff_ready = true;
}

/* Settings > Meter on Battery: meter.link through the model (this is the
 * executor), so the key, the shell's `set meter.link` and a saved record
 * are one state. The display task turns the link on or off within a frame
 * (main.c meter_link_service); the key's own capture saves it. */
static void meter_link_adjust(int dir)
{
    (void)prop_step_local(P(METER_LINK), dir, ORIGIN_KEY, NULL);
}

/* Settings > Sound and Light (depth 7): row 0 volume, row 1 brightness. */
static void sound_light_adjust(int dir, bool wrap)
{
    if (settings_sub_selected == 0) {
        uint8_t v = sys_prefs_volume();
        uint8_t n = sys_vol_step(v, dir);
        if (wrap && n == v) n = (dir > 0) ? 0u : SYS_VOL_MAX;
        sys_prefs_set_volume(n);
        sys_pwm_click();             /* hear the new level */
    } else {
        uint8_t b = sys_prefs_brightness();
        uint8_t n = sys_bright_step(b, dir);
        if (wrap && n == b) n = (dir > 0) ? SYS_BRIGHT_MIN : SYS_BRIGHT_MAX;
        sys_prefs_set_brightness(n);
        sys_pwm_apply_brightness();
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * Oscilloscope settings Left/Right handler
 * ═══════════════════════════════════════════════════════════════════ */

static void set_focus(key_focus_t f);

/* The hardware side of coupling, trigger, Roll, view and persistence
 * changes is scope_apply.c, shared with the Oscilloscope Settings page. */

/* Settings -> Oscilloscope -> Cursors: what TRIGGER did before C3. */
static void cursor_mode_cycle(void)
{
    scope_state_t *ss = scope_state_get();
    scope_cursor_cycle_mode();
    /* Cursors on = the arrows move them, as before the key model; off =
     * back to zoom. */
    set_focus(key_focus_after_cursor_change(scope_key_focus, ss->cursor.mode != CURSOR_OFF));
}

/* Stock's "Display Mode" item (menu table 0x0804C824: Y-T / Rolling / X-Y).
 * This item used to cycle the colour theme, which now has its own row.
 * Roll is the C11 MCU-paced mode (re-applied to the FPGA here, so the menu
 * and the timebase keys reach the same state); X-Y is the existing Lissajous
 * view, so it needs the FEATURE_FFT build that carries it and is skipped
 * otherwise. The view goes through scope_view_select() (scope_apply.c), which
 * releases the FFT pool on the way out of a spectrum view. */
static void display_mode_step(int dir)
{
    const scope_disp_mode_t cur = (scope_disp_mode_t)scope_display_mode();
    scope_disp_mode_t next = scope_disp_mode_cycle(cur, dir);
#ifndef FEATURE_FFT
    if (next == SCOPE_DISP_XY)
        next = scope_disp_mode_cycle(next, dir);
#else
    scope_view_select(next == SCOPE_DISP_XY ? SCOPE_VIEW_XY : SCOPE_VIEW_TIME);
#endif
    (void)scope_apply_roll(next == SCOPE_DISP_ROLL);   /* refused: the old mode stays */
}

static void osc_settings_adjust(int dir)
{
    scope_state_t *ss = scope_state_get();
    switch (settings_sub_selected) {
    case 0: /* CH1 Coupling -- the relay, DC <-> AC; two states, so dir is moot */
        { char t[24]; (void)scope_apply_coupling_toggle(0, t, sizeof(t)); }
        break;
    case 1: /* CH1 Probe */
        ss->ch1.probe = (probe_t)((ss->ch1.probe + PROBE_COUNT + dir) % PROBE_COUNT);
        break;
    case 2: /* CH2 Coupling -- the relay, DC <-> AC; two states, so dir is moot */
        { char t[24]; (void)scope_apply_coupling_toggle(1, t, sizeof(t)); }
        break;
    case 3: /* CH2 Probe */
        ss->ch2.probe = (probe_t)((ss->ch2.probe + PROBE_COUNT + dir) % PROBE_COUNT);
        break;
    case 4: /* Trigger Mode */
    case 5: /* Trigger Edge */
    case 6: /* Trigger Source */
    {
        /* The panel's model, so Settings and the panel cannot disagree on
         * what a change means (or on refusing a source that is off). */
        static const uint8_t row[3] = { TM_ROW_MODE, TM_ROW_EDGE, TM_ROW_SOURCE };
        trig_menu_t m = { row[settings_sub_selected - 4] };
        char t[24];
        (void)scope_apply_trigger_change(trig_menu_change(&m, ss, dir), t, sizeof(t));
        break;
    }
    case 7: /* Cursors */
        cursor_mode_cycle();
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
        case 1: /* Sound and Light -- stock sub-page: volume, brightness */
            settings_depth = 7;
            settings_sub_selected = 0;
            break;
        case 2: /* Auto Shutdown: Off / 15 / 30 / 60 min */
            autooff_adjust(+1, true);
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
        case 14: /* Meter on Battery: With Screen <-> Always (meter.link) */
            meter_link_adjust(1);
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
        case 13: /* USB Sharing: re-enumerate as CDC + read-only gallery
                  * drive, or back to CDC only. Stock (0x0800BF08) goes
                  * MSC-only and stops the scope; ours keeps the shell and
                  * the scope, and falls back to CDC-only by itself if the
                  * host does not configure the composite device (usb_mode_sm.h). */
#if defined(USB_MSC) && USB_MSC
            usb_share_toggle();
#endif
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
        case 0: { char t[24]; (void)scope_apply_coupling_toggle(0, t, sizeof(t)); } break;
        case 1: scope_cycle_probe(&ss->ch1); break;
        case 2: { char t[24]; (void)scope_apply_coupling_toggle(1, t, sizeof(t)); } break;
        case 3: scope_cycle_probe(&ss->ch2); break;
        case 4: case 5: case 6: case 7: osc_settings_adjust(+1); break;
        default: break;
        }
    } else if (settings_depth == 7) {
        sound_light_adjust(+1, true);
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
            /* Persistence: Off -> 500ms -> 1s -> Inf -> Off (stock's menu,
             * manual p.30), with the pool lifecycle. */
            if (!persist_enabled) {
                /* Persistence needs the pool: a spectrum view gives it up
                 * (scope_apply.c; until the merge the FFT was evicted but
                 * its view left selected). */
                (void)scope_apply_persist_on(PERSIST_LOW);
            } else if (persist_get_mode() == PERSIST_LOW) {
                persist_set_mode(PERSIST_MEDIUM);
            } else if (persist_get_mode() == PERSIST_MEDIUM) {
                persist_set_mode(PERSIST_INFINITE);
            } else {
                scope_apply_persist_off();
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

static uint8_t mode_key(button_id_t button, QueueHandle_t dq);

/* What the arrow keys adjust in the scope time view (key_model.h). Short
 * SELECT cycles it: zoom -> channel move -> trigger move -> [cursor] -> [mask]. The three
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

/* key_model.h: read-only views for the status bar. */
uint8_t key_focus_get(void)          { return (uint8_t)scope_key_focus; }
uint8_t key_active_channel_get(void) { return active_channel; }

/* The key that opened the Oscilloscope Settings page (or last jumped in it):
 * pressing it again closes the page (key_model.c map_osc_menu). */
static uint8_t osc_menu_opener = BTN_NONE;

/* Helper: show popup and send redraw */
static void popup_and_redraw(QueueHandle_t q, const char *text)
{
    scope_show_popup(text);
    uint8_t cmd = DCMD_REDRAW_ALL;
    send_cmd(q, cmd);
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

/* Enter a device mode: the one path for MENU, the home page's OK and the
 * long-press shortcuts, so a mode is never entered half-way (the FPGA
 * posture, the settings list, the persistence flush). */
static void enter_mode(device_mode_t m, QueueHandle_t dq)
{
    meter_autoselect_cancel();
    siggen_ui_editor()->active = false;   /* a long-press mode switch keeps the value */
    osc_menu_close();                    /* never reappears over another mode's return */
    if (m == MODE_HOME)
        home_enter(home_ui_model(), current_mode);   /* preselect where we came from (stock 0x0800D834) */
    current_mode = m;
    if (current_mode == MODE_SETTINGS) {
        settings_selected = 0;
        settings_depth = 0;
    }
    legacy_screens_sync();               /* the router's base follows the mode (sys.instrument) */
    /* Tell the FPGA which mode we're entering.
     * Each mode needs different FPGA commands and analog MUX config. */
    if (current_mode == MODE_MULTIMETER) {
        /* Background: no frozen buttons. Nothing is sent when the meter
         * link is already on and running this function (fpga.c). */
        fpga_meter_screen_entered(meter_submode);
    } else if (current_mode == MODE_SIGNAL_GEN) {
        fpga_enter_siggen_mode();
    } else if (current_mode == MODE_OSCILLOSCOPE) {
        fpga_enter_scope_mode();
    }
    /* Settings and home: the FPGA keeps the posture it had. PC11 is not
     * written here any more: the meter link has one owner (the display
     * task, main.c meter_link_service), which switches the meter SoC off
     * within a frame when the link policy says so (stock's exit to home,
     * 0x0800D874, always did) and leaves it running when a USB host or
     * meter.link = always wants it. */
    /* Leaving a mode, or backing out of the settings menu, is a deliberate
     * boundary: commit now rather than waiting out the settle window. */
    (void)settings_store_flush(settings_now_ms());
    send_cmd(dq, DCMD_REDRAW_ALL);
}

/* Short SAVE, every mode (owner decision D1). */
static void save_key_short(QueueHandle_t dq)
{
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
        send_cmd(dq, DCMD_REDRAW_ALL);
        return;
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
        send_cmd(dq, DCMD_REDRAW_ALL);
        return;
    }
#endif
    /* Screenshot (parity C16): stock writes 2:/Screenshot file/%d.bmp
     * and pops "Saved"/"Save failed" (ms[0xE10] 2/3). Ours writes
     * <n>.bmp into our own W25Q scratch slots (gallery_store.h), never
     * stock's FAT. The capture freezes rendering for the ~0.5 s write,
     * so the popup comes after it and is not in the picture. The
     * result is only reported once the region layer has read every
     * page back (until 2026-10-03 this said "SAVED #n" and wrote
     * nothing). Every mode since UI round 1: home, meter, generator,
     * settings and the Oscilloscope Settings page included. */
    uint32_t seq = 0;
    const gallery_status_t gst = gallery_ui_capture(&seq);
    char msg[28];
    gallery_ui_result_text(gst, seq, msg, sizeof msg);
    scope_show_popup(msg);
    if (current_mode == MODE_HOME) {
        /* Home repaints only its selection on key presses; the popup
         * overlay (main.c) draws itself and asks for the repaint when it
         * expires. */
        send_cmd(dq, legacy_screens_active_cmd());   /* the home page: DCMD_DRAW_HOME */
    } else {
        send_cmd(dq, DCMD_REDRAW_ALL);
    }
}

/* After the page applied a change: the cursors switched on from its Cursors
 * row put the arrows on the cursor stage, as TRIGGER's old cursor cycle did
 * (stock behaviour [inferred], UI_STUDY 2.3). */
static void osc_menu_after_key(cursor_mode_t was)
{
    const scope_state_t *ss = scope_state_get();
    if (ss->cursor.mode != was)
        set_focus(key_focus_after_cursor_change(scope_key_focus, ss->cursor.mode != CURSOR_OFF));
}

/* Frequency digit editor (parity G4). While it is open the arrows move the
 * digit cursor / change the digit, OK or PRM accept, MENU cancels. Every
 * other key (POWER included) falls through to the normal handlers. Returns
 * true when the key was consumed. The value goes live after every change,
 * as in stock. */
static bool siggen_edit_input(button_id_t button, QueueHandle_t dq)
{
    if (current_mode != MODE_SIGNAL_GEN) return false;
    siggen_edit_t *ed = siggen_ui_editor();
    siggen_edit_key_t k;

    if (!ed->active) {
        if (button != BTN_PRM) return false;
        siggen_edit_begin(ed, siggen_get_config()->frequency_hz);
    } else {
        /* The shell (`siggen freq`) can change the frequency while the
         * editor is open; step from the live value, not a stale copy. */
        if (ed->value != siggen_get_config()->frequency_hz)
            ed->value = siggen_get_config()->frequency_hz;
        switch (button) {
        case BTN_LEFT:  k = SGE_KEY_LEFT;   break;
        case BTN_RIGHT: k = SGE_KEY_RIGHT;  break;
        case BTN_UP:    k = SGE_KEY_UP;     break;
        case BTN_DOWN:  k = SGE_KEY_DOWN;   break;
        case BTN_OK:
        case BTN_PRM:   k = SGE_KEY_ACCEPT; break;
        case BTN_MENU:  k = SGE_KEY_CANCEL; break;
        default: return false;
        }
        siggen_edit_result_t r = siggen_edit_key(ed, k);
        if (r == SGE_CHANGED || r == SGE_CANCELLED)
            siggen_set_frequency(ed->value);
    }
    send_cmd(dq, legacy_screens_active_cmd());       /* the generator: DCMD_DRAW_SIGGEN */
    return true;
}

/* A long mode key: sys.mode through the model, the path `set sys.mode` and
 * `set sys.instrument` take (props_sys.c -> model_hw_fw.c op_mode ->
 * input_mode_switch -> enter_mode). */
static void key_mode_set(device_mode_t m)
{
    (void)prop_set_local(prop_by_id(PROP_ID_SYS_MODE), (int32_t)m, ORIGIN_KEY, NULL);
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
    c.osc_menu_open  = (current_mode == MODE_OSCILLOSCOPE) && osc_menu_is_open();
    c.osc_menu_locked = c.osc_menu_open && osc_menu_is_locked();
    c.osc_menu_opener = osc_menu_opener;
    return c;
}

/* Execute one answer of key_map() for key `btn`. Returns the display command sent. */
static uint8_t exec_key_cmd(key_cmd_t k, button_id_t btn, QueueHandle_t dq)
{
    scope_state_t *ss = scope_state_get();
    uint8_t cmd = DCMD_REDRAW_ALL;
    char pb[24];
    channel_state_t *ach = (active_channel == 0) ? &ss->ch1 : &ss->ch2;
    const int dir = k.arg;

    switch (k.act) {
    case KA_NONE:
    case KA_LEGACY:
    /* Not executed here: the router's POWER global runs the per-key switch
     * (mode_key) itself, after the gallery handed POWER back. */
    case KA_POWER_PROMPT:
        break;

    /* -- long presses: mode shortcuts, from any mode -------------------- */
    /* The router's globals take these first (legacy_screens.c ->
     * lgc_mode_set); kept here so key_map's answer means the same wherever
     * it is executed. Long BACK: stock's mode-selection page from any mode,
     * with the mode left preselected (0x0800D834); on home itself it does
     * nothing, as in stock (input_mode_switch: nothing if it is the mode). */
    case KA_ENTER_HOME:   key_mode_set(MODE_HOME);         break;
    case KA_ENTER_METER:  key_mode_set(MODE_MULTIMETER);   break;
    case KA_ENTER_SCOPE:  key_mode_set(MODE_OSCILLOSCOPE); break;
    case KA_ENTER_SIGGEN: key_mode_set(MODE_SIGNAL_GEN);   break;

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

    case KA_SIGGEN_RUNSTOP: {
        /* Same effect as the short OK of the previous build; stock moved
         * Run/Stop to the long confirm. An open frequency edit is not
         * interrupted: its value is already live. */
        siggen_enable(!siggen_get_config()->output_enabled);
        cmd = DCMD_DRAW_SIGGEN;
        send_cmd(dq, legacy_screens_active_cmd());
        break;
    }

    case KA_SIGGEN_BOOT_TOGGLE: {
        siggen_set_start_on(!siggen_get_config()->start_on);
        cmd = DCMD_DRAW_SIGGEN;
        send_cmd(dq, legacy_screens_active_cmd());
        break;
    }

    /* Long SAVE (parity C16): stock's 3x3 screenshot gallery, from every
     * mode (D1). It owns the screen (ui_modal_active) and every key until it
     * closes itself; its closing DCMD_REDRAW_ALL repaints whatever was under
     * it, the Oscilloscope Settings page included. */
    case KA_GALLERY_OPEN:
        gallery_ui_open(dq);
        cmd = DCMD_NONE_MODAL_DROP;
        break;

    /* Long AUTO / long OK (parity C10): queued for the key task's selfcal
     * service, which runs right after this returns and shows its own
     * popups. Baseline correction walks banks 0 and 1 (stock's order); bank
     * 2, the interleaved single-channel regime, is `fpga scope basecal 7`. */
    case KA_BASECAL:
        if (!scope_selfcal_request_basecal(0x03u)) popup_and_redraw(dq, "Busy");
        break;
    case KA_CENTER50:
        if (!scope_selfcal_request_center50()) popup_and_redraw(dq, "Busy");
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

    case KA_SCREENSHOT:
        save_key_short(dq);
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
        /* The status bar's H= shows the new code; only a failed write,
         * which the bar cannot show, gets a popup. */
        if (tb_ok) {
            send_cmd(dq, DCMD_REDRAW_ALL);
            break;
        }
        char tb[12];
        if (ss->roll)
            scope_roll_label(ss->roll_step, tb, sizeof(tb));
        else
            /* The status bar's label (merge x2, fast-code zoom; C5). */
            scope_timebase_view_label(ss->timebase_idx,
                                      scope_ui_view_merged(),
                                      tb, sizeof(tb));
        snprintf(pb, sizeof(pb), "H=%s NOT SET", tb);
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
        /* The bottom bar shows the new volts/div; only a failed relay
         * write, which the bar cannot show, gets a popup. */
        if (vd_ok) {
            send_cmd(dq, DCMD_REDRAW_ALL);
            break;
        }
        char vd[12];
        scope_cal_range_label((uint8_t)(active_channel + 1), ach->vdiv_idx, vd, sizeof(vd));
        snprintf(pb, sizeof(pb), "CH%d %s NOT SET", active_channel + 1, vd);
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
        /* Move the trigger point across the screen, 16 px per press,
         * over stock's 150 + hpos, hpos +-150 (parity C5, scope_geom.h).
         * The popup reports the column it was ASKED for; the marker at
         * the top shows where it actually landed. */
        int tx = ss->trig_x + dir * 16;
        if (tx < SCOPE_GEOM_TRIG_COL_MIN) tx = SCOPE_GEOM_TRIG_COL_MIN;
        if (tx > SCOPE_GEOM_TRIG_COL_MAX) tx = SCOPE_GEOM_TRIG_COL_MAX;
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

    /* The live frame draws the cursors and the AUX row their readout, so a
     * move is one live frame, not a clear-and-repaint (scope_ui.c). */
    case KA_CURSOR_MOVE:
        scope_cursor_move((int16_t)(dir * 4));
        scope_ui_request_live();
        break;

    case KA_CURSOR_NEXT:
        scope_cursor_next_sel();
        scope_ui_request_live();
        break;

    case KA_MASK_TOL_V:
        mask_adjust_tol(dir * 2, 0, pb, sizeof(pb));
        popup_and_redraw(dq, pb);
        break;

    case KA_MASK_TOL_H:
        mask_adjust_tol(0, dir, pb, sizeof(pb));
        popup_and_redraw(dq, pb);
        break;

    /* -- the Oscilloscope Settings page (osc_menu_model.h) -------------- */
    case KA_OSCMENU_OPEN:
        /* From the plot (MENU), or a deep-link key jumping to its row while
         * the full page is up. */
        osc_menu_open((osc_row_t)dir);
        osc_menu_opener = (uint8_t)btn;
        cmd = DCMD_DRAW_OSC_MENU;
        send_cmd(dq, cmd);
        break;
    case KA_OSCMENU_PANE:
        /* PRM / TRIGGER: the page locked inside their pane (stock V1.2.0);
         * that key, or MENU, closes it. */
        osc_menu_open_locked((osc_row_t)dir);
        osc_menu_opener = (uint8_t)btn;
        cmd = DCMD_DRAW_OSC_MENU;
        send_cmd(dq, cmd);
        break;
    case KA_OSCMENU_KEY: {
        const cursor_mode_t was = ss->cursor.mode;
        const osc_result_t r = osc_menu_key((osc_key_t)dir);
        osc_menu_after_key(was);
        if (r == OSC_RES_REDRAW) {
            cmd = DCMD_DRAW_OSC_MENU;
            send_cmd(dq, legacy_screens_active_cmd());  /* the page stays up: DCMD_DRAW_OSC_MENU */
        } else if (r == OSC_RES_CLOSE) {
            osc_menu_close();               /* idempotent: the model may have closed already */
            osc_menu_opener = BTN_NONE;
            send_cmd(dq, cmd);              /* REDRAW_ALL: the scope, in whatever view is now set */
        } else {
            cmd = DCMD_NONE_MODAL_DROP;     /* nothing changed on screen */
        }
        break;
    }
    case KA_OSCMENU_CLOSE:
        osc_menu_close();
        osc_menu_opener = BTN_NONE;
        send_cmd(dq, cmd);
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
 * The handlers the legacy screens forward to (ui/legacy_screens.h)
 *
 * Until the screen router (ARCH-1 B1) these were the steps of one function,
 * input_handle_button(), asked in a fixed order. They are the same code; the
 * router (ui/screen.c) now decides which one a key reaches, in that same
 * order, and legacy_screens.c says which screen forwards to which.
 * ═══════════════════════════════════════════════════════════════════ */

/* The display queue of the key being handled: the router's callbacks take no
 * argument for it. Set before the first key (input_router_init). */
static QueueHandle_t g_dq;
/* The display command the last handler sent (input_handle_button's answer). */
static uint8_t g_cmd;

void lgc_state(lgc_state_t *s)
{
    s->mode           = current_mode;
    s->settings_depth = settings_depth;
    s->osc_open       = osc_menu_is_open();
    s->gallery_open   = gallery_ui_is_open();
}

key_ctx_t lgc_ctx(void) { return key_ctx_now(); }

void lgc_exec(key_cmd_t k, button_id_t btn)
{
    g_cmd = exec_key_cmd(k, btn, g_dq);
    settings_after_button();
}

/* The home page takes every key but POWER (shutdown must always work):
 * LEFT/RIGHT move, OK enters, MENU goes back (home_model.h). Before the
 * meter softkeys and the generator editor, which must never see them. */
void lgc_home(button_id_t button)
{
    home_key_t hk;
    uint8_t cmd = DCMD_REDRAW_ALL;
    switch (button) {
    case BTN_LEFT:  hk = HOME_KEY_LEFT;  break;
    case BTN_RIGHT: hk = HOME_KEY_RIGHT; break;
    case BTN_UP:    hk = HOME_KEY_UP;    break;
    case BTN_DOWN:  hk = HOME_KEY_DOWN;  break;
    case BTN_OK:    hk = HOME_KEY_OK;    break;
    case BTN_MENU:  hk = HOME_KEY_BACK;  break;
    default:        hk = HOME_KEY_OTHER; break;
    }
    const home_result_t r = home_key(home_ui_model(), hk);
    if (r.act == HOME_MOVED) {
        cmd = DCMD_DRAW_HOME;
        send_cmd(g_dq, legacy_screens_active_cmd());      /* the home page: DCMD_DRAW_HOME */
    } else if (r.act == HOME_ENTER) {
        enter_mode(r.mode, g_dq);
    } else {
        cmd = DCMD_NONE_MODAL_DROP;   /* nothing to draw */
    }
    g_cmd = cmd;
    settings_after_button();
}

/* Softkeys (docs/specs/platform/softkey-ui.md): in the meter, the four
 * buttons under the screen do what the bar above them says. Asked before the
 * per-button cases, so the meter never falls into another mode's meaning for
 * MOVE/SELECT/TRIGGER/PRM. */
bool lgc_meter_softkey(button_id_t button)
{
    int8_t sk = softkey_slot_for_button(button);
    if (sk < 0) return false;
    (void)meter_softkey_press(sk);
    g_cmd = DCMD_DRAW_METER;
    send_cmd(g_dq, legacy_screens_active_cmd());          /* the meter: DCMD_DRAW_METER */
    /* Same settings hook as every other key: a softkey changes saved state
     * (function, view) like any other button. */
    settings_after_button();
    return true;
}

bool lgc_siggen_edit(button_id_t button)
{
    if (!siggen_edit_input(button, g_dq)) return false;
    g_cmd = DCMD_DRAW_SIGGEN;
    settings_after_button();
    return true;
}

void lgc_mode_key(button_id_t button) { g_cmd = mode_key(button, g_dq); }

void lgc_mode_set(device_mode_t m)
{
    g_cmd = DCMD_REDRAW_ALL;
    key_mode_set(m);
    settings_after_button();
}

/* The screenshot gallery (long SAVE) takes every key while it is open; it
 * hands POWER back after closing itself so power-off always works. No
 * settings hook, as before the router. */
bool lgc_gallery_key(uint8_t raw)
{
    g_cmd = DCMD_NONE_MODAL_DROP;
    return gallery_ui_key(raw, g_dq);
}

/* While autoset or the baseline correction runs the keys are locked, as
 * stock locks them while ms[0x2F]/[0x30] are set (0x08040074): a range or
 * timebase press would pull the front end out from under the servo.
 * AUTO and MENU (short or long) cancel and restore; POWER still works (the
 * router never asks about it). No settings hook, as before the router. */
bool lgc_selfcal_busy(void)    { return scope_selfcal_busy(); }
void lgc_selfcal_cancel(void)  { scope_selfcal_request_cancel(); g_cmd = DCMD_REDRAW_ALL; }
void lgc_selfcal_refuse(void)  { popup_and_redraw(g_dq, "Busy: AUTO cancels"); g_cmd = DCMD_REDRAW_ALL; }

/* Every key event: restart the auto power-off count and click at the stored
 * volume (stock does both for each key the key task handles, 0x08040038; a
 * volume of 0 is silent). */
void lgc_touch(void) { autooff_touch(); }
void lgc_click(void) { sys_pwm_click(); }
void lgc_lock(void)   { taskENTER_CRITICAL(); }
void lgc_unlock(void) { taskEXIT_CRITICAL(); }

void input_router_init(void *display_queue)
{
    g_dq = (QueueHandle_t)display_queue;
    legacy_screens_init();
}

bool input_mode_switch(device_mode_t m)
{
    if ((unsigned)m >= (unsigned)MODE_COUNT || g_dq == NULL) return false;
    if (current_mode != m) enter_mode(m, g_dq);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * Main button dispatcher: the screen router
 * ═══════════════════════════════════════════════════════════════════ */

uint8_t input_handle_button(button_id_t button, QueueHandle_t dq)
{
    /* MENU + POWER → clean handoff to the factory IAP bootloader.
     * The FNIRSI bootloader at 0x08000000 checks the MENU key at
     * reset; the user is holding it, so a plain reset (NOT the DFU
     * magic-word path — that was for our retired HID bootloader)
     * lands in stock upgrade mode. Replaces the old behavior where
     * POWER alone hit the shutdown countdown and MENU+POWER froze.
     * The bootloader draws its own "firmware upgrade" screen, so no
     * delay/message here — reset immediately while MENU is still down.
     * Until the router this sat at the top of the POWER case; it is taken
     * first now, before any screen sees the key. What it skips (the key
     * click, the auto-off restart, a gallery closing itself) is RAM state
     * the reset discards anyway; the settings flush is the same. */
    if (button == BTN_POWER && (button_scan_get_raw() & BTN_SCAN_MENU_MASK)) {
        /* Reboot boundary — same reasoning as the DFU entry above. */
        (void)settings_store_flush(settings_now_ms());
        __DSB();
        NVIC_SystemReset();
        while (1) { }
    }

    g_dq = dq;
    g_cmd = DCMD_REDRAW_ALL;
    legacy_screens_sync();             /* the shell may have changed the mode or closed the gallery */
    (void)screen_route_key(button);
    legacy_screens_sync();             /* what the key opened or closed */
    return g_cmd;
}

/* The per-key cases: what a key does in a mode when nothing before it (the
 * key model, home, softkeys, the frequency editor) took it. POWER comes here
 * from the router's globals. */
static uint8_t mode_key(button_id_t button, QueueHandle_t dq)
{
    scope_state_t *ss = scope_state_get();
    uint8_t cmd = DCMD_REDRAW_ALL;
    char pb[24];

    switch (button) {

    /* -- Mode / Navigation ---------------------------------------- */

    case BTN_MENU:
        /* Short MENU (stock's BACK). Inside a level it goes up one, as in
         * stock (0x08009FE8: settings / generator sub-levels; the trigger
         * panel and the frequency editor are closed earlier, by key_map()
         * and siggen_edit_input()). At a mode's top level it opens the home
         * page. That is ours where stock differs: stock's short BACK does
         * nothing at the top of the meter, generator and settings (owner
         * decision D2 keeps ours). The scope never reaches here: its short
         * MENU opens the Oscilloscope Settings page (key_model.c, stock
         * ms[0xF68] = 9). Until 2026-11 this cycled scope -> meter ->
         * generator -> settings. */
        if (current_mode == MODE_SETTINGS && settings_depth > 0) {
            meter_autoselect_cancel();
            settings_depth = 0;
            settings_sub_selected = 0;
            (void)settings_store_flush(settings_now_ms());
            send_cmd(dq, cmd);
        } else {
            enter_mode(MODE_HOME, dq);
        }
        break;

    /* -- Channel buttons ------------------------------------------ */

    case BTN_CH1:
        if (current_mode == MODE_OSCILLOSCOPE) {
            active_channel = 0;
            (void)scope_apply_coupling_toggle(0, pb, sizeof(pb));
            popup_and_redraw(dq, pb);
        } else {
            send_cmd(dq, cmd);
        }
        break;

    case BTN_CH2:
        if (current_mode == MODE_OSCILLOSCOPE) {
            active_channel = 1;
            (void)scope_apply_coupling_toggle(1, pb, sizeof(pb));
            popup_and_redraw(dq, pb);
        } else {
            send_cmd(dq, cmd);
        }
        break;

    /* -- Trigger -------------------------------------------------- */

    case BTN_TRIGGER:
        /* Scope: key_model.c claims it in every view (KA_OSCMENU_PANE,
         * the Trigger pane). What is left is the other modes. */
        send_cmd(dq, cmd);
        break;

    case BTN_MOVE:
        /* Scope: key_model.c answers it (KA_ADJ_CH_SWITCH, executed above). What
         * is left is the other modes, where it only repaints; the meter's
         * softkey meaning was caught at the top. */
        send_cmd(dq, cmd);
        break;

    /* -- Save (screenshot) ---------------------------------------- */

    /* BTN_SAVE never reaches here: key_map() claims it in every mode
     * (KA_SCREENSHOT -> save_key_short(), the bench-image hooks included). */

    /* -- Auto ----------------------------------------------------- */

    case BTN_AUTO:
        if (current_mode == MODE_MULTIMETER) {
            /* Stock's AUTO key in the meter (0x0800CB50) sends word 0x0514
             * and lets the SoC pick the function (V or Ohm, AC or DC); it
             * resets hold/REL/min/max like any function change. Pressed
             * again it re-arms the SoC. The software autoscan this key used
             * to start stays reachable as the shell's `meter auto`. */
            meter_function_select(FPGA_METER_SUBMODE_AUTO);
            scope_show_popup("AUTO");
            cmd = DCMD_DRAW_METER;
            send_cmd(dq, legacy_screens_active_cmd());
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
            /* Time view: autoset (parity C9; stock 0x0800CB50). While a
             * mask exists, or the arrows are on its stage, AUTO keeps its
             * mask meaning (cancel a teach / clear): the mask is ours, and
             * an autoset would move the picture it was taught on. Teaching
             * a new one is `mask teach` in the shell. */
            if (scope_mask_state() != MASK_PF_EMPTY || scope_key_focus == KF_MASK) {
                mask_auto_key(pb, sizeof(pb));
                popup_and_redraw(dq, pb);
            } else if (!scope_selfcal_request_autoset()) {
                popup_and_redraw(dq, "Busy");
            }
        }
        break;

    /* -- PRM / SELECT --------------------------------------------- */

    /* BTN_PRM: in the scope key_map() claims it (the Oscilloscope Settings
     * page at Parameter Measurement; the FFT views it used to cycle are
     * chosen on that page, through scope_view_select()). The meter's
     * softkey and the generator's frequency editor took it above; anywhere
     * else it does nothing. */

    case BTN_SELECT:
#ifdef FEATURE_FFT
        if (current_mode == MODE_OSCILLOSCOPE &&
            scope_view != SCOPE_VIEW_TIME) {
            fft_cycle_window();
            send_cmd(dq, cmd);
        } else
#endif
        if (current_mode == MODE_SIGNAL_GEN) {
            siggen_cycle_waveform();
            cmd = DCMD_DRAW_SIGGEN;
            send_cmd(dq, legacy_screens_active_cmd());
        } else if (current_mode == MODE_SETTINGS &&
                   settings_depth == 5) {
            /* Resistor calc: simulate measurement */
            resistor_calc_simulate_measure();
            cmd = DCMD_DRAW_SETTINGS;
            send_cmd(dq, legacy_screens_active_cmd());
        } else if (current_mode == MODE_SETTINGS &&
                   settings_depth == 4) {
            /* Component tester: cycle component type */
            comp_test_cycle_type();
            cmd = DCMD_DRAW_SETTINGS;
            send_cmd(dq, legacy_screens_active_cmd());
        }
        /* Scope time view: SELECT cycles the arrow stage -- key_model.c */
        break;

    /* -- UP / DOWN ------------------------------------------------ */

    case BTN_UP:
        if (current_mode == MODE_SETTINGS) {
            if (settings_depth == 5) {
                resistor_calc_change_color(1);
            } else if (settings_depth == 0) {
                if (settings_selected > 0) settings_selected--;
            } else if (settings_depth == 3 || settings_depth == 7) {
                if (settings_sub_selected > 0) settings_sub_selected--;
            } else if (settings_depth == 1) {
                if (settings_sub_selected > 0) settings_sub_selected--;
            }
            cmd = DCMD_DRAW_SETTINGS;
            send_cmd(dq, legacy_screens_active_cmd());
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
            send_cmd(dq, legacy_screens_active_cmd());
        }
        else if (current_mode == MODE_SIGNAL_GEN) {
            siggen_amplitude_up();
            cmd = DCMD_DRAW_SIGGEN;
            send_cmd(dq, legacy_screens_active_cmd());
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
            } else if (settings_depth == 7) {
                if (settings_sub_selected < 1) settings_sub_selected++;   /* 2 rows */
            } else if (settings_depth == 1) {
                if (settings_sub_selected < SETTINGS_OSC_ITEM_COUNT - 1)
                    settings_sub_selected++;
            }
            cmd = DCMD_DRAW_SETTINGS;
            send_cmd(dq, legacy_screens_active_cmd());
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
            send_cmd(dq, legacy_screens_active_cmd());
        }
        else if (current_mode == MODE_SIGNAL_GEN) {
            siggen_amplitude_down();
            cmd = DCMD_DRAW_SIGGEN;
            send_cmd(dq, legacy_screens_active_cmd());
        }
        break;

    /* -- LEFT / RIGHT --------------------------------------------- */

    case BTN_LEFT:
        if (current_mode == MODE_SETTINGS && settings_depth == 7) {
            sound_light_adjust(-1, false);
            cmd = DCMD_DRAW_SETTINGS;
            send_cmd(dq, legacy_screens_active_cmd());
        }
        else if (current_mode == MODE_SETTINGS && settings_depth == 0 &&
            (settings_selected == 2 || settings_selected == 3 || settings_selected == 7 || settings_selected == 12 ||
             settings_selected == 14)) {
            if (settings_selected == 14)
                meter_link_adjust(-1);
            else if (settings_selected == 2)
                autooff_adjust(-1, false);
            else if (settings_selected == 3)
                display_mode_step(-1);
            else if (settings_selected == 12)
                theme_cycle_reverse();
            else
                startup_mode_adjust(-1);
            cmd = DCMD_DRAW_SETTINGS;
            send_cmd(dq, legacy_screens_active_cmd());
        }
        else if (current_mode == MODE_SETTINGS && settings_depth == 1) {
            osc_settings_adjust(-1);
            cmd = DCMD_DRAW_SETTINGS;
            send_cmd(dq, legacy_screens_active_cmd());
        }
        else if (current_mode == MODE_SETTINGS && settings_depth == 5) {
            resistor_calc_move_band(-1);
            cmd = DCMD_DRAW_SETTINGS;
            send_cmd(dq, legacy_screens_active_cmd());
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
            /* Duty is editable for Square and Sawtooth only (stock 0x0800A478). */
            if (siggen_wave_uses_duty(siggen_get_config()->waveform))
                siggen_duty_cycle_down();
            cmd = DCMD_DRAW_SIGGEN;
            send_cmd(dq, legacy_screens_active_cmd());
        }
        break;

    case BTN_RIGHT:
        if (current_mode == MODE_SETTINGS && settings_depth == 7) {
            sound_light_adjust(+1, false);
            cmd = DCMD_DRAW_SETTINGS;
            send_cmd(dq, legacy_screens_active_cmd());
        }
        else if (current_mode == MODE_SETTINGS && settings_depth == 0 &&
            (settings_selected == 2 || settings_selected == 3 || settings_selected == 7 || settings_selected == 12 ||
             settings_selected == 14)) {
            if (settings_selected == 14)
                meter_link_adjust(+1);
            else if (settings_selected == 2)
                autooff_adjust(+1, false);
            else if (settings_selected == 3)
                display_mode_step(+1);
            else if (settings_selected == 12)
                theme_cycle();
            else
                startup_mode_adjust(1);
            cmd = DCMD_DRAW_SETTINGS;
            send_cmd(dq, legacy_screens_active_cmd());
        }
        else if (current_mode == MODE_SETTINGS && settings_depth == 1) {
            osc_settings_adjust(+1);
            cmd = DCMD_DRAW_SETTINGS;
            send_cmd(dq, legacy_screens_active_cmd());
        }
        else if (current_mode == MODE_SETTINGS && settings_depth == 5) {
            resistor_calc_move_band(1);
            cmd = DCMD_DRAW_SETTINGS;
            send_cmd(dq, legacy_screens_active_cmd());
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
            /* Duty is editable for Square and Sawtooth only (stock 0x0800A478). */
            if (siggen_wave_uses_duty(siggen_get_config()->waveform))
                siggen_duty_cycle_up();
            cmd = DCMD_DRAW_SIGGEN;
            send_cmd(dq, legacy_screens_active_cmd());
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
            send_cmd(dq, legacy_screens_active_cmd());
        } else if (current_mode == MODE_MULTIMETER) {
            /* OK = Hold (softkey UI): freeze the reading on screen now. */
            meter_hold_toggle_now();
            cmd = DCMD_DRAW_METER;
            send_cmd(dq, legacy_screens_active_cmd());
        } else if (current_mode == MODE_OSCILLOSCOPE) {
            if (scope_mask_hold_active()) {
                /* A mask failure is being held (stop-on-fail): OK resumes
                 * testing rather than toggling RUN/STOP, which would leave
                 * the acquisition frozen behind a STOP the user never set. */
                (void)scope_mask_post(SCOPE_MASK_REQ_RELEASE, 0, 0, 0);
                scope_show_popup("MASK: RESUME");
            } else if (ss->trigger.mode == TRIG_SINGLE &&
                       (!ss->running || fpga_acq_single_done())) {
                /* Stock's SINGLE: after the capture the scope is STOPPED
                 * (0x0802163E) and RUN re-arms it. A STOP the user pressed
                 * while armed re-arms the same way. */
                ss->running = true;
                fpga_acq_single_rearm();
                scope_show_popup("Await Trigger...");
            } else {
                scope_toggle_running(ss);
                if (!ss->running)
                    scope_ui_paused_latch();   /* ranges at STOP, for the zoom (C14) */
                /* No popup: the status bar's RUN/STOP tab says it. */
            }
            send_cmd(dq, cmd);
        }
        break;

    /* -- Power ---------------------------------------------------- */

    case BTN_POWER:
        {
            const theme_t *th = theme_get();
            uint16_t bg = 0x0010;  /* dark background */

            /* MENU + POWER (the IAP reset) never gets here: input_handle_button()
             * takes it before the router. */

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
            /* then the backlight to 0 and the PC9 release (stock's order) */
            power_off_now();
        }
        power_done:
        break;

    default:
        break;
    }

    settings_after_button();

    return cmd;
}
