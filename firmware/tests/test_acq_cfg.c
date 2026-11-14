/*
 * Host test for src/dsp/acq_cfg.c: a record is admitted only after the
 * capture configuration has been stable for its whole window.
 *
 * Build/run:  make -C firmware test-acq-cfg
 */
#include <stdio.h>
#include <string.h>
#include "../src/dsp/acq_cfg.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static acq_cfg_t base(void)
{
    acq_cfg_t c;
    memset(&c, 0, sizeof c);
    c.mask = 3; c.vdiv[0] = 7; c.vdiv[1] = 4; c.rate_idx = 6; c.trig_code = 128;
    return c;
}

int main(void)
{
    acq_cfg_track_t t;
    memset(&t, 0, sizeof t);
    acq_cfg_t c = base();

    /* not primed: nothing admitted */
    CHECK(!acq_cfg_admit(&t, 1000u, 1u, 30u, 30u));
    CHECK(!acq_cfg_note(&t, &c, 1000u));          /* priming is not a change */
    CHECK(t.gen == 0u && t.stable_since == 1000u);
    CHECK(!acq_cfg_admit(&t, 1060u, 1u, 30u, 30u));   /* 60 < 61 */
    CHECK(acq_cfg_admit(&t, 1061u, 1u, 30u, 30u));
    CHECK(!acq_cfg_note(&t, &c, 1500u));           /* same: stable_since stays */
    CHECK(t.stable_since == 1000u && acq_cfg_admit(&t, 1500u, 1u, 30u, 30u));

    /* every field is part of the configuration */
    const size_t n_fields = 20;
    for (size_t k = 0; k < n_fields; k++) {
        acq_cfg_t d = c;
        switch (k) {
        case 0: d.mask = 1; break;
        case 1: d.vdiv[0]++; break;
        case 2: d.vdiv[1]++; break;
        case 3: d.coupling[0] = 1; break;
        case 4: d.coupling[1] = 1; break;
        case 5: d.probe[0] = 1; break;
        case 6: d.probe[1] = 1; break;
        case 7: d.pos[0] = 5; break;
        case 8: d.pos[1] = -5; break;
        case 9: d.rate_idx++; break;
        case 10: d.trig_code++; break;
        case 11: d.trig_src = 1; break;
        case 12: d.trig_edge = 1; break;
        case 13: d.trig_edge = 2; break;
        case 14: d.trig_level = 10; break;
        case 15: d.il = 1; break;
        case 16: d.roll = 1; break;
        default: d.mask = 2; break;
        }
        CHECK(!acq_cfg_equal(&c, &d));
    }

    /* the trigger mode is carried, not compared (SINGLE's one record) */
    {
        acq_cfg_t d = c;
        d.trig_mode = 2;
        CHECK(acq_cfg_equal(&c, &d));
    }

    /* a change: gen moves, the window restarts; the bench case (CH2 back on)
     * refuses the record read 20 ms later and admits one read after the window */
    acq_cfg_t one = c;
    one.mask = 1; one.il = 1;
    CHECK(acq_cfg_note(&t, &one, 2000u) && t.gen == 1u);
    CHECK(acq_cfg_note(&t, &c, 3000u) && t.gen == 2u && t.stable_since == 3000u);
    const uint32_t fill = acq_cfg_fill_ms(5.0e6f);   /* 5 MSa/s: 0.2048 ms -> 1 */
    CHECK(fill == 1u);
    CHECK(!acq_cfg_admit(&t, 3020u, fill, 30u, ACQ_CFG_SETTLE_MS));
    CHECK(acq_cfg_admit(&t, 3061u, fill, 30u, ACQ_CFG_SETTLE_MS));

    /* a change between the two notes around a read refuses that read */
    CHECK(acq_cfg_admit(&t, 4000u, fill, 30u, 30u));
    acq_cfg_t v = c;
    v.vdiv[1] = 5;
    CHECK(acq_cfg_note(&t, &v, 4001u));              /* noted after the read */
    CHECK(!acq_cfg_admit(&t, 4001u, fill, 30u, 30u));

    /* a signalled change with the same snapshot (off then on again within
     * one sleep of the task) restarts the window too */
    const uint32_t g = t.gen;
    acq_cfg_touch(&t, 4100u);
    CHECK(t.gen == g + 1u && t.stable_since == 4100u && !acq_cfg_admit(&t, 4120u, fill, 30u, 30u));
    CHECK(!acq_cfg_note(&t, &v, 4130u) && t.stable_since == 4100u);
    CHECK(acq_cfg_admit(&t, 4161u, fill, 30u, 30u));
    t.stable_since = 4001u;                          /* back to the case below */

    /* slow timebase: the window grows with the record */
    CHECK(acq_cfg_fill_ms(2500.0f) == 410u);         /* 409.6 ms */
    CHECK(acq_cfg_fill_ms(0.0f) == 1000u);
    CHECK(!acq_cfg_admit(&t, 4001u + 400u, 410u, 30u, 30u));
    CHECK(acq_cfg_admit(&t, 4001u + 470u, 410u, 30u, 30u));

    /* tick wrap */
    memset(&t, 0, sizeof t);
    (void)acq_cfg_note(&t, &c, 0xFFFFFFF0u);
    CHECK(!acq_cfg_admit(&t, 0x00000010u, 1u, 30u, 30u));   /* 32 ms */
    CHECK(acq_cfg_admit(&t, 0x00000060u, 1u, 30u, 30u));    /* 112 ms */

    /* which buffers hold their own channel */
    acq_cfg_t m = c;
    m.mask = 3; CHECK(acq_cfg_has_ch(&m, 0) && acq_cfg_has_ch(&m, 1));
    m.mask = 1; CHECK(acq_cfg_has_ch(&m, 0) && !acq_cfg_has_ch(&m, 1));
    m.mask = 2; CHECK(!acq_cfg_has_ch(&m, 0) && acq_cfg_has_ch(&m, 1));
    CHECK(!acq_cfg_has_ch(&m, 2));

    printf("test_acq_cfg: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
