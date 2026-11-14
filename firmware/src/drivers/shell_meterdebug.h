/*
 * OpenScope 2C53T - the meter diagnostics' rows and handlers (FEATURE_METERDEBUG)
 *
 * The feature's contract with the shell core, after the template
 * src/stream/stream.h: usb_debug.c includes this header and places
 * METERDEBUG_SHELL_ROWS in the command table with no #if. With
 * FEATURE_METERDEBUG 0 the row macro is empty (`help` does not list the
 * commands, they answer "Unknown command" like any typo) and the handlers
 * are not compiled. CMD_A / CMD_V / SC_* are the table's own macros
 * (usb_debug.c); dispatch is by longest match, so where the rows sit in the
 * table only decides where `help` prints them.
 */
#ifndef SHELL_METERDEBUG_H
#define SHELL_METERDEBUG_H

#if FEATURE_METERDEBUG

void cmd_meter_hdr(const char *args);
void cmd_meter_fast(const char *args);
void cmd_meter_keepalive(const char *args);
void cmd_meter_overlay(const char *args);
void cmd_meter_beep(const char *args);
void cmd_meter_dump(const char *args);
void cmd_meter_autoscan(const char *args);
void cmd_meter_trace(void);
void cmd_meter_frontend(void);
void cmd_meter_probe_tail(const char *args);
void cmd_meter_mux_stream(const char *args);
void cmd_meter_mux_arms(const char *args);
void cmd_meter_boot_sequence(const char *args);
void cmd_meter_pc11_timing(const char *args);
void cmd_meter_stream(const char *args);
void cmd_meter_adc_snapshot(void);
void cmd_meter_wave_args(const char *args);

#define METERDEBUG_SHELL_ROWS \
    CMD_A("meter hdr", cmd_meter_hdr, 0, \
          "meter hdr [on|off]              Meter TX header AA 55 (default) vs 00 00 (EXP-25) + echo ladder\r\n"), \
    CMD_A("meter fast", cmd_meter_fast, 0, \
          "meter fast [on|off]             One-word function switching (stock) vs PC11 restart\r\n"), \
    CMD_A("meter keepalive", cmd_meter_keepalive, 0, \
          "meter keepalive [on|off]        4 Hz unobeyed poll traffic (stock sends none)\r\n"), \
    CMD_A("meter overlay", cmd_meter_overlay, 0, \
          "meter overlay                   Toggle the meter debug overlay (was short SAVE)\r\n"), \
    CMD_A("meter beep", cmd_meter_beep, 0, \
          "meter beep [on|off|src <c|d|e|off> [rise|fall|both] [pu|pd]|tick <ms>]  SoC-edge buzzer\r\n"), \
    CMD_A("meter dump", cmd_meter_dump, 0, \
          "meter dump [delay_ms]           Show parsed DMM/UI/raw frame state\r\n"), \
    CMD_A("meter autoscan", cmd_meter_autoscan, 0, \
          "meter autoscan [settle_ms]      Probe DMM submodes and select best live mode\r\n"), \
    CMD_V("meter trace", cmd_meter_trace, SC_EXACT, \
          "meter trace                     One machine-readable DMM producer record\r\n"), \
    CMD_V("meter frontend", cmd_meter_frontend, SC_EXACT, \
          "meter frontend                  Show DMM analog frontend GPIO state\r\n"), \
    CMD_A("meter probe-tail", cmd_meter_probe_tail, 0, \
          "meter probe-tail [auto|07|0a]   Override stock PC7-gated DMM tail for diagnostics\r\n"), \
    CMD_A("meter boot-sequence", cmd_meter_boot_sequence, 0, \
          "meter boot-sequence [ms]        Replay stock DMM boot word order + trace\r\n"), \
    CMD_A("meter pc11-timing", cmd_meter_pc11_timing, 0, \
          "meter pc11-timing [lo hi]       Probe DMM PC11 gate timing + trace\r\n"), \
    CMD_A("meter mux-arms", cmd_meter_mux_arms, SC_NEEDARGS, \
          "meter mux-arms <ce> <ab> [ms]   Apply stock mux arms, poll, trace\r\n"), \
    CMD_A("meter mux-stream", cmd_meter_mux_stream, 0, \
          "meter mux-stream [count] [ms]   Stream DMM frames plus frontend GPIOs\r\n"), \
    CMD_A("meter stream", cmd_meter_stream, 0, \
          "meter stream [count] [delay_ms] Print compact DMM frame stream\r\n"), \
    CMD_V("meter adc-snapshot", cmd_meter_adc_snapshot, SC_EXACT, \
          "meter adc-snapshot              Show read-only DMM waveform sampler state\r\n"), \
    CMD_A("meter wave", cmd_meter_wave_args, 0, \
          "meter wave                      Show DMM voltage waveform sample stats\r\n" "meter wave reset                Reset DMM waveform diagnostics\r\n" "meter wave sampler [on|off]     Enable/disable experimental SPI3 sampler\r\n" "meter wave path [direct|preacq] Get/set DMM waveform SPI path\r\n" "meter wave selector [auto|N]    DMM wave selector byte\r\n" "meter wave preacq [auto|N]      DMM wave pre-acq byte\r\n"),

#else  /* !FEATURE_METERDEBUG: not in this build */

#define METERDEBUG_SHELL_ROWS            /* no rows */

#endif /* FEATURE_METERDEBUG */
#endif /* SHELL_METERDEBUG_H */
