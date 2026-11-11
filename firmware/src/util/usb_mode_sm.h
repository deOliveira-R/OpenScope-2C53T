/*
 * usb_mode_sm — which USB device OpenScope presents, and the safety net
 * around switching it (pure logic; the glue is in usb_debug.c).
 *
 * The device boots as the plain CDC shell: byte for byte the USB_MSC=0
 * device (the library's cdc_desc.c), so a host that cannot handle the
 * composite device never loses the shell at boot. "USB Sharing" on (shell
 * `usb share on` or the Settings row) re-enumerates as CDC + read-only MSC:
 *
 *   CDC --request on--> DRAIN (let the shell's reply leave, 200 ms)
 *       --> DETACHED (pull-up off, USB_MODE_DETACH_MS so the host sees the
 *           unplug and tears the old device down)
 *       --> ATTACHING (composite descriptors, pull-up on)
 *       --> COMPOSITE when the host selects the configuration, or after
 *           USB_MODE_ATTACH_TIMEOUT_MS without it: FALLBACK -> detach and
 *           come back as CDC-only, and remember why (usb status says so).
 *
 * "usb share off" walks the same path back to CDC. A CDC re-attach that is
 * not configured in time (no host, cable out) is not an error: the device
 * stays CDC, which is what an unplugged unit is anyway.
 */
#ifndef USB_MODE_SM_H
#define USB_MODE_SM_H

#include <stdbool.h>
#include <stdint.h>

#define USB_MODE_DRAIN_MS            200u
#define USB_MODE_DETACH_MS           600u
#define USB_MODE_ATTACH_TIMEOUT_MS  3000u

typedef enum {
    USB_DEV_CDC = 0,          /* library CDC descriptors, VID:PID 2E3C:5740 bcd 2.00 */
    USB_DEV_COMPOSITE,        /* CDC + MSC (usb_composite.c), bcd 2.01               */
} usb_dev_kind_t;

typedef enum {
    USB_ST_STEADY = 0,        /* presenting `active`                        */
    USB_ST_DRAIN,             /* switch requested, reply still going out     */
    USB_ST_DETACHED,          /* pull-up off                                  */
    USB_ST_ATTACHING,         /* pull-up on as `active`, waiting for config   */
} usb_mode_state_t;

typedef enum {
    USB_ACT_NONE = 0,
    USB_ACT_DETACH,           /* pull-up off, stop the USB interrupt           */
    USB_ACT_ATTACH,           /* init the core as `active`, pull-up on         */
} usb_mode_action_t;

typedef enum {
    USB_RES_NONE = 0,         /* no switch attempted since boot                 */
    USB_RES_OK,               /* last switch reached the configured state        */
    USB_RES_FALLBACK,         /* composite not configured in time: back to CDC   */
    USB_RES_NO_HOST,          /* CDC re-attach not configured in time            */
} usb_mode_result_t;

typedef struct {
    usb_dev_kind_t    active;      /* what the core is (or is about to be) set up as */
    usb_dev_kind_t    want;        /* what the user asked for                         */
    usb_mode_state_t  state;
    uint32_t          t0;          /* entry time of the current state                  */
    usb_mode_result_t last;
    uint32_t          attach_ms;   /* how long the last attach took to configure       */
    uint32_t          switches;
    uint32_t          fallbacks;
    bool              fallback_pending;  /* after a fallback, do not retry by itself */
} usb_mode_sm_t;

void usb_mode_sm_init(usb_mode_sm_t *m);

/* Ask for sharing on (composite) or off (CDC). Takes effect on the next
 * step(). Asking for what is already active (or in progress) is a no-op. */
void usb_mode_sm_request(usb_mode_sm_t *m, bool share);

/* Advance: `now` in ms, `configured` = the core is in the configured state.
 * Returns the action the glue must perform now (at most one per call). */
usb_mode_action_t usb_mode_sm_step(usb_mode_sm_t *m, uint32_t now, bool configured);

/* True while the device is (or is switching to) the composite one. */
bool usb_mode_sm_sharing(const usb_mode_sm_t *m);

const char *usb_mode_state_str(usb_mode_state_t s);
const char *usb_mode_result_str(usb_mode_result_t r);

#endif /* USB_MODE_SM_H */
