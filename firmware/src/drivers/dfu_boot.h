/*
 * OpenScope 2C53T - entry into our bootloader, and its boot handshake
 */

#ifndef DFU_BOOT_H
#define DFU_BOOT_H

/* Open the bootloader's update screen, power held. Does not return. */
void dfu_request_reboot(void);

/*
 * Restart the app through the bootloader, without its update screen: the power
 * latch stays held. For a crash, from thread mode. Does not return.
 */
void dfu_restart_app(void);

/*
 * Tell the bootloader this boot succeeded: its count of unconfirmed boots goes
 * back to zero. Call once, after the tasks are created.
 */
void boot_validate(void);

#endif /* DFU_BOOT_H */
