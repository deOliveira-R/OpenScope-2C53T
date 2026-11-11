/*
 * usb_msc — bulk-only transport for OpenScope's read-only gallery LUN
 * (parity C16, "USB Sharing"). The SCSI decisions are msc_scsi.c, the volume
 * is gallery_vfat.c; this file moves bytes and owns the one sector buffer.
 *
 * CONTEXTS. The USB interrupt (priority 6) decodes every CBW and answers all
 * commands that need no flash (INQUIRY, TEST UNIT READY, READ CAPACITY, MODE
 * SENSE, REQUEST SENSE, write refusals) on the spot. A READ is latched and
 * served sector by sector from task context by usb_msc_poll(), because the
 * W25Q sits behind flash_fs's mutex, which an ISR cannot take. The ISR wakes
 * the polling task with a task notification, so a read costs no 10 ms poll
 * latency per sector.
 *
 * RAM: one 512-byte sector buffer (it also receives the CBW and absorbs any
 * data a host writes) plus ~150 bytes of state. One sector is the smallest
 * unit READ(10) addresses and the unit gallery_vfat computes, so a smaller
 * buffer would only multiply task round trips; a larger one buys nothing for
 * a 512 KB volume that holds nine 38 KB files. The library's BOT layer would
 * have taken 4096 bytes.
 */
#ifndef USB_MSC_H
#define USB_MSC_H

#include <stdbool.h>
#include <stdint.h>

#include "usbd_core.h"

#define USB_MSC_MPS   64u

/* Sharing state at boot. Off by default, like stock (the user enters USB
 * Sharing explicitly), and because a host shows a "not ejected" warning
 * every time a shared unit is unplugged or reflashed. */
#ifndef USB_MSC_SHARE_DEFAULT
#define USB_MSC_SHARE_DEFAULT 0
#endif

/* Composite class hooks (usb_composite.c, interrupt context). */
void         usb_msc_class_init(void *udev);
void         usb_msc_class_clear(void *udev);
usb_sts_type usb_msc_setup(void *udev, usb_setup_type *setup);
void         usb_msc_in_done(void *udev);
void         usb_msc_out_done(void *udev);
void         usb_msc_bus_reset(void);

/* Task side. usb_msc_poll() returns true if it did anything (the caller then
 * skips its idle sleep). The notify task is woken from the ISR on work. */
void usb_msc_set_notify_task(void *task_handle);
bool usb_msc_poll(void *udev);

/* Medium present or not. The glue (usb_debug.c) inserts it when the
 * composite device is attached for "USB Sharing"; a host eject removes it. */
void usb_msc_set_shared(bool on);
bool usb_msc_shared(void);

typedef struct {
    bool     shared;
    bool     configured;     /* the host selected the configuration         */
    uint8_t  state;          /* BOT state (usb_msc.c)                         */
    uint8_t  files;          /* shots listed on the volume                    */
    uint32_t blocks;         /* volume size in 512 B blocks                   */
    uint32_t commands;       /* CBWs executed                                 */
    uint32_t reads;          /* READ commands                                 */
    uint32_t sectors;        /* sectors served from task context              */
    uint32_t io_errors;      /* sector reads that failed                      */
    uint32_t bad_cbw;        /* invalid CBWs (both pipes stalled)             */
    uint32_t resets;         /* bulk-only mass storage resets                 */
    uint32_t writes_refused; /* WRITE/FORMAT commands refused                 */
    uint32_t rescans;        /* gallery index rebuilds                        */
    uint32_t last_lba;
    uint8_t  last_op;
} usb_msc_stats_t;

void usb_msc_get_stats(usb_msc_stats_t *out);

#endif /* USB_MSC_H */
