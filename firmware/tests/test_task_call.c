/*
 * Host test for src/util/task_call.c: a call made on another task. The
 * "owner" runs inside the caller's sleep, at the poll the test chooses, so
 * every interleaving of the withdrawal is walked on one thread.
 *
 * Build: make test-task-call
 */
#include <stdio.h>
#include <string.h>
#include "task_call.h"

static int run, passed;
#define CHECK(name, cond) do { run++; if (cond) { passed++; printf("  %-66s[PASS]\n", name); } \
                               else { printf("  %-66s[FAIL]\n", name); } } while (0)

static task_call_t box;
static int g_wakes, g_sleeps, g_ran, g_last;
static int g_serve_at = -1;        /* the sleep on which the owner serves (-1: never) */
static int g_start_only_at = -1;   /* the sleep on which the owner only takes it up   */
static int g_finish_at = -1;       /* ... and the sleep on which it finishes that one */

static void fn_add(void *arg) { g_ran++; g_last = *(int *)arg; }

static void wake(void) { g_wakes++; }
static void sleep_ms(uint32_t ms)
{
    (void)ms;
    if (g_sleeps == g_serve_at) (void)task_call_serve(&box);
    if (g_sleeps == g_start_only_at) box.started = box.req;      /* the owner is inside fn */
    if (g_sleeps == g_finish_at) { fn_add(box.arg); box.ran = box.req; box.done = box.req; }
    g_sleeps++;
}
static const task_call_port_t PORT = { wake, sleep_ms };

static void reset(void)
{
    memset(&box, 0, sizeof box);
    g_wakes = g_sleeps = g_ran = g_last = 0;
    g_serve_at = g_start_only_at = g_finish_at = -1;
}

int main(void)
{
    printf("task call\n");
    int v = 7;

    reset();
    CHECK("serve with nothing waiting does nothing", !task_call_serve(&box) && g_ran == 0);

    reset(); g_serve_at = 2;
    CHECK("a call the owner serves runs once, with its argument",
          task_call_sync(&box, &PORT, fn_add, &v, 100) && g_ran == 1 && g_last == 7 && g_wakes == 1);
    CHECK("... and is not run again on the next pass", !task_call_serve(&box) && g_ran == 1);

    reset();
    CHECK("no owner: false after the timeout, nothing ran",
          !task_call_sync(&box, &PORT, fn_add, &v, 50) && g_ran == 0 && g_sleeps == 5);
    CHECK("... and the withdrawn call never runs later", !task_call_serve(&box) && g_ran == 0);
    v = 9; g_serve_at = g_sleeps + 1;
    CHECK("the next call after a withdrawn one runs",
          task_call_sync(&box, &PORT, fn_add, &v, 100) && g_ran == 1 && g_last == 9);

    /* The owner took the call up just before the timeout and is still inside
     * fn: the caller must wait for it (arg is on its stack). */
    reset(); v = 3; g_start_only_at = 4; g_finish_at = 9;
    CHECK("started before the timeout: the caller waits for the end",
          task_call_sync(&box, &PORT, fn_add, &v, 50) && g_ran == 1 && g_last == 3 && g_sleeps == 10);

    reset(); g_serve_at = 0;
    for (int i = 0; i < 5; i++) { v = i; g_serve_at = g_sleeps; (void)task_call_sync(&box, &PORT, fn_add, &v, 100); }
    CHECK("five calls in a row: five runs, the last argument last", g_ran == 5 && g_last == 4);

    reset(); box.req = box.done = box.started = box.ran = box.withdrawn = 0xFFFFFFFFu; g_serve_at = 1;
    CHECK("the request number wraps", task_call_sync(&box, &PORT, fn_add, &v, 100) && g_ran == 1 && box.req == 0u);

    reset(); g_serve_at = 1;
    CHECK("a NULL function is a served no-op", task_call_sync(&box, &PORT, NULL, NULL, 100) && g_ran == 0);

    printf("\n%d/%d passed\n", passed, run);
    return passed == run ? 0 : 1;
}
