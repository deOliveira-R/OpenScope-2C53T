#ifndef SHELL_PERSIST_H
#define SHELL_PERSIST_H
/*
 * Shell "persist" switch: whether a shell command asks for a settings flush.
 *
 * The shell dispatcher requests a settings flush after every command, because
 * no key press stands behind a shell change. A bench sweep (hundreds of
 * timebase, range or DAC steps) would then append hundreds of settings records
 * to flash. `persist off` stops those requests for the session; key presses
 * still save as before, and so does `persist on`. The flag lives in RAM and is
 * on at every boot, so it can never leave a unit that silently stops saving.
 * Pure state, no hardware; host test: tests/test_shell_persist.c.
 */
#include <stdbool.h>
#include <stdint.h>

/* Back to the boot state: on, nothing skipped. */
void shell_persist_reset(void);

/* Turn flushing after shell commands on or off. Turning it on clears the
 * skipped count (the caller then flushes once). */
void shell_persist_set(bool on);

bool shell_persist_enabled(void);

/* Called by the dispatcher after each command: true = request a flush.
 * When off it returns false and counts the skipped request. */
bool shell_persist_after_command(void);

/* Flush requests skipped since persist was last turned off. */
uint32_t shell_persist_skipped(void);

#endif
