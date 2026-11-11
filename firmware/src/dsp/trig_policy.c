/* See trig_policy.h (parity C3). */
#include "trig_policy.h"

trig_acq_t trig_acq_decide(const trig_acq_in_t *in)
{
    if (in == 0)
        return TRIG_ACQ_HOLD;
    /* SINGLE holds its one record until re-armed (stock: STOP + snapshot
     * after the capture, 0x0802163E). */
    if (in->mode == TRIG_POL_SINGLE && in->single_done)
        return TRIG_ACQ_HOLD;
    if (in->triggered)
        return TRIG_ACQ_TRIGGERED;

    /* A roll read from here on. */
    const bool is_auto = (in->mode == TRIG_POL_AUTO);
    if (is_auto && in->gate_on)
        return TRIG_ACQ_HOLD;           /* stock's gate: never show the roll */
    if (in->interleave)
        return TRIG_ACQ_HOLD;           /* never merge two moments as 250 MS/s */
    if (!is_auto)
        return TRIG_ACQ_HOLD;           /* NORMAL/SINGLE: the roll is not a record */
    if (!(in->budget_up || in->first_read))
        return TRIG_ACQ_HOLD;           /* AUTO while edges occur: handed-over only */
    return in->two_channels ? TRIG_ACQ_FREERUN_SPLIT : TRIG_ACQ_FREERUN;
}

trig_status_t trig_status_eval(uint8_t mode, bool running, bool single_done,
                               trig_acq_t last, uint32_t ms_since_trig,
                               uint32_t stale_ms)
{
    if (!running)
        return TRIG_ST_STOP;
    if (mode == TRIG_POL_SINGLE)
        return single_done ? TRIG_ST_SINGLE_DONE : TRIG_ST_ARMED;
    const bool fresh = (last == TRIG_ACQ_TRIGGERED) && ms_since_trig < stale_ms;
    if (fresh)
        return TRIG_ST_TRIGD;
    if (mode == TRIG_POL_AUTO) {
        if (last == TRIG_ACQ_FREERUN_SPLIT) return TRIG_ST_AUTO_SPLIT;
        if (last == TRIG_ACQ_FREERUN)       return TRIG_ST_AUTO;
    }
    /* NORMAL without a recent trigger, or AUTO holding (interleaved, gated,
     * or nothing published yet): the trace on screen is not moving. */
    return TRIG_ST_WAIT;
}

const char *trig_status_label(trig_status_t s)
{
    switch (s) {
    case TRIG_ST_STOP:        return "STOP";
    case TRIG_ST_TRIGD:       return "Trig'd";
    case TRIG_ST_WAIT:        return "Wait";
    case TRIG_ST_AUTO:        return "Auto";
    case TRIG_ST_AUTO_SPLIT:  return "Auto*";
    case TRIG_ST_ARMED:       return "Await";
    case TRIG_ST_SINGLE_DONE: return "STOP";
    default:                  return "?";
    }
}

const char *trig_status_word(trig_status_t s)
{
    switch (s) {
    case TRIG_ST_STOP:        return "stopped";
    case TRIG_ST_TRIGD:       return "triggered";
    case TRIG_ST_WAIT:        return "waiting";
    case TRIG_ST_AUTO:        return "auto-freerun";
    case TRIG_ST_AUTO_SPLIT:  return "auto-freerun-split";
    case TRIG_ST_ARMED:       return "single-armed";
    case TRIG_ST_SINGLE_DONE: return "single-captured";
    default:                  return "?";
    }
}

trig_tone_t trig_status_tone(trig_status_t s)
{
    switch (s) {
    case TRIG_ST_TRIGD:
    case TRIG_ST_AUTO:        return TRIG_TONE_OK;
    case TRIG_ST_WAIT:
    case TRIG_ST_ARMED:       return TRIG_TONE_INFO;
    default:                  return TRIG_TONE_WARN;
    }
}
