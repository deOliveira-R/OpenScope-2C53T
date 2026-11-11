/*
 * usb_composite — OpenScope's USB device: the CDC debug shell plus a
 * read-only mass-storage LUN (parity C16, "USB Sharing").
 *
 * Interfaces:   0 CDC comm (INT IN 0x82)   } one IAD, exactly the endpoints,
 *               1 CDC data (0x81 / 0x01)   } addresses and class code the
 *                                          } CDC-only build had
 *               2 MSC, SCSI transparent, bulk-only (0x83 / 0x03)
 * VID:PID stays 2E3C:5740 (scripts/iap_flash.py, flash_preflight.py and
 * openscope_live_debug.py key "app running" on it); bcdDevice goes 2.00 ->
 * 2.01 so a host that caches descriptors per VID/PID/revision (Windows) sees
 * a new device instead of the old CDC-only configuration. The serial string
 * is computed from the MCU UID exactly as the AT32 CDC descriptor does, so
 * macOS keeps naming the port /dev/cu.usbmodem<serial>... .
 *
 * The CDC half is the AT32 library's own cdc_class.c, called through its
 * handler table: our handler's pdata is the library's cdc_struct, so
 * usb_vcp_get_rxdata()/usb_vcp_send_data() and usb_debug.c work unchanged.
 */
#ifndef USB_COMPOSITE_H
#define USB_COMPOSITE_H

#include "usbd_core.h"

#define USB_COMP_CDC_COMM_IF   0u
#define USB_COMP_CDC_DATA_IF   1u
#define USB_COMP_MSC_IF        2u
#define USB_COMP_MSC_IN_EPT    0x83u
#define USB_COMP_MSC_OUT_EPT   0x03u

extern usbd_class_handler usb_composite_class_handler;
extern usbd_desc_handler  usb_composite_desc_handler;

#endif /* USB_COMPOSITE_H */
