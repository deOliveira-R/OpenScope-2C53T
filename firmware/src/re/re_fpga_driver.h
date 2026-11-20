/*
 * OpenScope 2C53T - driver-level functions of the toolkit (re_fpga_driver.c)
 *
 * Experiments on the FPGA's configuration port and the meter link that the
 * instrument itself does not use. Built only with the toolkit (RETOOLS).
 */
#ifndef RE_FPGA_DRIVER_H
#define RE_FPGA_DRIVER_H

#include <stdint.h>

/* The knobs of fpga_spi3_config_sequence(), the upload of the design with the
 * SPI3 peripheral instead of GPIO. That path has never configured the FPGA;
 * the knobs exist to find out why (`fpga reinit`). All 0 = the plain sequence. */
typedef struct {
    uint32_t upload_br;       /* SPI3 baud divider for the 0x3B bulk phase (0=/2) */
    uint32_t prelude_gap_ms;  /* gap after each 05/12/15 prelude frame (stock ~100) */
    uint32_t post_close_ms;   /* delay after 0x3A close before scope-config (stock ~600) */
    uint8_t  arm_pb11;        /* 1 = drive PB11 HIGH before the handshake */
    uint8_t  reset_port;      /* a low pulse on a pin before the sequence: 0=none, 1=A,2=B,3=C,4=D,5=E */
    uint8_t  reset_pin;       /* 0..15 */
    uint16_t reset_low_ms;    /* its length */
    uint8_t  prelude_frame_mode; /* how 05/12/15 and 3B share chip-select frames:
                                  *   0 = each its own (as stock sends them),
                                  *   1 = 05 12 15 in one frame, then 3B,
                                  *   2 = 15 and 3B share a frame */
    uint32_t pre_upload_gap_ms;  /* delay between the prelude and the 0x3B open */
    uint32_t cmd_br;             /* SPI3 baud divider for the command frames and
                                  *   status reads (0 = /2). The configuration
                                  *   port's replies are only readable at /256. */
    uint8_t  strap_pd2;       /* PD2 through the whole sequence:       0=untouched, 1=hold HIGH, 2=hold LOW */
    uint8_t  strap_pd1213;    /* PD12+PD13 through the whole sequence: 0=untouched, 1=hold HIGH, 2=hold LOW */
    uint16_t trailing_clocks; /* dummy 0x00 bytes after the last design byte, before the 0x3A close */
    uint8_t  probe_edit;      /* 1 = after 0x15, read STATUS (0x41) at /256 into re_fpga.edit_mode_status[] */
    uint8_t  reload_3c;       /* 1 = send RELOAD (0x3C) at /256 before the prelude */
    uint8_t  probe_idcode;    /* 1 = read 0x11/0x00/0x41/0x13 at /256 (8 bytes each) before
                               *     the prelude and 0x11 again after 0x15, into
                               *     fpga.probe_*. The known answer is IDCODE 0x0120681B. */
    uint8_t  prelude_reads;   /* 1 = read 0x11, 0x13 and 0x41 at /256 between 0x05 and
                               *     0x12, each in its own frame (needs prelude_frame_mode 0) */
    uint8_t  omit_05;         /* 1 = leave the 0x05 frame out (the GPIO upload that works sends none) */
    uint8_t  single_br;       /* 1 = never change the SPI3 divider during the sequence, so
                               *     the peripheral is not disabled and enabled between
                               *     frames; the status reads still switch to /256 */
} fpga_cfg_seq_opts_t;

/* Run the sequence. Returns the byte read after the 0x3A close; fills
 * re_fpga.init_hs[], re_fpga.h2_close_status and fboot.scope_status[]. */
uint8_t fpga_spi3_config_sequence(const fpga_cfg_seq_opts_t *opt);

/* For a logic analyser on the SPI3 pins: `reps` (0 = 16) identical 0x15
 * frames from the SPI3 peripheral at /256, a 60 ms gap, then the same from
 * GPIO. Configures nothing. */
void fpga_edgecap_15(uint8_t reps);

/* Send one raw 10-byte frame on USART2, past the queue (frame: 10 bytes). */
void fpga_send_raw_frame(const uint8_t *frame);

/* What the toolkit's SPI3 upload (fpga_spi3_config_sequence) read back. */
typedef struct {
    volatile uint8_t  init_hs[12];  /* replies during the toolkit's SPI3 upload ([11] unused) */
    volatile uint8_t  h2_close_status;  /* byte read after the 0x3A close of the toolkit's SPI3 upload */
    volatile uint8_t  edit_mode_status[4];  /* STATUS read right after 0x15 (toolkit, probe_edit) */
    volatile uint8_t  probe_noop[8];  /* 0x00, the control */
    volatile uint8_t  probe_idcode_post[8];  /* 0x11 again, after 0x15 */
    volatile uint32_t h2_rx_00_count;
    volatile uint32_t h2_rx_ff_count;
    volatile uint32_t h2_rx_other_count;
    volatile uint8_t  h2_close_rx_len;
    volatile uint8_t  h2_close_rx[6];
} re_fpga_state_t;

extern re_fpga_state_t re_fpga;   /* defined in re_fpga_driver.c */

#endif /* RE_FPGA_DRIVER_H */
