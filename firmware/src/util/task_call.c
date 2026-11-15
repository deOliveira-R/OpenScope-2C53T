/*
 * OpenScope 2C53T - a call made on another task. See task_call.h.
 */
#include "task_call.h"

bool task_call_serve(task_call_t *c)
{
    const uint32_t want = c->req;
    if (want == c->done) return false;
    c->started = want;                       /* take it up, THEN look at the withdrawal */
    if (c->withdrawn == want) { c->done = want; return false; }
    void (*const fn)(void *) = c->fn;
    void *const arg = c->arg;
    if (fn) fn(arg);
    c->ran = want;
    c->done = want;
    return true;
}

bool task_call_sync(task_call_t *c, const task_call_port_t *port,
                    void (*fn)(void *), void *arg, uint32_t timeout_ms)
{
    c->fn = fn;                              /* before the request number moves */
    c->arg = arg;
    const uint32_t want = ++c->req;
    if (port->wake) port->wake();
    for (uint32_t waited = 0; c->done != want; waited += TASK_CALL_POLL_MS) {
        if (waited >= timeout_ms) {
            c->withdrawn = want;             /* withdraw, THEN look at the start */
            if (c->started != want) return false;        /* never taken up: it will not run */
            /* Taken up: skipped (done at once) or running. `arg` may be on
             * this stack, so wait it out. */
            while (c->done != want) port->sleep_ms(TASK_CALL_POLL_MS);
            break;
        }
        port->sleep_ms(TASK_CALL_POLL_MS);
    }
    return c->ran == want;
}
