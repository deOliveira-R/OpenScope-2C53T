/*
 * OpenScope 2C53T - the property model (registry core)
 *
 * Every user setting is a named, typed PROPERTY: one const descriptor in
 * flash (model_table.h, one X-macro line per property) with a getter that
 * reads the variable the firmware already has (scope_state_t, the main.c
 * globals, siggen_config_t, sys_prefs, meas_select, ...) and a setter that
 * validates, applies through the owner driver and stores ONLY on success.
 * A property never owns a value: the model adds no shadow copy. The one
 * exception is the stored-only block (sys.lang, fft.*), which nothing in the
 * firmware owns yet (props_sys.c says so).
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
 * session". PF_USERCAL = in the usercal record (explicit `cal user save`).
 * The readable profile is one `name = value [unit]` line per property
 * (prop_line_format / prop_line_parse), validated by the same setters.
 * Built-in defaults (prop_default) are separate from the current value, so
 * "Start with defaults" and "Reset to defaults" are model_load_defaults()
 * with or without a settings note.
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
    PF_STORED_ONLY = 1u << 5,            /* persisted but nothing applies it yet (fft.*, lang)   */
    PF_HIDDEN      = 1u << 6,            /* a build profile hid it from the menus                */
    PF_SESSION     = 1u << 7,            /* session only: restored by "resume last session" only */
    PF_ALIAS       = 1u << 8,            /* another name for state a primary property also shows
                                          * (sys.mode / sys.instrument): never saved or exported */
    PF_POSTED      = 1u << 9,            /* setter posts to another context and returns when
                                          * posted (mode/posture, mask requests): "posted" note   */
};

typedef enum { PS_OK = 0,      /* applied (value may equal the old one)                   */
               PS_CLAMPED,     /* applied, value was clamped to the range                 */
               PS_INVALID,     /* out of range / unknown name / RO: nothing touched       */
               PS_REFUSED,     /* a rule said no ("Keep one CH on", "FFT reads CH1 only") */
               PS_NOT_SET,     /* the owner driver failed: old value kept                 */
               PS_BUSY,        /* self-cal running / acquisition did not park             */
               PS_TIMEOUT,     /* shell only: the executor did not answer                 */
               PS_COUNT
             } prop_status_t;

typedef enum { ORIGIN_KEY, ORIGIN_SHELL, ORIGIN_AUTO, ORIGIN_BOOT, ORIGIN_PROFILE,
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
 * (6) PF_PERSIST + a changed value -> hw note_change, except ORIGIN_BOOT and
 * ORIGIN_SHELL with shell_persist off. note may be NULL. */
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

/* STORE. A store hook writes the backing state with no rules and no
 * settings note. Where the model can reach the variable (scope_state_t,
 * main.c globals, sys_prefs, theme, meas_select, the stored-only block) it
 * moves no hardware: at boot the owners' inits push it, as today. Where an
 * owner keeps the state in its own file (gen.* in signal_gen.c, il.* in
 * fpga.c, mask.* in scope_mask.c) the store IS that owner's setter through
 * model_hw(), and does nothing while the ops are unbound. view.persist's is
 * a no-op: a load never claims the shared pool (settings_store.c's rule).
 * So a store is a boot / start-up tool, not a runtime apply: at run time
 * "Reset to defaults" goes through prop_set_pairs_local (the setters).
 *
 * Built-in defaults through each store hook for the properties whose flags
 * include one of `want` (PF_PERSIST for "Start with defaults", PF_PERSIST |
 * PF_SESSION for a full reset). `in` != NULL: that instrument's members only
 * (system properties, inst == 0, are left alone), with its overrides. Bumps
 * the epochs that changed; note_change once if `note` and a persisted value
 * changed ("Reset to defaults" copies the defaults into the user profile).
 * Returns the number of values that changed. */
unsigned model_load_defaults(uint16_t want, const instrument_desc_t *in, bool note);

/* ── Change notification ─────────────────────────────────────────────── */
extern volatile uint32_t prop_epoch[PG_COUNT];   /* bumped by the executor on a change */
uint32_t prop_group_epoch(uint8_t g);
/* Bump group g's epoch (executor only). prop_set_local bumps the property's
 * own group; a setter calls this for what it changes in another group
 * (ch.enabled moves ui.ach, Single sets view.run, Roll is in tb and view),
 * and an owner that lands a POSTED change later (the mode switch, a mask
 * request) calls it when the change is in force. */
void     model_touch(uint8_t g);

/* ── Hardware ops (bound once at boot; fake in the host tests) ───────── */
/* Each is the owner API the setter calls (model_hw_fw.c binds the real ones
 * in M1). Channels are 0 = CH1, 1 = CH2. bool = the owner did it. Getters
 * here are for state an owner keeps in its own file (fpga.c, scope_mask.c,
 * persistence.c). */
typedef struct {
    /* scope front end and acquisition (fpga.c, scope_vpos.c) */
    bool    (*vdiv)(uint8_t ch, uint8_t idx);          /* fpga_apply_vdiv + scope_vpos_apply_ch */
    bool    (*coupling)(uint8_t ch, uint8_t coupling); /* fpga_apply_coupling (relay readback)  */
    bool    (*channel_enables)(void);                  /* fpga_apply_channel_enables (reads state) */
    bool    (*interleave_active)(void);                /* fpga_interleave_active                */
    bool    (*vpos)(uint8_t ch, int16_t pos);          /* scope_vpos_set; false = marker only   */
    bool    (*timebase)(uint8_t code);                 /* fpga_apply_timebase                   */
    bool    (*roll)(void);                             /* fpga_apply_roll (reads state)         */
    bool    (*trigger_source)(void);                   /* fpga_apply_trigger_source (reads state) */
    bool    (*trigger_level)(int16_t level);           /* fpga_apply_trigger_level              */
    void    (*single_rearm)(void);                     /* fpga_acq_single_rearm                 */
    /* views and the shared pool (scope_apply.c) */
    bool    (*view_select)(uint8_t view);              /* scope_view_select; false = not in build */
    bool    (*persist_on)(uint8_t mode);               /* scope_apply_persist_on                */
    void    (*persist_off)(void);                      /* scope_apply_persist_off               */
    uint8_t (*persist_mode)(void);                     /* persist_get_mode                      */
    /* mask test (scope_mask.c, applied by the acquisition task) */
    bool    (*mask_present)(void);
    void    (*mask_get)(uint8_t *tol_v, uint8_t *tol_h, bool *stop_on_fail);  /* pending or applied */
    bool    (*mask_tol)(uint8_t tol_v, uint8_t tol_h); /* scope_mask_post(REQ_TOL)              */
    bool    (*mask_stopfail)(bool on);                 /* scope_mask_post(REQ_STOPFAIL)         */
    /* generator (signal_gen.c owns its state and TMR7/DMA2/DAC2) */
    void    (*gen_wave)(uint8_t wave);
    void    (*gen_freq)(uint32_t hz);
    void    (*gen_amp)(uint8_t dv);
    void    (*gen_duty)(percent_t duty);
    void    (*gen_out)(bool on);
    void    (*gen_bootout)(bool on);
    /* meter: everything meter_function_set does except the store (cancel
     * autoselect unless keep_autoselect, reset min/max/hold/rel, post the
     * meter posture). false = not posted. */
    bool    (*meter_func)(uint8_t submode, bool keep_autoselect);
    /* system */
    bool    (*mode)(uint8_t device_mode);              /* posts the mode switch (enter_mode);
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

/* Bind the ops (NULL entries are allowed: the call is skipped and the setter
 * treats it as done, or as refused where it needs an answer). */
void                  model_bind_hw(const model_hw_ops_t *ops);
const model_hw_ops_t *model_hw(void);

/* Self-check of the table (names, ids, ranges, labels); 0 = sound, else the
 * number of problems found. Host tests call it; the shell's `arch` may. */
unsigned model_table_check(void (*report)(const char *what, const prop_desc_t *p, void *ctx),
                           void *ctx);

/* ── Internal: what model_table.h names (model.c and props_*.c only) ─── */
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
MODEL_PROP_FNS(mask_tolv)   MODEL_PROP_FNS(mask_tolh)   MODEL_PROP_FNS(mask_stopfail)
bool vdiv_label(const prop_desc_t *p, int32_t v, char *out, size_t n);
bool tb_label(const prop_desc_t *p, int32_t v, char *out, size_t n);
bool meas_label(const prop_desc_t *p, int32_t bit, char *out, size_t n);
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
MODEL_PROP_FNS(meter_fuserating) MODEL_PROP_FNS(meter_fusethr)
bool meter_fuserating_label(const prop_desc_t *p, int32_t v, char *out, size_t n);
extern const char *const lbl_meter_func[];
extern const char *const lbl_meter_layout[];
extern const char *const lbl_fuse_type[];

/* props_sys.c */
MODEL_PROP_FNS(sys_instrument) MODEL_PROP_FNS(sys_mode) MODEL_PROP_FNS(sys_startup)
MODEL_PROP_FNS(sys_bright)  MODEL_PROP_FNS(sys_volume)  MODEL_PROP_FNS(sys_autooff)
MODEL_PROP_FNS(sys_theme)   MODEL_PROP_FNS(sys_lang)    MODEL_PROP_FNS(fft_window)
MODEL_PROP_FNS(fft_avg)     MODEL_PROP_FNS(fft_maxhold)
int32_t ui_focus_get(const prop_desc_t *p);
int32_t ui_ach_get(const prop_desc_t *p);
int32_t ui_screen_get(const prop_desc_t *p);
bool    ui_focus_label(const prop_desc_t *p, int32_t v, char *out, size_t n);
extern const char *const lbl_sys_instrument[];
extern const char *const lbl_sys_mode[];
extern const char *const lbl_sys_startup[];
extern const char *const lbl_sys_autooff[];
extern const char *const lbl_sys_theme[];
extern const char *const lbl_sys_lang[];
extern const char *const lbl_fft_window[];
extern const char *const lbl_fft_avg[];

/* Shared by the setters (model.c). */
void model_note(prop_note_t *note, const char *text);
#endif /* MODEL_INTERNAL */

#endif /* MODEL_H */
