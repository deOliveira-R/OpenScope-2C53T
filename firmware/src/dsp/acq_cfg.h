/*
 * OpenScope 2C53T - the capture configuration of a record (pure)
 *
 * A record is only as good as the front end it was captured with. Every
 * change of channel routing, range, coupling, position, probe, timebase,
 * trigger, interleave or roll makes the records in flight describe the OLD
 * configuration, and the first ones after it can be a mixture: the FPGA may
 * hand over a record it held from before the change, the rolling buffer keeps
 * the pre-trigger samples from before it, and relays and the offset DAC take
 * time to settle. Bench, 2026-11-14: CH2 re-enabled after a single-channel
 * spell, and one record carried converter B's copy of CH1 in the CH2 slot.
 *
 * The acquisition task keeps one acq_cfg_track_t. It notes the configuration
 * before and after every read (acq_cfg_note); a record read at t is admitted
 * only when the configuration has not changed during the whole window its
 * samples can come from: fill (1024 / fs) + the poll gap (a completed record
 * waits at most one poll to be read) + a settle margin for relays, the
 * offset DAC and the routing (acq_cfg_admit). A refused record is dropped:
 * it cannot be described honestly by either configuration. The admitted
 * record is published with the configuration noted at its read, so the
 * screen and the stream describe what it was captured with, not what is set
 * now.
 *
 * Host-tested: make test-acq-cfg.
 */
#ifndef ACQ_CFG_H
#define ACQ_CFG_H

#include <stdbool.h>
#include <stdint.h>

#define ACQ_CFG_SETTLE_MS   30u     /* relays + offset DAC + routing; to be measured */

typedef struct {
    uint8_t  mask;                  /* converter routing in force: 1 CH1, 2 CH2, 3 both */
    uint8_t  vdiv[2];               /* range index per channel */
    uint8_t  coupling[2];
    uint8_t  probe[2];              /* probe_t */
    int16_t  pos[2];                /* vertical position (offset DAC) */
    uint8_t  rate_idx;              /* reg 0x01 code in force */
    uint8_t  trig_code;             /* reg 0x08 code in force */
    uint8_t  trig_src;              /* reg 0x06: 0 CH1, 1 CH2 */
    uint8_t  trig_edge;
    uint8_t  trig_mode;             /* carried, NOT compared: the mode does not change the
                                     * samples, and comparing it would drop the one record
                                     * a SINGLE armed just before it took */
    int16_t  trig_level;            /* UI level -100..100 */
    uint8_t  il;                    /* single-channel interleave active */
    uint8_t  roll;
} acq_cfg_t;

typedef struct {
    acq_cfg_t last;                 /* the configuration last noted */
    uint32_t  stable_since;         /* ms tick it was first noted */
    uint32_t  gen;                  /* +1 per change */
    bool      primed;
} acq_cfg_track_t;

/* Equal for admission: every field but trig_mode. */
bool     acq_cfg_equal(const acq_cfg_t *a, const acq_cfg_t *b);
/* Note the configuration now; true if it differs from the last one noted
 * (then gen moves and stable_since = now_ms). The first note primes. */
bool     acq_cfg_note(acq_cfg_track_t *t, const acq_cfg_t *now, uint32_t now_ms);
/* A change was signalled without necessarily showing in the snapshot (an
 * apply entry point that re-primes the capture; a quick off/on that ends
 * where it began): restart the window as for a change. */
void     acq_cfg_touch(acq_cfg_track_t *t, uint32_t now_ms);
/* Samples of one record span 1024 / fs: in whole ms, rounded up, >= 1. */
uint32_t acq_cfg_fill_ms(float fs_hz);
/* May a record read at read_ms be published? Only if the configuration has
 * been stable for fill + poll gap + settle before the read (modular ticks). */
bool     acq_cfg_admit(const acq_cfg_track_t *t, uint32_t read_ms, uint32_t fill_ms,
                       uint32_t poll_gap_ms, uint32_t settle_ms);
/* Does a record captured with c hold channel ch's own samples (0 = CH1)?
 * With one channel routed, the other buffer holds the second converter's
 * copy of the routed channel, never its own input. */
bool     acq_cfg_has_ch(const acq_cfg_t *c, uint8_t ch);

#endif /* ACQ_CFG_H */
