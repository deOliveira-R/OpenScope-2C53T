/*
 * Host test for the session snapshot (model/model_session.h,
 * util/session_store.h) and the boot apply it rides on (model.h, "VALUES A
 * LOAD KEEPS FOR THE SETTERS").
 *
 * Startup on Boot = Restore brings every instrument back as an orderly
 * power-off left it: the snapshot holds every settable property that is not
 * on the exclusion list, in a flash log of its own, and the input task sets
 * the values through the normal setters. What this file holds, on the real
 * model, codec, settings store, config and region layer over a NOR model of
 * the whole W25Q, with fake hardware ops that log and can refuse:
 *
 *   - membership: every settable property is in the snapshot or in the
 *     exclusion list with a reason, never both, and the list is exact;
 *   - the format: the firmware's encoder byte for byte against an encoder
 *     written here from the format's description; the size bound;
 *   - save -> power cycle -> load -> apply over random states reached
 *     through the setters: every member is back, view.run is on, hold and
 *     relative are off, the generator output follows gen.bootout alone, no
 *     calibration op and no mode switch is called;
 *   - the fallbacks: no snapshot, a tombstone, a bad checksum, garbage, a
 *     record too long, another format version, a stale snapshot (the
 *     settings record moved after it, or was not written with it): each is
 *     the SAME state, value for value, as a boot of the same settings
 *     record with a blank session log, i.e. what Restore did before;
 *   - other tables: unknown ids skipped, excluded ids skipped, values out
 *     of range skipped (not clamped), a property the snapshot lacks left
 *     alone;
 *   - a refused apply (an owner, a rule) does not stop the others, leaves
 *     the property where the settings record put it, is counted and listed
 *     by `session status`; a refusal that passes is retried once;
 *   - the apply order is table order whatever the record's order is;
 *   - Startup on Boot other than Restore does not even read the log;
 *   - Reset to defaults invalidates the snapshot, and writes nothing to the
 *     session log when there is none; BUSY leaves it;
 *   - flash: one record per save, none otherwise (boot, sets, settings
 *     saves), the settings log's bytes untouched by a save or a clear, the
 *     log compacting in its own sector, and no program or erase outside the
 *     two logs, ever.
 *
 * Build/run:  make -C firmware test-session
 */
#define MODEL_INTERNAL
#include "model.h"
#include "model_persist_v5.h"
#include "config.h"
#include "settings_store.h"
#include "session_store.h"
#include "model_session.h"
#include "view_table.h"
#include "model_shell.h"
#include "flash_regions.h"
#include "scope_state.h"
#include "meas_select.h"
#include "signal_gen.h"
#include "sys_prefs.h"
#include "theme.h"
#include "math_channel.h"
#include "startup_policy.h"
#include "ui.h"

#include <stdarg.h>
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
volatile device_mode_t  current_mode  = MODE_SETTINGS;
volatile startup_mode_t startup_mode  = STARTUP_RESTORE;
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

void dac_output_apply(const siggen_timing_t *t) { (void)t; }
void dac_output_stop(void) {}
static uint16_t g_dac_buf[SIGGEN_CYCLE_SAMPLES];
uint16_t *dac_output_get_buffer(void) { return g_dac_buf; }
uint32_t dac_output_timer_clock_hz(void) { return 240000000u; }

/* ═══════════════════════════════════════════════════════════════════
 * NOR model of the W25Q (the semantics of tests/test_config_persist.c's),
 * with a guard: every program and erase is checked, as it happens, against
 * the two logs this code may write: the settings log and the session log.
 * ═══════════════════════════════════════════════════════════════════ */

#define NOR_SIZE FLASH_REGION_CHIP_SIZE
static uint8_t *nor;
/* programs / erases per log: [0] settings, [1] session; anything else is
 * nor_outside. Reads of the session log are counted too. */
static uint32_t nor_programs[2], nor_erases[2], nor_outside, nor_session_reads;

static bool in_region(flash_region_id_t id, uint32_t addr, uint32_t len)
{
    const flash_region_t *r = &flash_region_table[id];
    return addr >= r->start && len <= r->length && addr - r->start <= r->length - len;
}
static int which(uint32_t addr, uint32_t len)
{
    if (in_region(FLASH_REGION_SETTINGS, addr, len)) return 0;
    if (in_region(FLASH_REGION_SESSION, addr, len) ||
        in_region(FLASH_REGION_SESSION_B, addr, len)) return 1;
    return -1;
}

static int nor_read(void *ctx, uint32_t addr, void *buf, uint32_t len)
{
    (void)ctx;
    if (addr >= NOR_SIZE || len > NOR_SIZE - addr) { printf("NOR: read out of range\n"); exit(3); }
    if (which(addr, len) == 1) nor_session_reads++;
    memcpy(buf, nor + addr, len);
    return 0;
}

static int nor_erase(void *ctx, uint32_t addr)
{
    (void)ctx;
    if (addr % FLASH_REGION_SECTOR_SIZE || addr >= NOR_SIZE) { printf("NOR: bad erase\n"); exit(3); }
    const int w = which(addr, FLASH_REGION_SECTOR_SIZE);
    if (w < 0) { nor_outside++; printf("  NOR: ERASE OUTSIDE THE TWO LOGS at 0x%06X\n", (unsigned)addr); }
    else nor_erases[w]++;
    memset(nor + addr, 0xFF, FLASH_REGION_SECTOR_SIZE);
    return 0;
}

static int nor_program(void *ctx, uint32_t addr, const void *data, uint32_t len)
{
    (void)ctx;
    const uint8_t *src = data;
    if (addr >= NOR_SIZE || len > NOR_SIZE - addr) { printf("NOR: program out of range\n"); exit(3); }
    const int w = which(addr, len);
    if (w < 0) {
        nor_outside++;
        printf("  NOR: PROGRAM OUTSIDE THE TWO LOGS at 0x%06X+%u\n", (unsigned)addr, (unsigned)len);
    } else nor_programs[w]++;
    for (uint32_t i = 0; i < len; i++) nor[addr + i] &= src[i];
    return 0;
}

static const flash_region_backend_t nor_backend = {
    .read = nor_read, .erase_sector = nor_erase, .program = nor_program, .ctx = NULL,
};
flash_region_status_t flash_regions_bind_w25q(void) { return flash_regions_init(&nor_backend); }

/* Content in every byte the two logs do not own: the factory-cal backup,
 * the user calibration log, the gallery, the logo, the firmware cache, the
 * stock volumes, the tail, modules. A different byte per region. The two
 * logs are blank, as on a device that never saved. */
static uint8_t *nor_snap;
static bool is_log(uint32_t i)
{
    return i == FLASH_REGION_SETTINGS || i == FLASH_REGION_SESSION || i == FLASH_REGION_SESSION_B;
}
static void nor_fill_everything_else(void)
{
    memset(nor, 0xFF, NOR_SIZE);
    for (uint32_t i = 0; i < FLASH_REGION_COUNT; i++) {
        const flash_region_t *r = &flash_region_table[i];
        if (is_log(i)) continue;
        memset(nor + r->start, (int)(0x41u + i), r->length);
    }
}
static void nor_take_snapshot(void) { memcpy(nor_snap, nor, NOR_SIZE); }
/* Every region but the two logs is exactly as the snapshot left it. */
static bool everything_else_intact(void)
{
    for (uint32_t i = 0; i < FLASH_REGION_COUNT; i++) {
        const flash_region_t *r = &flash_region_table[i];
        if (!is_log(i) && memcmp(nor + r->start, nor_snap + r->start, r->length) != 0) return false;
    }
    return true;
}
static const uint8_t *region_bytes(flash_region_id_t id) { return nor + flash_region_table[id].start; }
static uint32_t region_len(flash_region_id_t id) { return flash_region_table[id].length; }

/* ═══ fake hardware ops (the log is what the hardware was told) ═════ */
static char     g_log[8192];
static bool     g_busy;
static bool     g_fail_coupling;
static bool     g_il_en = true;
static int32_t  g_il_gain = 1000000, g_il_off = 0, g_il_skew = 0;
static uint8_t  g_il_knots;
static uint8_t  g_tolv = 8, g_tolh = 2, g_pmode;
static bool     g_stopfail = true;
static bool     g_flag[3];
static uint32_t g_now = 1000u;
static unsigned g_notes;

static void logf_(const char *fmt, ...)
{
    char t[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    strncat(g_log, t, sizeof g_log - strlen(g_log) - 1);
}
static bool logged(const char *what) { return strstr(g_log, what) != NULL; }
static unsigned logged_times(const char *what)
{
    unsigned n = 0;
    for (const char *s = g_log; (s = strstr(s, what)) != NULL; s += strlen(what)) n++;
    return n;
}

static bool f_vdiv(uint8_t ch, uint8_t i)        { logf_("vdiv(%u,%u);", ch, i); return true; }
static bool f_coupling(uint8_t ch, uint8_t c)    { logf_("coupling(%u,%u);", ch, c); return !g_fail_coupling; }
static int      g_fail_enables;                   /* > 0: that many calls refuse; < 0: every call */
static bool f_enables(void)
{
    logf_("enables;");
    if (g_fail_enables > 0) { g_fail_enables--; return false; }
    return g_fail_enables == 0;
}
static bool f_il_active(void)                    { return false; }
static bool f_vpos(uint8_t ch, int16_t p)        { logf_("vpos(%u,%d);", ch, p); return true; }
static bool f_timebase(uint8_t c)                { logf_("tb(%u);", c); return true; }
static bool f_roll(void)                         { logf_("roll;"); return true; }
static bool f_trig_src(void)                     { logf_("trigsrc;"); return true; }
static bool f_trig_lvl(int16_t l)                { logf_("triglvl(%d);", l); return true; }
static void f_rearm(void)                        { logf_("rearm;"); }
static bool     g_fail_view;                      /* the shared pool says no (not reset by boot()) */
static bool f_view(uint8_t v)
{
    logf_("view(%u);", v);
    if (g_fail_view && v != 0u) return false;
    scope_view = (scope_view_t)v;
    return true;
}
static bool     g_fail_pers;                      /* no pool for persistence */
static bool f_pers_on(uint8_t m)
{
    logf_("pers_on(%u);", m);
    if (g_fail_pers) return false;
    persist_enabled = true; g_pmode = m;
    return true;
}
static void f_pers_off(void)                     { logf_("pers_off;"); persist_enabled = false; g_pmode = 0; }
static uint8_t f_pers_mode(void)                 { return g_pmode; }
static bool f_mask_present(void)                 { return true; }
static void f_mask_get(uint8_t *v, uint8_t *h, bool *s) { *v = g_tolv; *h = g_tolh; *s = g_stopfail; }
static bool f_mask_tol(uint8_t v, uint8_t h)     { logf_("mask_tol(%u,%u);", v, h); g_tolv = v; g_tolh = h; return true; }
static bool f_mask_sf(bool on)                   { logf_("mask_sf(%d);", on); g_stopfail = on; return true; }
static void f_gen_wave(uint8_t w)                { logf_("gen_wave(%u);", w); siggen_set_waveform(w); }
static void f_gen_freq(uint32_t f)               { logf_("gen_freq(%u);", (unsigned)f); siggen_set_frequency(f); }
static void f_gen_amp(uint8_t a)                 { logf_("gen_amp(%u);", a); siggen_set_amplitude_dv(a); }
static void f_gen_duty(percent_t d)              { logf_("gen_duty(%u);", d.pct); siggen_set_duty(d.pct); }
static void f_gen_out(bool on)                   { logf_("gen_out(%d);", on); siggen_enable(on); }
static void f_gen_boot(bool on)                  { logf_("gen_boot(%d);", on); siggen_set_start_on(on); }
static bool f_meter(uint8_t m, bool keep)        { logf_("meter(%u,%d);", m, keep); return true; }
static bool f_meter_link_on(void)                { return false; }
static bool f_flag_get(uint8_t w)                { return w < 3 && g_flag[w]; }
static bool f_flag_set(uint8_t w, bool on, bool user) { (void)user; logf_("flag(%u,%d);", w, on); if (w < 3) g_flag[w] = on; return true; }
static void f_meter_layout(uint8_t l)            { logf_("layout(%u);", l); }
static bool f_mode(uint8_t m)                    { logf_("mode(%u);", m); current_mode = (device_mode_t)m; return true; }
static void f_backlight(uint8_t p)               { logf_("backlight(%u);", p); }
static void f_autooff(uint8_t m)                 { logf_("autooff(%u);", m); }
static bool f_il_en(bool on)                     { logf_("il_en(%d);", on); g_il_en = on; return true; }
static bool f_il_enabled(void)                   { return g_il_en; }
/* like fpga_interleave_corr_set: the line replaces the map, knots and all */
static bool f_il_corr_set(gain_e6_t g, int32_t o){ logf_("il_corr(%ld,%ld);", (long)g.e6, (long)o); g_il_gain = g.e6; g_il_off = o; g_il_knots = 0; return true; }
static uint8_t f_il_knots(void)                  { return g_il_knots; }
static void f_il_corr_get(gain_e6_t *g, int32_t *o) { g->e6 = g_il_gain; *o = g_il_off; }
static bool f_il_skew_set(ppm_period_t s)        { logf_("il_skew(%ld);", (long)s.ppm); g_il_skew = s.ppm; return true; }
static ppm_period_t f_il_skew_get(void)          { ppm_period_t s = { g_il_skew }; return s; }
static bool f_busy(void)                         { return g_busy; }
static bool f_shell_persist(void)                { return true; }
static void f_note(void)                         { g_notes++; settings_store_note_change(g_now); }

static const model_hw_ops_t k_fake = {
    .vdiv = f_vdiv, .coupling = f_coupling, .channel_enables = f_enables,
    .interleave_active = f_il_active, .vpos = f_vpos, .timebase = f_timebase, .roll = f_roll,
    .trigger_source = f_trig_src, .trigger_level = f_trig_lvl, .single_rearm = f_rearm,
    .view_select = f_view, .persist_on = f_pers_on, .persist_off = f_pers_off,
    .persist_mode = f_pers_mode, .mask_present = f_mask_present, .mask_get = f_mask_get,
    .mask_tol = f_mask_tol, .mask_stopfail = f_mask_sf,
    .gen_wave = f_gen_wave, .gen_freq = f_gen_freq, .gen_amp = f_gen_amp, .gen_duty = f_gen_duty,
    .gen_out = f_gen_out, .gen_bootout = f_gen_boot, .meter_func = f_meter,
    .meter_link_on = f_meter_link_on, .meter_flag_get = f_flag_get, .meter_flag_set = f_flag_set,
    .meter_layout = f_meter_layout, .mode = f_mode,
    .backlight = f_backlight, .autooff = f_autooff, .il_enable = f_il_en, .il_enabled = f_il_enabled,
    .il_corr_set = f_il_corr_set, .il_corr_get = f_il_corr_get, .il_knots = f_il_knots,
    .il_skew_set = f_il_skew_set, .il_skew_get = f_il_skew_get, .selfcal_busy = f_busy,
    .shell_persist = f_shell_persist, .note_change = f_note,
};
static unsigned g_between;
static void between(void) { g_between++; }

/* ═══ power cycle, power-off ═════════════════════════════════════════ */

/* RAM as main() leaves it before the settings load (flash survives, RAM
 * does not). The device sits in Settings. */
static void ram_reset(void)
{
    model_bind_hw(NULL);
    scope_state_init(scope_state_get());
    meas_select_defaults();
    siggen_init(NULL);
    sys_prefs_set_brightness(SYS_BRIGHT_DEFAULT);
    sys_prefs_set_volume(SYS_VOL_DEFAULT);
    sys_prefs_set_autooff_mins(0);
    theme_init(THEME_DARK_BLUE);
    current_mode = MODE_SETTINGS; startup_mode = STARTUP_RESTORE; active_channel = 0;
    meter_submode = 0; meter_layout = 0; math_enabled = false; math_op = 0;
    persist_enabled = false; scope_view = SCOPE_VIEW_TIME; g_pmode = 0;
    fuse_type = 0; fuse_rating_idx = 6; fuse_view = 0; fuse_scan_threshold_mv = 0.1f;
    g_busy = false; g_fail_coupling = false;
    g_il_en = true; g_il_gain = 1000000; g_il_off = 0; g_il_skew = 0; g_il_knots = 0;
    g_tolv = 8; g_tolh = 2; g_stopfail = true;
    memset(g_flag, 0, sizeof g_flag);
    model_bind_hw(&k_fake);
    (void)model_load_defaults(PF_SESSION, NULL, false);    /* the session properties' initialisers */
    model_boot_reset();
    current_mode = MODE_SETTINGS;
}

/* main() up to the scheduler: the settings load, which loads the snapshot. */
static void boot_load(void)
{
    ram_reset();
    (void)settings_store_load_and_apply();
    g_log[0] = '\0';
    g_between = 0;
    g_notes = 0;
}
/* ... and the input task's first step (model_fw_start). The log holds what
 * the apply told the hardware. */
static void boot(void)
{
    boot_load();
    (void)model_boot_apply(between);
}

static void fresh_device(void)
{
    nor_fill_everything_else();
    memset(nor_programs, 0, sizeof nor_programs);
    memset(nor_erases, 0, sizeof nor_erases);
    nor_outside = 0;
    config_persist_stats_reset();
    flash_regions_stats_reset();
    g_fail_view = false; g_fail_enables = 0; g_fail_pers = false;
    if (flash_regions_bind_w25q() != FLASH_REGION_OK) { printf("FATAL: region layer\n"); exit(2); }
    boot();
    nor_take_snapshot();
}

static void key_pressed(void)      /* what every handled key ends with (input_handler.c) */
{
    g_now += 100u;
    settings_store_note_change(g_now);
    (void)settings_store_service(g_now);
}
static void settle(void)           /* ... and the settle window gone by: a changed record is written */
{
    g_now += SETTINGS_STORE_SETTLE_MS + 100u;
    key_pressed();
}

/* The orderly power-off as input_handler.c's power_off_now() makes it once
 * the session line is in: the settings flush that notes the screen, then
 * the snapshot. */
static bool power_off(void)
{
    g_now += 100u;
    (void)settings_store_flush_power_off(g_now, startup_screen_of_mode(current_mode));
    return session_snapshot_save();
}

static bool set(const prop_desc_t *p, int32_t v)
{
    const prop_status_t s = prop_set_local(p, v, ORIGIN_KEY, NULL);
    return s == PS_OK || s == PS_CLAMPED;
}

/* ═══ state vectors, random states ═══════════════════════════════════ */

#define NPROP PROP_IX_COUNT
static void take(int32_t *v) { for (uint16_t i = 0; i < NPROP; i++) v[i] = prop_get(prop_at(i)); }
static unsigned differ(const int32_t *a, const int32_t *b, bool say)
{
    unsigned n = 0;
    for (uint16_t i = 0; i < NPROP; i++) {
        if (a[i] == b[i]) continue;
        if (say) printf("    %s: %ld != %ld\n", prop_at(i)->name, (long)a[i], (long)b[i]);
        n++;
    }
    return n;
}

static uint32_t g_rng = 0x2C53A7u;
static uint32_t R(uint32_t n)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return n ? g_rng % n : 0u;
}
static int32_t random_value(const prop_desc_t *p)
{
    switch (p->type) {
    case PT_BOOL: return (int32_t)R(2);
    case PT_MASK: return (int32_t)(R((uint32_t)p->max + 1u) & (uint32_t)p->max);
    case PT_INT: {
        const uint32_t steps = (uint32_t)(p->max - p->min) / (p->step ? p->step : 1u);
        return p->min + (int32_t)(R(steps + 1u) * (p->step ? p->step : 1u));
    }
    default:      return p->min + (int32_t)R((uint32_t)(p->max - p->min) + 1u);
    }
}

/* A user's session: n sets of random properties to random values through
 * the setters (refusals are part of it), so the state is one the device can
 * be in. Calibration is left alone (it is not a session's to move). */
static void random_session(unsigned n)
{
    for (unsigned k = 0; k < n; k++) {
        const prop_desc_t *p = prop_at((uint16_t)R(NPROP));
        if (!prop_is_settable(p) || (p->flags & PF_USERCAL) || p->id == PROP_ID_IL_KNOTS) continue;
        (void)prop_set_local(p, random_value(p), ORIGIN_KEY, NULL);
        if (R(4) == 0) key_pressed();
        g_log[0] = '\0';
    }
}

/* What a Restore boot must show for a state `was` that was powered off. */
static unsigned check_restored(const int32_t *was, const char *what)
{
    unsigned bad = 0;
    for (uint16_t i = 0; i < NPROP; i++) {
        const prop_desc_t *p = prop_at(i);
        const int32_t now = prop_get(p);
        int32_t want;
        if (!prop_is_settable(p)) continue;
        if (!model_session_excluded(p)) want = was[i];
        else switch (p->id) {
        case PROP_ID_VIEW_RUN:    want = 1; break;
        case PROP_ID_METER_HOLD:
        case PROP_ID_METER_REL:   want = 0; break;
        case PROP_ID_GEN_OUT:     want = (was[PROP_IX_GEN_OUT] && was[PROP_IX_GEN_BOOTOUT]) ? 1 : 0; break;
        default: continue;       /* calibration and the screen binding: not the snapshot's */
        }
        if (now != want) {
            bad++;
            checks++; fails++;
            printf("FAIL %s: %s is %ld, expected %ld (was %ld)\n", what, p->name, (long)now, (long)want,
                   (long)was[i]);
        }
    }
    return bad;
}

/* ═══ the format, written again from its description ═════════════════ */

typedef struct { uint16_t id; int32_t v; } ent_t;

static uint32_t t_crc32(const uint8_t *d, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int b = 0; b < 8; b++) c = (c & 1u) ? (c >> 1) ^ 0xEDB88320u : (c >> 1);
    }
    return ~c;
}
static uint16_t craft(uint8_t *b, uint8_t version, uint8_t flags, uint16_t seq, uint32_t stamp,
                      const ent_t *e, uint16_t n)
{
    uint16_t o = 0;
    b[o++] = 0x53; b[o++] = 0x4E;                       /* "SN" */
    b[o++] = version; b[o++] = flags;
    b[o++] = (uint8_t)seq; b[o++] = (uint8_t)(seq >> 8);
    b[o++] = (uint8_t)n;   b[o++] = (uint8_t)(n >> 8);
    for (int k = 0; k < 4; k++) b[o++] = (uint8_t)(stamp >> (8 * k));
    for (uint16_t i = 0; i < n; i++) {
        b[o++] = (uint8_t)e[i].id; b[o++] = (uint8_t)(e[i].id >> 8);
        uint32_t z = e[i].v < 0 ? (((uint32_t)~(uint32_t)e[i].v) << 1) | 1u : ((uint32_t)e[i].v << 1);
        while (z >= 0x80u) { b[o++] = (uint8_t)(z | 0x80u); z >>= 7; }
        b[o++] = (uint8_t)z;
    }
    const uint32_t c = t_crc32(b, o);
    for (int k = 0; k < 4; k++) b[o++] = (uint8_t)(c >> (8 * k));
    return o;
}

/* The live members as entries, table order. */
static uint16_t live_entries(ent_t *e)
{
    uint16_t n = 0;
    for (uint16_t i = 0; i < NPROP; i++) {
        const prop_desc_t *p = prop_at(i);
        if (model_session_excluded(p)) continue;
        e[n].id = p->id; e[n].v = prop_get(p); n++;
    }
    return n;
}
static ent_t *ent_find(ent_t *e, uint16_t n, uint16_t id)
{
    for (uint16_t i = 0; i < n; i++) if (e[i].id == id) return &e[i];
    printf("FATAL: no entry %04X\n", id); exit(2);
}

static uint8_t  g_rec[1024];
static uint16_t newest(model_session_info_t *info)     /* the newest record of the session log */
{
    uint32_t n = 0;
    if (flash_region_read_latest(FLASH_REGION_SESSION, g_rec, sizeof g_rec, &n) != FLASH_REGION_OK) return 0;
    if (info) (void)model_session_parse(g_rec, (uint16_t)(n <= MODEL_SESSION_MAX_BYTES ? n : 0u), info);
    return (uint16_t)n;
}
static void append_raw(const uint8_t *b, uint32_t n)   /* as another firmware, or damage, would */
{
    if (flash_region_append(FLASH_REGION_SESSION, b, n) != FLASH_REGION_OK) { printf("FATAL: append\n"); exit(2); }
}
static uint32_t log_records(void)
{
    uint32_t recs = 0;
    (void)session_snapshot_log_info(NULL, NULL, &recs);
    return recs;
}

/* The two logs, kept and put back: one settings record + one snapshot can
 * be booted many ways. */
static uint8_t g_logs[0x10000u + 0x1000u + 0x1000u];
static void logs_keep(void)
{
    memcpy(g_logs, region_bytes(FLASH_REGION_SETTINGS), region_len(FLASH_REGION_SETTINGS));
    memcpy(g_logs + 0x10000u, region_bytes(FLASH_REGION_SESSION), region_len(FLASH_REGION_SESSION));
    memcpy(g_logs + 0x11000u, region_bytes(FLASH_REGION_SESSION_B), region_len(FLASH_REGION_SESSION_B));
}
static void logs_back(void)
{
    memcpy(nor + flash_region_table[FLASH_REGION_SETTINGS].start, g_logs, region_len(FLASH_REGION_SETTINGS));
    memcpy(nor + flash_region_table[FLASH_REGION_SESSION].start, g_logs + 0x10000u, region_len(FLASH_REGION_SESSION));
    memcpy(nor + flash_region_table[FLASH_REGION_SESSION_B].start, g_logs + 0x11000u, region_len(FLASH_REGION_SESSION_B));
}
static void session_log_blank(void)
{
    memset(nor + flash_region_table[FLASH_REGION_SESSION].start, 0xFF, region_len(FLASH_REGION_SESSION));
    memset(nor + flash_region_table[FLASH_REGION_SESSION_B].start, 0xFF, region_len(FLASH_REGION_SESSION_B));
}

static char g_out[4096];
static void emit(const char *line) { strncat(g_out, line, sizeof g_out - strlen(g_out) - 2); strcat(g_out, "\n"); }
static const char *status_text(void) { g_out[0] = '\0'; model_shell_session_status(emit); return g_out; }

/* A session that moved things only the snapshot holds. */
static void a_session(void)
{
    CHECK(set(P(CH2_ENABLED), 0));
    CHECK(set(P(CH1_POS), 40));
    CHECK(set(P(TRIG_X), 96));
    CHECK(set(P(TRIG_SOFT), 0));
    CHECK(set(P(VIEW_CURSOR), 3));
    CHECK(set(P(VIEW_CURSEL), 3));
    CHECK(set(P(VIEW_CURT1), -987654321));  /* a five-byte varint */
    CHECK(set(P(VIEW_PERSIST), 2));
    CHECK(set(P(VIEW_UPPER), 2));          /* split's pair turned over: FFT over Y-T, restored in table order */
    CHECK(set(P(METER_BEEP), 3));
    CHECK(set(P(METER_DEGF), 1));
    CHECK(set(P(MASK_TOLV), 20));
    CHECK(set(P(CH1_VDIV), 6));            /* a persisted one too */
    key_pressed();
}

/* ═══ tests ══════════════════════════════════════════════════════════ */

static void test_membership(void)
{
    printf("membership\n");
    unsigned members = 0, excluded = 0, ro = 0;
    for (uint16_t i = 0; i < NPROP; i++) {
        const prop_desc_t *p = prop_at(i);
        const char *why = model_session_excluded(p);
        bool listed = false;
        for (uint8_t k = 0; k < model_session_exclusion_count; k++)
            if (model_session_exclusions[k].id == p->id) listed = true;
        if (!prop_is_settable(p)) {
            ro++;
            CHECKF(why && !listed, "%s is read only: never saved, and not a list entry", p->name);
            continue;
        }
        /* settable: in the snapshot, or on the list with a reason. Never both, never neither. */
        CHECKF((why == NULL) != listed, "%s is neither in the snapshot nor on the list (or both)", p->name);
        if (why) { excluded++; CHECKF(strlen(why) >= 8u, "%s: a reason, not a word", p->name); }
        else members++;
        /* what the flags already say must not be in a snapshot */
        if (p->flags & (PF_USERCAL | PF_ALIAS)) CHECKF(why != NULL, "%s is calibration or an alias", p->name);
        /* a member is a setting someone saves somewhere */
        if (!why) CHECKF(p->flags & (PF_PERSIST | PF_SESSION), "%s", p->name);
    }
    /* the list is exact: every entry is a settable property, once */
    for (uint8_t k = 0; k < model_session_exclusion_count; k++) {
        const prop_desc_t *p = prop_by_id(model_session_exclusions[k].id);
        CHECKF(p && prop_is_settable(p), "exclusion %u names no settable property", k);
        for (uint8_t j = 0; j < k; j++)
            CHECK(model_session_exclusions[j].id != model_session_exclusions[k].id);
    }
    CHECK(excluded == model_session_exclusion_count);
    CHECK(members == model_session_member_count() && members + excluded + ro == NPROP);
    /* the ones the design names */
    static const uint16_t out[] = { PROP_ID_IL_GAIN, PROP_ID_IL_OFFSET, PROP_ID_IL_SKEW, PROP_ID_IL_KNOTS,
        PROP_ID_GEN_OUT, PROP_ID_METER_HOLD, PROP_ID_METER_REL, PROP_ID_VIEW_RUN, PROP_ID_SYS_INSTRUMENT,
        PROP_ID_SYS_MODE };
    for (unsigned k = 0; k < sizeof out / sizeof out[0]; k++) CHECK(model_session_excluded(prop_by_id(out[k])));
    static const uint16_t in[] = { PROP_ID_CH1_ENABLED, PROP_ID_CH2_ENABLED, PROP_ID_CH1_POS, PROP_ID_VIEW_MODE,
        PROP_ID_VIEW_PERSIST, PROP_ID_TB_ROLLSTEP, PROP_ID_TRIG_X, PROP_ID_VIEW_CURSOR, PROP_ID_GEN_BOOTOUT,
        PROP_ID_GEN_FREQ, PROP_ID_METER_FUNC, PROP_ID_METER_DEGF, PROP_ID_MASK_TOLV, PROP_ID_IL_ENABLED,
        PROP_ID_SYS_STARTUP, PROP_ID_SYS_BRIGHTNESS, PROP_ID_TB_ROLL,
        PROP_ID_VIEW_CURT1, PROP_ID_VIEW_CURT2, PROP_ID_VIEW_CURR1, PROP_ID_VIEW_CURR2,
        PROP_ID_VIEW_CURY1, PROP_ID_VIEW_CURY2,     /* the time cursors, in their units */
        PROP_ID_VIEW_UPPER, PROP_ID_VIEW_LOWER,     /* split's pair: how the user left split */
        PROP_ID_VIEW_MEASHIDE, PROP_ID_VIEW_SPLITHIDE };   /* the results, full views' and split's */
    for (unsigned k = 0; k < sizeof in / sizeof in[0]; k++) CHECK(!model_session_excluded(prop_by_id(in[k])));
    printf("  %u properties: %u in the snapshot, %u left out with a reason, %u read only\n",
           (unsigned)NPROP, members, excluded, ro);
    g_out[0] = '\0';
    model_shell_session_members(emit);
    CHECK(strstr(g_out, "gen.out") && strstr(g_out, "boot policy gen.bootout") && strstr(g_out, "ch2.enabled"));
    /* the widest snapshot this table can make fits the buffer and a log record */
    CHECK(MODEL_SESSION_HDR_BYTES + members * MODEL_SESSION_ENTRY_MAX_BYTES + 4u <= MODEL_SESSION_MAX_BYTES);
    CHECK(MODEL_SESSION_MAX_BYTES <= FLASH_REGION_RECORD_MAX);
}

static void test_format(void)
{
    printf("format\n");
    static uint8_t a[MODEL_SESSION_MAX_BYTES], b[MODEL_SESSION_MAX_BYTES];
    static ent_t e[NPROP];
    fresh_device();
    for (int round = 0; round < 200; round++) {
        random_session(40);
        const uint16_t n = live_entries(e);
        const uint16_t la = model_session_encode(a, sizeof a, (uint16_t)(round * 331), 0xC0FFEE00u + (uint32_t)round);
        const uint16_t lb = craft(b, 1, 0, (uint16_t)(round * 331), 0xC0FFEE00u + (uint32_t)round, e, n);
        CHECK(la == lb && la != 0 && memcmp(a, b, la) == 0);
        model_session_info_t info;
        CHECK(model_session_parse(a, la, &info) == MODEL_SESSION_OK);
        CHECK(info.count == n && info.seq == (uint16_t)(round * 331) && info.version == 1 && info.flags == 0);
        /* any one flipped bit is caught */
        const uint16_t at = (uint16_t)R(la);
        a[at] ^= (uint8_t)(1u << R(8));
        CHECK(model_session_parse(a, la, &info) != MODEL_SESSION_OK);
        /* and a cut */
        CHECK(model_session_parse(b, (uint16_t)(lb - 1u), &info) == MODEL_SESSION_BAD);
    }
    /* negative and wide values */
    const ent_t wide[] = { { 0x0106, -100 }, { 0x0602, 50000 }, { 0x7001, INT32_MAX }, { 0x7002, INT32_MIN },
                           { 0x7003, -1 }, { 0x7004, 63 }, { 0x7005, 64 }, { 0x7006, -64 }, { 0x7007, -65 } };
    const uint16_t lw = craft(a, 1, 0, 1, 0, wide, 9);
    model_session_info_t info;
    CHECK(model_session_parse(a, lw, &info) == MODEL_SESSION_OK && info.count == 9);
    CHECK(model_session_encode(a, 20, 1, 0) == 0);                  /* does not fit: nothing */
    CHECK(model_session_encode_cleared(a, sizeof a, 7) == 16);
    CHECK(model_session_parse(a, 16, &info) == MODEL_SESSION_CLEARED && info.seq == 7);
    CHECK(model_session_parse(a, 15, &info) == MODEL_SESSION_BAD);
    CHECK(model_session_parse(NULL, 16, &info) == MODEL_SESSION_BAD);
    const uint16_t l = model_session_encode(a, sizeof a, 1, 0);
    printf("  a snapshot of today's table: %u bytes for %u values (limit %u)\n", l,
           (unsigned)model_session_member_count(), (unsigned)MODEL_SESSION_MAX_BYTES);
}

static void test_round_trip(void)
{
    printf("round trip\n");
    static int32_t was[NPROP];
    fresh_device();
    unsigned bad = 0, sets = 0;
    const int rounds = 400;
    for (int round = 0; round < rounds; round++) {
        random_session(60 + R(120));
        CHECK(set(P(SYS_STARTUP), STARTUP_RESTORE));
        take(was);
        const uint32_t recs = log_records(), er = nor_erases[1];
        const uint32_t moves = session_snapshot_status()->compactions;
        CHECK(power_off());
        CHECK(session_snapshot_status()->compactions == moves
                  ? (nor_erases[1] == er && log_records() == recs + 1u)
                  : (nor_erases[1] <= er + 1u && log_records() == 1u));
        boot();
        const session_status_t *st = session_snapshot_status();
        CHECKF(st->load == SESSION_LOAD_OK, "round %d: %s", round, session_load_text(st->load));
        CHECK(st->kept == model_session_member_count() && !st->unknown && !st->excluded && !st->out_of_range);
        CHECKF(model_boot_report()->refused == 0, "round %d: %u refused, first %04X", round,
               model_boot_report()->refused, model_boot_report()->first_refused);
        bad += check_restored(was, "round trip");
        sets += model_boot_report()->set;
        /* never a calibration op, never a mode switch, never the output on by the apply */
        CHECK(!logged("il_corr(") && !logged("il_skew(") && !logged("mode(") && !logged("gen_out(1)"));
        CHECK(g_notes == 0);                                /* ORIGIN_BOOT: no settings note */
        /* and the restore did not dirty the settings record */
        const uint32_t w = settings_store_get_status()->writes;
        settle();
        CHECKF(settings_store_get_status()->writes == w, "round %d: the boot apply changed the record", round);
        if (bad > 20) break;
    }
    CHECK(bad == 0);
    CHECK(nor_outside == 0 && everything_else_intact());
    printf("  %d random sessions restored; %u setter calls in all (%.1f a boot), %u log erases\n", rounds, sets,
           (double)sets / rounds, (unsigned)nor_erases[1]);
}

static void test_fallbacks(void)
{
    printf("fallbacks\n");
    static int32_t base[NPROP], v[NPROP], with[NPROP];
    static uint8_t rec[1024];
    static ent_t e[NPROP];
    model_session_info_t info;

    fresh_device();
    a_session();
    CHECK(set(P(VIEW_MODE), VIEW_MODE_XY));
    CHECK(power_off());
    logs_keep();
    const uint16_t good_len = newest(&info);
    const uint32_t stamp = info.stamp;
    CHECK(good_len && info.count == model_session_member_count());

    /* what Restore did before there was a snapshot: this settings record, a blank session log */
    session_log_blank();
    boot();
    take(base);
    CHECK(session_snapshot_status()->load == SESSION_LOAD_NONE);
    const uint8_t base_kept = model_boot_report()->kept;
    CHECK(base_kept == 2 && prop_get(P(VIEW_MODE)) == VIEW_MODE_XY);   /* the record's view byte (view.mode, tb.roll), as before */
    CHECK(prop_get(P(CH2_ENABLED)) == 1 && prop_get(P(CH1_POS)) == 0 && prop_get(P(CH1_VDIV)) == 6);

    /* the snapshot itself does something (or the cases below prove nothing) */
    logs_back(); boot(); take(with);
    CHECK(session_snapshot_status()->load == SESSION_LOAD_OK && differ(with, base, false) >= 10);
    CHECK(strstr(status_text(), "session at this boot: valid") && strstr(g_out, "refused") && strstr(g_out, "0 refused"));

#define FALLBACK(name, expect) do { \
        boot(); take(v); \
        CHECKF(session_snapshot_status()->load == (expect), "%s: %s", name, \
               session_load_text(session_snapshot_status()->load)); \
        CHECKF(differ(v, base, true) == 0, "%s: not the settings record's state", name); \
        CHECK(model_boot_report()->kept == base_kept && session_snapshot_status()->kept == 0); \
    } while (0)

    /* a tombstone */
    logs_back();
    CHECK(session_snapshot_clear() && session_snapshot_status()->clears == 1);
    FALLBACK("cleared", SESSION_LOAD_CLEARED);
    CHECK(strstr(status_text(), "none (cleared)"));

    /* a bad checksum under a sound log record */
    logs_back();
    memcpy(rec, g_rec, good_len);
    rec[40] ^= 0x10u;
    append_raw(rec, good_len);
    FALLBACK("bad checksum", SESSION_LOAD_BAD);

    /* garbage, and a record longer than any snapshot */
    logs_back();
    for (unsigned i = 0; i < 300; i++) rec[i] = (uint8_t)R(256);
    append_raw(rec, 300);
    FALLBACK("garbage", SESSION_LOAD_BAD);
    logs_back();
    append_raw(rec, 900);
    FALLBACK("too long", SESSION_LOAD_BAD);

    /* another format version (the same entries, a sound checksum) */
    logs_back(); boot();
    const uint16_t n = live_entries(e);
    logs_back();
    append_raw(rec, craft(rec, 2, 0, 9, stamp, e, n));
    FALLBACK("version 2", SESSION_LOAD_VERSION);
    logs_back();
    append_raw(rec, craft(rec, 0, 0, 9, stamp, e, n));
    FALLBACK("version 0", SESSION_LOAD_VERSION);

    /* a snapshot saved with another settings record */
    logs_back();
    append_raw(rec, craft(rec, 1, 0, 9, stamp ^ 1u, e, n));
    FALLBACK("stamp", SESSION_LOAD_STALE);
    CHECK(strstr(status_text(), "stale"));

    /* damage inside the log: the newest record is torn, there is none before it */
    logs_back();
    nor[flash_region_table[FLASH_REGION_SESSION].start + 8u + 30u] &= 0x00u;
    FALLBACK("torn record", SESSION_LOAD_NONE);

#undef FALLBACK

    /* no storage: the settings are the built-in defaults then, and so is the session */
    logs_back();
    (void)flash_regions_init(NULL);
    boot();
    CHECK(session_snapshot_status()->load == SESSION_LOAD_NO_STORAGE && session_snapshot_status()->kept == 0);
    CHECK(prop_get(P(CH1_VDIV)) == prop_default(P(CH1_VDIV)) && prop_get(P(CH2_ENABLED)) == 1);
    CHECK(!session_snapshot_save() && session_snapshot_status()->save == SESSION_SAVE_NO_STORAGE);
    CHECK(session_snapshot_clear());                    /* nothing there to clear */
    CHECK(flash_regions_bind_w25q() == FLASH_REGION_OK);

    /* STALE as it happens: the session is restored, a setting changes and is autosaved, and the
     * device goes down without a power-off (battery critical, a crash). The snapshot is older
     * than the settings: the boot is the new settings record alone. */
    logs_back(); boot();
    CHECK(session_snapshot_status()->load == SESSION_LOAD_OK);
    CHECK(set(P(CH1_VDIV), 2) && set(P(CH1_POS), -30));
    settle();
    logs_keep();
    session_log_blank(); boot(); take(base);            /* the new record with no snapshot */
    CHECK(prop_get(P(CH1_VDIV)) == 2 && prop_get(P(CH1_POS)) == 0);
    logs_back(); boot(); take(v);
    CHECK(session_snapshot_status()->load == SESSION_LOAD_STALE && differ(v, base, true) == 0);
    CHECK(prop_get(P(CH1_VDIV)) == 2 && prop_get(P(CH2_ENABLED)) == 1);    /* not the old 6, not the old session */
    /* the next orderly power-off makes a snapshot that is used again */
    CHECK(set(P(CH2_ENABLED), 0) && power_off());
    boot();
    CHECK(session_snapshot_status()->load == SESSION_LOAD_OK && prop_get(P(CH2_ENABLED)) == 0);

    /* a settings write that did not happen at power-off (the snapshot stamps the image the flush
     * WOULD write): stale too, the old record rules */
    CHECK(set(P(CH1_VDIV), 8));                          /* changed, not saved: no key, no flush */
    CHECK(session_snapshot_save());
    boot();
    CHECK(session_snapshot_status()->load == SESSION_LOAD_STALE && prop_get(P(CH1_VDIV)) == 2);

    /* the order of the two saves does not matter once the screen is noted: snapshot first */
    CHECK(set(P(CH1_VDIV), 8) && set(P(CH1_POS), 15));
    current_mode = MODE_OSCILLOSCOPE;
    model_last_screen_note_power_off(startup_screen_of_mode(current_mode));
    CHECK(session_snapshot_save());
    g_now += 100u;
    (void)settings_store_flush(g_now);
    boot();
    CHECK(session_snapshot_status()->load == SESSION_LOAD_OK && prop_get(P(CH1_VDIV)) == 8 &&
          prop_get(P(CH1_POS)) == 15 && model_last_screen() == STARTUP_SCREEN_SCOPE);
    /* and the shell's rehearsal: `session save` (flush, snapshot), then a restart that flushes again */
    CHECK(set(P(CH1_POS), -45));
    g_now += 100u; (void)settings_store_flush(g_now);
    CHECK(session_snapshot_save());
    g_now += 100u; (void)settings_store_flush(g_now);    /* the reboot's own flush: nothing new */
    boot();
    CHECK(session_snapshot_status()->load == SESSION_LOAD_OK && prop_get(P(CH1_POS)) == -45);

    /* a torn newest record with an older snapshot before it: the older one is the newest the log
     * can read, and it is used only because the settings are still the ones it was saved with */
    CHECK(set(P(CH1_POS), 55));
    CHECK(session_snapshot_save());                      /* settings unchanged: same stamp */
    uint32_t used = 0;
    (void)session_snapshot_log_info(&used, NULL, NULL);
    nor[flash_region_table[FLASH_REGION_SESSION].start + used - 6u] &= 0x00u;      /* in its payload */
    boot();
    CHECK(session_snapshot_status()->load == SESSION_LOAD_OK && prop_get(P(CH1_POS)) == -45);
    CHECK(nor_outside == 0 && everything_else_intact());
}

static void test_other_tables(void)
{
    printf("another table's snapshot\n");
    static uint8_t rec[1024];
    static ent_t e[NPROP + 16];
    static int32_t was[NPROP];
    model_session_info_t info;

    fresh_device();
    a_session();
    CHECK(set(P(GEN_OUT), 1) && prop_get(P(GEN_BOOTOUT)) == 0);
    CHECK(power_off());
    (void)newest(&info);
    take(was);
    uint16_t n = live_entries(e);

    /* a table with properties this build does not have, one it lacks, values it cannot hold,
     * and entries this build leaves out on purpose */
    ent_find(e, n, PROP_ID_CH1_POS)->id = 0x01F0;                 /* "ch1.pos" left, an unknown id took its value */
    e[n++] = (ent_t){ 0x0D01, 7 };                                /* a group that does not exist here */
    e[n++] = (ent_t){ 0x05F7, -3 };
    ent_find(e, n, PROP_ID_CH1_VDIV)->v = 200;                    /* enum out of range */
    ent_find(e, n, PROP_ID_TRIG_X)->v = 5000;                     /* int out of range: not clamped */
    ent_find(e, n, PROP_ID_CH2_ENABLED)->v = 2;                   /* bool */
    ent_find(e, n, PROP_ID_CH1_MEAS)->v = 0x1FFF;                 /* a mask bit that does not exist */
    e[n++] = (ent_t){ PROP_ID_GEN_OUT, 1 };                       /* never from a snapshot */
    e[n++] = (ent_t){ PROP_ID_VIEW_RUN, 0 };
    e[n++] = (ent_t){ PROP_ID_IL_GAIN, 900000 };
    e[n++] = (ent_t){ PROP_ID_IL_KNOTS, 0 };
    e[n++] = (ent_t){ PROP_ID_METER_HOLD, 1 };
    e[n++] = (ent_t){ PROP_ID_SYS_INSTRUMENT, 2 };
    e[n++] = (ent_t){ PROP_ID_TRIG_HELD, 1 };                     /* read only */
    append_raw(rec, craft(rec, 1, 0, (uint16_t)(info.seq + 1u), info.stamp, e, n));
    g_il_knots = 3;
    boot();
    const session_status_t *st = session_snapshot_status();
    CHECK(st->load == SESSION_LOAD_OK && st->load_count == n);
    CHECKF(st->unknown == 3 && st->out_of_range == 4 && st->excluded == 7,
           "unknown %u, out of range %u, excluded %u", st->unknown, st->out_of_range, st->excluded);
    CHECK(st->kept == model_session_member_count() - 5u);
    CHECK(model_boot_report()->refused == 0);
    /* the skipped ones are where the settings record or the default has them */
    CHECK(prop_get(P(CH1_POS)) == 0);                             /* missing: default */
    CHECK(prop_get(P(CH1_VDIV)) == 6);                            /* the settings record's */
    CHECK(prop_get(P(TRIG_X)) == prop_default(P(TRIG_X)));        /* not clamped to 300 */
    CHECK(prop_get(P(CH2_ENABLED)) == 1);
    CHECK(prop_get(P(CH1_MEAS)) == was[PROP_IX_CH1_MEAS]);
    CHECK(prop_get(P(GEN_OUT)) == 0 && prop_get(P(VIEW_RUN)) == 1 && prop_get(P(METER_HOLD)) == 0);
    CHECK(g_il_gain == 1000000 && !logged("il_corr(") && !logged("mode(") && !logged("gen_out(1)"));
    g_il_knots = 0;
    /* everything both tables share is back */
    CHECK(prop_get(P(VIEW_CURSOR)) == 3 && prop_get(P(VIEW_CURSEL)) == 3 && prop_get(P(VIEW_CURT1)) == -987654321 &&
          prop_get(P(VIEW_PERSIST)) == 2 && prop_get(P(METER_BEEP)) == 3 && prop_get(P(METER_DEGF)) == 1 &&
          prop_get(P(MASK_TOLV)) == 20 && prop_get(P(TRIG_SOFT)) == 0);
    CHECK(strstr(status_text(), "skipped 3 unknown id, 7 not restored by this build, 4 out of range"));

    /* an empty snapshot is a snapshot: nothing kept, nothing changed */
    append_raw(rec, craft(rec, 1, 0, 50, info.stamp, e, 0));
    boot();
    CHECK(session_snapshot_status()->load == SESSION_LOAD_OK && session_snapshot_status()->kept == 0);
    CHECK(prop_get(P(CH2_ENABLED)) == 1 && prop_get(P(VIEW_CURSOR)) == 0);
    /* entries that run past the end, under a sound checksum: not used at all */
    n = live_entries(e);
    uint16_t l = craft(rec, 1, 0, 51, info.stamp, e, n);
    rec[6] = (uint8_t)(n + 1u);                                   /* one more than there are */
    const uint32_t c = t_crc32(rec, (uint32_t)l - 4u);
    for (int k = 0; k < 4; k++) rec[l - 4 + k] = (uint8_t)(c >> (8 * k));
    append_raw(rec, l);
    boot();
    CHECK(session_snapshot_status()->load == SESSION_LOAD_BAD && session_snapshot_status()->kept == 0);
}

/* A snapshot an older firmware wrote: view.mode under 0x0501, the id it had
 * while roll was one of its values (its 3 was fft, here split), and no tb.roll
 * (it was left out then). The retired id is skipped as unknown, so the view
 * is the settings record's, whose byte means the same to both firmwares. */
static void test_retired_view_id(void)
{
    printf("a snapshot with view.mode's retired id\n");
    static uint8_t rec[1024];
    static ent_t e[NPROP + 16];
    model_session_info_t info;

    CHECK(prop_by_id(0x0501) == NULL && PROP_ID_VIEW_MODE != 0x0501);
    fresh_device();
    a_session();
    CHECK(set(P(VIEW_MODE), VIEW_MODE_XY) && power_off());       /* the record: byte 2, xy */
    (void)newest(&info);
    uint16_t n = live_entries(e), m = 0;
    for (uint16_t i = 0; i < n; i++)
        if (e[i].id != PROP_ID_TB_ROLL) e[m++] = e[i];
    n = m;
    ent_t *vm = ent_find(e, n, PROP_ID_VIEW_MODE);
    CHECK(vm != NULL);
    if (!vm) return;
    *vm = (ent_t){ 0x0501, 3 };
    append_raw(rec, craft(rec, 1, 0, (uint16_t)(info.seq + 1u), info.stamp, e, n));
    boot();
    const session_status_t *st = session_snapshot_status();
    CHECKF(st->load == SESSION_LOAD_OK && st->unknown == 1 && st->excluded == 0 && st->out_of_range == 0,
           "load %u unknown %u excluded %u", st->load, st->unknown, st->excluded);
    CHECK(model_boot_report()->refused == 0);
    CHECK(prop_get(P(VIEW_MODE)) == VIEW_MODE_XY && prop_get(P(TB_ROLL)) == 0);   /* the record's pair */
    CHECK(model_view_record() == 2u);
}

/* A snapshot an older firmware wrote, with the time cursors as screen
 * pixels under 0x0509..0x050C: released ids, skipped as unknown; the lines
 * the snapshot also holds under their own ids come back. */
static void test_retired_cursor_ids(void)
{
    printf("a snapshot with the pixel cursors' retired ids\n");
    static uint8_t rec[1024];
    static ent_t e[NPROP + 16];
    model_session_info_t info;

    for (uint16_t id = 0x0509; id <= 0x050C; id++) CHECK(prop_by_id(id) == NULL);
    fresh_device();
    a_session();
    CHECK(power_off());
    (void)newest(&info);
    uint16_t n = live_entries(e);
    for (uint16_t id = 0x0509; id <= 0x050C; id++) e[n++] = (ent_t){ id, 106 };
    for (uint16_t i = 1; i < n; i++)                       /* in id order, as a writer lays them */
        for (uint16_t j = i; j > 0 && e[j - 1].id > e[j].id; j--) {
            const ent_t t = e[j]; e[j] = e[j - 1]; e[j - 1] = t;
        }
    append_raw(rec, craft(rec, 1, 0, (uint16_t)(info.seq + 1u), info.stamp, e, n));
    boot();
    const session_status_t *st = session_snapshot_status();
    CHECKF(st->load == SESSION_LOAD_OK && st->unknown == 4 && st->excluded == 0 && st->out_of_range == 0,
           "load %u unknown %u excluded %u", st->load, st->unknown, st->excluded);
    CHECK(prop_get(P(VIEW_CURT1)) == -987654321 && prop_get(P(VIEW_CURSOR)) == 3);
}

/* A boot with owners that never answer stays bounded: once more than the
 * limit of hardware properties were refused (here 1: ch2.enabled, then
 * view.mode), the remaining hardware ones (view.persist) are not tried and
 * none is retried; the others still apply. */
static void test_refused_bound(void)
{
    puts("a dead owner does not stretch the boot");
    fresh_device();
    a_session();
    CHECK(set(P(VIEW_MODE), VIEW_MODE_WATERFALL));
    CHECK(power_off());
    g_fail_enables = -1; g_fail_pers = true; g_fail_view = true;
    model_boot_hw_refused_max = 1;                  /* the firmware's is MODEL_BOOT_HW_REFUSED_MAX */
    boot();
    const model_boot_report_t *br = model_boot_report();
    CHECK(session_snapshot_status()->load == SESSION_LOAD_OK);
    CHECKF(br->refused >= 3, "%u refused", br->refused);   /* the skipped hardware ones are reported too */
    /* nothing was tried twice */
    CHECKF(logged_times("enables;") == 1 && logged_times("pers_on(2);") == 0, "enables x%d, pers x%d",
           logged_times("enables;"), logged_times("pers_on(2);"));
    /* a property with no hardware behind it is back all the same */
    CHECK(br->applied > 0);
    model_boot_hw_refused_max = MODEL_BOOT_HW_REFUSED_MAX;
    g_fail_enables = 0; g_fail_pers = false; g_fail_view = false;
}

static void test_refused(void)
{
    printf("refused applies\n");
    static int32_t was[NPROP];
    static uint8_t rec[1024];
    static ent_t e[NPROP];
    model_session_info_t info;

    fresh_device();
    a_session();
    CHECK(set(P(VIEW_MODE), VIEW_MODE_WATERFALL));  /* in the record and in the snapshot */
    take(was);
    CHECK(power_off());

    /* two owners say no: the routing, and the pool (persistence and the view) */
    g_fail_enables = -1; g_fail_pers = true; g_fail_view = true;
    boot();
    const model_boot_report_t *br = model_boot_report();
    CHECK(session_snapshot_status()->load == SESSION_LOAD_OK);
    CHECKF(br->refused == 3 && br->first_refused == PROP_ID_CH2_ENABLED, "%u refused, first %04X", br->refused,
           br->first_refused);
    CHECK(br->kept == model_session_member_count() && br->applied == br->kept - 3u);
    CHECK(model_boot_state(P(CH2_ENABLED)) == BOOT_KEEP_REFUSED && model_boot_state(P(VIEW_PERSIST)) == BOOT_KEEP_REFUSED &&
          model_boot_state(P(VIEW_MODE)) == BOOT_KEEP_REFUSED && model_boot_state(P(CH1_POS)) == BOOT_KEEP_APPLIED &&
          model_boot_state(P(TRIG_CODE)) == BOOT_KEEP_NONE);
    /* each was tried twice, and the others went on */
    CHECK(logged_times("enables;") == 2 && logged_times("pers_on(2);") == 2);
    /* the refused ones are where the settings record / the default has them */
    CHECK(prop_get(P(CH2_ENABLED)) == 1 && prop_get(P(VIEW_PERSIST)) == 0 && prop_get(P(VIEW_MODE)) == VIEW_MODE_YT);
    /* everything else is back */
    unsigned other = 0;
    for (uint16_t i = 0; i < NPROP; i++) {
        const prop_desc_t *p = prop_at(i);
        if (model_session_excluded(p) || p == P(CH2_ENABLED) || p == P(VIEW_PERSIST) || p == P(VIEW_MODE)) continue;
        if (prop_get(p) != was[i]) { other++; printf("    %s not restored\n", p->name); }
    }
    CHECK(other == 0);
    CHECK(strstr(status_text(), "3 refused") && strstr(g_out, "  refused: ch2.enabled view.mode view.persist\n"));
    /* a snapshot value that was refused is not kept: the live value is what is saved next. The
     * settings record's view is (sticky): record and snapshot keep the view the boot could not show */
    int32_t kv = -1;
    CHECK(!model_boot_kept(P(CH2_ENABLED), NULL) && !model_boot_kept(P(VIEW_PERSIST), NULL));
    CHECK(model_boot_kept(P(VIEW_MODE), &kv) && kv == VIEW_MODE_WATERFALL && model_view_record() == 5u);
    g_fail_enables = 0; g_fail_pers = false; g_fail_view = false;
    CHECK(power_off());
    boot();
    CHECK(model_boot_report()->refused == 0);
    CHECK(prop_get(P(CH2_ENABLED)) == 1 && prop_get(P(VIEW_PERSIST)) == 0 && prop_get(P(VIEW_MODE)) == VIEW_MODE_WATERFALL);
    /* the user's own change ends a keep, and the refusal stays in the report */
    g_fail_view = true;
    boot();
    CHECK(prop_get(P(VIEW_MODE)) == 0 && model_boot_kept(P(VIEW_MODE), NULL));
    g_fail_view = false;
    CHECK(set(P(VIEW_MODE), VIEW_MODE_XY) && !model_boot_kept(P(VIEW_MODE), NULL) &&
          model_boot_state(P(VIEW_MODE)) == BOOT_KEEP_REFUSED);
    CHECK(set(P(VIEW_MODE), VIEW_MODE_YT));

    /* a refusal that passes: tried again after the others, in force, not reported */
    CHECK(set(P(CH2_ENABLED), 0) && power_off());
    g_fail_enables = 1;
    boot();
    CHECK(model_boot_report()->refused == 0 && prop_get(P(CH2_ENABLED)) == 0 && logged_times("enables;") == 2);
    CHECK(model_boot_state(P(CH2_ENABLED)) == BOOT_KEEP_APPLIED);

    /* a rule refuses (no owner involved): a hand-made snapshot that wants the trigger on a
     * channel it switches off. The rest is applied. */
    CHECK(set(P(CH2_ENABLED), 1) && set(P(TRIG_SOURCE), 0) && power_off());
    (void)newest(&info);
    const uint16_t n = live_entries(e);
    ent_find(e, n, PROP_ID_CH2_ENABLED)->v = 0;
    ent_find(e, n, PROP_ID_TRIG_SOURCE)->v = 1;
    ent_find(e, n, PROP_ID_CH2_POS)->v = -25;
    append_raw(rec, craft(rec, 1, 0, (uint16_t)(info.seq + 1u), info.stamp, e, n));
    boot();
    CHECK(model_boot_report()->refused == 1 && model_boot_report()->first_refused == PROP_ID_TRIG_SOURCE);
    CHECK(prop_get(P(CH2_ENABLED)) == 0 && prop_get(P(TRIG_SOURCE)) == 0 && prop_get(P(CH2_POS)) == -25);
    CHECK(g_between >= 1);                              /* the check-in ran between the sets */
}

static void test_order(void)
{
    printf("apply order\n");
    static uint8_t rec[1024];
    static ent_t e[NPROP], rev[NPROP];
    static int32_t was[NPROP];
    model_session_info_t info;

    fresh_device();
    /* the rules, by hand: each of these needs the other property first */
    CHECK(!set(P(VIEW_CURSEL), 3));                                 /* cursors are off */
    CHECK(set(P(CH1_ENABLED), 0) && prop_get(P(TRIG_SOURCE)) == 1);  /* the source followed to CH2 */
    CHECK(!set(P(TRIG_SOURCE), 0));                                 /* CH1 is off */
    CHECK(set(P(CH1_ENABLED), 1));

    /* CH1 off with the trigger on CH2; Roll at step 3; both cursor pairs with h2 selected */
    CHECK(set(P(CH1_ENABLED), 0) && set(P(TB_ROLLSTEP), 3) && set(P(TB_ROLL), 1));
    CHECK(set(P(VIEW_CURSOR), 3) && set(P(VIEW_CURSEL), 3));
    CHECK(prop_get(P(TB_ROLL)) == 1);
    take(was);
    CHECK(power_off());
    boot();
    CHECK(model_boot_report()->refused == 0 && check_restored(was, "order") == 0);
    CHECK(scope_state_get()->roll && scope_state_get()->roll_step == 3);
    /* table order: tb.roll (0x0302) goes in at the step in RAM, then tb.rollstep (0x0303)
     * re-enters Roll at the saved one: TWO FPGA writes (model.h, the boot apply's order) */
    CHECKF(logged_times("roll;") == 2, "roll written %u times: %s", logged_times("roll;"), g_log);
    /* the routing before anything of the trigger; the cursor pair before the selected line */
    CHECK(logged("enables;") && !logged("trigsrc;"));

    /* The same snapshot with its entries in REVERSE order (another writer): the apply is table
     * order whatever the record's order is, so nothing is refused and Roll is still two writes. */
    (void)newest(&info);
    boot();
    const uint16_t n = live_entries(e);
    for (uint16_t i = 0; i < n; i++) rev[i] = e[n - 1u - i];
    append_raw(rec, craft(rec, 1, 0, (uint16_t)(info.seq + 1u), info.stamp, rev, n));
    boot();
    CHECK(session_snapshot_status()->load == SESSION_LOAD_OK && model_boot_report()->refused == 0);
    CHECK(check_restored(was, "reverse order") == 0 && logged_times("roll;") == 2);

    /* a spectrum view, then persistence on top of X-Y: the view first, the pool claim after */
    CHECK(set(P(VIEW_MODE), VIEW_MODE_XY) && set(P(VIEW_PERSIST), 4) && set(P(CH1_ENABLED), 1));
    take(was);
    CHECK(power_off());
    boot();
    CHECK(model_boot_report()->refused == 0 && check_restored(was, "view and persistence") == 0);
    const char *view = strstr(g_log, "view("), *pers = strstr(g_log, "pers_on(4);");
    CHECK(view && pers && view < pers);
    /* the two mask tolerances travel as ONE request */
    CHECK(set(P(MASK_TOLV), 30) && set(P(MASK_TOLH), 7) && power_off());
    boot();
    CHECK(logged_times("mask_tol(") == 1 && logged("mask_tol(30,7);") && model_boot_report()->refused == 0);

    /* before the apply (the window between the load and the input task): a capture of either
     * kind writes what was kept, so nothing is lost if the device goes down there */
    boot_load();
    CHECK(prop_get(P(VIEW_PERSIST)) == 0 && model_boot_state(P(VIEW_PERSIST)) == BOOT_KEEP_PENDING);
    CHECK(session_snapshot_save());
    boot();
    CHECK(prop_get(P(VIEW_PERSIST)) == 4 && prop_get(P(MASK_TOLV)) == 30 && prop_get(P(VIEW_MODE)) == VIEW_MODE_XY);
}

static void test_not_restore(void)
{
    printf("Startup on Boot is not Restore\n");
    static int32_t base[NPROP], v[NPROP];
    static const startup_mode_t modes[] = { STARTUP_SCOPE, STARTUP_METER, STARTUP_HOME, STARTUP_SIGGEN };
    for (unsigned m = 0; m < 4; m++) {
        fresh_device();
        a_session();
        CHECK(set(P(SYS_STARTUP), (int32_t)modes[m]));
        CHECK(power_off());                              /* the snapshot is written all the same */
        CHECK(session_snapshot_status()->save == SESSION_SAVE_OK);
        logs_keep();
        session_log_blank(); boot(); take(base);         /* the same record with no snapshot */
        logs_back();
        nor_session_reads = 0;
        boot(); take(v);
        CHECK(session_snapshot_status()->load == SESSION_LOAD_NOT_RESTORE);
        CHECK(nor_session_reads == 0);                   /* not even read */
        CHECK(differ(v, base, true) == 0 && model_boot_report()->kept == 2);   /* the record's view byte */
        CHECK(prop_get(P(CH2_ENABLED)) == 1 && prop_get(P(VIEW_CURSOR)) == 0 && prop_get(P(CH1_VDIV)) == 6);
        CHECK(strstr(status_text(), "not read (sys.startup is not restore)"));
        /* back to Restore: the power-off that follows is the one a Restore boot shows */
        CHECK(set(P(SYS_STARTUP), STARTUP_RESTORE) && set(P(CH1_POS), 35) && power_off());
        boot();
        CHECK(session_snapshot_status()->load == SESSION_LOAD_OK && prop_get(P(CH1_POS)) == 35 &&
              prop_get(P(CH2_ENABLED)) == 1);
    }
}

static void test_never_restored(void)
{
    printf("view.run, the generator output, hold and relative\n");
    fresh_device();
    /* stopped at power-off: running at boot */
    CHECK(set(P(VIEW_RUN), 0) && prop_get(P(VIEW_RUN)) == 0);
    CHECK(set(P(CH1_POS), 20) && power_off());
    boot();
    CHECK(session_snapshot_status()->load == SESSION_LOAD_OK && prop_get(P(CH1_POS)) == 20);
    CHECK(prop_get(P(VIEW_RUN)) == 1 && scope_state_get()->running);
    /* SINGLE that had fired and was stopped: armed and running, the mode kept */
    CHECK(set(P(TRIG_MODE), 2) && set(P(VIEW_RUN), 0) && power_off());
    boot();
    CHECK(prop_get(P(TRIG_MODE)) == 2 && prop_get(P(VIEW_RUN)) == 1);
    /* hold and relative */
    CHECK(set(P(METER_HOLD), 1) && set(P(METER_REL), 1) && set(P(METER_DEGF), 1) && power_off());
    boot();
    CHECK(prop_get(P(METER_HOLD)) == 0 && prop_get(P(METER_REL)) == 0 && prop_get(P(METER_DEGF)) == 1);

    /* the generator: its settings come back, its output follows gen.bootout alone */
    for (int bootout = 0; bootout < 2; bootout++) for (int out = 0; out < 2; out++) {
        CHECK(set(P(GEN_BOOTOUT), bootout) && set(P(GEN_OUT), out) && set(P(GEN_FREQ), 4321 + out) &&
              set(P(GEN_WAVE), 2 + bootout));
        CHECK(power_off());
        boot();
        CHECK(session_snapshot_status()->load == SESSION_LOAD_OK && model_boot_report()->refused == 0);
        CHECKF(prop_get(P(GEN_OUT)) == (bootout && out), "bootout %d, out %d: output %ld", bootout, out,
               (long)prop_get(P(GEN_OUT)));
        CHECK(prop_get(P(GEN_FREQ)) == 4321 + out && prop_get(P(GEN_WAVE)) == 2 + bootout &&
              prop_get(P(GEN_BOOTOUT)) == bootout);
        CHECK(!logged("gen_out("));                      /* the apply never touches the output */
    }
}

static void test_reset(void)
{
    printf("Reset to defaults\n");
    fresh_device();
    /* nothing to invalidate: nothing written to the session log */
    settings_reset_result_t r = settings_store_reset_to_defaults(g_now, between);
    CHECK(r.status == PS_OK && nor_programs[1] == 0 && nor_erases[1] == 0);
    CHECK(session_snapshot_clear() && nor_programs[1] == 0);

    a_session();
    CHECK(power_off());
    boot();
    CHECK(session_snapshot_status()->load == SESSION_LOAD_OK && prop_get(P(CH2_ENABLED)) == 0);

    /* self-cal running: nothing happens, the snapshot stays */
    g_busy = true;
    uint32_t p1 = nor_programs[1];
    r = settings_store_reset_to_defaults(g_now, between);
    g_busy = false;
    CHECK(r.status == PS_BUSY && nor_programs[1] == p1);

    const uint16_t clears = session_snapshot_status()->clears;
    r = settings_store_reset_to_defaults(g_now, between);
    CHECK(r.status == PS_OK && r.left == 0);
    /* the tombstone: header + payload (+ a page crossing, as at line ~1268) */
    CHECKF(session_snapshot_status()->clears == clears + 1u && nor_programs[1] >= p1 + 2u &&
           nor_programs[1] <= p1 + 3u && nor_erases[1] == 0,
           "clears %u->%u programs %u->%u erases %u", clears, session_snapshot_status()->clears, (unsigned)p1,
           (unsigned)nor_programs[1], (unsigned)nor_erases[1]);
    /* the next Restore boot does not undo the reset */
    boot();
    CHECK(session_snapshot_status()->load == SESSION_LOAD_CLEARED && session_snapshot_status()->kept == 0);
    for (uint16_t i = 0; i < NPROP; i++) {
        const prop_desc_t *p = prop_at(i);
        if (!prop_is_settable(p) || prop_reset_keeps(p)) continue;
        CHECKF(prop_get(p) == prop_default(p), "%s after a reset and a Restore boot", p->name);
    }
    /* again: already cleared, no write */
    p1 = nor_programs[1];
    r = settings_store_reset_to_defaults(g_now, between);
    CHECK(r.status == PS_OK && nor_programs[1] == p1);
    /* a power-off after the reset starts a new snapshot */
    CHECK(set(P(CH1_POS), 10) && power_off());
    boot();
    CHECK(session_snapshot_status()->load == SESSION_LOAD_OK && prop_get(P(CH1_POS)) == 10);

    /* a reset with a keep the boot could not put in force: gone too (it is not saved back) */
    CHECK(set(P(VIEW_MODE), VIEW_MODE_WATERFALL) && set(P(VIEW_CURSOR), 1) && power_off());
    g_fail_view = true;
    boot();
    g_fail_view = false;
    CHECK(model_boot_kept(P(VIEW_MODE), NULL));
    r = settings_store_reset_to_defaults(g_now, between);
    CHECK(r.status == PS_OK && !model_boot_kept(P(VIEW_MODE), NULL));
    boot();
    CHECK(prop_get(P(VIEW_MODE)) == 0 && prop_get(P(VIEW_CURSOR)) == 0);

    /* no room for the tombstone: it goes to the other bank */
    fresh_device();
    a_session();
    CHECK(power_off());
    model_session_info_t info;
    const uint32_t rec_total = 8u + ((newest(&info) + 3u) & ~3u);
    session_log_blank();
    static uint8_t junk[1024];
    memset(junk, 0x5A, sizeof junk);
    uint32_t fill = 4096u - rec_total - 8u;              /* leaves 8 bytes after the snapshot */
    while (fill > 1032u) { junk[0]++; append_raw(junk, 1000u); fill -= 1008u; }     /* (an equal record is elided) */
    append_raw(junk, fill - 8u);
    CHECK(power_off());                                  /* fits exactly: no erase */
    CHECK(nor_erases[1] == 0);
    uint32_t used = 0, total = 0;
    CHECK(session_snapshot_log_info(&used, &total, NULL) && total - used == 8u);
    CHECK(session_snapshot_clear() && nor_erases[1] == 0 && session_snapshot_status()->compactions == 1);
    boot();
    CHECK(session_snapshot_status()->load == SESSION_LOAD_CLEARED);
    CHECK(nor_outside == 0 && everything_else_intact());
}

static void test_flash(void)
{
    printf("flash writes\n");
    fresh_device();
    /* a boot writes nothing, anywhere */
    CHECK(nor_programs[0] == 0 && nor_programs[1] == 0 && nor_erases[0] == 0 && nor_erases[1] == 0);
    /* a session of changes, key presses and settings saves: the settings log only */
    random_session(300);
    settle();
    CHECK(set(P(SYS_STARTUP), STARTUP_RESTORE));
    settle();
    CHECK(nor_programs[0] > 0 && nor_programs[1] == 0 && nor_erases[1] == 0);
    CHECK(session_snapshot_status()->saves == 0 && log_records() == 0);

    /* one save = one record; the settings log's bytes are not touched by it */
    static uint8_t settings_was[0x10000];
    memcpy(settings_was, region_bytes(FLASH_REGION_SETTINGS), sizeof settings_was);
    const uint32_t sw = settings_store_get_status()->writes, sp = nor_programs[0];
    CHECK(session_snapshot_save());
    CHECK(session_snapshot_status()->saves == 1 && log_records() == 1);
    CHECK(nor_programs[1] >= 2 && nor_programs[1] <= 3);        /* header + payload (+ a page crossing) */
    CHECK(memcmp(settings_was, region_bytes(FLASH_REGION_SETTINGS), sizeof settings_was) == 0);
    CHECK(settings_store_get_status()->writes == sw && nor_programs[0] == sp && nor_erases[0] == 0);
    /* the settings record is the 60-byte v5 one, as before */
    device_config_t cfg;
    CHECK(config_load(&cfg) && sizeof cfg == 60u && cfg.version == CONFIG_VERSION);
    /* a clear: one tombstone, the settings log untouched again */
    uint32_t p1 = nor_programs[1];
    CHECK(session_snapshot_clear() && nor_programs[1] >= p1 + 2u && nor_programs[1] <= p1 + 3u &&
          log_records() == 2);                                  /* header + payload (+ a page crossing) */
    CHECK(memcmp(settings_was, region_bytes(FLASH_REGION_SETTINGS), sizeof settings_was) == 0);
    /* status, the shell: reading costs no write */
    p1 = nor_programs[1];
    (void)status_text();
    CHECK(nor_programs[1] == p1 && strstr(g_out, "log: 2 records") && strstr(g_out, "1 saves, 1 clears"));

    /* a year of power-offs: every save is one record, the log moves between its two sectors,
     * the sequence number counts on across the erases, and the last state is the one restored */
    static int32_t was[NPROP];
    uint16_t seq = session_snapshot_status()->save_seq;
    uint32_t erases = 0;
    for (int day = 0; day < 365; day++) {
        CHECK(set(P(CH1_POS), (day % 41) * 5 - 100) && set(P(VIEW_CURT2), 100 + day % 200));
        const uint32_t recs = log_records(), e0 = nor_erases[1], pr = nor_programs[1];
        const uint32_t moves = session_snapshot_status()->compactions;
        CHECK(power_off());
        const session_status_t *st = session_snapshot_status();
        CHECK(st->save == SESSION_SAVE_OK && st->save_seq == (uint16_t)(seq + 2u));   /* a tombstone was 1 */
        seq = (uint16_t)(st->save_seq - 1u);
        erases += nor_erases[1] - e0;
        if (st->compactions != moves) {                 /* + the bank header */
            CHECK(nor_erases[1] <= e0 + 1u && log_records() == 1);
            CHECK(nor_programs[1] - pr >= 3u && nor_programs[1] - pr <= 4u);
        } else {
            CHECK(nor_erases[1] == e0 && log_records() == recs + 1u);
            CHECK(nor_programs[1] - pr >= 2u && nor_programs[1] - pr <= 3u);
        }
        seq = st->save_seq;
        if (day % 30 == 0) {
            take(was);
            boot();
            CHECK(session_snapshot_status()->load == SESSION_LOAD_OK && check_restored(was, "a year") == 0);
        }
        seq = (uint16_t)(seq - 1u);
    }
    CHECK(erases >= 15 && erases == nor_erases[1]);
    CHECK(nor_outside == 0 && everything_else_intact());
    printf("  365 power-offs: %u sector erases of the session log (one in %.1f saves), none elsewhere\n",
           (unsigned)erases, 365.0 / erases);

    /* a log that is not blank where the next record goes (damage, a header torn by a power cut):
     * the save erases the other bank and goes on there. Poked in both banks: the one
     * in use is damaged, and the other then does need its erase. */
    uint32_t used = 0;
    (void)session_snapshot_log_info(&used, NULL, NULL);
    nor[flash_region_table[FLASH_REGION_SESSION].start + used + 1u] = 0x00u;
    nor[flash_region_table[FLASH_REGION_SESSION_B].start + used + 1u] = 0x00u;
    const uint32_t e0 = nor_erases[1];
    CHECK(power_off() && nor_erases[1] == e0 + 1u && log_records() == 1);
    boot();
    CHECK(session_snapshot_status()->load == SESSION_LOAD_OK);
    CHECK(nor_outside == 0 && everything_else_intact());
}

/* The keep table by itself (model.h): what the snapshot's values and the
 * settings record's view share. */
static void test_keep_table(void)
{
    printf("the keep table\n");
    fresh_device();
    int32_t v = 0;
    model_boot_reset();
    CHECK(!model_boot_kept(P(CH1_POS), NULL) && model_boot_state(P(CH1_POS)) == BOOT_KEEP_NONE);
    model_boot_keep(P(CH1_POS), 45, false);
    model_boot_keep(P(TRIG_CODE), 1, false);                    /* read only: not kept */
    model_boot_keep(NULL, 1, false);
    CHECK(model_boot_kept(P(CH1_POS), &v) && v == 45 && prop_get(P(CH1_POS)) == 0);    /* no live state moved */
    CHECK(!model_boot_kept(P(TRIG_CODE), NULL));
    /* the later keep replaces the earlier one; sticky stays */
    model_boot_keep(P(VIEW_MODE), 3, true);
    model_boot_keep(P(VIEW_MODE), 2, false);
    CHECK(model_boot_kept(P(VIEW_MODE), &v) && v == 2);
    /* a set by the user before the apply drops the keep; so does another setter moving it */
    CHECK(set(P(CH1_POS), 5) && !model_boot_kept(P(CH1_POS), NULL));
    model_boot_keep(P(TRIG_SOURCE), 0, false);
    CHECK(set(P(CH1_ENABLED), 0) && !model_boot_kept(P(TRIG_SOURCE), NULL));          /* the source followed */
    CHECK(set(P(CH1_ENABLED), 1));
    g_fail_view = true;
    CHECK(model_boot_apply(NULL) == 1 && model_boot_report()->kept == 1);
    CHECK(model_boot_kept(P(VIEW_MODE), &v) && v == 2);          /* sticky: still kept */
    g_fail_view = false;
    CHECK(model_boot_apply_one(P(VIEW_MODE)) && prop_get(P(VIEW_MODE)) == 2 && !model_boot_kept(P(VIEW_MODE), NULL));
    CHECK(!model_boot_apply_one(P(VIEW_MODE)));                  /* once */
    CHECK(model_boot_apply(NULL) == 0 && model_boot_report()->kept == 0);
    CHECK(prop_value_in_range(P(CH1_POS), 100) && !prop_value_in_range(P(CH1_POS), 101) &&
          !prop_value_in_range(P(CH1_ENABLED), 2) && !prop_value_in_range(P(CH1_MEAS), 0x1000) &&
          !prop_value_in_range(P(CH1_VDIV), -1) && !prop_value_in_range(NULL, 0));
    CHECK(set(P(VIEW_MODE), 0));
}

int main(void)
{
    nor = malloc(NOR_SIZE);
    nor_snap = malloc(NOR_SIZE);
    if (!nor || !nor_snap) { printf("FATAL: no memory\n"); return 2; }

    test_membership();
    test_format();
    test_keep_table();
    test_round_trip();
    test_fallbacks();
    test_other_tables();
    test_retired_view_id();
    test_retired_cursor_ids();
    test_refused();
    test_refused_bound();
    test_order();
    test_not_restore();
    test_never_restored();
    test_reset();
    test_flash();

    printf("test_session: %d checks, %d failed\n", checks, fails);
    free(nor); free(nor_snap);
    return fails ? 1 : 0;
}
