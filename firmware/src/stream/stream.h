/*
 * OpenScope 2C53T - binary stream over the shell's CDC channel (FEATURE_STREAM)
 *
 *   stream on [scope] [meter] [props] [fast]   start (no names = all three)
 *   stream off                                 stop (a last STATS frame after the prompt)
 *   stream status                              counters, one key=value line
 *
 * The sender runs on the shell task (usb_dbg, prio 2), between commands: the
 * shell loop calls stream_poll() once per pass, so a frame is never written
 * while a command runs and a command's reply is never split by one. Frame
 * format and rules: docs/stream_protocol.md.
 *
 * THIS HEADER IS THE FEATURE'S WHOLE CONTRACT WITH THE CORE, and the template
 * for every other feature (firmware/features.mk):
 *
 *   - the core includes it and uses it with NO #if. With FEATURE_STREAM 0 the
 *     calls are inline no-ops (the compiler removes them), the row macro is
 *     empty (`help` does not list the command, `stream` answers "unknown
 *     command" like any typo), and the sources in src/stream/ are not compiled;
 *   - what the feature needs FROM the core (the two port functions below) is
 *     plain core code, always declared; unused, the linker drops it.
 *
 * So FEATURE_STREAM is tested here and in src/stream/ only. The core's hooks,
 * all in usb_debug.c: this include, the two port functions, STREAM_SHELL_ROWS
 * in the command table, stream_poll() in the task loop.
 */
#ifndef STREAM_H
#define STREAM_H

#include <stdbool.h>
#include <stdint.h>

/* Port into the CDC IN path, defined in usb_debug.c next to the frozen
 * usb_send_bytes() (additions only; shell task only).
 *   ready: the previous IN packet has left (heal: also recover a lost
 *          completion, as the write path does after 2 ms);
 *   write: usb_send_bytes(); false if it dropped data (host not reading). */
bool usb_debug_stream_tx_ready(bool heal);
bool usb_debug_stream_write(const uint8_t *data, uint16_t len);

#if FEATURE_STREAM

/* Shell task only. True when a frame went out this pass (the loop then
 * skips its idle wait, so a backlog drains between RX polls). */
bool stream_poll(void);
void cmd_stream(const char *args);

/* The feature's rows of the shell table (usb_debug.c places them; the row
 * keeps its place, so matching order does not change). */
#define STREAM_SHELL_ROWS \
    CMD_A("stream", cmd_stream, 0, \
          "stream on [scope] [meter] [props] [fast] | off | status   Binary frames on this channel (docs/stream_protocol.md)\r\n"),

#else  /* !FEATURE_STREAM: the feature is not in this build */

static inline bool stream_poll(void) { return false; }
#define STREAM_SHELL_ROWS            /* no rows */

#endif /* FEATURE_STREAM */
#endif /* STREAM_H */
