/*
 * OpenScope 2C53T - reverse-engineering toolkit: shell rows (FEATURE_RETOOLS)
 *
 * Commands that exist to find out how an unknown board works (which MCU pin
 * does what to the FPGA, what a bus carries), not to use or develop the
 * scope. Off in every product and bench image; `make app-re` (profile `re`)
 * builds the bench image with them. Same contract as shell_bench.h:
 * usb_debug.c places RETOOLS_SHELL_ROWS in the command table with no #if,
 * and with the feature off the macro is empty and nothing is compiled.
 * docs/re/README.md is the guide.
 */
#ifndef SHELL_RE_H
#define SHELL_RE_H

#if FEATURE_RETOOLS

void cmd_re_pinsweep(const char *args);

#define RETOOLS_SHELL_ROWS \
    CMD_A("re pinsweep", cmd_re_pinsweep, SC_SPI3, \
          "re pinsweep [run]               Which MCU pin reloads the FPGA: pulse each candidate low,\r\n" \
          "                                watch the config-port STATUS (no argument: list the pins)\r\n"),

#else  /* !FEATURE_RETOOLS: not in this build */

#define RETOOLS_SHELL_ROWS               /* no rows */

#endif /* FEATURE_RETOOLS */
#endif /* SHELL_RE_H */
