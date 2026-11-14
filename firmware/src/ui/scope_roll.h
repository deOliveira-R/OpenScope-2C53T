/*
 * OpenScope 2C53T — roll mode for the slow timebases (parity item C11)
 *
 * WHAT STOCK DOES (V1.2.0, static RE of v120.bin, base 0x08007000)
 * ----------------------------------------------------------------
 * Stock's timebase index ms[0x2D] runs 0..0x1C (29 labels, 10 ns..20 s,
 * pointer table 0x0804C988). Indices 0..0x13 are FPGA records; 0x14..0x1C
 * (50 ms..20 s/div) are MCU-paced ROLL:
 *
 *  - Pacing. The timebase-change routine (0x0800B6B2..0x0800B722), for
 *    tb >= 0x14, loads TMR3 with PSC = clock/10000 - 1 (a 10 kHz tick) and
 *    ARR = table 0x080465A4[tb - 0x14] = 19, 39, 79, 199, 399, 799, 1999,
 *    3999, 7999, i.e. one point every (ARR + 1) / 10 ms = 2..800 ms. Stock
 *    draws 25 points per division, so those are 50 ms..20 s/div exactly
 *    (period table 0x080465E0 is label/25 for the same reason).
 *  - Sample. The TMR3 ISR (0x0802E71C) queues FPGA op 0x03 unless ms[0x2C]
 *    (the run/stop flag) is set. The op03 handler (0x0803E9F2) clocks four
 *    0xFF bytes after the opcode; received byte 2 is CH1 and byte 4 is CH2
 *    (strb at 0x0803EAC0 -> ms[0x482], 0x0803EB38 -> ms[0x5AF]).
 *  - FIFO. Two 301-byte FIFOs, CH1 ms[0x356..0x482] and CH2
 *    ms[0x483..0x5AF], newest at the RIGHT end; the count ms[0xDB6]
 *    saturates at 301 (`cmp #300; bhi` at 0x0803EA04) and the older points
 *    shift left by one per sample (0x0803EA1C..0x0803EA48). Drawn
 *    right-to-left: the trace enters at the right edge and scrolls left
 *    (stock Update Log V1.1.0).
 *  - FPGA register. The same routine then queues op 0x02 and op 0x01
 *    (0x0800B772, 0x0800B792), and op01's handler (0x0803E54C) writes
 *    ms[0x2D] unmodified, so stock DOES write reg 0x01 = 0x14..0x1C while
 *    rolling. (SYNTHESIS s1.4 says it never does; the checker had already
 *    downgraded that to [H], and the code above contradicts it.)
 *  - Amplitude. The op03 handler trims each point with g = (m - 28)/192 and
 *    only when m >= 221 (0x0803EB58), where the triggered path uses /150
 *    with no gate. That is a V1.1.2 regression (V1.0.7 used /150 in roll
 *    too, ADDENDUM_B S16): healthy units (m ~ 181-186) get uncorrected roll
 *    points and a ~3 % step against the triggered trace.
 *
 * WHAT THIS MODULE DOES
 * ---------------------
 * The pure half: the step table, the FIFO, the step state machine, the
 * screen geometry and the volts of one point. No FreeRTOS, no hardware, so
 * all of it is host-tested (tests/test_scope_roll.c). The op03 read and the
 * pacing loop live in fpga.c; the drawing in scope_ui.c.
 *
 * Deliberate differences from stock, each for a stated reason:
 *  - Pacing is the RTOS tick, not TMR3: OpenScope's TMR3 already runs the
 *    500 Hz button-matrix scan. Every stock period is a whole number of
 *    milliseconds ((ARR + 1)/10 = 2..800), so a 1 kHz tick reproduces the
 *    table exactly; the ARR values stay here as the evidence.
 *  - The FIFO is a ring with a count, not a memmove: the drawn result is
 *    identical (newest right, oldest left, count saturating at 301), and a
 *    sequence counter lets the display task copy it without tearing.
 *  - The FIFO holds RECORD codes (raw + FPGA_ADC_OFFSET, the same domain as
 *    the Y-T record), uncorrected. The amplitude correction is the unit's
 *    g = (m - 28)/150 (calpage_gain), applied where the Y-T trace applies
 *    it: in volts, via scope_cal_volts_per_count(). So roll and triggered
 *    agree by construction, on every unit, with no m >= 221 gate — the
 *    V1.0.7 behaviour, not the V1.1.2 regression. Trimming the samples as
 *    well would divide by g twice.
 *  - The grid is stock's C5 geometry (scope_geom.h): 25 px per division
 *    over a 301-column window, so one point per column, 25 points per
 *    division, and the whole 301-point FIFO is on the glass.
 */

#ifndef SCOPE_ROLL_H
#define SCOPE_ROLL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCOPE_ROLL_STEPS            9u     /* stock indices 0x14..0x1C */
#define SCOPE_ROLL_POINTS           301u   /* stock FIFO length (ms[0xDB6] cap) */
#define SCOPE_ROLL_POINTS_PER_DIV   25u    /* stock geometry: label = 25 x period */
#define SCOPE_ROLL_PX_PER_DIV       25u    /* C5 grid pitch (scope_geom.h): one point per column */
#define SCOPE_ROLL_TICK_HZ          10000u /* stock TMR3 tick: PSC = clk/10000 - 1 */
#define SCOPE_ROLL_STOCK_CODE_BASE  0x14u  /* stock ms[0x2D] of the first roll step */

/* ── Step table ─────────────────────────────────────────────────────── */

/* Stock TMR3 ARR for a roll step (table 0x080465A4), 0 out of range. */
uint16_t scope_roll_arr(uint8_t step);

/* Milliseconds between points: (ARR + 1) / 10 kHz. 0 out of range. */
uint32_t scope_roll_period_ms(uint8_t step);

/* Seconds per division: 25 points x the period. 0.0f out of range. */
float scope_roll_seconds_per_div(uint8_t step);

/* "50ms", "100ms", ..., "1s", ..., "20s"; "--" out of range. NUL-terminated. */
void scope_roll_label(uint8_t step, char *out, uint32_t n);

/* The reg-0x01 byte stock writes for this step (ms[0x2D] = 0x14 + step). */
uint8_t scope_roll_stock_code(uint8_t step);

/* ── FIFO ───────────────────────────────────────────────────────────── */

typedef struct {
    uint8_t           pt[2][SCOPE_ROLL_POINTS];  /* [ch][ring slot], record codes */
    uint16_t          head;    /* next slot to write */
    uint16_t          count;   /* valid points, saturates at SCOPE_ROLL_POINTS */
    uint32_t          total;   /* points ever pushed since the last reset */
    volatile uint32_t seq;     /* odd while a writer is inside push/reset */
} scope_roll_fifo_t;

void scope_roll_fifo_reset(scope_roll_fifo_t *f);
void scope_roll_fifo_push(scope_roll_fifo_t *f, uint8_t ch1, uint8_t ch2);

/* Point `age` of channel `ch` (0/1): age 0 = newest. False when age >= count. */
bool scope_roll_fifo_get(const scope_roll_fifo_t *f, uint8_t ch, uint16_t age,
                         uint8_t *out);

/* Copy a FIFO another task is writing into `dst`, retrying while a push is in
 * flight. True when the copy is consistent (it always is with no writer). */
bool scope_roll_fifo_snapshot(const scope_roll_fifo_t *src, scope_roll_fifo_t *dst);

/* Record code to store for one op03 byte: raw + adc_offset, clamped 0..255 —
 * the same transform the Y-T read applies (fpga_warmtest_read_channel). */
uint8_t scope_roll_record_code(uint8_t raw, int adc_offset);

/* ── FIFO order and the roll trigger ────────────────────────────────── */

/* Rotate both channels in place so the oldest point is at pt[ch][0] and the
 * newest at pt[ch][count - 1] (head = count mod SCOPE_ROLL_POINTS), for the
 * consumers that need a time-ordered array (the measurements). The FIFO
 * stays valid for scope_roll_fifo_get(). Meant for a snapshot copy, never
 * the live FIFO (the writer owns that). */
void scope_roll_fifo_linearize(scope_roll_fifo_t *f);

/* Stock's roll trigger (0x0803EC2E..0x0803ED18, after every op03 point).
 * Armed only when the FIFO is FULL (ms[0xDB6] == 301) and the mode is not AUTO
 * (ms[0x17] == 1). It looks at ONE pair of neighbours in the middle of the
 * FIFO, oldest-first indices 149 and 150 (+ the trigger column ms[0x1A]), of
 * the source channel ms[0x16] against the level, with stock's edge ms[0x18]:
 * 0 rising (a <= level < b), 1 falling (a >= level > b), 2 either. A hit
 * leaves the crossing at the centre of the 301 points, 150 of them after it. */
#define SCOPE_ROLL_TRIG_IDX     149u
enum { SCOPE_ROLL_EDGE_RISING = 0, SCOPE_ROLL_EDGE_FALLING = 1, SCOPE_ROLL_EDGE_BOTH = 2 };

/* True when the pair at SCOPE_ROLL_TRIG_IDX + col_shift of channel `ch` (0/1)
 * crosses `level` (record code) in `edge`. False while the FIFO is not full
 * or the pair falls outside the FIFO. */
bool scope_roll_trig_hit(const scope_roll_fifo_t *f, uint8_t ch, int col_shift,
                         uint8_t level, uint8_t edge);

/* What a hit does, by the mode (OUR numbering, scope_state.h: 0 AUTO,
 * 1 NORMAL, 2 SINGLE; stock's ms[0x17] is 0 NORMAL / 1 AUTO / 2 SINGLE).
 * AUTO never checks.
 * NORMAL restarts the window: 0x0803FFFC..0x08040000 is a 32-bit
 * `str.w #301, [ms + 0xDB4]`, i.e. ms[0xDB4] (start column) = 301 AND
 * ms[0xDB6] (width, the 301 the trigger gate tests) = 0. Each point then
 * decrements the start and grows the width (0x0803E9F2..0x0803EA14), so the
 * trace refills from the right and the trigger re-arms only once 301 new
 * points are in. Our equivalent: empty the FIFO (scope_roll_trig_step).
 * SINGLE stops: RUN = 0, TMR3 off (0x0803ECAC..0x0803ECC4), and the full FIFO
 * is the frozen frame, trigger in the middle. */
typedef enum {
    SCOPE_ROLL_TRIG_NONE = 0,   /* no hit, or AUTO */
    SCOPE_ROLL_TRIG_HIT,        /* hit in NORMAL: window restarts, rolling continues */
    SCOPE_ROLL_TRIG_STOP        /* hit in SINGLE: stop and hold */
} scope_roll_trig_act_t;

scope_roll_trig_act_t scope_roll_trig_decide(uint8_t mode, bool hit);

/* One trigger check after a push, as stock runs it after every op03 point:
 * hit -> decide -> apply to the FIFO. A NORMAL hit empties the FIFO (head and
 * count 0; `total` keeps counting the points ever pushed), which is stock's
 * width = 0 and disarms the check for the next 300 points. SINGLE leaves the
 * FIFO as the frozen frame; stopping RUN is the caller's. The caller owns
 * the FIFO (the writer task). */
scope_roll_trig_act_t scope_roll_trig_step(scope_roll_fifo_t *f, uint8_t mode,
                                           uint8_t ch, int col_shift,
                                           uint8_t level, uint8_t edge);

/* ── Timebase stepping ──────────────────────────────────────────────── */

/* One timebase key press. `dir` > 0 is slower. In Y-T the index steps as
 * before; slower from the last FPGA code (yt_count - 1) enters roll at step
 * 0. In roll the step moves; faster from step 0 leaves roll (the Y-T index is
 * left at yt_count - 1). Clamped at the slowest step. Returns true when the
 * roll flag or the step changed (the caller must re-apply roll), false when
 * only the Y-T index moved (or nothing did). */
bool scope_roll_tb_step(uint8_t *yt_idx, bool *roll, uint8_t *step, int dir,
                        uint8_t yt_count);


/* ── Screen geometry ────────────────────────────────────────────────── */

/* Age of the point drawn at column x of a `width`-px trace: the newest at
 * x = width - 1, one division (25 points) every 25 px to the left
 * (one point per column; the window is SCOPE_GEOM_COLS = 301 wide). */
uint16_t scope_roll_age_at_column(uint16_t x, uint16_t width);

/* Visible points of a `width`-px trace: the ages scope_roll_age_at_column()
 * can return, capped by what the FIFO holds. */
uint16_t scope_roll_visible(const scope_roll_fifo_t *f, uint16_t width);

/* Vertical transform of one band [y_top, y_bot). Two forms, the same two the
 * Y-T renderer has (scope_ui.c): FIXED = true volts/div, `256/scope_h` counts
 * per pixel about `center`, so the status bar's volts/div holds; FIT = the
 * band spans the visible points' own min..max (at least 16 counts, so a quiet
 * input is not blown up into noise). Results are clamped into the band. */
typedef struct {
    bool    fixed;
    uint8_t lo, hi;        /* FIT: code range mapped onto the band */
    uint8_t center;        /* FIXED: code drawn at the band's middle */
    int16_t y_top, y_bot;  /* band, y_bot exclusive */
    int16_t scope_h;       /* FIXED: full trace height the slope is set by */
} scope_roll_vmap_t;

/* FIT bounds from the newest `n` points of channel `ch`; false when empty. */
bool    scope_roll_fit(const scope_roll_fifo_t *f, uint8_t ch, uint16_t n,
                       scope_roll_vmap_t *m);
int16_t scope_roll_y(const scope_roll_vmap_t *m, uint8_t code);

/* ── Volts ──────────────────────────────────────────────────────────── */

/* Volts of one roll point, by the Y-T record's rule: (code - zero) x
 * volts_per_count, with zero from scope_measure_zero_for() and
 * volts_per_count from scope_cal_volts_per_count() (which carries the unit's
 * 1/g, g = (m - 28)/150). volts_per_count <= 0 means uncalibrated: returns
 * 0.0f and the caller prints counts. */
float scope_roll_point_volts(uint8_t code, uint8_t zero, float volts_per_count);

#ifdef __cplusplus
}
#endif

#endif /* SCOPE_ROLL_H */
