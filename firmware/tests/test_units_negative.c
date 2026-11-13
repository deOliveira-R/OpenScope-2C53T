/*
 * Negative-compile cases for util/units.h. Each CASE below mixes units the way
 * the old bare-integer code could, and MUST be rejected by the compiler.
 * CASE 0 is the positive control (correct use): it must compile, so a failure
 * of the other cases is a unit-type error and not a typo in this file.
 *
 * Driven by `make test-units-negative`, which compiles the file once per case
 * with -DCASE=n and passes only if CASE 0 compiles and every other case fails.
 */
#include "../src/util/units.h"

#ifndef CASE
#define CASE 0
#endif

#if CASE == 0
int ok(adc_raw_t r, rec_code_t z, volts_per_count_t k)
{
    rec_code_t c = rec_from_raw(r);
    volts_t v = volts_of(rec_sub(c, z), k);
    permille_t pm = permille_from_ratio(0.5f);
    return (int)v.v + percent_from_permille(pm).pct;
}
#elif CASE == 1   /* raw code where a record code is expected */
rec_delta_t bad(adc_raw_t r, rec_code_t z) { return rec_sub(r, z); }
#elif CASE == 2   /* record code where a raw code is expected */
rec_code_t bad(rec_code_t c) { return rec_from_raw(c); }
#elif CASE == 3   /* implicit integer -> unit */
rec_code_t bad(void) { rec_code_t c = 5; return c; }
#elif CASE == 4   /* unit -> integer without .v */
int bad(rec_code_t c) { int x = c; return x; }
#elif CASE == 5   /* arithmetic on units */
rec_code_t bad(rec_code_t a, rec_code_t b) { return a + b; }
#elif CASE == 6   /* comparing different units */
int bad(adc_raw_t r, rec_code_t c) { return r == c; }
#elif CASE == 7   /* assigning one unit to another */
void bad(adc_raw_t r, rec_code_t *c) { *c = r; }
#elif CASE == 8   /* per-mille where percent is expected */
percent_t bad(permille_t p) { percent_t q = p; return q; }
#elif CASE == 9   /* percent passed to the per-mille converter */
percent_t bad(percent_t p) { return percent_from_permille(p); }
#elif CASE == 10  /* ppm of a period used as a gain x 1e6 */
gain_e6_t bad(ppm_period_t p) { return p; }
#elif CASE == 11  /* ms where ticks (uint32_t) are fine but ms stays typed */
uint32_t bad(void) { ms_t m = ms_from_ticks(10); return m + 1u; }
#elif CASE == 12  /* volts where microvolts are expected */
microvolts_t bad(volts_t v) { return v; }
#elif CASE == 13  /* timebase code used as a sample rate */
sample_rate_t bad(tb_code_t t) { return t; }
#elif CASE == 14  /* delta passed where a code is expected */
adc_raw_t bad(rec_delta_t d) { return raw_from_rec(d); }
#elif CASE == 15  /* volts-per-count passed as volts */
volts_t bad(volts_per_count_t k, rec_delta_t d) { return volts_of(d, (volts_t){ 1.0f }); (void)k; }
#elif CASE == 16  /* literal float where a typed rate is expected */
sample_rate_t bad(void) { sample_rate_t s = 1.0e6f; return s; }
#else
#error unknown CASE
#endif
