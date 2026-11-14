/* OpenScope 2C53T - signal generator frequency editor (see siggen_edit.h). */

#include "siggen_edit.h"

uint32_t siggen_edit_step(uint8_t digit)
{
    uint32_t s = 1u;
    for (uint8_t i = 0; i < digit && i < SIGGEN_EDIT_DIGITS; i++) s *= 10u;
    return s;
}

uint8_t siggen_edit_digit_of(uint32_t value, uint8_t digit)
{
    return (uint8_t)((value / siggen_edit_step(digit)) % 10u);
}

void siggen_edit_begin(siggen_edit_t *e, uint32_t current_hz)
{
    if (current_hz > SIGGEN_EDIT_MAX_HZ) current_hz = SIGGEN_EDIT_MAX_HZ;
    e->active = true;
    e->digit = SIGGEN_EDIT_START_DIGIT;
    e->value = current_hz;
    e->original = current_hz;
}

void siggen_edit_track(siggen_edit_t *e, uint32_t live_hz)
{
    if (!e->active) return;
    if (live_hz > SIGGEN_EDIT_MAX_HZ) live_hz = SIGGEN_EDIT_MAX_HZ;
    e->value = live_hz;
}

siggen_edit_result_t siggen_edit_key(siggen_edit_t *e, siggen_edit_key_t key)
{
    if (!e->active) return SGE_IGNORED;

    switch (key) {
    case SGE_KEY_LEFT:
        if (e->digit + 1u >= SIGGEN_EDIT_DIGITS) return SGE_IGNORED;
        e->digit++;
        return SGE_MOVED;

    case SGE_KEY_RIGHT:
        if (e->digit == 0u) return SGE_IGNORED;
        e->digit--;
        return SGE_MOVED;

    case SGE_KEY_UP: {
        /* Stock clamps at 50000 (0x0800A45A, movcc), it does not wrap. */
        uint32_t v = e->value + siggen_edit_step(e->digit);
        if (v > SIGGEN_EDIT_MAX_HZ) v = SIGGEN_EDIT_MAX_HZ;
        if (v == e->value) return SGE_MOVED;
        e->value = v;
        return SGE_CHANGED;
    }

    case SGE_KEY_DOWN: {
        /* Floor at 0, which is "output off" (0x08029D5C). Stock converts
         * through 0x080454B0 (0x0800A456); that it saturates a negative
         * result to 0 is [H] (siggen-parity check.md item 2). */
        uint32_t s = siggen_edit_step(e->digit);
        uint32_t v = (e->value > s) ? e->value - s : 0u;
        if (v == e->value) return SGE_MOVED;
        e->value = v;
        return SGE_CHANGED;
    }

    case SGE_KEY_ACCEPT:
        e->active = false;
        return SGE_ACCEPTED;

    case SGE_KEY_CANCEL:
        e->active = false;
        if (e->value == e->original) return SGE_ACCEPTED;
        e->value = e->original;
        return SGE_CANCELLED;
    }
    return SGE_IGNORED;
}
