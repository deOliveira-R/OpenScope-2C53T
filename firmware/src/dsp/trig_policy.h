/*
 * Trigger acquisition policy and trigger status.
 *
 * Pure functions, no FreeRTOS and no fpga.h, so tests/test_trig_policy.c runs
 * the very code the acquisition task (fpga.c) and the badge (scope_ui.c) run.
 *
 * WHAT A READ IS. OpenScope's capture core writes both converters into one
 * 1024-sample ring and HOLDS a record when a trigger completes it, or when
 * the firmware asks for one without a trigger (op 09). A read of ops 04/05
 * then is one of
 *   - a triggered record: one moment, both converters, in time order, the
 *     trigger crossing at index 512;
 *   - a forced record: the same, without a crossing -- Auto's picture of a
 *     signal that does not cross the level;
 *   - nothing held: the ring is still being written and the samples read are
 *     not a record. They are never shown.
 * The reply header says which (drivers/fpga.c, dsp/trig_edge_reg.h).
 *
 * THE MODES.
 *   AUTO    shows triggered records; when none has come for the wait
 *           (dsp/acq_timing.h) it asks the core for a forced record before
 *           each read (trig_auto_ask). The capture stays armed between reads,
 *           so a trigger that comes is still shown.
 *   NORMAL  shows triggered records only.
 *   SINGLE  shows one triggered record and holds it until re-armed.
 *
 * Mode values are scope_state.h's trigger_mode_t: 0 AUTO, 1 NORMAL, 2 SINGLE.
 */
#ifndef TRIG_POLICY_H
#define TRIG_POLICY_H

#include <stdint.h>
#include <stdbool.h>

#define TRIG_POL_AUTO    0u
#define TRIG_POL_NORMAL  1u
#define TRIG_POL_SINGLE  2u

typedef struct {
    uint8_t mode;           /* TRIG_POL_* */
    bool    triggered;      /* this read is a record a trigger completed */
    bool    forced;         /* this read is a record held on request, without a trigger */
    bool    single_done;    /* SINGLE already holds its one record */
} trig_acq_in_t;

/* The values go out in the stream's record header (stream_frame.h): fixed. */
typedef enum {
    TRIG_ACQ_HOLD      = 0,     /* do not publish: keep the trace on screen */
    TRIG_ACQ_TRIGGERED = 1,     /* a triggered record */
    TRIG_ACQ_FREERUN   = 2      /* AUTO's forced record: no trigger in it */
} trig_acq_t;

trig_acq_t trig_acq_decide(const trig_acq_in_t *in);

/* AUTO: ask the core for a forced record before this read? Yes once no
 * triggered record has come for wait_ms (and never in the other modes). */
static inline bool trig_auto_ask(uint8_t mode, uint32_t ms_since_trigger, uint32_t wait_ms)
{
    return mode == TRIG_POL_AUTO && ms_since_trigger >= wait_ms;
}

/* Before the first read after the task has not been reading (STOP, a SINGLE
 * record being held, roll) or after SINGLE is (re-)armed: the core may hold
 * a record from before, and its hand-over would pass for a new one. Any
 * register write restarts the core's capture with a fresh pre-trigger part
 * (fpga/core/rtl/cmd.sv), so one write makes every later record newer than
 * the write. Returns whether to make that write. */
static inline bool trig_fresh_restart(bool single_armed, bool was_holding)
{
    return single_armed || was_holding;
}

/* ── Trigger status (the badge, the shell) ─────────────────────────── */
typedef enum {
    TRIG_ST_STOP = 0,       /* RUN/STOP stopped */
    TRIG_ST_TRIGD,          /* a triggered record, recent */
    TRIG_ST_WAIT,           /* no recent trigger, trace held */
    TRIG_ST_AUTO,           /* AUTO showing forced records */
    TRIG_ST_ARMED,          /* SINGLE: waiting for its trigger */
    TRIG_ST_SINGLE_DONE,    /* SINGLE: captured and held */
    TRIG_ST_NOCORE,         /* the FPGA holds no usable capture core: nothing is acquired (core_ident.h) */
    TRIG_ST_COUNT
} trig_status_t;

/* last: what the last PUBLISHED record was (TRIG_ACQ_HOLD = none yet).
 * ms_since_trig: since the last triggered record was published.
 * stale_ms: after this long without one, "Trig'd" is no longer true. */
trig_status_t trig_status_eval(uint8_t mode, bool running, bool single_done,
                               trig_acq_t last, uint32_t ms_since_trig,
                               uint32_t stale_ms);

typedef enum { TRIG_TONE_OK = 0, TRIG_TONE_INFO, TRIG_TONE_WARN } trig_tone_t;

const char *trig_status_label(trig_status_t s);   /* <= 6 chars: the 44-px badge */
const char *trig_status_word(trig_status_t s);    /* one shell word */
trig_tone_t trig_status_tone(trig_status_t s);

#endif /* TRIG_POLICY_H */
