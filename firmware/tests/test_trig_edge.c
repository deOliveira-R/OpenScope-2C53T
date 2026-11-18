/*
 * dsp/trig_edge.c: the trigger anchor of a time-ordered record, the software
 * edge search and the in-place rotation. Sections [1]..[6] tested the
 * classifier and the seam finders that were removed with the factory FPGA
 * design (the core names the seam, dsp/trig_edge_reg.h); the numbers of the
 * remaining sections are unchanged.
 */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "trig_edge.h"
#include "fixtures/trig_edge_records.h"

static int fails = 0;

/* Fixture and synthetic levels are record codes; the typed API takes
 * rec_code_t. */
#define RC(x) ((rec_code_t){ (uint8_t)(x) })
#define CHECK(cond, ...) do { if (cond) { printf("  PASS: "); } else { printf("  FAIL: "); fails++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

/* The seam of each record in fixtures/trig_edge_records.h (the index of its
 * oldest sample; -1 = none stood out, the record is skipped as before), as
 * the removed value-step seam finder gave it on 2026-11-17, its last day.
 * The fixtures predate the core: they carry no header that names the seam. */
static const int16_t FIXTURE_SEAM[] = {
    292, 291, 322, 934, 322, 300, 321, 319, 565, 101,  86, 538,  79,  91,  83, 100,  -1, 594,  10,  67,
    572, 638,  46,  28, 356, 360, 346, 965, 910, 293, 292, 287,  68, 533, 595,  51, 592,  41,  32,  98,
    571,  94,  79,  97,  46, 584, 103,  98, 103, 497, 103, 504, 108,  78, 116, 510, 490, 523
};
_Static_assert(sizeof FIXTURE_SEAM / sizeof FIXTURE_SEAM[0] == EDGE_FIXTURE_N, "one seam per fixture record");

static uint32_t lcg = 12345u;
static double urand(void) { lcg = lcg * 1664525u + 1013904223u; return (lcg >> 8) / 16777216.0; }

int main(void)
{
    static uint8_t rec[1024];
    printf("[7] edge search: hyst 0 is stock's test exactly (0x08028DE4, ADDENDUM_A S3)\n");
    {
        int mism = 0, hits = 0;
        static uint8_t r[1024];
        for (int t = 0; t < 30000; t++) {
            /* Short random walks and plain noise around a random level, so
             * crossings, spikes and plateaus at the level all occur. */
            int n = 8 + (int)(urand() * 120.0);
            int v = (int)(urand() * 255.0);
            int walk = (t & 1);
            for (int i = 0; i < n; i++) {
                if (walk) { v += (int)(urand() * 9.0) - 4; if (v < 0) v = 0; if (v > 255) v = 255; }
                else v = 100 + (int)(urand() * 12.0);
                r[i] = (uint8_t)v;
            }
            int L = walk ? (int)r[(int)(urand() * n)] : 100 + (int)(urand() * 12.0);
            int rising = (t >> 1) & 1;
            unsigned conf = (unsigned)((t >> 2) & 1);
            int lo = (int)(urand() * 6.0), hi = n - (int)(urand() * 6.0);
            /* Stock's strict "L < s" is our inclusive ">= L + 1" (and
             * "<= L - 1" falling): the same test, one count apart in how the
             * level is named. */
            int a = trig_edge_search(r, n, lo, hi, rising ? L + 1 : L - 1, rising, 0u, conf);
            int b = trig_edge_search_stock(r, n, lo, hi, L, rising, conf);
            if (a >= 0) hits++;
            if (a != b) { if (++mism <= 3) printf("    case %d: ours %d stock %d\n", t, a, b); }
        }
        CHECK(mism == 0, "30000 random cases, both edges, 2 and 3 points: %d mismatches (%d hits)", mism, hits);
    }

    printf("[8] edge search: what each filter buys\n");
    {
        /* (a) A lone one-sample spike over the level, then the real edge. */
        static uint8_t r[256];
        for (int i = 0; i < 256; i++) r[i] = (uint8_t)(i < 150 ? 80 : 180);
        r[60] = 140;                         /* spike: 80 -> 140 -> 80 across L = 128 */
        int st3 = trig_edge_search_stock(r, 256, 1, 255, 127, 1, 1u);
        int st2 = trig_edge_search_stock(r, 256, 1, 255, 127, 1, 0u);
        int sch = trig_edge_search(r, 256, 1, 255, 128, 1, 3u, 0u);   /* Schmitt only (old soft trigger) */
        int both = trig_edge_search(r, 256, 1, 255, 128, 1, 3u, 1u);
        CHECK(st3 == 150 && both == 150, "spike: the third point skips it (stock 3-pt %d, ours %d)", st3, both);
        CHECK(st2 == 60 && sch == 60, "spike: 2 points / Schmitt alone take it (stock 2-pt %d, Schmitt %d)", st2, sch);

        /* (b) A slow noisy ramp: the jitter of the index found, record to
         * record, around the true crossing. */
        int js = 0, jb = 0, jo = 0, ns = 0, nb = 0, no = 0;
        double ss = 0, sb = 0, so = 0;
        for (int t = 0; t < 2000; t++) {
            double x0 = 400.0 + urand() * 8.0;          /* true crossing, fractional */
            for (int i = 0; i < 1024; i++) {
                double v = 128.0 + (i - x0) * 0.25 + (urand() - 0.5) * 6.0;   /* 0.25 ct/sample, +/-3 noise */
                if (v < 0) v = 0; if (v > 255) v = 255;
                rec[i] = (uint8_t)(v + 0.5);
            }
            int a = trig_edge_search_stock(rec, 1024, 300, 600, 127, 1, 1u);
            int b = trig_edge_search(rec, 1024, 300, 600, 128, 1, 3u, 1u);
            if (a >= 0) { double d = a - x0; ss += d * d; ns++; if (d < -6 || d > 6) js++; }
            int o = trig_edge_search(rec, 1024, 300, 600, 128, 1, 3u, 0u);   /* the old soft trigger */
            if (b >= 0) { double d = b - x0; sb += d * d; nb++; if (d < -6 || d > 6) jb++; }
            if (o >= 0) { double d = o - x0; so += d * d; no++; if (d < -6 || d > 6) jo++; }
        }
        double rs = ns ? sqrt(ss / ns) : 99, rb = nb ? sqrt(sb / nb) : 99, ro = no ? sqrt(so / no) : 99;
        printf("    noisy ramp, rms error vs the true crossing: stock 3-pt %.2f (%d of %d > 6 off), "
               "Schmitt+3-pt %.2f (%d of %d), Schmitt 2-pt %.2f (%d of %d)\n",
               rs, js, ns, rb, jb, nb, ro, jo, no);
        /* The third point costs nothing on a slow edge: it finds every edge
         * and is no less steady than either test alone. Its gain is the
         * spike in (a). */
        CHECK(nb == 2000 && rb <= rs + 0.05 && rb <= ro + 0.05,
              "noisy slow edge: Schmitt+3-pt finds all 2000 and is as steady as either alone (%.2f vs %.2f / %.2f)",
              rb, rs, ro);
    }

    printf("[9] edge search on the real records vs the hardware anchor (2-point)\n");
    {
        /* These are 2 Hz triangles at 0x12: ~0.1 count per sample under
         * +/-1 count of noise, so the "crossing" is a 20-sample band of
         * chatter (EXP-53 saw the FPGA fire anywhere in 501..520). The third
         * point skips the chatter samples that dip straight back, so it lands
         * at or after the hardware's first touch, never before it, and the
         * offset measures how much chatter these records hold -- the display
         * fallback cost, not an error. With confirm 0 the search must be the
         * anchor itself (same >= compare, same Schmitt). */
        static uint8_t tmp[1024];
        int anch = 0, found = 0, early = 0, same2 = 0, maxlate = 0, within4 = 0;
        for (unsigned i = 0; i < EDGE_FIXTURE_N; i++) {
            if (FIXTURE_SEAM[i] < 0) continue;
            const uint32_t k = (uint32_t)FIXTURE_SEAM[i];
            for (int j = 0; j < 1024; j++) tmp[j] = EDGE_FIXTURES[i].rec[(j + k) & 1023u];
            int rising = EDGE_FIXTURES[i].label > 0;
            int c = EDGE_FIXTURES[i].crossing;
            int a = trig_edge_anchor(tmp, RC(c), rising);
            if (a < 0) continue;
            anch++;
            /* The anchor arms from 448: search the same span. It takes the
             * crossing NEAREST 512, the search the FIRST, so a record with
             * two touches in the span can differ; compare b2 with a. */
            int b2 = trig_edge_search(tmp, 1024, 449, 561, c, rising, 3u, 0u);
            int b = trig_edge_search(tmp, 1024, 449, 561, c, rising, 3u, 1u);
            if (b2 == a) same2++;
            if (b < 0) continue;
            found++;
            if (b < b2) early++;
            if (b - b2 > maxlate) maxlate = b - b2;
            if (b - b2 <= 4) within4++;
        }
        CHECK(anch > 0 && same2 * 10 >= anch * 9,
              "2-point search = the hardware anchor on %d of %d records (the rest: an earlier touch in the span)",
              same2, anch);
        CHECK(found == anch && early == 0,
              "3-point: finds all %d of %d, never before the 2-point touch (%d early); within 4 samples on %d, latest +%d",
              found, anch, early, within4, maxlate);
    }

    printf("[10] in-place rotation == the old staged three-memcpy rotation\n");
    {
        /* The reference is the code fpga_acq_unrotate_staging() ran before
         * the RAM diet, verbatim apart from the buffer. */
        static uint8_t ref[1024], got[1024], tmp[1024], orig[1024];
        int bad = 0;
        for (unsigned i = 0; i < 1024u; i++) orig[i] = (uint8_t)(urand() * 256.0);
        for (uint32_t k = 0; k < 1024u; k++) {
            memcpy(ref, orig, 1024u);
            memcpy(got, orig, 1024u);
            if (k != 0u) {
                memcpy(tmp, ref + k, 1024u - k);
                memcpy(tmp + (1024u - k), ref, k);
                memcpy(ref, tmp, 1024u);
            }
            trig_edge_rotate_left(got, 1024u, k);
            if (memcmp(ref, got, 1024u) != 0) bad++;
        }
        CHECK(bad == 0, "all 1024 rotations of a 1024-sample record match (%d differ)", bad);
        uint8_t small[5] = { 0, 1, 2, 3, 4 };
        trig_edge_rotate_left(small, 5u, 7u);    /* k mod n = 2 */
        CHECK(small[0] == 2 && small[1] == 3 && small[2] == 4 && small[3] == 0 && small[4] == 1,
              "odd length, k >= n taken mod n");
    }

    printf("[11] a comparator code below the ADC offset: record level 0, no trigger found\n");
    {
        /* Before the typed API the callers passed code - 28 unclamped (a
         * negative int); trig_level_rec() clamps it to record 0. Both must
         * mean "no crossing": a record resting on the low rail (bytes at 0)
         * is not a falling edge through a level the record cannot go below. */
        static uint8_t rail[1024];
        for (int i = 0; i < 1024; i++)
            rail[i] = (uint8_t)((i / 16) & 1 ? 0 : 60);       /* square 0..60, low half on the rail */
        const rec_code_t below = trig_level_rec((adc_raw_t){ 10 });
        CHECK(below.v == 0, "code 10 (< %d) is record level 0", ADC_REC_OFFSET);
        CHECK(trig_level_rec((adc_raw_t){ 128 }).v == 100, "code 128 is record level 100");
        CHECK(trig_edge_anchor(rail, below, 0) == -1 && trig_edge_anchor(rail, below, 1) == -1,
              "no anchor on either edge at level 0 with the record on the low rail");
        CHECK(trig_edge_anchor(rail, RC(30), 0) >= 0 && trig_edge_anchor(rail, RC(30), 1) >= 0,
              "the same record anchors on both edges at level 30 (control)");
    }

    printf("[11] the trigger of a record our core ordered: at 512 by construction\n");
    {
        static uint8_t r[1024];
        bool exact = false;
        /* a slow ramp, 1 code per 60 samples, crossing 100 at index 512: it never goes 3 counts below the level
         * inside the search window, so the search has no anchor; the known place has it */
        for (int i = 0; i < 1024; i++) r[i] = (uint8_t)(i < 512 ? 99 - (511 - i) / 60 : 100 + (i - 512) / 60);
        CHECK(trig_edge_anchor(r, RC(100), 1) == -1, "slow rising ramp: the search finds no anchor");
        CHECK(trig_edge_anchor_known(r, RC(100), 1) == 512, "slow rising ramp: the known place is 512");
        CHECK(trig_edge_anchor_of(r, RC(100), 1, true, &exact) == 512 && exact, "core-ordered: 512, exact");
        CHECK(trig_edge_anchor_of(r, RC(100), 1, false, &exact) == -1 && !exact, "not core-ordered: the search's answer");
        CHECK(trig_edge_anchor_known(r, RC(100), 0) == -1, "wrong direction: not there");
        for (int i = 0; i < 1024; i++) r[i] = (uint8_t)(255 - r[i]);         /* mirror: falling through 155 */
        CHECK(trig_edge_anchor_known(r, RC(156), 0) == 512, "slow falling ramp: 512");
        /* merged 250 MS/s record: the other converter's sample at 511 is already past the level */
        for (int i = 0; i < 1024; i++) r[i] = (uint8_t)(i < 511 ? 90 : 110);
        CHECK(trig_edge_anchor_known(r, RC(100), 1) == 511, "merged record: crossing one index earlier");
        /* the firmware's level is not where the core fired: fall back to the search */
        for (int i = 0; i < 1024; i++) r[i] = (uint8_t)(i < 520 ? 60 : 140);
        CHECK(trig_edge_anchor_known(r, RC(100), 1) == -1, "no crossing at 511/512: not claimed");
        CHECK(trig_edge_anchor_of(r, RC(100), 1, true, &exact) == 520 && !exact, "then the search decides (520), not exact");
        /* levels the search refuses (outside 4..251) */
        for (int i = 0; i < 1024; i++) r[i] = (uint8_t)(i < 512 ? 0 : 5);
        CHECK(trig_edge_anchor(r, RC(2), 1) == -1 && trig_edge_anchor_known(r, RC(2), 1) == 512, "level 2: known place only");
    }

    printf("\n%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
