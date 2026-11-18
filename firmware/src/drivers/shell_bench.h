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

/* The 1 s timer hook of `pinlog` (usb_debug_pinlog_tick() forwards to it). */
void benchshell_pinlog_tick(void);

void cmd_usart_tx(const char *args);
void cmd_usart_raw(const char *args);
void cmd_fpga_frame(const char *args);
void cmd_bench_snapshot(void);
void cmd_bench_restore(void);
void cmd_gpio_mode(const char *args);
void cmd_gpio_set(const char *args);
void cmd_buzzer_test(const char *args);
void cmd_gpio_read(const char *args);
void cmd_gpio_scan(void);
void cmd_mem_read(const char *args);
void cmd_mem_write(const char *args);
void cmd_flash_diag(void);
void cmd_flash_wtest(const char *args);
void cmd_scope_trig(const char *args);
void cmd_scope_trig2(const char *args);
void cmd_fpga_cmd(const char *args);
void cmd_fpga_usart(const char *args);
void cmd_fpga_autowait(const char *args);
void cmd_fpga_pollgap(const char *args);
void cmd_fpga_scope_coupling(const char *args);
void cmd_fpga_acqbr(const char *args);
void cmd_fpga_scope_reinit(void);
void cmd_fpga_scope_zero(const char *args);
void cmd_fpga_scope_freq(const char *args);
void cmd_fpga_scope_cal(void);
void cmd_fpga_diag_clear(void);
void cmd_fpga_bus_release(void);
void cmd_fpga_bus_reacquire(void);
void cmd_fpga_configbb(void);
void cmd_fpga_slot(const char *args);
void cmd_fpga_reload(const char *args);
void cmd_fpga_stock_diag(void);
void cmd_fpga_stock_clear(void);
void cmd_fpga_stock_set(const char *args);
void cmd_fpga_stock_preset(const char *args);
void cmd_fpga_stock_base2(void);
void cmd_fpga_stock_state5(const char *args);
void cmd_fpga_stock_state6(const char *args);
void cmd_fpga_stock_reenter(void);
void cmd_fpga_stock_prev(void);
void cmd_fpga_stock_next(void);
void cmd_fpga_stock_select(void);
void cmd_fpga_stock_toggle(void);
void cmd_fpga_stock_commit(void);
void cmd_fpga_stock_consume(void);
void cmd_fpga_stock_bridge_fixed(void);
void cmd_fpga_stock_bridge_dynamic(const char *args);
void cmd_fpga_wire_words(const char *args);
void cmd_fpga_wire_entry(const char *args);
void cmd_fpga_wire_scope(const char *args);
void cmd_fpga_meter_reinit(const char *args);
void cmd_fpga_scope_wake(void);
void cmd_fpga_scope_acqmode(void);
void cmd_fpga_scope_beat(const char *args);
void cmd_fpga_scope_entry(const char *args);
void cmd_fpga_scope_timing(const char *args);
void cmd_fpga_scope_trig(const char *args);
void cmd_spi3_read(const char *args);
void cmd_spi3_frame(void);
void cmd_pinlog(const char *args);
void cmd_lcd(const char *args);
void cmd_lcd_readback(const char *args);
void cmd_spi3_acqtest(void);
void cmd_spi3_h2txdiag(void);
void cmd_spi3_xfer(const char *args);
void cmd_spi3_acqread(void);
void cmd_spi3_opread(const char *args);
void cmd_spi3_opsweep(const char *args);
void cmd_spi3_gowin(void);
void cmd_spi3_edgecap(const char *args);
void cmd_fpga_selftest(void);
void cmd_spi3_scopetest(const char *args);
void cmd_spi3_armtest(const char *args);
void cmd_fpga_reinit(const char *args);
void cmd_spi3_seq(const char *args);
void cmd_spi3_stock_readback(void);
void cmd_spi3_probe(void);
#if defined(FPGA_ALT_BITSTREAM)
void cmd_fpga_dbgclk(const char *args);
void cmd_fpga_dbgarm(const char *args);
#define BENCHSHELL_ALT_BITSTREAM_ROWS \
    CMD_A("fpga dbgclk", cmd_fpga_dbgclk, 0, \
          "fpga dbgclk <count> [half_us]   debugclk image: N rising edges on PC6 (=N samples)\r\n"), \
    CMD_A("fpga dbgarm", cmd_fpga_dbgarm, 0, \
          "fpga dbgarm [low_ms]            debugclk image: re-arm via PB11 low->high (run_enable)\r\n"),
#else
#define BENCHSHELL_ALT_BITSTREAM_ROWS
#endif

#define BENCHSHELL_SHELL_ROWS \
    CMD_A("usart raw", cmd_usart_raw, SC_NEEDARGS, \
          "usart raw <10 hex bytes>       Send raw 10-byte USART frame\r\n" "  e.g.: usart raw 00 00 00 0B 01 00 00 00 00 0B\r\n"), \
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
    CMD_A("fpga cmd", cmd_fpga_cmd, SC_NEEDARGS, \
          "fpga cmd <hi> <lo>              Send FPGA command bytes (HEX)\r\n" "  e.g.: fpga cmd 05 14 (sends 0x05 0x14)\r\n" "        fpga cmd 0x0509 (sends 0x05 0x09)\r\n"), \
    CMD_A("fpga frame", cmd_fpga_frame, SC_NEEDARGS, \
          "fpga frame <hi> <lo> [p1..p5 [ck]]  Build/send full 10-byte frame (HEX)\r\n" "  e.g.: fpga frame 00 0B 01 00 00 00 00\r\n"), \
    CMD_V("fpga diag clear", cmd_fpga_diag_clear, SC_EXACT, \
          "fpga diag clear                 Clear FPGA bench counters/state\r\n"), \
    CMD_V("fpga selftest", cmd_fpga_selftest, SC_EXACT, \
          "fpga selftest                   Precondition table: SPI3 clk, IDCODE anchor,\r\n" "                                USART2 state, every driven pin's mode+level\r\n"), \
    BENCHSHELL_ALT_BITSTREAM_ROWS \
    CMD_V("fpga busrelease", cmd_fpga_bus_release, SC_EXACT, \
          "fpga busrelease                 Hand SPI3 to an external master (one-way, EXPERIMENTAL)\r\n"), \
    CMD_V("fpga busreacquire", cmd_fpga_bus_reacquire, SC_EXACT, \
          "fpga busreacquire               Take SPI3 back after busrelease (H7 capture prep)\r\n"), \
    CMD_A("fpga slot", cmd_fpga_slot, 0, \
          "fpga slot [info|put <len> <crc32hex> <name> [force]|data <off> <hex>|pos|end|boot on|off|clear]\r\n" "                                FPGA design stored on the flash (scripts/fpga_put.py)\r\n"), \
    CMD_A("fpga reload", cmd_fpga_reload, 0, \
          "fpga reload [builtin|slot]      Power-cycle the FPGA (PC6) and load a design, no unit power cycle\r\n"), \
    CMD_V("fpga configbb", cmd_fpga_configbb, SC_EXACT, \
          "fpga configbb                   Run GPIO bit-bang SSPI config on demand (H7 step 1)\r\n"), \
    CMD_V("fpga stock diag", cmd_fpga_stock_diag, SC_EXACT, \
          "fpga stock diag                Show stock-state bench shadow\r\n"), \
    CMD_V("fpga stock clear", cmd_fpga_stock_clear, SC_EXACT, \
          "fpga stock clear               Reset stock-state bench shadow\r\n"), \
    CMD_A("fpga stock set", cmd_fpga_stock_set, SC_NEEDARGS, \
          "fpga stock set <9 bytes>       Set F68/F69/F6A/F6B/E1A/E1B/E1C/E1D/355\r\n"), \
    CMD_A("fpga stock preset", cmd_fpga_stock_preset, SC_NEEDARGS, \
          "fpga stock preset <4|5 bytes>  Set F68/F69/F6A/F6B [355]\r\n"), \
    CMD_V("fpga stock base2", cmd_fpga_stock_base2, SC_EXACT, \
          "fpga stock base2               Seed visible state 2 scope posture\r\n"), \
    CMD_A("fpga stock state5", cmd_fpga_stock_state5, 0, \
          "fpga stock state5 [E1B] [E1D]  Seed visible state 5 editor posture\r\n"), \
    CMD_A("fpga stock state6", cmd_fpga_stock_state6, 0, \
          "fpga stock state6 [E1B] [E1D]  Seed visible state 6 pre-entry posture\r\n"), \
    CMD_V("fpga stock prev", cmd_fpga_stock_prev, SC_EXACT, \
          "fpga stock prev                Drive stock-like adjust-prev family\r\n"), \
    CMD_V("fpga stock next", cmd_fpga_stock_next, SC_EXACT, \
          "fpga stock next                Drive stock-like adjust-next family\r\n"), \
    CMD_V("fpga stock select", cmd_fpga_stock_select, SC_EXACT, \
          "fpga stock select              Stage single detail selection\r\n"), \
    CMD_V("fpga stock toggle", cmd_fpga_stock_toggle, SC_EXACT, \
          "fpga stock toggle              Toggle staged detail bitmap\r\n"), \
    CMD_V("fpga stock commit", cmd_fpga_stock_commit, SC_EXACT, \
          "fpga stock commit              Walk E1C 0->2->1->0x2B commit path\r\n"), \
    CMD_V("fpga stock consume", cmd_fpga_stock_consume, SC_EXACT, \
          "fpga stock consume             Consume packed state-9 preset path\r\n"), \
    CMD_V("fpga stock bridge fixed", cmd_fpga_stock_bridge_fixed, SC_EXACT, \
          "fpga stock bridge fixed        Probe post-13/14 fixed 0x0501 path\r\n"), \
    CMD_A("fpga stock bridge dynamic", cmd_fpga_stock_bridge_dynamic, 0, \
          "fpga stock bridge dynamic [ch1|ch2|both]  Probe post-13/14 0x050x path\r\n"), \
    CMD_V("fpga stock reenter", cmd_fpga_stock_reenter, SC_EXACT, \
          "fpga stock reenter             Re-enter scope path with staged shadow\r\n"), \
    CMD_A("fpga wire words", cmd_fpga_wire_words, SC_NEEDARGS, \
          "fpga wire words <w...>         Send final 16-bit wire words directly\r\n"), \
    CMD_A("fpga wire entry", cmd_fpga_wire_entry, 0, \
          "fpga wire entry [ch1|ch2|both] Send candidate scope-entry wire-word bank\r\n"), \
    CMD_A("fpga wire scope", cmd_fpga_wire_scope, 0, \
          "fpga wire scope [ch1|ch2|both] Wire-word entry + runtime scope blocks\r\n"), \
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
    CMD_V("fpga scope reinit", cmd_fpga_scope_reinit, SC_EXACT, \
          "fpga scope reinit               Re-apply scope frontend + FPGA cfg\r\n"), \
    CMD_A("fpga meter reinit", cmd_fpga_meter_reinit, 0, \
          "fpga meter reinit [submode]     Re-apply meter frontend + FPGA cfg\r\n"), \
    CMD_V("fpga scope wake", cmd_fpga_scope_wake, SC_EXACT, \
          "fpga scope wake                 Meter wake preamble then scope cfg\r\n"), \
    CMD_V("fpga scope acqmode", cmd_fpga_scope_acqmode, SC_EXACT, \
          "fpga scope acqmode              Send stock-like 0x20/0x21 block\r\n"), \
    CMD_A("fpga scope beat", cmd_fpga_scope_beat, 0, \
          "fpga scope beat [count] [ms]    Send stock-like cmd-3 heartbeat(s)\r\n"), \
    CMD_A("fpga scope entry", cmd_fpga_scope_entry, SC_NEEDARGS, \
          "fpga scope entry <8 bytes>      Reset + send 0x01,0B..11 params\r\n"), \
    CMD_A("fpga scope timing", cmd_fpga_scope_timing, SC_NEEDARGS, \
          "fpga scope timing <5 bytes>     Send 0x20,0x21,0x26..0x28 params\r\n"), \
    CMD_A("fpga scope trig", cmd_fpga_scope_trig, SC_NEEDARGS, \
          "fpga scope trig <4 bytes>       Send 0x07/0x0A,0x16..0x19\r\n"), \
    CMD_A("pinlog", cmd_pinlog, 0, \
          "pinlog [start]                  Bench: log all GPIO inputs + battery ADC once a second\r\n"), \
    CMD_A("lcd readback", cmd_lcd_readback, 0, \
          "lcd readback [x y w h] [w565]   EXPERIMENT: read the panel's RAM back, CRC vs the shadow\r\n"), \
    CMD_A("lcd", cmd_lcd, 0, \
          "lcd [selftest]                  Panel health; selftest resets the ST7789 to test recovery\r\n"), \
    CMD_A("fpga reinit", cmd_fpga_reinit, SC_SPI3, \
          "fpga reinit [br][gap][close][f0|1|2][u<ms>][a-e<pin>][s2|sd[h|l]][tc<n>] Replay cfg\r\n" "    f=prelude frame: 0 split(stock) 1 combined 2 merge15+3B; u=pre-upload gap; k<br>=cmd-phase clk div; tc<n>=trailing clocks\r\n" "    pe=probe SYSTEM_EDIT_MODE after 0x15 (STATUS@/256); rl=send 0x3C RELOAD before prelude; reports 0x41 STATUS\r\n"), \
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
          "spi3 opread <op> [len [dump]]   One read window under any opcode + stats\r\n"), \
    CMD_A("spi3 opsweep", cmd_spi3_opsweep, 0, \
          "spi3 opsweep [a b [len]]        Sweep read opcodes (BSRAM_1/2 hunt, Step 0b)\r\n"), \
    CMD_A("spi3 armtest", cmd_spi3_armtest, SC_SPI3, \
          "spi3 armtest [pb11|pc6]         Pulse FPGA run/re-arm pin, re-cfg, acqread\r\n"), \
    CMD_V("spi3 gowin", cmd_spi3_gowin, SC_EXACT | SC_SPI3, \
          "spi3 gowin                      Read+decode Gowin ID/USERCODE/STATUS regs\r\n"), \
    CMD_A("spi3 edgecap", cmd_spi3_edgecap, SC_SPI3, \
          "spi3 edgecap [reps]             EXP-37: AF vs bit-bang 0x15 frames for LA edge-diff\r\n"), \
    CMD_A("spi3 scopetest", cmd_spi3_scopetest, SC_SPI3, \
          "spi3 scopetest [bank]           Full scope seq: USART cfg->PC0->0x04/05 read\r\n"), \
    CMD_V("spi3 acqtest", cmd_spi3_acqtest, SC_EXACT | SC_SPI3, \
          "spi3 acqtest                    Decomposer Phase 20 validation test\r\n"), \
    CMD_V("spi3 stock-readback", cmd_spi3_stock_readback, SC_EXACT | SC_SPI3, \
          "spi3 stock-readback             Stock case-8 SPI3 readback; not DMM proof\r\n"), \
    CMD_V("spi3 h2txdiag", cmd_spi3_h2txdiag, SC_EXACT | SC_SPI3, \
          "spi3 h2txdiag                   Replay H2 TX + sample MISO; no ACK/apply proof\r\n"), \
    CMD_V("spi3 h2verify", cmd_spi3_h2txdiag, SC_EXACT | SC_SPI3, \
          NULL /* alias */), \
    CMD_V("spi3 probe", cmd_spi3_probe, SC_EXACT | SC_SPI3, \
          "spi3 probe                      Bit-bang MISO probe; pokes SPI3 registers directly\r\n"),

#else  /* !FEATURE_BENCHSHELL: not in this build */

static inline void benchshell_pinlog_tick(void) { }
#define BENCHSHELL_SHELL_ROWS            /* no rows */

#endif /* FEATURE_BENCHSHELL */
#endif /* SHELL_BENCH_H */
