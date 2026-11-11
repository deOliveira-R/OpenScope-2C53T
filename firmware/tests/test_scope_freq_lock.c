/*
 * Host test for the scope_freq lock hooks (robust item b): the shell task and
 * the display task share fft_re/fft_im, so every estimate that touches the
 * scratch must run inside lock/unlock, balanced on every return path, and
 * must not take the lock for rejected arguments (no scratch use).
 */
#include "../src/ui/scope_freq.h"
#include <stdio.h>
#include <math.h>
#include <stdint.h>

static int failures, depth, locks, unlocks, max_depth;
static int in_cs_violation;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, m); failures++; } } while (0)

static void lock(void)   { locks++; depth++; if (depth > max_depth) max_depth = depth; }
static void unlock(void) { unlocks++; depth--; if (depth < 0) in_cs_violation = 1; }

static void sine(uint8_t *b, unsigned n, double cycles)
{
    for (unsigned i = 0; i < n; i++)
        b[i] = (uint8_t)(128.0 + 100.0 * sin(6.283185307179586 * cycles * i / n));
}

int main(void)
{
    static uint8_t rec[1024];
    scope_freq_t o;
    sine(rec, 1024, 37.0);

    scope_freq_set_lock(lock, unlock);

    CHECK(scope_freq_estimate(rec, 1024, 1e6f, &o), "clean sine answers");
    CHECK(locks == 1 && unlocks == 1 && depth == 0, "answering path: one balanced lock");

    /* Garbage record: refusal path must still release. */
    for (unsigned i = 0; i < 1024; i++) rec[i] = (uint8_t)((i * 2654435761u) >> 13);
    locks = unlocks = 0;
    (void)scope_freq_estimate(rec, 1024, 1e6f, &o);
    CHECK(locks == 1 && unlocks == 1 && depth == 0, "refusal path: balanced");

    /* Constant record: analyse() fails (the !got early return). */
    for (unsigned i = 0; i < 1024; i++) rec[i] = 128;
    locks = unlocks = 0;
    CHECK(!scope_freq_estimate(rec, 1024, 1e6f, &o), "flat record refused");
    CHECK(locks == unlocks && depth == 0, "flat record: balanced");

    /* Rejected arguments never touch the scratch, so never lock. */
    locks = unlocks = 0;
    CHECK(!scope_freq_estimate(rec, 1000, 1e6f, &o), "bad n");
    CHECK(!scope_freq_estimate(rec, 1024, 0.0f, &o), "bad rate");
    CHECK(!scope_freq_estimate(NULL, 1024, 1e6f, &o), "null samples");
    CHECK(locks == 0 && unlocks == 0, "argument rejection takes no lock");

    CHECK(max_depth == 1 && !in_cs_violation, "never nested, never over-released");

    /* Without hooks it still works (host default). */
    scope_freq_set_lock(NULL, NULL);
    sine(rec, 1024, 37.0);
    locks = 0;
    CHECK(scope_freq_estimate(rec, 1024, 1e6f, &o) && locks == 0, "unlocked path works");

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("scope_freq lock: all checks passed\n");
    return 0;
}
