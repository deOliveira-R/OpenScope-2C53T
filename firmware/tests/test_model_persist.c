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
 * bytes (sys.lang, fft.*, display_persist_mode, padding) carried through a
 * load/save cycle and saved once set; table coverage (every PF_PERSIST
 * property has one row, every row a persisted property or a const one, every
 * record byte is one row's or padding); the session-only list; settings
 * notes by origin (key, shell with persist on/off, boot, load).
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
 * ═══════════════════════════════════════════════════════════════════ */
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
    cfg->startup_mode = CONFIG_MODE_MULTIMETER;

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
    uint8_t  theme, math_en, math_op, meter_sub, meter_lay, startup;
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
    s->startup   = (uint8_t)startup_mode;
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
    startup_mode  = (startup_mode_t)s->startup;
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
    ss->roll = R(2); ss->roll_step = (uint8_t)R(9); ss->trig_x = (int16_t)R(300);
    ss->cursor.mode = (cursor_mode_t)R(CURSOR_MODE_COUNT);
    meas_select_set(0, (uint16_t)rnd());
    meas_select_set(1, (uint16_t)rnd());
    theme_set((theme_id_t)R(THEME_COUNT));
    math_enabled  = R(2);
    math_op       = (uint8_t)R(MATH_COUNT);
    meter_submode = (uint8_t)R(METER_SUBMODE_COUNT);
    meter_layout  = (uint8_t)R(METER_LAYOUT_COUNT);
    startup_mode  = (startup_mode_t)R(STARTUP_COUNT);
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
        } else {
            CHECK(f->codec == V5_CONST || f->codec == V5_CARRY);
        }
        for (unsigned k = 0; k < f->size; k++) {
            CHECKF(f->offset + k < sizeof owner, "row %u past the record", r);
            if (f->offset + k < sizeof owner) {
                CHECKF(owner[f->offset + k] == 0, "byte %u in two rows", f->offset + k);
                owner[f->offset + k] = 1;
            }
        }
    }
    /* every payload byte between the header and the checksum: one row's, or
     * the struct's padding (11, 22, 23) */
    for (unsigned b = offsetof(device_config_t, scope_ch1_vdiv); b < offsetof(device_config_t, checksum); b++) {
        const bool pad = (b == 11u || b == 22u || b == 23u);
        CHECKF(owner[b] == (pad ? 0 : 1), "record byte %u %s", b, pad ? "is padding but owned" : "has no row");
    }
    CHECK(offsetof(device_config_t, scope_trigger_level) == 12u);
    CHECK(offsetof(device_config_t, siggen_frequency) == 24u);

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

    /* and every persisted property then reads its built-in default */
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
        settings_store_apply(&rec);
        for (uint16_t i = 0; i < prop_count(); i++) {
            const prop_desc_t *p = prop_at(i);
            if (!(p->flags & PF_PERSIST)) continue;
            int32_t w = want[i];
            /* gen.out is saved only with gen.bootout on */
            if (p->id == PROP_ID_GEN_OUT && !want[PROP_IX_GEN_BOOTOUT]) w = 0;
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
    static const unsigned carried[] = { 11, 18, 19, 20, 22, 23, 37, 38 };   /* pad, fft x3, pad x2, persist mode, lang */
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
    device_config_t rec;
    config_init_defaults(&rec);
    rec.language = 7; rec.fft_window = 9; rec.fft_avg_count = 3;
    ((uint8_t *)&rec)[offsetof(device_config_t, fft_max_hold)] = 0x40;   /* a bool field: poke the byte */
    settings_store_apply(&rec);
    CHECK(prop_get(P(SYS_LANG)) == 0 && prop_get(P(FFT_WINDOW)) == 1 && prop_get(P(FFT_AVG)) == 0);
    CHECK(prop_get(P(FFT_MAXHOLD)) == 1);
    /* ... one it does understands reads back ... */
    rec.fft_window = 3; rec.fft_avg_count = 16;
    settings_store_apply(&rec);
    CHECK(prop_get(P(FFT_WINDOW)) == 3 && prop_get(P(FFT_AVG)) == 4);
    /* ... and a set is saved, the other carried bytes untouched */
    CHECK(prop_set_local(P(FFT_WINDOW), 2, ORIGIN_KEY, NULL) == PS_OK);
    CHECK(prop_set_local(P(FFT_AVG), 3, ORIGIN_KEY, NULL) == PS_OK);
    device_config_t next = rec;
    settings_store_capture(&next);
    CHECK(next.fft_window == 2 && next.fft_avg_count == 8);
    CHECKF(next.language == 7 && ((uint8_t *)&next)[20] == 0x40, "lang %u maxhold 0x%02x", next.language, ((uint8_t *)&next)[20]);
    CHECK(config_validate(&next));
}

/* The stored-only rows decode and encode by their own tables: the
 * properties have no RAM variable before M3, so the snapshot comparisons
 * above never see them. Every byte of sys.lang, fft.window and fft.avg_count
 * against the documented mapping (props_sys.c labels; fft.avg is an index
 * into the counts 0/2/4/8/16/32), and every value back. */
static void test_stored_only_codec(void)
{
    static const uint8_t counts[6] = { 0u, 2u, 4u, 8u, 16u, 32u };   /* lbl_fft_avg: off x2 .. x32 */
    const v5_field_t *fw = model_persist_v5_find(PROP_ID_FFT_WINDOW);
    const v5_field_t *fa = model_persist_v5_find(PROP_ID_FFT_AVG);
    const v5_field_t *fl = model_persist_v5_find(PROP_ID_SYS_LANG);
    CHECK(fw && fa && fl);
    if (!fw || !fa || !fl) return;
    CHECK(fw->offset == offsetof(device_config_t, fft_window) &&
          fa->offset == offsetof(device_config_t, fft_avg_count) &&
          fl->offset == offsetof(device_config_t, language));
    unsigned bad = 0;
    for (unsigned b = 0; b < 256u; b++) {
        device_config_t rec;
        config_init_defaults(&rec);
        rec.fft_window = (uint8_t)b; rec.fft_avg_count = (uint8_t)b; rec.language = (uint8_t)b;
        const int32_t want_w = b <= 4u ? (int32_t)b : 1;              /* rect..flattop, else Hanning */
        int32_t want_a = 0;                                            /* not a count: off */
        for (int32_t i = 0; i < 6; i++) if (counts[i] == b) want_a = i;
        const int32_t want_l = 0;                                      /* English, the only one */
        if (model_persist_v5_decode(fw, &rec) != want_w) bad++;
        if (model_persist_v5_decode(fa, &rec) != want_a) bad++;
        if (model_persist_v5_decode(fl, &rec) != want_l) bad++;
        /* the load puts the same into the properties */
        settings_store_apply(&rec);
        if (prop_get(P(FFT_WINDOW)) != want_w || prop_get(P(FFT_AVG)) != want_a ||
            prop_get(P(SYS_LANG)) != want_l) bad++;
    }
    CHECKF(bad == 0, "%u stored-only bytes decoded wrong", bad);
    /* every value: captured as its record byte, and read back */
    for (int32_t v = 0; v <= 5; v++) {
        device_config_t rec;
        config_init_defaults(&rec);
        rec.fft_window = 0xEE; rec.fft_avg_count = 0xEE; rec.language = 0xEE;   /* nothing understood */
        settings_store_apply(&rec);
        if (v <= 4) CHECK(prop_set_local(P(FFT_WINDOW), v, ORIGIN_KEY, NULL) == PS_OK);
        CHECK(prop_set_local(P(FFT_AVG), v, ORIGIN_KEY, NULL) == PS_OK);
        device_config_t next = rec;
        settings_store_capture(&next);
        /* the value the byte already decodes to (the default) is carried as
         * the byte; any other is written as its record byte */
        CHECKF(next.fft_avg_count == (v == 0 ? 0xEE : counts[v]), "fft.avg %ld saved as %u", (long)v,
               next.fft_avg_count);
        CHECK(model_persist_v5_decode(fa, &next) == v);
        if (v <= 4) {
            CHECKF(next.fft_window == (v == 1 ? 0xEE : (uint8_t)v), "fft.window %ld saved as %u", (long)v,
                   next.fft_window);
            CHECK(model_persist_v5_decode(fw, &next) == v);
        }
        CHECK(next.language == 0xEE);                  /* unchanged property: the byte is carried */
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
    test_stored_only_codec();
    test_notes_by_origin();
    test_refused_commit_no_note();

    printf("test_model_persist: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
