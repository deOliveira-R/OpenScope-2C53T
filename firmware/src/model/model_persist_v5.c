/*
 * OpenScope 2C53T - the v5 settings record, generated from the property table
 * (see model_persist_v5.h).
 *
 * Out-of-range policy on load, field by field, as settings_store.c applied it
 * before this table (and as tests/test_model_persist.c pins against a frozen
 * copy of that code). No field rejects the record: a record is rejected only
 * by config.c (magic, version, checksum, size; a v4 record is upgraded).
 *
 *   chN.vdiv, tb.code, trig.mode/edge/source, chN.coupling/probe, sys.theme,
 *   meter.func/layout, gen.wave, gen.duty, sys.volume  -> default
 *   chN.coupling accepts 2 (GND) on load, outside the property's 0..1
 *   trig.level                                         -> saturate at +-103
 *                                                         (the property's
 *                                                         range is +-100)
 *   chN.bwlimit, gen.bootout                           -> any non-zero = on
 *   chN.meas                                           -> bits above 12 dropped
 *   view.math                                          -> enable = non-zero,
 *                                                         op >= 8 -> 0
 *   gen.freq  (float)   NaN, < 0, > 50000 -> default 1000; else rounded
 *   gen.amp   (float)   x10 + 0.5 NaN, < 0, > 31 -> default 30; else
 *                       truncated, then 31 -> 30
 *   gen.out             on only with gen.bootout on and a non-zero byte
 *   sys.startup         0 scope, 2 siggen, 4 home, anything else meter
 *   sys.brightness      0 or > 100 -> 100, < 25 -> 25, else down to a 5 step
 *   sys.autooff         minutes other than 0/15/30/60 -> off
 *   sys.brightness/volume/autooff: without sys_prefs_mark 0xC1 -> 100 / 1 / off
 *   chN.enabled         ignored: always on after a load (session state)
 *   sys.lang, fft.*     out of range -> default in the property, byte carried
 *   meter.link          the former padding byte at offset 11: 0 in every older
 *                       record = screen (the default); anything above 1 -> default
 */
#include "model_persist_v5.h"
#include "model.h"
#include "scope_state.h"
#include "signal_gen.h"
#include "sys_prefs.h"
#include "startup_policy.h"
#include "math_channel.h"
#include "theme.h"
#include "ui.h"
#include "meter_link_policy.h"

#include <stddef.h>
#include <string.h>

/* The load limits are today's, from the firmware's own enums. */
_Static_assert(VDIV_COUNT == 10 && TIMEBASE_COUNT == 21, "vdiv 0..9, timebase 0..20");
_Static_assert(COUPLING_COUNT == 3 && PROBE_COUNT == 2 && TRIG_COUNT == 3 &&
               TRIG_EDGE_COUNT == 2 && TRIG_SRC_COUNT == 2, "scope enums (load limits)");
_Static_assert(THEME_COUNT == 4 && MATH_COUNT == 8 && METER_SUBMODE_COUNT == 12 &&
               METER_LAYOUT_COUNT == 5, "theme / math / meter enums (load limits)");
_Static_assert(SIGGEN_WAVEFORM_COUNT == 13 && SIGGEN_FREQ_MAX_HZ == 50000u &&
               SIGGEN_AMP_MAX_DV == 30u && SIGGEN_DUTY_MAX == 100u, "generator limits");
_Static_assert(SYS_BRIGHT_MIN == 25u && SYS_BRIGHT_MAX == 100u && SYS_VOL_MAX == 10u &&
               SYS_AUTOOFF_COUNT == 4u, "sys_prefs limits");
/* Two-byte rows. */
_Static_assert(offsetof(device_config_t, math_op) == offsetof(device_config_t, math_enabled) + 1u,
               "V5_MATH_PAIR: math_op follows math_enabled");
_Static_assert(sizeof(device_config_t) == 60u, "the v5 record is 60 bytes");

#define TRIGGER_LEVEL_LIMIT  103    /* settings_store.c's clamp before M3 */

#define OFF(f)  ((uint8_t)offsetof(device_config_t, f))
#define SZ(f)   ((uint8_t)sizeof(((device_config_t *)0)->f))
/*        property id               field                 codec          load              flags        lo    hi */
#define ROW(id, f, codec, load, flags, lo, hi) { (id), OFF(f), SZ(f), (codec), (load), (flags), (lo), (hi) }

const v5_field_t model_persist_v5_fields[] = {
    ROW(PROP_ID_CH1_VDIV,     scope_ch1_vdiv,       V5_U8,        V5_LOAD_DEFAULT,  0, 0, VDIV_COUNT - 1),
    ROW(PROP_ID_CH2_VDIV,     scope_ch2_vdiv,       V5_U8,        V5_LOAD_DEFAULT,  0, 0, VDIV_COUNT - 1),
    ROW(PROP_ID_TB_CODE,      scope_timebase,       V5_U8,        V5_LOAD_DEFAULT,  0, 0, TIMEBASE_COUNT - 1),
    ROW(PROP_ID_TRIG_MODE,    scope_trigger_mode,   V5_U8,        V5_LOAD_DEFAULT,  0, 0, TRIG_COUNT - 1),
    ROW(PROP_ID_TRIG_EDGE,    scope_trigger_edge,   V5_U8,        V5_LOAD_DEFAULT,  0, 0, TRIG_EDGE_COUNT - 1),
    ROW(PROP_ID_TRIG_LEVEL,   scope_trigger_level,  V5_I16,       V5_LOAD_SATURATE, 0, -TRIGGER_LEVEL_LIMIT, TRIGGER_LEVEL_LIMIT),
    ROW(PROP_ID_CH1_COUPLING, scope_ch1_coupling,   V5_U8,        V5_LOAD_DEFAULT,  0, 0, COUPLING_COUNT - 1),
    ROW(PROP_ID_CH2_COUPLING, scope_ch2_coupling,   V5_U8,        V5_LOAD_DEFAULT,  0, 0, COUPLING_COUNT - 1),
    ROW(PROP_ID_CH1_ENABLED,  scope_ch1_enabled,    V5_CONST,     V5_LOAD_EXACT,    0, 1, 1),
    ROW(PROP_ID_CH2_ENABLED,  scope_ch2_enabled,    V5_CONST,     V5_LOAD_EXACT,    0, 1, 1),
    ROW(PROP_ID_FFT_WINDOW,   fft_window,           V5_U8,        V5_LOAD_DEFAULT,  V5_F_STORED, 0, 4),
    ROW(PROP_ID_FFT_AVG,      fft_avg_count,        V5_FFT_COUNT, V5_LOAD_DEFAULT,  V5_F_STORED, 0, 32),
    ROW(PROP_ID_FFT_MAXHOLD,  fft_max_hold,         V5_BOOL8,     V5_LOAD_EXACT,    V5_F_STORED, 0, 1),
    ROW(PROP_ID_GEN_WAVE,     siggen_waveform,      V5_U8,        V5_LOAD_DEFAULT,  0, 0, SIGGEN_WAVEFORM_COUNT - 1),
    ROW(PROP_ID_GEN_FREQ,     siggen_frequency,     V5_F32_INT,   V5_LOAD_DEFAULT,  0, 0, SIGGEN_FREQ_MAX_HZ),
    ROW(PROP_ID_GEN_AMP,      siggen_amplitude,     V5_F32_X10,   V5_LOAD_DEFAULT,  0, 0, SIGGEN_AMP_MAX_DV),
    ROW(PROP_ID_GEN_DUTY,     siggen_duty,          V5_U8,        V5_LOAD_DEFAULT,  0, 0, SIGGEN_DUTY_MAX),
    ROW(PROP_ID_GEN_OUT,      siggen_enabled,       V5_GEN_OUT,   V5_LOAD_EXACT,    0, 0, 1),
    ROW(PROP_ID_GEN_BOOTOUT,  siggen_start_on,      V5_BOOL8,     V5_LOAD_EXACT,    0, 0, 1),
    ROW(0,                    sys_prefs_mark,       V5_CONST,     V5_LOAD_EXACT,    0, CONFIG_SYS_PREFS_MARK, CONFIG_SYS_PREFS_MARK),
    ROW(PROP_ID_SYS_BRIGHTNESS, display_brightness, V5_U8,        V5_LOAD_BRIGHT,   V5_F_PREFS, SYS_BRIGHT_MIN, SYS_BRIGHT_MAX),
    ROW(0,                    display_persist_mode, V5_CARRY,     V5_LOAD_EXACT,    0, 0, 0),
    ROW(PROP_ID_SYS_LANG,     language,             V5_U8,        V5_LOAD_DEFAULT,  V5_F_STORED, 0, 0),
    ROW(PROP_ID_SYS_STARTUP,  startup_mode,         V5_STARTUP,   V5_LOAD_DEFAULT,  0, 0, CONFIG_MODE_HOME),
    ROW(PROP_ID_SYS_AUTOOFF,  auto_shutdown_mins,   V5_MINUTES,   V5_LOAD_DEFAULT,  V5_F_PREFS, 0, 60),
    ROW(PROP_ID_SYS_VOLUME,   sound_enabled,        V5_U8,        V5_LOAD_DEFAULT,  V5_F_PREFS, 0, SYS_VOL_MAX),
    ROW(PROP_ID_SYS_THEME,    theme,                V5_U8,        V5_LOAD_DEFAULT,  0, 0, THEME_COUNT - 1),
    ROW(PROP_ID_CH1_PROBE,    scope_ch1_probe,      V5_U8,        V5_LOAD_DEFAULT,  0, 0, PROBE_COUNT - 1),
    ROW(PROP_ID_CH2_PROBE,    scope_ch2_probe,      V5_U8,        V5_LOAD_DEFAULT,  0, 0, PROBE_COUNT - 1),
    ROW(PROP_ID_CH1_BWLIMIT,  scope_ch1_bw_limit,   V5_BOOL8,     V5_LOAD_EXACT,    0, 0, 1),
    ROW(PROP_ID_CH2_BWLIMIT,  scope_ch2_bw_limit,   V5_BOOL8,     V5_LOAD_EXACT,    0, 0, 1),
    ROW(PROP_ID_TRIG_SOURCE,  scope_trigger_source, V5_U8,        V5_LOAD_DEFAULT,  0, 0, TRIG_SRC_COUNT - 1),
    { PROP_ID_VIEW_MATH, OFF(math_enabled), 2u, V5_MATH_PAIR, V5_LOAD_DEFAULT, 0, 0, MATH_COUNT - 1 },
    ROW(PROP_ID_METER_FUNC,   meter_submode,        V5_U8,        V5_LOAD_DEFAULT,  0, 0, METER_SUBMODE_COUNT - 1),
    ROW(PROP_ID_METER_LAYOUT, meter_layout,         V5_U8,        V5_LOAD_DEFAULT,  0, 0, METER_LAYOUT_COUNT - 1),
    ROW(PROP_ID_METER_LINK,   meter_link,           V5_U8,        V5_LOAD_DEFAULT,  0, 0, METER_LINK_POLICY_COUNT - 1),
    ROW(PROP_ID_CH1_MEAS,     meas_ch1_mask,        V5_MASK16,    V5_LOAD_EXACT,    0, 0, 0x0FFF),
    ROW(PROP_ID_CH2_MEAS,     meas_ch2_mask,        V5_MASK16,    V5_LOAD_EXACT,    0, 0, 0x0FFF),
};
const unsigned model_persist_v5_field_count =
    (unsigned)(sizeof model_persist_v5_fields / sizeof model_persist_v5_fields[0]);

#undef ROW
#undef SZ
#undef OFF

/* Record units <-> property values. */
static const uint8_t k_autooff_mins[SYS_AUTOOFF_COUNT] = { 0u, 15u, 30u, 60u };   /* lbl_sys_autooff */
static const uint8_t k_fft_counts[6] = { 0u, 2u, 4u, 8u, 16u, 32u };            /* lbl_fft_avg     */

const v5_field_t *model_persist_v5_find(uint16_t prop_id)
{
    for (unsigned i = 0; i < model_persist_v5_field_count; i++)
        if (prop_id && model_persist_v5_fields[i].prop_id == prop_id) return &model_persist_v5_fields[i];
    return NULL;
}

/* ── byte access (memcpy: no alignment or bool-representation assumptions) ── */

static uint8_t  rd_u8(const device_config_t *c, unsigned off) { return ((const uint8_t *)c)[off]; }
static int16_t  rd_i16(const device_config_t *c, unsigned off) { int16_t v; memcpy(&v, (const uint8_t *)c + off, 2); return v; }
static uint16_t rd_u16(const device_config_t *c, unsigned off) { uint16_t v; memcpy(&v, (const uint8_t *)c + off, 2); return v; }
static float    rd_f32(const device_config_t *c, unsigned off) { float v; memcpy(&v, (const uint8_t *)c + off, 4); return v; }
static void wr_u8(device_config_t *c, unsigned off, uint8_t v)   { ((uint8_t *)c)[off] = v; }
static void wr_i16(device_config_t *c, unsigned off, int16_t v)  { memcpy((uint8_t *)c + off, &v, 2); }
static void wr_u16(device_config_t *c, unsigned off, uint16_t v) { memcpy((uint8_t *)c + off, &v, 2); }
static void wr_f32(device_config_t *c, unsigned off, float v)    { memcpy((uint8_t *)c + off, &v, 4); }

static int32_t index_of(const uint8_t *t, unsigned n, uint8_t v)
{
    for (unsigned i = 0; i < n; i++)
        if (t[i] == v) return (int32_t)i;
    return -1;
}

/* ── load ─────────────────────────────────────────────────────────────── */

int32_t model_persist_v5_decode(const v5_field_t *f, const device_config_t *cfg)
{
    const prop_desc_t *p = prop_by_id(f->prop_id);
    if (!p) return 0;
    const int32_t def = prop_default(p);
    if ((f->flags & V5_F_PREFS) && rd_u8(cfg, offsetof(device_config_t, sys_prefs_mark)) != CONFIG_SYS_PREFS_MARK)
        return def;

    int32_t r;
    switch (f->codec) {
    case V5_U8:      r = rd_u8(cfg, f->offset); break;
    case V5_I16:     r = rd_i16(cfg, f->offset); break;
    case V5_BOOL8:   return rd_u8(cfg, f->offset) != 0u;
    case V5_MASK16:  return (int32_t)(rd_u16(cfg, f->offset) & (uint16_t)f->hi);
    case V5_CONST:   return f->lo;
    case V5_STARTUP: return (int32_t)startup_mode_from_config(rd_u8(cfg, f->offset));
    case V5_GEN_OUT:
        return (rd_u8(cfg, offsetof(device_config_t, siggen_start_on)) != 0u &&
                rd_u8(cfg, f->offset) != 0u) ? 1 : 0;
    case V5_MINUTES: {
        const int32_t i = index_of(k_autooff_mins, SYS_AUTOOFF_COUNT, rd_u8(cfg, f->offset));
        return i < 0 ? def : i;
    }
    case V5_FFT_COUNT: {
        const int32_t i = index_of(k_fft_counts, sizeof k_fft_counts, rd_u8(cfg, f->offset));
        return i < 0 ? def : i;
    }
    case V5_F32_INT: {       /* NaN fails both compares */
        const float v = rd_f32(cfg, f->offset);
        return (v >= 0.0f && v <= (float)f->hi) ? (int32_t)(uint32_t)(v + 0.5f) : def;
    }
    case V5_F32_X10: {
        const float a = rd_f32(cfg, f->offset) * 10.0f + 0.5f;
        if (!(a >= 0.0f && a <= (float)f->hi + 1.0f)) return def;
        const int32_t v = (int32_t)(uint8_t)a;
        return v > f->hi ? f->hi : v;
    }
    default:
        return 0;           /* V5_MATH_PAIR, V5_CARRY: not one value */
    }

    switch (f->load) {
    case V5_LOAD_SATURATE: return r < f->lo ? f->lo : r > f->hi ? f->hi : r;
    case V5_LOAD_BRIGHT:   return (int32_t)sys_bright_clamp((uint8_t)r);
    case V5_LOAD_DEFAULT:  return (r >= f->lo && r <= f->hi) ? r : def;
    default:               return r;
    }
}

/* The generator's load hook: one siggen_init(), as at every boot before M3. */
static void gen_put(siggen_config_t *sg, uint16_t id, int32_t v)
{
    switch (id) {
    case PROP_ID_GEN_WAVE:    sg->waveform       = (uint8_t)v;  break;
    case PROP_ID_GEN_FREQ:    sg->frequency_hz   = (uint32_t)v; break;
    case PROP_ID_GEN_AMP:     sg->amplitude_dv   = (uint8_t)v;  break;
    case PROP_ID_GEN_DUTY:    sg->duty_pct       = (uint8_t)v;  break;
    case PROP_ID_GEN_OUT:     sg->output_enabled = (v != 0);    break;
    case PROP_ID_GEN_BOOTOUT: sg->start_on       = (v != 0);    break;
    default: break;
    }
}

void model_persist_v5_apply(const device_config_t *cfg)
{
    if (!cfg) return;
    siggen_config_t sg;
    memset(&sg, 0, sizeof sg);
    bool gen = false;

    for (unsigned i = 0; i < model_persist_v5_field_count; i++) {
        const v5_field_t *f = &model_persist_v5_fields[i];
        const prop_desc_t *p = prop_by_id(f->prop_id);
        if (!p || f->codec == V5_CARRY) continue;
        if (f->codec == V5_MATH_PAIR) {
            const uint8_t op = rd_u8(cfg, f->offset + 1u);
            math_enabled = (rd_u8(cfg, f->offset) != 0u);
            math_op      = (op <= (uint8_t)f->hi) ? op : 0u;
            continue;
        }
        const int32_t v = model_persist_v5_decode(f, cfg);
        if (p->group == PG_GEN) { gen_put(&sg, f->prop_id, v); gen = true; continue; }
        if (p->store) p->store(p, v);
    }
    if (gen) siggen_init(&sg);
}

/* ── capture and defaults ─────────────────────────────────────────────── */

static void encode(const v5_field_t *f, device_config_t *cfg, int32_t v, int32_t bootout)
{
    switch (f->codec) {
    case V5_U8:        wr_u8(cfg, f->offset, (uint8_t)v); break;
    case V5_BOOL8:     wr_u8(cfg, f->offset, v ? 1u : 0u); break;
    case V5_I16:       wr_i16(cfg, f->offset, (int16_t)v); break;
    case V5_MASK16:    wr_u16(cfg, f->offset, (uint16_t)v); break;
    case V5_CONST:     wr_u8(cfg, f->offset, (uint8_t)f->lo); break;
    case V5_STARTUP:   wr_u8(cfg, f->offset, startup_mode_to_config((startup_mode_t)v)); break;
    case V5_MINUTES:   wr_u8(cfg, f->offset, (v >= 0 && v < (int32_t)SYS_AUTOOFF_COUNT) ? k_autooff_mins[v] : 0u); break;
    case V5_FFT_COUNT: wr_u8(cfg, f->offset, (v >= 0 && v < (int32_t)sizeof k_fft_counts) ? k_fft_counts[v] : 0u); break;
    case V5_F32_INT:   wr_f32(cfg, f->offset, (float)(uint32_t)v); break;
    case V5_F32_X10:   wr_f32(cfg, f->offset, (float)v / 10.0f); break;
    /* The enable is a saved setting only while the user wants it restored;
     * with the boot policy off it is session state (no record per Run/Stop). */
    case V5_GEN_OUT:   wr_u8(cfg, f->offset, (bootout && v) ? 1u : 0u); break;
    case V5_MATH_PAIR: wr_u8(cfg, f->offset, v ? 1u : 0u);
                       wr_u8(cfg, f->offset + 1u, (uint8_t)(v ? v - 1 : 0)); break;
    default: break;     /* V5_CARRY */
    }
}

void model_persist_v5_defaults(device_config_t *cfg)
{
    if (!cfg) return;
    const int32_t bootout = prop_default(P(GEN_BOOTOUT));
    for (unsigned i = 0; i < model_persist_v5_field_count; i++) {
        const v5_field_t *f = &model_persist_v5_fields[i];
        const prop_desc_t *p = prop_by_id(f->prop_id);
        encode(f, cfg, p ? prop_default(p) : 0, bootout);
    }
}

void model_persist_v5_capture(device_config_t *cfg)
{
    if (!cfg) return;
    const int32_t bootout = prop_get(P(GEN_BOOTOUT));
    for (unsigned i = 0; i < model_persist_v5_field_count; i++) {
        const v5_field_t *f = &model_persist_v5_fields[i];
        const prop_desc_t *p = prop_by_id(f->prop_id);
        if (f->codec == V5_CARRY) continue;
        if (f->codec == V5_MATH_PAIR) {          /* the variables, not the composite */
            wr_u8(cfg, f->offset, math_enabled ? 1u : 0u);
            wr_u8(cfg, f->offset + 1u, math_op);
            continue;
        }
        const int32_t v = p ? prop_get(p) : 0;
        if ((f->flags & V5_F_STORED) && v == model_persist_v5_decode(f, cfg)) continue;   /* carry */
        encode(f, cfg, v, bootout);
    }
}
