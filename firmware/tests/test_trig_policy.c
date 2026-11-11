/*
 * trig_policy.c: the acquisition policy (upstream #84) and the trigger status.
 *
 * [1] the whole input space (3 modes x 2^7) against a transcription of the
 *     acq task's if-chain as it stood before parity C3 (fpga.c, 2026-11-10),
 *     with the one intended change: an AUTO roll read with both channels on
 *     is published as SPLIT, not as a plain record.
 * [2] the #84 rules by name, so a failure says which rule broke.
 * [3] the badge/status.
 */
#include <stdio.h>
#include <string.h>
#include "trig_policy.h"

static int fails = 0;
#define CHECK(cond, ...) do { if (cond) { printf("  PASS: "); } else { printf("  FAIL: "); fails++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

/* The pre-C3 loop, transcribed (single_done skipped the read entirely;
 * the gate and interleave holds came first; then the AUTO budget). Returns
 * 0 hold, 1 commit triggered, 2 commit roll. */
static int legacy(const trig_acq_in_t *in)
{
    bool auto_mode = in->mode == TRIG_POL_AUTO;
    if (in->mode == TRIG_POL_SINGLE && in->single_done) return 0;
    if (!in->triggered) {
        if (auto_mode && in->gate_on) return 0;
        if (in->interleave) return 0;
        if (!(auto_mode && (in->budget_up || in->first_read))) return 0;
        return 2;
    }
    return 1;
}

static trig_acq_in_t mk(uint8_t mode, unsigned bits)
{
    trig_acq_in_t in;
    memset(&in, 0, sizeof in);
    in.mode = mode;
    in.triggered    = (bits >> 0) & 1u;
    in.first_read   = (bits >> 1) & 1u;
    in.budget_up    = (bits >> 2) & 1u;
    in.gate_on      = (bits >> 3) & 1u;
    in.interleave   = (bits >> 4) & 1u;
    in.two_channels = (bits >> 5) & 1u;
    in.single_done  = (bits >> 6) & 1u;
    return in;
}

int main(void)
{
    printf("[1] full input space vs the pre-C3 loop\n");
    {
        int bad = 0, split = 0;
        for (uint8_t m = 0; m < 3; m++)
            for (unsigned b = 0; b < 128u; b++) {
                trig_acq_in_t in = mk(m, b);
                trig_acq_t got = trig_acq_decide(&in);
                int want = legacy(&in);
                int g = (got == TRIG_ACQ_HOLD) ? 0 : (got == TRIG_ACQ_TRIGGERED) ? 1 : 2;
                if (g != want) { if (++bad <= 4) printf("    mode %u bits %02X: got %d want %d\n", m, b, g, want); }
                if (got == TRIG_ACQ_FREERUN_SPLIT) {
                    split++;
                    if (!in.two_channels || in.triggered || m != TRIG_POL_AUTO) bad++;
                }
                if (got == TRIG_ACQ_FREERUN && in.two_channels) bad++;
            }
        CHECK(bad == 0, "384 cases: same hold/commit as before, SPLIT exactly on two-channel AUTO roll reads (%d split)", split);
    }

    printf("[2] the #84 rules\n");
    {
        trig_acq_in_t in = mk(TRIG_POL_AUTO, 0);
        in.two_channels = true; in.budget_up = false;
        CHECK(trig_acq_decide(&in) == TRIG_ACQ_HOLD,
              "two-channel AUTO, edges still occurring (budget not up): roll read held");
        in.budget_up = true;
        CHECK(trig_acq_decide(&in) == TRIG_ACQ_FREERUN_SPLIT && !trig_acq_coherent(TRIG_ACQ_FREERUN_SPLIT),
              "two-channel AUTO, free-running: published, marked not coherent");
        in.two_channels = false;
        CHECK(trig_acq_decide(&in) == TRIG_ACQ_FREERUN && trig_acq_coherent(TRIG_ACQ_FREERUN),
              "one-channel AUTO, free-running: a coherent record");
        in.interleave = true;
        CHECK(trig_acq_decide(&in) == TRIG_ACQ_HOLD, "never merge a roll read as 250 MS/s");
        in.interleave = false; in.triggered = true; in.two_channels = true;
        CHECK(trig_acq_decide(&in) == TRIG_ACQ_TRIGGERED && trig_acq_coherent(TRIG_ACQ_TRIGGERED),
              "a handed-over record is coherent with both channels on");
        trig_acq_in_t n = mk(TRIG_POL_NORMAL, 0);
        n.budget_up = true; n.first_read = true;
        CHECK(trig_acq_decide(&n) == TRIG_ACQ_HOLD, "NORMAL never shows a roll read");
        trig_acq_in_t s = mk(TRIG_POL_SINGLE, 0);
        s.triggered = true;
        CHECK(trig_acq_decide(&s) == TRIG_ACQ_TRIGGERED, "SINGLE armed takes the first handed-over record");
        s.single_done = true;
        CHECK(trig_acq_decide(&s) == TRIG_ACQ_HOLD, "SINGLE done holds it, even against a new handover");
    }

    printf("[3] status / badge\n");
    {
        CHECK(trig_status_eval(TRIG_POL_AUTO, false, false, TRIG_ACQ_TRIGGERED, 0, 500) == TRIG_ST_STOP, "stopped wins");
        CHECK(trig_status_eval(TRIG_POL_AUTO, true, false, TRIG_ACQ_TRIGGERED, 100, 500) == TRIG_ST_TRIGD, "AUTO triggered, recent");
        CHECK(trig_status_eval(TRIG_POL_AUTO, true, false, TRIG_ACQ_FREERUN_SPLIT, 9999, 500) == TRIG_ST_AUTO_SPLIT &&
              strcmp(trig_status_label(TRIG_ST_AUTO_SPLIT), "Auto*") == 0 &&
              trig_status_tone(TRIG_ST_AUTO_SPLIT) == TRIG_TONE_WARN,
              "AUTO split free-run: \"Auto*\" in the warning tone");
        CHECK(trig_status_eval(TRIG_POL_AUTO, true, false, TRIG_ACQ_FREERUN, 9999, 500) == TRIG_ST_AUTO, "AUTO coherent free-run");
        CHECK(trig_status_eval(TRIG_POL_AUTO, true, false, TRIG_ACQ_TRIGGERED, 600, 500) == TRIG_ST_WAIT,
              "AUTO holding an old triggered record (interleaved, no edges): Wait, not Trig'd");
        CHECK(trig_status_eval(TRIG_POL_NORMAL, true, false, TRIG_ACQ_TRIGGERED, 100, 500) == TRIG_ST_TRIGD &&
              trig_status_eval(TRIG_POL_NORMAL, true, false, TRIG_ACQ_TRIGGERED, 900, 500) == TRIG_ST_WAIT &&
              trig_status_eval(TRIG_POL_NORMAL, true, false, TRIG_ACQ_HOLD, 0, 500) == TRIG_ST_WAIT,
              "NORMAL: Trig'd only while handovers keep coming (it said Trig'd always before C3)");
        CHECK(trig_status_eval(TRIG_POL_SINGLE, true, false, TRIG_ACQ_TRIGGERED, 0, 500) == TRIG_ST_ARMED &&
              strcmp(trig_status_label(TRIG_ST_ARMED), "Await") == 0,
              "SINGLE armed: Await (stock's \"Await Trigger...\")");
        CHECK(trig_status_eval(TRIG_POL_SINGLE, true, true, TRIG_ACQ_TRIGGERED, 0, 500) == TRIG_ST_SINGLE_DONE &&
              strcmp(trig_status_label(TRIG_ST_SINGLE_DONE), "STOP") == 0,
              "SINGLE captured: STOP, as stock");
        int longest = 0;
        for (int s = 0; s < TRIG_ST_COUNT; s++) {
            int n = (int)strlen(trig_status_label((trig_status_t)s));
            if (n > longest) longest = n;
        }
        CHECK(longest <= 6, "every label fits the 44-px badge (<= 6 chars; longest %d)", longest);
    }

    printf("\n%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
