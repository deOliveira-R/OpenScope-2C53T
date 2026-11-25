/*
 * Host test for "Reset to defaults" (settings_store_reset_to_defaults,
 * model_reset_to_defaults, ui/settings_reset.h).
 *
 * BUILT-IN DEFAULTS are the property table's (const). USER SETTINGS are the
 * saved record. A reset copies the first over the second, through the
 * setters, and writes the record. What this file holds:
 *
 *   - every property a reset covers equals its built-in default afterwards
 *     (the table is iterated; nothing is listed by hand), and the ones it
 *     keeps (prop_reset_keeps: user calibration il.*, the screen binding,
 *     view.run) keep the value they had;
 *   - the hardware followed: the owner ops were called with the defaults
 *     (the fake ops log), and no calibration op and no mode switch was;
 *   - the record in flash is byte for byte the one config_init_defaults()
 *     builds. There are NO excepted bytes: the last-screen byte is cleared,
 *     and the bytes no property owns (display_persist_mode, padding, a
 *     stored-only byte nothing understands) are the defaults' too;
 *   - CALIBRATION IS OUT OF REACH: against a NOR model of the whole W25Q
 *     with every region outside the settings log filled (factory-cal
 *     backup, user calibration, gallery, logo, firmware cache, the stock
 *     volumes), no program and no erase lands outside the settings region,
 *     also when the reset's write makes the log compact (erase and
 *     rewrite). The MCU's own flash (the factory calibration page at
 *     0x08006000) is not in this model. What covers it: the code under
 *     test here (the model, its setters, the v5 codec, settings_store.c,
 *     config.c, the region layer) links without calpage.c, cal_backup.c,
 *     usercal.c, gallery_store.c, logo_store.c and fw_loader.c, so none of
 *     it can call them. The hardware ops are fakes here; the real ones
 *     (model_hw_fw.c) and the owner functions they call are outside this
 *     test, and were read for such calls by hand (none);
 *   - the confirm step: one OK on the row only asks; the reset needs a
 *     second OK on the question; MENU or any other key never resets;
 *   - self-cal running: BUSY, nothing changed, nothing written;
 *   - an owner that refuses: tried twice, reported, the rest is reset, the
 *     record holds the live value for the refused one;
 *   - a device already on defaults with no record: nothing is written.
 *
 * "Start with defaults for this session" (reset settings SESSION) is not
 * built; test_session_is_not_possible_with_this_store shows why (any later
 * key press saves the live state).
 *
 * Build/run:  make -C firmware test-settings-reset
 */
#define MODEL_INTERNAL
#include "model.h"
#include "view_table.h"
#include "model_persist_v5.h"
#include "config.h"
#include "settings_store.h"
#include "settings_reset.h"
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
 * with a guard: every program and erase is checked against the settings
 * region as it happens.
 * ═══════════════════════════════════════════════════════════════════ */

#define NOR_SIZE FLASH_REGION_CHIP_SIZE
static uint8_t *nor;
static uint32_t nor_programs, nor_erases, nor_outside;

static bool in_settings(uint32_t addr, uint32_t len)
{
    const flash_region_t *r = &flash_region_table[FLASH_REGION_SETTINGS];
    return addr >= r->start && len <= r->length && addr - r->start <= r->length - len;
}

static int nor_read(void *ctx, uint32_t addr, void *buf, uint32_t len)
{
    (void)ctx;
    if (addr >= NOR_SIZE || len > NOR_SIZE - addr) { printf("NOR: read out of range\n"); exit(3); }
    memcpy(buf, nor + addr, len);
    return 0;
}

static int nor_erase(void *ctx, uint32_t addr)
{
    (void)ctx;
    if (addr % FLASH_REGION_SECTOR_SIZE || addr >= NOR_SIZE) { printf("NOR: bad erase\n"); exit(3); }
    if (!in_settings(addr, FLASH_REGION_SECTOR_SIZE)) {
        nor_outside++;
        printf("  NOR: ERASE OUTSIDE THE SETTINGS REGION at 0x%06X\n", (unsigned)addr);
    }
    nor_erases++;
    memset(nor + addr, 0xFF, FLASH_REGION_SECTOR_SIZE);
    return 0;
}

static int nor_program(void *ctx, uint32_t addr, const void *data, uint32_t len)
{
    (void)ctx;
    const uint8_t *src = data;
    if (addr >= NOR_SIZE || len > NOR_SIZE - addr) { printf("NOR: program out of range\n"); exit(3); }
    if (!in_settings(addr, len)) {
        nor_outside++;
        printf("  NOR: PROGRAM OUTSIDE THE SETTINGS REGION at 0x%06X+%u\n", (unsigned)addr, (unsigned)len);
    }
    nor_programs++;
    for (uint32_t i = 0; i < len; i++) nor[addr + i] &= src[i];
    return 0;
}

static const flash_region_backend_t nor_backend = {
    .read = nor_read, .erase_sector = nor_erase, .program = nor_program, .ctx = NULL,
};
flash_region_status_t flash_regions_bind_w25q(void) { return flash_regions_init(&nor_backend); }

/* Content in every byte the settings log does not own: the factory-cal
 * backup, the user calibration log, the gallery, the logo, the firmware
 * cache, the stock volumes, the tail. A different byte per region, so a
 * write that lands in the wrong one cannot look right by accident. */
static uint8_t *nor_snap;
static void nor_fill_everything_else(void)
{
    memset(nor, 0xFF, NOR_SIZE);
    for (uint32_t i = 0; i < FLASH_REGION_COUNT; i++) {
        const flash_region_t *r = &flash_region_table[i];
        if (i == FLASH_REGION_SETTINGS) continue;
        memset(nor + r->start, (int)(0x41u + i), r->length);
    }
}
static void nor_take_snapshot(void) { memcpy(nor_snap, nor, NOR_SIZE); }
/* Every region but the settings log is exactly as the snapshot left it. */
static bool everything_else_intact(void)
{
    const flash_region_t *s = &flash_region_table[FLASH_REGION_SETTINGS];
    return memcmp(nor, nor_snap, s->start) == 0 &&
           memcmp(nor + s->start + s->length, nor_snap + s->start + s->length,
                  NOR_SIZE - (s->start + s->length)) == 0;
}

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
static bool f_enables(void)                      { logf_("enables;"); return true; }
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
static bool f_pers_on(uint8_t m)                 { logf_("pers_on(%u);", m); persist_enabled = true; g_pmode = m; return true; }
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

/* RAM as main() leaves it before the settings load (a power cycle: flash
 * survives, RAM does not), then the load. The device sits in Settings. */
static void boot(void)
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
    /* the session properties' built-in defaults (their initialisers in the
     * firmware), then the record or the defaults on top */
    (void)model_load_defaults(PF_SESSION, NULL, false);
    current_mode = MODE_SETTINGS;
    (void)settings_store_load_and_apply();
    (void)model_view_boot_apply();      /* the input task's first step (model_fw_start) */
    g_log[0] = '\0';
    g_between = 0;
    g_notes = 0;
}

/* A factory-fresh W25Q with everything else on it, then a boot. */
static void fresh_device(void)
{
    nor_fill_everything_else();
    nor_programs = nor_erases = nor_outside = 0;
    config_persist_stats_reset();
    flash_regions_stats_reset();
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

/* A value of p that is in range and is not its default. */
static int32_t other_value(const prop_desc_t *p)
{
    switch (p->type) {
    case PT_BOOL: return !p->def;
    case PT_MASK: return p->def ^ 1;
    default:      return p->def == p->max ? p->min : p->max;
    }
}

/* Move every property a set accepts off its default, as a user would over
 * time; returns how many moved. */
static unsigned change_everything(void)
{
    unsigned moved = 0;
    /* two rounds: a rule may want another property moved first */
    for (int round = 0; round < 2; round++) {
        for (uint16_t i = 0; i < prop_count(); i++) {
            const prop_desc_t *p = prop_at(i);
            if (!prop_is_settable(p) || (p->flags & PF_ALIAS)) continue;
            if (p->id == PROP_ID_SYS_INSTRUMENT || p->id == PROP_ID_IL_KNOTS) continue;
            if (prop_get(p) != prop_default(p)) continue;
            /* a channel off would refuse the other one ("Keep one CH on")
             * and the trigger source: only CH2 goes off */
            if (p->id == PROP_ID_CH1_ENABLED) continue;
            (void)prop_set_local(p, other_value(p), ORIGIN_KEY, NULL);
            if (prop_get(p) != prop_default(p)) moved++;
        }
    }
    g_il_knots = 3;                    /* a piecewise interleave map (cal user) */
    return moved;
}

static bool record_in_flash(device_config_t *out)
{
    memset(out, 0xEE, sizeof *out);
    return config_load(out);
}

/* ═══════════════════════════════════════════════════════════════════
 * 1. The confirm step
 * ═══════════════════════════════════════════════════════════════════ */

/* input_handle_settings_ok() + the MENU branch, on the pure step function:
 * the same three lines input_handler.c has. Returns true when the reset ran. */
static int8_t t_depth, t_row;
static bool press(char key)
{
    if (key == 'M') { if (t_depth > 0) t_depth = SET_PAGE_LIST; return false; }
    if (key != 'O') return false;                 /* arrows, SELECT...: not the reset's */
    switch (settings_reset_on_ok(t_depth, t_row)) {
    case SET_RESET_ASK: t_depth = SET_PAGE_RESET; return false;
    case SET_RESET_DO:
        (void)settings_store_reset_to_defaults(g_now, between);
        t_depth = SET_PAGE_LIST;
        return true;
    default: return false;
    }
}

static void test_confirm_step(void)
{
    /* The step function over every page and row: DO only on the question,
     * ASK only on the row of the list. */
    for (int d = -128; d <= 127; d++) {
        for (int r = -1; r <= SETTINGS_ITEM_COUNT; r++) {
            const settings_reset_step_t s = settings_reset_on_ok((int8_t)d, (int8_t)r);
            const settings_reset_step_t want =
                d == SET_PAGE_RESET ? SET_RESET_DO :
                (d == SET_PAGE_LIST && r == SET_ITEM_RESET_DEFAULTS) ? SET_RESET_ASK : SET_RESET_NOT_MINE;
            CHECKF(s == want, "depth %d row %d", d, r);
        }
    }
    CHECK(SET_PAGE_RESET != SET_PAGE_LIST && SET_PAGE_RESET != SET_PAGE_ABOUT &&
          SET_PAGE_RESET != SET_PAGE_SOUND);

    fresh_device();
    CHECK(change_everything() > 30);
    g_now += 5000u; key_pressed(); g_now += 5000u; key_pressed();      /* saved */
    device_config_t before, after;
    CHECK(record_in_flash(&before));
    const uint32_t programs = nor_programs;
    const int32_t vdiv = prop_get(P(CH1_VDIV));
    CHECK(vdiv != prop_default(P(CH1_VDIV)));

    /* One OK on the row: the question, and nothing else. */
    t_depth = SET_PAGE_LIST; t_row = SET_ITEM_RESET_DEFAULTS;
    CHECK(!press('O'));
    CHECK(t_depth == SET_PAGE_RESET);
    CHECK(prop_get(P(CH1_VDIV)) == vdiv && nor_programs == programs);
    /* MENU: no. Back on the list, nothing changed. */
    CHECK(!press('M'));
    CHECK(t_depth == SET_PAGE_LIST);
    CHECK(prop_get(P(CH1_VDIV)) == vdiv && nor_programs == programs);
    /* The question again; every key but OK leaves it up and resets nothing. */
    CHECK(!press('O') && t_depth == SET_PAGE_RESET);
    for (const char *k = "UDLRSP12A"; *k; k++) { CHECK(!press(*k)); CHECK(t_depth == SET_PAGE_RESET); }
    CHECK(prop_get(P(CH1_VDIV)) == vdiv && nor_programs == programs);
    CHECK(record_in_flash(&after) && memcmp(&before, &after, sizeof before) == 0);
    /* OK on another row never asks. */
    CHECK(!press('M'));
    for (int r = 0; r < SETTINGS_ITEM_COUNT; r++) {
        if (r == SET_ITEM_RESET_DEFAULTS) continue;
        t_depth = SET_PAGE_LIST; t_row = (int8_t)r;
        CHECK(!press('O') && t_depth == SET_PAGE_LIST);
    }
    CHECK(prop_get(P(CH1_VDIV)) == vdiv && nor_programs == programs);
    /* Row, OK, OK: the reset, and the list again. */
    t_depth = SET_PAGE_LIST; t_row = SET_ITEM_RESET_DEFAULTS;
    CHECK(!press('O'));
    CHECK(press('O'));
    CHECK(t_depth == SET_PAGE_LIST);
    CHECK(prop_get(P(CH1_VDIV)) == prop_default(P(CH1_VDIV)) && nor_programs > programs);
    CHECK(current_mode == MODE_SETTINGS);          /* the screen stays in Settings */
}

/* ═══════════════════════════════════════════════════════════════════
 * 2. The reset itself
 * ═══════════════════════════════════════════════════════════════════ */

static void test_reset_everything(void)
{
    fresh_device();
    const unsigned moved = change_everything();
    CHECKF(moved >= 50, "only %u properties moved", moved);
    /* an orderly power-off on the scope names the last screen; the user
     * had chosen to boot into the meter */
    CHECK(prop_set_local(P(SYS_STARTUP), STARTUP_METER, ORIGIN_KEY, NULL) == PS_OK);
    CHECK(settings_store_flush_power_off(g_now, STARTUP_SCREEN_SCOPE));
    device_config_t user, defaults, now;
    CHECK(record_in_flash(&user));
    config_init_defaults(&defaults);
    CHECK(memcmp(&user, &defaults, sizeof user) != 0);
    CHECK(user.last_screen == STARTUP_SCREEN_SCOPE);

    /* what a reset must leave alone, as it is now */
    int32_t kept[PROP_IX_COUNT];
    unsigned n_kept = 0, n_covered = 0;
    for (uint16_t i = 0; i < prop_count(); i++) kept[i] = prop_get(prop_at(i));
    CHECK(prop_get(P(IL_GAIN)) != prop_default(P(IL_GAIN)));
    CHECK(prop_get(P(IL_OFFSET)) != prop_default(P(IL_OFFSET)));
    CHECK(prop_get(P(IL_SKEW)) != prop_default(P(IL_SKEW)));
    CHECK(prop_get(P(IL_KNOTS)) == 3);
    CHECK(prop_get(P(VIEW_RUN)) != prop_default(P(VIEW_RUN)));

    g_log[0] = '\0';
    g_between = 0;
    const uint32_t notes = g_notes;
    const settings_reset_result_t r = settings_store_reset_to_defaults(g_now, between);
    CHECK(r.status == PS_OK && r.left == 0 && r.first_left == 0);
    CHECKF(r.changed >= 50, "changed %u", (unsigned)r.changed);
    CHECK(r.saved == SETTINGS_FLUSH_SAVED);
    CHECK(g_between >= r.changed);                 /* the health check-in: between the setters, and
                                                    * once more before the write */
    CHECK(g_notes == notes);                       /* no settings note per property: one write */

    /* every property: its built-in default, or kept as it was */
    for (uint16_t i = 0; i < prop_count(); i++) {
        const prop_desc_t *p = prop_at(i);
        if (prop_reset_keeps(p)) {
            n_kept++;
            if (!(p->flags & PF_RO))
                CHECKF(prop_get(p) == kept[i], "%s was kept but moved: %ld -> %ld", p->name,
                       (long)kept[i], (long)prop_get(p));
        } else {
            n_covered++;
            CHECKF(prop_get(p) == prop_default(p), "%s = %ld, default %ld", p->name,
                   (long)prop_get(p), (long)prop_default(p));
            CHECK(!prop_is_changed(p));
        }
    }
    CHECKF(n_covered >= 55 && n_kept >= 8, "covered %u kept %u", n_covered, n_kept);
    /* the kept list, by name: nothing else may join it unnoticed */
    static const char *const k_kept[] = {
        "trig.code", "trig.held", "mask.state", "mask.hold", "mask.pending",
        "meter.live", "sys.instrument", "sys.mode", "sys.lastscreen",
        "il.gain", "il.offset", "il.skew", "il.knots", "ui.focus", "ui.ach", "ui.screen",
    };
    CHECKF(n_kept == sizeof k_kept / sizeof k_kept[0], "%u kept", n_kept);
    for (unsigned i = 0; i < sizeof k_kept / sizeof k_kept[0]; i++)
        CHECKF(prop_reset_keeps(prop_find(k_kept[i])) != NULL, "%s", k_kept[i]);
    CHECK(current_mode == MODE_SETTINGS);          /* the screen stays */

    /* the hardware followed (what the owners were told) */
    static const char *const k_ops[] = {
        "vdiv(0,3);", "vdiv(1,5);", "coupling(0,0);", "coupling(1,0);", "enables;",
        "vpos(0,0);", "vpos(1,0);", "triglvl(0);",
        "gen_wave(0);", "gen_freq(1000);", "gen_amp(30);", "gen_duty(50);", "gen_out(0);",
        "gen_boot(0);", "meter(0,0);", "backlight(100);", "autooff(0);", "il_en(1);",
        "mask_tol(8,2);", "mask_sf(1);", "pers_off;", "layout(0);",
    };
    for (unsigned i = 0; i < sizeof k_ops / sizeof k_ops[0]; i++)
        CHECKF(logged(k_ops[i]), "no %s in: %s", k_ops[i], g_log);
    /* and nobody touched calibration or the screen binding */
    CHECK(!logged("il_corr(") && !logged("il_skew(") && !logged("mode("));
    CHECK(siggen_get_config()->output_enabled == false);

    /* the record: byte for byte the built-in defaults' */
    CHECK(record_in_flash(&now));
    CHECK(memcmp(&now, &defaults, sizeof now) == 0);
    for (unsigned b = 0; b < sizeof now; b++)
        CHECKF(((const uint8_t *)&now)[b] == ((const uint8_t *)&defaults)[b], "record byte %u", b);
    CHECK(now.last_screen == STARTUP_SCREEN_NONE);
    CHECK(now.startup_mode == startup_mode_to_config(STARTUP_RESTORE));
    CHECK(now.meter_link == 0);

    /* flash: the settings log only */
    CHECK(nor_outside == 0);
    CHECK(everything_else_intact());

    /* the next boot: defaults, Startup = restore, no last screen (Home) */
    boot();
    CHECK(prop_get(P(SYS_STARTUP)) == STARTUP_RESTORE);
    CHECK(model_last_screen() == STARTUP_SCREEN_NONE);
    for (uint16_t i = 0; i < prop_count(); i++) {
        const prop_desc_t *p = prop_at(i);
        if (p->flags & PF_PERSIST) CHECKF(prop_get(p) == prop_default(p), "%s after the boot", p->name);
    }
    /* and a later orderly power-off names a screen again (the byte's meaning is unchanged) */
    CHECK(settings_store_flush_power_off(g_now, STARTUP_SCREEN_METER));
    CHECK(record_in_flash(&now) && now.last_screen == STARTUP_SCREEN_METER);
    CHECK(nor_outside == 0 && everything_else_intact());
}

/* A reset on a second run finds nothing to do and writes nothing. */
static void test_reset_twice_and_untouched_device(void)
{
    fresh_device();
    /* no record, everything on defaults: nothing changes, nothing is written */
    settings_reset_result_t r = settings_store_reset_to_defaults(g_now, between);
    CHECK(r.status == PS_OK && r.changed == 0 && r.left == 0);
    CHECK(r.saved == SETTINGS_FLUSH_CLEAN);
    CHECK(nor_programs == 0 && nor_erases == 0);
    CHECK(g_log[0] == '\0');                       /* no owner call at all */
    CHECK(memcmp(nor, nor_snap, NOR_SIZE) == 0);   /* the whole chip, the settings log included */

    CHECK(prop_set_local(P(SYS_BRIGHTNESS), 50, ORIGIN_KEY, NULL) == PS_OK);
    CHECK(prop_set_local(P(TB_CODE), 4, ORIGIN_KEY, NULL) == PS_OK);
    /* changed and changed back before anything was saved: still nothing to write */
    r = settings_store_reset_to_defaults(g_now, between);
    CHECK(r.status == PS_OK && r.changed == 2 && r.saved == SETTINGS_FLUSH_CLEAN && nor_programs == 0);
    CHECK(logged("tb(10);") && logged("backlight(100);"));   /* roll off: reg 0x01 is the timebase */

    CHECK(prop_set_local(P(SYS_BRIGHTNESS), 50, ORIGIN_KEY, NULL) == PS_OK);
    CHECK(prop_set_local(P(TB_CODE), 4, ORIGIN_KEY, NULL) == PS_OK);
    CHECK(settings_store_flush(g_now));                      /* now they are the user settings */
    r = settings_store_reset_to_defaults(g_now, between);
    CHECKF(r.status == PS_OK && r.changed == 2 && r.saved == SETTINGS_FLUSH_SAVED, "status %u changed %u saved %u",
           (unsigned)r.status, (unsigned)r.changed, (unsigned)r.saved);
    CHECK(prop_set_local(P(TRIG_SOURCE), 1, ORIGIN_KEY, NULL) == PS_OK);
    CHECK(prop_set_local(P(VIEW_MATH), 4, ORIGIN_KEY, NULL) == PS_OK);
    CHECK(prop_set_local(P(VIEW_MATH), 0, ORIGIN_KEY, NULL) == PS_OK);      /* off, operator 3 remembered */
    CHECK(math_op == 3);
    CHECK(settings_store_flush(g_now));
    g_log[0] = '\0';
    r = settings_store_reset_to_defaults(g_now, between);
    CHECK(r.status == PS_OK && r.saved == SETTINGS_FLUSH_SAVED && logged("trigsrc;"));
    CHECK(math_op == 0 && !math_enabled);          /* the remembered operator is the default's too */
    const uint32_t programs = nor_programs;
    g_log[0] = '\0';
    r = settings_store_reset_to_defaults(g_now, between);
    CHECK(r.status == PS_OK && r.changed == 0 && r.saved == SETTINGS_FLUSH_CLEAN);
    CHECK(nor_programs == programs && g_log[0] == '\0');
    CHECK(nor_outside == 0 && everything_else_intact());
}

/* The bytes no property owns are the defaults' too. */
static void test_carried_bytes_go_back(void)
{
    fresh_device();
    device_config_t rec, now, defaults;
    config_init_defaults(&rec);
    rec.display_persist_mode = 7;                  /* no property (V5_CARRY) */
    rec.language = 0xEE;                           /* stored only, a byte nothing understands */
    rec.fft_window = 3;
    rec.fft_avg_count = 16;
    rec.fft_max_hold = 1;
    rec.last_screen = STARTUP_SCREEN_SIGGEN;
    ((uint8_t *)&rec)[23] = 0xA5;                  /* view.mode, a byte no view has: yt */
    rec.checksum = config_compute_checksum(&rec);
    CHECK(config_save(&rec));
    boot();
    CHECK(model_last_screen() == STARTUP_SCREEN_SIGGEN);
    CHECK(prop_get(P(VIEW_MODE)) == 0 && model_view_record() == 0u);
    CHECK(prop_get(P(FFT_WINDOW)) == 3 && prop_get(P(FFT_AVG)) == 4 && prop_get(P(FFT_MAXHOLD)) == 1);
    nor_take_snapshot();

    const settings_reset_result_t r = settings_store_reset_to_defaults(g_now, between);
    CHECK(r.status == PS_OK && r.left == 0 && r.saved == SETTINGS_FLUSH_SAVED);
    config_init_defaults(&defaults);
    CHECK(record_in_flash(&now));
    CHECK(memcmp(&now, &defaults, sizeof now) == 0);
    CHECK(now.display_persist_mode == 0 && now.language == 0 && ((uint8_t *)&now)[23] == 0);
    CHECK(nor_outside == 0 && everything_else_intact());
}

/* The display view (view.mode, record byte 23). In force: the reset takes it
 * back to Y-T through its owner. Kept by a boot that could not show it (the
 * pool refused): view.mode already reads its default, so the reset loop never
 * calls the setter, and the kept view must go all the same or the reset's own
 * save writes it straight back. Self-cal running: it stays kept. */
static void test_view_goes_back(void)
{
    device_config_t rec, now, defaults;
    config_init_defaults(&defaults);

    /* in force */
    fresh_device();
    CHECK(prop_set_local(P(VIEW_MODE), VIEW_MODE_WATERFALL, ORIGIN_KEY, NULL) == PS_OK);
    g_now += 5000u; key_pressed(); g_now += 5000u; key_pressed();
    CHECK(record_in_flash(&now) && now.view_mode == 5u);
    boot();                                         /* a power cycle: it comes back */
    CHECK(prop_get(P(VIEW_MODE)) == VIEW_MODE_WATERFALL && scope_view == SCOPE_VIEW_WATERFALL);
    CHECK(g_notes == 0);
    g_now += 5000u; key_pressed(); g_now += 5000u; key_pressed();
    CHECK(record_in_flash(&rec) && memcmp(&rec, &now, sizeof rec) == 0);    /* and wrote nothing */
    nor_take_snapshot();
    g_log[0] = '\0';
    settings_reset_result_t r = settings_store_reset_to_defaults(g_now, between);
    CHECK(r.status == PS_OK && r.left == 0 && r.saved == SETTINGS_FLUSH_SAVED);
    CHECK(strstr(g_log, "view(0);") != NULL && prop_get(P(VIEW_MODE)) == 0 && scope_view == SCOPE_VIEW_TIME);
    CHECK(record_in_flash(&now) && memcmp(&now, &defaults, sizeof now) == 0);
    boot();
    CHECK(prop_get(P(VIEW_MODE)) == 0);

    /* kept, not in force */
    fresh_device();
    rec = defaults;
    rec.view_mode = 5;
    rec.checksum = config_compute_checksum(&rec);
    CHECK(config_save(&rec));
    g_fail_view = true;
    boot();
    CHECK(prop_get(P(VIEW_MODE)) == 0 && scope_view == SCOPE_VIEW_TIME && model_view_record() == 5u);
    g_now += 5000u; key_pressed(); g_now += 5000u; key_pressed();
    CHECK(record_in_flash(&now) && memcmp(&now, &rec, sizeof now) == 0);    /* that boot alone: nothing written */
    /* self-cal running: refused, and the view is still kept */
    g_busy = true;
    r = settings_store_reset_to_defaults(g_now, between);
    g_busy = false;
    CHECK(r.status == PS_BUSY && model_view_record() == 5u);
    nor_take_snapshot();
    r = settings_store_reset_to_defaults(g_now, between);
    CHECK(r.status == PS_OK && r.left == 0 && r.saved == SETTINGS_FLUSH_SAVED);
    CHECK(model_view_record() == 0u);
    CHECK(record_in_flash(&now) && memcmp(&now, &defaults, sizeof now) == 0);
    CHECK(nor_outside == 0 && everything_else_intact());
    g_fail_view = false;
    boot();                                         /* the pool is free again: still Y-T */
    CHECK(prop_get(P(VIEW_MODE)) == 0 && scope_view == SCOPE_VIEW_TIME);
}

/* Self-cal running: nothing at all. */
static void test_busy(void)
{
    fresh_device();
    CHECK(change_everything() > 30);
    g_now += 5000u; key_pressed(); g_now += 5000u; key_pressed();
    int32_t was[PROP_IX_COUNT];
    for (uint16_t i = 0; i < prop_count(); i++) was[i] = prop_get(prop_at(i));
    const uint32_t programs = nor_programs, erases = nor_erases;
    device_config_t before, after;
    CHECK(record_in_flash(&before));
    g_log[0] = '\0';
    g_busy = true;
    const settings_reset_result_t r = settings_store_reset_to_defaults(g_now, between);
    g_busy = false;
    CHECK(r.status == PS_BUSY && r.changed == 0);
    CHECK(g_log[0] == '\0');
    for (uint16_t i = 0; i < prop_count(); i++) CHECK(prop_get(prop_at(i)) == was[i]);
    CHECK(nor_programs == programs && nor_erases == erases);
    CHECK(record_in_flash(&after) && memcmp(&before, &after, sizeof before) == 0);
    /* the last-screen byte was not cleared either */
    CHECK(model_last_screen_record() == before.last_screen);
}

/* An owner that says no (a coupling relay that does not read back). */
static void test_owner_refuses(void)
{
    fresh_device();
    CHECK(prop_set_local(P(CH1_COUPLING), 1, ORIGIN_KEY, NULL) == PS_OK);
    CHECK(prop_set_local(P(CH2_VDIV), 8, ORIGIN_KEY, NULL) == PS_OK);
    CHECK(prop_set_local(P(GEN_FREQ), 440, ORIGIN_KEY, NULL) == PS_OK);
    g_log[0] = '\0';
    g_fail_coupling = true;
    const settings_reset_result_t r = settings_store_reset_to_defaults(g_now, between);
    g_fail_coupling = false;
    CHECK(r.status == PS_NOT_SET && r.left == 1 && r.first_left == PROP_ID_CH1_COUPLING);
    CHECK(r.changed == 2);
    CHECK(logged_times("coupling(0,0);") == 2);    /* tried, and once more after the others */
    CHECK(prop_get(P(CH1_COUPLING)) == 1);         /* the wire, not the intent */
    CHECK(prop_get(P(CH2_VDIV)) == prop_default(P(CH2_VDIV)));
    CHECK(prop_get(P(GEN_FREQ)) == prop_default(P(GEN_FREQ)));
    /* saved as it is: the defaults, and AC where the relay stayed */
    CHECK(r.saved == SETTINGS_FLUSH_SAVED);
    device_config_t now, defaults;
    config_init_defaults(&defaults);
    CHECK(record_in_flash(&now));
    CHECK(now.scope_ch1_coupling == 1);
    now.scope_ch1_coupling = defaults.scope_ch1_coupling;
    now.checksum = config_compute_checksum(&now);
    CHECK(memcmp(&now, &defaults, sizeof now) == 0);
    CHECK(nor_outside == 0 && everything_else_intact());
}

/* A bank that is nearly full: the reset's write makes the log move to its other
 * bank, erasing it. Still nothing outside the settings region. */
static void test_reset_when_the_log_compacts(void)
{
    fresh_device();
    unsigned writes = 0;
    for (unsigned i = 0; i < 6000u && nor_erases == 0; i++) {
        CHECK(prop_set_local(P(GEN_FREQ), 100 + (int32_t)(i % 2000u), ORIGIN_KEY, NULL) == PS_OK);
        if (settings_store_flush(g_now)) writes++;
    }
    CHECKF(nor_erases > 0, "%u writes and the log never compacted", writes);
    /* The first erase came with the second move, after two banks of writes. Fill
     * this bank to just short of the next move. */
    const uint32_t erases_per = nor_erases;
    const unsigned per_log = writes / 2u;
    for (unsigned i = 0; i + 2u < per_log; i++) {
        CHECK(prop_set_local(P(GEN_FREQ), 3000 + (int32_t)(i % 2000u), ORIGIN_KEY, NULL) == PS_OK);
        (void)settings_store_flush(g_now);
    }
    CHECK(nor_erases == erases_per);
    CHECK(change_everything() > 30);
    (void)settings_store_flush(g_now);
    const uint32_t erases = nor_erases;
    settings_reset_result_t r = { 0 };
    /* a few resets with a change in between: one of the writes compacts */
    for (int k = 0; k < 4; k++) {
        CHECK(prop_set_local(P(GEN_FREQ), 777 + k, ORIGIN_KEY, NULL) == PS_OK);
        (void)settings_store_flush(g_now);
        r = settings_store_reset_to_defaults(g_now, between);
        CHECK(r.status == PS_OK && r.saved == SETTINGS_FLUSH_SAVED);
    }
    CHECKF(nor_erases > erases, "no compaction around the reset (%u erases)", (unsigned)nor_erases);
    device_config_t now, defaults;
    config_init_defaults(&defaults);
    CHECK(record_in_flash(&now) && memcmp(&now, &defaults, sizeof now) == 0);
    CHECK(nor_outside == 0);
    CHECK(everything_else_intact());
}

/* ═══════════════════════════════════════════════════════════════════
 * 3. Why `reset settings SESSION` is not built
 * ═══════════════════════════════════════════════════════════════════ */

/* "Defaults for this session, the user settings untouched" would need the
 * record to stay as it is until the user changes something. With this
 * store it does not: every handled key captures the live state and
 * compares it with the saved image, so the first key press after such a
 * reset starts a write of the defaults, whatever the key was. This test
 * pins that behaviour, so whoever builds SESSION knows what has to change
 * first (a hold in the store). */
static void test_session_is_not_possible_with_this_store(void)
{
    fresh_device();
    CHECK(prop_set_local(P(SYS_BRIGHTNESS), 50, ORIGIN_KEY, NULL) == PS_OK);
    CHECK(prop_set_local(P(TB_CODE), 4, ORIGIN_KEY, NULL) == PS_OK);
    (void)settings_store_flush(g_now);
    device_config_t user, now;
    CHECK(record_in_flash(&user));

    /* the defaults over the live state, with no note and no write: what a
     * SESSION reset would do */
    model_reset_report_t m;
    CHECK(model_reset_to_defaults(NULL, &m) == PS_OK && m.changed == 2);
    CHECK(record_in_flash(&now) && memcmp(&user, &now, sizeof user) == 0);   /* untouched so far */

    /* an arrow key that changes nothing, then any key 2 s later */
    key_pressed();
    g_now += SETTINGS_STORE_SETTLE_MS + 100u;
    key_pressed();
    CHECK(record_in_flash(&now));
    CHECK(memcmp(&user, &now, sizeof user) != 0);          /* the user settings are gone */
    CHECK(now.display_brightness == SYS_BRIGHT_DEFAULT);
    CHECK(nor_outside == 0 && everything_else_intact());
}

int main(void)
{
    nor = malloc(NOR_SIZE);
    nor_snap = malloc(NOR_SIZE);
    if (!nor || !nor_snap) { printf("FATAL: flash model alloc\n"); return 2; }

    test_confirm_step();
    test_reset_everything();
    test_reset_twice_and_untouched_device();
    test_carried_bytes_go_back();
    test_view_goes_back();
    test_busy();
    test_owner_refuses();
    test_reset_when_the_log_compacts();
    test_session_is_not_possible_with_this_store();

    printf("test_settings_reset: %d checks, %d failures\n", checks, fails);
    free(nor);
    free(nor_snap);
    return fails ? 1 : 0;
}
