/*
 * usb_composite.c — CDC (the debug shell) + read-only MSC as one USB device.
 * See usb_composite.h for the interface/endpoint map and why the CDC half is
 * the unmodified AT32 class reached through its handler table.
 *
 * Why not the AT32 library's composite_cdc_msc class: it moves the CDC to
 * different endpoints (0x83/0x02) and interface numbers, defines PID 0x5760,
 * never assigns packet-memory addresses for its endpoints (no
 * usbd_ept_buf_custom_define), and its BOT layer keeps a 4096-byte static
 * buffer and calls the disk read from the USB interrupt — where the W25Q's
 * filesystem mutex (flash_fs.c) cannot be taken. usb_msc.c replaces that part.
 */
#include "usb_composite.h"
#include "usb_msc.h"

#include "usb_std.h"
#include "usbd_sdr.h"
#include "cdc_class.h"
#include "cdc_desc.h"       /* VID/PID, strings and MCU_ID addresses: the CDC build's own values */

#include <string.h>

/* The library's CDC data block (cdc_class.c). Its handlers find it through
 * udev->class_handler->pdata, so ours must point at it. */
extern cdc_struct_type cdc_struct;

/* ── Class handler ─────────────────────────────────────────────────────── */

static usb_sts_type comp_init(void *udev)
{
    cdc_class_handler.init_handler(udev);
    usb_msc_class_init(udev);
    return USB_OK;
}

static usb_sts_type comp_clear(void *udev)
{
    cdc_class_handler.clear_handler(udev);
    usb_msc_class_clear(udev);
    return USB_OK;
}

static usb_sts_type comp_setup(void *udev, usb_setup_type *setup)
{
    switch (setup->bmRequestType & USB_REQ_RECIPIENT_MASK) {
    case USB_REQ_RECIPIENT_INTERFACE:
        if (LBYTE(setup->wIndex) == USB_COMP_MSC_IF) return usb_msc_setup(udev, setup);
        break;
    case USB_REQ_RECIPIENT_ENDPOINT:
        if ((LBYTE(setup->wIndex) & 0x7Fu) == (USB_COMP_MSC_IN_EPT & 0x7Fu))
            return usb_msc_setup(udev, setup);
        break;
    default:
        break;
    }
    return cdc_class_handler.setup_handler(udev, setup);
}

/* EP0 data stages belong to CDC only (SET_LINE_CODING); MSC has none. */
static usb_sts_type comp_ept0_tx(void *udev) { return cdc_class_handler.ept0_tx_handler(udev); }
static usb_sts_type comp_ept0_rx(void *udev) { return cdc_class_handler.ept0_rx_handler(udev); }

static usb_sts_type comp_in(void *udev, uint8_t ept_num)
{
    if ((ept_num & 0x7Fu) == (USB_COMP_MSC_IN_EPT & 0x7Fu)) {
        usb_msc_in_done(udev);
        return USB_OK;
    }
    return cdc_class_handler.in_handler(udev, ept_num);
}

static usb_sts_type comp_out(void *udev, uint8_t ept_num)
{
    if ((ept_num & 0x7Fu) == (USB_COMP_MSC_OUT_EPT & 0x7Fu)) {
        usb_msc_out_done(udev);
        return USB_OK;
    }
    return cdc_class_handler.out_handler(udev, ept_num);
}

static usb_sts_type comp_sof(void *udev) { return cdc_class_handler.sof_handler(udev); }

static usb_sts_type comp_event(void *udev, usbd_event_type event)
{
    if (event == USBD_RESET_EVENT) usb_msc_bus_reset();
    return cdc_class_handler.event_handler(udev, event);
}

usbd_class_handler usb_composite_class_handler = {
    comp_init,
    comp_clear,
    comp_setup,
    comp_ept0_tx,
    comp_ept0_rx,
    comp_in,
    comp_out,
    comp_sof,
    comp_event,
    &cdc_struct,
};

/* ── Descriptors ───────────────────────────────────────────────────────── */

#define COMP_BCD_DEVICE     0x0201u   /* CDC-only build: 0x0200 */
#define COMP_CONFIG_LEN     (9u + 8u + 9u + 5u + 5u + 4u + 5u + 7u + 9u + 7u + 7u + 9u + 7u + 7u)

static const uint8_t device_desc[USB_DEVICE_DESC_LEN] ALIGNED_TAIL = {
    USB_DEVICE_DESC_LEN,
    USB_DESCIPTOR_TYPE_DEVICE,
    0x00, 0x02,                         /* bcdUSB 2.00 */
    0xEF, 0x02, 0x01,                   /* misc / common / IAD: a composite device */
    USB_MAX_EP0_SIZE,
    LBYTE(USBD_CDC_VENDOR_ID), HBYTE(USBD_CDC_VENDOR_ID),
    LBYTE(USBD_CDC_PRODUCT_ID), HBYTE(USBD_CDC_PRODUCT_ID),
    LBYTE(COMP_BCD_DEVICE), HBYTE(COMP_BCD_DEVICE),
    USB_MFC_STRING, USB_PRODUCT_STRING, USB_SERIAL_STRING,
    1,
};

static const uint8_t config_desc[COMP_CONFIG_LEN] ALIGNED_TAIL = {
    USB_DEVICE_CFG_DESC_LEN, USB_DESCIPTOR_TYPE_CONFIGURATION,
    LBYTE(COMP_CONFIG_LEN), HBYTE(COMP_CONFIG_LEN),
    3,                                  /* interfaces */
    1,                                  /* bConfigurationValue */
    0,
    0xC0,                               /* self powered (as the CDC build) */
    0x32,                               /* 100 mA */

    /* IAD: CDC owns interfaces 0..1 */
    0x08, 0x0B, USB_COMP_CDC_COMM_IF, 2, USB_CLASS_CODE_CDC, 0x02, 0x01, 0,

    /* interface 0: CDC comm, ACM — byte for byte the CDC build's */
    USB_DEVICE_IF_DESC_LEN, USB_DESCIPTOR_TYPE_INTERFACE, USB_COMP_CDC_COMM_IF, 0, 1,
    USB_CLASS_CODE_CDC, 0x02, 0x01, 0,
    0x05, USBD_CDC_CS_INTERFACE, USBD_CDC_SUBTYPE_HEADER, LBYTE(CDC_BCD_NUM), HBYTE(CDC_BCD_NUM),
    0x05, USBD_CDC_CS_INTERFACE, USBD_CDC_SUBTYPE_CMF, 0x00, USB_COMP_CDC_DATA_IF,
    0x04, USBD_CDC_CS_INTERFACE, USBD_CDC_SUBTYPE_ACM, 0x02,
    0x05, USBD_CDC_CS_INTERFACE, USBD_CDC_SUBTYPE_UFD, USB_COMP_CDC_COMM_IF, USB_COMP_CDC_DATA_IF,
    USB_DEVICE_EPT_LEN, USB_DESCIPTOR_TYPE_ENDPOINT, USBD_CDC_INT_EPT, USB_EPT_DESC_INTERRUPT,
    LBYTE(USBD_CDC_CMD_MAXPACKET_SIZE), HBYTE(USBD_CDC_CMD_MAXPACKET_SIZE), CDC_HID_BINTERVAL_TIME,

    /* interface 1: CDC data */
    USB_DEVICE_IF_DESC_LEN, USB_DESCIPTOR_TYPE_INTERFACE, USB_COMP_CDC_DATA_IF, 0, 2,
    USB_CLASS_CODE_CDCDATA, 0x00, 0x00, 0,
    USB_DEVICE_EPT_LEN, USB_DESCIPTOR_TYPE_ENDPOINT, USBD_CDC_BULK_IN_EPT, USB_EPT_DESC_BULK,
    LBYTE(USBD_CDC_IN_MAXPACKET_SIZE), HBYTE(USBD_CDC_IN_MAXPACKET_SIZE), 0,
    USB_DEVICE_EPT_LEN, USB_DESCIPTOR_TYPE_ENDPOINT, USBD_CDC_BULK_OUT_EPT, USB_EPT_DESC_BULK,
    LBYTE(USBD_CDC_OUT_MAXPACKET_SIZE), HBYTE(USBD_CDC_OUT_MAXPACKET_SIZE), 0,

    /* interface 2: mass storage, SCSI transparent, bulk-only — the class
     * triple stock's own USB Sharing descriptor carries (08 06 50,
     * docs/re/parity2_static.md 4.5) */
    USB_DEVICE_IF_DESC_LEN, USB_DESCIPTOR_TYPE_INTERFACE, USB_COMP_MSC_IF, 0, 2,
    USB_CLASS_CODE_MSC, 0x06, 0x50, 0,
    USB_DEVICE_EPT_LEN, USB_DESCIPTOR_TYPE_ENDPOINT, USB_COMP_MSC_IN_EPT, USB_EPT_DESC_BULK,
    LBYTE(USB_MSC_MPS), HBYTE(USB_MSC_MPS), 0,
    USB_DEVICE_EPT_LEN, USB_DESCIPTOR_TYPE_ENDPOINT, USB_COMP_MSC_OUT_EPT, USB_EPT_DESC_BULK,
    LBYTE(USB_MSC_MPS), HBYTE(USB_MSC_MPS), 0,
};

static const uint8_t lang_id_desc[4] ALIGNED_TAIL = { 4, USB_DESCIPTOR_TYPE_STRING, 0x09, 0x04 };

/* Strings are built on request into one buffer (the core sends it before the
 * next request can arrive). 64 bytes: the longest, the product string
 * "AT32 Virtual Com Port  ", is 2 + 23 * 2 = 48. The library's CDC descriptor
 * file keeps 256 bytes for this; with --gc-sections it no longer links. */
static uint8_t str_buf[64] ALIGNED_TAIL;

static usbd_desc_t d_device = { sizeof device_desc, (uint8_t *)device_desc };
static usbd_desc_t d_config = { sizeof config_desc, (uint8_t *)config_desc };
static usbd_desc_t d_lang   = { sizeof lang_id_desc, (uint8_t *)lang_id_desc };
static usbd_desc_t d_str;

static usbd_desc_t *ascii_string(const char *s)
{
    uint32_t n = 0;
    while (s[n] != '\0' && 2u + 2u * (n + 1u) <= sizeof str_buf) {
        str_buf[2u + 2u * n] = (uint8_t)s[n];
        str_buf[3u + 2u * n] = 0;
        n++;
    }
    str_buf[0] = (uint8_t)(2u + 2u * n);
    str_buf[1] = USB_DESCIPTOR_TYPE_STRING;
    d_str.length = str_buf[0];
    d_str.descriptor = str_buf;
    return &d_str;
}

/* 12 hex digits from the MCU UID: the AT32 CDC descriptor's get_serial_num()
 * (cdc_desc.c), reproduced so the serial — and with it the macOS port name
 * /dev/cu.usbmodem<serial>N — does not change with the composite device. */
static void hex_unicode(uint32_t v, uint8_t *p, uint32_t digits)
{
    for (uint32_t i = 0; i < digits; i++) {
        uint32_t d = v >> 28;
        p[2u * i] = (uint8_t)(d < 10u ? '0' + d : 'A' + d - 10u);
        p[2u * i + 1u] = 0;
        v <<= 4;
    }
}

static usbd_desc_t *get_device(void)  { return &d_device; }
static usbd_desc_t *get_qualifier(void) { return NULL; }
static usbd_desc_t *get_config(void)  { return &d_config; }
static usbd_desc_t *get_other_speed(void) { return NULL; }
static usbd_desc_t *get_lang(void)    { return &d_lang; }
static usbd_desc_t *get_mfc(void)     { return ascii_string(USBD_CDC_DESC_MANUFACTURER_STRING); }
static usbd_desc_t *get_product(void) { return ascii_string(USBD_CDC_DESC_PRODUCT_STRING); }
static usbd_desc_t *get_iface(void)   { return ascii_string(USBD_CDC_DESC_INTERFACE_STRING); }
static usbd_desc_t *get_cfgstr(void)  { return ascii_string(USBD_CDC_DESC_CONFIGURATION_STRING); }

static usbd_desc_t *get_serial(void)
{
    uint32_t s0 = *(volatile uint32_t *)MCU_ID1;
    uint32_t s1 = *(volatile uint32_t *)MCU_ID2;
    uint32_t s2 = *(volatile uint32_t *)MCU_ID3;
    s0 += s2;
    memset(str_buf, 0, USBD_CDC_SIZ_STRING_SERIAL);
    str_buf[0] = USBD_CDC_SIZ_STRING_SERIAL;
    str_buf[1] = USB_DESCIPTOR_TYPE_STRING;
    /* the library leaves the digits zero when the UID sum is zero; same here */
    if (s0 != 0u) {
        hex_unicode(s0, &str_buf[2], 8);
        hex_unicode(s1, &str_buf[18], 4);
    }
    d_str.length = USBD_CDC_SIZ_STRING_SERIAL;
    d_str.descriptor = str_buf;
    return &d_str;
}

usbd_desc_handler usb_composite_desc_handler = {
    get_device,
    get_qualifier,
    get_config,
    get_other_speed,
    get_lang,
    get_mfc,
    get_product,
    get_serial,
    get_iface,
    get_cfgstr,
};
