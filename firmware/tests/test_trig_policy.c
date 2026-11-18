/*
 * trig_policy.c: what a read of the capture core is worth and what the badge says.
 * The whole input space is small enough to state outright.
 */
#include <stdio.h>
#include <string.h>
#include "trig_policy.h"

static int fails = 0;
#define CHECK(cond, ...) do { if (cond) { printf("  PASS: "); } else { printf("  FAIL: "); fails++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static trig_acq_t d(uint8_t mode, bool triggered, bool forced, bool single_done)
{
    const trig_acq_in_t in = { .mode = mode, .triggered = triggered, .forced = forced, .single_done = single_done };
    return trig_acq_decide(&in);
}

int main(void)
{
    printf("[1] what a read is worth\n");
    {
        CHECK(trig_acq_decide(0) == TRIG_ACQ_HOLD, "no input: hold");
        for (uint8_t m = TRIG_POL_AUTO; m <= TRIG_POL_SINGLE; m++) {
            CHECK(d(m, true, false, false) == TRIG_ACQ_TRIGGERED, "mode %u: a triggered record is shown", (unsigned)m);
            CHECK(d(m, false, false, false) == TRIG_ACQ_HOLD, "mode %u: nothing held is never shown", (unsigned)m);
        }
        CHECK(d(TRIG_POL_AUTO, false, true, false) == TRIG_ACQ_FREERUN, "AUTO: a forced record is its untriggered picture");
        CHECK(d(TRIG_POL_NORMAL, false, true, false) == TRIG_ACQ_HOLD, "NORMAL: a forced record left over from AUTO is not shown");
        CHECK(d(TRIG_POL_SINGLE, false, true, false) == TRIG_ACQ_HOLD, "SINGLE: nor there");
        CHECK(d(TRIG_POL_SINGLE, true, false, true) == TRIG_ACQ_HOLD && d(TRIG_POL_SINGLE, false, true, true) == TRIG_ACQ_HOLD,
              "SINGLE with its record: everything is held until re-armed");
        CHECK(d(TRIG_POL_AUTO, true, false, true) == TRIG_ACQ_TRIGGERED, "single_done means nothing outside SINGLE");
        CHECK(TRIG_ACQ_HOLD == 0 && TRIG_ACQ_TRIGGERED == 1 && TRIG_ACQ_FREERUN == 2, "the kinds' values are the stream's: fixed");
    }

    printf("[2] AUTO: when to ask the core for a forced record\n");
    {
        CHECK(!trig_auto_ask(TRIG_POL_AUTO, 229u, 230u), "a trigger came within the wait: do not ask");
        CHECK(trig_auto_ask(TRIG_POL_AUTO, 230u, 230u) && trig_auto_ask(TRIG_POL_AUTO, 100000u, 230u), "none for the whole wait: ask");
        CHECK(!trig_auto_ask(TRIG_POL_NORMAL, 100000u, 230u) && !trig_auto_ask(TRIG_POL_SINGLE, 100000u, 230u),
              "NORMAL and SINGLE never ask");
    }

    printf("[3] the first read after a hold or a SINGLE arm\n");
    {
        CHECK(!trig_fresh_restart(false, false), "steady reading: nothing");
        CHECK(trig_fresh_restart(true, false), "SINGLE armed: restart the capture");
        CHECK(trig_fresh_restart(false, true), "RUN after STOP: restart the capture");
        CHECK(trig_fresh_restart(true, true), "armed while stopped: restart");
    }

    printf("[4] the badge\n");
    {
        CHECK(trig_status_eval(TRIG_POL_AUTO, false, false, TRIG_ACQ_TRIGGERED, 0, 500) == TRIG_ST_STOP, "stopped: STOP, whatever came last");
        CHECK(trig_status_eval(TRIG_POL_AUTO, true, false, TRIG_ACQ_TRIGGERED, 100, 500) == TRIG_ST_TRIGD, "AUTO, a recent trigger: Trig'd");
        CHECK(trig_status_eval(TRIG_POL_AUTO, true, false, TRIG_ACQ_FREERUN, 100000, 500) == TRIG_ST_AUTO, "AUTO showing forced records: Auto");
        CHECK(trig_status_eval(TRIG_POL_AUTO, true, false, TRIG_ACQ_HOLD, 0, 500) == TRIG_ST_WAIT, "AUTO, nothing published yet: Wait");
        CHECK(trig_status_eval(TRIG_POL_NORMAL, true, false, TRIG_ACQ_TRIGGERED, 100, 500) == TRIG_ST_TRIGD &&
              trig_status_eval(TRIG_POL_NORMAL, true, false, TRIG_ACQ_TRIGGERED, 900, 500) == TRIG_ST_WAIT &&
              trig_status_eval(TRIG_POL_NORMAL, true, false, TRIG_ACQ_HOLD, 0, 500) == TRIG_ST_WAIT,
              "NORMAL: Trig'd only while triggered records keep coming");
        CHECK(trig_status_eval(TRIG_POL_NORMAL, true, false, TRIG_ACQ_FREERUN, 100000, 500) == TRIG_ST_WAIT, "NORMAL after AUTO's forced record: Wait, not Auto");
        CHECK(trig_status_eval(TRIG_POL_SINGLE, true, false, TRIG_ACQ_TRIGGERED, 0, 500) == TRIG_ST_ARMED &&
              strcmp(trig_status_label(TRIG_ST_ARMED), "Await") == 0, "SINGLE armed: Await");
        CHECK(trig_status_eval(TRIG_POL_SINGLE, true, true, TRIG_ACQ_TRIGGERED, 0, 500) == TRIG_ST_SINGLE_DONE &&
              strcmp(trig_status_label(TRIG_ST_SINGLE_DONE), "STOP") == 0, "SINGLE captured: STOP");
        CHECK(strcmp(trig_status_label(TRIG_ST_NOCORE), "NoCore") == 0 && strcmp(trig_status_word(TRIG_ST_NOCORE), "no-core") == 0 &&
              trig_status_tone(TRIG_ST_NOCORE) == TRIG_TONE_WARN, "no usable core: NoCore, a warning");
        int longest = 0;
        for (int s = 0; s < TRIG_ST_COUNT; s++) {
            int n = (int)strlen(trig_status_label((trig_status_t)s));
            if (n > longest) longest = n;
            CHECK(strcmp(trig_status_label((trig_status_t)s), "?") != 0 && strcmp(trig_status_word((trig_status_t)s), "?") != 0,
                  "status %d has a label and a word", s);
        }
        CHECK(longest <= 6, "every label fits the 44-px badge (<= 6 chars; longest %d)", longest);
    }

    printf("\n%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
