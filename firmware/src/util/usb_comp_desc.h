/*
 * usb_comp_desc — the composite (CDC + read-only MSC) device's descriptor
 * bytes, and a strict structural checker for any device + configuration
 * descriptor pair.
 *
 * Pure C, no AT32 headers, so tests/test_usb_desc.c compiles the SAME tables
 * the firmware sends and validates them on the host. usb_composite.c
 * _Static_asserts every number below against the AT32 library's own macros
 * (endpoint addresses, packet sizes, VID:PID, class codes), so the two cannot
 * drift apart silently.
 *
 * The tables live in RAM (not const), as the library's cdc_desc.c keeps its
 * own: the CDC-only build that works everywhere serves EP0 from SRAM, and the
 * composite one now does too, so the descriptor source is not a difference
 * between them.
 */
#ifndef USB_COMP_DESC_H
#define USB_COMP_DESC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define USB_COMP_VID              0x2E3Cu
#define USB_COMP_PID              0x5740u
#define USB_COMP_BCD_DEVICE       0x0201u   /* CDC-only build: 0x0200 */
#define USB_COMP_EP0_MPS          64u

#define USB_COMP_CDC_COMM_IF      0u
#define USB_COMP_CDC_DATA_IF      1u
#define USB_COMP_MSC_IF           2u
#define USB_COMP_NUM_IF           3u

#define USB_COMP_CDC_INT_EPT      0x82u
#define USB_COMP_CDC_IN_EPT       0x81u
#define USB_COMP_CDC_OUT_EPT      0x01u
#define USB_COMP_MSC_IN_EPT       0x83u
#define USB_COMP_MSC_OUT_EPT      0x03u

#define USB_COMP_CDC_INT_MPS      8u
#define USB_COMP_CDC_BULK_MPS     64u
#define USB_COMP_MSC_MPS          64u
#define USB_COMP_CDC_INT_INTERVAL 0xFFu
#define USB_COMP_CDC_BCD          0x0110u

#define USB_COMP_DEVICE_LEN       18u
#define USB_COMP_CONFIG_LEN \
    (9u + 8u + 9u + 5u + 5u + 4u + 5u + 7u + 9u + 7u + 7u + 9u + 7u + 7u)   /* 98 */

extern uint8_t usb_comp_device_desc[USB_COMP_DEVICE_LEN];
extern uint8_t usb_comp_config_desc[USB_COMP_CONFIG_LEN];

/* ── Checker ───────────────────────────────────────────────────────────── */

/* One packet-memory buffer the firmware gives an endpoint direction
 * (usb_conf.h EPTn_TX_ADDR / EPTn_RX_ADDR). */
typedef struct {
    uint8_t  ept_addr;      /* 0x80 | n for IN (TX), n for OUT (RX) */
    uint16_t offset;        /* byte offset in packet memory          */
    uint16_t size;          /* bytes reserved                        */
} usb_pma_buf_t;

typedef struct {
    uint16_t pma_size;          /* 512 on the AT32F403A (usbbufs = 0)   */
    uint16_t btable_size;       /* 8 endpoints x 8 bytes = 64           */
    const usb_pma_buf_t *bufs;  /* every allocated buffer, EP0 included */
    size_t   nbufs;
} usb_pma_layout_t;

/* Validate a device descriptor and its full configuration descriptor (cfg,
 * cfg_len = the bytes the device would send for wLength = 0xFFFF) as
 * strictly as the USB 2.0 spec, the IAD ECN, CDC 1.2 / PSTN and MSC BOT ask,
 * plus the packet-memory layout when pma != NULL. Returns true when clean;
 * otherwise writes the first problem into err. */
bool usb_desc_check(const uint8_t *dev, size_t dev_len,
                    const uint8_t *cfg, size_t cfg_len,
                    const usb_pma_layout_t *pma,
                    char *err, size_t err_len);

#endif /* USB_COMP_DESC_H */
