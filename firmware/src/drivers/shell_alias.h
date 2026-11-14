/*
 * OpenScope 2C53T - the old state commands as aliases of the property model
 * (ARCH-1 M4)
 *
 * `fpga scope vdiv 1 4`, `mode meter 3`, `siggen freq 2000 on` and the other
 * shell commands that set instrument state used to write that state, and
 * its hardware, from the shell task. Each is now a thin wrapper: it parses
 * its OLD syntax, asks the executor for the change (prop_request: the input
 * task applies it, exactly as `set` does, with the same timeout) and prints
 * its OLD text, so bench scripts and transcripts keep working. Nothing here
 * writes state or hardware.
 *
 * What a command printed that is not a property (register read-backs,
 * counters, the multi-line status blocks) is still printed, read only,
 * through shell_alias_env_t: usb_debug.c binds the firmware's getters and
 * its unchanged status printers, tests/test_shell_alias.c binds fixed ones
 * and compares every byte with the text the old handlers printed.
 *
 * Replies that did not exist before (docs/shell_profiles.md lists them):
 *   TIMEOUT: input task busy (...)        the executor did not answer in time
 *   BUSY <prop> (self-cal running)        autoset / baseline correction runs
 *   ...  (stored: <instrument> FE off; applied when it is next entered)
 *                                         after the old reply, when the
 *                                         hardware path was not live
 * A failed apply no longer leaves the state changed.
 */
#ifndef SHELL_ALIAS_H
#define SHELL_ALIAS_H

#include <stdbool.h>
#include <stdint.h>

/* The multi-line status blocks the old handlers end with (usb_debug.c
 * prints them, unchanged). */
typedef enum {
    SA_ST_TRIGGER,          /* print_trigger_status()                              */
    SA_ST_TIMEBASE,         /* boot reconcile + timebase line; arg = after a set   */
    SA_ST_ROLL,             /* the roll status block                               */
    SA_ST_INTERLEAVE,       /* the interleave status block                         */
    SA_ST_SKEW,             /* the interleave skew line                            */
    SA_ST_VPOS,             /* `cal pos`: the cal-page note + the channel; arg = 1 / 2 */
    SA_ST_PRM,              /* `fpga scope prm`: hidden + the two channels' lists  */
    SA_ST_SIGGEN,           /* siggen_print_state()                                */
    SA_ST_MASK,             /* mask_print_status()                                 */
    SA_ST_METER_FUNC_BEGIN, /* `meter func`: note the counters before the request; arg = function */
    SA_ST_METER_FUNC,       /* wait for the switch, then the old one-line report; arg = function */
    SA_ST_COUNT
} shell_alias_status_t;

typedef struct {
    void        (*emit)(const char *text);              /* text as sent, line endings included  */
    void        (*sleep_ms)(uint32_t ms);               /* between polls of a posted change; NULL = none */
    void        (*status)(shell_alias_status_t which, int arg);
    /* Read-only values the one-line replies print. */
    int         (*trig_x_actual)(void);                 /* scope_ui_trig_x_actual                */
    uint8_t     (*trig_anchor)(void);                   /* scope_ui_trig_anchor: 2 hw, 1 soft    */
    bool        (*edge_filter)(void);                   /* fpga_acq_edge_filter_get              */
    uint8_t     (*channel_mask)(void);                  /* fpga.channel_mask                     */
    uint8_t     (*trig_code)(void);                     /* fpga_acq_trig_code_get (reg 0x08)     */
    uint8_t     (*trig_reconcile_code)(void);           /* fpga_trigger_reconcile_code           */
    uint8_t     (*trig_src_code)(void);                 /* fpga_acq_trig_src_code_get (reg 0x06) */
    const char *(*startup_name)(uint8_t startup);       /* startup_mode_name                     */
    const char *(*meter_func_name)(uint8_t func);       /* meter_submode_name                    */
    const char *(*meter_layout_name)(uint8_t layout);   /* the shell's meter_layout_name         */
    /* input_settings_flush_sync: false = the input task did not answer;
     * else *outcome = settings_flush_outcome_text(). */
    bool        (*settings_flush)(uint32_t timeout_ms, const char **outcome);
    /* `mode meter`: note the meter's switch counters before the request, and
     * wait (bounded) for the posted function change after it, so that the
     * reply still means "done" (the old handler switched inline). */
    void        (*meter_switch_begin)(void);
    void        (*meter_switch_wait)(void);
    /* `fpga roll code <hex>|stock`: the entry byte override, a debug knob of
     * fpga.c that is no property (-1 = stock's byte). */
    void        (*roll_code_override)(int code);
} shell_alias_env_t;

/* One per converted command: args is what follows the command name. */
void shell_alias_scope_vdiv(const char *args, const shell_alias_env_t *e);
void shell_alias_scope_ch(const char *args, const shell_alias_env_t *e);
void shell_alias_scope_probe(const char *args, const shell_alias_env_t *e);
void shell_alias_scope_timebase(const char *args, const shell_alias_env_t *e);
void shell_alias_scope_hpos(const char *args, const shell_alias_env_t *e);
void shell_alias_scope_edge(const char *args, const shell_alias_env_t *e);
void shell_alias_scope_trigmode(const char *args, const shell_alias_env_t *e);
void shell_alias_scope_source(const char *args, const shell_alias_env_t *e);
void shell_alias_scope_single(const char *args, const shell_alias_env_t *e);
void shell_alias_scope_level(const char *args, const shell_alias_env_t *e);
void shell_alias_scope_softtrig(const char *args, const shell_alias_env_t *e);
void shell_alias_scope_graticule(const char *args, const shell_alias_env_t *e);
void shell_alias_scope_prm(const char *args, const shell_alias_env_t *e);
void shell_alias_roll(const char *args, const shell_alias_env_t *e);
void shell_alias_cal_pos(const char *args, const shell_alias_env_t *e);
/* `fpga interleave` without its procedures: [on|off|corr g o|skew [set <ppm>|off]].
 * usb_debug.c keeps `cal`, `tone` and `skew measure` and never passes them. */
void shell_alias_interleave(const char *args, const shell_alias_env_t *e);
void shell_alias_siggen(const char *args, const shell_alias_env_t *e);
/* `mask tol ...` / `mask stop ...`: args is what follows the sub-command. */
void shell_alias_mask_tol(const char *args, const shell_alias_env_t *e);
void shell_alias_mask_stop(const char *args, const shell_alias_env_t *e);
void shell_alias_meter_func(const char *args, const shell_alias_env_t *e);
void shell_alias_meter_degf(const char *args, const shell_alias_env_t *e);
void shell_alias_mode(const char *args, const shell_alias_env_t *e);

#endif /* SHELL_ALIAS_H */
