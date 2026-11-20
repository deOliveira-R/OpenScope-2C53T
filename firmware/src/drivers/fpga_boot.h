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

/* Upload the boot design: takes PB3..PB6 as GPIO for the configuration
 * commands and the design, then gives PB3..PB5 back to SPI3. Returns the last
 * byte of the configuration STATUS register read after the upload and stores
 * all four in fpga.cfg_status_reg[]. */
uint8_t fpga_bitbang_config_sequence(void);

/* The configuration STATUS register (0x41) as read after the last upload,
 * first byte clocked in the top byte, and its DONE bit (the design started). */
uint32_t fpga_cfg_status(void);
#define GOWIN_STATUS_DONE_BIT  13u

/* How the last design upload ended (fpga.upload_result, shown by `status`). */
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
 * back. While released, fpga.bus_released is set and the acquisition task
 * stays off the bus. Toolkit use. */
void fpga_bus_release(void);
void fpga_spi3_bus_reacquire(void);

#endif /* FPGA_BOOT_H */
