/*
 * Host test for the v5 settings record generated from the property table
 * (src/model/model_persist_v5.c, ARCH-1 M3).
 *
 * THE PROOF. Below, under "frozen reference", is a verbatim copy of the
 * hand-written settings_store_capture / settings_store_apply and
 * config_init_defaults this table replaced (only renamed: ref_capture,
 * ref_apply, ref_defaults). It lives here, never in the firmware, and is not
 * to be edited: it is what "today's behaviour" means. The generated code
 * must give
 *   - byte-identical defaults, and identical state after loading them;
 *   - byte-identical records for random live states (every value a load can
 *     produce, coupling GND and trigger level +-103 included);
 *   - identical state, field by field and the whole scope_state_t, after
 *     loading random records, hostile ones included (any byte, NaN, inf,
 *     boundary floats, a missing sys_prefs_mark, each index at count - 1,
 *     count and 255), and identical records when those are captured back.
 * Record acceptance (magic, version, checksum, size, the v4 upgrade) is
 * config.c's and untouched by M3, so every record today's code accepts is
 * accepted, and the apply comparison covers what it then does with it.
 *
 * Also: the per-field out-of-range policy, field by field; the stored-only
 * bytes (sys.lang, display_persist_mode) carried through a load/save cycle
 * and saved once set; the fft.* bytes, live settings, through their codec;
 * table coverage (every PF_PERSIST
 * property has one row, every row a persisted property or a const one, every
 * record byte is one row's: no padding is left); the session-only list;
 * settings notes by origin (key, shell with persist on/off, boot, load); and
 * the display view (view.mode, the former padding byte 23): kept by a load,
 * put in force by the boot apply, kept in the record when that is refused.
 *
 * Build/run:  make -C firmware test-model-persist
 */
#define MODEL_INTERNAL
#include "model.h"
#include "model_persist_v5.h"
#include "config.h"
#include "settings_store.h"
#include "scope_state.h"
#include "meas_select.h"
#include "signal_gen.h"
#include "sys_prefs.h"
#include "theme.h"
#include "math_channel.h"
#include "startup_policy.h"
#include "ui.h"
#include "meter_link_policy.h"
#include "view_table.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECKF(c, ...) do { checks++; if (!(c)) { fails++; \
    printf("FAIL %s:%d  %s: ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ═══ the globals main.c / input_handler.c own in the firmware ═══════ */
volatile device_mode_t  current_mode  = MODE_MULTIMETER;
volatile startup_mode_t startup_mode  = STARTUP_METER;
volatile uint8_t        active_channel = 0;
volatile uint8_t        meter_submode = 0;
volatile uint8_t        meter_layout  = 0;
volatile bool           math_enabled  = false;
volatile uint8_t        math_op       = 0;
volatile bool           persist_enabled = false;
volatile scope_view_t   scope_view    = SCOPE_VIEW_TIME;
volatile uint8_t        fuse_type     = 0;
volatile uint8_t        fuse_rating_idx = 6;
volatile uint8_t        fuse_view     = 0;
volatile float          fuse_scan_threshold_mv = 0.1f;
uint8_t key_focus_get(void) { return 0; }

/* signal_gen.c's output driver: what the GEN lead would be doing */
static bool     dac_running;
static uint32_t dac_apply_calls;
void dac_output_apply(const siggen_timing_t *t) { (void)t; dac_running = true; dac_apply_calls++; }
void dac_output_stop(void) { dac_running = false; }
static uint16_t g_dac_buf[SIGGEN_CYCLE_SAMPLES];
uint16_t *dac_output_get_buffer(void) { return g_dac_buf; }
uint32_t dac_output_timer_clock_hz(void) { return 240000000u; }

/* No W25Q here: the store runs unbound (settings_store_load_and_apply then
 * comes up on defaults). The flash path is test_config_persist's. */
#include "flash_regions.h"
flash_region_status_t flash_regions_bind_w25q(void) { return FLASH_REGION_ERR_NOT_INIT; }

/* ═══════════════════════════════════════════════════════════════════
 * Frozen reference: settings_store.c and config.c before M3, verbatim
 * (renamed only). DO NOT EDIT.
 *
 * One extension since (meter.link): the padding byte at offset 11 became the
 * field meter_link. The reference gained exactly three lines, each marked
 * "meter.link": ref_capture writes the byte from the property's backing
 * state, ref_apply clamps it (anything above 1 -> 0, like the other
 * indices), ref_defaults sets 0. Nothing else in the reference changed, and
 * every other byte of every record is compared as before. A record written
 * before the field existed holds 0 there (the zero-init invariant), which
 * test_meter_link_record below checks against the reference defaults.
 *
 * A second one (Startup on Boot = Restore, the last screen): the first of
 * the two padding bytes at offset 22 became the field last_screen. Three
 * more lines, each marked "last screen": ref_capture writes the screen the
 * model holds for the record (the loaded one, or the one the last orderly
 * power-off named: scope 1, meter 2, generator 3, anything else 0; never
 * the screen shown now, current_mode),
 * ref_apply clamps it (above 3 -> 0) into the model's loaded-screen state,
 * ref_defaults sets 0. And ONE line of ref_defaults changed its value,
 * marked "sys.startup default": the built-in default of sys.startup went
 * from the meter to Restore (an owner decision, 2026-11-14), so the default
 * record's startup_mode byte is CONFIG_MODE_RESTORE where it was
 * CONFIG_MODE_MULTIMETER. How stored values decode did not move.
 *
 * A third (the display view, view.mode): the last padding byte, offset 23,
 * became the field view_mode. Three more lines, each marked "view.mode":
 * ref_capture writes the view the model holds for the record (the loaded
 * one until the boot apply put it in force or the user changed the view,
 * then the live view.mode), ref_apply clamps it (above 5 -> 0) into the
 * model's kept-view state and moves no live state (scope_view and the roll
 * flag are the owners', set later through the setter), ref_defaults sets 0.
 *
 * A fourth (the FFT settings live): fft.window, fft.avg and fft.maxhold
 * are settings the spectrum views apply, no longer bytes the store only
 * carries. The lines marked "fft.*": ref_capture writes the three from
 * the properties' backing state (the window as is, the averaging as its
 * count 0/2/4/8/16/32, max hold as 0/1), ref_apply puts them there (a window
 * above 4 -> Hanning, a byte that is not a count -> off, max hold = any
 * non-zero byte). ref_defaults already wrote their values (Hanning, off, off).
 * ═══════════════════════════════════════════════════════════════════ */
#define REF_FFT_WINDOW_COUNT    5u      /* fft.*: rect hanning hamming blackman flattop */
#define REF_FFT_WINDOW_DEFAULT  1u      /* fft.*: Hanning, ref_defaults' */
static const uint8_t ref_fft_counts[] = { 0u, 2u, 4u, 8u, 16u, 32u };   /* fft.*: off x2 .. x32 */
static uint8_t ref_fft_avg_index(uint8_t count)                          /* fft.* */
{
    for (uint8_t i = 0; i < sizeof ref_fft_counts; i++)
        if (ref_fft_counts[i] == count) return i;
    return 0u;
}
#define REF_TRIGGER_LEVEL_LIMIT  103

static uint8_t ref_clamp_idx(uint8_t value, uint8_t count, uint8_t fallback)
{
    return (value < count) ? value : fallback;
}

static int16_t ref_clamp_level(int16_t v)
{
    if (v >  REF_TRIGGER_LEVEL_LIMIT) return  REF_TRIGGER_LEVEL_LIMIT;
    if (v < -REF_TRIGGER_LEVEL_LIMIT) return -REF_TRIGGER_LEVEL_LIMIT;
    return v;
}

static void ref_stamp(device_config_t *cfg)
{
    cfg->magic    = CONFIG_MAGIC;
    cfg->version  = CONFIG_VERSION;
    cfg->checksum = config_compute_checksum(cfg);
}

static void ref_capture(device_config_t *cfg)
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
    cfg->meter_link    = (uint8_t)meter_link_get(NULL);                      /* meter.link */
    cfg->last_screen   = model_last_screen_record();                        /* last screen */
    cfg->view_mode     = model_view_record();                               /* view.mode */
    cfg->fft_window    = (uint8_t)fft_window_get(NULL);                     /* fft.* */
    cfg->fft_avg_count = ref_fft_counts[fft_avg_get(NULL)];                 /* fft.* */
    cfg->fft_max_hold  = fft_maxhold_get(NULL) != 0;                        /* fft.* */

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
    cfg->meas_ch1_mask       = meas_select_get(0);   /* v5: Parameter Measurement */
    cfg->meas_ch2_mask       = meas_select_get(1);

    ref_stamp(cfg);
}

static void ref_apply(const device_config_t *cfg)
{
    if (!cfg) return;

    scope_state_t *ss = scope_state_get();

    ss->ch1.enabled  = true;   /* not restored: see settings_store_capture */
    ss->ch1.vdiv_idx = ref_clamp_idx(cfg->scope_ch1_vdiv, VDIV_COUNT, 3);
    ss->ch1.coupling = (coupling_t)ref_clamp_idx(cfg->scope_ch1_coupling, COUPLING_COUNT, COUPLING_DC);
    ss->ch1.probe    = (probe_t)ref_clamp_idx(cfg->scope_ch1_probe, PROBE_COUNT, PROBE_1X);
    ss->ch1.bw_limit = (cfg->scope_ch1_bw_limit != 0u);

    ss->ch2.enabled  = true;
    ss->ch2.vdiv_idx = ref_clamp_idx(cfg->scope_ch2_vdiv, VDIV_COUNT, 5);
    ss->ch2.coupling = (coupling_t)ref_clamp_idx(cfg->scope_ch2_coupling, COUPLING_COUNT, COUPLING_DC);
    ss->ch2.probe    = (probe_t)ref_clamp_idx(cfg->scope_ch2_probe, PROBE_COUNT, PROBE_1X);
    ss->ch2.bw_limit = (cfg->scope_ch2_bw_limit != 0u);

    ss->timebase_idx    = ref_clamp_idx(cfg->scope_timebase, TIMEBASE_COUNT, 10);
    ss->trigger.mode    = (trigger_mode_t)ref_clamp_idx(cfg->scope_trigger_mode, TRIG_COUNT, TRIG_AUTO);
    ss->trigger.edge    = (trigger_edge_t)ref_clamp_idx(cfg->scope_trigger_edge, TRIG_EDGE_COUNT,
                                                    TRIG_RISING);
    ss->trigger.source  = (trigger_source_t)ref_clamp_idx(cfg->scope_trigger_source, TRIG_SRC_COUNT,
                                                      TRIG_SRC_CH1);
    ss->trigger.level   = ref_clamp_level(cfg->scope_trigger_level);

    theme_set((theme_id_t)ref_clamp_idx(cfg->theme, THEME_COUNT, THEME_DARK_BLUE));

    math_enabled = (cfg->math_enabled != 0u);
    math_op      = ref_clamp_idx(cfg->math_op, MATH_COUNT, 0);

    meter_submode = ref_clamp_idx(cfg->meter_submode, METER_SUBMODE_COUNT, 0);
    meter_layout  = ref_clamp_idx(cfg->meter_layout, METER_LAYOUT_COUNT, METER_LAYOUT_FULL);
    meter_link_store(NULL, ref_clamp_idx(cfg->meter_link, METER_LINK_POLICY_COUNT,
                                         METER_LINK_SCREEN));                /* meter.link */
    model_last_screen_load(cfg->last_screen <= STARTUP_SCREEN_MAX ? cfg->last_screen : 0u);   /* last screen */
    model_view_boot_load(cfg->view_mode <= 5u ? cfg->view_mode : 0u);       /* view.mode */
    fft_window_store(NULL, ref_clamp_idx(cfg->fft_window, REF_FFT_WINDOW_COUNT, REF_FFT_WINDOW_DEFAULT));   /* fft.* */
    fft_avg_store(NULL, ref_fft_avg_index(cfg->fft_avg_count));            /* fft.* */
    fft_maxhold_store(NULL, ((const uint8_t *)cfg)[offsetof(device_config_t, fft_max_hold)] != 0u);   /* fft.* */

    /* v5: the measurement selection (meas_select_set drops bits above 12). */
    meas_select_set(0, cfg->meas_ch1_mask);
    meas_select_set(1, cfg->meas_ch2_mask);

    /* Signal generator (parity G5). Clamped, NaN-safe: a float field that is
     * not a number or not in range falls back to stock's factory value
     * rather than driving the output with garbage. The output comes back ON
     * only when the user opted in (siggen_start_on) AND it was on at the last
     * save; otherwise it boots OFF, which stock does not do. */
    {
        siggen_config_t sg;
        float f = cfg->siggen_frequency;
        float a = cfg->siggen_amplitude * 10.0f + 0.5f;
        sg.waveform     = ref_clamp_idx(cfg->siggen_waveform, SIGGEN_WAVEFORM_COUNT, SIGGEN_DEFAULT_WAVE);
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

static void ref_defaults(device_config_t *cfg)
{
    if (!cfg) return;

    /* Memset first: this is what makes the struct's padding deterministic,
     * which is what makes the append log's no-op elision work. */
    memset(cfg, 0, sizeof(*cfg));

    cfg->magic   = CONFIG_MAGIC;
    cfg->version = CONFIG_VERSION;

    /* Oscilloscope defaults */
    cfg->scope_ch1_vdiv     = 3;    /* 2V/div */
    cfg->scope_ch2_vdiv     = 5;    /* 200mV/div */
    cfg->scope_timebase     = 10;   /* 50us/div */
    cfg->scope_trigger_mode = 0;    /* Auto */
    cfg->scope_trigger_edge = 0;    /* Rising */
    cfg->scope_trigger_level = 0;
    cfg->scope_ch1_coupling = 0;    /* DC */
    cfg->scope_ch2_coupling = 0;    /* DC */
    cfg->scope_ch1_enabled  = true;
    cfg->scope_ch2_enabled  = true;

    /* FFT defaults */
    cfg->fft_window    = 1;         /* Hanning */
    cfg->fft_avg_count = 0;
    cfg->fft_max_hold  = false;

    /* Signal generator defaults */
    cfg->siggen_waveform  = 0;      /* Sine */
    cfg->siggen_frequency = 1000.0f;
    cfg->siggen_amplitude = 3.0f;   /* stock factory 3.0 V (0x080294A0) */
    /* Stock factory duty; the output boots OFF unless the user opted in
     * (stock boots ON, 0x080294A0). */
    cfg->siggen_duty      = 50;
    cfg->siggen_enabled   = 0;
    cfg->siggen_start_on  = 0;

    /* Display defaults */
    cfg->display_brightness  = 100;   /* stock factory (C15) */
    cfg->display_persist_mode = 0;
    cfg->language = 0;              /* English */
    cfg->startup_mode = CONFIG_MODE_RESTORE;        /* sys.startup default (was CONFIG_MODE_MULTIMETER) */

    /* System defaults */
    cfg->auto_shutdown_mins = 0;      /* stock factory: off (C15) */
    cfg->sound_enabled      = 1;
    cfg->sys_prefs_mark     = CONFIG_SYS_PREFS_MARK;   /* these are C15's values */

    /* v3 block */
    cfg->theme                = 0;  /* THEME_DARK_BLUE */
    cfg->scope_ch1_probe      = 0;  /* 1x */
    cfg->scope_ch2_probe      = 0;
    cfg->scope_ch1_bw_limit   = 0;
    cfg->scope_ch2_bw_limit   = 0;
    cfg->scope_trigger_source = 0;  /* CH1 */
    cfg->math_enabled         = 0;
    cfg->math_op              = 0;
    cfg->meter_submode        = 0;  /* DCV */
    cfg->meter_layout         = 0;  /* Full */
    cfg->meter_link           = 0;  /* meter.link: with the screen */
    cfg->last_screen          = 0;  /* last screen: none */
    cfg->view_mode            = 0;  /* view.mode: yt */

    /* v5 block: the same five on both channels, CH1's badges before the
     * selection page existed (meas_select.h MEAS_SELECT_CH1/CH2_DEFAULT;
     * scope_measure.h ids: Freq 0, Peri 1, Duty+ 2, Vp-p 8, Vrms 10). Literal
     * here so config.c stays free of UI headers; test_config_persist checks
     * they agree. A saved record keeps its own CH2 set. */
    cfg->meas_ch1_mask        = (uint16_t)((1u << 0) | (1u << 1) | (1u << 2) | (1u << 8) | (1u << 10));
    cfg->meas_ch2_mask        = cfg->meas_ch1_mask;


    /* Compute and store checksum */
    cfg->checksum = config_compute_checksum(cfg);
}

/* ═══════════════════════════════════════════════════════════════════
 * State: everything a load may write, as one comparable value
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    scope_state_t ss;               /* the whole struct: session fields included */
    uint16_t meas2;
    uint8_t  theme, math_en, math_op, meter_sub, meter_lay, meter_link, startup, last_screen;
    uint8_t  view, view_rec;        /* scope_view (a load must not move it) and the view kept for the record */
    uint8_t  fft_win, fft_avg, fft_hold;
    uint8_t  wave, amp, duty, out, start_on;
    uint32_t freq;
    uint8_t  bright, vol, autooff;
    uint8_t  dac_running;
    uint32_t dac_calls;
} snap_t;

static void snap(snap_t *s)
{
    memset(s, 0, sizeof *s);
    memcpy(&s->ss, scope_state_get(), sizeof s->ss);
    s->meas2     = meas_select_get(1);
    s->theme     = (uint8_t)theme_get_id();
    s->math_en   = math_enabled ? 1u : 0u;
    s->math_op   = math_op;
    s->meter_sub = meter_submode;
    s->meter_lay = meter_layout;
    s->meter_link = (uint8_t)meter_link_get(NULL);
    s->startup   = (uint8_t)startup_mode;
    s->last_screen = model_last_screen();
    s->view      = (uint8_t)scope_view;
    s->view_rec  = model_view_record();
    s->fft_win   = (uint8_t)fft_window_get(NULL);
    s->fft_avg   = (uint8_t)fft_avg_get(NULL);
    s->fft_hold  = (uint8_t)fft_maxhold_get(NULL);
    const siggen_config_t *g = siggen_get_config();
    s->wave = g->waveform; s->freq = g->frequency_hz; s->amp = g->amplitude_dv;
    s->duty = g->duty_pct; s->out = g->output_enabled; s->start_on = g->start_on;
    s->bright  = sys_prefs_brightness();
    s->vol     = sys_prefs_volume();
    s->autooff = sys_prefs_autooff_mins();
    s->dac_running = dac_running;
    s->dac_calls   = dac_apply_calls;
}

static void restore(const snap_t *s)
{
    memcpy(scope_state_get(), &s->ss, sizeof s->ss);
    meas_select_set(1, s->meas2);
    theme_set((theme_id_t)s->theme);
    math_enabled  = s->math_en;
    math_op       = s->math_op;
    meter_submode = s->meter_sub;
    meter_layout  = s->meter_lay;
    meter_link_store(NULL, s->meter_link);
    startup_mode  = (startup_mode_t)s->startup;
    model_last_screen_load(s->last_screen);
    scope_view = (scope_view_t)s->view;
    model_view_boot_load(s->view_rec);      /* the same record value, kept */
    fft_window_store(NULL, s->fft_win);
    fft_avg_store(NULL, s->fft_avg);
    fft_maxhold_store(NULL, s->fft_hold);
    siggen_config_t g;
    memset(&g, 0, sizeof g);
    g.waveform = s->wave; g.frequency_hz = s->freq; g.amplitude_dv = s->amp;
    g.duty_pct = s->duty; g.output_enabled = s->out; g.start_on = s->start_on;
    siggen_init(&g);
    sys_prefs_set_brightness(s->bright);
    sys_prefs_set_volume(s->vol);
    sys_prefs_set_autooff_mins(s->autooff);
    dac_running     = s->dac_running;
    dac_apply_calls = s->dac_calls;
}

static void diff_snap(const snap_t *a, const snap_t *b)
{
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    for (size_t i = 0; i < sizeof *a; i++)
        if (x[i] != y[i]) printf("    state differs at byte %zu: ref %u, generated %u\n", i, x[i], y[i]);
}

static void diff_rec(const device_config_t *a, const device_config_t *b)
{
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    for (size_t i = 0; i < sizeof *a; i++)
        if (x[i] != y[i]) printf("    record differs at byte %zu: ref 0x%02x, generated 0x%02x\n", i, x[i], y[i]);
}

/* ═══ deterministic randomness ═══════════════════════════════════════ */

static uint32_t g_rng = 0x2C53u;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static uint32_t R(uint32_t n) { return rnd() % n; }

static const uint8_t k_autooff[4] = { 0, 15, 30, 60 };

/* A live state today's code could be in: every value a key, the shell or a
 * load can leave (coupling GND and trigger level +-103 only by a load). The
 * session fields are random too: neither capture may look at them. */
static void random_state(void)
{
    scope_state_t *ss = scope_state_get();
    channel_state_t *ch[2] = { &ss->ch1, &ss->ch2 };
    for (int i = 0; i < 2; i++) {
        ch[i]->enabled  = R(2);
        ch[i]->vdiv_idx = (uint8_t)R(VDIV_COUNT);
        ch[i]->coupling = (coupling_t)R(COUPLING_COUNT);
        ch[i]->probe    = (probe_t)R(PROBE_COUNT);
        ch[i]->bw_limit = R(2);
        ch[i]->position = (int16_t)((int)R(201) - 100);
    }
    ss->timebase_idx   = (uint8_t)R(TIMEBASE_COUNT);
    ss->trigger.mode   = (trigger_mode_t)R(TRIG_COUNT);
    ss->trigger.edge   = (trigger_edge_t)R(TRIG_EDGE_COUNT);
    ss->trigger.source = (trigger_source_t)R(TRIG_SRC_COUNT);
    ss->trigger.level  = (int16_t)((int)R(207) - 103);
    ss->running = R(2); ss->true_scale = R(2); ss->soft_trigger = R(2); ss->prm_hidden = R(2);
    ss->split_prm_hidden = R(2);
    ss->roll = R(2); ss->roll_step = (uint8_t)R(9); ss->trig_x = (int16_t)R(300);
    ss->cursor.mode = (cursor_mode_t)R(CURSOR_MODE_COUNT);
    meas_select_set(0, (uint16_t)rnd());
    meas_select_set(1, (uint16_t)rnd());
    theme_set((theme_id_t)R(THEME_COUNT));
    math_enabled  = R(2);
    math_op       = (uint8_t)R(MATH_COUNT);
    meter_submode = (uint8_t)R(METER_SUBMODE_COUNT);
    meter_layout  = (uint8_t)R(METER_LAYOUT_COUNT);
    meter_link_store(NULL, (int32_t)R(METER_LINK_POLICY_COUNT));
    startup_mode  = (startup_mode_t)R(STARTUP_COUNT);
    current_mode  = (device_mode_t)R(MODE_COUNT);          /* the screen shown: a capture must not look at it */
    model_last_screen_load((uint8_t)R(STARTUP_SCREEN_MAX + 1u));   /* the loaded screen: a capture carries it ... */
    if (R(2)) model_last_screen_note_power_off((uint8_t)R(STARTUP_SCREEN_MAX + 1u));   /* ... until a power-off */
    scope_view    = (scope_view_t)R(SCOPE_VIEW_COUNT);       /* with ss->roll above: any view.mode */
    model_view_boot_load((uint8_t)R(MODEL_VIEW_RECORD_MAX + 1u)); /* the loaded view: a capture carries it ... */
    if (R(2)) model_view_boot_forget();                    /* ... until it is in force: then the live one */
    fft_window_store(NULL, (int32_t)R((uint32_t)P(FFT_WINDOW)->max + 1u));
    fft_avg_store(NULL, (int32_t)R(MODEL_FFT_AVG_STEPS));
    fft_maxhold_store(NULL, (int32_t)R(2));
    siggen_config_t g;
    memset(&g, 0, sizeof g);
    g.waveform = (uint8_t)R(SIGGEN_WAVEFORM_COUNT);
    g.frequency_hz = R(SIGGEN_FREQ_MAX_HZ + 1u);
    g.amplitude_dv = (uint8_t)R(SIGGEN_AMP_MAX_DV + 1u);
    g.duty_pct = (uint8_t)R(SIGGEN_DUTY_MAX + 1u);
    g.output_enabled = R(2); g.start_on = R(2);
    siggen_init(&g);
    sys_prefs_set_brightness((uint8_t)rnd());
    sys_prefs_set_volume((uint8_t)R(SYS_VOL_MAX + 1u));
    sys_prefs_set_autooff_mins(k_autooff[R(4)]);
}

static uint8_t hostile_byte(void)
{
    static const uint8_t k[] = { 0, 1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 20, 21, 22,
                                 24, 25, 26, 29, 30, 31, 32, 33, 59, 60, 61, 99, 100, 101, 102,
                                 103, 104, 127, 128, 0xC0, 0xC1, 0xC2, 0xFE, 0xFF };
    switch (R(3)) {
    case 0:  return (uint8_t)rnd();
    case 1:  return (uint8_t)R(16);
    default: return k[R(sizeof k)];
    }
}

static float hostile_float(void)
{
    static const float k[] = { 0.0f, -0.0f, -1.0f, 0.04f, 0.05f, 0.06f, 0.1f, 1.0f, 2.94f, 2.95f,
                               2.96f, 3.0f, 3.04f, 3.05f, 3.06f, 3.1f, 3.14f, 3.15f, 3.16f, 4.0f,
                               31.0f, 0.49f, 0.5f, 999.5f, 1000.0f, 49999.4f, 49999.5f, 50000.0f,
                               50000.4f, 50000.5f, 50001.0f, 1e9f, -1e9f, 1e-30f };
    switch (R(5)) {
    case 0:  return NAN;
    case 1:  return R(2) ? INFINITY : -INFINITY;
    case 2:  { float f; uint32_t u = rnd(); memcpy(&f, &u, 4); return f; }   /* any bit pattern */
    case 3:  return (float)R(60000) + (float)R(100) / 100.0f;
    default: return k[R(sizeof k / sizeof k[0])];
    }
}

/* Any record: every byte hostile, padding included, then the floats, the
 * level and the mark biased to the edges; magic/version/checksum stamped as
 * a valid record would have them (the apply never reads them). */
static void random_record(device_config_t *c)
{
    uint8_t *b = (uint8_t *)c;
    for (size_t i = 0; i < sizeof *c; i++) b[i] = hostile_byte();
    if (R(4)) { float f = hostile_float(); memcpy(&c->siggen_frequency, &f, 4); }
    if (R(4)) { float f = hostile_float(); memcpy(&c->siggen_amplitude, &f, 4); }
    if (R(2)) {
        static const int16_t lv[] = { -32768, -1000, -104, -103, -102, -101, -100, -1, 0, 1,
                                      100, 101, 102, 103, 104, 1000, 32767 };
        int16_t l = R(2) ? lv[R(sizeof lv / sizeof lv[0])] : (int16_t)rnd();
        memcpy(&c->scope_trigger_level, &l, 2);
    }
    if (R(2)) c->sys_prefs_mark = CONFIG_SYS_PREFS_MARK;
    uint16_t m = (uint16_t)rnd();
    memcpy(&c->meas_ch1_mask, &m, 2);
    m = (uint16_t)rnd();
    memcpy(&c->meas_ch2_mask, &m, 2);
    c->magic = CONFIG_MAGIC;
    c->version = CONFIG_VERSION;
    c->checksum = config_compute_checksum(c);
}

/* ═══════════════════════════════════════════════════════════════════
 * Table coverage
 * ═══════════════════════════════════════════════════════════════════ */

static void test_table_coverage(void)
{
    /* every PF_PERSIST property has exactly one row */
    for (uint16_t i = 0; i < prop_count(); i++) {
        const prop_desc_t *p = prop_at(i);
        unsigned rows = 0;
        for (unsigned r = 0; r < model_persist_v5_field_count; r++)
            if (model_persist_v5_fields[r].prop_id == p->id) rows++;
        if (p->flags & PF_PERSIST) CHECKF(rows == 1, "%s: %u codec rows", p->name, rows);
        else if (rows) CHECKF(rows == 1 && model_persist_v5_find(p->id)->codec == V5_CONST,
                              "%s is not PF_PERSIST but has a codec row that is not const", p->name);
    }
    /* every row: a persisted property, a const one (channel enables), or a
     * record field with no property */
    uint8_t owner[sizeof(device_config_t)];
    memset(owner, 0, sizeof owner);
    for (unsigned r = 0; r < model_persist_v5_field_count; r++) {
        const v5_field_t *f = &model_persist_v5_fields[r];
        const prop_desc_t *p = prop_by_id(f->prop_id);
        if (f->prop_id) {
            CHECKF(p != NULL, "row %u names unknown id 0x%04x", r, f->prop_id);
            if (p && f->codec != V5_CONST) CHECKF(p->flags & PF_PERSIST, "row %u: %s not PF_PERSIST", r, p->name);
            if (p && (p->flags & PF_STORED_ONLY)) CHECK(f->flags & V5_F_STORED);
            if (p && (f->flags & V5_F_STORED)) CHECK(p->flags & PF_STORED_ONLY);
            /* the FFT settings are live: written from the property; sys.lang is stored only */
            if (f->prop_id == PROP_ID_FFT_WINDOW || f->prop_id == PROP_ID_FFT_AVG || f->prop_id == PROP_ID_FFT_MAXHOLD)
                CHECKF(!(f->flags & V5_F_STORED) && p && !(p->flags & PF_STORED_ONLY), "%s is stored only", p ? p->name : "?");
            if (f->prop_id == PROP_ID_SYS_LANG) CHECK((f->flags & V5_F_STORED) && p && (p->flags & PF_STORED_ONLY));
        } else {
            CHECK(f->codec == V5_CONST || f->codec == V5_CARRY || f->codec == V5_SCREEN);
        }
        /* the rows that keep their values for a later apply are the view
         * byte's: view.mode owns it, tb.roll names it with no bytes */
        CHECK((f->codec == V5_VIEW) == (f->prop_id == PROP_ID_VIEW_MODE || f->prop_id == PROP_ID_TB_ROLL));
        if (f->codec == V5_VIEW) {
            CHECK(f->offset == 23u && f->size == (f->prop_id == PROP_ID_VIEW_MODE ? 1u : 0u) &&
                  f->lo == 0 && f->hi == (int32_t)MODEL_VIEW_RECORD_MAX);
            CHECK(p && (p->flags & PF_HW));
            if (f->prop_id == PROP_ID_VIEW_MODE) CHECK(p && p->max == (int32_t)MODEL_VIEW_MODE_MAX);
        }
        for (unsigned k = 0; k < f->size; k++) {
            CHECKF(f->offset + k < sizeof owner, "row %u past the record", r);
            if (f->offset + k < sizeof owner) {
                CHECKF(owner[f->offset + k] == 0, "byte %u in two rows", f->offset + k);
                owner[f->offset + k] = 1;
            }
        }
    }
    /* every payload byte between the header and the checksum is one row's:
     * the struct has no padding left (11 was padding until meter.link took
     * it, 22 until the last screen did, 23 until view.mode did) */
    for (unsigned b = offsetof(device_config_t, scope_ch1_vdiv); b < offsetof(device_config_t, checksum); b++)
        CHECKF(owner[b] == 1, "record byte %u has no row", b);
    CHECK(offsetof(device_config_t, scope_trigger_level) == 12u);
    CHECK(offsetof(device_config_t, meter_link) == 11u);
    CHECK(offsetof(device_config_t, last_screen) == 22u);
    CHECK(offsetof(device_config_t, view_mode) == 23u);
    CHECK(offsetof(device_config_t, siggen_frequency) == 24u);
    CHECK(sizeof(device_config_t) == 60u && CONFIG_VERSION == 5);
    CHECK((P(VIEW_MODE)->flags & PF_PERSIST) && !(P(VIEW_MODE)->flags & PF_SESSION));
    CHECK((P(TB_ROLL)->flags & PF_PERSIST) && !(P(TB_ROLL)->flags & PF_SESSION));

    /* Session-only as a result: settable, not an alias, not usercal, no row
     * (or a const row that the record forces). Printed for the report. */
    printf("  session-only (not in the v5 record):");
    for (uint16_t i = 0; i < prop_count(); i++) {
        const prop_desc_t *p = prop_at(i);
        if (!prop_is_settable(p) || (p->flags & (PF_ALIAS | PF_USERCAL))) continue;
        const v5_field_t *f = model_persist_v5_find(p->id);
        if (!f || f->codec == V5_CONST) {
            printf(" %s%s", p->name, f ? "(forced on)" : "");
            CHECKF(p->flags & PF_SESSION, "%s not in the record but not PF_SESSION", p->name);
        }
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════
 * Defaults
 * ═══════════════════════════════════════════════════════════════════ */

static void test_defaults_identical(void)
{
    device_config_t a, b;
    memset(&a, 0xA5, sizeof a);
    memset(&b, 0x5A, sizeof b);
    ref_defaults(&a);
    config_init_defaults(&b);
    CHECK(memcmp(&a, &b, sizeof a) == 0);
    if (memcmp(&a, &b, sizeof a)) diff_rec(&a, &b);
    CHECK(config_validate(&b));

    /* the same state from both loads */
    random_state();
    snap_t pre, s1, s2;
    snap(&pre);
    ref_apply(&a);
    snap(&s1);
    restore(&pre);
    settings_store_apply(&b);
    snap(&s2);
    CHECK(memcmp(&s1, &s2, sizeof s1) == 0);
    if (memcmp(&s1, &s2, sizeof s1)) diff_snap(&s1, &s2);

    /* and every persisted property then reads its built-in default (the
     * view byte's two: kept by the load, in force after the boot apply) */
    CHECK(model_view_record() == model_view_record_of(prop_default(P(VIEW_MODE)), prop_default(P(TB_ROLL))));
    CHECK(model_view_boot_apply());
    for (uint16_t i = 0; i < prop_count(); i++) {
        const prop_desc_t *p = prop_at(i);
        if (!(p->flags & PF_PERSIST)) continue;
        CHECKF(prop_get(p) == prop_default(p), "%s = %ld after a defaults load, default %ld",
               p->name, (long)prop_get(p), (long)prop_default(p));
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * Random live states -> byte-identical records
 * ═══════════════════════════════════════════════════════════════════ */

#define N_STATES   20000u
#define N_RECORDS  20000u

static void test_random_states_capture_identical(void)
{
    unsigned bad = 0, bad_state = 0;
    for (unsigned n = 0; n < N_STATES; n++) {
        /* The image a capture starts from is settings_store's last one: the
         * carried bytes (stored-only, display_persist_mode, padding) come
         * from a loaded record. Load one, then move every live value. */
        device_config_t base;
        random_record(&base);
        settings_store_apply(&base);
        random_state();

        device_config_t a = base, b = base;
        ref_capture(&a);
        settings_store_capture(&b);
        if (memcmp(&a, &b, sizeof a) != 0) {
            if (bad++ < 3) { printf("  state %u:\n", n); diff_rec(&a, &b); }
        }

        /* and each record loads to the same state from another live state */
        random_state();
        snap_t pre, s1, s2;
        snap(&pre);
        ref_apply(&a);
        snap(&s1);
        restore(&pre);
        settings_store_apply(&b);
        snap(&s2);
        if (memcmp(&s1, &s2, sizeof s1) != 0) {
            if (bad_state++ < 3) { printf("  state %u loaded back:\n", n); diff_snap(&s1, &s2); }
        }
    }
    CHECKF(bad == 0, "%u of %u random states captured differently", bad, N_STATES);
    CHECKF(bad_state == 0, "%u of %u captured states loaded differently", bad_state, N_STATES);
    printf("  %u random live states: records byte-identical, identical state loaded back\n", N_STATES);
}

/* ═══════════════════════════════════════════════════════════════════
 * Random (hostile) records -> identical state, identical records back
 * ═══════════════════════════════════════════════════════════════════ */

static void test_random_records_apply_identical(void)
{
    unsigned bad_state = 0, bad_rec = 0;
    for (unsigned n = 0; n < N_RECORDS; n++) {
        device_config_t rec;
        random_record(&rec);
        random_state();
        snap_t pre, s1, s2;
        snap(&pre);

        ref_apply(&rec);
        snap(&s1);
        device_config_t a = rec;
        ref_capture(&a);

        restore(&pre);
        settings_store_apply(&rec);
        snap(&s2);
        device_config_t b = rec;
        settings_store_capture(&b);

        if (memcmp(&s1, &s2, sizeof s1) != 0) {
            if (bad_state++ < 3) { printf("  record %u:\n", n); diff_snap(&s1, &s2); diff_rec(&rec, &rec); }
        }
        if (memcmp(&a, &b, sizeof a) != 0) {
            if (bad_rec++ < 3) { printf("  record %u captured back:\n", n); diff_rec(&a, &b); }
        }
    }
    CHECKF(bad_state == 0, "%u of %u records loaded to a different state", bad_state, N_RECORDS);
    CHECKF(bad_rec == 0, "%u of %u records captured back differently", bad_rec, N_RECORDS);
    printf("  %u random records: identical state, identical records captured back\n", N_RECORDS);
}

/* Each index field at count - 1, count and 255 (and every other byte value),
 * one field at a time over the defaults: exhaustive for the one-byte rows. */
static void test_every_byte_value_of_every_one_byte_field(void)
{
    unsigned bad = 0, cases = 0;
    for (unsigned r = 0; r < model_persist_v5_field_count; r++) {
        const v5_field_t *f = &model_persist_v5_fields[r];
        if (f->size != 1) continue;
        for (unsigned v = 0; v < 256; v++) {
            for (unsigned mark = 0; mark < 2; mark++) {
                device_config_t rec;
                config_init_defaults(&rec);
                if (!mark) rec.sys_prefs_mark = 0;
                ((uint8_t *)&rec)[f->offset] = (uint8_t)v;
                if (f->codec == V5_GEN_OUT) rec.siggen_start_on = 1;     /* the enable only counts with it */
                random_state();
                snap_t pre, s1, s2;
                snap(&pre);
                ref_apply(&rec);
                snap(&s1);
                restore(&pre);
                settings_store_apply(&rec);
                snap(&s2);
                cases++;
                if (memcmp(&s1, &s2, sizeof s1) != 0 && bad++ < 3) {
                    printf("  byte %u = %u (mark %u):\n", f->offset, v, mark);
                    diff_snap(&s1, &s2);
                }
            }
        }
    }
    CHECKF(bad == 0, "%u of %u single-field records loaded differently", bad, cases);
    printf("  %u single-field records (every byte value of every one-byte field): identical\n", cases);
}

/* ═══════════════════════════════════════════════════════════════════
 * Round trip through the properties
 * ═══════════════════════════════════════════════════════════════════ */

static void test_round_trip_through_properties(void)
{
    for (unsigned n = 0; n < 2000u; n++) {
        random_state();
        model_view_boot_forget();               /* the live view is the one to save */
        int32_t want[PROP_IX_COUNT];
        for (uint16_t i = 0; i < prop_count(); i++) want[i] = prop_get(prop_at(i));
        const bool men = math_enabled; const uint8_t mop = math_op;
        device_config_t rec;
        config_init_defaults(&rec);
        settings_store_capture(&rec);
        CHECK(config_validate(&rec));

        device_config_t d;
        config_init_defaults(&d);
        settings_store_apply(&d);
        /* a power cycle for the view: RAM comes up in Y-T, the load keeps
         * the record's view, the boot apply sets it */
        scope_view = SCOPE_VIEW_TIME;
        scope_state_get()->roll = false;
        settings_store_apply(&rec);
        CHECK(prop_get(P(VIEW_MODE)) == 0 &&
              model_view_record() == model_view_record_of(want[PROP_IX_VIEW_MODE], want[PROP_IX_TB_ROLL]));
        CHECK(model_view_boot_apply());
        for (uint16_t i = 0; i < prop_count(); i++) {
            const prop_desc_t *p = prop_at(i);
            if (!(p->flags & PF_PERSIST)) continue;
            int32_t w = want[i];
            /* gen.out is saved only with gen.bootout on */
            if (p->id == PROP_ID_GEN_OUT && !want[PROP_IX_GEN_BOOTOUT]) w = 0;
            /* roll is saved only with a view that takes it (a store can leave it under another) */
            if (p->id == PROP_ID_TB_ROLL && !view_takes_roll((uint8_t)want[PROP_IX_VIEW_MODE])) w = 0;
            if (prop_get(p) != w) { CHECKF(0, "%s: %ld back as %ld", p->name, (long)w, (long)prop_get(p)); break; }
        }
        CHECK(math_enabled == men && math_op == mop);   /* "off, op 3" comes back as op 3 */
        CHECK(scope_state_get()->ch1.enabled && scope_state_get()->ch2.enabled);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * (a) The out-of-range policy, field by field (model_persist_v5.c's list)
 * ═══════════════════════════════════════════════════════════════════ */

static int32_t load_get(device_config_t *rec, uint16_t id)
{
    rec->checksum = config_compute_checksum(rec);
    settings_store_apply(rec);
    return prop_get(prop_by_id(id));
}

static void put_f32(float *dst, float v) { memcpy(dst, &v, 4); }

static void test_out_of_range_policy(void)
{
    device_config_t c;
#define FRESH() config_init_defaults(&c)
    /* enums: out of range -> the default */
    FRESH(); c.scope_ch1_vdiv = 10;   CHECK(load_get(&c, PROP_ID_CH1_VDIV) == 3);
    FRESH(); c.scope_ch2_vdiv = 255;  CHECK(load_get(&c, PROP_ID_CH2_VDIV) == 5);
    FRESH(); c.scope_ch1_vdiv = 9;    CHECK(load_get(&c, PROP_ID_CH1_VDIV) == 9);
    FRESH(); c.scope_timebase = 21;   CHECK(load_get(&c, PROP_ID_TB_CODE) == 10);
    FRESH(); c.scope_trigger_mode = 3; CHECK(load_get(&c, PROP_ID_TRIG_MODE) == 0);
    FRESH(); c.scope_trigger_edge = 2; CHECK(load_get(&c, PROP_ID_TRIG_EDGE) == 0);
    FRESH(); c.scope_trigger_source = 2; CHECK(load_get(&c, PROP_ID_TRIG_SOURCE) == 0);
    FRESH(); c.scope_ch1_coupling = 2; CHECK(load_get(&c, PROP_ID_CH1_COUPLING) == 2);   /* GND kept */
    FRESH(); c.scope_ch2_coupling = 3; CHECK(load_get(&c, PROP_ID_CH2_COUPLING) == 0);
    FRESH(); c.scope_ch1_probe = 2;   CHECK(load_get(&c, PROP_ID_CH1_PROBE) == 0);
    FRESH(); c.theme = 4;             CHECK(load_get(&c, PROP_ID_SYS_THEME) == 0);
    FRESH(); c.meter_submode = 12;    CHECK(load_get(&c, PROP_ID_METER_FUNC) == 0);
    FRESH(); c.meter_submode = 11;    CHECK(load_get(&c, PROP_ID_METER_FUNC) == 11);
    FRESH(); c.meter_layout = 5;      CHECK(load_get(&c, PROP_ID_METER_LAYOUT) == 0);
    FRESH(); c.meter_link = 1;        CHECK(load_get(&c, PROP_ID_METER_LINK) == METER_LINK_ALWAYS);
    FRESH(); c.meter_link = 2;        CHECK(load_get(&c, PROP_ID_METER_LINK) == METER_LINK_SCREEN);
    FRESH(); c.meter_link = 0xFF;     CHECK(load_get(&c, PROP_ID_METER_LINK) == METER_LINK_SCREEN);
    FRESH(); c.siggen_waveform = 13;  CHECK(load_get(&c, PROP_ID_GEN_WAVE) == 0);
    FRESH(); c.siggen_duty = 101;     CHECK(load_get(&c, PROP_ID_GEN_DUTY) == 50);
    FRESH(); c.siggen_duty = 100;     CHECK(load_get(&c, PROP_ID_GEN_DUTY) == 100);
    FRESH(); c.sound_enabled = 11;    CHECK(load_get(&c, PROP_ID_SYS_VOLUME) == 1);
    FRESH(); c.sound_enabled = 10;    CHECK(load_get(&c, PROP_ID_SYS_VOLUME) == 10);
    /* trigger level saturates at +-103 (wider than the property's +-100) */
    FRESH(); c.scope_trigger_level = 500;  CHECK(load_get(&c, PROP_ID_TRIG_LEVEL) == 103);
    FRESH(); c.scope_trigger_level = -104; CHECK(load_get(&c, PROP_ID_TRIG_LEVEL) == -103);
    FRESH(); c.scope_trigger_level = 102;  CHECK(load_get(&c, PROP_ID_TRIG_LEVEL) == 102);
    /* bools: any non-zero byte */
    FRESH(); c.scope_ch1_bw_limit = 0x80; CHECK(load_get(&c, PROP_ID_CH1_BWLIMIT) == 1);
    /* masks: bits above 12 dropped */
    FRESH(); c.meas_ch1_mask = 0xF00F;    CHECK(load_get(&c, PROP_ID_CH1_MEAS) == 0x000F);
    FRESH(); c.meas_ch2_mask = 0xFFFF;    CHECK(load_get(&c, PROP_ID_CH2_MEAS) == 0x0FFF);
    /* math: enable = non-zero, op >= 8 -> 0, "off, op 3" keeps op 3 */
    FRESH(); c.math_enabled = 7; c.math_op = 8; (void)load_get(&c, PROP_ID_VIEW_MATH);
    CHECK(math_enabled && math_op == 0);
    FRESH(); c.math_enabled = 0; c.math_op = 3; CHECK(load_get(&c, PROP_ID_VIEW_MATH) == 0);
    CHECK(!math_enabled && math_op == 3);
    /* generator floats */
    FRESH(); put_f32(&c.siggen_frequency, NAN);      CHECK(load_get(&c, PROP_ID_GEN_FREQ) == 1000);
    FRESH(); put_f32(&c.siggen_frequency, -1.0f);    CHECK(load_get(&c, PROP_ID_GEN_FREQ) == 1000);
    FRESH(); put_f32(&c.siggen_frequency, 50000.5f); CHECK(load_get(&c, PROP_ID_GEN_FREQ) == 1000);
    FRESH(); put_f32(&c.siggen_frequency, 49999.5f); CHECK(load_get(&c, PROP_ID_GEN_FREQ) == 50000);
    FRESH(); put_f32(&c.siggen_frequency, 1234.4f);  CHECK(load_get(&c, PROP_ID_GEN_FREQ) == 1234);
    FRESH(); put_f32(&c.siggen_amplitude, INFINITY); CHECK(load_get(&c, PROP_ID_GEN_AMP) == 30);
    FRESH(); put_f32(&c.siggen_amplitude, -0.1f);    CHECK(load_get(&c, PROP_ID_GEN_AMP) == 30);
    FRESH(); put_f32(&c.siggen_amplitude, 1.54f);    CHECK(load_get(&c, PROP_ID_GEN_AMP) == 15);
    FRESH(); put_f32(&c.siggen_amplitude, 3.05f);    CHECK(load_get(&c, PROP_ID_GEN_AMP) == 30);   /* 31 -> 30 */
    FRESH(); put_f32(&c.siggen_amplitude, 0.0f);     CHECK(load_get(&c, PROP_ID_GEN_AMP) == 0);
    /* the enable needs the boot policy */
    FRESH(); c.siggen_enabled = 1; c.siggen_start_on = 0; CHECK(load_get(&c, PROP_ID_GEN_OUT) == 0);
    FRESH(); c.siggen_enabled = 9; c.siggen_start_on = 2; CHECK(load_get(&c, PROP_ID_GEN_OUT) == 1);
    CHECK(prop_get(P(GEN_BOOTOUT)) == 1);
    /* startup: 0 scope, 2 siggen, 4 home, anything else meter */
    FRESH(); c.startup_mode = 0; CHECK(load_get(&c, PROP_ID_SYS_STARTUP) == STARTUP_SCOPE);
    FRESH(); c.startup_mode = 2; CHECK(load_get(&c, PROP_ID_SYS_STARTUP) == STARTUP_SIGGEN);
    FRESH(); c.startup_mode = 3; CHECK(load_get(&c, PROP_ID_SYS_STARTUP) == STARTUP_METER);
    FRESH(); c.startup_mode = 4; CHECK(load_get(&c, PROP_ID_SYS_STARTUP) == STARTUP_HOME);
    FRESH(); c.startup_mode = 99; CHECK(load_get(&c, PROP_ID_SYS_STARTUP) == STARTUP_METER);
    FRESH(); c.startup_mode = 5; CHECK(load_get(&c, PROP_ID_SYS_STARTUP) == STARTUP_RESTORE);
    FRESH(); c.startup_mode = 6; CHECK(load_get(&c, PROP_ID_SYS_STARTUP) == STARTUP_METER);
    FRESH(); c.startup_mode = 1; CHECK(load_get(&c, PROP_ID_SYS_STARTUP) == STARTUP_METER);
    FRESH();                     CHECK(load_get(&c, PROP_ID_SYS_STARTUP) == STARTUP_RESTORE);   /* the default record */
    /* the last screen: 0..3, anything else none */
    FRESH();                      CHECK(load_get(&c, PROP_ID_SYS_LASTSCREEN) == STARTUP_SCREEN_NONE);
    FRESH(); c.last_screen = 1;   CHECK(load_get(&c, PROP_ID_SYS_LASTSCREEN) == STARTUP_SCREEN_SCOPE);
    FRESH(); c.last_screen = 2;   CHECK(load_get(&c, PROP_ID_SYS_LASTSCREEN) == STARTUP_SCREEN_METER);
    FRESH(); c.last_screen = 3;   CHECK(load_get(&c, PROP_ID_SYS_LASTSCREEN) == STARTUP_SCREEN_SIGGEN);
    FRESH(); c.last_screen = 4;   CHECK(load_get(&c, PROP_ID_SYS_LASTSCREEN) == STARTUP_SCREEN_NONE);
    FRESH(); c.last_screen = 0xFF; CHECK(load_get(&c, PROP_ID_SYS_LASTSCREEN) == STARTUP_SCREEN_NONE);
    /* brightness: 0 or > 100 -> 100, < 25 -> 25, else down to a 5 step */
    FRESH(); c.display_brightness = 0;   CHECK(load_get(&c, PROP_ID_SYS_BRIGHTNESS) == 100);
    FRESH(); c.display_brightness = 101; CHECK(load_get(&c, PROP_ID_SYS_BRIGHTNESS) == 100);
    FRESH(); c.display_brightness = 10;  CHECK(load_get(&c, PROP_ID_SYS_BRIGHTNESS) == 25);
    FRESH(); c.display_brightness = 67;  CHECK(load_get(&c, PROP_ID_SYS_BRIGHTNESS) == 65);
    /* auto-off: minutes other than 0/15/30/60 -> off */
    FRESH(); c.auto_shutdown_mins = 30; CHECK(load_get(&c, PROP_ID_SYS_AUTOOFF) == 2);
    FRESH(); c.auto_shutdown_mins = 45; CHECK(load_get(&c, PROP_ID_SYS_AUTOOFF) == 0);
    /* no sys_prefs_mark: the factory trio, whatever the bytes say */
    FRESH(); c.sys_prefs_mark = 0; c.display_brightness = 50; c.sound_enabled = 5; c.auto_shutdown_mins = 15;
    (void)load_get(&c, 0);
    CHECK(sys_prefs_brightness() == 100 && sys_prefs_volume() == 1 && sys_prefs_autooff_mins() == 0);
    /* channel enables: always on after a load */
    FRESH(); c.scope_ch1_enabled = 0; c.scope_ch2_enabled = 0;
    scope_state_get()->ch2.enabled = false;
    CHECK(load_get(&c, PROP_ID_CH2_ENABLED) == 1 && prop_get(P(CH1_ENABLED)) == 1);
#undef FRESH
}

/* ═══════════════════════════════════════════════════════════════════
 * (b) Stored-only fields: raw carry, and saved once set
 * ═══════════════════════════════════════════════════════════════════ */

static void test_stored_only_raw_carry(void)
{
    /* persist mode, lang (11 is meter.link now, 22 the last screen, 23
     * view.mode, 18..20 the FFT settings) */
    static const unsigned carried[] = { offsetof(device_config_t, display_persist_mode),
                                        offsetof(device_config_t, language) };
    for (unsigned n = 0; n < 2000u; n++) {
        device_config_t rec;
        config_init_defaults(&rec);
        for (unsigned i = 0; i < sizeof carried / sizeof carried[0]; i++)
            ((uint8_t *)&rec)[carried[i]] = (uint8_t)rnd();
        rec.checksum = config_compute_checksum(&rec);
        settings_store_apply(&rec);
        random_state();                            /* every owned setting moves */
        device_config_t next = rec;                /* settings_store's g_live */
        settings_store_capture(&next);
        for (unsigned i = 0; i < sizeof carried / sizeof carried[0]; i++)
            CHECKF(((uint8_t *)&next)[carried[i]] == ((uint8_t *)&rec)[carried[i]],
                   "byte %u: 0x%02x saved as 0x%02x", carried[i],
                   ((uint8_t *)&rec)[carried[i]], ((uint8_t *)&next)[carried[i]]);
        /* and a second cycle: load what was saved, save again */
        settings_store_apply(&next);
        device_config_t again = next;
        settings_store_capture(&again);
        CHECK(memcmp(&again, &next, sizeof again) == 0);
    }

    /* A byte nothing understands loads the default into the property ... */
    const unsigned hold_at = offsetof(device_config_t, fft_max_hold);
    device_config_t rec;
    config_init_defaults(&rec);
    rec.language = 7; rec.fft_window = 9; rec.fft_avg_count = 3;
    ((uint8_t *)&rec)[hold_at] = 0x40;                     /* a bool field: poke the byte */
    settings_store_apply(&rec);
    CHECK(prop_get(P(SYS_LANG)) == 0 && prop_get(P(FFT_WINDOW)) == 1 && prop_get(P(FFT_AVG)) == 0);
    CHECK(prop_get(P(FFT_MAXHOLD)) == 1);
    /* ... and is saved as that value: the FFT settings are live, so a capture
     * writes the property; sys.lang's byte is carried */
    device_config_t back = rec;
    settings_store_capture(&back);
    CHECKF(back.fft_window == 1 && back.fft_avg_count == 0 && ((uint8_t *)&back)[hold_at] == 1u &&
           back.language == 7, "win %u avg %u maxhold 0x%02x lang %u", back.fft_window, back.fft_avg_count,
           ((uint8_t *)&back)[hold_at], back.language);
    /* ... one it does understand reads back ... */
    rec.fft_window = 3; rec.fft_avg_count = 16;
    settings_store_apply(&rec);
    CHECK(prop_get(P(FFT_WINDOW)) == 3 && prop_get(P(FFT_AVG)) == 4);
    /* ... and a set is saved, the carried bytes untouched */
    CHECK(prop_set_local(P(FFT_WINDOW), 2, ORIGIN_KEY, NULL) == PS_OK);
    CHECK(prop_set_local(P(FFT_AVG), 3, ORIGIN_KEY, NULL) == PS_OK);
    device_config_t next = rec;
    settings_store_capture(&next);
    CHECK(next.fft_window == 2 && next.fft_avg_count == 8);
    CHECKF(next.language == 7 && ((uint8_t *)&next)[hold_at] == 1u, "lang %u maxhold 0x%02x", next.language,
           ((uint8_t *)&next)[hold_at]);
    CHECK(config_validate(&next));
}

/* The FFT settings' and sys.lang's rows decode and encode by their own
 * tables. Every byte of sys.lang, fft.window, fft.avg_count and
 * fft.max_hold against the documented mapping (props_sys.c labels; fft.avg
 * is an index into the counts 0/2/4/8/16/32), and every value back: the FFT
 * settings, live, are written as their record byte whatever the byte held;
 * sys.lang, stored only, keeps a byte that decodes to its value. */
static void test_fft_and_lang_codec(void)
{
    static const uint8_t counts[] = { 0u, 2u, 4u, 8u, 16u, 32u };    /* lbl_fft_avg: off x2 .. x32 */
    const int32_t n_counts = (int32_t)sizeof counts;
    const int32_t win_max = P(FFT_WINDOW)->max, win_def = prop_default(P(FFT_WINDOW));
    const v5_field_t *fw = model_persist_v5_find(PROP_ID_FFT_WINDOW);
    const v5_field_t *fa = model_persist_v5_find(PROP_ID_FFT_AVG);
    const v5_field_t *fh = model_persist_v5_find(PROP_ID_FFT_MAXHOLD);
    const v5_field_t *fl = model_persist_v5_find(PROP_ID_SYS_LANG);
    CHECK(fw && fa && fh && fl);
    if (!fw || !fa || !fh || !fl) return;
    CHECK(fw->offset == offsetof(device_config_t, fft_window) &&
          fa->offset == offsetof(device_config_t, fft_avg_count) &&
          fh->offset == offsetof(device_config_t, fft_max_hold) &&
          fl->offset == offsetof(device_config_t, language));
    /* the counts are one table: the record's, the FFT's and the labels' */
    CHECK(MODEL_FFT_AVG_STEPS == n_counts && P(FFT_AVG)->max == n_counts - 1 &&
          memcmp(model_fft_avg_counts, counts, sizeof counts) == 0);
    CHECK(win_max == (int32_t)REF_FFT_WINDOW_COUNT - 1 && win_def == (int32_t)REF_FFT_WINDOW_DEFAULT);
    unsigned bad = 0;
    for (unsigned b = 0; b < 256u; b++) {
        device_config_t rec;
        config_init_defaults(&rec);
        rec.fft_window = (uint8_t)b; rec.fft_avg_count = (uint8_t)b; rec.language = (uint8_t)b;
        ((uint8_t *)&rec)[fh->offset] = (uint8_t)b;
        const int32_t want_w = (int32_t)b <= win_max ? (int32_t)b : win_def;   /* rect..flattop, else Hanning */
        int32_t want_a = 0;                                            /* not a count: off */
        for (int32_t i = 0; i < n_counts; i++) if (counts[i] == b) want_a = i;
        const int32_t want_h = b != 0u;                                /* any non-zero byte: on */
        const int32_t want_l = 0;                                      /* English, the only one */
        if (model_persist_v5_decode(fw, &rec) != want_w) bad++;
        if (model_persist_v5_decode(fa, &rec) != want_a) bad++;
        if (model_persist_v5_decode(fh, &rec) != want_h) bad++;
        if (model_persist_v5_decode(fl, &rec) != want_l) bad++;
        /* the load puts the same into the properties */
        settings_store_apply(&rec);
        if (prop_get(P(FFT_WINDOW)) != want_w || prop_get(P(FFT_AVG)) != want_a ||
            prop_get(P(FFT_MAXHOLD)) != want_h || prop_get(P(SYS_LANG)) != want_l) bad++;
        /* and a capture writes the FFT settings' values, never the byte */
        device_config_t next = rec;
        settings_store_capture(&next);
        if (next.fft_window != (uint8_t)want_w || next.fft_avg_count != counts[want_a] ||
            ((uint8_t *)&next)[fh->offset] != (uint8_t)want_h || next.language != (uint8_t)b) bad++;
    }
    CHECKF(bad == 0, "%u bytes decoded or captured back wrong", bad);
    /* every value: captured as its record byte, and read back */
    for (int32_t v = 0; v < n_counts; v++) {
        device_config_t rec;
        config_init_defaults(&rec);
        rec.fft_window = 0xEE; rec.fft_avg_count = 0xEE; rec.language = 0xEE;   /* nothing understood */
        ((uint8_t *)&rec)[fh->offset] = 0xEE;
        settings_store_apply(&rec);
        if (v <= win_max) CHECK(prop_set_local(P(FFT_WINDOW), v, ORIGIN_KEY, NULL) == PS_OK);
        CHECK(prop_set_local(P(FFT_AVG), v, ORIGIN_KEY, NULL) == PS_OK);
        device_config_t next = rec;
        settings_store_capture(&next);
        /* the default included: a live setting is written, not carried */
        CHECKF(next.fft_avg_count == counts[v], "fft.avg %ld saved as %u", (long)v, next.fft_avg_count);
        CHECK(model_persist_v5_decode(fa, &next) == v);
        if (v <= win_max) {
            CHECKF(next.fft_window == (uint8_t)v, "fft.window %ld saved as %u", (long)v, next.fft_window);
            CHECK(model_persist_v5_decode(fw, &next) == v);
        }
        CHECK(((uint8_t *)&next)[fh->offset] == 1u);   /* 0xEE loaded as on, written as 1 */
        CHECK(next.language == 0xEE);                  /* unchanged stored-only property: the byte is carried */
    }
    /* sys.lang's one value is written when it is not what the byte says */
    {
        device_config_t rec;
        config_init_defaults(&rec);
        rec.language = 0;
        settings_store_apply(&rec);
        device_config_t next = rec;
        next.language = 1;                             /* a record byte that decodes to the default */
        settings_store_capture(&next);
        CHECK(next.language == 1);                     /* 1 decodes to 0 = the property: carried */
        CHECK(model_persist_v5_decode(fl, &next) == 0);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * Change notification by origin
 * ═══════════════════════════════════════════════════════════════════ */

static unsigned g_notes;
static bool     g_shell_persist = true;
static bool     g_real_note;
static void f_note(void) { g_notes++; if (g_real_note) settings_store_note_change(1000u); }
static bool f_shell_persist(void) { return g_shell_persist; }
static bool g_op_fail, g_selfcal;
static bool f_ok_u8(uint8_t ch, uint8_t v) { (void)ch; (void)v; return !g_op_fail; }
static bool f_ok_pos(uint8_t ch, int16_t v) { (void)ch; (void)v; return true; }
static bool f_ok_void(void) { return !g_op_fail; }
static bool f_busy(void) { return g_selfcal; }
static const model_hw_ops_t k_ops = {
    .vdiv = f_ok_u8, .coupling = f_ok_u8, .vpos = f_ok_pos, .trigger_source = f_ok_void,
    .selfcal_busy = f_busy, .shell_persist = f_shell_persist, .note_change = f_note,
};

/* A refused commit is no settings change: whatever said no (a rule, the
 * owner, the range check, the self-cal lock), no note reaches the store and
 * the record still matches what is saved. Persisted properties, key origin
 * (the one that always notes). */
static void test_refused_commit_no_note(void)
{
    device_config_t d;
    config_init_defaults(&d);
    settings_store_apply(&d);
    model_bind_hw(&k_ops);
    (void)settings_store_load_and_apply();           /* no storage bound: defaults */
    g_real_note = true;
    g_notes = 0;
    const uint32_t seen = settings_store_get_status()->changes_seen;
    scope_state_t *ss = scope_state_get();
    prop_note_t n;

    /* REFUSED by a rule: the trigger source on a channel that is off */
    ss->ch2.enabled = false;
    CHECK(prop_set_local(P(TRIG_SOURCE), TRIG_SRC_CH2, ORIGIN_KEY, &n) == PS_REFUSED);
    CHECK(strcmp(n.text, "CH2 is off") == 0 && prop_get(P(TRIG_SOURCE)) == TRIG_SRC_CH1);
    ss->ch2.enabled = true;
    /* NOT_SET by the owner */
    g_op_fail = true;
    CHECK(prop_set_local(P(CH1_VDIV), 6, ORIGIN_KEY, &n) == PS_NOT_SET);
    CHECK(prop_set_local(P(TRIG_SOURCE), TRIG_SRC_CH2, ORIGIN_KEY, &n) == PS_NOT_SET);
    g_op_fail = false;
    /* BUSY: self-cal holds the scope's properties */
    g_selfcal = true;
    CHECK(prop_set_local(P(TB_CODE), 4, ORIGIN_KEY, &n) == PS_BUSY);
    g_selfcal = false;
    /* INVALID: out of range */
    CHECK(prop_set_local(P(CH1_COUPLING), 5, ORIGIN_KEY, &n) == PS_INVALID);
    CHECK(prop_set_local(P(SYS_THEME), 9, ORIGIN_KEY, &n) == PS_INVALID);
    CHECK(prop_set_local(P(FFT_WINDOW), 7, ORIGIN_KEY, &n) == PS_INVALID);

    CHECKF(g_notes == 0, "%u notes after refused commits", g_notes);
    CHECK(settings_store_get_status()->changes_seen == seen);
    /* and an accepted one still notes (the hook is live) */
    CHECK(prop_set_local(P(CH1_VDIV), 6, ORIGIN_KEY, &n) == PS_OK);
    CHECK(g_notes == 1 && settings_store_get_status()->changes_seen == seen + 1);
    g_real_note = false;
    model_bind_hw(NULL);
}

static void test_notes_by_origin(void)
{
    device_config_t d;
    config_init_defaults(&d);
    settings_store_apply(&d);
    model_bind_hw(&k_ops);
    g_notes = 0;

    CHECK(prop_set_local(P(CH1_VDIV), 4, ORIGIN_KEY, NULL) == PS_OK);     CHECK(g_notes == 1);
    CHECK(prop_set_local(P(CH1_VDIV), 4, ORIGIN_KEY, NULL) == PS_OK);     CHECK(g_notes == 1);  /* no change */
    CHECK(prop_set_local(P(CH1_VDIV), 5, ORIGIN_SHELL, NULL) == PS_OK);   CHECK(g_notes == 2);
    g_shell_persist = false;                                              /* `persist off` */
    CHECK(prop_set_local(P(CH1_VDIV), 6, ORIGIN_SHELL, NULL) == PS_OK);   CHECK(g_notes == 2);
    CHECK(prop_set_local(P(CH1_VDIV), 7, ORIGIN_KEY, NULL) == PS_OK);     CHECK(g_notes == 3);  /* keys still save */
    g_shell_persist = true;
    CHECK(prop_set_local(P(CH1_VDIV), 8, ORIGIN_BOOT, NULL) == PS_OK);    CHECK(g_notes == 3);
    CHECK(prop_set_local(P(CH1_VDIV), 2, ORIGIN_AUTO, NULL) == PS_OK);    CHECK(g_notes == 4);
    CHECK(prop_set_local(P(CH1_POS), 20, ORIGIN_KEY, NULL) == PS_OK);     CHECK(g_notes == 4);  /* session */
    CHECK(prop_set_local(P(VIEW_TRUESCALE), 0, ORIGIN_KEY, NULL) == PS_OK); CHECK(g_notes == 4);
    settings_store_apply(&d);                                             CHECK(g_notes == 4);  /* a load never notes */

    /* bound to the real store: a persisted key commit is a pending change */
    (void)settings_store_load_and_apply();           /* no storage bound: defaults */
    g_real_note = true;
    const uint32_t seen = settings_store_get_status()->changes_seen;
    CHECK(prop_set_local(P(TB_CODE), 12, ORIGIN_KEY, NULL) == PS_OK);
    CHECK(settings_store_get_status()->changes_seen == seen + 1);
    CHECK(prop_set_local(P(CH2_POS), 40, ORIGIN_KEY, NULL) == PS_OK);     /* session: no capture */
    CHECK(settings_store_get_status()->changes_seen == seen + 1);
    g_real_note = false;
    model_bind_hw(NULL);
}

/* ═══════════════════════════════════════════════════════════════════
 * meter.link: the former padding byte at offset 11
 * ═══════════════════════════════════════════════════════════════════ */

static void test_meter_link_record(void)
{
    /* A record from before the field existed: byte 11 is 0 (padding, zeroed
     * by the defaults' memset and carried by every capture since). It loads
     * as "screen", from any live state. */
    device_config_t old;
    ref_defaults(&old);
    CHECK(((const uint8_t *)&old)[11] == 0u);
    current_mode = MODE_HOME;
    meter_link_store(NULL, METER_LINK_ALWAYS);
    settings_store_apply(&old);
    CHECK(prop_get(P(METER_LINK)) == METER_LINK_SCREEN);
    CHECK(prop_default(P(METER_LINK)) == METER_LINK_SCREEN);

    /* Saving it back unchanged gives the same 60 bytes: no record is burnt
     * by the update alone (the append log's "identical payload" elision). */
    device_config_t same = old;
    settings_store_capture(&same);
    CHECK(memcmp(&same, &old, sizeof old) == 0);
    if (memcmp(&same, &old, sizeof old)) diff_rec(&old, &same);

    /* "always" changes byte 11 and the checksum, nothing else. */
    meter_link_store(NULL, METER_LINK_ALWAYS);
    device_config_t al = old;
    settings_store_capture(&al);
    for (size_t i = 0; i < offsetof(device_config_t, checksum); i++) {
        const uint8_t x = ((const uint8_t *)&old)[i], y = ((const uint8_t *)&al)[i];
        if (i == 11u) CHECK(x == 0u && y == 1u);
        else CHECKF(x == y, "byte %zu moved with meter.link: 0x%02x -> 0x%02x", i, x, y);
    }
    CHECK(config_validate(&al));
    CHECK(al.checksum == old.checksum + 1u);

    /* and it comes back */
    meter_link_store(NULL, METER_LINK_SCREEN);
    settings_store_apply(&al);
    CHECK(prop_get(P(METER_LINK)) == METER_LINK_ALWAYS);
    settings_store_apply(&old);
    CHECK(prop_get(P(METER_LINK)) == METER_LINK_SCREEN);
}

/* ═══════════════════════════════════════════════════════════════════
 * The last screen (Startup on Boot = Restore): the former padding byte at
 * offset 22, and the new default of sys.startup
 * ═══════════════════════════════════════════════════════════════════ */

static device_mode_t boot_of(void)       /* the plain image's boot screen after a load */
{
    return startup_boot_mode((startup_mode_t)prop_get(P(SYS_STARTUP)), model_last_screen());
}

static void test_last_screen_record(void)
{
    /* The built-in default is Restore, and a default record names no
     * instrument: a unit without a record boots to Home. */
    CHECK(prop_default(P(SYS_STARTUP)) == STARTUP_RESTORE);
    device_config_t def;
    config_init_defaults(&def);
    CHECK(def.startup_mode == CONFIG_MODE_RESTORE && def.last_screen == STARTUP_SCREEN_NONE);
    model_last_screen_load(STARTUP_SCREEN_METER);
    settings_store_apply(&def);
    CHECK(model_last_screen() == STARTUP_SCREEN_NONE && boot_of() == MODE_HOME);

    /* A record from before the byte existed: byte 22 is 0 (padding, zeroed
     * by the defaults' memset and carried by every capture since) and its
     * startup choice is whatever was stored, the old default (meter) here.
     * The choice is kept; the last screen is unknown. */
    device_config_t old = def;
    old.startup_mode = CONFIG_MODE_MULTIMETER;
    old.checksum = config_compute_checksum(&old);
    CHECK(((const uint8_t *)&old)[22] == 0u);
    model_last_screen_load(STARTUP_SCREEN_SIGGEN);
    settings_store_apply(&old);
    CHECK(prop_get(P(SYS_STARTUP)) == STARTUP_METER && boot_of() == MODE_MULTIMETER);
    CHECK(model_last_screen() == STARTUP_SCREEN_NONE);
    /* ... and if its owner then picks Restore, the next boot is Home */
    CHECK(startup_boot_mode(STARTUP_RESTORE, model_last_screen()) == MODE_HOME);

    /* Saved again on any screen it is the same 60 bytes: a capture does not
     * look at the screen shown, so the update alone burns no record and a
     * screen change is not a change of the record. */
    static const device_mode_t every[5] = { MODE_OSCILLOSCOPE, MODE_MULTIMETER, MODE_SIGNAL_GEN,
                                            MODE_SETTINGS, MODE_HOME };
    for (unsigned i = 0; i < 5; i++) {
        current_mode = every[i];
        device_config_t same = old;
        settings_store_capture(&same);
        CHECK(memcmp(&same, &old, sizeof old) == 0);
        if (memcmp(&same, &old, sizeof old)) diff_rec(&old, &same);
    }

    /* A power-off on an instrument: byte 22 and the checksum move, nothing
     * else; the record loads back as that instrument and Restore boots into
     * it. */
    static const struct { device_mode_t mode; uint8_t code; } inst[3] = {
        { MODE_OSCILLOSCOPE, STARTUP_SCREEN_SCOPE }, { MODE_MULTIMETER, STARTUP_SCREEN_METER },
        { MODE_SIGNAL_GEN, STARTUP_SCREEN_SIGGEN },
    };
    for (unsigned k = 0; k < 3; k++) {
        settings_store_apply(&def);                  /* sys.startup = restore, no last screen */
        current_mode = inst[k].mode;
        device_config_t rec = def;
        settings_store_capture(&rec);                /* on the instrument, no power-off: nothing */
        CHECK(memcmp(&rec, &def, sizeof def) == 0);
        model_last_screen_note_power_off(startup_screen_of_mode(current_mode));
        current_mode = MODE_HOME;                    /* the byte is the one named, not the screen shown */
        settings_store_capture(&rec);
        for (size_t i = 0; i < offsetof(device_config_t, checksum); i++) {
            const uint8_t x = ((const uint8_t *)&def)[i], y = ((const uint8_t *)&rec)[i];
            if (i == 22u) CHECK(x == 0u && y == inst[k].code);
            else CHECKF(x == y, "byte %zu moved with the last screen: 0x%02x -> 0x%02x", i, x, y);
        }
        CHECK(config_validate(&rec));
        CHECK(rec.checksum == def.checksum + inst[k].code);

        /* the power-off did not move what the boot loaded (sys.lastscreen) */
        CHECK(model_last_screen() == STARTUP_SCREEN_NONE && model_last_screen_record() == inst[k].code);
        settings_store_apply(&rec);                  /* a new boot */
        CHECK(model_last_screen() == inst[k].code);
        CHECK(prop_get(P(SYS_LASTSCREEN)) == inst[k].code);
        CHECK(boot_of() == inst[k].mode);

        /* A record written for another reason on every other screen carries
         * that byte: only the changed field and the checksum move. */
        for (unsigned i = 0; i < 5; i++) {
            current_mode = every[i];
            device_config_t next = rec;
            meter_layout = (uint8_t)(1u + i % (METER_LAYOUT_COUNT - 1u));    /* never the 0 `rec` holds */
            settings_store_capture(&next);
            CHECK(next.last_screen == inst[k].code && next.meter_layout == meter_layout);
            CHECK(memcmp(&next, &rec, sizeof rec) != 0);
        }
        meter_layout = 0;
        settings_store_apply(&def);
        CHECK(model_last_screen() == STARTUP_SCREEN_NONE && model_last_screen_record() == STARTUP_SCREEN_NONE &&
              boot_of() == MODE_HOME);
    }
    /* Home and Settings name no instrument; a value out of range neither. */
    model_last_screen_note_power_off(STARTUP_SCREEN_METER);
    model_last_screen_note_power_off(startup_screen_of_mode(MODE_HOME));
    CHECK(model_last_screen_record() == STARTUP_SCREEN_NONE);
    model_last_screen_note_power_off(200);
    CHECK(model_last_screen_record() == STARTUP_SCREEN_NONE);

    /* Garbage in the byte (it passed the checksum): none, Home. */
    for (unsigned v = STARTUP_SCREEN_MAX + 1u; v < 256u; v++) {
        device_config_t g = def;
        g.last_screen = (uint8_t)v;
        g.checksum = config_compute_checksum(&g);
        settings_store_apply(&g);
        if (model_last_screen() != STARTUP_SCREEN_NONE || boot_of() != MODE_HOME) {
            CHECKF(0, "last screen byte %u loaded as %u", v, (unsigned)model_last_screen());
            break;
        }
    }
    checks++;

    /* A fixed choice ignores the byte. */
    device_config_t fx = def;
    fx.startup_mode = CONFIG_MODE_OSCILLOSCOPE;
    fx.last_screen = STARTUP_SCREEN_METER;
    fx.checksum = config_compute_checksum(&fx);
    settings_store_apply(&fx);
    CHECK(boot_of() == MODE_OSCILLOSCOPE);

    /* sys.lastscreen is read only: no setter, no store hook, not in a profile. */
    const prop_desc_t *ls = P(SYS_LASTSCREEN);
    CHECK((ls->flags & PF_RO) && !ls->set && !ls->store && !(ls->flags & PF_PERSIST));
    CHECK(model_persist_v5_find(PROP_ID_SYS_LASTSCREEN) == NULL);

    settings_store_apply(&def);
    current_mode = MODE_MULTIMETER;
}

/* ═══════════════════════════════════════════════════════════════════
 * view.mode and tb.roll (the display view): the former padding byte at
 * offset 23
 *
 * A load keeps the pair and moves no live state; the boot apply
 * (model_view_boot_apply here, model_boot_apply on the input task once the
 * ops are bound) sets it through the setters. Refused: Y-T without roll, and
 * the record keeps the view until the user changes it.
 * ═══════════════════════════════════════════════════════════════════ */

static bool g_view_fail, g_roll_fail;
static unsigned g_view_calls, g_roll_calls;
static bool fv_view(uint8_t v)
{
    g_view_calls++;
    if (g_view_fail && v != 0u) return false;       /* the pool: only leaving Y-T can fail */
    scope_view = (scope_view_t)v;
    return true;
}
static bool fv_roll(void) { g_roll_calls++; return !g_roll_fail; }
/* scope_apply_persist_on: a spectrum view goes back to Y-T for the pool */
static bool fv_persist_on(uint8_t m)
{
    (void)m;
    if (scope_view != SCOPE_VIEW_TIME && scope_view != SCOPE_VIEW_XY) scope_view = SCOPE_VIEW_TIME;
    persist_enabled = true;
    return true;
}
static void fv_persist_off(void) { persist_enabled = false; }
static const model_hw_ops_t k_view_ops = {
    .vdiv = f_ok_u8, .view_select = fv_view, .roll = fv_roll,
    .persist_on = fv_persist_on, .persist_off = fv_persist_off,
    .selfcal_busy = f_busy, .shell_persist = f_shell_persist, .note_change = f_note,
};

/* RAM after a power cycle (scope_state_init: Y-T, not rolling), then the
 * load of *rec as main() makes it before the scheduler. */
static void view_power_cycle(const device_config_t *rec)
{
    scope_view = SCOPE_VIEW_TIME;
    scope_state_get()->roll = false;
    persist_enabled = false;
    settings_store_apply(rec);
    g_notes = 0; g_view_calls = 0; g_roll_calls = 0;
}

static device_config_t view_rec_of(const device_config_t *base, uint8_t v)
{
    device_config_t r = *base;
    r.view_mode = v;
    r.checksum = config_compute_checksum(&r);
    return r;
}

/* The record's byte (the expectation, written out): 0 yt, 1 yt with roll,
 * 2 xy, 3 fft, 4 split, 5 waterfall, the encoding of the firmware that had
 * roll as a view, kept for every firmware since. */
static const struct { const char *name; uint8_t view; bool roll; } k_view_bytes[MODEL_VIEW_RECORD_MAX + 1u] = {
    { "yt", VIEW_MODE_YT, false }, { "yt+roll", VIEW_MODE_YT, true }, { "xy", VIEW_MODE_XY, false },
    { "fft", VIEW_MODE_FFT, false }, { "split", VIEW_MODE_SPLIT, false }, { "waterfall", VIEW_MODE_WATERFALL, false },
};

/* The user's choice of byte v: the view, then roll on where it has it. */
static bool view_choose(uint8_t v)
{
    if (prop_set_local(P(VIEW_MODE), k_view_bytes[v].view, ORIGIN_KEY, NULL) != PS_OK) return false;
    return !k_view_bytes[v].roll || prop_set_local(P(TB_ROLL), 1, ORIGIN_KEY, NULL) == PS_OK;
}

static void test_view_mode_record(void)
{
    static const char *const name[VIEW_MODE_COUNT] = { "yt", "xy", "fft", "split", "waterfall" };
    model_bind_hw(&k_view_ops);
    g_view_fail = g_roll_fail = false;
    g_real_note = false;
    for (unsigned v = 0; v < VIEW_MODE_COUNT; v++) CHECK(strcmp(P(VIEW_MODE)->labels[v], name[v]) == 0);
    CHECK(prop_default(P(VIEW_MODE)) == 0 && prop_default(P(TB_ROLL)) == 0);
    /* the pairs <-> the byte, both ways; roll counts only with a view that takes it */
    for (unsigned b = 0; b <= MODEL_VIEW_RECORD_MAX; b++)
        CHECKF(model_view_record_of(k_view_bytes[b].view, k_view_bytes[b].roll) == b, "%s", k_view_bytes[b].name);
    for (unsigned v = 1; v < VIEW_MODE_COUNT; v++)
        CHECK(model_view_record_of((int32_t)v, 1) == model_view_record_of((int32_t)v, 0));

    /* A record from before the field existed: byte 23 is 0 (padding, zeroed
     * by the defaults' memset and carried by every capture since). It loads
     * as yt without roll. */
    device_config_t old;
    ref_defaults(&old);
    CHECK(((const uint8_t *)&old)[23] == 0u && config_validate(&old));
    view_power_cycle(&old);
    CHECK(model_view_record() == 0u);
    CHECK(model_view_boot_apply());
    CHECK(prop_get(P(VIEW_MODE)) == 0 && prop_get(P(TB_ROLL)) == 0 && g_view_calls == 0 && g_roll_calls == 0 &&
          g_notes == 0);
    /* Saving it back unchanged gives the same 60 bytes: no record is burnt
     * by the update alone. */
    device_config_t same = old;
    settings_store_capture(&same);
    CHECK(memcmp(&same, &old, sizeof old) == 0);
    if (memcmp(&same, &old, sizeof old)) diff_rec(&old, &same);

    /* A load moves no live state, whatever view is live (the store hooks
     * would have written scope_view and the roll flag behind their owners). */
    for (unsigned v = 0; v <= MODEL_VIEW_RECORD_MAX; v++) {
        const device_config_t r = view_rec_of(&old, (uint8_t)v);
        scope_view = SCOPE_VIEW_SPLIT;
        scope_state_get()->roll = false;
        const int32_t live = prop_get(P(VIEW_MODE));
        settings_store_apply(&r);
        CHECK(prop_get(P(VIEW_MODE)) == live && !scope_state_get()->roll);
        CHECK(model_view_record() == v);
    }

    /* Each of the six bytes: chosen by the user, saved in byte 23 and nowhere
     * else, and back after a power cycle through the setters. Byte 1 is the
     * round trip of Y-T with roll on. */
    for (unsigned v = 0; v <= MODEL_VIEW_RECORD_MAX; v++) {
        const char *nm = k_view_bytes[v].name;
        view_power_cycle(&old);
        CHECK(model_view_boot_apply());
        CHECK(view_choose((uint8_t)v));
        CHECKF(g_notes == (v ? 1u : 0u), "%s: %u settings notes", nm, g_notes);   /* saved when it changes */
        device_config_t rec = old;
        settings_store_capture(&rec);
        for (size_t i = 0; i < offsetof(device_config_t, checksum); i++) {
            const uint8_t x = ((const uint8_t *)&old)[i], y = ((const uint8_t *)&rec)[i];
            if (i == 23u) CHECKF(y == v, "%s saved as %u", nm, y);
            else CHECKF(x == y, "byte %zu moved with the view: 0x%02x -> 0x%02x", i, x, y);
        }
        CHECK(config_validate(&rec) && rec.checksum == old.checksum + v);

        view_power_cycle(&rec);
        /* before the apply: Y-T on screen, no owner called, and a save made
         * in this window (any other setting) still writes the view */
        CHECK(prop_get(P(VIEW_MODE)) == 0 && prop_get(P(TB_ROLL)) == 0 && g_view_calls == 0 && g_roll_calls == 0);
        CHECK(model_view_record() == v);
        device_config_t early = rec;
        settings_store_capture(&early);
        CHECK(memcmp(&early, &rec, sizeof rec) == 0);

        CHECK(model_view_boot_apply());
        CHECKF(prop_get(P(VIEW_MODE)) == k_view_bytes[v].view, "%s came back as %ld", nm, (long)prop_get(P(VIEW_MODE)));
        CHECK(prop_get(P(TB_ROLL)) == k_view_bytes[v].roll && scope_state_get()->roll == k_view_bytes[v].roll);
        CHECK(g_roll_calls == (k_view_bytes[v].roll ? 1u : 0u));                 /* the FPGA is told about Roll */
        CHECK(g_view_calls == (k_view_bytes[v].view != VIEW_MODE_YT ? 1u : 0u)); /* the pool's owner is asked */
        CHECK(g_notes == 0);                                        /* a boot apply is not a change */
        CHECK(!model_view_boot_apply());                            /* once */
        device_config_t again = rec;
        settings_store_capture(&again);
        CHECK(memcmp(&again, &rec, sizeof rec) == 0);               /* and burns no record */
        if (memcmp(&again, &rec, sizeof rec)) diff_rec(&rec, &again);
    }

    /* A record an older firmware wrote with Roll chosen (byte 1, its
     * view.mode = roll): this one boots in Y-T with roll on, through the
     * general boot apply, as a power cycle runs it. */
    {
        const device_config_t r = view_rec_of(&old, 1);
        view_power_cycle(&r);
        CHECK(model_boot_kept(P(VIEW_MODE), NULL) && model_boot_kept(P(TB_ROLL), NULL));
        CHECK(model_boot_apply(NULL) == 0);
        CHECK(prop_get(P(VIEW_MODE)) == VIEW_MODE_YT && prop_get(P(TB_ROLL)) == 1 && g_roll_calls == 1 &&
              g_view_calls == 0 && g_notes == 0 && model_view_record() == 1u);
    }

    /* A self-cal run does not refuse the boot apply (ORIGIN_BOOT). */
    {
        const device_config_t r = view_rec_of(&old, 1);
        view_power_cycle(&r);
        g_selfcal = true;
        CHECK(model_view_boot_apply() && prop_get(P(TB_ROLL)) == 1);
        g_selfcal = false;
    }

    /* A byte no view has: yt without roll, and the record then says so (like
     * every other index field). */
    for (unsigned v = MODEL_VIEW_RECORD_MAX + 1u; v < 256; v++) {
        const device_config_t r = view_rec_of(&old, (uint8_t)v);
        view_power_cycle(&r);
        if (model_view_record() != 0u || !model_view_boot_apply() || prop_get(P(VIEW_MODE)) != 0 ||
            prop_get(P(TB_ROLL)) != 0 || g_view_calls || g_roll_calls) {
            CHECKF(0, "view byte %u loaded as %u", v, (unsigned)model_view_record());
            break;
        }
        device_config_t back = r;
        settings_store_capture(&back);
        if (back.view_mode != 0u) { CHECKF(0, "view byte %u saved back as %u", v, back.view_mode); break; }
    }
    checks++;

    /* Roll refused at boot (the acquisition park timed out): Y-T, the roll
     * flag not left set, and the record keeps roll through other saves. */
    {
        const device_config_t r = view_rec_of(&old, 1);
        view_power_cycle(&r);
        g_roll_fail = true;
        CHECK(!model_view_boot_apply());
        g_roll_fail = false;
        CHECK(prop_get(P(VIEW_MODE)) == 0 && !scope_state_get()->roll && g_notes == 0);
        CHECK(prop_set_local(P(CH1_VDIV), 7, ORIGIN_KEY, NULL) == PS_OK && g_notes == 1);
        device_config_t next = r;
        settings_store_capture(&next);
        CHECK(next.view_mode == 1u && next.scope_ch1_vdiv == 7u);
        /* the next boot shows it */
        view_power_cycle(&next);
        CHECK(model_view_boot_apply() && prop_get(P(TB_ROLL)) == 1);
        CHECK(prop_set_local(P(CH1_VDIV), prop_default(P(CH1_VDIV)), ORIGIN_KEY, NULL) == PS_OK);
    }

    /* The pool refused at boot (view_select says no): the same, for each
     * view that needs its owner. */
    for (unsigned v = 2; v <= MODEL_VIEW_RECORD_MAX; v++) {
        const device_config_t r = view_rec_of(&old, (uint8_t)v);
        view_power_cycle(&r);
        g_view_fail = true;
        CHECK(!model_view_boot_apply());
        CHECK(prop_get(P(VIEW_MODE)) == 0 && scope_view == SCOPE_VIEW_TIME && !scope_state_get()->roll);
        CHECK(g_notes == 0 && model_view_record() == v);
        device_config_t next = r;
        settings_store_capture(&next);
        CHECK(memcmp(&next, &r, sizeof r) == 0);            /* that boot alone destroys nothing */
        CHECK(!model_view_boot_apply() && model_view_record() == v);    /* still refused, still kept */
        g_view_fail = false;
        view_power_cycle(&next);
        CHECK(model_view_boot_apply() && prop_get(P(VIEW_MODE)) == k_view_bytes[v].view);
    }

    /* After a refused boot the user's own change of the view is what the
     * record holds from then on ... */
    {
        const device_config_t r = view_rec_of(&old, 5);
        /* ... another view */
        view_power_cycle(&r);
        g_view_fail = true;  CHECK(!model_view_boot_apply());  g_view_fail = false;
        CHECK(prop_set_local(P(VIEW_MODE), VIEW_MODE_FFT, ORIGIN_KEY, NULL) == PS_OK && g_notes == 1);
        CHECK(model_view_record() == 3u);
        /* ... a set that is refused changes nothing: still kept */
        view_power_cycle(&r);
        g_view_fail = true;  CHECK(!model_view_boot_apply());
        CHECK(prop_set_local(P(VIEW_MODE), VIEW_MODE_SPLIT, ORIGIN_KEY, NULL) == PS_NOT_SET && g_notes == 0);
        g_view_fail = false;
        CHECK(model_view_record() == 5u);
        /* ... Y-T chosen while Y-T is shown: the user's choice, no hardware */
        CHECK(prop_set_local(P(VIEW_MODE), VIEW_MODE_YT, ORIGIN_SHELL, NULL) == PS_OK);
        CHECK(model_view_record() == 0u);
        /* ... Roll through the timebase ladder: tb.roll is the byte's other
         * half, so its change replaces the kept view too, with its own note */
        view_power_cycle(&r);
        g_view_fail = true;  CHECK(!model_view_boot_apply());  g_view_fail = false;
        CHECK(prop_set_local(P(TB_ROLL), 1, ORIGIN_KEY, NULL) == PS_OK);
        CHECK(model_view_record() == 1u && g_notes == 1);
        CHECK(prop_set_local(P(TB_ROLL), 0, ORIGIN_KEY, NULL) == PS_OK);
        CHECK(model_view_record() == 0u && g_notes == 2);
        /* ... and after a refused Roll, the user's view: the kept roll goes too */
        const device_config_t rr = view_rec_of(&old, 1);
        view_power_cycle(&rr);
        g_roll_fail = true;  CHECK(!model_view_boot_apply());  g_roll_fail = false;
        CHECK(model_view_record() == 1u);
        CHECK(prop_set_local(P(VIEW_MODE), VIEW_MODE_YT, ORIGIN_KEY, NULL) == PS_OK);
        CHECK(model_view_record() == 0u && !model_boot_kept(P(TB_ROLL), NULL));
    }

    /* Persistence takes the pool from a spectrum view: view.mode goes back
     * to yt through view.persist's setter, and that is saved too. */
    {
        view_power_cycle(&old);
        CHECK(model_view_boot_apply());
        CHECK(prop_set_local(P(VIEW_MODE), VIEW_MODE_WATERFALL, ORIGIN_KEY, NULL) == PS_OK && g_notes == 1);
        CHECK(prop_set_local(P(VIEW_PERSIST), 2, ORIGIN_KEY, NULL) == PS_OK);
        CHECK(prop_get(P(VIEW_MODE)) == 0 && model_view_record() == 0u);
        CHECKF(g_notes == 2, "%u notes: the view persistence evicted was not noted", g_notes);
        CHECK(prop_set_local(P(VIEW_PERSIST), 0, ORIGIN_KEY, NULL) == PS_OK && g_notes == 2);
        /* in X-Y nothing is evicted: no note */
        CHECK(prop_set_local(P(VIEW_MODE), VIEW_MODE_XY, ORIGIN_KEY, NULL) == PS_OK && g_notes == 3);
        CHECK(prop_set_local(P(VIEW_PERSIST), 1, ORIGIN_KEY, NULL) == PS_OK);
        CHECK(prop_get(P(VIEW_MODE)) == VIEW_MODE_XY && g_notes == 3);
        CHECK(prop_set_local(P(VIEW_PERSIST), 0, ORIGIN_KEY, NULL) == PS_OK);
    }

    /* Reset to defaults after a refused boot: the view is on its default
     * already, so the reset loop never calls its setter; the kept view goes
     * all the same, or the reset's own save would write it back. */
    {
        const device_config_t r = view_rec_of(&old, 5);
        view_power_cycle(&r);
        g_view_fail = true;
        (void)model_view_boot_apply();                      /* refused */
        CHECK(prop_get(P(VIEW_MODE)) == 0 && model_view_record() == 5u);
        (void)model_reset_to_defaults(NULL, NULL);
        g_view_fail = false;
        CHECK(prop_get(P(VIEW_MODE)) == 0 && model_view_record() == 0u);
        CHECK(prop_reset_keeps(P(VIEW_MODE)) == NULL && prop_reset_keeps(P(TB_ROLL)) == NULL);
    }
    /* ... and roll in force goes off through its setter */
    {
        view_power_cycle(&old);
        CHECK(model_view_boot_apply());
        CHECK(prop_set_local(P(TB_ROLL), 1, ORIGIN_KEY, NULL) == PS_OK);
        (void)model_reset_to_defaults(NULL, NULL);
        CHECK(prop_get(P(VIEW_MODE)) == 0 && !scope_state_get()->roll && model_view_record() == 0u);
    }

    view_power_cycle(&old);
    (void)model_view_boot_apply();
    model_bind_hw(NULL);
}

int main(void)
{
    scope_state_init(scope_state_get());
    theme_init(THEME_DARK_BLUE);
    meas_select_defaults();
    siggen_init(NULL);

    test_table_coverage();
    test_defaults_identical();
    test_random_states_capture_identical();
    test_random_records_apply_identical();
    test_every_byte_value_of_every_one_byte_field();
    test_round_trip_through_properties();
    test_out_of_range_policy();
    test_stored_only_raw_carry();
    test_fft_and_lang_codec();
    test_notes_by_origin();
    test_refused_commit_no_note();
    test_meter_link_record();
    test_last_screen_record();
    test_view_mode_record();

    printf("test_model_persist: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
