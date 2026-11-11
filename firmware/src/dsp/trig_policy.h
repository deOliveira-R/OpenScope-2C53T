/*
 * Trigger acquisition policy and trigger status (parity C3).
 *
 * Pure functions, no FreeRTOS and no fpga.h, so tests/test_trig_policy.c runs
 * the very code the acquisition task (fpga.c) and the badge (scope_ui.c) run.
 *
 * WHAT A READ IS. The FPGA writes a rolling 1024-sample buffer per channel
 * and, on a crossing of the reg-0x08 level, finishes the record and HOLDS it
 * until a read takes it; that read pulses PC0 (EXP-53/54). So a read is one of
 *   - a handed-over record (PC0 strobed): one moment, both channels, the
 *     trigger at index 512 -- coherent;
 *   - a roll read (no strobe): whatever the FPGA is writing. op04 (CH1) and
 *     op05 (CH2) are read ~140 us apart while it keeps writing (same-wire test,
 *     2026-11-10: one signal on both inputs, the two roll reads disagree), so
 *     the two channels of a roll read are two different moments (upstream
 *     issue #84).
 *
 * THE CHOICE (#84). Stock reads only strobe-held records in AUTO (PC0 gate,
 * ADDENDUM_A S3) and draws its fallback anchor when the search misses. We keep
 * OpenScope's AUTO free-run fallback, because a quiet or off-level input must
 * still move the trace, but:
 *   1. while edges occur, AUTO shows ONLY handed-over records: a roll read is
 *      held until no handover has come for a whole budget (unchanged since
 *      EXP-72 -- the budget runs from the last handover);
 *   2. a roll read is never merged into a 250 MS/s record (it would be two
 *      moments interleaved), it is held instead (2026-11-10);
 *   3. when AUTO does fall back to a roll read with BOTH channels on, the
 *      record is published as TRIG_ACQ_FREERUN_SPLIT and the badge says so
 *      ("Auto*", warning colour): the two traces are drawn, but never
 *      presented as one coherent two-channel capture. With one channel on, a
 *      roll read is a single read of a single channel and is shown as plain
 *      free-run ("Auto").
 * NORMAL and SINGLE never show a roll read (unchanged).
 *
 * Mode values are scope_state.h's trigger_mode_t: 0 AUTO, 1 NORMAL, 2 SINGLE.
 * (Stock's ms[0x17] is 0 NORMAL, 1 AUTO, 2 SINGLE -- ADDENDUM_A S3; this
 * module does not use stock's numbering.)
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
    bool    triggered;      /* PC0 strobed this read: a handed-over record */
    bool    first_read;     /* the read that started a capture (nothing to compare yet) */
    bool    budget_up;      /* AUTO: no handover for a whole budget */
    bool    gate_on;        /* `fpga gate on`: AUTO never shows a roll read */
    bool    interleave;     /* the record would be the 250 MS/s two-converter merge */
    bool    two_channels;   /* both channels on */
    bool    single_done;    /* SINGLE already holds its one record */
} trig_acq_in_t;

typedef enum {
    TRIG_ACQ_HOLD = 0,          /* do not publish: keep the trace on screen */
    TRIG_ACQ_TRIGGERED,         /* a handed-over record (the edge filter may still drop it) */
    TRIG_ACQ_FREERUN,           /* AUTO roll read, one channel on: coherent */
    TRIG_ACQ_FREERUN_SPLIT      /* AUTO roll read, both on: CH1 and CH2 are two moments */
} trig_acq_t;

trig_acq_t trig_acq_decide(const trig_acq_in_t *in);

/* A published record whose two channels belong to one moment. */
static inline bool trig_acq_coherent(trig_acq_t a) { return a != TRIG_ACQ_FREERUN_SPLIT; }

/* ── Trigger status (the badge, the shell) ─────────────────────────── */
typedef enum {
    TRIG_ST_STOP = 0,       /* RUN/STOP stopped */
    TRIG_ST_TRIGD,          /* a handed-over record, recent */
    TRIG_ST_WAIT,           /* NORMAL (or AUTO holding): no recent trigger, trace held */
    TRIG_ST_AUTO,           /* AUTO free-run, coherent */
    TRIG_ST_AUTO_SPLIT,     /* AUTO free-run, two channels not simultaneous */
    TRIG_ST_ARMED,          /* SINGLE: "Await Trigger..." (stock 0x0801C8C2) */
    TRIG_ST_SINGLE_DONE,    /* SINGLE: captured and held (stock: STOP, 0x0802163E) */
    TRIG_ST_COUNT
} trig_status_t;

/* last: what the last PUBLISHED record was (TRIG_ACQ_HOLD = none yet).
 * ms_since_trig: since the last handed-over record was published.
 * stale_ms: after this long without one, "Trig'd" is no longer true. */
trig_status_t trig_status_eval(uint8_t mode, bool running, bool single_done,
                               trig_acq_t last, uint32_t ms_since_trig,
                               uint32_t stale_ms);

typedef enum { TRIG_TONE_OK = 0, TRIG_TONE_INFO, TRIG_TONE_WARN } trig_tone_t;

const char *trig_status_label(trig_status_t s);   /* <= 6 chars: the 44-px badge */
const char *trig_status_word(trig_status_t s);    /* one shell word */
trig_tone_t trig_status_tone(trig_status_t s);

#endif /* TRIG_POLICY_H */
