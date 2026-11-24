/*
 * OpenScope 2C53T - the shell's UI route (ui_shell.h): `ui press/long/seq/
 * screen`.
 *
 * The first half (key names, the `ui seq` token parser) is pure and host
 * tested. The second half is the firmware's: it injects into the key queue
 * and waits on the input task's handled counter.
 */
#include "ui_shell.h"
#include "ui.h"          /* button_id_t, BTN_*, device_mode_t */
#include "key_model.h"   /* KEYBIT_*, KEY_LONG_MASK            */

#include <string.h>

/* ═══ key names (pure) ════════════════════════════════════════════════ */

/* The keys, then POWER for the refusal. */
static const struct { const char *name; uint8_t id; uint8_t bit; } k_keys[] = {
    { "CH1",     BTN_CH1,     KEYBIT_CH1     }, { "CH2",     BTN_CH2,     KEYBIT_CH2     },
    { "MOVE",    BTN_MOVE,    KEYBIT_MOVE    }, { "SELECT",  BTN_SELECT,  KEYBIT_SELECT  },
    { "TRIGGER", BTN_TRIGGER, KEYBIT_TRIGGER }, { "PRM",     BTN_PRM,     KEYBIT_PRM     },
    { "AUTO",    BTN_AUTO,    KEYBIT_AUTO    }, { "SAVE",    BTN_SAVE,    KEYBIT_SAVE    },
    { "MENU",    BTN_MENU,    KEYBIT_MENU    }, { "UP",      BTN_UP,      KEYBIT_UP      },
    { "DOWN",    BTN_DOWN,    KEYBIT_DOWN    }, { "LEFT",    BTN_LEFT,    KEYBIT_LEFT    },
    { "RIGHT",   BTN_RIGHT,   KEYBIT_RIGHT   }, { "OK",      BTN_OK,      KEYBIT_OK      },
    { "POWER",   BTN_POWER,   KEYBIT_POWER   },
};
#define N_KEYS (sizeof k_keys / sizeof k_keys[0])

static char up(char c)  { return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c; }

int ui_key_find(const char *name, size_t len)
{
    if (!name) return -1;
    for (unsigned i = 0; i < N_KEYS; i++) {
        const char *k = k_keys[i].name;
        if (strlen(k) != len) continue;
        size_t j = 0;
        while (j < len && up(name[j]) == k[j]) j++;
        if (j == len) return k_keys[i].id;
    }
    return -1;
}

const char *ui_key_name(uint8_t key)
{
    for (unsigned i = 0; i < N_KEYS; i++)
        if (k_keys[i].id == key) return k_keys[i].name;
    return "?";
}

bool ui_key_has_long(uint8_t key)
{
    for (unsigned i = 0; i < N_KEYS; i++)
        if (k_keys[i].id == key) return (KEY_LONG_MASK & (1u << k_keys[i].bit)) != 0u;
    return false;
}

/* ═══ the `ui seq` token parser (pure) ═══════════════════════════════ */

/* Strict decimal, no sign, at most 5 digits. */
static bool parse_dec(const char *s, size_t len, uint32_t *v)
{
    uint32_t acc = 0;
    if (len == 0u || len > 5u) return false;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        acc = acc * 10u + (uint32_t)(s[i] - '0');
    }
    *v = acc;
    return true;
}

static bool prefix_ieq(const char *s, size_t len, const char *pre)
{
    size_t n = strlen(pre);
    if (len < n) return false;
    for (size_t i = 0; i < n; i++)
        if (up(s[i]) != up(pre[i])) return false;
    return true;
}

ui_parse_t ui_seq_next(const char **sp, ui_tok_t *t, char *tok, size_t tn)
{
    const char *s = *sp;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    const char *b = s;
    while (*s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') s++;
    *sp = s;
    const size_t len = (size_t)(s - b);
    if (tok && tn) {
        size_t c = len < tn - 1u ? len : tn - 1u;
        memcpy(tok, b, c);
        tok[c] = '\0';
    }
    if (len == 0u) return UI_PARSE_END;
    memset(t, 0, sizeof *t);

    if (prefix_ieq(b, len, "wait:")) {
        uint32_t ms;
        if (!parse_dec(b + 5, len - 5u, &ms) || ms > UI_SEQ_MAX_WAIT_MS) return UI_PARSE_BAD_FORM;
        t->kind = UI_TOK_WAIT;
        t->ms = (uint16_t)ms;
        return UI_PARSE_OK;
    }
    if (prefix_ieq(b, len, "long:")) {
        const int k = ui_key_find(b + 5, len - 5u);
        if (k < 0) return memchr(b + 5, '*', len - 5u) ? UI_PARSE_BAD_FORM : UI_PARSE_BAD_KEY;
        if (k == BTN_POWER) return UI_PARSE_POWER;
        if (!ui_key_has_long((uint8_t)k)) return UI_PARSE_NO_LONG;
        t->kind = UI_TOK_LONG;
        t->key = (uint8_t)k;
        t->count = 1u;
        return UI_PARSE_OK;
    }
    const char *star = memchr(b, '*', len);
    const size_t klen = star ? (size_t)(star - b) : len;
    const int k = ui_key_find(b, klen);
    if (k < 0) return UI_PARSE_BAD_KEY;
    if (k == BTN_POWER) return UI_PARSE_POWER;
    uint32_t n = 1u;
    if (star && (!parse_dec(star + 1, len - klen - 1u, &n) || n == 0u || n > UI_SEQ_MAX_REPEAT))
        return UI_PARSE_BAD_FORM;
    t->kind = UI_TOK_PRESS;
    t->key = (uint8_t)k;
    t->count = (uint16_t)n;
    return UI_PARSE_OK;
}

ui_parse_t ui_seq_check(const char *s, unsigned *ntok, unsigned *events, unsigned *bad,
                        char *tok, size_t tn)
{
    unsigned n = 0, ev = 0;
    ui_tok_t t;
    if (ntok) *ntok = 0;
    if (events) *events = 0;
    if (bad) *bad = 0;
    if (!s) return UI_PARSE_END;
    for (;;) {
        const ui_parse_t r = ui_seq_next(&s, &t, tok, tn);
        if (r == UI_PARSE_END) break;
        n++;
        if (r != UI_PARSE_OK) {
            if (bad) *bad = n;
            return r;
        }
        ev += t.count;
    }
    if (ntok) *ntok = n;
    if (events) *events = ev;
    return n ? UI_PARSE_OK : UI_PARSE_END;
}

const char *ui_parse_text(ui_parse_t r)
{
    switch (r) {
    case UI_PARSE_OK:       return "ok";
    case UI_PARSE_END:      return "no keys given";
    case UI_PARSE_BAD_KEY:  return "unknown key";
    case UI_PARSE_BAD_FORM: return "bad form (KEY*1..50, long:KEY, wait:0..5000)";
    case UI_PARSE_POWER:    return "POWER: shutdown / IAP";
    case UI_PARSE_NO_LONG:  return "no long press on this key";
    default:                return "?";
    }
}

/* ═══ one press (pure) ═══════════════════════════════════════════════ */

static bool press_reached(const ui_press_port_t *p, uint32_t target)
{
    return (int32_t)(p->done() - target) >= 0;
}

ui_press_t ui_press_one(const ui_press_port_t *p, uint32_t *target, uint8_t id)
{
    const uint32_t t0 = p->now_ms();
    while (!press_reached(p, *target)) {             /* an earlier press still owed */
        if (p->now_ms() - t0 >= UI_PRESS_TIMEOUT_MS) { *target = p->done(); return UI_PRESS_STALE; }
        p->sleep_ms(UI_PRESS_POLL_MS);
    }
    *target = p->done();
    if (!p->inject(id)) return UI_PRESS_FULL;
    (*target)++;
    while (!press_reached(p, *target)) {             /* the rest of the same budget */
        if (p->now_ms() - t0 >= UI_PRESS_TIMEOUT_MS) return UI_PRESS_TIMEOUT;
        p->sleep_ms(UI_PRESS_POLL_MS);
    }
    return UI_PRESS_DONE;
}

/* ═══ firmware: the presses, the waits, the lines ═══════════════════ */

#ifndef UI_SHELL_TEST
#include "FreeRTOS.h"
#include "task.h"
#include "usb_debug.h"
#include "model.h"
#include "model_exec.h"       /* input_events_done, model_exec_ready */
#include "osc_menu_model.h"
#include "gallery_ui.h"
#include "scope_state.h"
#include "input_handler.h"    /* scope_key_focus */
#include "fpga.h"             /* fpga_interleave_active (keys) */
#include "screen.h"           /* the router's snapshot (`ui screen`) */
#include "lcd.h"              /* lcd_foreign_calls (`ui screen`) */
#include "overlay_screens.h"  /* the POWER prompt and the logo (`ui screen` overlay=modal) */

#include <stdio.h>
#include <stdlib.h>

/* The handled count the last press made us wait for (ui_press_one). 4
 * bytes: the only static state of the route. */
static uint32_t g_target;

static uint32_t ms_now(void) { return (uint32_t)xTaskGetTickCount() * (1000u / configTICK_RATE_HZ); }
static uint32_t fw_done(void) { return input_events_done; }
static bool     fw_inject(uint8_t id) { return input_inject_button((button_id_t)id); }
static void     fw_sleep(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1);
}

static const ui_press_port_t k_press_port = {
    .now_ms = ms_now, .done = fw_done, .inject = fw_inject, .sleep_ms = fw_sleep,
};

static ui_press_t press_one(uint8_t id) { return ui_press_one(&k_press_port, &g_target, id); }

/* ── ui screen ─────────────────────────────────────────────────────── */

static void fmt_prop(char *out, size_t n, const prop_desc_t *p)
{
    prop_format_value(p, prop_get(p), out, n);
}

/* One line, fixed keys in a fixed order, "-" where a key does not apply:
 *   screen=scope instrument=scope overlay=osc_menu row=ch1 pane=pane item=2
 *   settings=- home=- focus=zoom ach=CH1 run=run view=yt meter=- popup=0
 *   stack=scope>osc_menu edit=- mview=-
 * From the screen router's snapshot (ui/screen.h): `screen` is its base,
 * `overlay` its top layer, `stack` every screen bottom-up; row/pane/item,
 * settings and home are the describe() of the osc page (when on top), the
 * settings base and the home base; edit (the last key, added with the
 * generator screen) is the generator base's: off, or the digit under the
 * cursor (0..4) while its frequency editor is open; mview (after it, added with
 * the meter screen) is the meter base's: its layout (big, graph, stats, fuse,
 * limits), in the fuse tester with the page (fuse/detail, fuse/table,
 * fuse/scan, fuse/types). The rest are properties. The stack is
 * the authority for what is shown: `set sys.mode` answers after the switch. */
static void screen_line(void)
{
    char inst[12], focus[12], view[16], meter[16];
    char osc[48] = "row=- pane=- item=-", set[24] = "settings=-", home[16] = "home=-";
    char gen[16] = "edit=-", mview[24] = "mview=-", gal[20] = "gal=-";
    char stack[64] = "";
    const screen_stack_t st = screen_snapshot();
    const screen_t *base = st.n ? screen_get(st.id[0]) : NULL;
    const screen_t *top  = st.n ? screen_get(st.id[st.n - 1]) : NULL;

    fmt_prop(inst, sizeof inst, P(SYS_INSTRUMENT));
    fmt_prop(view, sizeof view, P(VIEW_MODE));
    prop_format(P(UI_FOCUS), prop_get(P(UI_FOCUS)), focus, sizeof focus);
    if (base && base->id == SCR_METER) fmt_prop(meter, sizeof meter, P(METER_FUNC));
    else strcpy(meter, "-");

    const char *overlay = "none";
    if (!model_exec_ready())                       overlay = "logo";   /* boot: the logo holds the keys */
    else if (st.n > 1 && top && top->id == SCR_GALLERY)  overlay = "gallery";
    else if (st.n > 1 && top && top->id == SCR_OSC_MENU) overlay = "osc_menu";
    else if (power_prompt_up() || logo_screen_up()) overlay = "modal";
    if (top && top->id == SCR_OSC_MENU && top->describe) top->describe(osc, sizeof osc);
    if (top && top->id == SCR_GALLERY && top->describe) top->describe(gal, sizeof gal);
    if (base && base->id == SCR_SETTINGS && base->describe) base->describe(set, sizeof set);
    if (base && base->id == SCR_HOME && base->describe) base->describe(home, sizeof home);
    if (base && base->id == SCR_SIGGEN && base->describe) base->describe(gen, sizeof gen);
    if (base && base->id == SCR_METER && base->describe) base->describe(mview, sizeof mview);
    for (uint8_t i = 0; i < st.n; i++) {
        const screen_t *sc = screen_get(st.id[i]);
        const size_t o = strlen(stack);
        snprintf(stack + o, sizeof stack - o, "%s%s", i ? ">" : "", sc && sc->name ? sc->name : "?");
    }
    usb_debug_printf("screen=%s instrument=%s overlay=%s %s %s %s "
                     "focus=%s ach=CH%u run=%s view=%s meter=%s popup=%u stack=%s %s %s %s lcdforeign=%lu\r\n",
                     base && base->name ? base->name : "?", inst, overlay,
                     osc, set, home, focus, (unsigned)active_channel + 1u,
                     prop_get(P(VIEW_RUN)) ? "run" : "stop", view, meter,
                     scope_popup_active() ? 1u : 0u, stack[0] ? stack : "-", gen, mview, gal,
                     (unsigned long)lcd_foreign_calls);
}

void cmd_ui_screen(void) { screen_line(); }

/* ── ui press / long / seq ─────────────────────────────────────────── */

static void report_parse(ui_parse_t r, unsigned bad, const char *tok)
{
    const bool refused = (r == UI_PARSE_POWER || r == UI_PARSE_NO_LONG);
    usb_debug_printf("%s token %u '%s': %s; nothing pressed\r\n", refused ? "REFUSED" : "INVALID",
                     bad, tok, ui_parse_text(r));
}

/* Run a checked line. Prints the status line and the screen line. */
static void run_seq(const char *s, unsigned events)
{
    const uint32_t t0 = ms_now();
    unsigned k = 0, handled = 0;
    ui_tok_t t;
    char tok[16];
    while (ui_seq_next(&s, &t, tok, sizeof tok) == UI_PARSE_OK) {
        k++;
        if (t.kind == UI_TOK_WAIT) { if (t.ms) vTaskDelay(pdMS_TO_TICKS(t.ms)); continue; }
        const uint8_t id = (uint8_t)(t.key | (t.kind == UI_TOK_LONG ? BTN_LONG_FLAG : 0u));
        for (unsigned i = 0; i < t.count; i++) {
            const ui_press_t r = press_one(id);
            if (r == UI_PRESS_FULL) {
                usb_debug_printf("BUSY at token %u (%s): key queue full; %u of %u events handled\r\n",
                                 k, ui_key_name(t.key), handled, events);
                screen_line();
                return;
            }
            if (r == UI_PRESS_TIMEOUT || r == UI_PRESS_STALE) {
                usb_debug_printf("TIMEOUT at token %u (%s): input task busy (gallery/countdown/selfcal?); "
                                 "%u of %u events handled\r\n", k, ui_key_name(t.key), handled, events);
                screen_line();
                return;
            }
            handled++;
        }
    }
    usb_debug_printf("OK %u events %lu ms\r\n", handled, (unsigned long)(ms_now() - t0));
    screen_line();
}

static void seq_line(const char *s)
{
    unsigned ntok, events, bad;
    char tok[24];
    const ui_parse_t r = ui_seq_check(s, &ntok, &events, &bad, tok, sizeof tok);
    if (r == UI_PARSE_END) {
        usb_debug_printf("%s", "usage: ui seq KEY|KEY*n|long:KEY|wait:ms ...  (KEY = CH1 CH2 MOVE SELECT TRIGGER PRM "
                     "AUTO SAVE MENU UP DOWN LEFT RIGHT OK)\r\n");
        return;
    }
    if (r != UI_PARSE_OK) { report_parse(r, bad, tok); return; }
    run_seq(s, events);
}

void cmd_ui_seq(const char *args) { seq_line(args); }

/* `ui press KEY [n]` = `ui seq KEY*n`; `ui long KEY` = `ui seq long:KEY`.
 * Rewritten into a seq token so there is one parser. */
static void one_key(const char *args, bool is_long)
{
    char line[40];
    const char *s = args ? args : "";
    while (*s == ' ' || *s == '\t') s++;
    const char *k = s;
    while (*s && *s != ' ' && *s != '\t') s++;
    const size_t klen = (size_t)(s - k);
    while (*s == ' ' || *s == '\t') s++;
    const char *n = s;
    while (*s && *s != ' ' && *s != '\t') s++;
    const size_t nlen = (size_t)(s - n);
    while (*s == ' ' || *s == '\t') s++;
    if (klen == 0u || klen > 12u || nlen > 5u || *s || (is_long && nlen)) {
        usb_debug_printf("%s", is_long ? "usage: ui long KEY\r\n" : "usage: ui press KEY [1..50]\r\n");
        return;
    }
    if (is_long)   snprintf(line, sizeof line, "long:%.*s", (int)klen, k);
    else if (nlen) snprintf(line, sizeof line, "%.*s*%.*s", (int)klen, k, (int)nlen, n);
    else           snprintf(line, sizeof line, "%.*s", (int)klen, k);
    seq_line(line);
}

void cmd_ui_press(const char *args) { one_key(args, false); }
void cmd_ui_long(const char *args)  { one_key(args, true); }

#endif /* !UI_SHELL_TEST */
