/*
 * OpenScope 2C53T - the v5 settings record, generated from the property table
 *
 * One const row per field of device_config_t (config.h) says which property
 * it holds, where, how it is encoded and what a load does with a value the
 * record holds out of range. settings_store_capture / settings_store_apply
 * and config_init_defaults are loops over this table: there is no
 * hand-written field list left. The record itself (layout, CONFIG_VERSION 5,
 * the zero-init invariant, the v4 upgrade in config.c) is unchanged, byte for
 * byte; tests/test_model_persist.c proves it against a frozen copy of the
 * code this replaced.
 *
 * LOAD. A load writes the backing state through each property's store hook
 * (model.h STORE: no rules, no settings note, no hardware), with three
 * exceptions that keep today's behaviour:
 *   - the gen.* rows are collected into one siggen_config_t and handed to
 *     siggen_init(), the generator owner's boot entry (its store hooks go
 *     through model_hw(), which is not bound yet when settings_store_init()
 *     runs in main());
 *   - view.math is two record bytes and two variables (math_enabled,
 *     math_op): a saved "off, op 3" must come back as op 3, which the
 *     composite value cannot say, so the codec writes the variables;
 *   - rows with no property (sys_prefs_mark, display_persist_mode) are
 *     record bookkeeping.
 *
 * STORED ONLY (sys.lang, fft.*): a load decodes the byte into the property
 * when it is in range, and a capture writes the byte only when the property
 * no longer equals what the byte decodes to. A byte nothing understands is
 * therefore carried through any number of load/save cycles unchanged, and a
 * value somebody set is saved. display_persist_mode and the padding bytes
 * have no property at all and are carried the same way (settings_store.c
 * captures over its last image).
 *
 * Host test: tests/test_model_persist.c (make test-model-persist).
 */
#ifndef MODEL_PERSIST_V5_H
#define MODEL_PERSIST_V5_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

/* How a property value sits in the record. */
typedef enum {
    V5_U8,          /* uint8_t = the value                                          */
    V5_BOOL8,       /* uint8_t 0/1; a load reads any non-zero byte as 1             */
    V5_I16,         /* int16_t                                                      */
    V5_MASK16,      /* uint16_t; a load drops the bits above `hi`                   */
    V5_CONST,       /* always written as `lo`; a load gives the property `lo`
                     * (channel enables: session state, the record says true)       */
    V5_STARTUP,     /* CONFIG_MODE_* in the record, startup_mode_t in the property  */
    V5_MINUTES,     /* auto-off minutes in the record, an index in the property     */
    V5_F32_INT,     /* float, integer-valued (gen.freq Hz)                          */
    V5_F32_X10,     /* float volts, property in tenths (gen.amp dV)                 */
    V5_GEN_OUT,     /* saved enable: gen.bootout && gen.out; a load needs both      */
    V5_MATH_PAIR,   /* math_enabled + math_op (the next byte) <-> the variables     */
    V5_FFT_COUNT,   /* fft averaging count in the record, an index in the property  */
    V5_CARRY,       /* no property: the bytes are kept as loaded, zero by default   */
} v5_codec_t;

/* What a load does with a value outside lo..hi (record units). */
typedef enum {
    V5_LOAD_EXACT,      /* every raw value maps (bool, mask, const, carry)            */
    V5_LOAD_DEFAULT,    /* out of range -> the property's built-in default           */
    V5_LOAD_SATURATE,   /* out of range -> the nearer limit                          */
    V5_LOAD_BRIGHT,     /* sys_bright_clamp: 0 or > hi -> default, < lo -> lo, else
                         * rounded down to the property's step                       */
} v5_load_t;

enum {
    V5_F_PREFS  = 1u << 0,  /* only with sys_prefs_mark == CONFIG_SYS_PREFS_MARK; an
                             * older record loads the property's default instead      */
    V5_F_STORED = 1u << 1,  /* stored only: capture writes it only when it changed     */
};

typedef struct {
    uint16_t prop_id;       /* 0 = a record field with no property                     */
    uint8_t  offset;        /* offsetof(device_config_t, field)                        */
    uint8_t  size;          /* bytes the row owns (V5_MATH_PAIR: 2)                     */
    uint8_t  codec;         /* v5_codec_t                                              */
    uint8_t  load;          /* v5_load_t                                               */
    uint8_t  flags;         /* V5_F_*                                                  */
    int32_t  lo, hi;        /* the range a load accepts (V5_CONST: lo is the value)     */
} v5_field_t;

extern const v5_field_t model_persist_v5_fields[];
extern const unsigned   model_persist_v5_field_count;

/* The row of a property, or NULL (not persisted in v5). */
const v5_field_t *model_persist_v5_find(uint16_t prop_id);

/* The property value a load gives row f from *cfg (the out-of-range policy
 * and the sys_prefs_mark rule applied). Not for V5_MATH_PAIR / V5_CARRY /
 * rows without a property (returns 0). */
int32_t model_persist_v5_decode(const v5_field_t *f, const device_config_t *cfg);

/* Built-in defaults (prop_default) into every field; the caller has zeroed
 * the struct and stamps the header and checksum (config_init_defaults). */
void model_persist_v5_defaults(device_config_t *cfg);

/* Live state into every field the table owns; carried bytes stay as they are
 * in *cfg. No header, no checksum (settings_store_capture stamps). */
void model_persist_v5_capture(device_config_t *cfg);

/* *cfg into live state through the load hooks. No hardware beyond what
 * siggen_init() does at boot today, no settings note, no epoch bump. */
void model_persist_v5_apply(const device_config_t *cfg);

#endif /* MODEL_PERSIST_V5_H */
