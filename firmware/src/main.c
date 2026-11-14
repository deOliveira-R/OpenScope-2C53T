/*
 * OpenScope 2C53T - Custom Firmware
 *
 * Target: Artery AT32F403A (ARM Cortex-M4F @ 240MHz)
 * Display: ST7789V 320x240 via EXMC/XMC
 * RTOS: FreeRTOS
 *
 * This firmware initializes the LCD, draws a scope-like UI,
 * and responds to button inputs for mode selection.
 */

#include "at32f403a_407.h"
#include "cal_dump.h"

/* AT32 clock config (from at32f403a_gcc/user/) */
extern void system_clock_config(void);
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "timers.h"

/* Include our drivers and UI */
#include "lcd.h"
#include "lcd_health.h"
#include "ui.h"
#include "signal_gen.h"
#include "ui/siggen_edit.h"
#include "ui/scope_freq.h"
#include "watchdog.h"
#include "scope_state.h"
#include "scope_cal.h"
#include "scope_vpos.h"
#include "scope_trigger.h"
#include "scope_selfcal_run.h"
#include "input_handler.h"
#include "theme.h"
#include "math_channel.h"
#include "persistence.h"
#include "button_scan.h"
#include "continuity_buzzer.h"
#include "dfu_boot.h"
#include "battery.h"
#include "fpga.h"
#include "meter_autoselect.h"
#include "meter_data.h"
#include "meter_voltage_wave.h"
#include "flash_fs.h"
#include "settings_store.h"
#include "startup_policy.h"
#include "cal_backup.h"
#include "usercal.h"
#include "usb_debug.h"
#include "rtt.h"
#include "fault.h"
#include "redraw_gate.h"
#include "screen.h"
#include "legacy_screens.h"
#include "scope_mask.h"
#include "can_scope.h"
#include "osc_menu_model.h"
#include "meas_select.h"
#include "sys_pwm.h"
#include "logo_ui.h"
#include "model.h"
#include "meter_link_policy.h"

/* ═══════════════════════════════════════════════════════════════════
 * Global State (extern'd via ui.h for UI modules)
 * ═══════════════════════════════════════════════════════════════════ */

#if FPGA_WARM_HANDOFF_TEST
/* Warm-handoff build: boot straight into scope mode — the LCD (scope screen +
 * debug overlay) is the only readout on the bench unit (USB CDC dead). */
volatile device_mode_t current_mode = MODE_OSCILLOSCOPE;
#else
volatile device_mode_t current_mode = MODE_MULTIMETER;
#endif
volatile startup_mode_t startup_mode = STARTUP_METER;
volatile uint32_t      uptime_seconds = 0;
volatile int8_t        settings_selected = 0;
volatile int8_t        settings_depth = 0;
volatile int8_t        settings_sub_selected = 0;
volatile uint8_t       active_channel = 0;  /* 0=CH1, 1=CH2 */
volatile uint8_t       meter_submode = 0;   /* 0-10: current meter sub-mode */
volatile uint8_t       meter_layout = 0;   /* 0=full, 1=chart, 2=stats, 3=fuse */
volatile bool          meter_rel_enabled = false;  /* Relative/delta mode */
volatile float         meter_rel_reference = 0.0f;
volatile bool          meter_hold_enabled = false;  /* Auto-hold mode */
volatile bool          meter_hold_locked = false;   /* Hold has captured */
volatile float         meter_hold_value = 0.0f;

/* Fuse tester state */
volatile uint8_t       fuse_type = 0;               /* FUSE_TYPE_ATO_ATC */
volatile uint8_t       fuse_rating_idx = 6;          /* 10 A (index 6 in the ATO table) */
volatile uint8_t       fuse_view = 0;                /* FUSE_VIEW_DETAIL */
volatile float         fuse_scan_threshold_mv = 0.1f; /* Scan: DRAW at or above this drop */

/* Modal overlay lock. While true, the display task suppresses ALL rendering
 * (queue commands are drained and dropped, periodic repaints skipped) so an
 * overlay painted from another task — currently only the BTN_POWER shutdown
 * countdown in input_handler.c — stays on screen instead of being overdrawn
 * by the next scope frame. The overlay owner clears the flag and sends
 * DCMD_REDRAW_ALL to hand the screen back. */
volatile bool          ui_modal_active = false;
volatile uint32_t      ui_modal_idle_count = 0;

/* Scope feature toggles */
volatile bool          math_enabled = false;
volatile uint8_t       math_op = 0;        /* MATH_ADD */
volatile bool          persist_enabled = false;

/* Startup-on-Boot (parity M6, upstream F07). The choice is part of the
 * settings record (device_config_t.startup_mode, saved by settings_store with
 * everything else); main() applies it right after settings_store_init().
 * Until 2026-11 it lived in its own record in the MCU's flash at 0x080FF800,
 * which was erased and rewritten on every change and never consulted in the
 * bench builds. */

const char *startup_mode_name(startup_mode_t mode)
{
    switch (mode) {
    case STARTUP_SCOPE:  return "Scope";
    case STARTUP_METER:  return "Meter";
    case STARTUP_HOME:   return "Home";       /* stock's "None" */
    case STARTUP_SIGGEN: return "Signal Gen";
    default:             return "?";
    }
}

void startup_mode_set(startup_mode_t mode)
{
    if (mode >= STARTUP_COUNT) return;
    startup_mode = mode;       /* captured on the next key, or at once via input_request_settings_flush() (shell) */
}

#ifndef FPGA_WARM_HANDOFF_TEST
#define FPGA_WARM_HANDOFF_TEST 0
#endif
#ifndef FPGA_METER_SUBMODES
#define FPGA_METER_SUBMODES 0   /* as fpga.c: lifts the "scope-only" half of a warm build */
#endif

static device_mode_t startup_target_mode(void)
{
    return startup_boot_mode(startup_mode, FPGA_WARM_HANDOFF_TEST != 0,
                             FPGA_METER_SUBMODES != 0);
}

#if FEATURE_FFT
volatile scope_view_t scope_view = SCOPE_VIEW_TIME;
fft_result_t fft_result;
#endif

/* FreeRTOS handles */
static TaskHandle_t  xDisplayTaskHandle = NULL;
static TaskHandle_t  xInputTaskHandle   = NULL;
static QueueHandle_t xDisplayQueue      = NULL;
/* Last key event (input task), for the LCD health log. */
static volatile TickType_t g_last_key_tick;
static volatile uint16_t   g_last_key_id;
static QueueHandle_t xInputQueue        = NULL;

/* Health monitor slots (assigned in main, used in task loops) */
static int health_slot_display = -1;
static int health_slot_input   = -1;

/* ═══════════════════════════════════════════════════════════════════
 * Simple delay (used before RTOS starts)
 * ═══════════════════════════════════════════════════════════════════ */

/* delay_ms is declared extern in lcd.h — provide the implementation here */
void delay_ms(uint32_t ms)
{
    volatile uint32_t count;
    while (ms--) {
        count = system_core_clock / 10000;  /* Clock-speed independent */
        while (count--) {
            __asm volatile("nop");
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * Redraw gating for the scope and signal-generator screens
 *
 * The multimeter was fixed on 2026-04-04 by refusing to repaint unless
 * its reading counter had moved. Scope and siggen kept the old
 * behaviour: the scope repainted on an unconditional 1 s tick whether or
 * not anything had changed, and took the clear-and-repaint path at the
 * capture rate whenever cursors were on or the view was FFT/waterfall;
 * siggen full-cleared the whole 320x240 on every button press, including
 * presses that hit a clamp and changed nothing.
 *
 * The gate itself is in ui/redraw_gate.h (pure C, host-tested). This
 * file supplies the two "epochs" — hashes over exactly the state each
 * screen renders. Equal epoch + no new samples => the next draw would
 * produce identical pixels.
 *
 * Cadences below are in display-task ticks (50 ms each).
 * ═══════════════════════════════════════════════════════════════════ */

/* Floor between data-driven FULL repaints. The live compositor
 * (draw_scope_live_frame) is exempt — it never blanks — so this only
 * throttles the paths that do clear: FFT, split, waterfall, cursors-on,
 * and the pre-data demo state. 4 ticks = 5 Hz. */
#define SCOPE_FULL_MIN_FRAMES   4

/* Cadence while the screen is genuinely a function of `frame`: the
 * synthetic demo waveform before the first real sample, and the
 * persistence overlay (which decays once per draw). Unchanged from the
 * pre-B2 tick so the demo animation looks the same. */
#define SCOPE_ANIM_TICK_FRAMES  20

/* Idle heartbeat, split by whether the heartbeat blanks the screen.
 *
 * Something must still tick over when nothing changes, because the
 * SCOPE_DEBUG_OVERLAY strip (compiled into every build today, Makefile
 * C_DEFS) shows SPI3 timeout and status counters that move without
 * touching anything the epoch can see — and that strip is the only
 * instrument on bench unit #1.
 *
 * draw_scope_live_frame() redraws that strip as opaque fixed-width text
 * with no refill (scope_ui.c), so when the compositor is available the
 * heartbeat costs no blank and can stay at the pre-B2 1 s. When it is
 * not available the heartbeat is a full clear-and-repaint, and that is
 * the one that was flashing a settled screen once a second forever. */
#define SCOPE_IDLE_LIVE_FRAMES  20    /* 1 s, flicker-free */
#define SCOPE_STRIP_REFRESH_FRAMES 10 /* 500 ms: measurement cells in place */
#define SCOPE_IDLE_FULL_FRAMES  100   /* 5 s, blanks — back it off */

/* Draw accounting. Non-static on purpose: these are the evidence that
 * the gate works, and they need to be reachable from a debugger or a
 * future overlay without recompiling. */
volatile uint32_t ui_scope_full_draws  = 0;
volatile uint32_t ui_scope_live_draws  = 0;
volatile uint32_t ui_redraw_all_count  = 0;   /* DCMD_REDRAW_ALL handled (any mode) */
volatile uint32_t ui_scope_skipped     = 0;
volatile uint32_t ui_siggen_full_draws = 0;
volatile uint32_t ui_siggen_skipped    = 0;

static redraw_gate_t scope_gate  = { 0, 0, 0, false, false };
static redraw_gate_t siggen_gate = { 0, 0, 0, false, false };

/* Hash of everything draw_scope_screen()/draw_fft_screen()/
 * draw_waterfall_screen() read out of program state. Sample data is NOT
 * hashed — that is what the new_data term is for — so this stays valid
 * as the FFT/waterfall/measurement-badge inputs get rewired to real
 * capture. */
/* The bottom info bar is drawn on a full-screen redraw only (DCMD_REDRAW_ALL,
 * which every button path sends). A state change from anywhere else -- the
 * USB shell's `fpga scope trigmode`, `mode scope` -- left it stale: on
 * 2026-09-30 it read "Normal" while the scope was in SINGLE. This hashes
 * exactly what draw_info_bar() prints, and the display loop repaints the
 * bar whenever it changes, whoever changed it. */
static uint32_t g_info_bar_epoch;
/* Set whenever this loop iteration repainted the current non-scope screen,
 * so the out-of-scope popup is re-drawn on top of it (softkey UI P0). */
static bool g_screen_painted;

static uint32_t info_bar_epoch(void)
{
    uint32_t h = REDRAW_EPOCH_SEED;
    h = redraw_epoch_mix(h, (uint32_t)current_mode | ((uint32_t)theme_get_id() << 8));
    if (current_mode == MODE_OSCILLOSCOPE) {
        /* What draw_info_bar() prints in the time view, hashed by the bar's
         * owner (status_bar.c): channel badges as drawn (measured volts/div,
         * probe, coupling, on/off), the trigger badge including its level,
         * and the scope view. The timebase moved to the top bar, which
         * repaints its own slots. */
        h = redraw_epoch_mix(h, status_bar_info_epoch());
        /* The FFT view's own bar (window, averaging, max hold). */
#if FEATURE_FFT
        const fft_config_t *cfg = fft_get_config();
        if (cfg)
            h = redraw_epoch_mix(h, (uint32_t)cfg->window | ((uint32_t)cfg->avg_count << 8)
                                    | ((uint32_t)cfg->max_hold << 24));
#endif
    } else if (current_mode == MODE_MULTIMETER) {
        h = redraw_epoch_mix(h, (uint32_t)meter_submode);
        h = redraw_epoch_mix(h, meter_softkeys_epoch());
    }
    return h;
}

/* ── The meter link policy, evaluated here and nowhere else ──────────────
 *
 * Once per display frame (<= 50 ms), in the place the mode-change block has
 * always switched PC11: the screen, the USB host state and meter.link go
 * into meter_link_want_on() (meter_link_policy.h) and the answer goes to
 * fpga_meter_link_set(), which acts only on a change. So a screen change is
 * seen on the frame it used to be, a `set meter.link` or the Settings row
 * within one frame, and a USB host appearing or leaving once its state has
 * held for a second (a re-enumeration must not reach PC11: every rise
 * restarts the meter SoC, ~1.4 s).
 *
 * Two things outside the policy function clamp its inputs:
 *   - an image whose scope side talks on USART2 cannot keep the meter under
 *     another screen (fpga_meter_link_background_capable): there the link
 *     follows the screen, as it always did, and meter.link has no effect;
 *   - `meter relays on` (the old relay projection, kept for A/B) makes the
 *     meter take the scope's range relays, so it also gets the old link:
 *     with the screen only. The same while a projection is still in place
 *     (fpga_meter_holds_fe: the switch was turned off inside the meter), so
 *     that leaving the meter puts the scope's relays back once.
 */
static struct {
    meter_link_debounce_t usb;
    bool usb_host, background, want_on;
} g_meter_link;

static void meter_link_service(void)
{
    const uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    const bool usb = meter_link_debounce(&g_meter_link.usb, usb_debug_connected(), now_ms,
                                         METER_LINK_USB_HOLD_MS);
    const bool background = fpga_meter_link_background_capable() && !fpga_meter_relays_get() &&
                            !fpga_meter_holds_fe();
    const meter_link_policy_t policy =
        background ? (meter_link_policy_t)prop_get(P(METER_LINK)) : METER_LINK_SCREEN;
    const bool want = meter_link_want_on(current_mode == MODE_MULTIMETER, background && usb, policy);

    g_meter_link.usb_host   = usb;
    g_meter_link.background = background;
    g_meter_link.want_on    = want;
    fpga_meter_link_set(want);
}

/* For the shell's `meter state` (read only; the display task writes). */
void meter_link_status(bool *usb_host, bool *background, bool *want_on)
{
    if (usb_host)   *usb_host   = g_meter_link.usb_host;
    if (background) *background = g_meter_link.background;
    if (want_on)    *want_on    = g_meter_link.want_on;
}

static uint32_t scope_ui_epoch(const scope_state_t *s)
{
    uint32_t h = REDRAW_EPOCH_SEED;

    h = redraw_epoch_mix(h, (uint32_t)theme_get_id());
    h = redraw_epoch_mix(h, (uint32_t)active_channel);
    h = redraw_epoch_mix(h, (uint32_t)s->timebase_idx
                            | ((uint32_t)s->running << 8)
                            | ((uint32_t)s->roll << 9)
                            | ((uint32_t)s->roll_step << 12));

    h = redraw_epoch_mix(h, (uint32_t)s->ch1.enabled
                            | ((uint32_t)s->ch1.vdiv_idx << 1)
                            | ((uint32_t)s->ch1.coupling << 8)
                            | ((uint32_t)s->ch1.probe    << 12)
                            | ((uint32_t)s->ch1.bw_limit << 16));
    h = redraw_epoch_mix(h, (uint32_t)(uint16_t)s->ch1.position);
    h = redraw_epoch_mix(h, (uint32_t)s->ch2.enabled
                            | ((uint32_t)s->ch2.vdiv_idx << 1)
                            | ((uint32_t)s->ch2.coupling << 8)
                            | ((uint32_t)s->ch2.probe    << 12)
                            | ((uint32_t)s->ch2.bw_limit << 16));
    h = redraw_epoch_mix(h, (uint32_t)(uint16_t)s->ch2.position);

    h = redraw_epoch_mix(h, (uint32_t)s->trigger.mode
                            | ((uint32_t)s->trigger.edge   << 4)
                            | ((uint32_t)s->trigger.source << 8));
    h = redraw_epoch_mix(h, (uint32_t)(uint16_t)s->trigger.level);

    /* The measurement selection, both channels (meas_select.h: CH1's is
     * prm_mask, CH2's lives in meas_select.c); prm_hidden is mixed below. */
    h = redraw_epoch_mix(h, (uint32_t)meas_select_get(0)
                            | ((uint32_t)meas_select_get(1) << 16));

    /* Cursor mode only: it moves the strip (the AUX row). Positions and the
     * selection are drawn by the live frame (scope_ui_request_live). */
    h = redraw_epoch_mix(h, (uint32_t)s->cursor.mode);

    h = redraw_epoch_mix(h, (uint32_t)math_enabled
                            | ((uint32_t)math_op         << 1)
                            | ((uint32_t)persist_enabled << 8)
                            | ((uint32_t)persist_get_mode() << 9));
    h = redraw_epoch_mix(h, (uint32_t)s->prm_hidden);   /* long PRM */
    /* The trigger status (Trig'd / Wait / Auto* ...) left the plot at the UI
     * round 1 merge: the status bar's dot and RUN tab show it and repaint
     * their own slots, so it does not force a full plot repaint. */

#if FEATURE_FFT
    h = redraw_epoch_mix(h, (uint32_t)scope_view);
    {
        const fft_config_t *fc = fft_get_config();
        if (fc != NULL) {
            h = redraw_epoch_mix_f(h, fc->ref_level_db);
            h = redraw_epoch_mix_f(h, fc->db_range);
            h = redraw_epoch_mix_f(h, fc->sample_rate_hz);
            h = redraw_epoch_mix(h, (uint32_t)fc->window
                                    | ((uint32_t)fc->peak_count << 8)
                                    | ((uint32_t)fc->avg_count  << 16)
                                    | ((uint32_t)fc->max_hold   << 24));
            h = redraw_epoch_mix(h, ((uint32_t)fc->zoom_start_bin << 16)
                                    | fc->zoom_end_bin);
        }
    }
#endif
    return h;
}

/* Hash of everything draw_siggen_screen() renders. */
static uint32_t siggen_ui_epoch(void)
{
    uint32_t h = REDRAW_EPOCH_SEED;
    const siggen_config_t *c = siggen_get_config();

    h = redraw_epoch_mix(h, (uint32_t)theme_get_id());
    if (c == NULL) return h;

    h = redraw_epoch_mix(h, (uint32_t)c->waveform
                            | ((uint32_t)c->duty_pct << 8)
                            | ((uint32_t)c->output_enabled << 16)
                            | ((uint32_t)c->amplitude_dv << 24));
    h = redraw_epoch_mix(h, c->frequency_hz);
    /* Editor state and boot policy: a cursor move changes neither the
     * frequency nor the waveform, so without these it would not repaint. */
    {
        const siggen_edit_t *ed = siggen_ui_editor();
        h = redraw_epoch_mix(h, (uint32_t)ed->active | ((uint32_t)ed->digit << 1)
                                | ((uint32_t)c->start_on << 8));
    }
    return h;
}

/* Is the scope showing the roll view (C11)? Time view with roll on. */
static bool s_roll_view(void)
{
#if FEATURE_FFT
    if (scope_view != SCOPE_VIEW_TIME) return false;
#endif
    return scope_state_get()->roll;
}

/* Can this frame be served by the flicker-free column compositor?
 * It draws the trace band only, so anything living outside that band or
 * layered on top (popup, cursors) forces the full path. */
static bool scope_incremental_available(const scope_state_t *s)
{
#ifdef EMULATOR_BUILD
    (void)s;
    return false;   /* draw_scope_live_frame reads FPGA buffers */
#else
#if FEATURE_FFT
    /* FFT, split, waterfall and X-Y compose every pixel (no blank phase). */
    if (scope_view != SCOPE_VIEW_TIME) return true;
#endif
    /* Roll: its compositor needs no record and draws no cursors. */
    if (s->roll) return true;
    /* A popup no longer disqualifies the compositor. It used to, and that is
     * what made every timebase change blank the whole screen ten times over:
     * the popup pinned the renderer to the full clear-then-redraw for its
     * entire life. draw_scope_live_frame() now steps around the popup box.
     * Cursors are a layer of the compositor (2026-11-13). */
    /* Math and persistence (C12) are layers of the compositor too
     * (2026-11-13). A re-sampled held
     * record (zoom while paused, C14) no longer forces the full path: both
     * renderers draw the one window view_build() fills (scope_ui.c), blank
     * columns included. */
    return scope_acquisition_ready();
#endif
}

/* Single scope render entry point, shared by the queue commands and the
 * periodic path so both always pick the same renderer for a given view. */
/* The Oscilloscope Settings page (osc_menu_model.h) is up. It is a full page
 * in scope mode: osc_menu_draw() owns y 16..223, between the two bars, so the
 * scope's own paints (plot, badge rows, mask/CAN rows) stand aside while it
 * is up; the bars keep repainting. Not ui_modal_active: that would drop DCMD_DRAW_OSC_MENU too. The
 * acquisition keeps running behind it (UI_STUDY 2.3). */
static bool osc_page_up(void)
{
    return current_mode == MODE_OSCILLOSCOPE && osc_menu_is_open();
}

static void scope_render(uint32_t frame, redraw_action_t action)
{
#if FEATURE_FFT
    /* The other views compose every pixel and never blank: an incremental
     * frame is the same drawing without the once-only margins and titles. */
    if (scope_view != SCOPE_VIEW_TIME) {
        const bool full = (action != REDRAW_INCREMENTAL);
        if (full) ui_scope_full_draws++; else ui_scope_live_draws++;
        switch (scope_view) {
        case SCOPE_VIEW_FFT:       draw_fft_screen(full);          break;
        case SCOPE_VIEW_SPLIT:     draw_split_screen(frame, full); break;
        case SCOPE_VIEW_WATERFALL: draw_waterfall_screen();        break;
        case SCOPE_VIEW_XY:        draw_xy_screen(full);           break;
        default:                                                   break;
        }
        scope_popup_view_tick();
        return;
    }
#endif
    /* Roll (C11) has its own compositor over the roll FIFO; the Y-T paths
     * below would draw the stale 1024-sample record. */
    if (s_roll_view()) {
        const bool inc = (action == REDRAW_INCREMENTAL);
        if (inc) ui_scope_live_draws++; else ui_scope_full_draws++;
        draw_roll_screen(!inc);
        if (inc) scope_popup_overlay_tick();
        return;
    }
#ifndef EMULATOR_BUILD
    if (action == REDRAW_INCREMENTAL) {
        ui_scope_live_draws++;
        draw_scope_live_frame();
        scope_popup_overlay_tick();
        return;
    }
#endif
    ui_scope_full_draws++;
    draw_scope_screen(frame);
}

/* ═══════════════════════════════════════════════════════════════════
 * FreeRTOS Tasks
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * Display Task (Priority 1 — lowest, matches original firmware)
 *
 * Receives display commands via queue and renders the appropriate screen.
 * This is the only task that writes to the LCD.
 */
static void vDisplayTask(void *pvParameters)
{
    (void)pvParameters;
    uint8_t cmd;
    uint32_t frame = 0;

    /* The splash is already on screen — main() paints it before the backlight
     * comes up and before the FPGA config sequence, so by the time this task
     * runs it has typically been visible for seconds. Hold it briefly for
     * builds where init is fast (emulator, warm boots), then move on. */
    vTaskDelay(pdMS_TO_TICKS(750));

    /* Initial draw, unless the boot logo still owns the screen:
     * logo_ui_boot_show() claimed it in main() and the key task's ~1 s hold
     * ends with DCMD_REDRAW_ALL, which paints all of this. Drawing here
     * regardless cut the logo short at 750 ms. */
    if (!ui_modal_active) {
        lcd_clear(COLOR_BLACK);
        draw_status_bar();
        draw_info_bar();
        if (current_mode == MODE_MULTIMETER) {
            draw_meter_screen();
        } else if (current_mode == MODE_HOME) {
            draw_home_screen();
        } else if (current_mode == MODE_SIGNAL_GEN) {
            draw_siggen_screen(0);        /* Startup on Boot = Signal Gen */
        } else {
            draw_scope_screen(0);
        }
    }

    for (;;) {
        /* Track mode transitions: for SPI3 acquisition warmup, and to
         * invalidate both redraw gates. Entering a screen must always
         * repaint it, even when that screen's own state is unchanged
         * from the last time it was visible. Hoisted above the queue
         * handling (2026-08-13) so a DCMD arriving on the same tick as a
         * mode switch sees an already-invalidated gate. */
        static device_mode_t last_rendered_mode = (device_mode_t)0xFF;
        static uint32_t scope_entered_frame = 0;
        meter_link_service();
        if (current_mode != last_rendered_mode) {
            /* PC11 = meter MUX enable. Stock switches it WITH the mode (HIGH in
             * meter mode, LOW in scope mode); we used to pin it LOW forever, so
             * the meter was dead in every scope build and every USART
             * measurement was taken with the far end switched off. Bench A/B/A
             * 2026-08-17: HIGH -> 276 bytes returned, LOW -> 0, HIGH -> 276.
             * It now follows the meter link policy, evaluated just above
             * (meter_link_service): the screen is one of its three inputs. */
            if (current_mode == MODE_OSCILLOSCOPE) {
                scope_entered_frame = frame;
                ui_popup_overlay_cancel();    /* scope popups are the scope's own */
            }
            redraw_gate_invalidate(&scope_gate);
            redraw_gate_invalidate(&siggen_gate);
            last_rendered_mode = current_mode;
        }

#ifndef EMULATOR_BUILD
        /* LCD panel health (util/lcd_health.h): about once a second read the
         * ST7789's power mode; a panel that reset itself under the running MCU
         * is re-initialised and repainted instead of staying white. */
        {
            static TickType_t last_lcd_check;
            const TickType_t now = xTaskGetTickCount();
            if (!ui_modal_active && (now - last_lcd_check) >= pdMS_TO_TICKS(500)) {
                last_lcd_check = now;
                taskENTER_CRITICAL();
                uint8_t pm = lcd_read_power_mode();
                taskEXIT_CRITICAL();
                bool reinit = lcd_health_step(&g_lcd_health, pm, (uint32_t)(now * portTICK_PERIOD_MS),
                                              (uint8_t)current_mode);
                if (!reinit && !lcd_health_pm_ok(pm)) {
                    /* Confirm a bad read 10 ms later instead of on the next
                     * check: the white screen lasts well under a second. */
                    vTaskDelay(pdMS_TO_TICKS(10));
                    taskENTER_CRITICAL();
                    pm = lcd_read_power_mode();
                    taskEXIT_CRITICAL();
                    reinit = lcd_health_step(&g_lcd_health, pm,
                                             (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS),
                                             (uint8_t)current_mode);
                }
                if (reinit) {
                    g_lcd_health.event_since_key_ms =
                        (uint32_t)((xTaskGetTickCount() - g_last_key_tick) * portTICK_PERIOD_MS);
                    g_lcd_health.event_key = g_last_key_id;
                    g_lcd_health.event_buzzer = (uint8_t)TMR11->c1dt;
                    g_lcd_health.event_backlight = (uint8_t)TMR10->c1dt;
                    lcd_panel_init();
                    const uint8_t redraw = DCMD_REDRAW_ALL;
                    (void)xQueueSend(xDisplayQueue, &redraw, 0);
                }
            }
        }
#endif

        /* Check for commands (non-blocking with short timeout for animation).
         * While a modal overlay owns the screen (ui_modal_active), commands
         * are still drained — senders must never block — but dropped instead
         * of rendered: one frame painted here would overdraw the overlay,
         * which is exactly the countdown flicker this flag exists to stop.
         * The overlay owner ends with DCMD_REDRAW_ALL, which repaints
         * everything a dropped command would have. */
        /* The tick is 50 ms of WALL time, not 50 ms of waiting after the
         * work: a frame that took 40 ms used to be followed by a full 50 ms
         * wait, so a 40 ms live frame ran at 11 Hz instead of 20. Wait only
         * for what is left of the tick (at least one RTOS tick, so the idle
         * task still runs). */
        static TickType_t tick_due;
        TickType_t wait = 1;
        {
            const TickType_t now = xTaskGetTickCount();
            if ((int32_t)(tick_due - now) > 1)
                wait = tick_due - now;
        }
        if (xQueueReceive(xDisplayQueue, &cmd, wait) == pdTRUE) {
            if (ui_modal_active)
                cmd = DCMD_NONE_MODAL_DROP;
            /* DCMD_DRAW_ACTIVE + the sender's screen: that screen's own
             * command below, whose checks drop it if the screen has gone
             * since (ui/legacy_screens.c). */
            cmd = legacy_screens_resolve_cmd(cmd);
            switch (cmd) {
            case DCMD_DRAW_SPLASH:
                draw_splash();
                break;
            case DCMD_REDRAW_ALL:
                ui_redraw_all_count++;
                /* NEVER GATED, deliberately. This is the hard invalidate,
                 * and some of its senders have already scribbled on the
                 * LCD from outside this task — BTN_POWER paints its
                 * "Hold to power off" overlay inline (input_handler.c) and
                 * relies on REDRAW_ALL to erase it on release. An epoch
                 * comparison cannot see that, so gating here would strand
                 * the overlay on screen. Screens that want cheap,
                 * gate-able repaints send their own DCMD_DRAW_* instead. */
                lcd_clear(COLOR_BLACK);
                status_bar_invalidate();
                draw_status_bar();
                draw_info_bar();
                g_info_bar_epoch = info_bar_epoch();
                if (osc_page_up()) {
                    /* The page covers y 16..223; both bars stay visible.
                     * Also what repaints the page after the gallery or an
                     * LCD-health reinit: the LCD was cleared under it, so
                     * the page repaints its background too. */
                    osc_menu_invalidate();
                    osc_menu_draw();
                    g_screen_painted = true;
                    break;
                }
                /* Draw current mode's screen */
                if (current_mode == MODE_OSCILLOSCOPE) {
                    scope_render(frame, REDRAW_FULL);
                    redraw_gate_mark(&scope_gate,
                                     scope_ui_epoch(scope_state_get()),
                                     frame, REDRAW_FULL);
                } else if (current_mode == MODE_MULTIMETER) {
                    meter_screen_invalidate();
                    draw_meter_screen();
                } else if (current_mode == MODE_SIGNAL_GEN) {
                    ui_siggen_full_draws++;
                    draw_siggen_screen(frame);
                    redraw_gate_mark(&siggen_gate, siggen_ui_epoch(),
                                     frame, REDRAW_FULL);
                } else if (current_mode == MODE_SETTINGS) {
                    draw_settings_screen();
                } else if (current_mode == MODE_HOME) {
                    draw_home_screen();
                }
                g_screen_painted = true;
                break;
            case DCMD_DRAW_SCOPE:
                /* Always honoured: the sender changed scope state or
                 * raised a popup. Marking the gate stops the periodic
                 * path from immediately repeating the same paint. Not while
                 * the Oscilloscope Settings page covers the plot. */
                if (current_mode == MODE_OSCILLOSCOPE && !osc_page_up()) {
                    scope_render(frame, REDRAW_FULL);
                    redraw_gate_mark(&scope_gate,
                                     scope_ui_epoch(scope_state_get()),
                                     frame, REDRAW_FULL);
                }
                break;
            case DCMD_DRAW_METER:
                if (current_mode == MODE_MULTIMETER) {
                    draw_meter_screen();
                    g_screen_painted = true;
                }
                break;
            case DCMD_DRAW_SIGGEN:
                if (current_mode == MODE_SIGNAL_GEN) {
                    if (siggen_gate.primed &&
                        siggen_ui_epoch() == siggen_gate.drawn_epoch) {
                        ui_siggen_skipped++;
                        break;
                    }
                    ui_siggen_full_draws++;
                    draw_siggen_screen(frame);
                    redraw_gate_mark(&siggen_gate, siggen_ui_epoch(),
                                     frame, REDRAW_FULL);
                    g_screen_painted = true;
                }
                break;
            case DCMD_DRAW_SETTINGS:
                if (current_mode == MODE_SETTINGS) {
                    draw_settings_screen();
                    g_screen_painted = true;
                }
                break;
            case DCMD_DRAW_STATUS_BAR:
                draw_status_bar();
                break;
            case DCMD_DRAW_HOME:
                if (current_mode == MODE_HOME) {
                    draw_home_selection();
                    g_screen_painted = true;
                }
                break;
            case DCMD_DRAW_OSC_MENU:
                if (osc_page_up()) {
                    osc_menu_draw();
                    g_screen_painted = true;
                }
                break;
#if FEATURE_FFT
            case DCMD_DRAW_FFT:
                if (current_mode == MODE_OSCILLOSCOPE && !osc_page_up()) {
                    ui_scope_full_draws++;
                    draw_fft_screen(true);
                    redraw_gate_mark(&scope_gate,
                                     scope_ui_epoch(scope_state_get()),
                                     frame, REDRAW_FULL);
                }
                break;
#endif
            default:
                break;
            }
        }

        /* Check in with health monitor */
        health_checkin(health_slot_display);

#ifndef EMULATOR_BUILD
        if (fpga_service_requests()) {
            scope_entered_frame = frame;
        }
#endif

        /* Periodic redraws for active modes.
         *
         * All modes receive explicit redraws via queue commands above
         * (DCMD_DRAW_SCOPE, DCMD_DRAW_SIGGEN, etc.) when button presses
         * or data events require it. The code below only handles cases
         * that need continuous or periodic updates beyond those events.
         *
         * Before the meter fix (2026-04-04), scope and siggen both
         * redrew unconditionally every 50ms tick, causing visible
         * flicker from the full-area lcd_fill_rect + repaint sequence.
         * The meter was gated then; scope and siggen were gated on
         * 2026-08-13 (B2) via redraw_gate.h — see the epoch functions
         * above. All three data modes are now gated the same way.
         *
         * ui_modal_active suppresses this whole section: while the shutdown
         * countdown (or any future modal) owns the screen, a live scope
         * would otherwise overdraw it within one 50 ms tick — the "flashing
         * countdown" bug (2026-08-20). Acquisition heartbeats pause with it;
         * three seconds of paused re-arm is harmless and resumes on the
         * DCMD_REDRAW_ALL that ends every modal. */
        if (ui_modal_active) {
            ui_modal_idle_count++;
            frame++;
            continue;
        }
        /* The info bar shows under the Oscilloscope Settings page too, so a
         * channel or trigger change made on the page shows in it at once. */
        const bool page_up = osc_page_up();
        {
            /* RUN/STOP/ARM tab and trigger dot: repaint the bar when what
             * they show changes, not on the next 1 s tick (the slots that
             * did not change are not touched). Under the page too. */
            static uint32_t g_status_live_key;
            const uint32_t sk = status_bar_live_key();
            if (sk != g_status_live_key) {
                draw_status_bar();
                g_status_live_key = sk;
            }
        }
        {
            uint32_t ib = info_bar_epoch();
            if (ib != g_info_bar_epoch) {
                draw_info_bar();
                g_info_bar_epoch = ib;
            }
        }
        if (current_mode == MODE_OSCILLOSCOPE) {
            const scope_state_t *ss_anim = scope_state_get();
            /* Mask pass/fail readout (badge row 2, below the live band): its
             * own epoch, like the info bar, so a counter never sits stale
             * waiting for a full repaint. Time view only -- the slot belongs
             * to the time-view badge layout. */
            {
                static uint32_t g_mask_slot_epoch;
                uint32_t me = scope_mask_epoch();
#if FEATURE_FFT
                const bool time_view = (scope_view == SCOPE_VIEW_TIME);
#else
                const bool time_view = true;
#endif
                if (me != g_mask_slot_epoch && time_view && !page_up) {
                    scope_ui_mask_slot_refresh();
                    g_mask_slot_epoch = me;
                }
                /* CAN readout (`can screen on`): 5 Hz, its own row. */
                if (time_view && !page_up && can_scope_screen_on() && (frame & 3u) == 0u)
                    scope_ui_can_row_refresh();
            }
            if (ss_anim->running) {
                /* ── Scope heartbeat / acquisition re-arm ───────────
                 * Keep the existing warmup and cadence for now, but use the
                 * stock-like cmd-3 path instead of a bare SPI trigger. That
                 * re-applies timebase state before each queued read, matching
                 * the live shell experiments more closely. */
#ifndef EMULATOR_BUILD
                if ((frame - scope_entered_frame) >= 10 /* 500ms warmup */
                    && (frame % 7) == 0
                    && fpga.initialized) {
                    fpga_scope_heartbeat();
                }
#endif
            }
            {
                /* Redraw decision (B2, 2026-08-13). Also while STOPPED:
                 * nothing new arrives then, so the gate skips, but a popup
                 * still has to be shown and erased on wall-clock time
                 * (until 2026-11-13 this block was inside the running test
                 * and a popup raised while stopped stayed up for good).
                 *
                 * Previously: popup OR new SPI3 data OR an unconditional
                 * 1 s tick. The 1 s arm repainted a settled screen once a
                 * second forever, and the data arm ran the full
                 * clear-and-repaint at the capture rate in every view the
                 * live compositor cannot serve — the flash observed on
                 * the bench 2026-08-12.
                 *
                 * Now: redraw_gate_step() picks between skipping, the
                 * flicker-free compositor (unthrottled, so the live trace
                 * still updates at the capture rate) and a rate-capped
                 * full repaint, using an epoch over everything the screen
                 * renders. Rendering pass note (2026-08-12) still holds:
                 * the full clear-then-redraw path stays for popups (they
                 * overwrite the band and need erasing afterward), cursors
                 * (drawn only by the full path), the demo/pre-data state,
                 * and all button-driven DCMD redraws. */
                static uint16_t last_spi3_ok = 0;
                uint16_t spi3_now = fpga.spi3_ok_count;
                bool new_data = (spi3_now != last_spi3_ok);
                last_spi3_ok = spi3_now;

                redraw_query_t q;
                q.frame            = frame;
                q.epoch            = scope_ui_epoch(ss_anim);
                /* A live popup counts as new data so the compositor runs on
                 * every 50 ms tick while it is up, and once more to erase it
                 * (its lifetime is wall-clock, scope_ui.c POPUP_MS). */
                q.new_data         = new_data || scope_popup_active()
                                     || scope_ui_take_live_request();
                q.incremental_ok   = scope_incremental_available(ss_anim);
                /* Only the full path can show a popup when the compositor is
                 * unavailable (demo trace, cursors on, FFT views). When it IS
                 * available the popup rides along as an overlay, so forcing
                 * here would reinstate exactly the flashing this replaced. */
                q.force            = scope_popup_active() && !q.incremental_ok;
                /* Both of these genuinely produce new pixels every tick
                 * from `frame` alone: the synthetic demo waveform (only
                 * drawn before the first real sample) and the
                 * persistence overlay (which decays once per draw). */
                /* X-Y draws its own phosphor decay every frame, so it must
                 * tick at the fast floor (200 ms) rather than the 1 s anim
                 * cadence — otherwise the trail fades in visible 1 s steps. */
                q.animating        = !scope_acquisition_ready() || persist_enabled
                                     || scope_view == SCOPE_VIEW_XY;
                q.full_min_frames  = SCOPE_FULL_MIN_FRAMES;
                /* Persistence fades on wall time: with the live frame serving
                 * it, a tick every 200 ms keeps a stopped trace fading
                 * smoothly at no blanking cost. */
                q.anim_tick_frames = (scope_view == SCOPE_VIEW_XY ||
                                      (persist_enabled && q.incremental_ok))
                                     ? SCOPE_FULL_MIN_FRAMES
                                     : SCOPE_ANIM_TICK_FRAMES;
                /* Stopped, nothing a hash cannot see changes: no heartbeat
                 * (it drew one live frame a second while stopped). */
                q.idle_tick_frames = !ss_anim->running ? 0u
                                   : q.incremental_ok ? SCOPE_IDLE_LIVE_FRAMES
                                                      : SCOPE_IDLE_FULL_FRAMES;

                redraw_action_t act = page_up ? REDRAW_SKIP
                                              : redraw_gate_step(&scope_gate, &q);
                if (act == REDRAW_SKIP)
                    ui_scope_skipped++;
                else
                    scope_render(frame, act);
#if FEATURE_FFT
                const bool yt_view = !page_up && scope_view == SCOPE_VIEW_TIME;
#else
                const bool yt_view = !page_up;
#endif
                if (act != REDRAW_SKIP && yt_view)
                    scope_ui_cursor_row_refresh();   /* only when its text changed */
                /* The badges between full frames (the live frame does not
                 * touch the strip): in place, twice a second, running only
                 * (a held record's values do not change). */
                {
                    static uint32_t last_strip;
                    if (act == REDRAW_FULL)
                        last_strip = frame;
                    else if (yt_view && ss_anim->running &&
                             (frame - last_strip) >= SCOPE_STRIP_REFRESH_FRAMES) {
                        scope_ui_strip_refresh();
                        last_strip = frame;
                    }
                }
            }
        } else if (current_mode == MODE_SIGNAL_GEN) {
            /* Siggen UI is static: no animation, no live data. It is
             * drawn only from queue commands, and those are gated on the
             * siggen epoch above, so a settled generator screen costs
             * zero LCD writes per tick. */
        } else if (current_mode == MODE_MULTIMETER) {
            /* Redraw the meter when the visible reading changes, when the
             * submode changes, or when a debug/animated panel explicitly
             * needs a heartbeat. Rejected frames still advance raw counters,
             * but they must not clear/repaint the LCD. */
            static uint32_t last_meter_update = 0xFFFFFFFFu;
            static uint32_t last_meter_frame  = 0;
            static uint8_t  last_meter_submode = 0xFFu;
            uint32_t uc = meter_reading.display_update_count;
            bool auto_select_running = meter_autoselect_is_running();
            bool submode_changed = (meter_submode != last_meter_submode);
            bool enough_time = (frame - last_meter_frame) >= 5;  /* 20Hz loop -> max 4Hz redraw */
            bool periodic_due = meter_screen_needs_periodic_redraw() &&
                                ((frame - last_meter_frame) >= 20);
            if (!auto_select_running &&
                (submode_changed || ((uc != last_meter_update) && enough_time) ||
                 periodic_due)) {
                draw_meter_screen();
                last_meter_update = meter_screen_last_reading_display_update;
                last_meter_frame  = frame;
                last_meter_submode = meter_submode;
                g_screen_painted = true;
            }
        }

        /* Popups outside the scope (softkey UI P0, findings F15/F20): drawn
         * over the screen, re-drawn after any repaint, and the screen
         * repainted under them when they expire. The Oscilloscope Settings
         * page uses them too (scope_show_popup routes there while it is up,
         * e.g. SAVE's "Saved 3.bmp"); one still up when the page closes is
         * dropped, the scope's REDRAW_ALL has painted over it. */
        {
            static bool page_was_up;
            if (page_was_up && !page_up)
                ui_popup_overlay_cancel();
            page_was_up = page_up;
        }
        if (current_mode != MODE_OSCILLOSCOPE || page_up) {
            if (ui_popup_overlay_service(g_screen_painted) == 2) {
                if (page_up) {
                    osc_menu_invalidate();   /* the box covered part of the pane */
                    osc_menu_draw();
                } else if (current_mode == MODE_MULTIMETER) {
                    if (meter_layout != METER_LAYOUT_BIG &&
                        meter_layout != METER_LAYOUT_LIMITS)
                        meter_screen_invalidate();
                    draw_meter_screen();
                } else if (current_mode == MODE_SIGNAL_GEN) {
                    draw_siggen_screen(frame);
                    redraw_gate_mark(&siggen_gate, siggen_ui_epoch(),
                                     frame, REDRAW_FULL);
                } else if (current_mode == MODE_SETTINGS) {
                    draw_settings_screen();
                } else if (current_mode == MODE_HOME) {
                    draw_home_screen();
                }
            }
        }
        g_screen_painted = false;

        frame++;
        {
            /* Next tick 50 ms after the last; resync after a long stall. */
            const TickType_t now = xTaskGetTickCount();
            tick_due += pdMS_TO_TICKS(50);
            if ((int32_t)(now - tick_due) > (int32_t)pdMS_TO_TICKS(50) ||
                (int32_t)(tick_due - now) > (int32_t)pdMS_TO_TICKS(50))
                tick_due = now + pdMS_TO_TICKS(50);
        }
    }
}

/*
 * Input Task (Priority 4 — highest user task, matches original firmware)
 *
 * Receives hardware-debounced button presses from the TMR3 matrix scan
 * driver (button_scan.c), then delegates to input_handle_button() for
 * all action logic.
 *
 * The old GPIO polling code was replaced after hardware testing confirmed
 * the buttons use a bidirectional 4x3 matrix requiring active scanning.
 * See: reverse_engineering/analysis_v120/button_map_confirmed.md
 */
#ifdef EMULATOR_BUILD
/* Emulator key mailbox: Renode has no model of the 4x3 key matrix, so a host
 * script writes a button id + 1 here (`sysbus WriteByte <&g_emu_key> n`) and
 * the input task queues it like the `btn` shell command (input_inject_button). */
volatile uint8_t g_emu_key __attribute__((used)) = 0;
#endif

/* The property model's executor (src/model/model_exec.h): other tasks queue
 * property sets and this task applies them, after the key it just handled.
 * A request's wake is a BTN_NONE posted into the key queue. */
#include "model_exec.h"

volatile uint32_t input_events_done;   /* keys handled (the wake is not one) */

static void vInputTask(void *pvParameters)
{
    (void)pvParameters;
    button_scan_start();
    /* The screen router over the legacy screens: up before the first key and
     * before the shell's `ui screen` can ask for its stack. */
    input_router_init(xDisplayQueue);
    logo_ui_boot_hold(xInputQueue, xDisplayQueue, 1000u);   /* boot logo, any key skips */
    /* After the logo: its "any key skips" would take a wake for a key. Until
     * here a shell `set` answers BUSY. */
    model_fw_start(xDisplayQueue);

    for (;;) {
        button_id_t pressed;
#ifdef EMULATOR_BUILD
        if (g_emu_key != 0u) {
            const uint8_t k = (uint8_t)(g_emu_key - 1u);
            g_emu_key = 0u;
            /* Only real buttons: 1 would queue BTN_NONE and anything past
             * BTN_POWER an id no handler knows (review of #63). */
            if (k > (uint8_t)BTN_NONE && k <= (uint8_t)BTN_POWER)
                (void)input_inject_button((button_id_t)k);
        }
#endif

        /* Block until TMR3 ISR confirms a debounced button press */
        if (xQueueReceive(xInputQueue, &pressed, pdMS_TO_TICKS(100)) == pdTRUE &&
            pressed != BTN_NONE) {      /* BTN_NONE = a model request's wake: no key */
            g_last_key_tick = xTaskGetTickCount();
            g_last_key_id = (uint16_t)pressed;
            input_handle_button(pressed, xDisplayQueue);
            input_events_done++;
        }
        /* Property sets queued by other tasks (the shell's `set`), after the
         * key: the two never interleave. No-op when the queue is empty. */
        (void)model_exec_drain();
        /* The router mirrors the legacy state (ui/legacy_screens.h): catch
         * what the shell changed without a key (`mode`, a gallery close). */
        legacy_screens_sync();
        input_autooff_poll();   /* idle power-off (C15); no-op while "Off" */
        /* Backstop: the buzzer task ends a key click, but if it failed to
         * start a click would sound forever; this bounds it to <= ~210 ms. */
        sys_pwm_buzzer_service();
#ifndef EMULATOR_BUILD
        /* Autoset / baseline correction / 50 % centre (parity C9/C10): one
         * non-blocking pass, so a run never holds this task (or its health
         * check-in) and AUTO/MENU can always cancel it. */
        scope_selfcal_service(xDisplayQueue);
#endif

        /* Check in with health monitor */
        health_checkin(health_slot_input);
    }
}

/* Bench/test injection: post a press into the SAME queue the TMR3 key scan
 * feeds, so everything downstream of the physical scan (the 15/15 matrix is
 * hardware-confirmed) runs exactly as for a real press. Used by the shell's
 * `btn` command; returns false if the queue is full or not yet created. */
/* scope_cal's probe source: the channel's probe setting as a factor. */
static float probe_factor_from_state(uint8_t ch)
{
    const scope_state_t *ss = scope_state_get();
    const channel_state_t *c = (ch == 2u) ? &ss->ch2 : &ss->ch1;
    return (c->probe == PROBE_10X) ? 10.0f : 1.0f;
}

bool input_inject_button(button_id_t b)
{
    if (xInputQueue == NULL) return false;
    return xQueueSend(xInputQueue, &b, 0) == pdTRUE;
}

/*
 * Timer callback — runs every 1 second
 */
static void vOneSecondTimerCallback(TimerHandle_t xTimer)
{
    (void)xTimer;
    uptime_seconds++;
    battery_update();
    usb_debug_pinlog_tick();
    status_bar_second_tick();     /* the critical-battery countdown: per second, not per draw */
    uint8_t cmd = DCMD_DRAW_STATUS_BAR;
    /* Zero timeout is intentional: timer service task is highest priority,
     * so blocking here would stall all timers including the health check.
     * A dropped status bar update is harmless — the next one catches up. */
    xQueueSend(xDisplayQueue, &cmd, 0);
}

/*
 * Health check timer — runs every 500ms
 *
 * Checks all registered tasks for liveness and stack health.
 * Only feeds the watchdog if everything is OK.
 */
static void vHealthCheckCallback(TimerHandle_t xTimer)
{
    (void)xTimer;
    health_check();
}

/* ═══════════════════════════════════════════════════════════════════
 * Main
 * ═══════════════════════════════════════════════════════════════════ */

int main(void)
{
#ifdef EMULATOR_BUILD
    system_core_clock = 240000000;
    *(volatile uint32_t *)0x40021014 = 0x00000114;
    *(volatile uint32_t *)0x40021018 = 0x0000FFFD;
    *(volatile uint32_t *)0x4002101C = 0x3FFFFFFF;
#else
    /* Power hold — PC9 HIGH to keep device on (MUST be first!) */
    crm_periph_clock_enable(CRM_GPIOC_PERIPH_CLOCK, TRUE);
    GPIOC->cfghr = (GPIOC->cfghr & ~(0xF << 4)) | (0x3 << 4); /* PC9 push-pull 50MHz */
    GPIOC->scr = (1 << 9);  /* PC9 HIGH */

    /* Set VTOR. Normal build runs at 0x08004000 under our HID bootloader;
     * GUEST_BUILD runs at 0x08007000 under the FNIRSI stock bootloader (unit #2). */
#ifdef GUEST_BUILD
    SCB->VTOR = FLASH_BASE | 0x7000;
#else
    SCB->VTOR = FLASH_BASE | 0x4000;
#endif

    /* Check if previous run requested DFU reboot (magic word in RAM).
     * RAM persists across soft reset, so the magic word survives. */
    dfu_check_magic();

    /* NOTE: Boot button DFU check (dfu_check_boot_button) is disabled for now.
     * PC8 (POWER) reads LOW during power-on since it's the same button that
     * turns the device on, causing false DFU entry. Need a different trigger
     * (e.g., MENU + POWER combo, or long-hold detection with LCD feedback).
     * For now, use Settings > Firmware Update for software DFU entry. */

    /* NOTE: EOPB0 must be set to 0xFE (224KB SRAM) via DFU option bytes
     * before this firmware will work. See eopb0_setup.c or use:
     *   dfu-util -a 1 -d 2e3c:df11 -s 0x1FFFF800 -D option_bytes48.bin */

    /* Feed watchdog early — IWDG may still be running from previous boot
     * (it can't be stopped once started, survives system reset) */
    wdt_counter_reload();

    /* Arm the RTT console before anything else can want to print. Costs a few
     * hundred cycles and no peripheral, so it is safe this early — and it means
     * boot-time output is captured even if the device later hangs. */
    rtt_init();

    /* Enable the configurable fault handlers and clear stale CFSR bits before
     * anything can fault. Also recovers the record from a previous boot, which
     * survives reset in .noinit — the point of the whole exercise for a fault
     * that only shows up ~55s in. */
    fault_init();

    /* Clock init to 240MHz */
    system_clock_config();

    wdt_counter_reload();

    /* Enable peripheral clocks */
    crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_GPIOD_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_GPIOE_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_IOMUX_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_XMC_PERIPH_CLOCK, TRUE);

#if FPGA_WARM_HANDOFF_TEST
    /* Warm-handoff round 2: drive the FPGA control pins to stock's scope-run
     * posture as EARLY as possible (right after the GPIO clocks) to minimise
     * the float window. Round 1 left these floating through ~2 s of init, which
     * knocked the FPGA out of continuous-capture; we caught one buffer then it
     * went idle. Restore PC6(SPI-en)=HIGH, PB11(active)=HIGH, PC11(meter mux)=LOW
     * (scope posture) before anything else. See docs/fpga_warm_handoff_test.md. */
    GPIOC->cfglr = (GPIOC->cfglr & ~(0xFu << 24)) | (0x3u << 24); /* PC6 PP 50MHz */
    GPIOC->scr   = (1u << 6);                                     /* PC6 HIGH */
    GPIOB->cfghr = (GPIOB->cfghr & ~(0xFu << 12)) | (0x3u << 12); /* PB11 PP 50MHz */
    GPIOB->scr   = (1u << 11);                                    /* PB11 HIGH */
    GPIOC->cfghr = (GPIOC->cfghr & ~(0xFu << 12)) | (0x3u << 12); /* PC11 PP 50MHz */
    GPIOC->clr   = (1u << 11);                                    /* PC11 LOW (scope) */
#endif

    /* PB8 = LCD backlight — configured as output but held LOW (dark) until
     * the panel has been initialized and the first frame painted. Raising it
     * here used to light the ST7789V's uninitialized GRAM for the entire LCD
     * init window (~0.7 s of visible static at every boot). The pin floats
     * during the bootloader, so drive it LOW explicitly for a deterministic
     * dark panel; it goes HIGH right after draw_splash() below. */
    GPIOB->cfghr = (GPIOB->cfghr & ~(0xF << 0)) | (0x3 << 0); /* PB8 push-pull 50MHz */
    GPIOB->clr = (1 << 8);
#endif

    /* Initialize battery ADC (PB1) */
    battery_adc_init();

    wdt_counter_reload();

    /* Initialize theme system */
    theme_init(THEME_DARK_BLUE);

    /* Initialize oscilloscope state */
    scope_state_init(scope_state_get());
    /* Probe 1X/10X scales every volts readout through scope_cal (one
     * application point); scope_state stays its one home. */
    scope_cal_set_probe_source(probe_factor_from_state);

    /* Initialize LCD — using proven hwtest approach */
    {
        /* GPIO config for EXMC pins (same as hwtest) */
        #define _GPIO_CFG(base, pin, mode, cnf) do { \
            volatile uint32_t *r = (pin < 8) ? \
                (volatile uint32_t *)(base + 0x00) : \
                (volatile uint32_t *)(base + 0x04); \
            uint8_t p = (pin < 8) ? pin : (pin - 8); \
            uint32_t v = *r; \
            v &= ~(0xFU << (p * 4)); \
            v |= (((mode) | ((cnf) << 2)) << (p * 4)); \
            *r = v; \
        } while(0)

        /* PD0,1,4,5,7,8,9,10,11,12,14,15 as AF push-pull */
        uint8_t pd_pins[] = {0,1,4,5,7,8,9,10,11,12,14,15};
        for (int i = 0; i < 12; i++)
            _GPIO_CFG(0x40011400, pd_pins[i], 3, 2);
        /* PE7-15 as AF push-pull */
        for (int i = 7; i <= 15; i++)
            _GPIO_CFG(0x40011800, i, 3, 2);

        /* EXMC config — proven working values */
        *(volatile uint32_t *)0xA0000000 = 0x00005010;
        *(volatile uint32_t *)0xA0000004 = 0x02020424;
        *(volatile uint32_t *)0xA0000104 = 0x00000202;
        *(volatile uint32_t *)0xA0000000 |= 0x0001;
        delay_ms(50);

        /* LCD init — proven working sequence from hwtest */
        lcd_write_cmd(0x01);  /* Software reset */
        delay_ms(200);
        lcd_write_cmd(0x11);  /* Sleep out */
        delay_ms(200);
        lcd_write_cmd(0x36);  /* MADCTL — landscape, flipped */
        delay_ms(1);
        lcd_write_data8(0xA0);
        delay_ms(10);
        lcd_write_cmd(0x3A);  /* Pixel format 16-bit */
        delay_ms(1);
        lcd_write_data8(0x55);
        delay_ms(10);
        lcd_write_cmd(0x29);  /* Display on */
        delay_ms(50);

        #undef _GPIO_CFG
    }

    wdt_counter_reload();

    /* First frame, then light the backlight. Painting before PB8 goes HIGH is
     * what eliminates the boot static (uninitialized GRAM was visible from
     * the moment the backlight rose until the display task's first draw).
     * Drawing the splash HERE — not in the display task — also means the
     * multi-second FPGA config sequence below runs behind the logo instead of
     * behind garbage. The display task takes over rendering from this point. */
#ifndef EMULATOR_BUILD
    /* Here, not right after the clock setup: writes to the EXMC before its
     * clock is back on are dropped (bench, unit #2). */
    lcd_bus_timing_spec();   /* the bootloader's LCD bus timing is 4x too fast for the ST7789V */
#endif
    draw_splash();
#ifndef EMULATOR_BUILD
    GPIOB->scr = (1 << 8);  /* PB8 = backlight ON, panel already showing splash */
#endif

#if CAL_DUMP_MODE
    /* `make guest-caldump` only. Reports MCU flash 0x08006000..0x08006FFF —
     * the saved-config sector that is stock's candidate home for per-device
     * calibration, and which nobody has ever read on any unit because RDP
     * blocks external debuggers (the CPU can read its own flash fine).
     *
     * Deliberately placed here: the LCD and backlight are up, and NOTHING
     * else has run — no FPGA init, no flash_fs, no settings store, no
     * scheduler. It never returns. A diagnostic image should do one thing,
     * and this one is read-only. See src/util/cal_dump.h. */
    cal_dump_run();
#endif

    /*
     * FFT engine is initialized ON DEMAND when user enters FFT view
     * (via BTN_PRM in input_handler.c). This keeps the 88KB shared
     * memory pool free for other features until actually needed.
     */

#ifndef EMULATOR_BUILD
    /* Initialize signal generator and DAC hardware */
    {
        extern void dac_output_init(void);
        dac_output_init();

        /* Stock factory values (sine, 1 kHz, 3.0 V, 50 %) but output OFF:
         * stock boots with it ON (0x080294A0). settings_store_init() below
         * then restores the saved generator settings; the output comes back
         * ON only with the opt-in boot policy (siggen boot on, parity G5). */
        siggen_init(NULL);
    }
#endif

    /* Initialize meter data parser */
    meter_data_init();
    meter_voltage_wave_init();
    /* The saved startup mode is not known yet (it is in the settings record,
     * restored below): boot on the factory default for now, nothing before
     * settings_store_init() looks at current_mode. */
    current_mode = startup_target_mode();

    /* Factory calibration boundary: initialize the W25Q wrapper, then
     * leave the calibration mirror unloaded. Stock evidence has not
     * recovered a host-readable DMM factory-calibration file or H2/SPI3
     * apply proof, so this path deliberately fails closed instead of
     * consuming invented filenames or low-voltage coefficients. */
    (void)flash_fs_init();
    (void)flash_fs_load_factory_cal();

#ifndef EMULATOR_BUILD
    /* Initialize FPGA communication (USART2 + SPI3 + boot sequence).
     * Must happen after clock init and GPIO clocks are enabled.
     * Sends boot commands, configures SPI3 Mode 3, performs handshake,
     * sets PB11 HIGH (FPGA active mode) and PC6 HIGH (SPI enable). */
    /* Initialize USB CDC debug shell (HICK 48MHz clock + CDC class).
     * Must happen after clock init. Enables USB pull-up so device
     * appears on the bus immediately — the debug task handles data
     * once the scheduler starts. */
    usb_debug_init();

    wdt_counter_reload();
    fpga_init();
    wdt_counter_reload();
#endif

    /* User settings: bind the W25Q region layer, load the newest saved record
     * (or defaults) and apply it to live UI state. Never fails in a way main()
     * has to handle — no storage, or a corrupt/foreign record, simply means
     * defaults. See settings_store.h.
     *
     * POSITION IS DELIBERATE, and it is the one thing to re-check if the cold
     * boot ever regresses. It must come AFTER:
     *   - flash_fs_init(), whose mutex and raw SPI2 primitives it uses;
     *   - theme_init() / scope_state_init(), whose defaults it overwrites;
     *   - fpga_init(), because this is the first code in this firmware's
     *     history to drive SPI2 (and PB12/CS) before the scheduler, and the
     *     FPGA config sequence right above it is bench-validated and fragile.
     *     Keeping it downstream leaves that sequence's electrical environment
     *     exactly as it was validated.
     * And BEFORE the fpga_set_meter_mode(meter_submode) call below, which
     * consumes the restored meter submode, and before the display task exists. */
    settings_store_init();
    scope_freq_lock_init();     /* shell + display share the estimator's FFT scratch */
    /* Startup-on-Boot, now restored (M6): the mode the display and FPGA tasks
     * and the mode entry below start in. */
    current_mode = startup_target_mode();

#if defined(CAL_BACKUP_AUTO_RESTORE) && CAL_BACKUP_AUTO_RESTORE
    /* OFF by default. Manual `cal restore` is the primary path. Even when built
     * in, this only acts when the live MCU cal page reads blank/zeroed AND a
     * valid W25Q backup exists — a programmed page is never overwritten
     * automatically (cal_backup_should_auto_restore). Placed after
     * settings_store_init() so the W25Q region layer is bound. */
    (void)cal_backup_maybe_auto_restore();
#endif

    /* settings_store_init() just restored scope_state.timebase_idx. Push it at
     * the FPGA so the axis label and the actual sample rate agree — without
     * this the restore silently undoes the reconcile inside fpga_init(), which
     * is exactly what the 2026-08-19 bench run showed (reconcile reported
     * "pulled", display still read 0x0A). Must stay AFTER the restore and
     * BEFORE fpga_create_tasks(), which is the point the acquisition task
     * could start using SPI3. */
#ifndef EMULATOR_BUILD
    fpga_reconcile_timebase_after_arm();
    /* Same reconcile for the vertical axis: fpga_init() applied the frontend
     * relays before the restore, so push the restored vdiv indices out too
     * (EXP-19, 2026-08-20). */
    fpga_reconcile_frontend_after_arm();
    /* And the trigger level: the arm burst's 0xAD sits above any mid-scale
     * signal, so PC0 never completes a capture (EXP-41, 2026-09-14). */
    fpga_reconcile_trigger_after_arm();
    /* This unit's stock calibration page (0x08006000, read-only): per-bank
     * offset-DAC tables and per-channel gain (parity C4, scope_vpos.h).
     * After the restores above, because bank and range come from the
     * restored timebase and vdiv; a unit without a usable page keeps the
     * fixed arm fpga_init() just made. */
    /* DAC channel 1 (PA4, CH1's offset reference): its boot init, right
     * before its first write in this image (dac.h). The warm images made it
     * in fpga_init() before the mid-scale arm; then this is a no-op. */
    scope_trigger_dac_init();
    scope_vpos_init();
    /* OpenScope's own saved corrections (usercal.h): baseline deltas on top
     * of the page just parsed, and the 250 MS/s B correction. After
     * scope_vpos_init() because corr_set needs the page's pairs; deltas
     * measured against a different page table are refused, not applied.
     * Read-only on flash — saving is `cal user save` only. */
    usercal_boot_load();
#endif

    /* Create queues */
    xDisplayQueue = xQueueCreate(20, sizeof(uint8_t));
    xInputQueue   = xQueueCreate(15, sizeof(button_id_t));

    /* Backlight (TMR10 on PB8) and buzzer (TMR11 on PB9), parity C15. After
     * settings_store_init() so the stored brightness is the first PWM value;
     * the pin has been held high as a GPIO since the splash, so nothing goes
     * dark. EMULATOR_BUILD compiles these to no-ops. */
    sys_pwm_init();
    continuity_buzzer_init();

    /* Initialize button matrix scan driver (TMR3 ISR at 500Hz).
     * This replaces the old passive GPIO reads that didn't work on hardware.
     * The driver handles all GPIO config for the 4x3 matrix + 3 passive pins. */
    button_scan_init(xInputQueue);

    /* Boot logo (stock: 2:/LOGO/LOGO2C53T.jpg; ours: logo_store.h). After the
     * backlight PWM and settings so the stored brightness applies, before the
     * tasks so the display task starts behind it (it claims the screen until
     * vInputTask releases it after the ~1 s hold). No logo stored: no-op. */
    (void)logo_ui_boot_show();

    /* Create tasks */
    xTaskCreate(vDisplayTask, "display", 768, NULL, 1, &xDisplayTaskHandle);
    xTaskCreate(vInputTask,   "key",     384, NULL, 4, &xInputTaskHandle);

    /* Register tasks with health monitor */
    health_slot_display = health_register("display", xDisplayTaskHandle);
    health_slot_input   = health_register("key",     xInputTaskHandle);

#ifndef EMULATOR_BUILD
    /* Create FPGA communication tasks (USART TX/RX + SPI3 acquisition).
     * These match the stock firmware's dvom_TX, dvom_RX, and fpga tasks. */
    fpga_create_tasks();

    /* Create USB debug shell task (CDC virtual serial port).
     * Priority 2 — above display (1) but below input (4) and FPGA tasks. */
    usb_debug_create_task();
    continuity_buzzer_create_task();   /* also sounds the boot beep (C15) */
    meter_autoselect_create_task();

    if (current_mode == MODE_MULTIMETER) {
#if FPGA_WARM_HANDOFF_TEST
        /* A warm-handoff image reaches here only with FPGA_METER_SUBMODES
         * (startup_policy.h). Its USART2 is parked (ctrl1 = 0, IRQ masked,
         * FPGA_USART_SILENT_SCOPE) and fpga_meter_reset_transport() re-arms it
         * only with the scheduler running, so a transition from here would
         * busy-wait ~3 s for a SoC frame and 5 x 300 ms for a selector echo
         * that cannot arrive, and the SoC would boot into its own Auto while
         * meter_submode says otherwise. Hand it to the poll task instead: it
         * runs the same transition `mode meter N` runs, once the scheduler
         * is up. */
        fpga_request_meter_mode(meter_submode);
#else
        fpga_set_meter_mode(meter_submode);
#endif
    } else if (current_mode == MODE_SIGNAL_GEN) {
        /* Startup on Boot = Signal Generator. GPIO only (fpga.c), no USART,
         * so it is safe before the scheduler, like the scope entry below. */
        fpga_enter_siggen_mode();
    } else {
        /* Scope, and home (stock "None"): home keeps the scope posture, as
         * the settings page does. */
        fpga_enter_scope_mode();
    }
#endif

    /* Create 1-second timer for uptime/status updates */
    TimerHandle_t xSecTimer = xTimerCreate(
        "1sec", pdMS_TO_TICKS(1000), pdTRUE, NULL, vOneSecondTimerCallback);
    if (xSecTimer != NULL) {
        xTimerStart(xSecTimer, 0);
    }

    /* Create 500ms health check timer — monitors tasks + feeds watchdog */
    TimerHandle_t xHealthTimer = xTimerCreate(
        "health", pdMS_TO_TICKS(500), pdTRUE, NULL, vHealthCheckCallback);
    if (xHealthTimer != NULL) {
        xTimerStart(xHealthTimer, 0);
    }

    /* Boot validation: tell the bootloader we started successfully.
     * Clears the boot attempt counter so we won't enter safe mode on
     * next reset. Must be called after LCD init and task creation —
     * if we got here, the firmware is healthy. */
    boot_validate();

    /* Initialize watchdog LAST — after all tasks and timers are running.
     * Once enabled, the FWDGT cannot be stopped (hardware limitation).
     * Skipped in GUEST_BUILD so interactive SPI3 shell sessions on unit #2
     * can't trip a watchdog reset mid-experiment, and with NO_WATCHDOG for the
     * same reason in app-layout bench builds (app-coldtrace). */
#if !defined(GUEST_BUILD) && !defined(NO_WATCHDOG)
    watchdog_init();
#endif

    vTaskStartScheduler();

    for (;;) {}
}

/* ═══════════════════════════════════════════════════════════════════
 * FreeRTOS Hooks
 * ═══════════════════════════════════════════════════════════════════ */

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    taskDISABLE_INTERRUPTS();
    fault_display("STACK OVERFLOW", pcTaskName);
    /* Watchdog will reset us */
    for (;;) {}
}

void vApplicationMallocFailedHook(void)
{
    taskDISABLE_INTERRUPTS();
    fault_display("HEAP EXHAUSTED", "pvPortMalloc returned NULL");
    /* Watchdog will reset us */
    for (;;) {}
}

/* ═══════════════════════════════════════════════════════════════════
 * SystemInit override for emulator
 * ═══════════════════════════════════════════════════════════════════ */
#ifdef EMULATOR_BUILD
/* Must match AT32 HAL declaration: "extern unsigned int system_core_clock".
 * The HAL's system_at32f403a_407.c also defines these symbols, but the
 * emu build uses --allow-multiple-definition so main.o's versions win
 * (linked first). */
unsigned int system_core_clock = 240000000;

void SystemInit(void)
{
    /* Stub for the emulator, except for the one thing every build needs: the
     * FPU. The HAL's SystemInit grants CP10/CP11 full access (CPACR bits
     * 20..23); without it the first VFP instruction takes a NOCP usage fault
     * (CFSR 0x00080000), which is how Renode stopped right after the splash. */
    *(volatile uint32_t *)0xE000ED88u |= (0xFu << 20);
    __asm volatile ("dsb\n\tisb" ::: "memory");
}

void system_clock_config(void)
{
    /* Stub — do nothing in emulator mode. */
}
#endif
