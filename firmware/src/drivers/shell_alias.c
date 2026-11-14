/*
 * OpenScope 2C53T - the old state commands as aliases of the property model
 * (shell_alias.h, ARCH-1 M4)
 *
 * Every handler here has the same three steps: parse the old syntax (the old
 * handler's own code, usage lines included), ask the executor (apply()), and
 * print the old text from the value read back. No scope_state / ui.h global
 * is written and no owner API is called: reads only, through prop_get and the
 * environment. Runs on the shell task; host-tested as it is
 * (tests/test_shell_alias.c).
 */
#include "shell_alias.h"
#include "model.h"
#include "model_exec.h"
#include "scope_state.h"
#include "scope_cal.h"
#include "scope_timebase.h"
#include "scope_roll.h"
#include "scope_geom.h"
#include "scope_measure.h"
#include "meas_select.h"
#include "interleave.h"
#include "mask_pf.h"
#include "signal_gen.h"
#include "ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef const shell_alias_env_t env_t;

/* ═══ output and parsing (as usb_debug.c's helpers) ══════════════════ */

static void outf(env_t *e, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void outf(env_t *e, const char *fmt, ...)
{
    char buf[256];                      /* usb_debug_printf's size */
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    e->emit(buf);
}

/* usb_debug.c parse_hex32 / parse_int, to the letter. */
static int parse_hex32(const char *s, uint32_t *out)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    char *end;
    *out = strtoul(s, &end, 16);
    return (*end == '\0' || *end == ' ' || *end == '\r' || *end == '\n') ? 0 : -1;
}

static int parse_int(const char *s, uint32_t *out)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        return parse_hex32(s, out);
    char *end;
    *out = strtoul(s, &end, 10);
    return (*end == '\0' || *end == ' ' || *end == '\r' || *end == '\n') ? 0 : -1;
}

static const char *onoff(bool b) { return b ? "on" : "off"; }

static int32_t get(uint16_t id) { return prop_get(prop_by_id(id)); }

static prop_pair_t pr(uint16_t id, int32_t v) { prop_pair_t p = { id, v }; return p; }

/* ═══ the request ════════════════════════════════════════════════════ */

/* Ask the executor for the pairs, as `set` does (same origin, same
 * timeouts). true = applied (OK or CLAMPED; *res says which were stored
 * rather than applied live). false = not applied, and the reply has been
 * printed: the old handler's own line where it had one for that case
 * (refused / not_set, may be NULL), else one of the lines that are new with
 * the executor, in the shape `set` gives them. */
static bool apply(env_t *e, const prop_pair_t *pairs, uint8_t n, model_result_t *res,
                  const char *refused, const char *not_set)
{
    const prop_status_t st = prop_request(pairs, n, ORIGIN_SHELL, prop_request_timeout(pairs, n), res);
    if (st == PS_OK || st == PS_CLAMPED) return true;
    if (st == PS_TIMEOUT) {
        e->emit(res->in_progress
                ? "TIMEOUT: input task busy (gallery/countdown/selfcal?); still applying: check with get\r\n"
                : "TIMEOUT: input task busy (gallery/countdown/selfcal?); nothing applied\r\n");
        return false;
    }
    if (st == PS_REFUSED && refused) { e->emit(refused); return false; }
    if (st == PS_NOT_SET && not_set) { e->emit(not_set); return false; }
    const prop_desc_t *fp = (res->failed < n) ? prop_by_id(pairs[res->failed].id) : NULL;
    if (fp) outf(e, "%s %s (%s)\r\n", prop_status_name(st), fp->name,
                 res->note.text[0] ? res->note.text : "no reason given");
    else    outf(e, "%s (%s)\r\n", prop_status_name(st),
                 res->note.text[0] ? res->note.text : "no reason given");
    return false;
}

/* After the old reply: which pairs were stored rather than applied live
 * (model.h DEFERRED). The old handlers drove the hardware whatever the
 * screen was; the model keeps the value and the instrument's entry applies
 * it. Nothing is printed when everything was applied live. */
static void stored_note(env_t *e, const prop_pair_t *pairs, uint8_t n, const model_result_t *res)
{
    for (uint8_t i = 0; i < n; i++) {
        if (!(res->deferred & (1u << i))) continue;
        const prop_desc_t *p = prop_by_id(pairs[i].id);
        const char *inst = "its";
        for (uint8_t k = 0; k < model_instrument_count(); k++) {
            const instrument_desc_t *in = model_instrument_at(k);
            if (in && prop_in_instrument(p, in)) { inst = in->name; break; }
        }
        outf(e, "  (%s stored: %s FE off; applied when it is next entered)\r\n", p->name, inst);
    }
}

/* A posted change (the acquisition task applies mask requests): wait, as the
 * old handlers did (40 x 10 ms), until the value reads back. */
static bool wait_value(env_t *e, uint16_t id, int32_t want)
{
    for (int i = 0; i < 40; i++) {
        if (get(id) == want) return true;
        if (e->sleep_ms) e->sleep_ms(10);
    }
    return get(id) == want;
}

/* ═══ channels ═══════════════════════════════════════════════════════ */

/* `fpga scope vdiv <1|2> <0-9>` -> chN.vdiv */
void shell_alias_scope_vdiv(const char *args, env_t *e)
{
    uint32_t chn = 0, n = 0;
    const char *space = args ? strchr(args, ' ') : NULL;
    if (!args || parse_int(args, &chn) != 0 || (chn != 1u && chn != 2u) ||
        !space) {
        e->emit("Usage: fpga scope vdiv <1|2> <0-9>\r\n");
        return;
    }
    while (*space == ' ') space++;
    if (parse_int(space, &n) != 0 || n > 9u) {
        e->emit("Usage: fpga scope vdiv <1|2> <0-9>\r\n");
        return;
    }
    const prop_pair_t p = pr(chn == 1u ? PROP_ID_CH1_VDIV : PROP_ID_CH2_VDIV, (int32_t)n);
    model_result_t res;
    if (!apply(e, &p, 1, &res, NULL, "vdiv: relay apply failed\r\n")) return;
    char vd[12];
    scope_cal_range_label((uint8_t)chn, (uint8_t)n, vd, sizeof(vd));
    outf(e, "vdiv CH%lu = range %lu (%s/div)\r\n",
         (unsigned long)chn, (unsigned long)n, vd);
    stored_note(e, &p, 1, &res);
}

/* `fpga scope ch <1|2> [on|off]` -> chN.enabled */
void shell_alias_scope_ch(const char *args, env_t *e)
{
    while (*args == ' ') args++;
    uint32_t ch = 0;
    if (parse_int(args, &ch) != 0 || (ch != 1u && ch != 2u)) {
        e->emit("usage: fpga scope ch <1|2> [on|off]\r\n");
        return;
    }
    const char *v = strchr(args, ' ');
    if (v) {
        while (*v == ' ') v++;
        int32_t on;
        if      (strncmp(v, "on", 2) == 0)  on = 1;
        else if (strncmp(v, "off", 3) == 0) on = 0;
        else { e->emit("usage: fpga scope ch <1|2> [on|off]\r\n"); return; }
        const prop_pair_t p = pr(ch == 1u ? PROP_ID_CH1_ENABLED : PROP_ID_CH2_ENABLED, on);
        model_result_t res;
        if (!apply(e, &p, 1, &res, "refused: at least one channel must stay on\r\n",
                   "refused: at least one channel must stay on\r\n"))
            return;
    }
    const model_hw_ops_t *hw = model_hw();
    outf(e, "CH1 %s  CH2 %s  mask %u  trigger CH%u  interleave %s\r\n",
         get(PROP_ID_CH1_ENABLED) ? "on" : "off", get(PROP_ID_CH2_ENABLED) ? "on" : "off",
         (unsigned)e->channel_mask(),
         get(PROP_ID_TRIG_SOURCE) == TRIG_SRC_CH2 ? 2u : 1u,
         (hw->interleave_active && hw->interleave_active()) ? "ACTIVE (250 MS/s)" : "inactive");
}

/* `fpga scope probe <1|2> [1x|10x]` -> chN.probe */
void shell_alias_scope_probe(const char *args, env_t *e)
{
    while (*args == ' ') args++;
    uint32_t ch = 0;
    if (parse_int(args, &ch) != 0 || (ch != 1u && ch != 2u)) {
        e->emit("usage: fpga scope probe <1|2> [1x|10x]\r\n");
        return;
    }
    const uint16_t id = (ch == 1u) ? PROP_ID_CH1_PROBE : PROP_ID_CH2_PROBE;
    const char *v = strchr(args, ' ');
    if (v) {
        while (*v == ' ') v++;
        int32_t want;
        if      (strncmp(v, "10x", 3) == 0) want = PROBE_10X;
        else if (strncmp(v, "1x", 2) == 0)  want = PROBE_1X;
        else { e->emit("usage: fpga scope probe <1|2> [1x|10x]\r\n"); return; }
        const prop_pair_t p = pr(id, want);
        model_result_t res;
        if (!apply(e, &p, 1, &res, NULL, NULL)) return;
    }
    outf(e, "CH%u probe %s\r\n", (unsigned)ch, probe_labels[get(id)]);
}

/* `cal pos <1|2> [-100..100]` -> chN.pos (clamped silently, as
 * scope_vpos_set clamped it) */
void shell_alias_cal_pos(const char *args, env_t *e)
{
    while (*args == ' ') args++;
    char *end;
    const long ch = strtol(args, &end, 10);
    if (end == args || (ch != 1 && ch != 2)) {
        e->emit("Usage: cal pos <1|2> [-100..100]\r\n");
        return;
    }
    args = end;
    while (*args == ' ') args++;
    prop_pair_t p = pr(ch == 1 ? PROP_ID_CH1_POS : PROP_ID_CH2_POS, 0);
    model_result_t res;
    bool set = false;
    if (*args) {
        long pos = strtol(args, &end, 10);
        if (end == args) { e->emit("Usage: cal pos <1|2> [-100..100]\r\n"); return; }
        if (pos < -100000L) pos = -100000L;          /* into the pair's 32 bits; the */
        if (pos >  100000L) pos =  100000L;          /* property clamps to its range */
        p.value = (int32_t)pos;
        if (!apply(e, &p, 1, &res, NULL, NULL)) return;
        set = true;
    }
    e->status(SA_ST_VPOS, (int)ch);
    if (set) stored_note(e, &p, 1, &res);
}

/* ═══ timebase and roll ══════════════════════════════════════════════ */

/* `fpga scope timebase [hex]` -> tb.code, then tb.roll = off: a Y-T code
 * leaves roll (C11). In that order one register write does both (in roll
 * tb.code is only stored, and leaving roll writes the code now stored). */
void shell_alias_scope_timebase(const char *args, env_t *e)
{
    while (*args == ' ') args++;
    prop_pair_t p[2];
    model_result_t res;
    if (*args) {
        uint32_t v;
        if (parse_hex32(args, &v) != 0) {
            outf(e, "usage: fpga scope timebase <hex code 00-%02X>\r\n",
                 SCOPE_TIMEBASE_CODE_COUNT - 1);
            return;
        }
        if (v >= SCOPE_TIMEBASE_CODE_COUNT) {
            outf(e, "usage: fpga scope timebase <hex code 00-%02X>\r\n",
                 SCOPE_TIMEBASE_CODE_COUNT - 1);
            return;
        }
        p[0] = pr(PROP_ID_TB_CODE, (int32_t)v);
        p[1] = pr(PROP_ID_TB_ROLL, 0);
        if (!apply(e, p, 2, &res, NULL, "acq task would not park — reg 0x01 NOT written\r\n"))
            return;
    }
    e->status(SA_ST_TIMEBASE, *args != '\0');
    if (*args) stored_note(e, p, 2, &res);
}

/* `fpga roll [off|<step 0-8>|code <hex>|code stock]` -> tb.roll, tb.rollstep.
 * A step enters roll there: the step first (stored while not rolling, applied
 * when rolling), then roll on, so one write does it. `code` sets fpga.c's
 * entry byte override, which is no property, and re-applies when rolling by
 * asking for the step in force again (the setter re-enters roll with it). */
void shell_alias_roll(const char *args, env_t *e)
{
    while (*args == ' ') args++;
    prop_pair_t p[2];
    uint8_t n = 0;

    if (strncmp(args, "off", 3) == 0) {
        p[n++] = pr(PROP_ID_TB_ROLL, 0);
    } else if (strncmp(args, "code", 4) == 0) {
        const char *q = args + 4;
        while (*q == ' ') q++;
        uint32_t v;
        if (strncmp(q, "stock", 5) == 0) {
            e->roll_code_override(-1);
        } else if (*q && parse_hex32(q, &v) == 0 && v <= 0xFFu) {
            e->roll_code_override((int)v);
        } else {
            e->emit("usage: fpga roll code <hex 00-FF>|stock\r\n");
            return;
        }
        if (get(PROP_ID_TB_ROLL)) p[n++] = pr(PROP_ID_TB_ROLLSTEP, get(PROP_ID_TB_ROLLSTEP));
    } else if (*args) {
        uint32_t v;
        if (parse_int(args, &v) != 0 || v >= SCOPE_ROLL_STEPS) {
            outf(e, "usage: fpga roll [off|0-%u|code <hex>|code stock]\r\n",
                 (unsigned)(SCOPE_ROLL_STEPS - 1u));
            return;
        }
        p[n++] = pr(PROP_ID_TB_ROLLSTEP, (int32_t)v);
        p[n++] = pr(PROP_ID_TB_ROLL, 1);
    }
    model_result_t res;
    if (n && !apply(e, p, n, &res, NULL, "acq task would not park -- reg 0x01 NOT written\r\n"))
        return;
    e->status(SA_ST_ROLL, 0);
    if (n) stored_note(e, p, n, &res);
}

/* ═══ trigger ════════════════════════════════════════════════════════ */

/* `fpga scope hpos [0..300]` -> trig.x */
void shell_alias_scope_hpos(const char *args, env_t *e)
{
    while (*args == ' ') args++;
    if (*args) {
        uint32_t v = 0;
        if (parse_int(args, &v) != 0 || v > (uint32_t)SCOPE_GEOM_TRIG_COL_MAX) { e->emit("usage: fpga scope hpos [0..300]\r\n"); return; }
        const prop_pair_t p = pr(PROP_ID_TRIG_X, (int32_t)v);
        model_result_t res;
        if (!apply(e, &p, 1, &res, NULL, NULL)) return;
    }
    const uint8_t a = e->trig_anchor();
    outf(e, "trigger position: asked column %d; last window landed at %d (anchor %s)\r\n",
         (int)get(PROP_ID_TRIG_X), e->trig_x_actual(),
         a == 2 ? "hardware" : a == 1 ? "soft" : "none");
}

/* `fpga scope edge [rising|falling]` -> trig.edge */
void shell_alias_scope_edge(const char *args, env_t *e)
{
    while (*args == ' ') args++;
    if (*args) {
        int32_t want;
        if      (strncmp(args, "rising",  6) == 0) want = TRIG_RISING;
        else if (strncmp(args, "falling", 7) == 0) want = TRIG_FALLING;
        else { e->emit("usage: fpga scope edge [rising|falling]\r\n"); return; }
        const prop_pair_t p = pr(PROP_ID_TRIG_EDGE, want);
        model_result_t res;
        if (!apply(e, &p, 1, &res, NULL, NULL)) return;
    }
    outf(e, "trigger edge %s (edge filter %s)\r\n", trigger_edge_labels[get(PROP_ID_TRIG_EDGE)],
         e->edge_filter() ? "ON" : "OFF");
}

/* `fpga scope trigmode [auto|normal|single]` -> trig.mode (the setter arms
 * the one-shot on Single, as the panel does) */
void shell_alias_scope_trigmode(const char *args, env_t *e)
{
    while (*args == ' ') args++;
    if (*args) {
        int32_t want;
        if      (strncmp(args, "auto",   4) == 0) want = TRIG_AUTO;
        else if (strncmp(args, "normal", 6) == 0) want = TRIG_NORMAL;
        else if (strncmp(args, "single", 6) == 0) want = TRIG_SINGLE;
        else { e->emit("usage: fpga scope trigmode [auto|normal|single]\r\n"); return; }
        const prop_pair_t p = pr(PROP_ID_TRIG_MODE, want);
        model_result_t res;
        if (!apply(e, &p, 1, &res, NULL, NULL)) return;
    }
    outf(e, "trigger mode %s (acq wait policy: AUTO falls back after the edge-wait; NORMAL/SINGLE hold)\r\n",
         trigger_mode_labels[get(PROP_ID_TRIG_MODE)]);
    e->status(SA_ST_TRIGGER, 0);
}

/* `fpga scope source [ch1|ch2]` -> trig.source */
void shell_alias_scope_source(const char *args, env_t *e)
{
    while (*args == ' ') args++;
    if (*args) {
        int32_t want;
        if      (strncmp(args, "ch1", 3) == 0) want = TRIG_SRC_CH1;
        else if (strncmp(args, "ch2", 3) == 0) want = TRIG_SRC_CH2;
        else { e->emit("usage: fpga scope source [ch1|ch2]\r\n"); return; }
        char refused[64];
        snprintf(refused, sizeof refused, "trigger source %s refused: that channel is off\r\n",
                 trigger_source_labels[want]);
        const prop_pair_t p = pr(PROP_ID_TRIG_SOURCE, want);
        model_result_t res;
        if (!apply(e, &p, 1, &res, refused, "trigger source NOT SET (acq task would not park)\r\n"))
            return;
        outf(e, "trigger source %s -> reg 0x06 = 0x%02X\r\n",
             trigger_source_labels[get(PROP_ID_TRIG_SOURCE)], (unsigned)e->trig_src_code());
        stored_note(e, &p, 1, &res);
        return;
    }
    outf(e, "trigger source %s -> reg 0x06 = 0x%02X\r\n",
         trigger_source_labels[get(PROP_ID_TRIG_SOURCE)], (unsigned)e->trig_src_code());
}

/* `fpga scope single` -> trig.mode = single (arms it: what OK does after a
 * capture) */
void shell_alias_scope_single(const char *args, env_t *e)
{
    (void)args;
    const prop_pair_t p = pr(PROP_ID_TRIG_MODE, TRIG_SINGLE);
    model_result_t res;
    if (!apply(e, &p, 1, &res, NULL, NULL)) return;
    e->emit("single armed (Await Trigger...)\r\n");
}

/* `fpga scope level [-100..100]` -> trig.level */
void shell_alias_scope_level(const char *args, env_t *e)
{
    while (*args == ' ') args++;
    prop_pair_t p = pr(PROP_ID_TRIG_LEVEL, 0);
    model_result_t res;
    if (*args) {
        int neg = 0; uint32_t lv = 0; const char *q = args;
        if (*q == '-') { neg = 1; q++; }
        if (parse_int(q, &lv) != 0 || lv > 100u) {
            e->emit("usage: fpga scope level [-100..100]\r\n"); return;
        }
        p.value = neg ? -(int32_t)lv : (int32_t)lv;
        if (!apply(e, &p, 1, &res, NULL, "acq task would not park — reg 0x08 NOT written\r\n"))
            return;
    }
    outf(e, "trigger level %d -> reg 0x08 code 0x%02X in force (boot reconcile wrote 0x%02X)\r\n",
         (int)get(PROP_ID_TRIG_LEVEL), e->trig_code(), e->trig_reconcile_code());
    if (*args) stored_note(e, &p, 1, &res);
}

/* `fpga scope softtrig [on|off|toggle]` -> trig.soft */
void shell_alias_scope_softtrig(const char *args, env_t *e)
{
    while (*args == ' ') args++;
    int32_t want = -1;
    if (strncmp(args, "on", 2) == 0)          want = 1;
    else if (strncmp(args, "off", 3) == 0)    want = 0;
    else if (strncmp(args, "toggle", 6) == 0) want = !get(PROP_ID_TRIG_SOFT);
    else if (*args) {
        e->emit("usage: fpga scope softtrig [on|off|toggle]\r\n");
        return;
    }
    if (want >= 0) {
        const prop_pair_t p = pr(PROP_ID_TRIG_SOFT, want);
        model_result_t res;
        if (!apply(e, &p, 1, &res, NULL, NULL)) return;
    }
    outf(e, "softtrig: %s   (level=%d px, %s, src=CH%d)\r\n",
         get(PROP_ID_TRIG_SOFT) ? "ON (trace locked to trigger)"
                                : "off (free-run / dancing)",
         (int)get(PROP_ID_TRIG_LEVEL),
         get(PROP_ID_TRIG_EDGE) == TRIG_RISING ? "rising" : "falling",
         get(PROP_ID_TRIG_SOURCE) == TRIG_SRC_CH2 ? 2 : 1);
}

/* ═══ view ═══════════════════════════════════════════════════════════ */

/* `fpga scope graticule [auto|true|toggle]` -> view.truescale */
void shell_alias_scope_graticule(const char *args, env_t *e)
{
    while (*args == ' ') args++;
    int32_t want = -1;
    if (strncmp(args, "true", 4) == 0)        want = 1;
    else if (strncmp(args, "auto", 4) == 0)   want = 0;
    else if (strncmp(args, "toggle", 6) == 0) want = !get(PROP_ID_VIEW_TRUESCALE);
    else if (*args) {
        e->emit("usage: fpga scope graticule [auto|true|toggle]\r\n");
        return;
    }
    if (want >= 0) {
        const prop_pair_t p = pr(PROP_ID_VIEW_TRUESCALE, want);
        model_result_t res;
        if (!apply(e, &p, 1, &res, NULL, NULL)) return;
    }
    const bool ts = get(PROP_ID_VIEW_TRUESCALE) != 0;
    outf(e, "graticule: %s\r\n",
         ts ? "TRUE SCALE (grid == volts/div)"
            : "autofit (grid == position only)");
    if (ts) {
        const uint8_t r1 = (uint8_t)get(PROP_ID_CH1_VDIV), r2 = (uint8_t)get(PROP_ID_CH2_VDIV);
        char l1[12], l2[12];
        scope_cal_range_label(1u, r1, l1, sizeof(l1));
        scope_cal_range_label(2u, r2, l2, sizeof(l2));
        outf(e, "  CH1 rng %u -> %s/div %s\r\n", (unsigned)r1, l1,
             scope_cal_true_scale_ok(1u, r1) ? "" : "(no cal -> autofit)");
        outf(e, "  CH2 rng %u -> %s/div %s\r\n", (unsigned)r2, l2,
             scope_cal_true_scale_ok(2u, r2) ? "" : "(no cal -> autofit)");
        e->emit("  center the baseline first: `fpga scope center`\r\n");
    }
}

/* `fpga scope prm [ch1|ch2] [mask|all|default|none|toggle N|hide|show]`
 * -> chN.meas, view.meashide */
void shell_alias_scope_prm(const char *args, env_t *e)
{
    while (*args == ' ') args++;
    uint8_t ch = 0u;
    if ((args[0] == 'c' || args[0] == 'C') && (args[1] == 'h' || args[1] == 'H') &&
        (args[2] == '1' || args[2] == '2') && (args[3] == ' ' || args[3] == '\0')) {
        ch = (uint8_t)(args[2] - '1');
        args += 3;
        while (*args == ' ') args++;
    }

    if (*args) {
        const uint16_t meas = ch ? PROP_ID_CH2_MEAS : PROP_ID_CH1_MEAS;
        prop_pair_t p;
        uint32_t v;
        if (strncmp(args, "all", 3) == 0)          p = pr(meas, (int32_t)SCOPE_PRM_ALL_MASK);
        else if (strncmp(args, "default", 7) == 0) p = pr(meas, ch ? MEAS_SELECT_CH2_DEFAULT
                                                                    : MEAS_SELECT_CH1_DEFAULT);
        else if (strncmp(args, "none", 4) == 0)    p = pr(meas, 0);
        else if (strncmp(args, "hide", 4) == 0)    p = pr(PROP_ID_VIEW_MEASHIDE, 1);
        else if (strncmp(args, "show", 4) == 0)    p = pr(PROP_ID_VIEW_MEASHIDE, 0);
        else if (strncmp(args, "toggle", 6) == 0) {
            const char *a = args + 6;
            while (*a == ' ') a++;
            if (parse_int(a, &v) != 0 || v >= SCOPE_PRM_COUNT) {
                e->emit("usage: fpga scope prm [ch1|ch2] toggle <0-11>\r\n");
                return;
            }
            p = pr(meas, (int32_t)((uint32_t)get(meas) ^ (1u << v)));
        } else if (parse_int(args, &v) == 0) {
            p = pr(meas, (int32_t)(v & SCOPE_PRM_ALL_MASK));
        } else {
            e->emit("usage: fpga scope prm [ch1|ch2] [mask|all|default|none|toggle <0-11>|hide|show]\r\n");
            return;
        }
        model_result_t res;
        if (!apply(e, &p, 1, &res, NULL, NULL)) return;
    }
    e->status(SA_ST_PRM, 0);
}

/* ═══ interleave ═════════════════════════════════════════════════════ */

static void interleave_skew(const char *args, env_t *e)
{
    static const char usage[] =
        "usage: fpga interleave skew [measure|set <ppm>|off]  ppm = B's deviation from half a "
        "converter period, ppm of a period (B at 0.451 -> -49000), |ppm| <= 150000\r\n";
    while (*args == ' ') args++;
    model_result_t res;
    if (strncmp(args, "set", 3) == 0) {
        const char *q = args + 3;
        while (*q == ' ') q++;
        const bool neg = (*q == '-');
        if (neg || *q == '+') q++;
        uint32_t v = 0;
        if (parse_int(q, &v) != 0 || v > (uint32_t)IL_SKEW_PPM_MAX) {
            e->emit(usage);
            return;
        }
        const prop_pair_t p = pr(PROP_ID_IL_SKEW, neg ? -(int32_t)v : (int32_t)v);
        if (!apply(e, &p, 1, &res, usage, usage)) return;
    } else if (strncmp(args, "off", 3) == 0) {
        const prop_pair_t p = pr(PROP_ID_IL_SKEW, 0);
        if (!apply(e, &p, 1, &res, NULL, NULL)) return;
    } else if (*args) {
        e->emit(usage);
        return;
    }
    e->status(SA_ST_SKEW, 0);
}

/* `fpga interleave [on|off]` -> il.enabled; `corr <gain x10000> <offset
 * x100>` -> il.knots = 0 (the correction replaces the whole level map, as
 * the old command did), il.gain, il.offset; `skew set <ppm>` / `skew off`
 * -> il.skew. */
void shell_alias_interleave(const char *args, env_t *e)
{
    while (*args == ' ') args++;
    model_result_t res;
    if (strncmp(args, "skew", 4) == 0) {
        interleave_skew(args + 4, e);
        return;
    } else if (strncmp(args, "corr", 4) == 0) {
        const char *p = args + 4;
        while (*p == ' ') p++;
        uint32_t g = 0, o = 0;
        int neg = 0;
        const char *q = strchr(p, ' ');
        if (parse_int(p, &g) != 0 || g < 5000u || g > 20000u || !q) {
            e->emit("usage: fpga interleave corr <gain x10000, 5000-20000> <offset x100, signed>\r\n");
            return;
        }
        while (*q == ' ') q++;
        if (*q == '-') { neg = 1; q++; }
        if (parse_int(q, &o) != 0 || o > 10000u) {
            e->emit("usage: fpga interleave corr <gain x10000, 5000-20000> <offset x100, signed>\r\n");
            return;
        }
        const prop_pair_t pp[3] = {
            pr(PROP_ID_IL_KNOTS, 0),
            pr(PROP_ID_IL_GAIN, (int32_t)(g * 100u)),            /* x1e4 -> x1e6 */
            pr(PROP_ID_IL_OFFSET, neg ? -(int32_t)o : (int32_t)o),
        };
        if (!apply(e, pp, 3, &res, NULL, NULL)) return;
    } else if (*args) {
        int32_t on;
        if      (strncmp(args, "on", 2) == 0)  on = 1;
        else if (strncmp(args, "off", 3) == 0) on = 0;
        else { e->emit("usage: fpga interleave [on|off|corr <g> <o>|cal|skew ...]\r\n"); return; }
        const prop_pair_t p = pr(PROP_ID_IL_ENABLED, on);
        if (!apply(e, &p, 1, &res, NULL, NULL)) return;
    }
    e->status(SA_ST_INTERLEAVE, 0);
}

/* ═══ generator ══════════════════════════════════════════════════════ */

/* `siggen [wave N] [freq HZ] [amp DV] [duty PCT] [on|off] [boot on|off]
 * [dump]` -> gen.*. One request per token, left to right, as the old
 * handler applied them: a bad token stops the line and what came before it
 * stays applied. */
void shell_alias_siggen(const char *args, env_t *e)
{
    char buf[96];
    size_t n = strlen(args);
    if (n >= sizeof buf) n = sizeof buf - 1;
    memcpy(buf, args, n);
    buf[n] = '\0';

    char *save = NULL;
    for (char *tok = strtok_r(buf, " \t", &save); tok != NULL;
         tok = strtok_r(NULL, " \t", &save)) {
        prop_pair_t p;
        if (strcmp(tok, "on") == 0)       p = pr(PROP_ID_GEN_OUT, 1);
        else if (strcmp(tok, "off") == 0) p = pr(PROP_ID_GEN_OUT, 0);
        else if (strcmp(tok, "boot") == 0) {
            char *bv = strtok_r(NULL, " \t", &save);
            if (bv != NULL && strcmp(bv, "on") == 0)       p = pr(PROP_ID_GEN_BOOTOUT, 1);
            else if (bv != NULL && strcmp(bv, "off") == 0) p = pr(PROP_ID_GEN_BOOTOUT, 0);
            else { e->emit("siggen: boot on|off\r\n"); return; }
        } else if (strcmp(tok, "dump") == 0) {
            const uint16_t *cy = siggen_get_cycle();
            for (unsigned i = 0; i < SIGGEN_CYCLE_SAMPLES; i += 10)
                outf(e, "  %3u: %4u %4u %4u %4u %4u %4u %4u %4u %4u %4u\r\n", i,
                     cy[i], cy[i + 1], cy[i + 2], cy[i + 3], cy[i + 4],
                     cy[i + 5], cy[i + 6], cy[i + 7], cy[i + 8], cy[i + 9]);
            continue;
        } else {
            char *val = strtok_r(NULL, " \t", &save);
            uint32_t v = 0;
            if (val == NULL || parse_int(val, &v) != 0) {
                outf(e, "siggen: '%s' needs a number\r\n", tok);
                return;
            }
            if (strcmp(tok, "wave") == 0) {
                if (v >= SIGGEN_WAVEFORM_COUNT) { e->emit("siggen: wave 0..12\r\n"); return; }
                p = pr(PROP_ID_GEN_WAVE, (int32_t)v);
            } else if (strcmp(tok, "freq") == 0) {
                if (v > SIGGEN_FREQ_MAX_HZ) { e->emit("siggen: freq 0..50000 Hz\r\n"); return; }
                p = pr(PROP_ID_GEN_FREQ, (int32_t)v);
            } else if (strcmp(tok, "amp") == 0) {
                if (v > SIGGEN_AMP_MAX_DV) { e->emit("siggen: amp 0..30 (0.1 V)\r\n"); return; }
                p = pr(PROP_ID_GEN_AMP, (int32_t)v);
            } else if (strcmp(tok, "duty") == 0) {
                if (v > SIGGEN_DUTY_MAX) { e->emit("siggen: duty 0..100\r\n"); return; }
                p = pr(PROP_ID_GEN_DUTY, (int32_t)v);
            } else {
                outf(e, "siggen: unknown '%s'\r\n", tok);
                return;
            }
        }
        model_result_t res;
        if (!apply(e, &p, 1, &res, NULL, NULL)) return;
    }
    e->status(SA_ST_SIGGEN, 0);
}

/* ═══ mask ═══════════════════════════════════════════════════════════ */

static const char k_mask_not_applied[] =
    "NOT APPLIED: the acquisition loop did not pick the request up in 400 ms\r\n"
    "(not a live-scope build, scope not initialised, or acq parked).\r\n";

/* `mask tol <v> [h]` -> mask.tolv, mask.tolh (h is the default when not
 * given, as before). The acquisition task applies it: the reply waits for
 * the values to read back, and says so when they do not. */
void shell_alias_mask_tol(const char *args, env_t *e)
{
    const char *p = args;
    while (*p == ' ') p++;
    uint32_t v = 0, h = MASK_PF_TOL_H_DEFAULT;
    if (parse_int(p, &v) != 0 || v > 255u) {
        outf(e, "usage: mask tol <v counts 0..255> [h samples 0..%u]\r\n", MASK_PF_HMAX);
        return;
    }
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    if (*p && (parse_int(p, &h) != 0 || h > MASK_PF_HMAX)) {
        outf(e, "usage: mask tol <v counts 0..255> [h samples 0..%u]\r\n", MASK_PF_HMAX);
        return;
    }
    const prop_desc_t *tv = prop_by_id(PROP_ID_MASK_TOLV);
    const prop_pair_t pp[2] = { pr(PROP_ID_MASK_TOLV, (int32_t)v), pr(PROP_ID_MASK_TOLH, (int32_t)h) };
    model_result_t res;
    if (!apply(e, pp, 2, &res, NULL, NULL)) return;
    const int32_t want_v = ((int32_t)v > tv->max) ? tv->max : (int32_t)v;
    if (want_v != (int32_t)v)
        outf(e, "mask tol: v %lu is past the model's range, set to %ld (mask.tolv 0..%ld)\r\n",
             (unsigned long)v, (long)want_v, (long)tv->max);
    if (!wait_value(e, PROP_ID_MASK_TOLV, want_v) || !wait_value(e, PROP_ID_MASK_TOLH, (int32_t)h)) {
        e->emit(k_mask_not_applied);
        return;
    }
    e->status(SA_ST_MASK, 0);
}

/* `mask stop on|off` -> mask.stopfail */
void shell_alias_mask_stop(const char *args, env_t *e)
{
    const char *p = args;
    while (*p == ' ') p++;
    int32_t on;
    if (strncmp(p, "on", 2) == 0)       on = 1;
    else if (strncmp(p, "off", 3) == 0) on = 0;
    else { e->emit("usage: mask stop on|off\r\n"); return; }
    const prop_pair_t pp = pr(PROP_ID_MASK_STOPFAIL, on);
    model_result_t res;
    if (!apply(e, &pp, 1, &res, NULL, NULL)) return;
    if (!wait_value(e, PROP_ID_MASK_STOPFAIL, on)) {
        e->emit(k_mask_not_applied);
        return;
    }
    e->status(SA_ST_MASK, 0);
}

/* ═══ meter ══════════════════════════════════════════════════════════ */

/* `meter func <n>` -> meter.func. The old refusal stays (with the meter link
 * off the old handler did nothing; a `set meter.func` there stores the
 * function for the next meter entry, which this command never did). */
void shell_alias_meter_func(const char *args, env_t *e)
{
    uint32_t sub;
    while (*args == ' ') args++;
    if (!*args || parse_int(args, &sub) != 0 || sub >= METER_SUBMODE_COUNT) {
        outf(e, "usage: meter func <0-%u>  (0 DCV 1 ACV 2 DCmA 3 DCA 4 ACmA 5 ACA 6 Ohm 7 Cont 8 Diode 9 Cap 10 Temp 11 Auto)\r\n",
             (unsigned)(METER_SUBMODE_COUNT - 1));
        return;
    }
    const model_hw_ops_t *hw = model_hw();
    if (get(PROP_ID_SYS_MODE) != MODE_MULTIMETER && !(hw->meter_link_on && hw->meter_link_on())) {
        e->emit("meter func: the meter link is off (`mode meter`, or see `meter state`)\r\n");
        return;
    }
    e->status(SA_ST_METER_FUNC_BEGIN, (int)sub);
    const prop_pair_t p = pr(PROP_ID_METER_FUNC, (int32_t)sub);
    model_result_t res;
    if (!apply(e, &p, 1, &res, NULL, NULL)) return;
    e->status(SA_ST_METER_FUNC, (int)sub);
}

/* `meter degf [on|off]` -> meter.degf */
void shell_alias_meter_degf(const char *args, env_t *e)
{
    const char *a = args;
    while (*a == ' ') a++;
    int32_t on = -1;
    if (strncmp(a, "on", 2) == 0)       on = 1;
    else if (strncmp(a, "off", 3) == 0) on = 0;
    if (on >= 0) {
        const prop_pair_t p = pr(PROP_ID_METER_DEGF, on);
        model_result_t res;
        if (!apply(e, &p, 1, &res, NULL, NULL)) return;
    } else if (*args) { e->emit("usage: meter degf [on|off]\r\n"); return; }
    outf(e, "meter degF: %s\r\n", onoff(get(PROP_ID_METER_DEGF) != 0));
}

/* ═══ mode ═══════════════════════════════════════════════════════════ */

#define MODE_USAGE_LINE \
    "Usage: mode [scope|0 | meter|1 [submode] [layout] | startup [scope|meter]]\r\n"

/* `mode startup <m>`: the set, then the flush the old handler asked the
 * input task for, and what really happened to the record. */
static void mode_startup(int32_t v, env_t *e)
{
    const prop_pair_t p = pr(PROP_ID_SYS_STARTUP, v);
    model_result_t res;
    if (!apply(e, &p, 1, &res, NULL, NULL)) return;
    const char *name = e->startup_name((uint8_t)get(PROP_ID_SYS_STARTUP));
    const char *outcome = "";
    if (e->settings_flush(500u, &outcome))
        outf(e, "startup=%s (%s)\r\n", name, outcome);
    else
        outf(e, "startup=%s (save pending: input task did not answer in 500 ms)\r\n", name);
}

/* `mode`, `mode scope|0`, `mode meter|1 [submode] [layout]`, `mode startup
 * [scope|meter|home|siggen|restore]` -> sys.mode (+ meter.func,
 * meter.layout), sys.startup. The numeric forms are the keywords (see the
 * 2026-09-12 note in the history of usb_debug.c: `mode 0` used to fall out
 * of the bottom). */
void shell_alias_mode(const char *args, env_t *e)
{
    char alias_buf[56];
    const char *as_typed = args;

    if (args == NULL || *args == '\0') {
        const uint8_t sub = (uint8_t)get(PROP_ID_METER_FUNC), lay = (uint8_t)get(PROP_ID_METER_LAYOUT);
        outf(e, "current=%lu startup=%s meter_submode=%u (%s) layout=%u (%s)\r\n",
             (unsigned long)get(PROP_ID_SYS_MODE),
             e->startup_name((uint8_t)get(PROP_ID_SYS_STARTUP)),
             (unsigned)sub, e->meter_func_name(sub),
             (unsigned)lay, e->meter_layout_name(lay));
        return;
    }

    {
        size_t tok_len = 0;
        const char *keyword = NULL;

        while (args[tok_len] != '\0' && args[tok_len] != ' ' && args[tok_len] != '\t')
            tok_len++;

        if (tok_len == 1 && args[0] == '0') keyword = "scope";
        else if (tok_len == 1 && args[0] == '1') keyword = "meter";

        if (keyword != NULL) {
            if (strlen(keyword) + strlen(args + tok_len) >= sizeof(alias_buf)) {
                e->emit(MODE_USAGE_LINE);
                return;
            }
            strcpy(alias_buf, keyword);
            strcat(alias_buf, args + tok_len);
            args = alias_buf;
        }
    }

    /* (The gallery is closed by the mode switch itself now, on the input
     * task: model_hw_fw.c op_mode.) */
    if (strcmp(args, "scope") == 0) {
        const prop_pair_t p = pr(PROP_ID_SYS_MODE, MODE_OSCILLOSCOPE);
        model_result_t res;
        if (!apply(e, &p, 1, &res, NULL, NULL)) return;
        e->emit("mode=scope\r\n");
        return;
    }

    if (strncmp(args, "startup", 7) == 0 &&
        (args[7] == '\0' || args[7] == ' ' || args[7] == '\t')) {
        const char *value = args + 7;
        while (*value == ' ' || *value == '\t') value++;

        if (*value == '\0') {
            outf(e, "startup=%s\r\n", e->startup_name((uint8_t)get(PROP_ID_SYS_STARTUP)));
        } else if (strcmp(value, "scope") == 0) {
            mode_startup(STARTUP_SCOPE, e);
        } else if (strcmp(value, "meter") == 0) {
            mode_startup(STARTUP_METER, e);
        } else if (strcmp(value, "home") == 0) {
            mode_startup(STARTUP_HOME, e);
        } else if (strcmp(value, "siggen") == 0) {
            mode_startup(STARTUP_SIGGEN, e);
        } else if (strcmp(value, "restore") == 0) {
            mode_startup(STARTUP_RESTORE, e);
        } else {
            e->emit("Usage: mode startup [home|meter|scope|siggen|restore]\r\n");
        }
        return;
    }

    if (strncmp(args, "meter", 5) == 0 &&
        (args[5] == '\0' || args[5] == ' ' || args[5] == '\t')) {
        char buf[48];
        char *tok;
        char *saveptr = NULL;
        uint32_t submode = (uint32_t)get(PROP_ID_METER_FUNC);
        uint32_t layout = (uint32_t)get(PROP_ID_METER_LAYOUT);
        bool layout_given = false;

        if (strlen(args) >= sizeof(buf)) {
            e->emit("Usage: mode meter [submode] [layout]\r\n");
            return;
        }
        strcpy(buf, args);
        tok = strtok_r(buf, " \t", &saveptr);  /* meter */
        tok = strtok_r(NULL, " \t", &saveptr);
        if (tok && (parse_int(tok, &submode) != 0 || submode >= METER_SUBMODE_COUNT)) {
            outf(e, "Usage: mode meter [0-%u] [0-%u]\r\n",
                 (unsigned)(METER_SUBMODE_COUNT - 1),
                 (unsigned)(METER_LAYOUT_COUNT - 1));
            return;
        }
        tok = strtok_r(NULL, " \t", &saveptr);
        if (tok && (parse_int(tok, &layout) != 0 || layout >= METER_LAYOUT_COUNT)) {
            outf(e, "Usage: mode meter [0-%u] [0-%u]\r\n",
                 (unsigned)(METER_SUBMODE_COUNT - 1),
                 (unsigned)(METER_LAYOUT_COUNT - 1));
            return;
        }
        if (tok) layout_given = true;

        /* The function first: on another screen it is stored and the meter
         * entry that follows applies it, so the meter chip gets one function
         * change (the entry first would post the old function, then this
         * one: the double relay click of 2026-06-07). The function is asked
         * for even when it is the one in force: the old handler re-sent it
         * every time. */
        prop_pair_t p[3];
        uint8_t n = 0;
        p[n++] = pr(PROP_ID_METER_FUNC, (int32_t)submode);
        if (layout_given) p[n++] = pr(PROP_ID_METER_LAYOUT, (int32_t)layout);
        p[n++] = pr(PROP_ID_SYS_MODE, MODE_MULTIMETER);
        model_result_t res;
        e->meter_switch_begin();
        if (!apply(e, p, n, &res, NULL, NULL)) return;
        e->meter_switch_wait();
        const uint8_t sub = (uint8_t)get(PROP_ID_METER_FUNC), lay = (uint8_t)get(PROP_ID_METER_LAYOUT);
        outf(e, "mode=meter submode=%lu (%s) layout=%lu (%s)\r\n",
             (unsigned long)sub, e->meter_func_name(sub),
             (unsigned long)lay, e->meter_layout_name(lay));
        return;
    }

    outf(e, "mode: unrecognised argument '%s'\r\n", as_typed);
    e->emit(MODE_USAGE_LINE);
}
