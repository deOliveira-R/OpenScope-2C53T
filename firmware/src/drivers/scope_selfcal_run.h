/*
 * scope_selfcal_run.h — device half of autoset (short AUTO), baseline
 * correction (long AUTO) and 50 % centre (long OK), parity items C9/C10.
 * The decisions are src/ui/scope_selfcal.c (pure, host-tested); this file
 * feeds them fresh records and carries out what they decide.
 *
 * No task of its own: the key task calls scope_selfcal_service() on every
 * pass of its loop (<= 100 ms apart, main.c), and the service never blocks —
 * it waits for settle times and record commits by returning and looking
 * again. So the key task keeps its health check-in and stays able to cancel,
 * and no heap goes to another stack (RAM is within ~3 KB of the ceiling).
 *
 * Requests may come from any task (keys, shell); only the service acts.
 * Nothing here writes flash: corrections live in scope_vpos' RAM table.
 */
#ifndef SCOPE_SELFCAL_RUN_H
#define SCOPE_SELFCAL_RUN_H

#include <stdint.h>
#include <stdbool.h>

#include "FreeRTOS.h"
#include "queue.h"

typedef enum {
    SCR_IDLE = 0,
    SCR_AUTOSET,
    SCR_BASECAL,
} scr_kind_t;

/* Queue a run; false if one is already running or queued. `banks` = bit b
 * for bank b (long AUTO walks banks 0 and 1; bank 2 is the interleaved
 * single-channel regime, see the .c). */
bool scope_selfcal_request_autoset(void);
bool scope_selfcal_request_basecal(uint8_t banks);
bool scope_selfcal_request_center50(void);
void scope_selfcal_request_cancel(void);

/* A run is in progress (or queued): the keys are locked except AUTO/MENU,
 * which cancel. */
bool scope_selfcal_busy(void);

/* Key task only: one non-blocking pass. `dq` is the display queue. */
void scope_selfcal_service(QueueHandle_t dq);

typedef struct {
    scr_kind_t kind;            /* running now, or SCR_IDLE */
    scr_kind_t last_kind;       /* of the last finished run */
    const char *last_result;    /* its outcome, "" before any */
    uint8_t  bank, range;       /* basecal position */
    bool     prechecking;
    uint8_t  n_ok, n_fail;      /* basecal: last or current run */
    const char *last_fail;      /* basecal: why the last failed entry failed */
    uint8_t  last_fail_ch, last_fail_bank, last_fail_range;
    uint32_t records;           /* records consumed by the current/last run */
    uint32_t timeouts;          /* runs ended for want of a fresh record */
    uint32_t torn;              /* reads retried because a commit landed */
    uint16_t corr_count;        /* corrections held in RAM */
} scope_selfcal_status_t;

void scope_selfcal_get_status(scope_selfcal_status_t *out);

#endif /* SCOPE_SELFCAL_RUN_H */
