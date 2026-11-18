/* The waits of the record acquisition, each with a name and its origin.
 *
 * Every number the capture task waits for is defined here and nowhere else:
 * what it is, and whether it was MEASURED, comes from the FACTORY design's
 * behaviour, follows from the CORE's construction, or was CHOSEN. The task
 * (drivers/fpga.c) calls the functions; the shell can override some waits at
 * run time (`fpga autowait`, `fpga postedge`, `fpga pollgap`), which does not
 * change what the defaults are.
 *
 * Pure, host tested (tests/test_acq_timing.c). */
#ifndef ACQ_TIMING_H
#define ACQ_TIMING_H

#include <stdbool.h>
#include <stdint.h>
#include "fpga_core_regs.h"

/* Samples in one record. CORE: the core's memory depth. */
#define ACQ_REC_SAMPLES                 (1u << FPGA_CORE_ADDR_W)

/* ---- the poll ---------------------------------------------------------- */

/* Time between two reads while waiting for a record. CHOSEN: the factory
 * application polls every 29 ms (EXP-54). On our core it is also the shortest
 * wait after a record, so it sets the top record rate: 33 per second. */
#define ACQ_POLL_GAP_MS                 30u

/* The task's longest sleep between two looks at a pause request. CHOSEN: at
 * the slowest timebase a fill takes 2 s, longer than a shell command's 1 s
 * wait for the bus (bench 2026-11-14). */
#define ACQ_WAIT_SLICE_MS               20u

/* How often the core's status block is read (dsp/core_status.h). CHOSEN: a
 * register that differs twice running is rewritten, i.e. within about 2 s. */
#define ACQ_CORE_WATCH_MS               1000u

/* ---- the wait after a record ------------------------------------------- */

/* FACTORY: after a hand-over that design does not arm again for about 190 ms
 * plus one fill (EXP-46: 180..210 ms measured at five timebases). Polling
 * starts a little before the end of that (EXP-54): fill + this. */
#define ACQ_POST_FACTORY_HOLD_MS        100u
/* FACTORY, timebase with no known rate: a wait that is long enough for any. */
#define ACQ_POST_UNKNOWN_RATE_MS        600u
/* CORE: our core starts the next capture when the pair has been read and can
 * hold again one record later. The wait is one fill plus this rounding
 * margin, and never less than the poll gap. */
#define ACQ_POST_CORE_MARGIN_MS         1u
/* CHOSEN: no wait after a record is longer than this, whatever the fill. */
#define ACQ_POST_MAX_MS                 5000u

/* ---- Auto: how long to wait for a trigger ------------------------------- */

/* Auto shows an untriggered picture when no trigger has come for
 *     ACQ_AUTO_FILLS fills + ACQ_AUTO_EXTRA_MS,  within [MIN, MAX].
 * A signal that repeats more slowly than this gets untriggered pictures
 * between its triggered ones; a shorter wait shows the untriggered picture
 * sooner. Owner, 2026-11-17: leave as it is.
 * ACQ_AUTO_FILLS     MEASURED on the factory design: edges per read stop
 *                    rising by about 3.6 fills (EXP-42).
 * ACQ_AUTO_EXTRA_MS  FACTORY: its re-arm bracket (180..210 ms, EXP-46) with
 *                    about 30 ms to spare.
 *                    Our core has no such bracket: on it this is only Auto's
 *                    patience, and may be chosen freely.
 * ACQ_AUTO_MIN_MS    CHOSEN floor; also the wait when the rate is unknown.
 * ACQ_AUTO_MAX_MS    CHOSEN: at the slowest timebases Auto shows something
 *                    after 1 s instead of waiting several fills. */
#define ACQ_AUTO_FILLS                  3u
#define ACQ_AUTO_EXTRA_MS               230u
#define ACQ_AUTO_MIN_MS                 25u
#define ACQ_AUTO_MAX_MS                 1000u

/* ---- op 09 (a record held on request) ----------------------------------- */

/* CORE: the hold takes effect at the next stored sample, after the request
 * has crossed into the core's clock. The read waits one sample period plus
 * this margin (one RTOS tick, which covers the crossing's 3 cycles of 8 ns). */
#define ACQ_FORCE_MARGIN_MS             1u
/* CHOSEN: with no known rate, the slowest sample period (2 ms) plus the margin. */
#define ACQ_FORCE_UNKNOWN_RATE_MS       3u
/* CHOSEN guard: never hold the task longer than this for it. */
#define ACQ_FORCE_MAX_MS                5u

/* ---- the Trig'd badge ---------------------------------------------------- */

/* The badge says Trig'd while triggered records keep coming: the last one is
 * younger than ACQ_TRIGD_WAITS times the longer of the two waits above, plus
 * ACQ_TRIGD_EXTRA_MS. FACTORY origin for the factor: a record arrives at most
 * once per wait, so one wait would flicker between records at the slow
 * timebases (EXP-72). The extra is CHOSEN; its origin is not on record. */
#define ACQ_TRIGD_WAITS                 2u
#define ACQ_TRIGD_EXTRA_MS              250u

static inline float acq_fill_ms(float fs)
{
    return (float)ACQ_REC_SAMPLES / fs * 1000.0f;
}

/* Auto's wait for a trigger at sample rate fs (<= 0: unknown), in ms. */
static inline uint16_t acq_time_auto_wait_ms(float fs, uint16_t min_ms)
{
    if (fs <= 0.0f) return min_ms;
    float ms = (float)ACQ_AUTO_FILLS * acq_fill_ms(fs) + (float)ACQ_AUTO_EXTRA_MS;
    if (ms < (float)min_ms) ms = (float)min_ms;
    if (ms > (float)ACQ_AUTO_MAX_MS) ms = (float)ACQ_AUTO_MAX_MS;
    return (uint16_t)ms;
}

/* The wait after a record before the next poll, in ms. core = our core,
 * identified; poll_gap_ms = the poll gap in force. */
static inline uint16_t acq_time_post_record_ms(float fs, bool core, uint16_t poll_gap_ms)
{
    if (fs <= 0.0f) return ACQ_POST_UNKNOWN_RATE_MS;
    float ms = acq_fill_ms(fs) + (float)ACQ_POST_FACTORY_HOLD_MS;
    if (core) {
        ms = acq_fill_ms(fs) + (float)ACQ_POST_CORE_MARGIN_MS;
        if (ms < (float)poll_gap_ms) ms = (float)poll_gap_ms;
    }
    if (ms > (float)ACQ_POST_MAX_MS) ms = (float)ACQ_POST_MAX_MS;
    return (uint16_t)ms;
}

/* The wait between op 09 and the read that follows it, in ms. */
static inline uint32_t acq_time_force_settle_ms(float fs)
{
    uint32_t ms = (fs > 0.0f) ? (uint32_t)(1000.0f / fs) + ACQ_FORCE_MARGIN_MS : ACQ_FORCE_UNKNOWN_RATE_MS;
    return ms > ACQ_FORCE_MAX_MS ? ACQ_FORCE_MAX_MS : ms;
}

/* How old the last triggered record may be for the badge to say Trig'd. */
static inline uint32_t acq_time_trigd_stale_ms(uint16_t post_record_ms, uint16_t auto_wait_ms)
{
    uint32_t a = ACQ_TRIGD_WAITS * (uint32_t)post_record_ms, b = ACQ_TRIGD_WAITS * (uint32_t)auto_wait_ms;
    return (a > b ? a : b) + ACQ_TRIGD_EXTRA_MS;
}

#endif
