/*
 * Negative-compile cases for the typed interfaces of the pure modules (ARCH
 * round, task C2): scope_measure, scope_roll, trig_edge, scope_cursor,
 * interleave, lcd_health. Each CASE calls a module the way the bare-integer
 * code could and MUST be rejected by the compiler. CASE 0 is the positive
 * control (correct use of every interface touched below): it must compile,
 * so a failure of the other cases is a unit-type error and not a typo here.
 *
 * Driven by `make test-units-c2-negative`, the same way as
 * tests/test_units_negative.c: one compile per case with -DCASE=n.
 */
#include "../src/ui/scope_measure.h"
#include "../src/ui/scope_roll.h"
#include "../src/ui/scope_cursor.h"
#include "../src/dsp/trig_edge.h"
#include "../src/dsp/interleave.h"
#include "../src/util/lcd_health.h"

#ifndef CASE
#define CASE 0
#endif

#if CASE == 0
int ok(scope_roll_fifo_t *f, const uint8_t *rec, interleave_corr_t *c, lcd_health_t *h,
       adc_raw_t raw, adc_raw_t trig_code, sample_rate_t fs, tb_code_t tb,
       volts_per_count_t k, ppm_period_t skew, uint32_t ticks)
{
    const rec_code_t code  = scope_roll_record_code(raw);
    const rec_code_t zero  = scope_measure_zero_for(1, 0);
    const rec_code_t level = trig_level_rec(trig_code);
    scope_prm_ctx_t ctx = { 0 };
    ctx.sample_rate = fs;
    ctx.volts_per_count = k;
    ctx.zero_count = zero;
    scope_roll_fifo_push(f, code, code);
    const volts_t v = scope_roll_point_volts(code, zero, k);
    const ms_t period = scope_roll_period_ms(0);
    scope_cursor_axes_t ax = { 0 };
    ax.t_conf = scope_cursor_tb_conf(tb);
    const microvolts_t l1 = { 1000 }, l2 = { 2000 };
    const scope_cursor_reading_t dt = scope_cursor_dt(&ax, 10, 20, CUR_SECOND_MINUS_FIRST);
    const scope_cursor_reading_t dv = scope_cursor_dv(&ax, l1, l2, CUR_SECOND_MINUS_FIRST);
    return (int)v.v + (int)period.ms + (int)dt.value + (int)dv.value + (int)ctx.zero_count.v
         + scope_roll_trig_hit(f, 0, 0, level, SCOPE_ROLL_EDGE_RISING)
         + trig_edge_anchor(rec, level, 1)
         + interleave_skew_set(c, skew)
         + lcd_health_step(h, 0x9C, ms_from_ticks(ticks), 0);
}
#elif CASE == 1   /* a raw ADC byte pushed into the roll FIFO (record codes) */
void bad(scope_roll_fifo_t *f, adc_raw_t raw) { scope_roll_fifo_push(f, raw, raw); }
#elif CASE == 2   /* a bare byte pushed into the roll FIFO */
void bad(scope_roll_fifo_t *f, uint8_t b) { scope_roll_fifo_push(f, b, b); }
#elif CASE == 3   /* the ADC offset passed as a parameter (roll) */
rec_code_t bad(adc_raw_t raw) { return scope_roll_record_code(raw, -28); }
#elif CASE == 4   /* the ADC offset passed as a parameter (0 V code) */
rec_code_t bad(void) { return scope_measure_zero_for(1, 0, -28); }
#elif CASE == 5   /* the 0 V code used as a number without .v */
uint8_t bad(void) { uint8_t z = scope_measure_zero_for(1, 0); return z; }
#elif CASE == 6   /* the comparator code (raw) as the anchor's record level */
int bad(const uint8_t *rec, adc_raw_t trig_code) { return trig_edge_anchor(rec, trig_code, 1); }
#elif CASE == 7   /* a bare int as the known anchor's record level */
int bad(const uint8_t *rec) { return trig_edge_anchor_known(rec, 100, 1); }
#elif CASE == 8   /* the comparator code (raw) as the roll trigger level */
int bad(const scope_roll_fifo_t *f, adc_raw_t trig_code) { return scope_roll_trig_hit(f, 0, 0, trig_code, 0); }
#elif CASE == 9   /* a raw code as the measurement zero */
void bad(scope_prm_ctx_t *ctx, adc_raw_t raw) { ctx->zero_count = raw; }
#elif CASE == 10  /* a timebase code as the record's sample rate */
void bad(scope_prm_ctx_t *ctx, tb_code_t tb) { ctx->sample_rate = tb; }
#elif CASE == 11  /* a bare float as the measurement scale */
void bad(scope_prm_ctx_t *ctx) { ctx->volts_per_count = 0.01f; }
#elif CASE == 12  /* volts where volts-per-count is expected */
volts_t bad(rec_code_t c, rec_code_t z, volts_t v) { return scope_roll_point_volts(c, z, v); }
#elif CASE == 13  /* the point period used as a bare number of ms */
uint32_t bad(void) { uint32_t t = scope_roll_period_ms(0); return t; }
#elif CASE == 14  /* a bare int as a cursor level (microvolts_t) */
scope_cursor_reading_t bad(const scope_cursor_axes_t *ax) { return scope_cursor_y(ax, 1500000); }
#elif CASE == 15  /* a sample rate where a timebase code is expected */
scope_cursor_conf_t bad(sample_rate_t fs) { return scope_cursor_tb_conf(fs); }
#elif CASE == 16  /* a difference the caller already took, no order */
scope_cursor_reading_t bad(const scope_cursor_axes_t *ax) { return scope_cursor_dt(ax, 10); }
#elif CASE == 17  /* a level difference the caller already took, no order */
scope_cursor_reading_t bad(const scope_cursor_axes_t *ax, microvolts_t d) { return scope_cursor_dv(ax, d); }
#elif CASE == 18  /* a gain x 1e6 as the interleave skew (ppm of a period) */
int bad(interleave_corr_t *c, gain_e6_t g) { return interleave_skew_set(c, g); }
#elif CASE == 19  /* a bare int as the interleave skew */
int bad(interleave_corr_t *c) { return interleave_skew_set(c, -49000); }
#elif CASE == 20  /* a bare tick count as the LCD health uptime */
int bad(lcd_health_t *h, uint32_t ticks) { return lcd_health_step(h, 0x9C, ticks, 0); }
#else
#error unknown CASE
#endif
