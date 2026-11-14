/*
 * OpenScope 2C53T - system properties (sys.*), the stored-only block
 * (sys.lang, fft.*) and the read-only UI state (ui.*)
 *
 * sys.instrument is the model's name for the active instrument (instruments
 * are sets of properties, model.h): scope, meter or gen, the table in model.c.
 * While Home or Settings is up no instrument view is, and it reads "none"
 * (there is no "last instrument" variable to report instead). sys.mode is
 * the old mode enum, Settings and Home included, kept as an alias over the
 * same current_mode. Both run the keys' mode switch (model_hw_fw.c op_mode ->
 * input_mode_switch -> enter_mode, the input task's owner of the mode
 * posture) and return when the switch is made and its posture posted; the
 * long mode keys come through sys.mode too (ORIGIN_KEY), so keys and shell
 * share the path.
 *
 * STORED ONLY. sys.lang and fft.window/avg/maxhold are in the settings
 * record but nothing in the firmware owns or applies them (config.h). Today
 * settings_store.c carries them through its g_live image untouched; they
 * have no RAM variable, so the model keeps the one 4-byte block below.
 * M3 moves the capture/apply of those four record fields onto it.
 */
#define MODEL_INTERNAL
#include "model.h"
#include "ui.h"
#include "key_model.h"
#include "sys_prefs.h"
#include "theme.h"

#define HW model_hw()

_Static_assert(MODE_OSCILLOSCOPE == 0 && MODE_MULTIMETER == 1 && MODE_SIGNAL_GEN == 2 &&
               MODE_SETTINGS == 3 && MODE_HOME == 4 && MODE_COUNT == 5,
               "sys.mode values = device_mode_t; sys.instrument 0..2 = the instrument modes");
_Static_assert(STARTUP_SCOPE == 0 && STARTUP_METER == 1 && STARTUP_HOME == 2 &&
               STARTUP_SIGGEN == 3 && STARTUP_COUNT == 4, "sys.startup values = startup_mode_t");
_Static_assert(THEME_COUNT == 4 && THEME_DARK_BLUE == 0, "sys.theme 0..3");
_Static_assert(SYS_BRIGHT_MIN == 25u && SYS_BRIGHT_MAX == 100u && SYS_BRIGHT_STEP == 5u &&
               SYS_BRIGHT_DEFAULT == 100u, "sys.brightness 25..100 step 5, default 100");
_Static_assert(SYS_VOL_MAX == 10u && SYS_VOL_DEFAULT == 1u, "sys.volume 0..10, default 1");
_Static_assert(SYS_AUTOOFF_COUNT == 4u, "sys.autooff: off/15/30/60 min");
_Static_assert(KF_COUNT == 5, "ui.focus 0..4");

/* ═══ instrument / mode ══════════════════════════════════════════════ */

#define INSTRUMENT_NONE 3
const char *const lbl_sys_instrument[4] = { "scope", "meter", "gen", "none" };
const char *const lbl_sys_mode[MODE_COUNT] = { "scope", "meter", "siggen", "settings", "home" };

/* While autoset or the baseline correction runs, a mode switch would pull
 * the front end out from under the servo: the long mode keys answer "Busy"
 * (the router's lock), and a shell set answers BUSY the same way. */
static prop_status_t mode_post(uint8_t m, uint8_t origin, prop_note_t *note)
{
    if (origin != ORIGIN_AUTO && HW->selfcal_busy && HW->selfcal_busy()) {
        model_note(note, "self-cal running");
        return PS_BUSY;
    }
    if (HW->mode) {
        if (!HW->mode(m)) { model_note(note, "mode switch NOT posted"); return PS_NOT_SET; }
        model_note(note, "posted");
        return PS_OK;
    }
    current_mode = (device_mode_t)m;       /* no owner bound: the pure part */
    return PS_OK;
}

int32_t sys_instrument_get(const prop_desc_t *p)
{
    (void)p;
    const device_mode_t m = current_mode;
    return (m <= MODE_SIGNAL_GEN) ? (int32_t)m : INSTRUMENT_NONE;
}
void sys_instrument_store(const prop_desc_t *p, int32_t v)
{
    (void)p;
    if (v != INSTRUMENT_NONE) current_mode = (device_mode_t)v;
}
prop_status_t sys_instrument_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)p;
    if (v == INSTRUMENT_NONE) { model_note(note, "not an instrument"); return PS_REFUSED; }
    const instrument_desc_t *in = model_instrument_at((uint8_t)v);
    return mode_post(in ? in->mode : (uint8_t)v, origin, note);
}

int32_t sys_mode_get(const prop_desc_t *p) { (void)p; return (int32_t)current_mode; }
void    sys_mode_store(const prop_desc_t *p, int32_t v) { (void)p; current_mode = (device_mode_t)v; }
prop_status_t sys_mode_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)p;
    return mode_post((uint8_t)v, origin, note);
}

/* ═══ startup, display, sound, power ═════════════════════════════════ */

/* startup_mode_t order. The Settings row steps in stock's order instead
 * (home, meter, scope, siggen: startup_mode_step); a later "resume last
 * session" target is appended, so no value moves. */
const char *const lbl_sys_startup[STARTUP_COUNT] = { "scope", "meter", "home", "siggen" };
int32_t sys_startup_get(const prop_desc_t *p) { (void)p; return (int32_t)startup_mode; }
void    sys_startup_store(const prop_desc_t *p, int32_t v) { (void)p; startup_mode = (startup_mode_t)v; }
prop_status_t sys_startup_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin; (void)note;
    sys_startup_store(p, v);
    return PS_OK;
}

int32_t sys_bright_get(const prop_desc_t *p) { (void)p; return sys_prefs_brightness(); }
void    sys_bright_store(const prop_desc_t *p, int32_t v) { (void)p; sys_prefs_set_brightness((uint8_t)v); }
prop_status_t sys_bright_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin; (void)note;
    sys_bright_store(p, v);
    if (HW->backlight) HW->backlight(sys_prefs_brightness());
    return PS_OK;
}

int32_t sys_volume_get(const prop_desc_t *p) { (void)p; return sys_prefs_volume(); }
void    sys_volume_store(const prop_desc_t *p, int32_t v) { (void)p; sys_prefs_set_volume((uint8_t)v); }
prop_status_t sys_volume_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin; (void)note;
    sys_volume_store(p, v);      /* the buzzer reads it at each beep */
    return PS_OK;
}

/* Index into stock's minutes table (the record stores minutes). */
static const uint8_t k_autooff_mins[SYS_AUTOOFF_COUNT] = { 0u, 15u, 30u, 60u };
const char *const lbl_sys_autooff[SYS_AUTOOFF_COUNT] = { "off", "15min", "30min", "60min" };
int32_t sys_autooff_get(const prop_desc_t *p)
{
    (void)p;
    const uint8_t m = sys_prefs_autooff_mins();
    for (int32_t i = 0; i < (int32_t)SYS_AUTOOFF_COUNT; i++)
        if (k_autooff_mins[i] == m) return i;
    return 0;
}
void sys_autooff_store(const prop_desc_t *p, int32_t v)
{
    (void)p;
    sys_prefs_set_autooff_mins(k_autooff_mins[v]);
}
prop_status_t sys_autooff_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin; (void)note;
    sys_autooff_store(p, v);
    if (HW->autooff) HW->autooff(k_autooff_mins[v]);
    return PS_OK;
}

const char *const lbl_sys_theme[THEME_COUNT] = { "blue", "green", "contrast", "red" };
int32_t sys_theme_get(const prop_desc_t *p) { (void)p; return (int32_t)theme_get_id(); }
void    sys_theme_store(const prop_desc_t *p, int32_t v) { (void)p; theme_set((theme_id_t)v); }
prop_status_t sys_theme_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin; (void)note;
    sys_theme_store(p, v);      /* screens repaint on the sys epoch */
    return PS_OK;
}

/* ═══ stored only ════════════════════════════════════════════════════ */

static struct {
    uint8_t lang;          /* 0 = English, the only one */
    uint8_t fft_window;    /* fft_window_t                       */
    uint8_t fft_avg;       /* index into k_fft_avg               */
    uint8_t fft_maxhold;
} g_stored = { 0u, 1u, 0u, 0u };

const char *const lbl_sys_lang[1] = { "en" };
int32_t sys_lang_get(const prop_desc_t *p) { (void)p; return g_stored.lang; }
void    sys_lang_store(const prop_desc_t *p, int32_t v) { (void)p; g_stored.lang = (uint8_t)v; }
prop_status_t sys_lang_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin; (void)note; sys_lang_store(p, v); return PS_OK;
}

/* fft_window_t order (fft.h). */
const char *const lbl_fft_window[5] = { "rect", "hanning", "hamming", "blackman", "flattop" };
int32_t fft_window_get(const prop_desc_t *p) { (void)p; return g_stored.fft_window; }
void    fft_window_store(const prop_desc_t *p, int32_t v) { (void)p; g_stored.fft_window = (uint8_t)v; }
prop_status_t fft_window_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin; (void)note; fft_window_store(p, v); return PS_OK;
}

/* fft_config_t.avg_count: 0 = off, 2/4/8/16/32 (an index here; the record
 * keeps the count, the codec maps). */
const char *const lbl_fft_avg[6] = { "off", "x2", "x4", "x8", "x16", "x32" };
int32_t fft_avg_get(const prop_desc_t *p) { (void)p; return g_stored.fft_avg; }
void    fft_avg_store(const prop_desc_t *p, int32_t v) { (void)p; g_stored.fft_avg = (uint8_t)v; }
prop_status_t fft_avg_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin; (void)note; fft_avg_store(p, v); return PS_OK;
}

int32_t fft_maxhold_get(const prop_desc_t *p) { (void)p; return g_stored.fft_maxhold; }
void    fft_maxhold_store(const prop_desc_t *p, int32_t v) { (void)p; g_stored.fft_maxhold = (uint8_t)(v != 0); }
prop_status_t fft_maxhold_set(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    (void)origin; (void)note; fft_maxhold_store(p, v); return PS_OK;
}

/* ═══ ui (read only) ═════════════════════════════════════════════════ */

int32_t ui_focus_get(const prop_desc_t *p)  { (void)p; return key_focus_get(); }
int32_t ui_ach_get(const prop_desc_t *p)    { (void)p; return active_channel; }
int32_t ui_screen_get(const prop_desc_t *p) { (void)p; return (int32_t)current_mode; }
bool ui_focus_label(const prop_desc_t *p, int32_t v, char *out, size_t n)
{
    (void)p;
    if (v < 0 || v >= KF_COUNT || !n) return false;
    const char *s = key_focus_name((key_focus_t)v);
    size_t i = 0;
    for (; s[i] && i + 1 < n; i++) out[i] = s[i];
    out[i] = '\0';
    return true;
}
