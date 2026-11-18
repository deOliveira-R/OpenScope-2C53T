/*
 * OpenScope 2C53T - reverse-engineering toolkit: the pin sweep's board data
 *
 * `re pinsweep` (shell_re.c) looks for the MCU pin wired to the FPGA's
 * RECONFIG_N: it pulses each candidate low and watches the FPGA's
 * configuration-port STATUS register. This file is the part to replace for
 * another board: which pins may be pulsed, which never, and the FPGA's
 * IDCODE (the anchor that says a STATUS read can be trusted).
 *
 * No MCU header is needed: a pin is a port letter and a number, so the host
 * test (tests/test_re_pin_sweep.c) checks the tables as they are compiled in.
 */
#ifndef RE_PIN_SWEEP_TABLE_H
#define RE_PIN_SWEEP_TABLE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char        port;   /* 'A'..'E' */
    uint8_t     pin;    /* 0..15 */
    const char *why;
} re_pin_t;

/* GW1N-2 family (the .fs header of every design for this chip carries it). */
#define RE_FPGA_IDCODE  0x0120681BUL

/* Candidates: pins the factory firmware itself drives both low and high
 * before it configures the FPGA (found by resolving every GPIO write in its
 * start-up code), so the MCU is known to own them and pulsing one cannot
 * fight another driver. */
static const re_pin_t RE_SWEEP_CANDIDATES[] = {
    { 'C',  1, "driven by the factory start-up, function unknown" },
    { 'C',  4, "driven by the factory start-up, function unknown" },
    { 'D', 12, "driven by the factory start-up; a held level was tested, a pulse is a different test" },
    { 'D', 13, "driven low by the factory start-up" },
    { 'C',  2, "driven low and high by the factory start-up" },
    { 'C', 11, "driven low and high by the factory start-up" },
    { 'B', 11, "driven low and high by the factory start-up" },
    { 'B', 10, "analog front end, driven low and high" },
    { 'A', 15, "analog front end, driven low and high" },
    { 'E',  4, "analog front end, driven low and high" },
    { 'E',  5, "analog front end, driven low and high" },
    { 'E',  6, "analog front end, driven low and high" },
};
#define RE_SWEEP_N  (sizeof RE_SWEEP_CANDIDATES / sizeof RE_SWEEP_CANDIDATES[0])

/* Never pulsed, whatever the candidate table says: the sweep checks every
 * candidate against this list at run time and the host test at build time. */
static const re_pin_t RE_SWEEP_EXCLUDED[] = {
    { 'C',  9, "power hold: low switches the unit off at once" },
    { 'C',  6, "known on this board: low reloads the FPGA (the design is lost)" },
    { 'C',  8, "button input" },
    { 'C', 13, "button input" },
    { 'B',  3, "SPI3 SCK, the bus the sweep reads through" },
    { 'B',  4, "SPI3 MISO" },
    { 'B',  5, "SPI3 MOSI" },
    { 'B',  6, "SPI3 chip select" },
    { 'A', 13, "SWD" },
    { 'A', 14, "SWD" },
    { 'A',  2, "USART2, the meter chip" },
    { 'A',  3, "USART2, the meter chip" },
    { 'A',  4, "DAC output" },
    { 'A',  5, "DAC output" },
    { 'A', 11, "USB" },
    { 'A', 12, "USB" },
    { 'B',  8, "LCD backlight" },
    { 'D',  0, "LCD bus" }, { 'D',  1, "LCD bus" }, { 'D',  4, "LCD bus" }, { 'D',  5, "LCD bus" },
    { 'D',  7, "LCD bus" }, { 'D',  8, "LCD bus" }, { 'D',  9, "LCD bus" }, { 'D', 10, "LCD bus" },
    { 'D', 11, "LCD bus" }, { 'D', 14, "LCD bus" }, { 'D', 15, "LCD bus" },
    { 'E',  7, "LCD bus" }, { 'E',  8, "LCD bus" }, { 'E',  9, "LCD bus" }, { 'E', 10, "LCD bus" },
    { 'E', 11, "LCD bus" }, { 'E', 12, "LCD bus" }, { 'E', 13, "LCD bus" }, { 'E', 14, "LCD bus" },
    { 'E', 15, "LCD bus" },
};
#define RE_SWEEP_EXCLUDED_N  (sizeof RE_SWEEP_EXCLUDED / sizeof RE_SWEEP_EXCLUDED[0])

/* The reason a pin must not be pulsed, or NULL when it may be. */
static inline const char *re_pin_excluded(char port, uint8_t pin)
{
    for (unsigned i = 0; i < RE_SWEEP_EXCLUDED_N; i++)
        if (RE_SWEEP_EXCLUDED[i].port == port && RE_SWEEP_EXCLUDED[i].pin == pin)
            return RE_SWEEP_EXCLUDED[i].why;
    return NULL;
}

/* A pin the sweep may pulse: a real port and number, and not excluded. */
static inline bool re_pin_sweepable(char port, uint8_t pin)
{
    return port >= 'A' && port <= 'E' && pin <= 15u && re_pin_excluded(port, pin) == NULL;
}

#endif /* RE_PIN_SWEEP_TABLE_H */
