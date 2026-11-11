/*
 * settings_store.c — capture / apply live settings, and decide when to write.
 *
 * See settings_store.h for the write policy and for the known gap.
 *
 * This is the first automated writer of the external W25Q in this firmware's
 * history. It reaches flash only through config.c, which reaches it only
 * through the region layer's append log, which refuses anything outside
 * FLASH_REGION_SETTINGS. There is deliberately no flash_fs_raw_* call and no
 * flash_regions_*_abs() call anywhere in this path.
 */

#include "settings_store.h"

#include "flash_regions.h"
#include "theme.h"
#include "scope_state.h"
#include "math_channel.h"
#include "ui.h"
#include "signal_gen.h"
#include "startup_policy.h"
#include "sys_prefs.h"

#include <string.h>

/* Trigger level is a pixel offset from the centre of a 206 px plot area, so
 * anything beyond ±103 is off-screen. Clamped rather than rejected: an
 * out-of-range level is a nuisance, not a reason to drop the whole config. */
#define TRIGGER_LEVEL_LIMIT  103

static device_config_t         g_live;    /* current settings image           */
static device_config_t         g_saved;   /* last image known to be in flash  */
static config_autosave_t       g_auto;
static settings_store_status_t g_status;

/* ═══════════════════════════════════════════════════════════════════
 * Helpers
 * ═══════════════════════════════════════════════════════════════════ */

static uint8_t clamp_idx(uint8_t value, uint8_t count, uint8_t fallback)
{
    return (value < count) ? value : fallback;
}

static int16_t clamp_level(int16_t v)
{
    if (v >  TRIGGER_LEVEL_LIMIT) return  TRIGGER_LEVEL_LIMIT;
    if (v < -TRIGGER_LEVEL_LIMIT) return -TRIGGER_LEVEL_LIMIT;
    return v;
}

static void stamp(device_config_t *cfg)
{
    cfg->magic    = CONFIG_MAGIC;
    cfg->version  = CONFIG_VERSION;
    cfg->checksum = config_compute_checksum(cfg);
}

/* ═══════════════════════════════════════════════════════════════════
 * Capture / apply
 *
 * The two functions must stay symmetric: a field captured but not applied is a
 * setting that silently does not restore, which is the exact failure this
 * whole task was fixing. Startup-on-Boot (startup_mode) is owned here since M6,
 * and brightness, sound volume and auto power-off since C15 (sys_prefs.c). Fields NOT listed
 * here (fft_*, language) are carried through untouched — they are
 * stored and round-tripped but nothing in the firmware owns them yet, and
 * config.h says so.
 * ═══════════════════════════════════════════════════════════════════ */

void settings_store_capture(device_config_t *cfg)
{
    if (!cfg) return;

    const scope_state_t *ss = scope_state_get();

    /* Channel enables are session state, not settings (parity C2): a CH2-off
     * saved here would come back after a power cycle as mask 1 (the 250 MS/s
     * single-channel mode, half the screen blank) with nothing on screen saying
     * why. The long CH1/CH2 key (KA_CH_ENABLE_TOGGLE) is the way to turn a
     * channel off and back on; a boot always starts with both on. The fields
     * stay in the record for format compatibility and are always written true,
     * so toggling a channel does not even dirty the settings image. */
    cfg->scope_ch1_enabled   = true;
    cfg->scope_ch1_vdiv      = ss->ch1.vdiv_idx;
    cfg->scope_ch1_coupling  = (uint8_t)ss->ch1.coupling;
    cfg->scope_ch1_probe     = (uint8_t)ss->ch1.probe;
    cfg->scope_ch1_bw_limit  = ss->ch1.bw_limit ? 1u : 0u;

    cfg->scope_ch2_enabled   = true;
    cfg->scope_ch2_vdiv      = ss->ch2.vdiv_idx;
    cfg->scope_ch2_coupling  = (uint8_t)ss->ch2.coupling;
    cfg->scope_ch2_probe     = (uint8_t)ss->ch2.probe;
    cfg->scope_ch2_bw_limit  = ss->ch2.bw_limit ? 1u : 0u;

    cfg->scope_timebase        = ss->timebase_idx;
    cfg->scope_trigger_mode    = (uint8_t)ss->trigger.mode;
    cfg->scope_trigger_edge    = (uint8_t)ss->trigger.edge;
    cfg->scope_trigger_source  = (uint8_t)ss->trigger.source;
    cfg->scope_trigger_level   = ss->trigger.level;

    cfg->theme        = (uint8_t)theme_get_id();
    cfg->math_enabled = math_enabled ? 1u : 0u;
    cfg->math_op      = math_op;

    cfg->meter_submode = meter_submode;
    cfg->meter_layout  = meter_layout;

    const siggen_config_t *sg = siggen_get_config();
    cfg->siggen_waveform  = sg->waveform;
    cfg->siggen_frequency = (float)sg->frequency_hz;   /* exact: <= 50000 */
    cfg->siggen_amplitude = (float)sg->amplitude_dv / 10.0f;
    cfg->siggen_duty      = sg->duty_pct;
    /* The enable is only a saved setting while the user wants it restored;
     * with the boot policy OFF it is session state, and recording it would
     * make every run/stop cycle look like a change to write. */
    cfg->siggen_enabled   = (sg->start_on && sg->output_enabled) ? 1u : 0u;
    cfg->siggen_start_on  = sg->start_on ? 1u : 0u;
    cfg->startup_mode = startup_mode_to_config(startup_mode);
    cfg->display_brightness  = sys_prefs_brightness();
    cfg->sound_enabled       = sys_prefs_volume();      /* 0..10 since C15 (was 0/1) */
    cfg->auto_shutdown_mins  = sys_prefs_autooff_mins();
    cfg->sys_prefs_mark      = CONFIG_SYS_PREFS_MARK;

    stamp(cfg);
}

void settings_store_apply(const device_config_t *cfg)
{
    if (!cfg) return;

    scope_state_t *ss = scope_state_get();

    ss->ch1.enabled  = true;   /* not restored: see settings_store_capture */
    ss->ch1.vdiv_idx = clamp_idx(cfg->scope_ch1_vdiv, VDIV_COUNT, 3);
    ss->ch1.coupling = (coupling_t)clamp_idx(cfg->scope_ch1_coupling, COUPLING_COUNT, COUPLING_DC);
    ss->ch1.probe    = (probe_t)clamp_idx(cfg->scope_ch1_probe, PROBE_COUNT, PROBE_1X);
    ss->ch1.bw_limit = (cfg->scope_ch1_bw_limit != 0u);

    ss->ch2.enabled  = true;
    ss->ch2.vdiv_idx = clamp_idx(cfg->scope_ch2_vdiv, VDIV_COUNT, 5);
    ss->ch2.coupling = (coupling_t)clamp_idx(cfg->scope_ch2_coupling, COUPLING_COUNT, COUPLING_DC);
    ss->ch2.probe    = (probe_t)clamp_idx(cfg->scope_ch2_probe, PROBE_COUNT, PROBE_1X);
    ss->ch2.bw_limit = (cfg->scope_ch2_bw_limit != 0u);

    ss->timebase_idx    = clamp_idx(cfg->scope_timebase, TIMEBASE_COUNT, 10);
    ss->trigger.mode    = (trigger_mode_t)clamp_idx(cfg->scope_trigger_mode, TRIG_COUNT, TRIG_AUTO);
    ss->trigger.edge    = (trigger_edge_t)clamp_idx(cfg->scope_trigger_edge, TRIG_EDGE_COUNT,
                                                    TRIG_RISING);
    ss->trigger.source  = (trigger_source_t)clamp_idx(cfg->scope_trigger_source, TRIG_SRC_COUNT,
                                                      TRIG_SRC_CH1);
    ss->trigger.level   = clamp_level(cfg->scope_trigger_level);

    theme_set((theme_id_t)clamp_idx(cfg->theme, THEME_COUNT, THEME_DARK_BLUE));

    math_enabled = (cfg->math_enabled != 0u);
    math_op      = clamp_idx(cfg->math_op, MATH_COUNT, 0);

    meter_submode = clamp_idx(cfg->meter_submode, METER_SUBMODE_COUNT, 0);
    meter_layout  = clamp_idx(cfg->meter_layout, METER_LAYOUT_COUNT, METER_LAYOUT_FULL);

    /* Signal generator (parity G5). Clamped, NaN-safe: a float field that is
     * not a number or not in range falls back to stock's factory value
     * rather than driving the output with garbage. The output comes back ON
     * only when the user opted in (siggen_start_on) AND it was on at the last
     * save; otherwise it boots OFF, which stock does not do. */
    {
        siggen_config_t sg;
        float f = cfg->siggen_frequency;
        float a = cfg->siggen_amplitude * 10.0f + 0.5f;
        sg.waveform     = clamp_idx(cfg->siggen_waveform, SIGGEN_WAVEFORM_COUNT, SIGGEN_DEFAULT_WAVE);
        sg.frequency_hz = (f >= 0.0f && f <= (float)SIGGEN_FREQ_MAX_HZ)
                          ? (uint32_t)(f + 0.5f) : SIGGEN_DEFAULT_FREQ;
        sg.amplitude_dv = (a >= 0.0f && a <= (float)SIGGEN_AMP_MAX_DV + 1.0f)
                          ? (uint8_t)a : SIGGEN_DEFAULT_AMP_DV;
        if (sg.amplitude_dv > SIGGEN_AMP_MAX_DV) sg.amplitude_dv = SIGGEN_AMP_MAX_DV;
        sg.duty_pct     = (cfg->siggen_duty <= SIGGEN_DUTY_MAX) ? cfg->siggen_duty
                                                               : SIGGEN_DEFAULT_DUTY;
        sg.start_on       = (cfg->siggen_start_on != 0u);
        sg.output_enabled = sg.start_on && (cfg->siggen_enabled != 0u);
        siggen_init(&sg);
    }
    startup_mode  = startup_mode_from_config(cfg->startup_mode);
    /* Brightness, volume and auto-off are clamped by the setters (a blank or
     * foreign byte lands on stock's factory value / off). The timers pick the
     * values up in sys_pwm_init() / the input task; this file stays hardware-free. */
    if (cfg->sys_prefs_mark == CONFIG_SYS_PREFS_MARK) {
        sys_prefs_set_brightness(cfg->display_brightness);
        sys_prefs_set_volume(cfg->sound_enabled);
        sys_prefs_set_autooff_mins(cfg->auto_shutdown_mins);
    } else {
        /* A pre-C15 record: its 80 % / 30 min were defaults, not choices
         * (config.h). Stock's factory values instead: 100 %, volume 1, off. */
        sys_prefs_set_brightness(SYS_BRIGHT_DEFAULT);
        sys_prefs_set_volume(SYS_VOL_DEFAULT);
        sys_prefs_set_autooff_mins(0);
    }

    /* Deliberately NOT restored: ss->running (a scope always boots running),
     * the channel enables (an old config with CH2 off boots with both on),
     * the cursor state, and channel position — session state, not settings.
     * persist_enabled is not restored either: turning persistence on allocates
     * the shared memory pool and evicts FFT, which is not something a boot
     * should do behind the user's back. */
}

/* ═══════════════════════════════════════════════════════════════════
 * Dirty tracking
 * ═══════════════════════════════════════════════════════════════════ */

typedef enum {
    CAPTURE_MATCHES_FLASH = 0,  /* nothing to write                         */
    CAPTURE_STILL_PENDING,      /* differs from flash, but nothing new       */
    CAPTURE_NEW_CHANGE,         /* differs from the previous capture too     */
} capture_result_t;

/* Capture into g_live and classify it against BOTH the previous capture and
 * the last saved image.
 *
 * The distinction is what makes the settle window terminate. If every press
 * restarted the window, a pending change would never be written while the user
 * kept pressing unrelated buttons. Only a NEW change restarts it.
 *
 * The comparison is over the whole record image, checksum included, which is
 * exactly the byte string the append log compares for its no-op elision — so
 * "unchanged here" and "elided there" can never disagree. */
static capture_result_t capture(void)
{
    device_config_t next = g_live;      /* keep the fields we do not own */
    settings_store_capture(&next);

    bool differs_from_last_look = (memcmp(&next, &g_live, sizeof next) != 0);
    bool differs_from_flash     = (memcmp(&next, &g_saved, sizeof next) != 0);
    g_live = next;

    if (!differs_from_flash)      return CAPTURE_MATCHES_FLASH;
    if (differs_from_last_look)   return CAPTURE_NEW_CHANGE;
    return CAPTURE_STILL_PENDING;
}

/* Set when a write failed, cleared when something new changes. Without it, a
 * device whose settings region cannot be written (region not blank, flash
 * failing, table wrong) would re-attempt the save on EVERY button press, and
 * each attempt costs a full scan of the log in short SPI2 reads. One refusal
 * per change is a diagnostic; one refusal per keypress is a fault of its own. */
static bool g_write_blocked;

static bool write_now(void)
{
    if (config_save(&g_live)) {
        g_write_blocked = false;
        /* g_live was stamped by settings_store_capture() with the same magic /
         * version / checksum config_save() stamps on its own copy, so g_live
         * is byte-identical to the record that just landed in flash. */
        g_saved = g_live;
        config_autosave_done(&g_auto);
        g_status.writes++;
        return true;
    }
    g_write_blocked = true;
    g_status.write_failures++;
    return false;
}

void settings_store_note_change(uint32_t now_ms)
{
    switch (capture()) {
    case CAPTURE_MATCHES_FLASH:
        /* Nothing to write — including the "changed and changed back" case,
         * where clearing here is what stops a cancelled edit costing a
         * record. */
        config_autosave_done(&g_auto);
        break;

    case CAPTURE_NEW_CHANGE:
        g_status.changes_seen++;
        g_write_blocked = false;        /* new input is worth another attempt */
        config_autosave_mark(&g_auto, now_ms);
        break;

    case CAPTURE_STILL_PENDING:
        /* Leave the running window alone. Re-arm only if something cleared
         * the flag without writing, so a pending change can never be
         * stranded. */
        if (!g_auto.pending) {
            config_autosave_mark(&g_auto, now_ms);
        }
        break;
    }
}

bool settings_store_service(uint32_t now_ms)
{
    if (g_write_blocked || !config_autosave_due(&g_auto, now_ms)) {
        return false;
    }
    return write_now();
}

settings_flush_outcome_t settings_store_flush_ex(uint32_t now_ms)
{
    settings_store_note_change(now_ms);
    if (g_write_blocked) {
        return SETTINGS_FLUSH_BLOCKED;
    }
    if (!g_auto.pending) {
        return SETTINGS_FLUSH_CLEAN;
    }
    return write_now() ? SETTINGS_FLUSH_SAVED : SETTINGS_FLUSH_FAILED;
}

bool settings_store_flush(uint32_t now_ms)
{
    return settings_store_flush_ex(now_ms) == SETTINGS_FLUSH_SAVED;
}

const char *settings_flush_outcome_text(settings_flush_outcome_t o)
{
    switch (o) {
    case SETTINGS_FLUSH_SAVED:   return "saved";
    case SETTINGS_FLUSH_CLEAN:   return "already saved";
    case SETTINGS_FLUSH_BLOCKED: return "NOT saved (settings storage blocked after a failed write)";
    case SETTINGS_FLUSH_FAILED:  return "NOT saved (write failed)";
    default:                     return "NOT saved (unknown)";
    }
}

const settings_store_status_t *settings_store_get_status(void) { return &g_status; }

/* ═══════════════════════════════════════════════════════════════════
 * Boot
 * ═══════════════════════════════════════════════════════════════════ */

config_load_result_t settings_store_load_and_apply(void)
{
    config_autosave_init(&g_auto, SETTINGS_STORE_SETTLE_MS);

    g_write_blocked = false;

    config_load_result_t r = config_load_or_defaults(&g_live);
    g_status.load_result = r;

    settings_store_apply(&g_live);

    /* Baseline for change detection. On a defaulted boot this is the DEFAULT
     * image, not a flash image — so the first real change writes a record and
     * an untouched device never writes at all. Both config_load_or_defaults()
     * outcomes leave a stamped, checksum-consistent struct. */
    g_saved = g_live;

    return r;
}

void settings_store_init(void)
{
#ifdef EMULATOR_BUILD
    /* Renode does not model the W25Q on SPI2. Binding here would make every
     * record scan spin the bounded SPI timeout loops in flash_fs.c — slow, and
     * it would "persist" into a peripheral that is not there. The emulator
     * therefore runs on defaults, with storage_bound false, and says so. */
    g_status.storage_bound = false;
#else
    g_status.storage_bound = (flash_regions_bind_w25q() == FLASH_REGION_OK);
#endif
    (void)settings_store_load_and_apply();
}
