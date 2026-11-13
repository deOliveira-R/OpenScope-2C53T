/*
 * OpenScope 2C53T - the executor: other tasks set properties through the
 * input task (ARCH-1 M1)
 *
 * Every setter runs on the input task (model.h THREADING). A task that wants
 * a set (the shell, later meter_auto) queues a request; vInputTask drains the
 * queue on every pass, after the key it just handled, so a key and a request
 * never interleave and no partial state is visible to the other.
 *
 *   shell task                               input task
 *   ----------                               ----------
 *   prop_request(pairs, n, origin, ms, &r)   xQueueReceive(keys, 100 ms)
 *     validate (INVALID / REFUSED at once)   BTN_NONE = the wake: no key handling
 *     seq = next; queue it (full: BUSY)      model_exec_drain():
 *     wake (BTN_NONE into the key queue)       each request, in order:
 *     poll the result slot for seq             validate all, then prop_set_local
 *       (10 ms steps, <= ms: TIMEOUT)          one by one (check-in between),
 *                                              stop at the first failure;
 *                                              result -> slot[seq], tag last
 *
 * LIFETIMES. A request carries its pairs by value and the result goes to a
 * static slot tagged with the request's sequence number, never through a
 * pointer into the caller's stack: a caller that timed out has returned and
 * its frame is gone. A late answer to a timed-out request lands in its own
 * slot with its own tag and is never read as the next request's answer.
 *
 * WITHDRAWAL. On a timeout the caller withdraws its request. The drain marks
 * a request as started before it looks at the withdrawal, and the caller
 * marks the withdrawal before it looks at the start, so exactly one of two
 * things is true and the caller can say which: the request was withdrawn and
 * will never be applied, or it had already started (TIMEOUT "in progress").
 * A `set` that timed out during a gallery grab therefore does not move a
 * relay when the gallery closes.
 *
 * PORT. FreeRTOS is reached only through model_exec_port_t (model_hw_fw.c
 * binds the real one; tests/test_model_exec.c a host one), so this file is
 * host-tested without an RTOS.
 */
#ifndef MODEL_EXEC_H
#define MODEL_EXEC_H

#include "model.h"

#define MODEL_EXEC_QUEUE_LEN   4u      /* requests in flight; also the result slots */
#define MODEL_EXEC_POLL_MS     10u     /* the caller's poll step                   */
#define MODEL_EXEC_TIMEOUT_MS  600u    /* a request with no PF_HW pair             */
#define MODEL_EXEC_HW_TIMEOUT_MS 1500u /* one with PF_HW pairs (fpga_acq_pause ~1 s) */

/* One request, by value (72 bytes). seq 0 = posted: nobody waits for it. */
typedef struct {
    uint32_t    seq;
    uint8_t     n;
    uint8_t     origin;
    prop_pair_t pairs[MODEL_MAX_PAIRS];
} model_req_t;

/* The answer. status is a prop_status_t. failed = the index of the pair that
 * stopped the request (n when none did); applied = how many pairs were set
 * before it (they stay set: no roll back, model.h). note = the failing pair's
 * note, else the last non-empty one. */
typedef struct {
    uint32_t    seq;
    uint8_t     status;
    uint8_t     failed;
    uint8_t     applied;
    bool        in_progress;    /* TIMEOUT only: the executor had started it */
    prop_note_t note;
} model_result_t;

typedef struct {
    bool     (*send)(const model_req_t *r);   /* non-blocking; false = queue full        */
    bool     (*receive)(model_req_t *r);      /* non-blocking; false = empty             */
    void     (*wake)(void);                   /* nudge the executor (BTN_NONE)           */
    void     (*sleep_ms)(uint32_t ms);        /* caller side, between polls              */
    void     (*checkin)(void);                /* executor: health check-in between pairs */
    void     (*changed)(void);                /* executor: a non-KEY request changed a
                                               * value -> one redraw request             */
    void     (*service)(void);                /* executor: every drain pass (posted
                                               * changes that landed -> model_touch)     */
    bool     (*on_executor)(void);            /* the calling task is the executor        */
    /* Properties whose real setter is not reachable yet: the reason, or NULL. */
    const char *(*unwired)(uint16_t id);
} model_exec_port_t;

/* Executor, before its first drain. Until then prop_request answers BUSY. */
void model_exec_init(const model_exec_port_t *port);
bool model_exec_ready(void);

/* Validate, queue, wake, wait (<= timeout_ms). Returns out->status:
 *   INVALID / REFUSED (validation, the not-wired list: nothing queued),
 *   BUSY (executor not up, or the queue is full: nothing queued),
 *   TIMEOUT (out->in_progress says whether it was withdrawn or had started),
 *   else what the executor said. Caller side only (not the executor). */
prop_status_t prop_request(const prop_pair_t *pairs, uint8_t n, uint8_t origin,
                           uint32_t timeout_ms, model_result_t *out);
/* The timeout prop_request should get for these pairs (600 / 1500 ms). */
uint32_t      prop_request_timeout(const prop_pair_t *pairs, uint8_t n);
/* Fire and forget (meter_auto): queued, applied on the next pass, no answer. */
bool          prop_post(uint16_t id, int32_t value, uint8_t origin);

/* Executor: apply every queued request. Returns how many were taken off the
 * queue (withdrawn ones included). */
unsigned      model_exec_drain(void);

/* The not-wired reason for p (NULL = settable through the executor). */
const char   *prop_unwired_reason(const prop_desc_t *p);

/* Counters (the shell's `describe`/`arch`; never asserts). */
typedef struct {
    uint32_t requests;      /* prop_request calls that were queued            */
    uint32_t posts;         /* prop_post calls that were queued               */
    uint32_t applied;       /* requests the drain ran                         */
    uint32_t withdrawn;     /* requests skipped because their caller gave up  */
    uint32_t timeouts;      /* prop_request TIMEOUTs                          */
    uint32_t full;          /* queue full (BUSY)                              */
} model_exec_stats_t;
void model_exec_stats(model_exec_stats_t *out);

/* Hardware applies made off the executor. Every real op (model_hw_fw.c)
 * calls model_exec_check_context() first; nonzero = a setter ran on the
 * wrong task. */
extern volatile uint32_t model_foreign_sets;
void model_exec_check_context(void);

#ifdef MODEL_EXEC_TEST
/* Host tests: start the sequence at s (wrap tests) and forget all state. */
void model_exec_test_reset(uint32_t first_seq);
#endif

/* ── Firmware glue (model_hw_fw.c; not in the host tests) ───────────── */
/* Bind the real ops, create the queue, bind the port; the input task calls
 * it once, after the boot logo (whose "any key skips" would take a wake for
 * a key). display_queue gets the redraw after a shell set. */
void model_fw_start(void *display_queue);

/* Bumped by vInputTask after each handled key (main.c), for the UI route's
 * deterministic replay (M2). The BTN_NONE wake does not count. */
extern volatile uint32_t input_events_done;

#endif /* MODEL_EXEC_H */
