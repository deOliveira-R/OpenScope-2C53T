/*
 * OpenScope 2C53T - debug shell, public interface
 *
 * The shell runs over USB CDC (usb_cdc.c) and over RTT on the SWD wires.
 * Connect via: screen /dev/tty.usbmodem* 115200 (the baud rate is ignored).
 * `help` lists the commands of the build; the table is in shell.c.
 */

#ifndef USB_DEBUG_H
#define USB_DEBUG_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Start the USB device:
 *   - USB clock from HICK 48 MHz, trimmed by ACC against the host's SOF
 *   - USB peripheral clock and interrupt on
 *   - device core initialised as the plain CDC device, pull-up on
 * Call after system_clock_config() and before the scheduler starts.
 * Does nothing in the emulator build.
 */
void usb_debug_init(void);

/*
 * Create the shell task (not in the emulator build).
 * Call after usb_debug_init() and before vTaskStartScheduler().
 */
void usb_debug_create_task(void);
void usb_debug_pinlog_tick(void);   /* 1 s timer: the toolkit's pin log (pinlog, src/re/) */

/*
 * Send a formatted string to the console (RTT, and CDC when configured).
 * Output is cut to 255 bytes. Returns vsnprintf's result: the untruncated
 * length, whether or not anything was sent. There is no lock: callers are
 * the shell task's commands.
 */
int usb_debug_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/*
 * True when the host has configured the USB device (always false in the
 * emulator build).
 */
bool usb_debug_connected(void);

/* USB Sharing (USB_MSC=1 builds only): ask the shell task to re-enumerate as
 * CDC + read-only gallery drive (on) or CDC only (off); safe from any task.
 * usb_share_wanted() is the request still pending, else what the mode state
 * machine holds (off after an automatic fallback to CDC-only). See
 * usb_mode_sm.h. */
void usb_share_request(bool on);
bool usb_share_wanted(void);
/* Settings row: re-insert a drive the host ejected, else toggle sharing;
 * and its label: "Off", "On" or "Ejected". */
void usb_share_toggle(void);
const char *usb_share_label(void);

#endif /* USB_DEBUG_H */
