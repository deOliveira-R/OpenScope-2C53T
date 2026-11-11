/*
 * OpenScope 2C53T - USB Configuration
 *
 * Required by AT32 USB middleware (usbd_core.h includes this).
 * Defines endpoint buffer layout and helper function declarations.
 */

#ifndef __USB_CONF_H
#define __USB_CONF_H

#include <stddef.h>
#include "at32f403a_407.h"
#include "at32f403a_407_usb.h"

/* Maximum endpoint count */
#define USB_EPT_MAX_NUM                   8

/* Endpoint buffer addresses in USB packet memory (512 bytes total).
 * Each endpoint needs TX and RX buffer space. Layout:
 *   0x00-0x3F  reserved (packet memory table, 8 endpoints x 8 bytes)
 *   0x40-0x7F  EPT0 TX (64 bytes)
 *   0x80-0xBF  EPT0 RX (64 bytes)
 *   0xC0-0xFF  EPT1 TX (64 bytes) - CDC bulk IN
 *   0x100-0x13F EPT1 RX (64 bytes) - CDC bulk OUT
 *   0x140-0x17F EPT2 TX (64 bytes) - CDC interrupt IN (8-byte packets)
 *   0x180-0x1BF EPT3 TX (64 bytes) - MSC bulk IN   (usb_msc.c)
 *   0x1C0-0x1FF EPT3 RX (64 bytes) - MSC bulk OUT
 * EPT2 RX used to hold 0x180 "unused but allocated": EPT2 is IN-only (the
 * CDC notification endpoint), so that slot went to the MSC endpoint, which
 * is what lets the composite device fit the 512 bytes exactly. The CDC-only
 * build (USB_MSC=0) never opens EPT3. */
#define EPT0_TX_ADDR                      0x40
#define EPT0_RX_ADDR                      0x80
#define EPT1_TX_ADDR                      0xC0
#define EPT1_RX_ADDR                      0x100
#define EPT2_TX_ADDR                      0x140
#define EPT2_RX_ADDR                      0x00
#define EPT3_TX_ADDR                      0x180
#define EPT3_RX_ADDR                      0x1C0
#define EPT4_TX_ADDR                      0x00
#define EPT4_RX_ADDR                      0x00
#define EPT5_TX_ADDR                      0x00
#define EPT5_RX_ADDR                      0x00
#define EPT6_TX_ADDR                      0x00
#define EPT6_RX_ADDR                      0x00
#define EPT7_TX_ADDR                      0x00
#define EPT7_RX_ADDR                      0x00

/* ── Endpoint-register CTR race (AT32 library bug, fixed here) ──────────
 * The endpoint registers' TXTC (bit 7) and RXTC (bit 15) are rc_w0:
 * writing 0 clears them, writing 1 leaves them. The library clears one with
 *     USB->ept[n] &= 0xFF7F & USB_EPT_BIT_MASK      (USB_CLEAR_TXTC)
 *     USB->ept[n] &= 0x7FFF & USB_EPT_BIT_MASK      (USB_CLEAR_RXTC)
 * i.e. a read-modify-write that writes the OTHER flag back as it was read.
 * If the other transfer completes between the read and the write (EP1
 * carries both the CDC bulk IN 0x81 and OUT 0x01), its flag is written 0
 * and that completion is silently lost: an IN completion lost means
 * cdc_struct.g_tx_completed never comes back (every send then times out:
 * the bench's "logo upload stalls at 12 %, shell dead until replug"); an OUT
 * completion lost leaves the OUT endpoint NAKing forever (the shell goes
 * deaf). It takes simultaneous IN and OUT traffic on one endpoint, which the
 * shell's per-character echo during a fast upload provides.
 * ST's HAL writes 1 to the other flag; so do these. usbd_int.c (their only
 * user) sees them through usbd_core.h -> usb_conf.h; `make test-usb`
 * preprocesses usbd_int.c and checks that the fixed expansion is the one
 * that compiles. */
#ifdef USB_CLEAR_TXTC
#undef USB_CLEAR_TXTC
#endif
#ifdef USB_CLEAR_RXTC
#undef USB_CLEAR_RXTC
#endif
#define USB_CLEAR_TXTC(eptn) \
    (USB->ept[eptn] = (USB->ept[eptn] & 0xFF7Fu & USB_EPT_BIT_MASK) | USB_RXTC)
#define USB_CLEAR_RXTC(eptn) \
    (USB->ept[eptn] = (USB->ept[eptn] & 0x7FFFu & USB_EPT_BIT_MASK) | USB_TXTC)

/* WinUSB not needed */
#define USBD_SUPPORT_WINUSB               0

/* Delay helpers (implemented in usb_debug.c) */
void usb_delay_ms(uint32_t ms);
void usb_delay_us(uint32_t us);

#endif /* __USB_CONF_H */
