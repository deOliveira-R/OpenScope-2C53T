/*
 * OpenScope 2C53T - the property model (registry core)
 *
 * Every user setting is a named, typed PROPERTY: one const descriptor in
 * flash (model_table.h, one X-macro line per property) with a getter that
 * reads the variable the firmware already has (scope_state_t, the main.c
 * globals, siggen_config_t, sys_prefs, meas_select, ...) and a setter that
 * validates, applies through the owner driver and stores ONLY on success.
 * A property never owns a value: the model adds no shadow copy. The one
 * exception is the stored block (sys.lang, which nothing applies yet, and
 * fft.*, whose owner exists only while a spectrum view runs: props_sys.c).
 *
 * The wire value is an int32_t. The unit and the number of decimals say how
 * it is printed and parsed ("gen.amp" = 15, UNIT_V, 1 decimal -> "1.5 V").
 * Enum values follow the firmware's own enums (coupling_t, startup_mode_t,
 * math_op_t + 1, ...), never a menu's item order, because the same int32 is
 * what a profile line and a later self-describing record store.
 *
 * Hardware is reached only through model_hw_ops_t (bound once at boot; the
 * host tests bind fake ops that log calls), so this layer and props_*.c
 * include no driver header and own no peripheral.
 *
 * INSTRUMENTS. A property carries a group (the epoch it bumps) and an
 * instrument membership mask (INST_*). An instrument is a const table entry:
 * its member bit, the mode it maps to and a list of default overrides on top
 * of the built-in defaults, so "Scope", "Meter", "Generator" and later
 * "CANscope" (CH1/CH2 as CAN_H/CAN_L with its own timebase defaults) are
 * data, not code. sys.instrument is the model's name for the active one;
 * sys.mode (the old mode enum, settings and home included) stays as an alias.
 *
 * SAVE FORMS. PF_PERSIST = in the user profile (the settings record, M3).
 * PF_SESSION = not in the user profile, restored only by "resume last
 * session": Startup on Boot = Restore and the session snapshot
 * (model_session.h, util/session_store.h). PF_USERCAL = in the usercal record (explicit `cal user save`).
 * The readable profile is one `name = value [unit]` line per property
 * (prop_line_format / prop_line_parse), validated by the same setters.
 *
 * BUILT-IN DEFAULTS and USER SETTINGS are two things. The built-in defaults
 * are const, in this table (prop_default: each property's `def`; a build may
 * change them, never a running device). The user settings are the saved
 * record (PF_PERSIST values, settings_store.c), which always wins over the
 * defaults at boot. prop_is_changed says where the live value differs from
 * the default. "Reset to defaults" (model_reset_to_defaults, through the
 * setters, so the hardware follows) copies the built-in defaults over the
 * live state; settings_store_reset_to_defaults then saves them as the user
 * settings. Calibration is neither: no reset touches it (prop_reset_keeps).
 *
 * THREADING. Lookup, get, format and parse are pure and run on any task.
 * prop_set_local / prop_step_local / model_load_defaults run on the
 * executor only (the input task; the queue in front of it is model_exec.c,
 * M1). This file is host-tested by tests/test_model.c (make test-model).
 */
#ifndef MODEL_H
#define MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "units.h"

/* ── Types ─────────────────────────────────────────────────────────────── */

typedef enum { PT_BOOL, PT_ENUM, PT_INT, PT_MASK, PT_COUNT } prop_type_t;

/* The unit of the wire value: each names the units.h type the setter hands
 * the owner (UNIT_PPM_PERIOD -> ppm_period_t, UNIT_GAIN_E6 -> gain_e6_t,
 * UNIT_PCT -> percent_t ...). prop_unit_symbol() gives the text. */
typedef enum { UNIT_NONE, UNIT_V, UNIT_MV, UNIT_HZ, UNIT_PCT, UNIT_PERMILLE, UNIT_PX,
               UNIT_MS, UNIT_MIN, UNIT_PPM_PERIOD, UNIT_GAIN_E6, UNIT_REC_CODE,
               UNIT_COUNT } prop_unit_t;

/* Groups: one change epoch each (prop_epoch). The name prefix before the
 * first '.' is the group's (prop_group_prefix), and the id's high byte is
 * PG_x + 1. */
typedef enum { PG_CH1, PG_CH2, PG_TB, PG_TRIG, PG_VIEW, PG_GEN, PG_METER, PG_SYS,
               PG_FFT, PG_IL, PG_MASK, PG_UI, PG_COUNT } prop_group_t;

/* Instrument membership (prop_desc_t.inst). 0 = belongs to no instrument
 * (system settings, UI state). INST_CANSCOPE is reserved: the CANscope
 * instrument reuses ch1/ch2/tb/trig with its own defaults once it has a
 * view and a posture. */
enum {
    INST_SCOPE    = 1u << 0,
    INST_METER    = 1u << 1,
    INST_GEN      = 1u << 2,
    INST_CANSCOPE = 1u << 3,
};

enum {                                   /* prop_desc_t.flags */
    PF_RO          = 1u << 0,            /* raw route may get, not set (ui.*)                   */
    PF_HW          = 1u << 1,            /* setter reaches hardware through model_hw_ops_t      */
    PF_WRAP        = 1u << 2,            /* step wraps (enums the keys cycle); else saturates    */
    PF_PERSIST     = 1u << 3,            /* in the user profile / settings record                */
    PF_USERCAL     = 1u << 4,            /* in the usercal record (explicit `cal user save`)     */
    PF_STORED_ONLY = 1u << 5,            /* persisted but nothing applies it yet (sys.lang)      */
    PF_HIDDEN      = 1u << 6,            /* a build profile hid it from the menus                */
    PF_SESSION     = 1u << 7,            /* session only: restored by "resume last session" only
                                          * (the session snapshot, model_session.h)             */
    PF_ALIAS       = 1u << 8,            /* another name for state a primary property also shows
                                          * (sys.mode / sys.instrument): never saved or exported */
    PF_POSTED      = 1u << 9,            /* setter posts to another context and returns when
                                          * posted (mode/posture, mask requests): "posted" note   */
};

typedef enum { PS_OK = 0,      /* applied (value may equal the old one)                   */
               PS_CLAMPED,     /* applied, value was clamped to the range                 */
               PS_INVALID,     /* out of range / unknown name / RO: nothing touched       */
               PS_REFUSED,     /* a rule said no ("Keep one CH on")                       */
               PS_NOT_SET,     /* the owner driver failed: old value kept                 */
               PS_BUSY,        /* self-cal running / acquisition did not park             */
               PS_TIMEOUT,     /* shell only: the executor did not answer                 */
               PS_COUNT
             } prop_status_t;

typedef enum { ORIGIN_KEY, ORIGIN_SHELL, ORIGIN_AUTO, ORIGIN_BOOT, ORIGIN_PROFILE,
               ORIGIN_RESET,   /* model_reset_to_defaults: no settings note per property */
               ORIGIN_COUNT } prop_origin_t;

typedef struct { char text[24]; } prop_note_t;   /* popup / shell note, same 24 as `pb` */

struct prop_desc;
typedef prop_status_t (*prop_set_fn)(const struct prop_desc *p, int32_t v, uint8_t origin,
                                     prop_note_t *note);
typedef void          (*prop_store_fn)(const struct prop_desc *p, int32_t v);
typedef int32_t       (*prop_get_fn)(const struct prop_desc *p);
/* Label for value v; false = no label for it. */
typedef bool          (*prop_label_fn)(const struct prop_desc *p, int32_t v, char *out, size_t n);

typedef struct prop_desc {
    const char         *name;           /* "ch1.vdiv"                                     */
    const char         *owner;          /* "fpga_apply_vdiv + scope_vpos", for describe    */
    const char *const  *labels;         /* PT_ENUM / PT_MASK: names by value / bit, or NULL */
    prop_label_fn       label;          /* computed / measured text, or NULL               */
    prop_get_fn         get;
    prop_set_fn         set;            /* validate rules, apply, store on success; NULL = RO */
    prop_store_fn       store;          /* the backing state, no rules, no note: boot load,
                                         * defaults, resume (see STORE below). NULL = RO     */
    int32_t             min, max, def;  /* def = built-in default (see prop_default)       */
    uint16_t            id;             /* stable, never reused: (group + 1) << 8 | n        */
    uint16_t            flags;
    uint16_t            step;
    uint8_t             type, unit, decimals, group, arg, inst;   /* arg: channel 0/1 */
} prop_desc_t;

/* Ids and table indices: PROP_ID_CH1_VDIV, PROP_IX_CH1_VDIV, P(CH1_VDIV). */
#define PROP(sym, id, ...) PROP_ID_##sym = (id),
enum {
#include "model_table.h"
};
#undef PROP
#define PROP(sym, ...) PROP_IX_##sym,
enum {
#include "model_table.h"
    PROP_IX_COUNT
};
#undef PROP

extern const prop_desc_t prop_table[];
#define P(sym) (&prop_table[PROP_IX_##sym])

/* ── Lookup and introspection (pure, any task) ─────────────────────────── */
const prop_desc_t *prop_find(const char *name);       /* exact name, NULL if none    */
const prop_desc_t *prop_by_id(uint16_t id);
uint16_t           prop_count(void);
const prop_desc_t *prop_at(uint16_t i);               /* table order (= id order)    */
/* Iterate the names matching pat ('*' = any run, e.g. "ch1.*", "*.enabled"):
 * start with *i = 0; each true return leaves the match at prop_at(*i - 1). */
bool               prop_glob(const char *pat, uint16_t *i);
bool               prop_glob_match(const char *pat, const char *name);
int32_t            prop_get(const prop_desc_t *p);
int32_t            prop_default(const prop_desc_t *p);
const char        *prop_group_prefix(uint8_t group);  /* "ch1", ..., "ui"              */
const char        *prop_unit_symbol(uint8_t unit);    /* "V", "Hz", "%", "" ...      */
const char        *prop_type_name(uint8_t type);      /* "bool", "enum", "int", "mask" */
const char        *prop_status_name(uint8_t s);       /* "OK", "CLAMPED", ...        */
/* Not PF_RO: it has a setter and a store hook. */
bool               prop_is_settable(const prop_desc_t *p);

/* Text of value v as a person reads it: "on", "AC", "1.00V" (a measured
 * label), "1.5 V", "Freq,Vp-p". Always NUL-terminated. */
void          prop_format(const prop_desc_t *p, int32_t v, char *out, size_t n);
/* The canonical text that prop_parse reads back exactly: bool on/off, enum
 * label (a measured label gives the index instead), int fixed point without
 * the unit, mask names joined by ',' ("none" for 0). */
void          prop_format_value(const prop_desc_t *p, int32_t v, char *out, size_t n);
/* Text -> value: a label (case-insensitive, must match exactly one value),
 * an index or number ("3", "1.5" with the property's decimals, a trailing
 * unit symbol allowed: "1.5 V", "1000Hz"), on/off for bools, names or a
 * number for masks ("Freq,Vp-p", "0x507", "none", "all"). Relative to the
 * current value: "+=N" / "-=N" for every type, and "+N" / "-N" for a
 * property that cannot hold a negative value (enums wrap with PF_WRAP).
 * PS_OK, or PS_INVALID (*v untouched). The range check is prop_set_local's. */
prop_status_t prop_parse(const prop_desc_t *p, const char *s, int32_t *v);

/* ── Readable profile form ───────────────────────────────────────────── */
/* The longest value text (a full 12-name mask is 61) and line (< the shell's
 * 128-character line buffer). */
#define PROP_TEXT_MAX  72u
#define PROP_LINE_MAX  112u
/* "gen.amp = 1.5 V", "ch1.coupling = AC", "ch1.vdiv = 3  # 1.00V".
 * Returns the length (snprintf style). */
int           prop_line_format(const prop_desc_t *p, int32_t v, char *out, size_t n);
/* One line back: PS_OK with *pp and *v set; PS_INVALID for a bad line (and
 * *pp set when the name was known). A blank or '#' comment line returns
 * PS_OK with *pp = NULL. */
prop_status_t prop_line_parse(const char *line, const prop_desc_t **pp, int32_t *v);
/* Every property whose flags include one of `want` (and not PF_ALIAS / PF_RO)
 * in table order, its current value, one line per emit() call. Returns the
 * number of lines. */
unsigned      model_profile_write(uint16_t want, void (*emit)(const char *line, void *ctx),
                                  void *ctx);

/* ── Instruments ──────────────────────────────────────────────────────── */
typedef struct { uint16_t id; int32_t value; } prop_pair_t;

typedef struct {
    const char        *name;            /* "scope"                                */
    uint8_t            value;           /* its sys.instrument value               */
    uint8_t            mode;            /* device_mode_t it maps to (sys.mode)    */
    uint8_t            member;          /* INST_* bit its properties carry        */
    uint8_t            n_defaults;
    const prop_pair_t *defaults;        /* overrides of prop_default() for it     */
} instrument_desc_t;

uint8_t                  model_instrument_count(void);
const instrument_desc_t *model_instrument_at(uint8_t i);
const instrument_desc_t *model_instrument_find(const char *name);
bool                     prop_in_instrument(const prop_desc_t *p, const instrument_desc_t *in);
/* The default of p when `in` starts on defaults: its override, else prop_default(). */
int32_t                  prop_instrument_default(const prop_desc_t *p, const instrument_desc_t *in);

/* ── Setting (EXECUTOR ONLY) ─────────────────────────────────────────── */
/* The single path: (1) RO/NULL -> INVALID; (2) self-cal busy -> BUSY for
 * scope properties unless ORIGIN_AUTO; (3) bool/enum/mask out of range ->
 * INVALID, int out of range -> clamped (CLAMPED); (4) p->set applies and
 * stores on success; (5) prop_epoch[group] bumps if the value changed;
 * (6) PF_PERSIST + a changed value -> hw note_change, except ORIGIN_BOOT,
 * ORIGIN_RESET and ORIGIN_SHELL with shell_persist off. note may be NULL. */
prop_status_t prop_set_local(const prop_desc_t *p, int32_t v, uint8_t origin, prop_note_t *note);
/* dir steps (one step each): wraps with PF_WRAP, else saturates; a bool
 * toggles on any dir with PF_WRAP, else +1 = on, -1 = off. At the limit it
 * returns PS_OK and calls nothing. PT_MASK has no step: INVALID. */
prop_status_t prop_step_local(const prop_desc_t *p, int dir, uint8_t origin, prop_note_t *note);

/* Multi-set validation, all or nothing: every pair names a settable
 * property and a value in range (bool/enum/mask; ints are clamped later),
 * at most MODEL_MAX_PAIRS pairs and MODEL_MAX_HW_PAIRS PF_HW ones. PS_OK, or
 * the status with *failed = the first bad index (n for a count limit). */
#define MODEL_MAX_PAIRS     8u
#define MODEL_MAX_HW_PAIRS  4u
prop_status_t prop_validate_pairs(const prop_pair_t *pairs, uint8_t n, uint8_t *failed);
/* Validate all, then apply in order; stops at the first failure (no roll
 * back: rolling back a relay can fail too) and reports its index. */
prop_status_t prop_set_pairs_local(const prop_pair_t *pairs, uint8_t n, uint8_t origin,
                                   prop_note_t *note, uint8_t *failed);

/* One request of several pairs (EXECUTOR ONLY): between begin and end a
 * setter that posts to another context stages its value instead, and end
 * posts what was staged ONCE, with every value of the request in it: the
 * two mask tolerances travel as one SCOPE_MASK_REQ_TOL to the acquisition
 * task whatever that task is doing (model_prop_batches says which
 * properties stage). end returns PS_OK, or PS_NOT_SET when the post was
 * refused (the note says so). prop_set_pairs_local brackets its pairs with
 * them; so does the executor's own loop (model_exec.c). */
void          model_batch_begin(void);
prop_status_t model_batch_end(prop_note_t *note);
bool          model_prop_batches(const prop_desc_t *p);

/* STORE. A store hook writes the backing state with no rules and no
 * settings note. Where the model can reach the variable (scope_state_t,
 * main.c globals, sys_prefs, theme, meas_select, the stored-only block) it
 * moves no hardware: at boot the owners' inits push it, as today. Where an
 * owner keeps the state in its own file (gen.* in signal_gen.c, il.* in
 * fpga.c, mask.* in scope_mask.c) the store IS that owner's setter through
 * model_hw(), and does nothing while the ops are unbound. view.persist's is
 * a no-op: a load never claims the shared pool (settings_store.c's rule).
 * view.mode's and tb.roll's stores are not called by a load either (the
 * record's view byte is kept and set through the setters later:
 * model_boot_keep below).
 * So a store is a boot / start-up tool, not a runtime apply: at run time
 * "Reset to defaults" goes through the setters (model_reset_to_defaults).
 *
 * Built-in defaults through each store hook for the properties whose flags
 * include one of `want` (PF_PERSIST for "Start with defaults", PF_PERSIST |
 * PF_SESSION for a full reset). `in` != NULL: that instrument's members only
 * (system properties, inst == 0, are left alone), with its overrides. Bumps
 * the epochs that changed; note_change once if `note` and a persisted value
 * changed ("Reset to defaults" copies the defaults into the user profile).
 * Returns the number of values that changed. */
unsigned model_load_defaults(uint16_t want, const instrument_desc_t *in, bool note);

/* ── Reset to defaults (EXECUTOR ONLY) ───────────────────────────────── */

/* Why "Reset to defaults" leaves p alone (a short reason for the shell), or
 * NULL: it goes back to its built-in default. Kept: read-only state and
 * aliases; user calibration (PF_USERCAL: il.gain / il.offset / il.skew, and
 * il.knots, whose only set drops the piecewise map); sys.instrument, which
 * is the screen binding and not a setting (the screen stays where it is);
 * view.run, which only the OK key can apply (its setter is not wired). */
const char *prop_reset_keeps(const prop_desc_t *p);

/* p is settable, not an alias, and its value is not its built-in default. */
bool        prop_is_changed(const prop_desc_t *p);

typedef struct {
    uint8_t  status;        /* prop_status_t: OK, BUSY (nothing touched), NOT_SET (some left) */
    uint8_t  changed;       /* properties whose value moved                                    */
    uint8_t  left;          /* properties a reset covers that are still not at their default   */
    uint16_t first_left;    /* the id of the first of them (0: none)                           */
} model_reset_report_t;

/* Every property a reset covers (not prop_reset_keeps) back to its built-in
 * default THROUGH ITS SETTER, in table order, so the rules run and the
 * hardware follows (ranges, timebase, trigger, generator off, backlight...;
 * a hardware path that is not live now takes the stored value when it is
 * next entered: DEFERRED below). Only a value that is not already the
 * default is set. Table order is the order model_load_defaults uses and
 * satisfies the setters' rules (channel enables before trig.source,
 * view.mode before view.persist); a property a rule or an owner refused is
 * tried once more after all the others, then reported in `left`.
 *
 * NOT the boot's path, on purpose: a boot load writes the backing state
 * through the store hooks with no hardware and lets each owner's init push
 * it; those inits cannot be run again on a running device.
 *
 * PS_BUSY while self-cal runs: nothing is touched. No settings note is made
 * here (ORIGIN_RESET): saving is settings_store_reset_to_defaults's, once.
 * between (may be NULL) runs before each set after the first: the health
 * check-in. Returns rep->status. */
prop_status_t model_reset_to_defaults(void (*between)(void), model_reset_report_t *rep);

/* The instrument screen the settings record held when it was last loaded
 * (STARTUP_SCREEN_*, ui/startup_policy.h; 0 = none: before a load, an older
 * record, Home, Settings, garbage). Startup on Boot = Restore boots into it;
 * sys.lastscreen reads it. model_last_screen_load is the record codec's
 * (model_persist_v5.c): it clamps the raw byte. */
uint8_t model_last_screen(void);
void    model_last_screen_load(uint8_t raw);
/* The value a capture writes into the record's last-screen byte: the loaded
 * one until model_last_screen_note_power_off() names the screen shown at an
 * orderly power-off (settings_store_flush_power_off; nothing else calls
 * it). sys.lastscreen does not follow it. */
void    model_last_screen_note_power_off(uint8_t screen);
uint8_t model_last_screen_record(void);

/* VALUES A LOAD KEEPS FOR THE SETTERS (the boot apply). A load runs before
 * the scheduler and may only write backing state (STORE above). A value
 * whose owners need the scheduler, the bound ops, the shared pool or an
 * acquisition park is therefore KEPT by the load (model_boot_keep: no live
 * state moved) and put in force later through the property's normal setter,
 * ORIGIN_BOOT (no settings note, not refused by a self-cal run), on the
 * executor once the ops are bound (model_fw_start -> model_boot_apply). Two
 * loads keep values: the settings record's view byte, view.mode and tb.roll
 * (model_persist_v5.c V5_VIEW) and, with Startup on Boot = Restore, every value of the session
 * snapshot (model_session.h). The later keep of a property replaces the
 * earlier one.
 *
 * model_boot_apply, the rule:
 *   - TABLE ORDER (= id order), the order "Reset to defaults" and
 *     model_load_defaults use and the one the setters' rules need: the
 *     channel enables before trig.source (a source on a channel that is off
 *     is refused), tb.roll before view.mode (a boot is in Y-T, the view
 *     that takes roll, until view.mode is applied; the record and the
 *     snapshot never hold roll with another view), view.mode before
 *     view.persist (persistence evicts a spectrum view), view.cursor before
 *     view.cursel. tb.roll comes before tb.rollstep: a Restore boot with
 *     roll on and a step other than 0 writes Roll twice (tb.roll at the step
 *     in RAM, 0, then tb.rollstep at the saved one);
 *   - only a value that is not already in force is set (the settings record
 *     put the persisted ones there: no relay, no park for those);
 *   - one request (model_batch_begin / end): the two mask tolerances post
 *     once;
 *   - a refused set does not stop the others; what was refused is tried
 *     once more after all the others (a rule may have wanted a later
 *     property first), as the reset does;
 *   - the verdict is read at the end, from the getters: APPLIED when the
 *     kept value is the one in force, else REFUSED. A refused property is
 *     left where the settings record or the built-in default put it.
 *   between (may be NULL) runs before each set after the first: the health
 *   check-in, so one acquisition park is the longest gap.
 *
 * A keep is STICKY (the settings record's view byte: view.mode, tb.roll) or not (the snapshot's
 * values). A refused sticky keep stays kept: model_boot_kept() still names
 * it, every capture writes it instead of the live value (the record keeps a
 * view the boot could not show), and a later model_boot_apply_one() may put
 * it in force. A refused keep that is not sticky is only reported
 * (model_boot_state, model_boot_report): the live value is what the next
 * capture saves. A keep is dropped by the first change of the property
 * that is not the boot apply: a set that succeeds (the value in force
 * included), another setter moving it (model_changed_also; not during the
 * boot apply, whose verdict is read at the end), and Reset to defaults
 * (model_boot_forget_all). */
typedef enum { BOOT_KEEP_NONE = 0,   /* nothing kept for it                              */
               BOOT_KEEP_PENDING,    /* kept, model_boot_apply has not run               */
               BOOT_KEEP_APPLIED,    /* in force                                         */
               BOOT_KEEP_REFUSED,    /* the setter said no, twice: left as it was        */
             } boot_keep_state_t;
typedef struct {
    uint8_t  kept;           /* values the loads kept                                    */
    uint8_t  set;            /* of those, values a setter was called for                 */
    uint8_t  applied;        /* in force at the end (already in force included)          */
    uint8_t  refused;        /* not in force at the end                                  */
    uint16_t first_refused;  /* the id of the first of them (0: none)                    */
} model_boot_report_t;
void    model_boot_keep(const prop_desc_t *p, int32_t v, bool sticky);   /* a load's; any context
                                                                         * before the apply */
/* model_boot_apply stops trying hardware (PF_HW) properties once more than
 * this many of them were refused: a boot with an owner that never answers
 * stays bounded (about MAX + 1 park timeouts). */
#define MODEL_BOOT_HW_REFUSED_MAX 3u
extern unsigned model_boot_hw_refused_max;   /* the limit in force (host tests lower it) */
unsigned model_boot_apply(void (*between)(void));    /* EXECUTOR ONLY; returns the refused count */
bool    model_boot_apply_one(const prop_desc_t *p);  /* EXECUTOR ONLY; true: kept and now in force */
/* p has a value kept that is not in force (pending, or sticky and refused);
 * *v (may be NULL) gets it. */
bool    model_boot_kept(const prop_desc_t *p, int32_t *v);
void    model_boot_forget(const prop_desc_t *p);
void    model_boot_forget_all(void);
/* Nothing kept, nothing reported: the table as a power cycle leaves it (RAM
 * is zero at boot; the host tests' "power cycle" calls this). */
void    model_boot_reset(void);
uint8_t model_boot_state(const prop_desc_t *p);      /* boot_keep_state_t */
const model_boot_report_t *model_boot_report(void);  /* of the last model_boot_apply */
/* v is a value p can hold: bool 0/1, enum min..max, mask bits, int min..max
 * (not clamped: a load drops a value out of range). */
bool    prop_value_in_range(const prop_desc_t *p, int32_t v);

/* The settings record's view byte across a power cycle: the case of the
 * above with two properties, both sticky. The byte (offset 23,
 * model_persist_v5.c V5_VIEW) is the pair (view.mode, tb.roll): 0 yt, 1 yt
 * with roll, 2 xy, 3 fft, 4 split, 5 waterfall (model_view_record_of; roll
 * counts only in a view that takes it, view_table.h). Older firmware reads
 * the same byte with the same meaning. model_view_boot_load is the
 * codec's (above MODEL_VIEW_RECORD_MAX -> yt) and keeps both;
 * model_view_boot_apply() is model_boot_apply_one() of each: true when both
 * kept values are the ones in force. model_view_record() is the byte every
 * capture writes: of each property the kept value until it is in force,
 * then the live one. A boot whose apply was refused (no pool, the Roll write
 * failed) therefore comes up in yt without roll and still saves the view it
 * could not show, whatever else is saved in that session, until the view
 * changes: a set of view.mode or tb.roll that succeeds (the value in force
 * included; the byte is one field, so either drops the keeps of both:
 * model_view_boot_forget), view.persist evicting a spectrum view, and Reset
 * to defaults (the reset loop skips a property already at its default, and
 * yt without roll is where a refused boot left it). */
#define MODEL_VIEW_MODE_MAX    4u   /* view.mode: yt, xy, fft, split, waterfall */
#define MODEL_VIEW_RECORD_MAX  5u   /* the record's byte: yt, yt with roll, xy, fft, split, waterfall */
void    model_view_boot_load(uint8_t raw);
/* Split's pair (view.upper, view.lower) read at once, [0] upper, [1] lower:
 * the display's read never sees half a swap. Any task. */
void    model_view_pair(uint8_t half[2]);
bool    model_view_boot_apply(void);            /* EXECUTOR ONLY */
void    model_view_boot_forget(void);
uint8_t model_view_record(void);
uint8_t model_view_record_of(int32_t view, int32_t roll);   /* the byte of a (view.mode, tb.roll) pair */

/* meter.hold / meter.rel / meter.degf: model_hw_ops_t.meter_flag_get/set. */
enum { METER_FLAG_HOLD = 0, METER_FLAG_REL, METER_FLAG_DEGF };
/* meter.beep: the continuity beeper's threshold in ohms (10, 30, 50, 100). */
#define METER_BEEP_COUNT 4
uint16_t meter_beep_ohms(void);

/* ── Change notification ─────────────────────────────────────────────── */
extern volatile uint32_t prop_epoch[PG_COUNT];   /* bumped by the executor on a change */
uint32_t prop_group_epoch(uint8_t g);
/* Bump group g's epoch (executor only). prop_set_local bumps the property's
 * own group; a setter calls this for what it changes in another group
 * (ch.enabled moves ui.ach, Single sets view.run, Roll redraws the view),
 * and an owner that lands a POSTED change later (the mode switch, a mask
 * request) calls it when the change is in force. */
void     model_touch(uint8_t g);
/* A setter changed ANOTHER property's value as part of its own rule (a
 * channel going off moves trig.source): that property's group epoch, and
 * the settings note its own set would have made (PF_PERSIST, by origin).
 * So a persisted change is noted whoever made it, also when the property
 * being set is not persisted itself. */
void     model_changed_also(const prop_desc_t *p, uint8_t origin);
/* The groups whose epoch moved since the last call (bit g = PG_x), and
 * forget them. The executor takes them around each request to know what a
 * request changed (model_exec.c: the repaint goes only to a screen that
 * shows one of them). Executor only. */
uint16_t model_touched_take(void);

/* ── Hardware ops (bound once at boot; fake in the host tests) ───────── */
struct scope_cursor_axes;           /* scope_cursor.h */
/* Each is the owner API the setter calls (model_hw_fw.c binds the real ones
 * in M1). Channels are 0 = CH1, 1 = CH2. bool = the owner did it. Getters
 * here are for state an owner keeps in its own file (fpga.c, scope_mask.c,
 * persistence.c). */
typedef struct {
    /* scope front end and acquisition (fpga.c, scope_vpos.c) */
    bool    (*vdiv)(uint8_t ch, uint8_t idx);          /* fpga_apply_vdiv + scope_vpos_apply_ch */
    bool    (*coupling)(uint8_t ch, uint8_t coupling); /* fpga_apply_coupling (relay readback)  */
    bool    (*channel_enables)(void);                  /* fpga_apply_channel_enables (reads state) */
    bool    (*trigger_source_follow)(void);            /* reg 0x06 for a source the chN.enabled
                                                        * rule moved: fpga_apply_trigger_source in
                                                        * every posture, as the routing is        */
    bool    (*interleave_active)(void);                /* fpga_interleave_active                */
    bool    (*vpos)(uint8_t ch, int16_t pos);          /* scope_vpos_set; false = marker only   */
    bool    (*timebase)(uint8_t code);                 /* fpga_apply_timebase                   */
    bool    (*roll)(void);                             /* fpga_apply_roll (reads state)         */
    bool    (*trigger_source)(void);                   /* fpga_apply_trigger_source (reads state) */
    bool    (*trigger_level)(int16_t level);           /* fpga_apply_trigger_level              */
    void    (*single_rearm)(void);                     /* fpga_acq_single_rearm                 */
    bool    (*single_done)(void);                      /* fpga_acq_single_done: SINGLE holds its record */
    void    (*paused_latch)(void);                     /* scope_ui_paused_latch: the ranges at STOP */
    void    (*cursor_axes)(struct scope_cursor_axes *ax); /* scope_ui_cursor_axes: the plot the
                                                        * cursor lines are put on now             */
    uint8_t (*trigger_code)(void);                     /* fpga_acq_trig_code_get (trig.code)    */
    /* views and the shared pool (scope_apply.c) */
    bool    (*view_select)(uint8_t view);              /* scope_view_select; false = not in build */
    bool    (*persist_on)(uint8_t mode);               /* scope_apply_persist_on                */
    void    (*persist_off)(void);                      /* scope_apply_persist_off               */
    uint8_t (*persist_mode)(void);                     /* persist_get_mode                      */
    void    (*fft_settings)(void);                     /* scope_apply_fft_settings: fft.* -> the live FFT */
    /* mask test (scope_mask.c, applied by the acquisition task) */
    bool    (*mask_present)(void);
    void    (*mask_get)(uint8_t *tol_v, uint8_t *tol_h, bool *stop_on_fail);  /* pending or applied */
    bool    (*mask_tol)(uint8_t tol_v, uint8_t tol_h); /* scope_mask_post(REQ_TOL)              */
    bool    (*mask_stopfail)(bool on);                 /* scope_mask_post(REQ_STOPFAIL)         */
    uint8_t (*mask_state)(void);                       /* scope_mask_state (mask.state)         */
    bool    (*mask_hold)(void);                        /* scope_mask_hold_active (mask.hold)    */
    bool    (*mask_pending)(void);                     /* a posted request is not acknowledged yet */
    bool    (*mask_tol_pending)(void);                 /* ... and it is the last tolerance request */
    /* generator (signal_gen.c owns its state and TMR7/DMA2/DAC2) */
    void    (*gen_wave)(uint8_t wave);
    void    (*gen_freq)(uint32_t hz);
    void    (*gen_amp)(uint8_t dv);
    void    (*gen_duty)(percent_t duty);
    void    (*gen_out)(bool on);
    void    (*gen_bootout)(bool on);
    /* meter: the rest of a function change once the setter has stored it
     * (cancel the software autoselect and release hold / relative unless
     * keep_autoselect, the autoselect's own candidate; reset min/max; post
     * the meter posture). false = not posted: the setter puts the old
     * function back. */
    bool    (*meter_func)(uint8_t submode, bool keep_autoselect);
    bool    (*meter_link_on)(void);                    /* the meter link is on now (meter.live) */
    /* Meter view state its owners keep (meter_ui.c: hold, relative;
     * meter_data.c: the temperature unit). which = METER_FLAG_*. A set with
     * user = true is the key's action (hold freezes the shown reading and
     * answers false without one; relative takes the reading as its
     * reference; degF resets min/max, hold and relative as stock does);
     * user = false is a load: the flag only (a hold cannot be loaded). */
    bool    (*meter_flag_get)(uint8_t which);
    bool    (*meter_flag_set)(uint8_t which, bool on, bool user);
    /* After meter.layout changed: the Limits view takes its limits from the
     * reading when it has none for this function (meter_ui.c). */
    void    (*meter_layout)(uint8_t layout);
    /* system */
    bool    (*mode)(uint8_t device_mode);              /* the mode switch (input_mode_switch);
                                                        * model_touch(PG_SYS / PG_UI) on landing */
    void    (*backlight)(uint8_t pct);                 /* sys_pwm                               */
    void    (*autooff)(uint8_t mins);                  /* re-arm the input task's idle timer    */
    /* interleave (fpga.c keeps float gain/offset; typed ints at this boundary) */
    bool    (*il_enable)(bool on);
    bool    (*il_enabled)(void);
    bool    (*il_corr_set)(gain_e6_t gain, int32_t offset_x100);
    void    (*il_corr_get)(gain_e6_t *gain, int32_t *offset_x100);
    uint8_t (*il_knots)(void);                         /* knots in the level map (0 = linear only);
                                                        * il_corr_set drops them (fpga.c)       */
    bool    (*il_skew_set)(ppm_period_t skew);
    ppm_period_t (*il_skew_get)(void);
    /* executor services */
    bool    (*selfcal_busy)(void);                     /* scope_selfcal_busy                    */
    bool    (*shell_persist)(void);                    /* shell_persist on (bench sweeps: off)  */
    void    (*note_change)(void);                      /* settings_store_note_change(now)       */
} model_hw_ops_t;

/* DEFERRED (the owner's model, 2026-11-13). The device is a measuring
 * instrument that happens to have a screen and keys: each instrument keeps
 * its own state, and the UI binding (sys.instrument) only says which one the
 * screen and keys act on. A set of a property whose instrument is not the
 * bound one ALWAYS updates the state. Its hardware apply happens at once when
 * that instrument's hardware path is live in the current front-end posture;
 * otherwise the op skips the hardware, calls model_hw_deferred(why) and
 * returns true, the setter stores as on success, and the posture's entry
 * code applies the stored value when that posture is next entered
 * (model_hw_fw.c lists, per op, what is live in which posture and where the
 * re-apply happens). prop_set_local then puts `why` in the note ("stored:
 * scope FE off"), so the answer says which happened, and
 * prop_last_deferred() gives it to the executor. Executor only. */
void        model_hw_deferred(const char *why);
/* The deferral reason of the last prop_set_local (NULL: applied live, or no
 * hardware, or it failed). Executor only. */
const char *prop_last_deferred(void);
/* Scope entry: re-send the scope's SPI3 registers from scope_state through
 * the ops, for timebase, roll and trigger sets that were stored while the
 * scope front end was not live: reg 0x01 (the roll code with roll on, else
 * the timebase; never both), reg 0x06 (trigger source), reg 0x08 (trigger
 * level). model_hw_fw.c calls it on the executor once the scope's posture is
 * back. between (may be NULL) runs before each op after the first: the
 * health check-in, so one acquisition park is the longest gap (plan 2.5).
 * Stops at the first refusal (a park that failed once would fail again and
 * cost 1 s each): false, the rest not sent; the state stays. */
bool        model_scope_reapply(void (*between)(void));

/* Bind the ops (NULL entries are allowed: the call is skipped and the setter
 * treats it as done, or as refused where it needs an answer). */
void                  model_bind_hw(const model_hw_ops_t *ops);
const model_hw_ops_t *model_hw(void);

/* Self-check of the table (names, ids, ranges, labels); 0 = sound, else the
 * number of problems found. Host tests call it; the shell's `arch` may. */
unsigned model_table_check(void (*report)(const char *what, const prop_desc_t *p, void *ctx),
                           void *ctx);

/* ── Internal: what model_table.h names (model.c and props_*.c only) ─── */
/* fft.avg value -> spectra averaged (0 = off): what the FFT and the settings
 * record keep. */
#define MODEL_FFT_AVG_STEPS 6
extern const uint8_t model_fft_avg_counts[MODEL_FFT_AVG_STEPS];

#ifdef MODEL_INTERNAL
#define MODEL_PROP_FNS(x)                                                          \
    int32_t       x##_get(const prop_desc_t *p);                                   \
    prop_status_t x##_set(const prop_desc_t *p, int32_t v, uint8_t origin,        \
                          prop_note_t *note);                                      \
    void          x##_store(const prop_desc_t *p, int32_t v);

/* props_scope.c */
MODEL_PROP_FNS(ch_enabled)  MODEL_PROP_FNS(ch_vdiv)     MODEL_PROP_FNS(ch_coupling)
MODEL_PROP_FNS(ch_probe)    MODEL_PROP_FNS(ch_bw)       MODEL_PROP_FNS(ch_pos)
MODEL_PROP_FNS(ch_meas)     MODEL_PROP_FNS(tb_code)     MODEL_PROP_FNS(tb_roll)
MODEL_PROP_FNS(tb_rollstep) MODEL_PROP_FNS(trig_mode)   MODEL_PROP_FNS(trig_edge)
MODEL_PROP_FNS(trig_source) MODEL_PROP_FNS(trig_level)  MODEL_PROP_FNS(trig_x)
MODEL_PROP_FNS(trig_soft)   MODEL_PROP_FNS(view_mode)   MODEL_PROP_FNS(view_persist)
MODEL_PROP_FNS(view_math)   MODEL_PROP_FNS(view_cursor) MODEL_PROP_FNS(view_ts)
MODEL_PROP_FNS(view_mh)     MODEL_PROP_FNS(view_run)    MODEL_PROP_FNS(il_enabled)
MODEL_PROP_FNS(il_gain)     MODEL_PROP_FNS(il_offset)   MODEL_PROP_FNS(il_skew)
MODEL_PROP_FNS(il_knots)
MODEL_PROP_FNS(mask_tolv)   MODEL_PROP_FNS(mask_tolh)   MODEL_PROP_FNS(mask_stopfail)
MODEL_PROP_FNS(view_cursel) MODEL_PROP_FNS(view_curt) MODEL_PROP_FNS(view_curr) MODEL_PROP_FNS(view_cury)
MODEL_PROP_FNS(view_scurpos) MODEL_PROP_FNS(view_half)   MODEL_PROP_FNS(view_sh)
int32_t trig_code_get(const prop_desc_t *p);
int32_t trig_held_get(const prop_desc_t *p);
int32_t mask_state_get(const prop_desc_t *p);
int32_t mask_hold_get(const prop_desc_t *p);
int32_t mask_pending_get(const prop_desc_t *p);
extern const char *const lbl_view_cursel[];
extern const char *const lbl_mask_state[];
bool          model_in_batch(void);
prop_status_t model_mask_commit(prop_note_t *note);
bool vdiv_label(const prop_desc_t *p, int32_t v, char *out, size_t n);
bool tb_label(const prop_desc_t *p, int32_t v, char *out, size_t n);
bool meas_label(const prop_desc_t *p, int32_t bit, char *out, size_t n);
bool fft_meas_label(const prop_desc_t *p, int32_t bit, char *out, size_t n);
extern const char *const lbl_view_mode[];
extern const char *const lbl_view_persist[];
extern const char *const lbl_view_math[];
extern const char *const lbl_view_cursor[];

/* props_gen.c */
MODEL_PROP_FNS(gen_wave)    MODEL_PROP_FNS(gen_freq)    MODEL_PROP_FNS(gen_amp)
MODEL_PROP_FNS(gen_duty)    MODEL_PROP_FNS(gen_out)     MODEL_PROP_FNS(gen_bootout)
extern const char *const lbl_gen_wave[];

/* props_meter.c */
MODEL_PROP_FNS(meter_func)  MODEL_PROP_FNS(meter_layout) MODEL_PROP_FNS(meter_fusetype)
MODEL_PROP_FNS(meter_fuserating) MODEL_PROP_FNS(meter_fusethr) MODEL_PROP_FNS(meter_link)
MODEL_PROP_FNS(meter_fuseview) MODEL_PROP_FNS(meter_beep) MODEL_PROP_FNS(meter_hold)
MODEL_PROP_FNS(meter_rel) MODEL_PROP_FNS(meter_degf)
extern const char *const lbl_fuse_view[];
extern const char *const lbl_meter_beep[];
int32_t meter_live_get(const prop_desc_t *p);
bool meter_fuserating_label(const prop_desc_t *p, int32_t v, char *out, size_t n);
extern const char *const lbl_meter_func[];
extern const char *const lbl_meter_layout[];
extern const char *const lbl_meter_link[];
extern const char *const lbl_fuse_type[];

/* props_sys.c */
MODEL_PROP_FNS(sys_instrument) MODEL_PROP_FNS(sys_mode) MODEL_PROP_FNS(sys_startup)
MODEL_PROP_FNS(sys_bright)  MODEL_PROP_FNS(sys_volume)  MODEL_PROP_FNS(sys_autooff)
MODEL_PROP_FNS(sys_theme)   MODEL_PROP_FNS(sys_lang)    MODEL_PROP_FNS(fft_window)
MODEL_PROP_FNS(fft_avg)     MODEL_PROP_FNS(fft_maxhold)
int32_t sys_lastscreen_get(const prop_desc_t *p);
int32_t ui_focus_get(const prop_desc_t *p);
int32_t ui_ach_get(const prop_desc_t *p);
int32_t ui_screen_get(const prop_desc_t *p);
bool    ui_focus_label(const prop_desc_t *p, int32_t v, char *out, size_t n);
extern const char *const lbl_sys_instrument[];
extern const char *const lbl_sys_mode[];
extern const char *const lbl_sys_startup[];
extern const char *const lbl_sys_lastscreen[];
extern const char *const lbl_sys_autooff[];
extern const char *const lbl_sys_theme[];
extern const char *const lbl_sys_lang[];
extern const char *const lbl_fft_window[];
extern const char *const lbl_fft_avg[];

/* Shared by the setters (model.c). */
void model_note(prop_note_t *note, const char *text);
#endif /* MODEL_INTERNAL */

#endif /* MODEL_H */
