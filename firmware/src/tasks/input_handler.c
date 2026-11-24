/*
 * OpenScope 2C53T - Input Handler
 *
 * All button-to-action mapping logic lives here.
 * The input task (in main.c) handles GPIO polling and debounce,
 * then calls input_handle_button() for the actual logic.
 *
 * Every screen's keys are its screen's (ui/home_screen.c, meter_screen.c,
 * siggen_screen.c, scope_screen.c, settings_screen.c, osc_menu_screen.c,
 * gallery_screen.c), and the keys that belong to no screen are the router's
 * globals (ui/ui_router.c). What is left here is what those name and this
 * task does: the mode's hardware posture, the screenshot, the POWER
 * countdown and the power-off, the auto power-off and the settings writes.
 */

#include "key_model.h"
#include "osc_menu_model.h"
#include "osc_menu_screen.h"
#include "scope_screen.h"
#include "overlay_screens.h"
#include "input_handler.h"
#include "ui.h"
#include "scope_state.h"
#include "../ui/startup_policy.h"   /* startup_screen_of_mode: the power-off flush */
#include "fpga.h"
#include "scope_selfcal_run.h"
#include "meter_autoselect.h"
#include "at32f403a_407.h"
#include "button_scan.h"
#include "settings_store.h"
#include "watchdog.h"       /* health_checkin_self: between the setters of a reset */
#include "sys_prefs.h"
#include "sys_pwm.h"
#include "../ui/gallery_ui.h"
#include "../ui/screen.h"
#include "../ui/ui_router.h"
#include "../ui/settings_screen.h"  /* the Settings screens' environment */
#include "model.h"           /* the long mode keys set sys.mode */
#include "task_call.h"       /* input_call_sync */
#include "model_exec.h"      /* model_exec_ready: the input loop is running */
#if FEATURE_USBDRIVE
#include "usb_debug.h"      /* Settings "USB Sharing" */
#endif
#include "task.h"
#include "power.h"
#include "dfu_boot.h"
#include "flash_regions.h"
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

/* How long a shutdown waits for a self-calibration to cancel and for a flash
 * save in another task to end: a W25Q128 sector erase (400 ms maximum) and the
 * records around it. Bounded, as the power-off must happen regardless. */
#define SHUTDOWN_SETTLE_MS      1500u
#define SHUTDOWN_POLL_MS        10u
/* The bootloader reads POWER held for 2 s as "boot the app", so the updater is
 * entered after the key is let go. A key stuck down must not block it. */
#define UPDATER_KEY_WAIT_MS     10000u
#define UPDATER_KEY_POLL_MS     50u

static QueueHandle_t g_dq;      /* set before the first key (input_router_init) */

/* Before the last saves: no self-calibration mid-run (its temporary ranges
 * would be saved) and no flash save running in another task. The run is
 * served here because this task is the one that serves it. */
static void shutdown_settle(void)
{
#ifndef EMULATOR_BUILD
    if (!scope_selfcal_quiet()) scope_selfcal_request_cancel();
    for (uint32_t waited = 0u;
         waited < SHUTDOWN_SETTLE_MS && (!scope_selfcal_quiet() || flash_region_save_active());
         waited += SHUTDOWN_POLL_MS) {
        scope_selfcal_service(g_dq);
        vTaskDelay(pdMS_TO_TICKS(SHUTDOWN_POLL_MS));
        health_checkin_self();
    }
#endif
}

/* The updater, from the keys: the settings are saved, power stays held. */
static void enter_updater(void)
{
    shutdown_settle();
    health_checkin_self();
    (void)settings_store_flush(settings_now_ms());
    health_checkin_self();
    if (power_key_down()) {
        power_prompt_show(POWER_PROMPT_UPDATE, 0);
        for (uint32_t waited = 0u; waited < UPDATER_KEY_WAIT_MS && power_key_down();
             waited += UPDATER_KEY_POLL_MS) {
            vTaskDelay(pdMS_TO_TICKS(UPDATER_KEY_POLL_MS));
            health_checkin_self();
        }
    }
    dfu_request_reboot();
}

/* The one orderly power-off: flush settings FIRST (the rail is about to stop),
 * backlight to 0 like stock's saver (0x080293EE), then release the PC9 hold.
 * The POWER hold and the auto power-off end here. It is also the only place
 * the last screen is saved (Startup on Boot = Restore): a screen change by
 * itself writes no record. The resets (MENU + POWER into the bootloader,
 * Settings > Firmware Update, the shell's reboot) flush the settings and
 * leave that byte as the last power-off wrote it. */
static void power_off_now(void)
{
    shutdown_settle();
    health_checkin_self();      /* each flash save starts with a full deadline */
    (void)settings_store_flush_power_off(settings_now_ms(), startup_screen_of_mode(current_mode));
    health_checkin_self();
    /* The session snapshot (Startup on Boot = Restore brings every instrument
     * back as it was): after the flush, so it is stamped with the settings
     * record just written and the last screen is already noted. Bounded: a
     * failed or refused save ends the call, the power-off goes on. */
    (void)session_snapshot_save();
    /* POWER may still be down, which keeps the rail up until it is let go. */
    health_powering_off();
    sys_pwm_backlight_off();
    power_hold_release();
    vTaskSuspend(NULL);
    for (;;) { }                /* not reached */
}

/* A setting changed from the shell (e.g. `set sys.startup`) has no key press
 * behind it, so nothing gives the store its turn and a reflash right after
 * loses it. The shell only raises this flag; the input task stays the
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

static void settings_reset_service(void);

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
    settings_reset_service();          /* the shell's `reset settings CONFIRM` */
    if (!g_autooff_ready) {
        sys_autooff_init(&g_autooff, sys_prefs_autooff_mins(), now);
        g_autooff_ready = true;
    }
    if (!sys_autooff_poll(&g_autooff, now)) return;

    /* "Auto power off / Goodbye!": the POWER prompt's layer, painted by the
     * display task (ui/overlay_screens.h) while this task waits out the
     * same 120 + 800 ms as when it drew the box itself. */
    power_prompt_show(POWER_PROMPT_AUTOOFF, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
    vTaskDelay(pdMS_TO_TICKS(800));
    power_off_now();
}

/* ═══════════════════════════════════════════════════════════════════
 * Settings (Home > Settings): device settings only, through the model
 * ═══════════════════════════════════════════════════════════════════
 * This task is the model's executor, so a key, the shell's `set` and a saved
 * record are one state. Rows are named (settings_item_t, ui.h), never
 * numbered. The scope's own settings are in its menu (osc_menu_model.h). */

/* The auto-off timer follows sys.autooff (model_hw_fw.c op_autooff): the
 * Settings row and the shell's `set sys.autooff` restart it the same way. */
void input_autooff_apply(uint8_t mins)
{
    sys_autooff_set_minutes(&g_autooff, mins, settings_now_ms());
    g_autooff_ready = true;
}

/* ── Reset to Defaults ───────────────────────────────────────────────
 * The built-in defaults over the user settings, on this task (it is the
 * model's executor and the one writer of the settings record):
 * settings_store_reset_to_defaults() runs every setter, so the hardware
 * follows, and writes the record. Two doors, one function: the Settings row
 * (after its question, settings_reset.h) and the shell's
 * `reset settings CONFIRM` (input_settings_reset_sync below). The health
 * check-in runs between the setters: each scope register write may wait for
 * an acquisition park. Calibration, gallery, logo and the firmware cache are
 * out of its reach (settings_store.h). */
static settings_reset_result_t settings_reset_now(void)
{
    meter_autoselect_cancel();          /* its posted meter.func would land after the reset */
    return settings_store_reset_to_defaults(settings_now_ms(), health_checkin_self);
}

/* The popup of a reset made from the Settings row (<= 23 characters). */
static const char *settings_reset_popup(const settings_reset_result_t *r)
{
    if (r->status == PS_BUSY)                 return "Busy: self-cal running";
    if (r->left)                              return "Reset: some NOT set";
    if (r->saved == SETTINGS_FLUSH_BLOCKED ||
        r->saved == SETTINGS_FLUSH_FAILED)    return "Reset, NOT saved";
    return "Settings reset";
}

/* ── The Settings screens' environment (ui/settings_screen.h) ────────
 * What a Settings key does that is not a property set: the screens name it,
 * this task's file does it. */

/* The key click at the volume just set. */
void settings_env_click(void) { sys_pwm_click(); }

/* MENU out of a page (or "no" to the question): backing out is a deliberate
 * boundary, so commit now rather than waiting out the settle window. */
void settings_env_page_left(void)
{
    meter_autoselect_cancel();
    (void)settings_store_flush(settings_now_ms());
}

/* OK on the question: the reset and its popup. */
void settings_env_reset(void)
{
    const settings_reset_result_t r = settings_reset_now();
    scope_show_popup(settings_reset_popup(&r));
}

/* USB Sharing: re-enumerate as CDC + read-only gallery drive, or back to
 * CDC only. Stock (0x0800BF08) goes MSC-only and stops the scope; ours
 * keeps the shell and the scope, and falls back to CDC-only by itself if
 * the host does not configure the composite device (usb_mode_sm.h). An
 * action, not a property: usb_debug.c owns the state (settings_screen.h). */
void settings_env_usb_share_toggle(void)
{
#if FEATURE_USBDRIVE
    usb_share_toggle();
#endif
}

/* Firmware Update: reboot into the DFU bootloader. A deliberate boundary:
 * anything still inside the settle window would be lost. */
void settings_env_fw_update(void)
{
    enter_updater();
}

/* ═══════════════════════════════════════════════════════════════════
 * The display queue
 * ═══════════════════════════════════════════════════════════════════ */


static void send_cmd(QueueHandle_t q, uint8_t cmd)
{
    xQueueSend(q, &cmd, 0);
}

/* ═══════════════════════════════════════════════════════════════════
 * Mode entry
 * ═══════════════════════════════════════════════════════════════════ */

/* Enter a device mode: the one path for MENU, the home page's OK, the long
 * mode keys and the shell's `set sys.mode` (all a sys.mode / sys.instrument
 * set: model_hw_fw.c op_mode), so a mode is never entered half-way. The
 * router switches the screen (ui_router_switch_mode: the gallery closed,
 * every layer closed, the old base closed and the new one opened;
 * current_mode follows there); this adds what a screen may not do itself:
 * the mode's hardware posture, the settings flush and the repaint. Nothing
 * but the gallery closing if it is the mode already. */
bool input_mode_switch(device_mode_t m)
{
    if ((unsigned)m >= (unsigned)MODE_COUNT || g_dq == NULL) return false;
    if (screen_base_for_mode(m) == screen_base()) {
        (void)ui_router_switch_mode(m);      /* the gallery, if it is up */
        return true;
    }
    meter_autoselect_cancel();
    if (!ui_router_switch_mode(m)) return false;
    /* Only the meter has hardware to prepare on entry (the meter chip's
     * function). The scope and the generator need nothing: the FPGA and the
     * front end stay in the scope posture, which the meter link restores
     * when it goes off. */
    if (current_mode == MODE_MULTIMETER) {
        /* Background: no frozen buttons. Nothing is sent when the meter
         * link is already on and running this function (fpga.c). */
        fpga_meter_screen_entered(meter_submode);
    }
    /* Settings and home: the FPGA keeps the posture it had. PC11 is not
     * written here any more: the meter link has one owner (the display
     * task, main.c meter_link_service), which switches the meter SoC off
     * within a frame when the link policy says so (stock's exit to home,
     * 0x0800D874, always did) and leaves it running when a USB host or
     * meter.link = always wants it. */
    /* Leaving a mode is a deliberate boundary: commit now rather than
     * waiting out the settle window. */
    (void)settings_store_flush(settings_now_ms());
    send_cmd(g_dq, DCMD_REDRAW_ALL);
    return true;
}

/* Short SAVE, every screen (owner decision D1): the router's global
 * (ui/ui_router.h). */
void ui_env_screenshot(void)
{
    const QueueHandle_t dq = g_dq;
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
        send_cmd(dq, (uint8_t)(DCMD_DRAW_ACTIVE + (uint8_t)screen_top()));   /* the home page, in place */
    } else {
        send_cmd(dq, DCMD_REDRAW_ALL);
    }
}

/* Settings persistence, once per key, after the screen has had its effect:
 * the store's turn to write. A change is not looked for here any more (until
 * ARCH-1 B8 the whole settings image was captured and compared after every
 * key): the model notes it when a persisted property changes (model.c ->
 * model_hw_fw.c op_note_change -> settings_store_note_change), whoever set
 * it. service() writes only once a change has been quiet for the settle
 * window, so holding an arrow key through ten values costs one record, and a
 * key that changed nothing costs nothing. The boundaries (a mode change,
 * backing out of a Settings page, the shell after a command, power-off)
 * still capture and flush everything themselves. Host test: make
 * test-settings-notify. */
static void settings_after_key(void)
{
    (void)settings_store_service(settings_now_ms());
}

/* ═══════════════════════════════════════════════════════════════════
 * The router's environment (ui/ui_router.h)
 * ═══════════════════════════════════════════════════════════════════ */

/* Every key event: restart the auto power-off count and click at the stored
 * volume (stock does both for each key the key task handles, 0x08040038; a
 * volume of 0 is silent). */
void ui_env_touch(void)  { autooff_touch(); }
void ui_env_click(void)  { sys_pwm_click(); }
void ui_env_lock(void)   { taskENTER_CRITICAL(); }
void ui_env_unlock(void) { taskEXIT_CRITICAL(); }
void ui_env_post(uint8_t dcmd) { send_cmd(g_dq, dcmd); }
void ui_env_settings_service(void) { settings_after_key(); }

void input_router_init(void *display_queue)
{
    g_dq = (QueueHandle_t)display_queue;
    gallery_ui_bind(g_dq);              /* where a capture is posted (DCMD_CAPTURE) */
    /* current_mode is the boot target main() chose before the scheduler
     * (Startup on Boot); from here on the router's base decides it. */
    ui_router_init(current_mode);
}

/* ── `reset settings CONFIRM`: the shell's door to settings_reset_now() ──
 * The shell task only asks; this task runs the reset on its next pass
 * (<= 100 ms) and hands the result back by sequence number, as the flush
 * does (input_settings_flush_sync). A caller that gave up withdraws the
 * request; if the reset had already started it says so (the reset then
 * finishes: there is no half-way to stop at). */
static volatile uint32_t g_reset_req_seq;       /* shell: the request wanted            */
static volatile uint32_t g_reset_started_seq;   /* input task: the one it took up       */
static volatile uint32_t g_reset_ran_seq;       /* input task: the one it really ran    */
static volatile uint32_t g_reset_done_seq;      /* input task: finished (ran or skipped) */
static volatile uint32_t g_reset_withdrawn_seq; /* shell: gave up on this one           */
static settings_reset_result_t g_reset_result;  /* written before g_reset_done_seq      */

static void settings_reset_service(void)
{
    const uint32_t want = g_reset_req_seq;
    if (want == g_reset_done_seq) return;
    g_reset_started_seq = want;                 /* take it up, THEN look at the withdrawal */
    if (g_reset_withdrawn_seq == want) { g_reset_done_seq = want; return; }
    g_reset_result = settings_reset_now();
    g_reset_ran_seq = want;
    g_reset_done_seq = want;
    if (g_dq != NULL) send_cmd(g_dq, DCMD_REDRAW_ALL);   /* theme, values, the status bar */
}

bool input_settings_reset_sync(uint32_t timeout_ms, settings_reset_result_t *out, bool *in_progress)
{
    const uint32_t want = ++g_reset_req_seq;
    if (in_progress != NULL) *in_progress = false;
    for (uint32_t waited = 0; g_reset_done_seq != want; waited += 10u) {
        if (waited >= timeout_ms) {
            g_reset_withdrawn_seq = want;       /* withdraw, THEN look at the start */
            if (g_reset_started_seq != want) return false;      /* never taken up: it will not run */
            /* Taken up: skipped (done at once) or running. One pass decides. */
            for (unsigned i = 0; i < 20u && g_reset_done_seq != want; i++) vTaskDelay(pdMS_TO_TICKS(10));
            if (g_reset_done_seq == want) break;
            if (in_progress != NULL) *in_progress = true;
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (g_reset_ran_seq != want) return false;  /* skipped: withdrawn in time */
    if (out != NULL) *out = g_reset_result;
    return true;
}

/* ── a call made on this task (input_handler.h) ─────────────────────── */
static task_call_t g_input_call;

static void input_call_wake(void)          { (void)input_inject_button(BTN_NONE); }
static void input_call_sleep(uint32_t ms)  { vTaskDelay(pdMS_TO_TICKS(ms)); }

bool input_call_sync(void (*fn)(void *), void *arg, uint32_t timeout_ms)
{
    static const task_call_port_t port = { input_call_wake, input_call_sleep };
    /* Not before the input loop runs: the wake is a BTN_NONE in the key
     * queue, and the boot logo's "any key skips" would take it for a key
     * (the model's executor answers BUSY until then for the same reason). */
    if (!model_exec_ready()) return false;
    return task_call_sync(&g_input_call, &port, fn, arg, timeout_ms);
}

void input_call_serve(void) { (void)task_call_serve(&g_input_call); }

/* ═══════════════════════════════════════════════════════════════════
 * Main button dispatcher: the screen router
 * ═══════════════════════════════════════════════════════════════════ */

/* input_handler.h: a key is being handled (its own repaint follows). */
volatile bool input_key_in_flight;

void input_handle_button(button_id_t button, QueueHandle_t dq)
{
    /* MENU + POWER opens our bootloader's update screen. Taken before any
     * screen sees the key. */
    if (button == BTN_POWER && (button_scan_get_raw() & BTN_SCAN_MENU_MASK)) {
        enter_updater();
    }

    g_dq = dq;
    input_key_in_flight = true;
    ui_router_key(button);
    input_key_in_flight = false;
}

/* POWER, from the router's globals (ui/ui_router.h): the hold-to-power-off
 * countdown. The block below is the POWER case of the old per-key switch,
 * unchanged but for the stack-mirror calls that are gone (power_prompt_show
 * / _hide push and close the prompt's layer themselves). */
void ui_env_power_key(void)
{
        {
            /* MENU + POWER (the update screen) never gets here: input_handle_button()
             * takes it before the router. */

            /* The countdown is a layer of the screen router (ui/
             * overlay_screens.h, SCR_POWER_PROMPT): this task says what it
             * shows and the display task paints the box, over the screen as
             * it stands, and draws nothing else while it is up (a live scope
             * would otherwise overdraw it: the "flashing countdown",
             * 2026-08-20). This task keeps what decides: the count, the PC8
             * poll and the power-off, with the delays they always had. */
            power_prompt_show(POWER_PROMPT_HOLD, 3);
            vTaskDelay(pdMS_TO_TICKS(120));

            /* Countdown 3..2..1 while POWER button held. The box is painted
             * again each second, so even a frame that slipped in heals
             * within 1 s. */
            for (int countdown = 3; countdown > 0; countdown--) {
                power_prompt_show(POWER_PROMPT_HOLD, (uint8_t)countdown);

                /* Wait 1 second, polling button every 50ms */
                for (int i = 0; i < 20; i++) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                    health_checkin_self();   /* waiting on the key is not a stall */
                    /* PC8 active LOW — if released, cancel */
                    if (GPIOC->idt & (1U << 8)) {
                        /* Button released — cancel: the layer goes and the
                         * display task repaints what the box covered. */
                        power_prompt_hide();
                        goto power_done;
                    }
                }
            }

            /* Still held after 3 seconds — power off */
            power_prompt_show(POWER_PROMPT_GOODBYE, 0);
            vTaskDelay(pdMS_TO_TICKS(500));

            /* Last chance to persist: an orderly power-off is the one place we
             * KNOW the settings are about to stop existing in RAM. Runs behind
             * the "Goodbye!" frame so its SPI2 traffic is invisible to the
             * user, and before the rail drops. */
            /* then the backlight to 0 and the PC9 release (stock's order) */
            power_off_now();
        }
        power_done:
        return;
}
