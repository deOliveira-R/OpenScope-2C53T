/*
 * OpenScope 2C53T - stock meter behaviours that are pure logic.
 *
 * Everything here is a transcription of stock V1.2.0 (runtime addresses, app
 * base 0x08007000) with no hardware access, so it builds on the host and is
 * covered by tests/test_meter_stock.c. Evidence tags: [V] verified from the
 * disassembly, [H] hypothesis.
 *
 *   M4  continuity-beep gate and fixed-length pulse (EXTI3 ISR 0x08009C10,
 *       housekeeping tick 0x08040508)
 *   M5  status text (parser 0x0803DB64..0x0803DE62), degF (0x0803E180),
 *       range index (tbb at 0x0803E1C8), per-stat range rescale
 *       (0x0803DD5E..0x0803DE2A)
 *   M2  Auto function: the selector-0 case of the parser's range switch
 *       (0x0803E1F2) and the AUTO key's entry state (0x0800CB50)
 *   M7  guard: the hidden SoC calibration wizard's words are never sent
 */
#ifndef METER_STOCK_H
#define METER_STOCK_H

#include <stdbool.h>
#include <stdint.h>

/* ── Status text (stock ms[0xF35], strings at 0x080163C0) ───────────── */

typedef enum {
    METER_STATUS_NONE  = 0,
    METER_STATUS_AUTO  = 1,   /* codes 10 11 12 13 = "AUTO" */
    METER_STATUS_OL    = 2,   /* codes  _  0 14  _ = " 0L " */
    METER_STATUS_BLANK = 3,   /* codes 16 16 16 16 */
    METER_STATUS_CAL   = 4,   /* codes 16 17 10 14 = " CAL" */
    METER_STATUS_PAS   = 5,   /* codes  _ 18 10  5 = " PAS" */
    METER_STATUS_ER1   = 6,   /* codes  _ 19 20  1 = " Er1" */
    METER_STATUS_ER2   = 7,
    METER_STATUS_ER3   = 8,
    METER_STATUS_ERC   = 9,   /* " Er" + any other glyph */
} meter_stock_status_t;

/* `codes` are the four segment-lookup results (meter_data.c bcd_lookup):
 * 0..9 digits, 10 'A', 11 'U', 12 't', 13 'o', 14 'L', 16 blank, 17 'C',
 * 18 'P', 19 'E', 20 'r'. Checked in stock's order. */
meter_stock_status_t meter_stock_status_from_codes(const uint8_t codes[4]);
const char *meter_stock_status_text(meter_stock_status_t st);

/* ── degF (stock 0x0803E180: v*9/5 + 32.0, literal at 0x0803E3F0) ───── */

float meter_stock_degc_to_degf(float c);

/* ── Selector / range ───────────────────────────────────────────────── */

/* Stock selector ms[0xF2D] order (word table 0x080BB3FC = 14 0C 17 0B 0A 12
 * 11 10): Auto, DCV/ACV, Diode/Continuity, Ohm, Cap, Temp, small I, large I. */
#define METER_STOCK_SEL_AUTO   0u
#define METER_STOCK_SEL_VOLT   1u
#define METER_STOCK_SEL_DIODE  2u   /* with ms[0xF36]: 1 = diode, 2 = continuity */
#define METER_STOCK_SEL_OHM    3u
#define METER_STOCK_SEL_CAP    4u
#define METER_STOCK_SEL_TEMP   5u
#define METER_STOCK_SEL_ISMALL 6u
#define METER_STOCK_SEL_ILARGE 7u

/* OpenScope submode (0 DCV .. 10 Temp) -> stock selector and ms[0xF36]
 * (1 = DC / diode, 2 = AC / continuity). False for an invalid submode. */
bool meter_stock_selector_for_submode(uint8_t submode, uint8_t *sel, uint8_t *f36);
/* Submode 11 is stock's Auto: selector 0, F36 0 (undecided). */

/* ── Auto function (M2, stock selector 0) ──────────────────────────── */

/* The Auto word (0x14) hands the choice of function to the SoC. Stock has no
 * per-function gate for it: its parser reads which function a frame belongs
 * to off the frame's own bits (ms[0xF2F], ms[0xF36], ms[0xF37]; case 0 of the
 * tbb at 0x0803E1C8, code at 0x0803E1F2..0x0803E3C8). Inputs below are the
 * frame bytes rx[2], rx[6], rx[7] (frame[2], frame[6], frame[7] here).
 *
 *   F2F  0     "AUTO" text seen: nothing is decided yet (the case does nothing)
 *        1     voltage      F36 1 = DC, 2 = AC
 *        2     continuity   F36 2
 *        3     resistance   F36 0
 *        0xFF  numeric frame, no bit recognised yet
 */
typedef struct {
    uint8_t f2f;
    uint8_t f36;
    uint8_t f37;    /* range index, as meter_stock_range_index() */
} meter_auto_fsm_t;

typedef enum {
    METER_AUTO_FN_NONE = 0,    /* not decided: show nothing as a number */
    METER_AUTO_FN_DCV,
    METER_AUTO_FN_ACV,
    METER_AUTO_FN_OHM,
    METER_AUTO_FN_CONT,
} meter_auto_fn_t;

/* Entry state: meter entry 0x0800E3C2 zeroes F2F and the AUTO key
 * (0x0800CBD6) sets F36 = 1. F37 is left as it is. */
void meter_auto_fsm_reset(meter_auto_fsm_t *st);

/* The SoC's "AUTO" glyph run (0x0803DCBE): F2F = 0, and F36 = 0 in Auto. */
void meter_auto_fsm_auto_text(meter_auto_fsm_t *st);

/* One frame. `numeric` = the four glyphs are a number, not one of stock's
 * status strings: stock presets F2F = 0xFF on that path only (0x0803E1AA), the
 * status paths (OL, blank, PAS, ...) jump past it and keep the last decision.
 * Returns the function the state now describes. */
meter_auto_fn_t meter_auto_fsm_step(meter_auto_fsm_t *st, bool numeric,
                                    uint8_t rx2, uint8_t rx6, uint8_t rx7);

/* The function a state describes (F2F, F36 as above), NONE if undecided. */
meter_auto_fn_t meter_auto_fsm_function(const meter_auto_fsm_t *st);

/* OpenScope submode a function is decoded as (0 DCV, 1 ACV, 6 Ohm, 7 Cont);
 * 0xFF for NONE. */
uint8_t meter_auto_fn_submode(meter_auto_fn_t fn);

/* Stock shows the companion frequency (rx[10..11], "%d Hz") for AC voltage
 * only: F36 == 2 and F2F == 1 (0x0803E358..0x0803E382). Continuity is F36 == 2
 * too but is not shown with a frequency. */
bool meter_auto_fsm_hz_valid(const meter_auto_fsm_t *st);

/* Range index ms[0xF37] from rx[6] and rx[7] for selectors 1..7 (Auto's own state
 * machine is meter_auto_fsm_step). `prev` is returned when
 * stock leaves the byte alone (continuity frames without rx[7].3). `degf` is
 * ms[0xF39]. Each index is a decade-of-1000 step: shown value x 1000^index is
 * the same physical quantity in the function's smallest unit [V, 0x0803DD6E]. */
uint8_t meter_stock_range_index(uint8_t sel, uint8_t f36, bool degf,
                                uint8_t rx6, uint8_t rx7, uint8_t prev);

/* Short label for the annunciator: the unit for the functions whose index is
 * a prefix step (V, Ohm, Temp), "R<n>" where the unit is not settled [H]
 * (capacitance, currents). `buf` >= 8 bytes. */
void meter_stock_range_label(uint8_t sel, uint8_t f36, bool degf, uint8_t idx,
                             char *buf, unsigned n);

/* 1000^idx (idx clamped to 0..4). */
float meter_stock_range_scale(uint8_t idx);

/* A stat captured at range `from` shown at range `to`:
 * v * 1000^from / 1000^to. */
float meter_stock_rescale(float v, uint8_t from, uint8_t to);

/* Factor that takes a reading in unit string `u` ("mV", "kOhm", "uF", ...)
 * to the base unit (V, Ohm, F, A). 0.0 for an unknown unit. OpenScope keeps
 * its own decoded unit string as the truth for the stats it shows; the stock
 * index above is for the annunciator and the shell. */
float meter_unit_factor(const char *u);

/* ── Beep (M4) ──────────────────────────────────────────────────────── */

/* Stock's EXTI3 ISR beeps iff the selector is Auto, or the selector is
 * 0x02 AND ms[0xF36] == 2, which is CONTINUITY (the left/right handler sets
 * F36 = 2 on landing in selector 2 and MOVE sends 0x17 when it sets F36 = 2),
 * and only in meter state [V 0x08009C3E..0x08009C58, checker]. */
bool meter_stock_beep_gate(uint8_t sel, uint8_t f36);
bool meter_stock_beep_gate_for_submode(uint8_t submode);   /* OpenScope numbering */

/* One SoC buzzer-line edge writes 10 to the buzzer timer register and zeroes
 * the counter at 0x20002D67; the housekeeping task increments the counter
 * while the register is non-zero and clears the register when it reaches 11.
 * A new edge during a pulse restarts it. */
#define METER_BEEP_PULSE_TICKS 11u

typedef struct {
    uint8_t ticks;
    bool    active;
} meter_beep_pulse_t;

void meter_beep_pulse_edge(meter_beep_pulse_t *p);
/* One housekeeping tick; returns whether the buzzer is still sounding. */
bool meter_beep_pulse_tick(meter_beep_pulse_t *p);

/* ── Calibration-wizard guard (M7 is NOT implemented) ───────────────── */

/* 0x02A0 / 0x0503 / 0x0501: stock sends these only from its hidden SoC
 * calibration sequence (key slots 7 and 2 in Ohm, 0x0800D6AC..0x0800D79A).
 * They may rewrite the SoC's calibration, so an OBEYED frame carrying one is
 * refused. */
bool meter_word_is_cal_wizard(uint16_t word);

/* ── Formatting ─────────────────────────────────────────────────────── */

/* Plain number with a fixed number of decimals ("-12.30"), rounded. */
void meter_stock_format_fixed(char *buf, unsigned n, float v, unsigned decimals);

#endif /* METER_STOCK_H */
