/*
 * OpenScope 2C53T - the capture configuration of a record (acq_cfg.h)
 */
#include "acq_cfg.h"

bool acq_cfg_equal(const acq_cfg_t *a, const acq_cfg_t *b)
{
    return a->mask == b->mask &&
           a->vdiv[0] == b->vdiv[0] && a->vdiv[1] == b->vdiv[1] &&
           a->coupling[0] == b->coupling[0] && a->coupling[1] == b->coupling[1] &&
           a->probe[0] == b->probe[0] && a->probe[1] == b->probe[1] &&
           a->pos[0] == b->pos[0] && a->pos[1] == b->pos[1] &&
           a->rate_idx == b->rate_idx && a->trig_code == b->trig_code && a->trig_src == b->trig_src &&
           a->trig_edge == b->trig_edge &&
           a->trig_level == b->trig_level && a->il == b->il && a->roll == b->roll;
}

bool acq_cfg_note(acq_cfg_track_t *t, const acq_cfg_t *now, uint32_t now_ms)
{
    if (t->primed && acq_cfg_equal(&t->last, now))
        return false;
    const bool changed = t->primed;
    t->last = *now;
    t->stable_since = now_ms;
    t->primed = true;
    if (changed) t->gen++;
    return changed;
}

void acq_cfg_touch(acq_cfg_track_t *t, uint32_t now_ms)
{
    if (!t->primed) return;                       /* the first note primes anyway */
    t->stable_since = now_ms;
    t->gen++;
}

uint32_t acq_cfg_fill_ms(float fs_hz)
{
    if (!(fs_hz > 0.0f)) return 1000u;            /* unknown rate: be generous */
    const float ms = 1024.0f * 1000.0f / fs_hz;
    if (ms >= 60000.0f) return 60000u;
    uint32_t m = (uint32_t)ms;
    if ((float)m < ms) m++;
    return m ? m : 1u;
}

bool acq_cfg_admit(const acq_cfg_track_t *t, uint32_t read_ms, uint32_t fill_ms,
                   uint32_t poll_gap_ms, uint32_t settle_ms)
{
    if (!t->primed) return false;
    const uint32_t stable = read_ms - t->stable_since;   /* modular */
    if ((int32_t)stable < 0) return false;
    return stable >= fill_ms + poll_gap_ms + settle_ms;
}

bool acq_cfg_has_ch(const acq_cfg_t *c, uint8_t ch)
{
    if (ch > 1u) return false;
    return (c->mask & (uint8_t)(1u << ch)) != 0u;
}
