/*
 * The firmware's screens for host tests (tests/test_screen_router.c part 2,
 * tests/test_settings_notify.c): src/ui/ui_router.c with every REAL screen on
 * the real property model (screen_test_env.h). Faked here is only what a
 * screen may not do itself, and all of it logs into R: the paints (display
 * task), the owners' procedures, the Settings screens' environment and the
 * router's environment (ui_env_*: the click, the display queue, the settings
 * service, the screenshot, the POWER countdown).
 *
 * fw_boot() brings up one of eleven states (every base, the generator's
 * editor, the Oscilloscope Settings page full and locked, the Settings
 * question and pages) by the keys a user presses, with nothing, the gallery
 * or the logo over it.
 *
 * Include once, after screen_test_env.h (define SCREEN_TEST_ENV_REAL_FOCUS
 * first: the scope screen is linked and owns the arrow stage).
 */
#ifndef FW_SCREENS_ENV_H
#define FW_SCREENS_ENV_H

#define FW_UNUSED __attribute__((unused))

#include "ui_router.h"
#include "home_screen.h"
#include "home_model.h"
#include "settings_screen.h"
#include "siggen_screen.h"
#include "siggen_edit.h"
#include "meter_screen.h"
#include "scope_screen.h"
#include "osc_menu_screen.h"
#include "gallery_screen.h"
#include "gallery_store.h"
#include "overlay_screens.h"
#include "scope_mask.h"
#include "persistence.h"
#include "fft.h"

/* ── the log ───────────────────────────────────────────────────────── */
static char R[256];
static void r(const char *fmt, int a, int b)
{
    char t[40];
    snprintf(t, sizeof t, fmt, a, b);
    if (strlen(R) + strlen(t) + 2 < sizeof R) { strcat(R, t); strcat(R, " "); }
}
FW_UNUSED static bool r_is(const char *want)
{
    char w[sizeof R];
    snprintf(w, sizeof w, "%s%s", want, want[0] ? " " : "");
    return strcmp(R, w) == 0;
}
FW_UNUSED static bool r_has(const char *tok) { return strstr(R, tok) != NULL; }
FW_UNUSED static int  r_count(const char *tok)
{
    int n = 0;
    for (const char *p = R; (p = strstr(p, tok)) != NULL; p += strlen(tok)) n++;
    return n;
}

/* ── the paints (display task): counted, nothing more ──────────────── */
static int g_fw_paints;
void home_ui_draw(uint8_t sel)                    { (void)sel; g_fw_paints++; }
void home_ui_draw_selection(uint8_t sel)          { (void)sel; g_fw_paints++; }
void siggen_ui_draw(void)                         { g_fw_paints++; }
void settings_ui_draw_list(uint8_t row)           { (void)row; g_fw_paints++; }
void settings_ui_draw_reset_question(uint8_t row) { (void)row; g_fw_paints++; }
void settings_ui_draw_sound(uint8_t sub)          { (void)sub; g_fw_paints++; }
void settings_ui_draw_about(void)                 { g_fw_paints++; }
void osc_menu_draw(void)                          { g_fw_paints++; }
void osc_menu_invalidate(void)                    { }
void gallery_ui_paint(scr_paint_t how)            { (void)how; g_fw_paints++; }
void logo_ui_paint(void)                          { g_fw_paints++; logo_screen_mark_painted(); }
void power_prompt_ui_paint(power_prompt_view_t v) { (void)v; g_fw_paints++; }
void scope_view_draw(scr_paint_t how, uint32_t frame) { (void)how; (void)frame; g_fw_paints++; }
redraw_action_t scope_view_tick(uint32_t frame)   { (void)frame; return REDRAW_SKIP; }
uint32_t scope_view_fft_epoch(void)               { return 0; }
void meter_ui_draw(scr_paint_t how)               { (void)how; g_fw_paints++; }

/* ── what the screens ask of their owners ──────────────────────────── */
static char g_fw_popup[32];
void scope_show_popup(const char *text)           { snprintf(g_fw_popup, sizeof g_fw_popup, "%s", text);
                                                    if (strncmp(text, "Busy", 4) == 0) r("busy", 0, 0); }
void scope_ui_request_live(void)                  { }
bool scope_selfcal_request_autoset(void)          { r("autoset", 0, 0); return true; }
bool scope_selfcal_request_basecal(uint8_t banks) { (void)banks; r("basecal", 0, 0); return true; }
bool scope_selfcal_request_center50(void)         { r("center50", 0, 0); return true; }
uint32_t scope_mask_post(scope_mask_req_t k, uint8_t a, uint8_t b, uint16_t n) { (void)k; (void)a; (void)b; (void)n; return 1; }
bool scope_mask_key_auto(char *text, unsigned n)  { snprintf(text, n, "MASK"); return false; }
persist_mode_t persist_get_mode(void)             { return PERSIST_OFF; }
float fft_adjust_ref_level(float d)               { (void)d; return 0.0f; }
void fft_auto_request(void)                       { }
fft_window_t fft_cycle_window(void)               { return (fft_window_t)0; }
void fft_zoom_in(void)                            { }
void fft_zoom_out(void)                           { }
void meter_ui_autoselect_cancel(void)             { }
bool meter_ui_autoselect_running(void)            { return false; }
bool meter_ui_cal_set(void)                       { return false; }
void meter_ui_cal_text(char *b, uint8_t n)        { if (n) b[0] = '\0'; }
void meter_ui_fuse_cal(void)                      { }
uint32_t meter_ui_gen(void)                       { return 0; }
bool meter_ui_limit_active(uint8_t which)         { (void)which; return false; }
void meter_ui_limit_press(uint8_t which)          { (void)which; }
void meter_ui_limit_text(uint8_t which, char *b, uint8_t n) { (void)which; if (n) b[0] = '\0'; }
void meter_ui_limits_nudge(int8_t dir)            { (void)dir; }
bool meter_ui_periodic(void)                      { return false; }
uint32_t meter_ui_reading_seq(void)               { return 0; }
uint32_t meter_ui_rendered_seq(void)              { return 0; }
void meter_ui_stats_reset(void)                   { }
/* Three stored shots to browse. */
gallery_status_t gallery_scan(gallery_index_t *idx)
{
    memset(idx, 0, sizeof *idx);
    idx->count = 3; idx->seq[0] = 3; idx->seq[1] = 2; idx->seq[2] = 1; idx->next_seq = 4;
    return GALLERY_OK;
}
gallery_status_t gallery_delete(uint32_t seq)     { (void)seq; return GALLERY_OK; }
uint32_t gallery_generation(void)                 { return 0; }
/* The Settings screens' environment. */
void settings_env_click(void)                     { }
void settings_env_page_left(void)                 { r("pageleft", 0, 0); }
void settings_env_reset(void)                     { r("reset", 0, 0); }
void settings_env_usb_share_toggle(void)          { r("usb", 0, 0); }
void settings_env_fw_update(void)                 { r("fwupdate", 0, 0); }

/* ── the router's environment (tasks/input_handler.c in the firmware) ── */
static bool fw_busy;                 /* autoset / baseline running */
static int  fw_lock_depth;
static bool fw_power_saw_gallery, fw_power_saw_prompt;
void ui_env_lock(void)             { fw_lock_depth++; }
void ui_env_unlock(void)           { fw_lock_depth--; }
void ui_env_touch(void)            { r("touch", 0, 0); }
void ui_env_click(void)            { r("click", 0, 0); }
void ui_env_post(uint8_t dcmd)
{
    if (dcmd == DCMD_REDRAW_ALL) r("post:all", 0, 0);
    else if (dcmd >= DCMD_DRAW_ACTIVE && dcmd < DCMD_DRAW_ACTIVE_END) r("post:%d", dcmd - DCMD_DRAW_ACTIVE, 0);
    else r("post:?%d", dcmd, 0);
}
/* After a key. fw_service_hook: what a test runs there (the real settings
 * store's service, in test_settings_notify.c). */
static void (*fw_service_hook)(void);
void ui_env_settings_service(void) { r("svc", 0, 0); if (fw_service_hook) fw_service_hook(); }
void ui_env_screenshot(void)       { r("shot", 0, 0); }
/* The countdown as input_handler.c runs it, released at once: the box up,
 * then down. What the stack looked like while it was up is kept. */
void ui_env_power_key(void)
{
    r("power", 0, 0);
    fw_power_saw_gallery = screen_is_open(SCR_GALLERY);
    power_prompt_show(POWER_PROMPT_HOLD, 3);
    fw_power_saw_prompt = (screen_top() == SCR_POWER_PROMPT) && power_prompt_up();
    power_prompt_hide();
}
bool scope_selfcal_busy(void)           { return fw_busy; }
void scope_selfcal_request_cancel(void) { r("cancel", 0, 0); }

/* The mode op as tasks/input_handler.c input_mode_switch makes it: the real
 * ui_router_switch_mode, then (if the base changed) the posture, the flush
 * and the repaint, logged as one token. */
static bool fw_mode(uint8_t m)
{
    const bool switched = ui_router_switch_mode((device_mode_t)m);
    r(switched ? "mode:%d" : "mode:%d=", m, 0);
    return true;
}
static model_hw_ops_t fw_ops;
/* The model's settings note (model_hw_ops_t.note_change), if a test wants the
 * real one; else screen_test_env.h's counter. */
static void (*fw_note_hook)(void);

/* ── states ────────────────────────────────────────────────────────── */
#define LONG(b) ((uint8_t)((b) | BTN_LONG_FLAG))

typedef enum { ST_HOME, ST_METER, ST_GEN, ST_GEN_EDIT, ST_SCOPE, ST_SCOPE_PAGE, ST_SCOPE_PANE,
               ST_SET, ST_SET_QUESTION, ST_SET_SOUND, ST_SET_ABOUT, ST_COUNT } fw_state_t;
typedef enum { OV_NONE, OV_GALLERY, OV_LOGO, OV_COUNT } fw_over_t;
FW_UNUSED static const char *const st_name[ST_COUNT] = { "home", "meter", "generator", "generator (editing)", "scope",
    "scope > page", "scope > locked pane", "settings", "settings (question)", "settings > sound", "settings > about" };
static const device_mode_t st_mode[ST_COUNT] = { MODE_HOME, MODE_MULTIMETER, MODE_SIGNAL_GEN, MODE_SIGNAL_GEN,
    MODE_OSCILLOSCOPE, MODE_OSCILLOSCOPE, MODE_OSCILLOSCOPE, MODE_SETTINGS, MODE_SETTINGS, MODE_SETTINGS, MODE_SETTINGS };

static void fw_key(uint8_t raw) { ui_router_key((button_id_t)raw); }
FW_UNUSED static void fw_keys(uint8_t raw, int n) { while (n-- > 0) fw_key(raw); }

/* A device booted into `st`, reached by the keys a user presses, with the
 * overlay `ov` over it; then a clean log. */
static void fw_boot(fw_state_t st, fw_over_t ov, bool busy)
{
    env_fresh(st_mode[st]);
    fw_ops = env_ops;
    fw_ops.mode = fw_mode;
    if (fw_note_hook) fw_ops.note_change = fw_note_hook;
    model_bind_hw(&fw_ops);
    fw_busy = false;
    /* UI state the screens keep between boots of one test run */
    scope_key_focus = KF_ZOOM;
    scope_trig_level_focus = scope_hpos_focus = scope_mask_focus = false;
    if (st == ST_HOME) {
        /* Home entered from the scope (it preselects the screen it was opened
         * from), so its selection is known: Oscilloscope. */
        current_mode = MODE_OSCILLOSCOPE;
        ui_router_init(MODE_OSCILLOSCOPE);
        (void)prop_set_local(P(SYS_MODE), MODE_HOME, ORIGIN_SHELL, NULL);
    } else {
        ui_router_init(st_mode[st]);
    }
    siggen_ui_editor()->active = false;
    switch (st) {
    case ST_GEN_EDIT:     fw_key(BTN_PRM); break;
    case ST_SCOPE_PAGE:   fw_key(BTN_MENU); break;
    case ST_SCOPE_PANE:   fw_key(BTN_TRIGGER); break;
    case ST_SET_QUESTION: fw_keys(BTN_DOWN, SET_ITEM_RESET_DEFAULTS); fw_key(BTN_OK); break;
    case ST_SET_SOUND:    fw_key(BTN_OK); break;
    case ST_SET_ABOUT:    fw_keys(BTN_DOWN, SET_ITEM_ABOUT); fw_key(BTN_OK); break;
    default: break;
    }
    if (ov == OV_GALLERY) fw_key(LONG(BTN_SAVE));
    if (ov == OV_LOGO)    (void)logo_screen_show(false);
    fw_busy = busy;
    fw_power_saw_gallery = fw_power_saw_prompt = false;
    g_fw_popup[0] = '\0';
    R[0] = '\0';
    env_log_clear();
}

static const char *fw_stack(void)
{
    static char s[80];
    const screen_stack_t st = screen_snapshot();
    s[0] = '\0';
    for (uint8_t i = 0; i < st.n; i++) {
        const screen_t *sc = screen_get(st.id[i]);
        snprintf(s + strlen(s), sizeof s - strlen(s), "%s%s", i ? ">" : "", sc ? sc->name : "?");
    }
    return s;
}
FW_UNUSED static bool stack_is(const char *want) { return strcmp(fw_stack(), want) == 0; }
FW_UNUSED static bool mirror_ok(void)
{
    const screen_t *b = screen_get(screen_base());
    return b != NULL && current_mode == b->mode;
}

#endif /* FW_SCREENS_ENV_H */
