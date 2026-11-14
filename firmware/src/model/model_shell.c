/*
 * OpenScope 2C53T - get / set / list / describe (model_shell.h)
 *
 * Runs on the shell task. Reads are direct (prop_get is safe on any task:
 * every backing field is <= 32 bits and aligned); sets go through
 * prop_request to the input task and print what the executor answered, then
 * the value read back (report the wire, not the request).
 *
 * Output (one line per property, scripts split on the first '='):
 *   get ch1.vdiv         ch1.vdiv=3  # 1.00V
 *   get gen.amp          gen.amp=1.5 V
 *   set ch1.vdiv 4       OK ch1.vdiv=4  # 2.00V
 *   (on the meter)       OK ch1.vdiv=4  # 2.00V ; stored: scope FE off
 *   set gen.freq 99999   CLAMPED gen.freq=50000 Hz
 *   multi-set            one line per pair; each stored pair's line says
 *                        "stored: ...", a line without it was applied live
 *   (relay fails)        NOT_SET ch1.vdiv (relay apply failed); old value kept: 3  # 1.00V
 *   set ch1.coupling 2   INVALID ch1.coupling=2 (out of range 0..1); nothing applied
 *   (input task busy)    TIMEOUT: input task busy (gallery/countdown/selfcal?); nothing applied
 */
#include "model_shell.h"
#include "model_exec.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LINE_MAX_ 160u

/* ═══ text helpers ═══════════════════════════════════════════════════ */

static void cat(char *out, size_t n, const char *s)
{
    size_t l = strlen(out);
    while (*s && l + 1 < n) out[l++] = *s++;
    out[l] = '\0';
}

/* out = the strings up to NULL, concatenated (truncated to n). */
static void join(char *out, size_t n, ...)
{
    va_list ap;
    const char *s;
    out[0] = '\0';
    va_start(ap, n);
    while ((s = va_arg(ap, const char *)) != NULL) cat(out, n, s);
    va_end(ap);
}

/* The canonical value, its unit for an int, and a measured label after '#'. */
static void value_text(const prop_desc_t *p, int32_t v, char *out, size_t n)
{
    char t[PROP_TEXT_MAX];
    out[0] = '\0';
    prop_format_value(p, v, t, sizeof t);
    cat(out, n, t);
    if (p->type == PT_INT && prop_unit_symbol(p->unit)[0]) {
        cat(out, n, " ");
        cat(out, n, prop_unit_symbol(p->unit));
    }
    if (p->label && p->type != PT_MASK) {
        char lab[PROP_TEXT_MAX];
        prop_format(p, v, lab, sizeof lab);
        if (lab[0] && strcmp(lab, out) != 0) {
            cat(out, n, "  # ");
            cat(out, n, lab);
        }
    }
}

void model_shell_get_line(const prop_desc_t *p, int32_t v, char *out, size_t n)
{
    char t[LINE_MAX_];
    if (!out || !n) return;
    out[0] = '\0';
    if (!p) return;
    value_text(p, v, t, sizeof t);
    cat(out, n, p->name);
    cat(out, n, "=");
    cat(out, n, t);
}

static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* Next whitespace-separated token of *s into tok; false at the end. */
static bool next_token(const char **s, char *tok, size_t n)
{
    const char *p = skip_ws(*s);
    size_t l = 0;
    if (!*p) return false;
    while (*p && *p != ' ' && *p != '\t') {
        if (l + 1 < n) tok[l++] = *p;
        p++;
    }
    tok[l] = '\0';
    *s = p;
    return true;
}

static uint32_t epoch_all(void)
{
    uint32_t s = 0;
    for (uint8_t g = 0; g < PG_COUNT; g++) s += prop_group_epoch(g);
    return s;
}

static bool has_star(const char *s) { return strchr(s, '*') != NULL; }

/* ═══ get ════════════════════════════════════════════════════════════ */

/* One argument: an exact name or a glob. A glob is read as one snapshot:
 * every value, then the epochs again, and once more if a change landed in
 * between (a seqlock with no lock; model.h THREADING). */
static void get_one(const char *pat, model_emit_fn emit)
{
    char line[LINE_MAX_];
    if (!has_star(pat)) {
        const prop_desc_t *p = prop_find(pat);
        if (!p) {
            snprintf(line, sizeof line, "ERR unknown property '%s' (try: list)", pat);
            emit(line);
            return;
        }
        model_shell_get_line(p, prop_get(p), line, sizeof line);
        emit(line);
        return;
    }
    int32_t vals[PROP_IX_COUNT];
    uint32_t e0;
    for (int tries = 0; tries < 2; tries++) {
        e0 = epoch_all();
        for (uint16_t i = 0; i < prop_count(); i++) {
            const prop_desc_t *p = prop_at(i);
            if (prop_glob_match(pat, p->name)) vals[i] = prop_get(p);
        }
        if (epoch_all() == e0) break;
    }
    unsigned hits = 0;
    for (uint16_t i = 0; i < prop_count(); i++) {
        const prop_desc_t *p = prop_at(i);
        if (!prop_glob_match(pat, p->name)) continue;
        model_shell_get_line(p, vals[i], line, sizeof line);
        emit(line);
        hits++;
    }
    if (!hits) {
        snprintf(line, sizeof line, "ERR no property matches '%s' (try: list)", pat);
        emit(line);
    }
}

void model_shell_get(const char *args, model_emit_fn emit)
{
    char tok[48];
    const char *s = args ? args : "";
    bool any = false;
    while (next_token(&s, tok, sizeof tok)) { get_one(tok, emit); any = true; }
    if (!any) emit("usage: get <prop|glob> [...]   e.g. get ch1.vdiv, get ch1.*, get *");
}

/* ═══ set ════════════════════════════════════════════════════════════ */

typedef struct {
    const prop_desc_t *p[MODEL_MAX_PAIRS];
    prop_pair_t        pair[MODEL_MAX_PAIRS];
    int32_t            asked[MODEL_MAX_PAIRS];
    uint8_t            n;
} set_req_t;

static void range_text(const prop_desc_t *p, char *out, size_t n);

/* Parse "name" + "value" into r; false (and the line emitted) on a bad one. */
static bool add_pair(set_req_t *r, const char *name, const char *val, model_emit_fn emit)
{
    char line[LINE_MAX_];
    if (r->n >= MODEL_MAX_PAIRS) {           /* r has 8 slots: refused before any write */
        snprintf(line, sizeof line, "INVALID %s (more than 8 pairs); nothing applied", name);
        emit(line);
        return false;
    }
    const prop_desc_t *p = prop_find(name);
    if (!p) {
        snprintf(line, sizeof line, "INVALID %s (unknown property); nothing applied", name);
        emit(line);
        return false;
    }
    if (!prop_is_settable(p)) {
        snprintf(line, sizeof line, "INVALID %s (read only); nothing applied", name);
        emit(line);
        return false;
    }
    int32_t v;
    if (!val[0] || prop_parse(p, val, &v) != PS_OK) {
        char rt[64];
        range_text(p, rt, sizeof rt);
        snprintf(line, sizeof line, "INVALID %s=%s (bad value; %s); nothing applied", name, val, rt);
        emit(line);
        return false;
    }
    r->p[r->n] = p;
    r->pair[r->n].id = p->id;
    r->pair[r->n].value = v;
    r->asked[r->n] = v;
    r->n++;
    return true;
}

/* The stored answer of pair p (model.h DEFERRED): the value is in force in
 * the model and the instrument's hardware path takes it when it is next
 * entered. "stored: scope FE off" / "stored: meter FE off", the texts the
 * ops give (model_hw_fw.c). */
static void stored_text(const prop_desc_t *p, char *out, size_t n)
{
    const char *inst = "its";
    for (uint8_t k = 0; k < model_instrument_count(); k++) {
        const instrument_desc_t *in = model_instrument_at(k);
        if (in && prop_in_instrument(p, in)) { inst = in->name; break; }
    }
    join(out, n, "stored: ", inst, " FE off", NULL);
}

/* One line per applied pair: OK / CLAMPED, the value read back, and after
 * " ; " either "stored: ..." (pair i was stored, not applied live) or the
 * note that pair i produced. A line without "stored" was applied live, or
 * has no hardware. */
static void emit_applied(const set_req_t *r, uint8_t upto, bool clamped, const model_result_t *res,
                         model_emit_fn emit)
{
    char line[LINE_MAX_], t[LINE_MAX_];
    for (uint8_t i = 0; i < upto; i++) {
        const int32_t now = prop_get(r->p[i]);
        model_shell_get_line(r->p[i], now, t, sizeof t);
        join(line, sizeof line, (clamped && now != r->asked[i]) ? "CLAMPED " : "OK ", t, NULL);
        if (res->deferred & (1u << i)) {
            stored_text(r->p[i], t, sizeof t);
            cat(line, sizeof line, " ; ");
            cat(line, sizeof line, t);
        } else if (res->note_from == i && res->note.text[0]) {
            cat(line, sizeof line, " ; ");
            cat(line, sizeof line, res->note.text);
        }
        emit(line);
    }
}

static void emit_not_applied(const set_req_t *r, uint8_t from, model_emit_fn emit)
{
    char line[LINE_MAX_];
    if (from >= r->n) return;
    snprintf(line, sizeof line, "  not applied:");
    for (uint8_t i = from; i < r->n; i++) { cat(line, sizeof line, " "); cat(line, sizeof line, r->p[i]->name); }
    emit(line);
}

void model_shell_set(const char *args, model_emit_fn emit)
{
    set_req_t r;
    char tok[PROP_TEXT_MAX + 32], line[LINE_MAX_], t[LINE_MAX_];
    const char *s = skip_ws(args ? args : "");
    memset(&r, 0, sizeof r);

    if (!next_token(&s, tok, sizeof tok)) {
        emit("usage: set <prop> <value>  |  set <p>=<v> [<p>=<v> ...]");
        emit("  value: a label, a number, or a step +=N / -=N; a bare +N / -N steps only where "
             "no value is negative (describe <prop>)");
        return;
    }
    if (strchr(tok, '=')) {
        do {
            char *eq = strchr(tok, '=');
            if (!eq) {
                snprintf(line, sizeof line, "INVALID '%s' (expected name=value); nothing applied", tok);
                emit(line);
                return;
            }
            *eq = '\0';
            if (!add_pair(&r, tok, eq + 1, emit)) return;
        } while (next_token(&s, tok, sizeof tok));
    } else {
        /* The rest of the line is the value ("1.5 V" has a space). */
        char val[PROP_TEXT_MAX];
        const char *v = skip_ws(s);
        size_t l = strlen(v);
        while (l && (v[l - 1] == ' ' || v[l - 1] == '\t')) l--;
        if (l >= sizeof val) l = sizeof val - 1;
        memcpy(val, v, l);
        val[l] = '\0';
        if (!add_pair(&r, tok, val, emit)) return;
    }

    model_result_t res;
    const prop_status_t st = prop_request(r.pair, r.n, ORIGIN_SHELL,
                                          prop_request_timeout(r.pair, r.n), &res);
    switch (st) {
    case PS_OK:
    case PS_CLAMPED:
        emit_applied(&r, r.n, st == PS_CLAMPED, &res, emit);
        return;
    case PS_TIMEOUT:
        emit(res.in_progress
             ? "TIMEOUT: input task busy (gallery/countdown/selfcal?); still applying: check with get"
             : "TIMEOUT: input task busy (gallery/countdown/selfcal?); nothing applied");
        return;
    default:
        break;
    }
    if (res.failed >= r.n) {                   /* the request as a whole */
        snprintf(line, sizeof line, "%s (%s)%s", prop_status_name(st), res.note.text,
                 st == PS_INVALID ? "; nothing applied" : "");
        emit(line);
        return;
    }
    const prop_desc_t *fp = r.p[res.failed];
    emit_applied(&r, res.applied, false, &res, emit);
    if (st == PS_INVALID) {
        value_text(fp, r.asked[res.failed], t, sizeof t);
        join(line, sizeof line, "INVALID ", fp->name, "=", t, " (",
             res.note.text[0] ? res.note.text : "out of range", "); nothing applied", NULL);
        emit(line);
        return;
    }
    value_text(fp, prop_get(fp), t, sizeof t);
    join(line, sizeof line, prop_status_name(st), " ", fp->name, " (",
         res.note.text[0] ? res.note.text : "no reason given", "); old value kept: ", t, NULL);
    if (res.applied < res.failed) {
        /* Refused before anything was queued (the not-wired list): the
         * pairs before it were not applied either. */
        cat(line, sizeof line, "; nothing applied");
        emit(line);
        return;
    }
    emit(line);
    emit_not_applied(&r, (uint8_t)(res.failed + 1u), emit);
}

/* ═══ list / describe ════════════════════════════════════════════════ */

static void range_text(const prop_desc_t *p, char *out, size_t n)
{
    char a[PROP_TEXT_MAX], b[PROP_TEXT_MAX], c[PROP_TEXT_MAX];
    switch (p->type) {
    case PT_BOOL:
        snprintf(out, n, "off/on");
        break;
    case PT_ENUM:
        snprintf(out, n, "%ld..%ld", (long)p->min, (long)p->max);
        break;
    case PT_MASK:
        snprintf(out, n, "0x%lX", (unsigned long)p->max);
        break;
    default:
        prop_format_value(p, p->min, a, sizeof a);
        prop_format_value(p, p->max, b, sizeof b);
        prop_format_value(p, (int32_t)p->step, c, sizeof c);
        join(out, n, a, "..", b, prop_unit_symbol(p->unit)[0] ? " " : "", prop_unit_symbol(p->unit),
             " step ", c, NULL);
        break;
    }
}

static void flags_text(const prop_desc_t *p, char *out, size_t n)
{
    static const struct { uint16_t f; const char *t; } k[] = {
        { PF_RO, "RO" }, { PF_HW, "HW" }, { PF_PERSIST, "P" }, { PF_USERCAL, "U" },
        { PF_SESSION, "S" }, { PF_ALIAS, "ALIAS" }, { PF_POSTED, "POST" }, { PF_WRAP, "W" },
        { PF_STORED_ONLY, "STORED" }, { PF_HIDDEN, "HID" },
    };
    out[0] = '\0';
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) {
        if (!(p->flags & k[i].f)) continue;
        if (out[0]) cat(out, n, ",");
        cat(out, n, k[i].t);
    }
    if (prop_unwired_reason(p)) { if (out[0]) cat(out, n, ","); cat(out, n, "NOWIRE"); }
}

void model_shell_list(const char *args, model_emit_fn emit)
{
    char pat[48] = "", line[256], rt[64], fl[48], val[PROP_TEXT_MAX];
    const char *s = args ? args : "";
    (void)next_token(&s, pat, sizeof pat);
    const size_t pl = strlen(pat);
    unsigned hits = 0;
    for (uint16_t i = 0; i < prop_count(); i++) {
        const prop_desc_t *p = prop_at(i);
        if (pl && (has_star(pat) ? !prop_glob_match(pat, p->name) : strncmp(p->name, pat, pl) != 0))
            continue;
        range_text(p, rt, sizeof rt);
        flags_text(p, fl, sizeof fl);
        prop_format_value(p, prop_get(p), val, sizeof val);
        snprintf(line, sizeof line, "%-17s %-4s %-22s %-10s %s", p->name, prop_type_name(p->type),
                 rt, val, fl);
        emit(line);
        hits++;
    }
    if (!hits) {
        snprintf(line, sizeof line, "ERR no property matches '%s'", pat);
        emit(line);
        return;
    }
    if (!pl)
        emit("flags: RO read only, HW owner driver, P user profile, U usercal, S session only, "
             "W wraps, POST posted, NOWIRE not settable here yet");
}

static const char *saved_text(const prop_desc_t *p)
{
    if (p->flags & PF_RO)          return "never (read only: the keys change it)";
    if (p->flags & PF_ALIAS)       return "never (an alias of another property)";
    if (p->flags & PF_STORED_ONLY) return "user profile (settings record); nothing applies it yet";
    if (p->flags & PF_PERSIST)     return "user profile (settings record)";
    if (p->flags & PF_USERCAL)     return "usercal record (cal user save)";
    if (p->flags & PF_SESSION)     return "session only (not in the user profile)";
    return "?";
}

void model_shell_describe(const char *args, model_emit_fn emit)
{
    char name[48], line[LINE_MAX_], t[LINE_MAX_], rt[64], fl[48];
    const char *s = args ? args : "";
    if (!next_token(&s, name, sizeof name)) { emit("usage: describe <prop>"); return; }
    const prop_desc_t *p = prop_find(name);
    if (!p) {
        snprintf(line, sizeof line, "ERR unknown property '%s' (try: list)", name);
        emit(line);
        return;
    }
    range_text(p, rt, sizeof rt);
    flags_text(p, fl, sizeof fl);
    snprintf(line, sizeof line, "%s: %s %s, id 0x%04X, group %s, flags %s", p->name,
             prop_type_name(p->type), rt, (unsigned)p->id, prop_group_prefix(p->group),
             fl[0] ? fl : "-");
    emit(line);
    value_text(p, prop_get(p), t, sizeof t);
    join(line, sizeof line, "  now:     ", t, NULL);
    emit(line);
    value_text(p, prop_default(p), t, sizeof t);
    join(line, sizeof line, "  default: ", t, " (built in)", NULL);
    emit(line);
    if (p->unit != UNIT_NONE || p->decimals) {
        snprintf(line, sizeof line, "  unit:    %s, %u decimal(s) on the wire value",
                 prop_unit_symbol(p->unit)[0] ? prop_unit_symbol(p->unit) : "(scaled)",
                 (unsigned)p->decimals);
        emit(line);
    }
    if (p->type == PT_ENUM || p->type == PT_MASK) {
        snprintf(line, sizeof line, "  %s:", p->type == PT_ENUM ? "values" : "bits");
        const int32_t lo = p->type == PT_ENUM ? p->min : 0, hi = p->type == PT_ENUM ? p->max : 30;
        for (int32_t v = lo; v <= hi; v++) {
            char item[48], lab[PROP_TEXT_MAX];
            if (p->type == PT_MASK) {
                if (!(p->max & (1L << v))) continue;
                prop_format_value(p, (int32_t)(1L << v), lab, sizeof lab);
            } else {
                prop_format(p, v, lab, sizeof lab);
            }
            char num[16];
            snprintf(num, sizeof num, " %ld=", (long)v);
            join(item, sizeof item, num, lab, NULL);
            if (strlen(line) + strlen(item) > 100u) { emit(line); snprintf(line, sizeof line, "         "); }
            cat(line, sizeof line, item);
        }
        emit(line);
    }
    if (prop_is_settable(p) && p->type != PT_MASK) {
        /* prop_parse: a step is +=N / -=N; a bare +N / -N steps only where
         * no value is negative, else it is the number itself. */
        emit(p->min < 0 ? "  steps:   +=N / -=N (a bare -N or +N is the value itself)"
                        : "  steps:   +=N / -=N, or +N / -N");
    }
    snprintf(line, sizeof line, "  saved:   %s", saved_text(p));
    emit(line);
    snprintf(line, sizeof line, "  owner:   %s", p->owner ? p->owner : "-");
    emit(line);
    const char *why = prop_unwired_reason(p);
    if (!prop_is_settable(p))
        snprintf(line, sizeof line, "  shell:   get only");
    else if (why)
        snprintf(line, sizeof line, "  shell:   get only for now: %s", why);
    else
        snprintf(line, sizeof line, "  shell:   set %s <value> (the input task applies it)", p->name);
    emit(line);
}

/* ═══ usb_debug.c rows ═══════════════════════════════════════════════ */

#ifndef MODEL_EXEC_TEST
#include "usb_debug.h"

static void usb_emit(const char *line) { (void)usb_debug_printf("%s\r\n", line); }

void cmd_model_get(const char *args)      { model_shell_get(args, usb_emit); }
void cmd_model_set(const char *args)      { model_shell_set(args, usb_emit); }
void cmd_model_list(const char *args)     { model_shell_list(args, usb_emit); }
void cmd_model_describe(const char *args) { model_shell_describe(args, usb_emit); }
#endif
