/*
 * OpenScope 2C53T - binary stream over the shell's CDC channel (FEATURE_STREAM)
 *
 *   stream on [scope] [meter] [props] [fast]   start (no names = all three)
 *   stream off                                 stop (a last STATS frame first)
 *   stream status                              counters, one key=value line
 *
 * The sender runs on the shell task (usb_dbg, prio 2), between commands: the
 * shell loop calls stream_poll() once per pass, so a frame is never written
 * while a command runs and a command's reply is never split by one. Frame
 * format and rules: docs/stream_protocol.md.
 *
 * Everything here exists only when FEATURE_STREAM is 1 (bench image
 * app-coldtrace-meter). Hooks in shared files (all under #if FEATURE_STREAM):
 * usb_debug.c includes this header, defines the two port functions below,
 * adds the `stream` table row, and calls stream_poll() in its task loop.
 */
#ifndef STREAM_H
#define STREAM_H

#if FEATURE_STREAM

#include <stdbool.h>
#include <stdint.h>

/* Shell task only. True when a frame went out this pass (the loop then
 * skips its idle wait, so a backlog drains between RX polls). */
bool stream_poll(void);
void cmd_stream(const char *args);

/* Port into the CDC IN path, defined in usb_debug.c next to the frozen
 * usb_send_bytes() (additions only; shell task only).
 *   ready: the previous IN packet has left (heal: also recover a lost
 *          completion, as the write path does after 2 ms);
 *   write: usb_send_bytes(); false if it dropped data (host not reading). */
bool usb_debug_stream_tx_ready(bool heal);
bool usb_debug_stream_write(const uint8_t *data, uint16_t len);

#endif /* FEATURE_STREAM */
#endif /* STREAM_H */
