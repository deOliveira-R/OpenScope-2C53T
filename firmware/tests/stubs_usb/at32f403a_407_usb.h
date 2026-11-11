/* Host stub for tests/test_usb.c. The endpoint-register bit values are the
 * real ones (at32f403a_407_usb.h); USB points at a fake register file; and
 * USB_CLEAR_TXTC / USB_CLEAR_RXTC are the LIBRARY'S ORIGINAL (racy)
 * definitions, so the test proves src/usb_conf.h replaces them. */
#ifndef AT32F403A_407_USB_H_STUB
#define AT32F403A_407_USB_H_STUB
#include <stdint.h>
#define USB_EPTADDR                      ((uint32_t)0x0000000F)
#define USB_TXSTS                        ((uint32_t)0x00000030)
#define USB_TXDTS                        ((uint32_t)0x00000040)
#define USB_TXTC                         ((uint32_t)0x00000080)
#define USB_EXF                          ((uint32_t)0x00000100)
#define USB_TRANS_TYPE                   ((uint32_t)0x00000600)
#define USB_SETUPTC                      ((uint32_t)0x00000800)
#define USB_RXSTS                        ((uint32_t)0x00003000)
#define USB_RXDTS                        ((uint32_t)0x00004000)
#define USB_RXTC                         ((uint32_t)0x00008000)
#define USB_EPT_BIT_MASK                 (uint32_t)(USB_TXTC | USB_SETUPTC | USB_EPTADDR | USB_EXF | USB_RXTC | USB_TRANS_TYPE)
typedef struct { uint32_t ept[8]; } usb_stub_regs_t;
extern usb_stub_regs_t usb_stub_regs;
#define USB (&usb_stub_regs)
#define USB_CLEAR_TXTC(eptn)    (USB->ept[eptn] &= 0xFF7F & USB_EPT_BIT_MASK)
#define USB_CLEAR_RXTC(eptn)    (USB->ept[eptn] &= 0x7FFF & USB_EPT_BIT_MASK)
#endif
