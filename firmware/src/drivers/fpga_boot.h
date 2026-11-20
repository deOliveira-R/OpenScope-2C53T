/*
 * OpenScope 2C53T - FPGA design upload (fpga_boot.c)
 *
 * The FPGA holds no design of its own that we use: at every boot the firmware
 * uploads the capture core through the FPGA's configuration port, on the SPI3
 * pins driven as plain GPIO. The design comes from the slot in the external
 * flash when one is stored and selected, else from the image itself.
 */
#ifndef FPGA_BOOT_H
#define FPGA_BOOT_H

#include <stdint.h>
#include <stdbool.h>
#include "../dsp/core_ident.h"   /* CORE_IDENT_REPLY_LEN */

/* Upload the boot design: takes PB3..PB6 as GPIO for the configuration
 * commands and the design, then gives PB3..PB5 back to SPI3. Returns the last
 * byte of the configuration STATUS register read after the upload and stores
 * all four in fboot.cfg_status_reg[]. */
uint8_t fpga_bitbang_config_sequence(void);

/* The configuration STATUS register (0x41) as read after the last upload,
 * first byte clocked in the top byte, and its DONE bit (the design started). */
uint32_t fpga_cfg_status(void);
#define GOWIN_STATUS_DONE_BIT  13u

/* How the last design upload ended (fboot.upload_result, shown by `status`). */
typedef enum {
    FPGA_UPLOAD_NOT_RUN = 0,     /* no upload was attempted since boot */
    FPGA_UPLOAD_SENT,            /* every byte of the design was sent */
    FPGA_UPLOAD_NOT_EMPTY,       /* nothing sent: the FPGA did not answer its IDCODE, so it runs a design */
    FPGA_UPLOAD_SOURCE_FAILED,   /* the design's source stopped part-way: the FPGA is empty */
} fpga_upload_result_t;

/* Reload while running: switch the FPGA's supply off and on with PC6, then
 * upload the compiled-in design (fn == NULL) or `len` bytes that fn supplies.
 * fn fills buf with n bytes of the design at offset off; false aborts. */
typedef bool (*fpga_payload_fn)(void *ctx, uint32_t off, uint8_t *buf, uint32_t n);
typedef enum {
    FPGA_RELOAD_OK = 0,        /* design loaded, DONE set                          */
    FPGA_RELOAD_NOT_EMPTY,     /* the FPGA did not power down; nothing was sent    */
    FPGA_RELOAD_SRC_FAIL,      /* the source failed mid-upload; the FPGA is empty  */
    FPGA_RELOAD_NOT_DONE,      /* upload sent, DONE not set; the FPGA is empty     */
    FPGA_RELOAD_BAD_ARG,
    FPGA_RELOAD_UNSUPPORTED,   /* this build's upload variant cannot check         */
} fpga_reload_status_t;
typedef struct {
    uint32_t idcode_before;    /* configuration port answer before the upload */
    uint32_t status;           /* STATUS (0x41) after the upload              */
    uint32_t sent;             /* payload bytes sent                          */
} fpga_reload_report_t;
#define FPGA_RELOAD_OFF_MS  1000u   /* PC6 low; measured on unit #2 to empty the FPGA */
#define FPGA_RELOAD_ON_MS    200u   /* supply back before the first configuration command */
fpga_reload_status_t fpga_reload(fpga_payload_fn fn, void *ctx, uint32_t len,
                                 fpga_reload_report_t *rep);

/* Name of the slot design the boot upload sent, or NULL when it sent the
 * compiled-in design. */
const char *fpga_boot_design(void);
/* What the boot found on the FPGA: 0 empty (cold boot), 1 a running design, removed
 * through PC6 and replaced, 2 a running design that stayed (nothing uploaded). */
uint8_t fpga_boot_live(void);

/* Give the SPI3 pins up (PB3, PB5, PB6 to inputs) so that a programmer on the
 * board's test pads can drive the FPGA's configuration port, and take them
 * back. While released, fboot.bus_released is set and the acquisition task
 * stays off the bus. Toolkit use. */
void fpga_bus_release(void);
void fpga_spi3_bus_reacquire(void);

/* State of the design upload and of the SPI3 pins (fpga_boot.c writes; the shell reads). */
typedef struct {
    volatile bool     bus_released;  /* the SPI3 pins are given up (fpga_bus_release); the acquisition task stays off the bus */
    volatile uint32_t h2_bytes_sent;  /* design bytes sent by the last upload */
    volatile uint32_t upload_len;  /* design bytes the last upload had to send */
    volatile uint8_t  upload_result;  /* a fpga_upload_result_t */
    volatile uint8_t  core_ident[CORE_IDENT_REPLY_LEN];  /* reply to op E2 after a load of our core (all 0: not asked) */
    volatile uint8_t  scope_status[4];  /* reply to op 03 after the upload */
    volatile uint8_t  cfg_status_reg[4];  /* configuration STATUS register (0x41) after the upload, as clocked.
                                            * Bits: 0 CRC error, 1 bad command, 2 ID verify failed,
                                            * 13 DONE, 15 READY, 16 POR. All FF = the FPGA does not drive MISO. */
    /* Replies of the configuration port, 8 bytes each, read at /256 (the only
     * SPI3 divider at which that port's replies are readable). probe_id_bit:
     * the bit offset at which IDCODE 0x0120681B was found, -1 = not found. */
    volatile uint8_t  probe_idcode[8];  /* 0x11 IDCODE, before the upload */
    volatile uint8_t  probe_status[8];  /* 0x41 STATUS */
    volatile uint8_t  probe_user[8];  /* 0x13 USERCODE */
    volatile int8_t   probe_id_bit;
    /* IDCODE read after the last design byte and before the port is closed
     * (0x3A), so the FPGA still answers it after a good upload. Printed by
     * the bench shell only; DONE in cfg_status_reg is the result. */
    volatile uint8_t  probe_idcode_close[8];
    volatile int8_t   probe_id_bit_close;
} fpga_boot_state_t;

extern fpga_boot_state_t fboot;   /* defined in fpga_boot.c */

#endif /* FPGA_BOOT_H */
