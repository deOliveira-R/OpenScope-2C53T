/*
 * Host test for the scope / siggen redraw gate (ui/redraw_gate.h).
 *
 * WHAT THIS PROVES, AND WHAT IT DOES NOT
 * --------------------------------------
 * It drives ui/redraw_gate.h, the exact header main.c compiles, over
 * scenario traces and counts what each tick sends the LCD: nothing, a live
 * frame or a full repaint. The queries are filled the way main.c's
 * scope_view_tick fills them, with the cadences mirrored below.
 *
 * It does NOT prove anything about pixels on the LCD, timing on the
 * device, or that main.c hashes the right fields into the epoch. The gate
 * is exercised; the wiring is not.
 *
 * The negative half matters as much as the positive half. Every check is
 * run again against deliberately broken gates (see `mutants[]`), and the
 * test FAILS if a broken gate passes them all: an instrument that cannot
 * fail reports confidently on state it cannot observe.
 *
 * Build/run:  make -C firmware test-redraw-gate
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "redraw_gate.h"

/* ═══════════════════════════════════════════════════════════════════
 * Test harness
 * ═══════════════════════════════════════════════════════════════════ */

static int checks_run = 0;
static int checks_failed = 0;
static bool expect_failures = false;   /* mutant pass: failures are the point */
static int failures_in_pass = 0;

#define CHECK(cond, fmt, ...)                                                \
    do {                                                                     \
        checks_run++;                                                        \
        if (!(cond)) {                                                       \
            failures_in_pass++;                                              \
            if (!expect_failures) {                                          \
                checks_failed++;                                             \
                printf("  FAIL: " fmt "\n", ##__VA_ARGS__);                  \
            }                                                                \
        }                                                                    \
    } while (0)

/* ═══════════════════════════════════════════════════════════════════
 * Firmware constants, mirrored from main.c so the numbers below are the
 * numbers the device runs.
 * ═══════════════════════════════════════════════════════════════════ */

#define FW_TICKS_PER_S  20   /* display task wakes every 50 ms */
#define FW_ANIM_FAST    4    /* SCOPE_ANIM_FAST_FRAMES: X-Y decay, fading persistence */
#define FW_ANIM_TICK    20   /* SCOPE_ANIM_TICK_FRAMES: demo picture before the first record */
#define FW_IDLE_TICK    20   /* SCOPE_IDLE_FRAMES: heartbeat while running */
#define FW_IDLE_STOPPED 0    /* main.c's idle cadence while stopped: no heartbeat */

#define SECONDS(s)      ((uint32_t)(s) * FW_TICKS_PER_S)

/* ═══════════════════════════════════════════════════════════════════
 * Gate under test: the real one, or a deliberately broken variant
 * ═══════════════════════════════════════════════════════════════════ */

typedef enum {
    GATE_REAL,
    GATE_MUT_ALWAYS,       /* no gating at all: a full repaint every tick */
    GATE_MUT_NEVER_IDLE,   /* draws only when the epoch moves */
    GATE_MUT_EPOCH_LIVE,   /* a state change gets a live frame, not a full repaint */
    GATE_MUT_DATA_FULL,    /* new data gets a full repaint, not a live frame */
    GATE_MUT_TIMER_FULL,   /* the animation and heartbeat ticks repaint in full */
} gate_kind_t;

static gate_kind_t gate_under_test = GATE_REAL;

static redraw_action_t gate_step(redraw_gate_t *g, const redraw_query_t *q)
{
    const bool was_primed = g->primed;
    const bool data       = q->new_data;

    switch (gate_under_test) {
    case GATE_MUT_ALWAYS:
        redraw_gate_mark(g, q->epoch, q->frame, REDRAW_FULL);
        return REDRAW_FULL;

    case GATE_MUT_NEVER_IDLE:
        /* Over-eager gating: refuses to draw unless the epoch moved, so
         * live capture never reaches the screen. */
        if (q->epoch == g->drawn_epoch && g->primed) return REDRAW_SKIP;
        redraw_gate_mark(g, q->epoch, q->frame, REDRAW_FULL);
        return REDRAW_FULL;

    case GATE_MUT_EPOCH_LIVE: {
        redraw_action_t a = redraw_gate_step(g, q);
        if (a == REDRAW_FULL && was_primed && !q->force) a = REDRAW_INCREMENTAL;
        return a;
    }
    case GATE_MUT_DATA_FULL: {
        redraw_action_t a = redraw_gate_step(g, q);
        if (a == REDRAW_INCREMENTAL && data) a = REDRAW_FULL;
        return a;
    }
    case GATE_MUT_TIMER_FULL: {
        redraw_action_t a = redraw_gate_step(g, q);
        if (a == REDRAW_INCREMENTAL && !data) a = REDRAW_FULL;
        return a;
    }
    case GATE_REAL:
    default:
        return redraw_gate_step(g, q);
    }
}

static const struct { const char *name; gate_kind_t kind; } mutants[] = {
    { "no gating at all",              GATE_MUT_ALWAYS },
    { "draws only on state change",    GATE_MUT_NEVER_IDLE },
    { "state change as a live frame",  GATE_MUT_EPOCH_LIVE },
    { "new data as a full repaint",    GATE_MUT_DATA_FULL },
    { "timed ticks as full repaints",  GATE_MUT_TIMER_FULL },
};

/* ═══════════════════════════════════════════════════════════════════
 * Scenario driver
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t ticks;          /* display-task ticks to simulate */
    uint32_t data_period;    /* new capture every N ticks; 0 = no data at all */
    uint32_t epoch_period;   /* bump the epoch every N ticks; 0 = never */
    bool     animating;
    uint16_t anim_tick_frames;
    uint16_t idle_tick_frames;
} scenario_t;

typedef struct {
    uint32_t full;
    uint32_t incremental;
    uint32_t skipped;
    uint32_t max_data_latency;   /* worst gap, in ticks, from a capture
                                    landing to the draw that rendered it */
    uint32_t dropped_data;       /* captures never rendered by end of run */
} tally_t;

/* Quiet ticks after the trace (no new captures, not counted in the
 * tally), so a capture landing on the final tick can still be rendered
 * by a gate that draws it late, and is reported as latency, not a drop. */
#define DRAIN_TICKS  (FW_IDLE_TICK + 1)

static tally_t run(const scenario_t *s)
{
    redraw_gate_t g = { 0 };
    tally_t t = { 0, 0, 0, 0, 0 };

    uint32_t epoch = 0x1234;
    bool have_pending = false;
    uint32_t pending_since = 0;

    for (uint32_t f = 0; f < s->ticks + DRAIN_TICKS; f++) {
        const bool counting = (f < s->ticks);
        bool new_data = counting &&
                        (s->data_period && f && (f % s->data_period) == 0);
        if (counting && s->epoch_period && f && (f % s->epoch_period) == 0)
            epoch++;

        if (new_data && !have_pending) { have_pending = true; pending_since = f; }

        const redraw_query_t q = {
            .frame            = f,
            .epoch            = epoch,
            .new_data         = new_data,
            .force            = false,
            .animating        = s->animating,
            .anim_tick_frames = s->anim_tick_frames,
            .idle_tick_frames = s->idle_tick_frames,
        };

        redraw_action_t a = gate_step(&g, &q);

        if (a == REDRAW_SKIP) {
            if (counting) t.skipped++;
        } else {
            if (counting) { if (a == REDRAW_FULL) t.full++; else t.incremental++; }
            if (have_pending) {
                uint32_t lat = f - pending_since;
                if (lat > t.max_data_latency) t.max_data_latency = lat;
                have_pending = false;
            }
        }
    }
    if (have_pending) t.dropped_data++;
    return t;
}

/* ═══════════════════════════════════════════════════════════════════
 * The checks
 * ═══════════════════════════════════════════════════════════════════ */

static void check_idle_settled_screen(bool verbose)
{
    /* 20 s on a configured, running scope with a trace already shown,
     * nothing connected changing, no user input, no new samples. The
     * screen entry is the one full repaint; after that only the heartbeat
     * draws, as a live frame, so a frozen display can be told from a hung
     * task. */
    const scenario_t s = { SECONDS(20), 0, 0, false, 0, FW_IDLE_TICK };
    const uint32_t heartbeats = (s.ticks - 1) / FW_IDLE_TICK;

    tally_t t = run(&s);

    if (verbose)
        printf("  idle settled screen, running (20 s, no data, no input):\n"
               "      %u full, %u live frames, %u ticks skipped\n",
               t.full, t.incremental, t.skipped);

    CHECK(t.full == 1, "only the screen entry may repaint in full, got %u", t.full);
    CHECK(t.incremental == heartbeats,
          "heartbeat every %u ticks should give %u live frames, got %u",
          (unsigned)FW_IDLE_TICK, heartbeats, t.incremental);
    CHECK(t.skipped == s.ticks - 1 - heartbeats,
          "every other tick must be skipped, %u skipped", t.skipped);
}

static void check_stopped_screen_goes_silent(bool verbose)
{
    /* Stopped: main.c turns the heartbeat off, nothing arrives, nothing
     * animates. After the entry paint the LCD is left alone. */
    const scenario_t s = { SECONDS(20), 0, 0, false, 0, FW_IDLE_STOPPED };

    tally_t t = run(&s);

    if (verbose)
        printf("  idle settled screen, stopped (20 s): %u full, %u live frames\n",
               t.full, t.incremental);

    CHECK(t.full == 1, "stopped screen: only the entry paint, got %u full", t.full);
    CHECK(t.incremental == 0, "stopped screen must not draw, got %u live frames",
          t.incremental);
}

static void check_live_trace_every_capture(bool verbose)
{
    /* Live frames are never rate-limited: a trace updates once per
     * capture, on the tick the capture lands, at 10 Hz and at 20 Hz
     * (a capture on every tick). */
    static const uint32_t periods[] = { 2, 1 };

    for (unsigned i = 0; i < sizeof periods / sizeof periods[0]; i++) {
        const scenario_t s = { SECONDS(10), periods[i], 0, false, 0, FW_IDLE_TICK };
        const uint32_t captures = (s.ticks - 1) / periods[i];

        tally_t t = run(&s);

        if (verbose)
            printf("  live trace, capture every %u tick(s) (10 s): %u live frames,"
                   " %u full, worst latency %u tick(s)\n",
                   periods[i], t.incremental, t.full, t.max_data_latency);

        CHECK(t.incremental == captures, "every capture should draw a live frame,"
              " got %u for %u", t.incremental, captures);
        CHECK(t.max_data_latency == 0, "live trace lagged %u ticks", t.max_data_latency);
        CHECK(t.dropped_data == 0, "a capture was never rendered");
        /* The one full repaint is the first paint on screen entry. */
        CHECK(t.full == 1, "live data took the full repaint %u times", t.full);
    }
}

static void check_data_on_a_state_change_tick(bool verbose)
{
    /* A capture every tick and a setting changed every 10 ticks. A tick
     * spent on a full repaint renders the latched capture with it: no
     * capture waits or is lost, and each state change costs exactly one
     * full repaint. */
    const uint32_t change_period = 10;
    const scenario_t s = { SECONDS(10), 1, change_period, false, 0, FW_IDLE_TICK };
    const uint32_t changes = (s.ticks - 1) / change_period;

    tally_t t = run(&s);

    if (verbose)
        printf("  20 Hz capture + a state change every 0.5 s (10 s): %u full,"
               " %u live frames, worst latency %u\n",
               t.full, t.incremental, t.max_data_latency);

    CHECK(t.full == 1 + changes, "expected the entry + %u full repaints, got %u",
          changes, t.full);
    CHECK(t.incremental == s.ticks - 1 - changes,
          "every other tick should be a live frame, got %u", t.incremental);
    CHECK(t.max_data_latency == 0, "a capture waited %u ticks", t.max_data_latency);
    CHECK(t.dropped_data == 0, "a capture was never rendered");
}

static void check_demo_animation(bool verbose)
{
    /* Before the first real record the screen is a function of `frame`
     * (the synthetic demo waveform). Stopped, so no heartbeat masks the
     * animation arm: it alone must keep the picture moving, as live
     * frames on SCOPE_ANIM_TICK_FRAMES. */
    const scenario_t s = { SECONDS(20), 0, 0, true, FW_ANIM_TICK, FW_IDLE_STOPPED };
    const uint32_t steps = (s.ticks - 1) / FW_ANIM_TICK;

    tally_t t = run(&s);

    if (verbose)
        printf("  demo animation before the first record (20 s): %u full,"
               " %u live frames\n", t.full, t.incremental);

    CHECK(t.full == 1, "the demo must not repaint in full, got %u", t.full);
    CHECK(t.incremental == steps, "demo should step every %u ticks (%u), got %u",
          (unsigned)FW_ANIM_TICK, steps, t.incremental);
}

static void check_fading_persistence(bool verbose)
{
    /* Persistence (or X-Y decay) while stopped: no new data, but the
     * ghosts fade on wall time. The fade ticks are live frames at the fast
     * cadence, never a full repaint. */
    redraw_gate_t g = { 0 };
    redraw_query_t q = { .frame = 0, .epoch = 0xFADE, .animating = true,
                         .anim_tick_frames = FW_ANIM_FAST,
                         .idle_tick_frames = FW_IDLE_STOPPED };
    const uint32_t ticks = SECONDS(5);

    gate_step(&g, &q);                          /* entry: full */
    uint32_t live = 0, full = 0;
    for (uint32_t f = 1; f <= ticks; f++) {
        q.frame = f;
        redraw_action_t a = gate_step(&g, &q);
        if (a == REDRAW_INCREMENTAL) live++;
        if (a == REDRAW_FULL)        full++;
    }
    if (verbose)
        printf("  fading persistence, stopped (5 s): %u live, %u full\n", live, full);
    CHECK(full == 0, "a fade must not repaint in full, got %u", full);
    CHECK(live == ticks / FW_ANIM_FAST, "the fade must tick every %u frames"
          " (%u), got %u live", (unsigned)FW_ANIM_FAST,
          (unsigned)(ticks / FW_ANIM_FAST), live);
}

static void check_state_change_always_draws(bool verbose)
{
    /* A user-visible change must repaint in full on the very next tick,
     * with no rate limiting, or button presses feel dead; and once only:
     * the ticks after it are skipped until the heartbeat is due. */
    redraw_gate_t g = { 0 };
    redraw_query_t q = { .frame = 0, .epoch = 0xAAAA,
                         .anim_tick_frames = FW_ANIM_TICK,
                         .idle_tick_frames = FW_IDLE_TICK };
    const uint32_t ticks = 39;

    (void)gate_step(&g, &q);                    /* prime */

    uint32_t full = 0;
    for (uint32_t f = 1; f <= ticks; f++) {
        q.frame = f;
        q.epoch = 0xAAAA + f;                   /* every tick changes something */
        if (gate_step(&g, &q) == REDRAW_FULL) full++;
    }
    if (verbose) printf("  state changed every tick: %u/%u full repaints\n", full, ticks);
    CHECK(full == ticks, "state change must always repaint in full, got %u/%u",
          full, ticks);

    /* One change, then a quiet screen until the heartbeat. */
    q.frame++;
    q.epoch++;
    const uint32_t changed_at = q.frame;
    CHECK(gate_step(&g, &q) == REDRAW_FULL, "a single state change must repaint in full");
    uint32_t after = 0;
    for (q.frame = changed_at + 1; q.frame < changed_at + FW_IDLE_TICK; q.frame++)
        if (gate_step(&g, &q) != REDRAW_SKIP) after++;
    if (verbose) printf("  one state change: %u draws after its full repaint\n", after);
    CHECK(after == 0, "a state change must repaint once, %u more draws", after);
}

static void check_popup_runs_at_tick_rate(bool verbose)
{
    /* main.c marks a popup as new data on every tick while it is up (its
     * countdown steps once per draw), so it is a live frame every tick
     * and never a full repaint. */
    redraw_gate_t g = { 0 };
    redraw_query_t q = { .frame = 0, .epoch = 0xBBBB, .new_data = true,
                         .anim_tick_frames = FW_ANIM_TICK,
                         .idle_tick_frames = FW_IDLE_TICK };
    const uint32_t ticks = 10;

    gate_step(&g, &q);                          /* prime, as screen entry does */

    uint32_t live = 0, full = 0;
    for (uint32_t f = 1; f <= ticks; f++) {
        q.frame = f;
        redraw_action_t a = gate_step(&g, &q);
        if (a == REDRAW_INCREMENTAL) live++;
        if (a == REDRAW_FULL)        full++;
    }

    if (verbose)
        printf("  popup up: %u live frames, %u full\n", live, full);
    CHECK(live == ticks, "a popup must draw a live frame every tick, got %u/%u",
          live, ticks);
    CHECK(full == 0, "a popup must not repaint in full, got %u", full);
}

static void check_screen_entry_always_draws(bool verbose)
{
    /* Switching away and back must repaint even though the screen's own
     * state never moved, otherwise the previous mode's pixels stay.
     * main.c calls redraw_gate_invalidate() on mode change. */
    redraw_gate_t g = { 0 };
    redraw_query_t q = { .frame = 0, .epoch = 0xC0DE };

    CHECK(gate_step(&g, &q) == REDRAW_FULL, "first paint must repaint in full");
    q.frame = 1;
    CHECK(gate_step(&g, &q) == REDRAW_SKIP, "unchanged screen must skip");

    q.force = true;                             /* an explicit invalidate by the caller */
    q.frame = 2;
    CHECK(gate_step(&g, &q) == REDRAW_FULL, "force must repaint in full");
    q.force = false;
    q.frame = 3;
    CHECK(gate_step(&g, &q) == REDRAW_SKIP, "after the forced repaint the unchanged screen must skip");

    redraw_gate_invalidate(&g);                 /* mode change */
    q.frame = 4;
    redraw_action_t a = gate_step(&g, &q);
    if (verbose) printf("  screen re-entry after invalidate: %s\n",
                        a == REDRAW_FULL ? "FULL" : "not full");
    CHECK(a == REDRAW_FULL, "screen entry must repaint in full");
}

static void check_siggen_clamped_presses(bool verbose)
{
    /* Siggen is drawn only from queue commands, and every adjustment asks
     * for a redraw, including one that hits a clamp. main.c's DCMD path
     * draws iff not primed or the epoch moved. 20 presses, 10 of them
     * no-ops. */
    redraw_gate_t g = { 0 };
    uint32_t epoch = 0x5165;
    uint32_t drew = 0;
    const uint32_t presses = 20;

    for (uint32_t i = 0; i < presses; i++) {
        if (i % 2 == 0) epoch++;                /* half the presses change state */
        if (!g.primed || epoch != g.drawn_epoch) {
            drew++;
            redraw_gate_mark(&g, epoch, i, REDRAW_FULL);
        }
    }

    if (verbose)
        printf("  siggen %u presses (half change nothing): %u full repaints\n",
               presses, drew);
    CHECK(drew == presses / 2, "expected %u draws for %u real changes, got %u",
          presses / 2, presses / 2, drew);
}

static void check_frame_counter_wrap(bool verbose)
{
    /* frame is a free-running uint32_t; at 20 Hz it wraps after ~6.8
     * years, but the arithmetic must be wrap-safe regardless because a
     * signed/absolute comparison here would stall the display for the
     * whole second half of the range. */
    const uint32_t start = 0xFFFFFFF0u;
    const uint32_t ticks = SECONDS(10);
    redraw_gate_t g = { 0 };
    redraw_query_t q = { .frame = start, .epoch = 0xD00D,
                         .idle_tick_frames = FW_IDLE_TICK };

    (void)gate_step(&g, &q);                    /* prime near the wrap */

    uint32_t live = 0, full = 0;
    for (uint32_t i = 1; i <= ticks; i++) {
        q.frame = start + i;                    /* wraps through zero */
        redraw_action_t a = gate_step(&g, &q);
        if (a == REDRAW_INCREMENTAL) live++;
        if (a == REDRAW_FULL)        full++;
    }

    if (verbose) printf("  across uint32 frame wrap: %u heartbeats in %u ticks\n",
                        live, ticks);
    CHECK(live == ticks / FW_IDLE_TICK, "heartbeat should fire %u times in %u ticks,"
          " got %u", (unsigned)(ticks / FW_IDLE_TICK), ticks, live);
    CHECK(full == 0, "the heartbeat must not repaint in full, got %u", full);
}

static int run_all_checks(bool verbose)
{
    int before = failures_in_pass;
    check_idle_settled_screen(verbose);
    check_stopped_screen_goes_silent(verbose);
    check_live_trace_every_capture(verbose);
    check_data_on_a_state_change_tick(verbose);
    check_demo_animation(verbose);
    check_fading_persistence(verbose);
    check_state_change_always_draws(verbose);
    check_popup_runs_at_tick_rate(verbose);
    check_screen_entry_always_draws(verbose);
    check_siggen_clamped_presses(verbose);
    check_frame_counter_wrap(verbose);
    return failures_in_pass - before;
}

int main(void)
{
    printf("redraw gate — scope/siggen repaint accounting\n");
    printf("================================================================\n");
    printf("\nmeasured behaviour of redraw_gate.h:\n\n");

    gate_under_test = GATE_REAL;
    int real_failures = run_all_checks(true);

    /* Every check again against each broken gate: at least one must fail,
     * or the checks cannot detect what they claim to. */
    printf("\nnegative controls — broken gates must be caught:\n\n");

    expect_failures = true;
    for (unsigned i = 0; i < sizeof mutants / sizeof mutants[0]; i++) {
        gate_under_test = mutants[i].kind;
        const int caught = run_all_checks(false);
        checks_run++;
        if (caught == 0) {
            checks_failed++;
            printf("  FAIL: mutant '%s' was NOT caught\n", mutants[i].name);
        } else {
            printf("  caught mutant: %-30s (%d checks failed)\n",
                   mutants[i].name, caught);
        }
    }
    expect_failures = false;
    gate_under_test = GATE_REAL;

    printf("\n================================================================\n");
    if (checks_failed == 0 && real_failures == 0) {
        printf("PASS — %d checks\n", checks_run);
        return 0;
    }
    printf("FAIL — %d of %d checks failed\n", checks_failed, checks_run);
    return 1;
}
