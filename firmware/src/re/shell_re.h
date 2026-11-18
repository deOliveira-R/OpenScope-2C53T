/*
 * OpenScope 2C53T - reverse-engineering toolkit: shell rows (FEATURE_RETOOLS)
 *
 * Commands that exist to find out how an unknown board works (which MCU pin
 * does what to the FPGA, what a bus carries), not to use or develop the
 * scope. The test for a command: would we need it to reverse engineer this
 * scope, or another board with an MCU and an FPGA, from scratch? Only
 * `re pinsweep` was written here; the others came from the bench shell and
 * keep the names the documents and scripts use. Off in every product and bench image; `make app-re` (profile `re`)
 * builds the bench image with them. Same contract as shell_bench.h:
 * usb_debug.c places RETOOLS_SHELL_ROWS in the command table with no #if,
 * and with the feature off the macro is empty and nothing is compiled.
 * docs/re/README.md is the guide.
 */
#ifndef SHELL_RE_H
#define SHELL_RE_H

#if FEATURE_RETOOLS

void cmd_re_pinsweep(const char *args);      /* shell_re.c */
void cmd_spi3_probe(void);                    /* re_spi3.c */
void cmd_spi3_gowin(void);
void cmd_spi3_edgecap(const char *args);
void cmd_spi3_opsweep(const char *args);
void cmd_fpga_bus_release(void);              /* re_fpga_config.c */
void cmd_fpga_bus_reacquire(void);
void cmd_fpga_reinit(const char *args);
void cmd_pinlog(const char *args);            /* re_board.c */

/* The 1 s timer hook of `pinlog` (usb_debug_pinlog_tick() forwards to it). */
void retools_pinlog_tick(void);

#define RETOOLS_SHELL_ROWS \
    CMD_A("re pinsweep", cmd_re_pinsweep, SC_SPI3, \
          "re pinsweep [run]               Which MCU pin reloads the FPGA: pulse each candidate low,\r\n" \
          "                                watch the config-port STATUS (no argument: list the pins)\r\n"), \
    CMD_V("fpga busrelease", cmd_fpga_bus_release, SC_EXACT, \
          "fpga busrelease                 Hand SPI3 to an external master (one-way, EXPERIMENTAL)\r\n"), \
    CMD_V("fpga busreacquire", cmd_fpga_bus_reacquire, SC_EXACT, \
          "fpga busreacquire               Take SPI3 back after busrelease (H7 capture prep)\r\n"), \
    CMD_A("pinlog", cmd_pinlog, 0, \
          "pinlog [start]                  Bench: log all GPIO inputs + battery ADC once a second\r\n"), \
    CMD_A("fpga reinit", cmd_fpga_reinit, SC_SPI3, \
          "fpga reinit [br][gap][close][f0|1|2][u<ms>][a-e<pin>][s2|sd[h|l]][tc<n>] Replay cfg\r\n" "    f=prelude frame: 0 split(stock) 1 combined 2 merge15+3B; u=pre-upload gap; k<br>=cmd-phase clk div; tc<n>=trailing clocks\r\n" "    pe=probe SYSTEM_EDIT_MODE after 0x15 (STATUS@/256); rl=send 0x3C RELOAD before prelude; reports 0x41 STATUS\r\n"), \
    CMD_A("spi3 opsweep", cmd_spi3_opsweep, 0, \
          "spi3 opsweep [a b [len]]        Sweep read opcodes (BSRAM_1/2 hunt, Step 0b)\r\n"), \
    CMD_V("spi3 gowin", cmd_spi3_gowin, SC_EXACT | SC_SPI3, \
          "spi3 gowin                      Read+decode Gowin ID/USERCODE/STATUS regs\r\n"), \
    CMD_A("spi3 edgecap", cmd_spi3_edgecap, SC_SPI3, \
          "spi3 edgecap [reps]             EXP-37: AF vs bit-bang 0x15 frames for LA edge-diff\r\n"), \
    CMD_V("spi3 probe", cmd_spi3_probe, SC_EXACT | SC_SPI3, \
          "spi3 probe                      Bit-bang MISO probe; pokes SPI3 registers directly\r\n"),

#else  /* !FEATURE_RETOOLS: not in this build */

static inline void retools_pinlog_tick(void) { }
#define RETOOLS_SHELL_ROWS               /* no rows */

#endif /* FEATURE_RETOOLS */
#endif /* SHELL_RE_H */
