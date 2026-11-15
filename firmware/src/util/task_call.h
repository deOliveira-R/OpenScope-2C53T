/*
 * OpenScope 2C53T - a call made on another task (ARCH-1 B8)
 *
 * State that one task owns is changed on that task: the screen stack and
 * every setter on the input task, the LCD on the display task. A shell
 * handler that needs such a step does not take a flag and do it itself; it
 * asks the owner to run one named function and waits for it:
 *
 *   caller (the shell task)                 owner (input / display task)
 *   -----------------------                 ----------------------------
 *   task_call_sync(&box, port, fn, arg, ms) task_call_serve(&box)   every pass
 *     post fn/arg, wake the owner             runs fn(arg) once
 *     wait for it, <= ms
 *
 * One mailbox per owner, ONE caller at a time (the shell task). `arg` may
 * point into the caller's stack: task_call_sync() does not return while fn
 * can still run. On a timeout the caller withdraws the call; the owner marks
 * a call as started before it looks at the withdrawal and the caller marks
 * the withdrawal before it looks at the start, so exactly one of two things
 * is true: the call will never run (false), or it had started, and the
 * caller waits for it to finish (true). The same hand-over the model's
 * executor uses for a `set` (model_exec.h).
 *
 * Pure: no FreeRTOS (the wake and the sleep are injected). Host test:
 * make test-task-call.
 */
#ifndef TASK_CALL_H
#define TASK_CALL_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    void (*fn)(void *);
    void *arg;
    volatile uint32_t req;        /* caller: the call wanted            */
    volatile uint32_t started;    /* owner: the one it took up          */
    volatile uint32_t ran;        /* owner: the one it really ran       */
    volatile uint32_t done;       /* owner: finished (ran or skipped)   */
    volatile uint32_t withdrawn;  /* caller: gave up on this one        */
} task_call_t;

typedef struct {
    void (*wake)(void);                 /* nudge the owner; may be NULL */
    void (*sleep_ms)(uint32_t ms);      /* caller side, between polls   */
} task_call_port_t;

#define TASK_CALL_POLL_MS 10u

/* Owner, every pass: run the call that is waiting, if any. true: one ran. */
bool task_call_serve(task_call_t *c);

/* Caller: have the owner run fn(arg) and wait (<= timeout_ms for it to
 * start). true: it ran, to the end. false: it did not and never will. */
bool task_call_sync(task_call_t *c, const task_call_port_t *port,
                    void (*fn)(void *), void *arg, uint32_t timeout_ms);

#endif /* TASK_CALL_H */
