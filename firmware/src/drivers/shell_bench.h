/*
 * OpenScope 2C53T - the bench shell's rows and handlers (FEATURE_BENCHSHELL)
 *
 * The feature's contract with the shell core, after the template
 * src/stream/stream.h: usb_debug.c includes this header and places
 * BENCHSHELL_SHELL_ROWS in the command table with no #if. With
 * FEATURE_BENCHSHELL 0 the row macro is empty (`help` does not list the
 * commands, they answer "Unknown command" like any typo) and the handlers
 * are not compiled. CMD_A / CMD_V / SC_* are the table's own macros
 * (usb_debug.c); dispatch is by longest match, so where the rows sit in the
 * table only decides where `help` prints them.
 */
#ifndef SHELL_BENCH_H
#define SHELL_BENCH_H

#if FEATURE_BENCHSHELL

/* Helpers the reverse-engineering toolkit (src/re/, FEATURE_RETOOLS) shares:
 * raw SPI3 byte exchange, a Gowin configuration-port register read (the
 * caller sets the clock), the SPI3 clock divider (returns the previous
 * CTRL1), one pin's 4-bit mode field. Shell task only. */
#include "at32f403a_407.h"
uint8_t  spi3_raw_xfer(uint8_t tx);
uint32_t spi3_gowin_read_reg(uint8_t opcode);
uint32_t spi3_set_baud(uint32_t br);
uint8_t  gpio_cfg_nibble(gpio_type *port, uint8_t pin_no);
void     gpio_cfg_nibble_set(gpio_type *port, uint8_t pin_no, uint8_t nibble);
/* One read window on the running design: CS low, the opcode, two filler
 * bytes, `len` payload bytes; prints the statistics (and the bytes with
 * `dump`), returns true when the payload varies. The caller parks the
 * acquisition task (fpga_acq_pause). `spi3 opread` here, `spi3 opsweep` in
 * the toolkit. */
#include <stdbool.h>
/* The longest window the two commands accept. The value came with them
 * (bench plan 2026-08-14, step 0b) and its reason was not written down; it
 * is twice the sweep's default of 2048 bytes (1024 words of a word-wide
 * block RAM pair). */
#define SPI3_OPREAD_MAX_LEN 4096u
bool     spi3_opread_window(uint8_t opcode, uint32_t len, bool dump);
/* One channel read (opcode 0x04 or 0x05) with its statistics: `spi3 acqread`
 * here, the read-back after the toolkit's `fpga reinit`. The caller holds the
 * bus (SC_SPI3 on its row). */
void     cmd_spi3_acqread_one(uint8_t opcode);
/* Can a byte leave PA2 now (USART2 enabled, transmitter on)? Prints the
 * refusal when not. `usart tx` here, `usart raw` in the toolkit. */
bool     usart_tx_wire_live(void);

void cmd_usart_tx(const char *args);
void cmd_bench_snapshot(void);
void cmd_bench_restore(void);
void cmd_gpio_mode(const char *args);
void cmd_gpio_set(const char *args);
void cmd_buzzer_test(const char *args);
void cmd_ui_livecheck(const char *args);
void cmd_gpio_read(const char *args);
void cmd_gpio_scan(void);
void cmd_mem_read(const char *args);
void cmd_mem_write(const char *args);
void cmd_flash_diag(void);
void cmd_flash_wtest(const char *args);
void cmd_scope_trig(const char *args);
void cmd_scope_trig2(const char *args);
void cmd_fpga_usart(const char *args);
void cmd_fpga_autowait(const char *args);
void cmd_fpga_pollgap(const char *args);
void cmd_fpga_scope_coupling(const char *args);
void cmd_fpga_acqbr(const char *args);
void cmd_fpga_scope_zero(const char *args);
void cmd_fpga_scope_freq(const char *args);
void cmd_fpga_scope_cal(void);
void cmd_fpga_diag_clear(void);
void cmd_fpga_configbb(void);
void cmd_fpga_slot(const char *args);
void cmd_fpga_reload(const char *args);
void cmd_fpga_meter_reinit(const char *args);
void cmd_spi3_read(const char *args);
void cmd_spi3_frame(void);
void cmd_lcd(const char *args);
void cmd_lcd_readback(const char *args);
void cmd_lcd_scroll(const char *args);
void cmd_lcd_te_scan(const char *args);
void cmd_lcd_scanline(const char *args);
void cmd_lcd_colcheck(void);
void cmd_spi3_xfer(const char *args);
void cmd_spi3_acqread(void);
void cmd_spi3_opread(const char *args);
void cmd_fpga_selftest(void);
void cmd_spi3_seq(const char *args);

#define BENCHSHELL_SHELL_ROWS \
    CMD_A("usart tx", cmd_usart_tx, SC_NEEDARGS, \
          "usart tx <cmd_hi> <cmd_lo>      Send FPGA command (queued, or direct if no dvom_TX)\r\n"), \
    CMD_A("gpio set", cmd_gpio_set, SC_NEEDARGS, \
          "gpio set <port><pin> <0|1>      Drive GPIO pin (REFUSES if pin is an input)\r\n" "  e.g.: gpio set B11 1\r\n"), \
    CMD_A("gpio mode", cmd_gpio_mode, SC_NEEDARGS, \
          "gpio mode <port><pin> <out|in>  Set pin direction (RMW of its CRL/CRH nibble)\r\n" "  e.g.: gpio mode A6 out   (out = push-pull 50MHz, in = floating)\r\n"), \
    CMD_V("bench snapshot", cmd_bench_snapshot, SC_EXACT, \
          "bench snapshot                  Save frontend/FPGA pin modes+levels\r\n"), \
    CMD_V("bench restore", cmd_bench_restore, SC_EXACT, \
          "bench restore                   Put that saved pin posture back (hermetic tests)\r\n"), \
    CMD_A("gpio read", cmd_gpio_read, SC_NEEDARGS, \
          "gpio read <port><pin>           Read GPIO pin\r\n"), \
    CMD_V("gpio scan", cmd_gpio_scan, SC_EXACT, \
          "gpio scan                       Scan FPGA-related pins\r\n"), \
    CMD_A("fpga scope livecheck", cmd_ui_livecheck, 0, \
          "fpga scope livecheck [frames]   Live trace redraw: every pixel against the per-pixel rule\r\n"), \
    CMD_A("buzzer test", cmd_buzzer_test, 0, \
          "buzzer test [ms]                Force continuity buzzer briefly\r\n"), \
    CMD_A("mem read", cmd_mem_read, SC_NEEDARGS, \
          "mem read <addr> [count]         Read 32-bit words\r\n" "  e.g.: mem read 0x40021000 4\r\n"), \
    CMD_A("mem write", cmd_mem_write, SC_NEEDARGS, \
          "mem write <addr> <value>        Write 32-bit word\r\n"), \
    CMD_A("trig2", cmd_scope_trig2, SC_NEEDARGS, \
          "trig2 raw <0-4095>              Write CH2 ref: TMR13 CH1 PWM-DAC (PA6)\r\n" "trig2 <range> <level>           CH2 vertical-offset ref via the CH1 cal formula\r\n"), \
    CMD_A("trig", cmd_scope_trig, SC_NEEDARGS, \
          "trig raw <0-4095>               Write DAC1 (PA4) directly + sw trigger\r\n" "trig <range> <level>            Scope trigger DAC: range 0-9, level -100..100\r\n"), \
    CMD_A("flash wtest", cmd_flash_wtest, SC_NEEDARGS, \
          "flash wtest <addr> CONFIRM      Non-destructive write-primitive self-test (blank 4KB sector)\r\n"), \
    CMD_V("flash diag", cmd_flash_diag, SC_EXACT, \
          "flash diag                      W25Q SR1/SR2/SR3 + WEL-latch check\r\n"), \
    CMD_V("fpga diag clear", cmd_fpga_diag_clear, SC_EXACT, \
          "fpga diag clear                 Clear FPGA bench counters/state\r\n"), \
    CMD_V("fpga selftest", cmd_fpga_selftest, SC_EXACT, \
          "fpga selftest                   Precondition table: SPI3 clk, IDCODE anchor,\r\n" "                                USART2 state, every driven pin's mode+level\r\n"), \
    CMD_A("fpga slot", cmd_fpga_slot, 0, \
          "fpga slot [info|put <len> <crc32hex> <name> [force]|data <off> <hex>|pos|end|boot on|off|clear]\r\n" "                                FPGA design stored on the flash (scripts/fpga_put.py)\r\n"), \
    CMD_A("fpga reload", cmd_fpga_reload, 0, \
          "fpga reload [builtin|slot]      Power-cycle the FPGA (PC6) and load a design, no unit power cycle\r\n"), \
    CMD_V("fpga configbb", cmd_fpga_configbb, SC_EXACT, \
          "fpga configbb                   Run GPIO bit-bang SSPI config on demand (H7 step 1)\r\n"), \
    CMD_V("fpga scope cal", cmd_fpga_scope_cal, SC_EXACT, \
          "fpga scope cal                  Dump the compiled vertical cal table (mV/count, V/div, tier)\r\n"), \
    CMD_A("fpga scope freq", cmd_fpga_scope_freq, 0, \
          "fpga scope freq [n]             Run the shipped frequency estimator n times, with diagnostics\r\n"), \
    CMD_A("fpga scope zero", cmd_fpga_scope_zero, 0, \
          "fpga scope zero                 Record mean vs each channel's 0 V, and the offset DAC in force\r\n"), \
    CMD_A("fpga scope coupling", cmd_fpga_scope_coupling, 0, \
          "fpga scope coupling             Coupling label vs PD12/PD13 relay pins (read-only)\r\n"), \
    CMD_A("fpga pollgap", cmd_fpga_pollgap, 0, \
          "fpga pollgap [ms]               Poll cadence after the poll start (EXP-54; default 30)\r\n"), \
    CMD_A("fpga acqbr", cmd_fpga_acqbr, 0, \
          "fpga acqbr [0-7|off]            SPI3 clock the acq task sets before each pair (EXP-46)\r\n"), \
    CMD_A("fpga usart", cmd_fpga_usart, 0, \
          "fpga usart [on|off]             Bring USART2 up post-config; show CTRL1+RX\r\n"), \
    CMD_A("fpga autowait", cmd_fpga_autowait, 0, \
          "fpga autowait [ms]              AUTO edge-wait budget before fallback read (EXP-42)\r\n"), \
    CMD_A("fpga meter reinit", cmd_fpga_meter_reinit, 0, \
          "fpga meter reinit [submode]     Re-apply meter frontend + FPGA cfg\r\n"), \
    CMD_V("lcd colcheck", cmd_lcd_colcheck, SC_EXACT, \
          "lcd colcheck                    Column write against the general stream: same screenshot shadow?\r\n"), \
    CMD_A("lcd scanline", cmd_lcd_scanline, 0, \
          "lcd scanline                    EXPERIMENT: read the panel's scan line, time its frame\r\n"), \
    CMD_A("lcd te scan", cmd_lcd_te_scan, 0, \
          "lcd te scan MS                  EXPERIMENT: is the panel's TE output wired to an MCU pin?\r\n"), \
    CMD_A("lcd scroll", cmd_lcd_scroll, 0, \
          "lcd scroll TFA VSA BFA SSA|off  EXPERIMENT: the panel's hardware scroll (glass only)\r\n"), \
    CMD_A("lcd readback", cmd_lcd_readback, 0, \
          "lcd readback [x y w h] [w565]   EXPERIMENT: read the panel's RAM back, CRC vs the shadow\r\n"), \
    CMD_A("lcd", cmd_lcd, 0, \
          "lcd [selftest]                  Panel health; selftest resets the ST7789 to test recovery\r\n"), \
    CMD_A("spi3 xfer", cmd_spi3_xfer, SC_SPI3, \
          "spi3 xfer <hex...>              Send arbitrary MOSI bytes, dump MISO\r\n"), \
    CMD_A("spi3 seq", cmd_spi3_seq, SC_SPI3, \
          "spi3 seq <b..> | <b..>          xfer w/ mid-sequence CS pulse at '|'\r\n"), \
    CMD_A("spi3 read", cmd_spi3_read, 0, \
          "spi3 read [len]                 Raw SPI3 read + hex dump\r\n"), \
    CMD_V("spi3 frame", cmd_spi3_frame, SC_EXACT, \
          "spi3 frame                      Coherent CH1+CH2 snapshot + gen + render trig offset\r\n"), \
    CMD_V("spi3 acqread", cmd_spi3_acqread, SC_EXACT | SC_SPI3, \
          "spi3 acqread                    Read CH1/CH2 via real 0x04/0x05 protocol\r\n"), \
    CMD_A("spi3 opread", cmd_spi3_opread, 0, \
          "spi3 opread <op> [len [dump]]   One read window under any opcode + stats\r\n"),

#else  /* !FEATURE_BENCHSHELL: not in this build */

#define BENCHSHELL_SHELL_ROWS            /* no rows */

#endif /* FEATURE_BENCHSHELL */
#endif /* SHELL_BENCH_H */
