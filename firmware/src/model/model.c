/*
 * OpenScope 2C53T - the property model: table, lookup, text, validation,
 * the single set path, defaults, instruments (see model.h).
 *
 * Pure: no FreeRTOS, no LCD, no driver header. Hardware is model_hw().
 */
#define MODEL_INTERNAL
#include "model.h"
#include "scope_state.h"   /* coupling_labels, probe_labels, trigger_*_labels */

#include <string.h>

/* ═══════════════════════════════════════════════════════════════════
 * The table
 * ═══════════════════════════════════════════════════════════════════ */

#define PROP(sym, id_, nm_, ty_, un_, de_, gr_, in_, ar_, mn_, mx_, st_, df_, fl_, lb_, lf_,   \
             ge_, se_, so_, ow_)                                                              \
    { .name = nm_, .owner = ow_, .labels = lb_, .label = lf_, .get = ge_, .set = se_,         \
      .store = so_, .min = mn_, .max = mx_, .def = df_, .id = id_, .flags = fl_, .step = st_, \
      .type = ty_, .unit = un_, .decimals = de_, .group = gr_, .arg = ar_, .inst = in_ },
const prop_desc_t prop_table[] = {
#include "model_table.h"
};
#undef PROP

_Static_assert(sizeof prop_table / sizeof prop_table[0] == PROP_IX_COUNT, "table / index enum");
_Static_assert(PROP_IX_COUNT < 0xFFFFu, "prop_count is a uint16_t");

/* Ids unique, at compile time: a duplicate is a duplicate case label. (Their
 * increasing order is checked by model_table_check(), which also needs it for
 * the binary search in prop_by_id.) */
static int prop_ids_unique(uint16_t id)
{
    switch (id) {
#define PROP(sym, id_, ...) case id_:
#include "model_table.h"
#undef PROP
        return 1;
    default:
        return 0;
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * Names, units, statuses
 * ═══════════════════════════════════════════════════════════════════ */

static const char *const k_group_prefix[PG_COUNT] = {
    "ch1", "ch2", "tb", "trig", "view", "gen", "meter", "sys", "fft", "il", "mask", "ui",
};
static const char *const k_unit_symbol[UNIT_COUNT] = {
    "", "V", "mV", "Hz", "%", "permille", "px", "ms", "min", "ppm", "", "counts",
};
static const char *const k_type_name[PT_COUNT] = { "bool", "enum", "int", "mask" };
static const char *const k_status_name[PS_COUNT] = {
    "OK", "CLAMPED", "INVALID", "REFUSED", "NOT_SET", "BUSY", "TIMEOUT",
};

const char *prop_group_prefix(uint8_t g) { return g < PG_COUNT ? k_group_prefix[g] : "?"; }
const char *prop_unit_symbol(uint8_t u)  { return u < UNIT_COUNT ? k_unit_symbol[u] : ""; }
const char *prop_type_name(uint8_t t)    { return t < PT_COUNT ? k_type_name[t] : "?"; }
const char *prop_status_name(uint8_t s)  { return s < PS_COUNT ? k_status_name[s] : "?"; }

/* ═══════════════════════════════════════════════════════════════════
 * Hardware ops and epochs
 * ═══════════════════════════════════════════════════════════════════ */

static const model_hw_ops_t k_no_ops;            /* all NULL until model_bind_hw() */
static const model_hw_ops_t *g_ops = &k_no_ops;

void model_bind_hw(const model_hw_ops_t *ops) { g_ops = ops ? ops : &k_no_ops; }

/* The deferral an op reported during the current set (model.h DEFERRED);
 * a static string, never a copy. Executor only. */
static const char *g_deferred;
void        model_hw_deferred(const char *why) { g_deferred = why; }
const char *prop_last_deferred(void)           { return g_deferred; }
const model_hw_ops_t *model_hw(void) { return g_ops; }

volatile uint32_t prop_epoch[PG_COUNT];
/* Bit g: group g's epoch moved since the last model_touched_take(). */
static uint16_t g_touched;
uint32_t prop_group_epoch(uint8_t g) { return g < PG_COUNT ? prop_epoch[g] : 0u; }
void     model_touch(uint8_t g)
{
    if (g < PG_COUNT) { prop_epoch[g]++; g_touched |= (uint16_t)(1u << g); }
}
uint16_t model_touched_take(void)
{
    const uint16_t t = g_touched;
    g_touched = 0u;
    return t;
}

void model_note(prop_note_t *note, const char *text)
{
    if (!note) return;
    size_t i = 0;
    if (text)
        for (; text[i] && i + 1 < sizeof note->text; i++) note->text[i] = text[i];
    note->text[i] = '\0';
}

/* ═══════════════════════════════════════════════════════════════════
 * Lookup
 * ═══════════════════════════════════════════════════════════════════ */

uint16_t prop_count(void) { return (uint16_t)PROP_IX_COUNT; }

const prop_desc_t *prop_at(uint16_t i) { return i < PROP_IX_COUNT ? &prop_table[i] : NULL; }

const prop_desc_t *prop_find(const char *name)
{
    if (!name) return NULL;
    for (uint16_t i = 0; i < PROP_IX_COUNT; i++)
        if (strcmp(prop_table[i].name, name) == 0) return &prop_table[i];
    return NULL;
}

const prop_desc_t *prop_by_id(uint16_t id)
{
    uint16_t lo = 0, hi = (uint16_t)PROP_IX_COUNT;
    while (lo < hi) {
        const uint16_t mid = (uint16_t)((lo + hi) / 2u);
        if (prop_table[mid].id == id) return &prop_table[mid];
        if (prop_table[mid].id < id) lo = (uint16_t)(mid + 1u);
        else                         hi = mid;
    }
    return NULL;
}

bool prop_glob_match(const char *pat, const char *s)
{
    const char *star = NULL, *resume = NULL;
    if (!pat || !s) return false;
    while (*s) {
        if (*pat == '*') { star = pat++; resume = s; continue; }
        if (*pat == *s)  { pat++; s++; continue; }
        if (star)        { pat = star + 1; s = ++resume; continue; }
        return false;
    }
    while (*pat == '*') pat++;
    return *pat == '\0';
}

bool prop_glob(const char *pat, uint16_t *i)
{
    if (!i) return false;
    while (*i < PROP_IX_COUNT) {
        const prop_desc_t *p = &prop_table[(*i)++];
        if (prop_glob_match(pat, p->name)) return true;
    }
    return false;
}

int32_t prop_get(const prop_desc_t *p)     { return (p && p->get) ? p->get(p) : 0; }
int32_t prop_default(const prop_desc_t *p) { return p ? p->def : 0; }
bool    prop_is_settable(const prop_desc_t *p)
{
    return p && !(p->flags & PF_RO) && p->set && p->store;
}

/* ═══════════════════════════════════════════════════════════════════
 * Text helpers
 * ═══════════════════════════════════════════════════════════════════ */

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

static bool str_ieq(const char *a, const char *b)
{
    while (*a && *b) if (lower(*a++) != lower(*b++)) return false;
    return *a == *b;
}

/* Copy [s, s+len) into out (n bytes), trimmed of spaces/tabs both ends. */
static void copy_trim(char *out, size_t n, const char *s, size_t len)
{
    while (len && (*s == ' ' || *s == '\t')) { s++; len--; }
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' ||
                   s[len - 1] == '\n')) len--;
    if (!n) return;
    if (len >= n) len = n - 1;
    memcpy(out, s, len);
    out[len] = '\0';
}

static void cat(char *out, size_t n, const char *s)
{
    size_t l = strlen(out);
    while (*s && l + 1 < n) out[l++] = *s++;
    if (n) out[l < n ? l : n - 1] = '\0';
}

/* Label of value v (enum) or of bit v (mask). */
static bool label_of(const prop_desc_t *p, int32_t v, char *out, size_t n)
{
    if (n) out[0] = '\0';
    if (p->label) return p->label(p, v, out, n);
    if (!p->labels) return false;
    if (p->type == PT_MASK) {
        if (v < 0 || v > 30) return false;
    } else if (v < p->min || v > p->max) {
        return false;
    }
    const char *t = p->labels[p->type == PT_MASK ? v : v - p->min];
    if (!t) return false;
    out[0] = '\0';
    cat(out, n, t);
    return true;
}

static void fmt_int(char *out, size_t n, int32_t v, uint8_t decimals)
{
    if (fmt_fixed(out, n, v, decimals) < 0 && n) out[0] = '\0';
}

static void fmt_mask(const prop_desc_t *p, int32_t v, char *out, size_t n)
{
    if (n) out[0] = '\0';
    if (v == 0) { cat(out, n, "none"); return; }
    bool first = true;
    for (int b = 0; b < 31; b++) {
        if (!(v & (1 << b))) continue;
        char t[16];
        if (!label_of(p, b, t, sizeof t)) {
            t[0] = 'b'; t[1] = 'i'; t[2] = 't';
            fmt_int(t + 3, sizeof t - 3, b, 0);
        }
        if (!first) cat(out, n, ",");
        cat(out, n, t);
        first = false;
    }
}

void prop_format_value(const prop_desc_t *p, int32_t v, char *out, size_t n)
{
    if (!out || !n) return;
    out[0] = '\0';
    if (!p) return;
    switch (p->type) {
    case PT_BOOL:
        cat(out, n, v ? "on" : "off");
        break;
    case PT_ENUM:
        /* A label array is canonical; a label hook's text may be measured
         * (it moves with calibration), so the index is. */
        if (p->labels && !p->label && label_of(p, v, out, n)) break;
        fmt_int(out, n, v, 0);
        break;
    case PT_MASK:
        fmt_mask(p, v, out, n);
        break;
    default:
        fmt_int(out, n, v, p->decimals);
        break;
    }
}

void prop_format(const prop_desc_t *p, int32_t v, char *out, size_t n)
{
    if (!out || !n) return;
    out[0] = '\0';
    if (!p) return;
    if ((p->type == PT_ENUM || p->type == PT_INT) && p->label && label_of(p, v, out, n)) return;
    prop_format_value(p, v, out, n);
    if (p->type == PT_INT && prop_unit_symbol(p->unit)[0]) {
        cat(out, n, " ");
        cat(out, n, prop_unit_symbol(p->unit));
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * Parsing
 * ═══════════════════════════════════════════════════════════════════ */

/* The one value whose label is s (case-insensitive); false if none or two. */
static bool match_label(const prop_desc_t *p, const char *s, int32_t *v)
{
    if (p->type != PT_ENUM && !(p->type == PT_INT && p->label)) return false;
    if (!p->labels && !p->label) return false;
    if ((int64_t)p->max - p->min > 1024) return false;      /* not a label set */
    unsigned hits = 0;
    int32_t found = 0;
    for (int32_t x = p->min; x <= p->max; x++) {
        char t[24];
        if (label_of(p, x, t, sizeof t) && t[0] && str_ieq(t, s)) { hits++; found = x; }
    }
    if (hits != 1) return false;
    *v = found;
    return true;
}

static bool parse_hex(const char *s, int32_t *v)
{
    uint32_t acc = 0;
    int nd = 0;
    if (!(s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))) return false;
    for (s += 2; *s; s++, nd++) {
        char c = lower(*s);
        uint32_t d;
        if (c >= '0' && c <= '9')      d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else return false;
        if (nd >= 8 || (acc >> 27)) return false;
        acc = (acc << 4) | d;
    }
    if (nd == 0 || acc > 0x7FFFFFFFu) return false;
    *v = (int32_t)acc;
    return true;
}

/* A number with the property's decimals and an optional trailing unit
 * symbol ("1.5 V", "1000Hz"). */
static bool parse_number(const prop_desc_t *p, const char *s, int32_t *v)
{
    char t[32];
    copy_trim(t, sizeof t, s, strlen(s));
    if (p->type == PT_INT && p->unit != UNIT_NONE) {
        const char *u = prop_unit_symbol(p->unit);
        size_t lt = strlen(t), lu = strlen(u);
        if (lu && lt > lu && str_ieq(t + lt - lu, u)) {
            t[lt - lu] = '\0';
            copy_trim(t, sizeof t, t, strlen(t));
        }
    }
    const uint8_t dec = (p->type == PT_INT) ? p->decimals : 0;
    return parse_fixed(t, dec, v) == 0;
}

static bool parse_bool_word(const char *s, int32_t *v)
{
    static const char *const on[]  = { "on", "true", "yes", "1" };
    static const char *const off[] = { "off", "false", "no", "0" };
    for (unsigned i = 0; i < 4; i++) {
        if (str_ieq(s, on[i]))  { *v = 1; return true; }
        if (str_ieq(s, off[i])) { *v = 0; return true; }
    }
    return false;
}

static bool parse_mask(const prop_desc_t *p, const char *s, int32_t *v)
{
    if (str_ieq(s, "none")) { *v = 0; return true; }
    if (str_ieq(s, "all"))  { *v = p->max; return true; }
    if (parse_hex(s, v)) return true;
    if (s[0] >= '0' && s[0] <= '9') return parse_fixed(s, 0, v) == 0;   /* no sign, no step */
    int32_t acc = 0;
    while (*s) {
        const char *e = s;
        while (*e && *e != ',') e++;
        char tok[16];
        copy_trim(tok, sizeof tok, s, (size_t)(e - s));
        int b;
        for (b = 0; b < 31; b++) {
            char t[16];
            if (label_of(p, b, t, sizeof t) && str_ieq(t, tok)) break;
        }
        if (b == 31) return false;
        acc |= (int32_t)(1u << b);
        s = *e ? e + 1 : e;
    }
    *v = acc;
    return true;
}

static int32_t wrap_or_saturate(const prop_desc_t *p, int64_t t)
{
    if (t >= p->min && t <= p->max) return (int32_t)t;
    if ((p->flags & PF_WRAP) && p->type != PT_INT) {
        const int64_t span = (int64_t)p->max - p->min + 1;
        int64_t r = (t - p->min) % span;
        if (r < 0) r += span;
        return (int32_t)(p->min + r);
    }
    if (p->type == PT_INT) return (int32_t)(t < INT32_MIN ? INT32_MIN : t > INT32_MAX ? INT32_MAX : t);
    return (int32_t)(t < p->min ? p->min : p->max);
}

static prop_status_t parse_value(const prop_desc_t *p, const char *s_in, bool allow_rel,
                                 int32_t *v)
{
    char s[PROP_TEXT_MAX];
    int32_t x;
    if (!p || !s_in || !v) return PS_INVALID;
    copy_trim(s, sizeof s, s_in, strlen(s_in));
    if (!s[0]) return PS_INVALID;

    /* 1. a word: a label, or on/off */
    if (p->type == PT_BOOL && parse_bool_word(s, &x)) { *v = x; return PS_OK; }
    if (match_label(p, s, &x)) { *v = x; return PS_OK; }

    /* 2. relative to the current value */
    if (allow_rel && p->type != PT_MASK) {
        const char *num = NULL;
        int sign = 0;
        if ((s[0] == '+' || s[0] == '-') && s[1] == '=') { sign = s[0] == '-' ? -1 : 1; num = s + 2; }
        else if ((s[0] == '+' || s[0] == '-') && p->min >= 0) { sign = s[0] == '-' ? -1 : 1; num = s + 1; }
        if (num) {
            char t[40];
            copy_trim(t, sizeof t, num, strlen(num));
            if (t[0] == '+' || t[0] == '-' || !parse_number(p, t, &x)) return PS_INVALID;
            *v = wrap_or_saturate(p, (int64_t)prop_get(p) + (int64_t)sign * x);
            return PS_OK;
        }
    }

    /* 3. an absolute number */
    if (p->type == PT_MASK) {
        if (!parse_mask(p, s, &x)) return PS_INVALID;
    } else if (!parse_number(p, s, &x)) {
        return PS_INVALID;
    }
    *v = x;
    return PS_OK;
}

prop_status_t prop_parse(const prop_desc_t *p, const char *s, int32_t *v)
{
    return parse_value(p, s, true, v);
}

/* ═══════════════════════════════════════════════════════════════════
 * Readable profile lines
 * ═══════════════════════════════════════════════════════════════════ */

int prop_line_format(const prop_desc_t *p, int32_t v, char *out, size_t n)
{
    char val[PROP_TEXT_MAX], lab[24];
    if (!out || !n) return -1;
    out[0] = '\0';
    if (!p) return -1;
    prop_format_value(p, v, val, sizeof val);
    cat(out, n, p->name);
    cat(out, n, " = ");
    cat(out, n, val);
    if (p->type == PT_INT && prop_unit_symbol(p->unit)[0]) {
        cat(out, n, " ");
        cat(out, n, prop_unit_symbol(p->unit));
    }
    if (p->label && p->type != PT_MASK && label_of(p, v, lab, sizeof lab) && lab[0]) {
        cat(out, n, "  # ");
        cat(out, n, lab);
    }
    return (int)strlen(out);
}

prop_status_t prop_line_parse(const char *line, const prop_desc_t **pp, int32_t *v)
{
    char name[32], val[PROP_TEXT_MAX];
    if (!line || !pp || !v) return PS_INVALID;
    *pp = NULL;
    while (*line == ' ' || *line == '\t') line++;
    if (!*line || *line == '#' || *line == '\r' || *line == '\n') return PS_OK;
    const char *eq = strchr(line, '=');
    if (!eq) return PS_INVALID;
    copy_trim(name, sizeof name, line, (size_t)(eq - line));
    const prop_desc_t *p = prop_find(name);
    if (!p) return PS_INVALID;
    *pp = p;
    const char *end = strchr(eq + 1, '#');
    copy_trim(val, sizeof val, eq + 1, end ? (size_t)(end - (eq + 1)) : strlen(eq + 1));
    /* A profile is absolute: no "+=", and "+N" is not a step. */
    return parse_value(p, val, false, v);
}

unsigned model_profile_write(uint16_t want, void (*emit)(const char *line, void *ctx), void *ctx)
{
    unsigned lines = 0;
    char buf[PROP_LINE_MAX];
    for (uint16_t i = 0; i < PROP_IX_COUNT; i++) {
        const prop_desc_t *p = &prop_table[i];
        if ((p->flags & (PF_RO | PF_ALIAS)) || !(p->flags & want)) continue;
        prop_line_format(p, prop_get(p), buf, sizeof buf);
        if (emit) emit(buf, ctx);
        lines++;
    }
    return lines;
}

/* ═══════════════════════════════════════════════════════════════════
 * Instruments
 * ═══════════════════════════════════════════════════════════════════ */

/* value = the sys.instrument value; mode = device_mode_t (props_sys.c
 * asserts both against ui.h). No default overrides yet: each instrument's
 * properties start on their built-in defaults. CANscope, when it exists,
 * is a fourth row reusing INST_SCOPE's channel/timebase/trigger properties
 * (it carries INST_CANSCOPE too) with overrides such as tb.code. */
static const instrument_desc_t k_instruments[] = {
    { "scope", 0, 0, INST_SCOPE, 0, NULL },
    { "meter", 1, 1, INST_METER, 0, NULL },
    { "gen",   2, 2, INST_GEN,   0, NULL },
};
#define N_INSTRUMENTS (sizeof k_instruments / sizeof k_instruments[0])

uint8_t model_instrument_count(void) { return (uint8_t)N_INSTRUMENTS; }

const instrument_desc_t *model_instrument_at(uint8_t i)
{
    return i < N_INSTRUMENTS ? &k_instruments[i] : NULL;
}

const instrument_desc_t *model_instrument_find(const char *name)
{
    for (uint8_t i = 0; name && i < N_INSTRUMENTS; i++)
        if (strcmp(k_instruments[i].name, name) == 0) return &k_instruments[i];
    return NULL;
}

bool prop_in_instrument(const prop_desc_t *p, const instrument_desc_t *in)
{
    return p && in && (p->inst & in->member);
}

int32_t prop_instrument_default(const prop_desc_t *p, const instrument_desc_t *in)
{
    if (!p) return 0;
    for (uint8_t i = 0; in && in->defaults && i < in->n_defaults; i++)
        if (in->defaults[i].id == p->id) return in->defaults[i].value;
    return prop_default(p);
}

const instrument_desc_t *model_instrument_active(void)
{
    const int32_t v = prop_get(P(SYS_INSTRUMENT));
    for (uint8_t i = 0; i < N_INSTRUMENTS; i++)
        if (k_instruments[i].value == v) return &k_instruments[i];
    return NULL;                                  /* "none": Home or Settings */
}

/* ═══════════════════════════════════════════════════════════════════
 * Setting
 * ═══════════════════════════════════════════════════════════════════ */

/* PS_OK (v in range), PS_CLAMPED (int, *v clamped), PS_INVALID. */
static prop_status_t check_range(const prop_desc_t *p, int32_t *v)
{
    switch (p->type) {
    case PT_BOOL:
        return (*v == 0 || *v == 1) ? PS_OK : PS_INVALID;
    case PT_MASK:
        return (*v >= 0 && (*v & ~p->max) == 0) ? PS_OK : PS_INVALID;
    case PT_INT:
        if (*v < p->min) { *v = p->min; return PS_CLAMPED; }
        if (*v > p->max) { *v = p->max; return PS_CLAMPED; }
        return PS_OK;
    default:
        return (*v >= p->min && *v <= p->max) ? PS_OK : PS_INVALID;
    }
}

static bool busy_for(const prop_desc_t *p, uint8_t origin)
{
    if (!(p->inst & INST_SCOPE) || origin == ORIGIN_AUTO || origin == ORIGIN_BOOT) return false;
    return g_ops->selfcal_busy && g_ops->selfcal_busy();
}

static bool may_note(const prop_desc_t *p, uint8_t origin)
{
    if (!(p->flags & PF_PERSIST) || origin == ORIGIN_BOOT) return false;
    if (origin == ORIGIN_SHELL && g_ops->shell_persist && !g_ops->shell_persist()) return false;
    return true;
}

prop_status_t prop_set_local(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note)
{
    prop_note_t scratch;
    if (!note) note = &scratch;
    note->text[0] = '\0';
    g_deferred = NULL;
    if (!prop_is_settable(p)) { model_note(note, "read only"); return PS_INVALID; }
    if (busy_for(p, origin))  { model_note(note, "self-cal running"); return PS_BUSY; }
    const prop_status_t range = check_range(p, &v);
    if (range == PS_INVALID)  { model_note(note, "out of range"); return PS_INVALID; }

    const int32_t old = prop_get(p);
    prop_status_t s = p->set(p, v, origin, note);
    const bool ok = (s == PS_OK || s == PS_CLAMPED);
    if (ok && range == PS_CLAMPED) s = PS_CLAMPED;
    /* Stored, hardware deferred (model.h DEFERRED): that is the answer,
     * over the setter's own note ("posture posted" was not posted). */
    if (!ok) g_deferred = NULL;
    else if (g_deferred) model_note(note, g_deferred);

    /* A failing setter keeps the old value; if one did not (a two-step
     * apply whose undo failed too), the change is still announced. */
    if (prop_get(p) != old) {
        model_touch(p->group);
        if (may_note(p, origin) && g_ops->note_change) g_ops->note_change();
    }
    return s;
}

prop_status_t prop_step_local(const prop_desc_t *p, int dir, uint8_t origin, prop_note_t *note)
{
    if (note) note->text[0] = '\0';
    if (!prop_is_settable(p) || p->type == PT_MASK) {
        model_note(note, p && p->type == PT_MASK ? "no step for a mask" : "read only");
        return PS_INVALID;
    }
    if (dir == 0) return PS_OK;
    const int32_t cur = prop_get(p);
    int32_t t;
    if (p->type == PT_BOOL) {
        t = (p->flags & PF_WRAP) ? !cur : (dir > 0);
    } else {
        int64_t raw = (int64_t)cur + (int64_t)dir * (p->step ? p->step : 1);
        if (raw < p->min || raw > p->max) {
            if ((p->flags & PF_WRAP) && p->type == PT_ENUM) t = wrap_or_saturate(p, raw);
            else t = (int32_t)(raw < p->min ? p->min : p->max);
        } else {
            t = (int32_t)raw;
        }
    }
    if (t == cur) return PS_OK;                    /* at the limit: nothing to do */
    return prop_set_local(p, t, origin, note);
}

prop_status_t prop_validate_pairs(const prop_pair_t *pairs, uint8_t n, uint8_t *failed)
{
    uint8_t dummy, hw = 0;
    if (!failed) failed = &dummy;
    *failed = n;
    if (n > MODEL_MAX_PAIRS || (n && !pairs)) return PS_INVALID;
    for (uint8_t i = 0; i < n; i++) {
        const prop_desc_t *p = prop_by_id(pairs[i].id);
        int32_t v = pairs[i].value;
        if (!prop_is_settable(p) || check_range(p, &v) == PS_INVALID) { *failed = i; return PS_INVALID; }
        if (p->flags & PF_HW) hw++;
    }
    if (hw > MODEL_MAX_HW_PAIRS) { *failed = n; return PS_INVALID; }
    return PS_OK;
}

prop_status_t prop_set_pairs_local(const prop_pair_t *pairs, uint8_t n, uint8_t origin,
                                   prop_note_t *note, uint8_t *failed)
{
    uint8_t dummy;
    if (!failed) failed = &dummy;
    prop_status_t s = prop_validate_pairs(pairs, n, failed);
    if (s != PS_OK) { model_note(note, n > MODEL_MAX_PAIRS ? "too many pairs" : "invalid pair"); return s; }
    prop_status_t worst = PS_OK;
    for (uint8_t i = 0; i < n; i++) {
        s = prop_set_local(prop_by_id(pairs[i].id), pairs[i].value, origin, note);
        if (s != PS_OK && s != PS_CLAMPED) { *failed = i; return s; }
        if (s == PS_CLAMPED) worst = PS_CLAMPED;
    }
    *failed = n;
    return worst;
}

unsigned model_load_defaults(uint16_t want, const instrument_desc_t *in, bool note)
{
    unsigned changed = 0;
    bool persisted = false;
    for (uint16_t i = 0; i < PROP_IX_COUNT; i++) {
        const prop_desc_t *p = &prop_table[i];
        if (!prop_is_settable(p) || (p->flags & PF_ALIAS) || !(p->flags & want)) continue;
        if (in && !(p->inst & in->member)) continue;
        const int32_t old = prop_get(p);
        p->store(p, in ? prop_instrument_default(p, in) : prop_default(p));
        if (prop_get(p) != old) {
            changed++;
            model_touch(p->group);
            if (p->flags & PF_PERSIST) persisted = true;
        }
    }
    if (note && persisted && g_ops->note_change) g_ops->note_change();
    return changed;
}

/* ═══════════════════════════════════════════════════════════════════
 * Self-check
 * ═══════════════════════════════════════════════════════════════════ */

static bool looks_numeric(const char *s)
{
    int32_t x;
    return parse_fixed(s, 9, &x) == 0 || parse_hex(s, &x);
}

unsigned model_table_check(void (*report)(const char *what, const prop_desc_t *p, void *ctx),
                           void *ctx)
{
    unsigned bad = 0;
#define BAD(what, p) do { bad++; if (report) report((what), (p), ctx); } while (0)
    for (uint16_t i = 0; i < PROP_IX_COUNT; i++) {
        const prop_desc_t *p = &prop_table[i];
        const char *dot = strchr(p->name, '.');
        if (!prop_ids_unique(p->id)) BAD("id not in the switch", p);
        if (i && prop_table[i - 1].id >= p->id) BAD("id not increasing", p);
        if (p->group >= PG_COUNT) { BAD("group", p); continue; }
        if ((p->id >> 8) != (uint16_t)(p->group + 1u)) BAD("id high byte != group + 1", p);
        if (!dot || strlen(k_group_prefix[p->group]) != (size_t)(dot - p->name) ||
            strncmp(p->name, k_group_prefix[p->group], (size_t)(dot - p->name)) != 0)
            BAD("name prefix != group", p);
        for (uint16_t j = 0; j < i; j++)
            if (strcmp(prop_table[j].name, p->name) == 0) BAD("duplicate name", p);
        if (p->type >= PT_COUNT || p->unit >= UNIT_COUNT) BAD("type/unit", p);
        if (p->min > p->max) BAD("min > max", p);
        if (p->step == 0) BAD("step 0", p);
        if (p->type == PT_BOOL && (p->min != 0 || p->max != 1)) BAD("bool range", p);
        if (p->type == PT_MASK && p->min != 0) BAD("mask min", p);
        if (p->type != PT_INT && p->decimals) BAD("decimals on a non-int", p);
        int32_t d = p->def;
        if (check_range(p, &d) != PS_OK) BAD("default out of range", p);
        const bool ro = (p->flags & PF_RO) != 0;
        if (ro != (p->set == NULL) || ro != (p->store == NULL)) BAD("RO <-> set/store", p);
        if (!p->get) BAD("no getter", p);
        if (!ro) {
            const unsigned forms = !!(p->flags & PF_PERSIST) + !!(p->flags & PF_USERCAL) +
                                   !!(p->flags & PF_SESSION) + !!(p->flags & PF_ALIAS);
            if (forms != 1) BAD("not exactly one save form", p);
        }
        if ((p->flags & PF_STORED_ONLY) && !(p->flags & PF_PERSIST)) BAD("stored-only, not persisted", p);
        if ((p->flags & (PF_HW | PF_STORED_ONLY)) == (PF_HW | PF_STORED_ONLY)) BAD("stored-only with HW", p);
        if (p->type == PT_ENUM) {
            for (int32_t v = p->min; v <= p->max; v++) {
                char t[24];
                if (!label_of(p, v, t, sizeof t) || !t[0]) { BAD("enum value without a label", p); break; }
                if (looks_numeric(t)) { BAD("label parses as a number", p); break; }
                int32_t back;
                if (!p->label && (!match_label(p, t, &back) || back != v)) { BAD("label not unique", p); break; }
            }
        }
        if (p->type == PT_MASK) {
            for (int b = 0; b < 31; b++) {
                if (!(p->max & (1 << b))) continue;
                char t[16];
                if (!label_of(p, b, t, sizeof t) || !t[0]) { BAD("mask bit without a label", p); break; }
                if (looks_numeric(t) || strchr(t, ',')) { BAD("mask label unusable", p); break; }
            }
        }
    }
#undef BAD
    return bad;
}
