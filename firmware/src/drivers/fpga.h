/*
 * OpenScope 2C53T - FPGA driver: bring-up, tasks and shared state
 *
 * The FPGA (Gowin GW1N-UV2) runs our capture core, uploaded at every boot
 * and read over SPI3 (PB3 SCK, PB4 MISO, PB5 MOSI, PB6 chip select; PC6 is
 * the FPGA's supply enable). The parts of the driver have their own headers,
 * all included here:
 *
 *   fpga_boot.h       design upload and reload
 *   fpga_acq.h        records from the core, its registers, interleave, roll
 *   scope_frontend.h  range relays, coupling, converter routing
 *   meter_link.h      the meter chip on USART2 (not part of the FPGA)
 */

#ifndef FPGA_H
#define FPGA_H

#include <stdint.h>
#include <stdbool.h>
#include "FreeRTOS.h"
#include "queue.h"
#include "fpga_boot.h"
#include "fpga_acq.h"
#include "scope_frontend.h"
#include "meter_link.h"

/* State and diagnostics of the driver's parts in one structure, read by the
 * shell's status commands. Each part writes its own fields. */
typedef struct {
    /* The published record (fpga_acq.c writes, the display reads; see
     * fpga_acq_frame_generation). */
    volatile uint8_t  ch1_buf[FPGA_ADC_BUF_SIZE];
    volatile uint8_t  ch2_buf[FPGA_ADC_BUF_SIZE];

    /* ── meter link (meter_link.c) ──────────────────────────────────── */

    /* The latest complete data frame from the meter chip. */
    volatile uint8_t  rx_frame[FPGA_RX_FRAME_SIZE];
    volatile bool     rx_frame_valid;

    /* The command frame being sent, and the receive state machine. */
    volatile uint8_t  tx_frame[FPGA_TX_FRAME_SIZE];
    volatile uint8_t  tx_index;
    volatile uint8_t  rx_buf[FPGA_RX_FRAME_SIZE];
    volatile uint8_t  rx_index;

    /* ── driver status ─────────────────────────────────────────────── */
    volatile bool     initialized;  /* fpga_init finished */
    volatile bool     bus_released;  /* the SPI3 pins are given up (fpga_bus_release); the acquisition task stays off the bus */

    /* ── meter link counters and histories, for the shell ──────────── */
    volatile uint16_t frame_count;  /* data frames (5A A5) */
    volatile uint8_t  channel_mask;  /* last PC1/PC2 code applied: 1 = CH1, 2 = CH2, 3 = both */
    volatile uint16_t echo_count;  /* echo frames (AA 55) */
    volatile uint16_t tx_count;  /* command frames sent */
    volatile uint16_t rx_byte_count;  /* bytes received */
    volatile uint16_t rx_sync_data_start_count;  /* saw 0x5A as byte 0 */
    volatile uint16_t rx_sync_echo_start_count;  /* saw 0xAA as byte 0 */
    volatile uint16_t rx_sync_data_header_count;  /* completed 5A A5 */
    volatile uint16_t rx_sync_echo_header_count;  /* completed AA 55 */
    volatile uint16_t rx_sync_bad_second_count;  /* second header byte rejected */
    volatile uint16_t rx_sync_stray_count;  /* byte ignored while not in a frame */
    volatile uint16_t rx_data_tx_busy_drop_count;  /* data frames dropped while a command frame was going out */
    volatile uint16_t rx_echo_valid_count;  /* echoes that match the command sent */
    volatile uint16_t rx_echo_bad_count;  /* echoes that do not */
    volatile uint8_t  rx_raw_history[FPGA_RX_RAW_HISTORY];
    volatile uint16_t rx_raw_history_tx_count[FPGA_RX_RAW_HISTORY];
    volatile uint8_t  rx_raw_history_tx_index[FPGA_RX_RAW_HISTORY];
    volatile uint8_t  rx_raw_history_rx_index[FPGA_RX_RAW_HISTORY];
    volatile uint8_t  rx_raw_history_head;
    volatile uint8_t  rx_raw_history_count;
    volatile uint16_t last_rx_frame_count;  /* the counters as they were when the latest data frame arrived */
    volatile uint16_t last_rx_tx_count;
    volatile uint16_t last_rx_echo_count;
    volatile uint16_t last_rx_mode_sequence_count;
    volatile uint8_t  last_rx_mode_sequence_submode;
    volatile uint8_t  last_rx_discard_remaining;
    volatile uint8_t  last_rx_transition_busy;
    volatile uint8_t  rx_frame_history[FPGA_RX_FRAME_HISTORY][FPGA_RX_FRAME_SIZE];
    volatile uint16_t rx_history_frame_count[FPGA_RX_FRAME_HISTORY];
    volatile uint16_t rx_history_tx_count[FPGA_RX_FRAME_HISTORY];
    volatile uint16_t rx_history_echo_count[FPGA_RX_FRAME_HISTORY];
    volatile uint16_t rx_history_sequence_count[FPGA_RX_FRAME_HISTORY];
    volatile uint8_t  rx_history_sequence_submode[FPGA_RX_FRAME_HISTORY];
    volatile uint8_t  rx_history_discard_remaining[FPGA_RX_FRAME_HISTORY];
    volatile uint8_t  rx_history_transition_busy[FPGA_RX_FRAME_HISTORY];
    volatile uint8_t  rx_frame_history_head;
    volatile uint8_t  rx_frame_history_count;
    volatile uint8_t  last_rx_echo_frame[FPGA_RX_ECHO_FRAME_SIZE];
    volatile uint8_t  tx_cmd_hi_history[16];
    volatile uint8_t  tx_cmd_lo_history[16];
    volatile uint8_t  tx_cmd_history_head;
    volatile uint8_t  tx_cmd_history_count;
    volatile uint8_t  last_tx_frame[FPGA_TX_FRAME_SIZE];  /* last 10-byte frame sent */

    /* Low byte of the last frame sent obeyed: the echo check compares against
     * this, not last_tx_frame, which the unobeyed keepalive overwrites
     * between a command and its echo. */
    volatile uint8_t  last_obeyed_tx_lo;
    volatile bool     last_obeyed_tx_valid;

    /* The last function change: ms from PC11 high to the chip's first data
     * frame (0xFFFF = timed out), selector sends until its echo, whether it
     * was confirmed, and totals of the failures. */
    volatile uint16_t meter_soc_wake_ms;
    volatile uint8_t  meter_selector_attempts;
    volatile uint8_t  meter_selector_confirmed;
    volatile uint16_t meter_selector_unconfirmed_total;
    volatile uint16_t meter_soc_wake_timeouts;

    /* Function changes done with one word, fast attempts that fell back to
     * the full sequence, and obeyed frames refused because the word belongs
     * to the chip's calibration procedure (meter_word_is_cal_wizard). */
    volatile uint16_t meter_fast_switch_count;
    volatile uint16_t meter_fast_fallback_count;
    volatile uint16_t meter_cal_word_refused;
    volatile uint16_t meter_cal_word_last;
    volatile uint8_t  tx_frame_history[FPGA_TX_FRAME_HISTORY][FPGA_TX_FRAME_SIZE];
    volatile uint16_t tx_frame_history_tx_count[FPGA_TX_FRAME_HISTORY];
    volatile uint8_t  tx_frame_history_head;
    volatile uint8_t  tx_frame_history_count;
    volatile uint8_t  tx_control_frame_history[FPGA_TX_FRAME_HISTORY][FPGA_TX_FRAME_SIZE];
    volatile uint16_t tx_control_frame_history_tx_count[FPGA_TX_FRAME_HISTORY];
    volatile uint8_t  tx_control_frame_history_head;
    volatile uint8_t  tx_control_frame_history_count;

    /* The words of the last function change, and a short history of changes. */
    volatile uint16_t meter_mode_sequence_count;
    volatile uint8_t  meter_mode_sequence_submode;
    volatile uint16_t meter_mode_config_word;
    volatile uint16_t meter_mode_selector_word;
    volatile uint16_t meter_mode_apply_word;
    volatile uint16_t meter_mode_probe_word;
    volatile uint16_t meter_mode_start_word;
    volatile uint8_t  meter_transition_history_submode[FPGA_METER_TRANSITION_HISTORY];
    volatile uint16_t meter_transition_history_config[FPGA_METER_TRANSITION_HISTORY];
    volatile uint16_t meter_transition_history_selector[FPGA_METER_TRANSITION_HISTORY];
    volatile uint16_t meter_transition_history_apply[FPGA_METER_TRANSITION_HISTORY];
    volatile uint8_t  meter_transition_history_bank[FPGA_METER_TRANSITION_HISTORY];
    volatile uint8_t  meter_transition_history_bank_first[FPGA_METER_TRANSITION_HISTORY];
    volatile uint8_t  meter_transition_history_bank_second[FPGA_METER_TRANSITION_HISTORY];
    volatile uint16_t meter_transition_history_probe[FPGA_METER_TRANSITION_HISTORY];
    volatile uint16_t meter_transition_history_start[FPGA_METER_TRANSITION_HISTORY];
    volatile uint16_t meter_transition_history_sequence_count[FPGA_METER_TRANSITION_HISTORY];
    volatile uint16_t meter_transition_history_tx_before[FPGA_METER_TRANSITION_HISTORY];
    volatile uint16_t meter_transition_history_tx_after[FPGA_METER_TRANSITION_HISTORY];
    volatile uint16_t meter_transition_history_frame_before[FPGA_METER_TRANSITION_HISTORY];
    volatile uint16_t meter_transition_history_frame_after[FPGA_METER_TRANSITION_HISTORY];
    volatile uint16_t meter_transition_history_planned_gpio[FPGA_METER_TRANSITION_HISTORY];
    volatile uint16_t meter_transition_history_actual_gpio[FPGA_METER_TRANSITION_HISTORY];
    volatile uint8_t  meter_transition_history_head;
    volatile uint8_t  meter_transition_history_count;

    /* The first data frame after the latest function change, with the state
     * it arrived in. Diagnostic only: nothing decodes from it. */
    volatile uint8_t  meter_first_rx_after_transition_armed;
    volatile uint8_t  meter_first_rx_after_transition_valid;
    volatile uint8_t  meter_first_rx_after_transition_submode;
    volatile uint16_t meter_first_rx_after_transition_seq;
    volatile uint16_t meter_first_rx_after_transition_config;
    volatile uint16_t meter_first_rx_after_transition_selector;
    volatile uint16_t meter_first_rx_after_transition_apply;
    volatile uint16_t meter_first_rx_after_transition_probe;
    volatile uint16_t meter_first_rx_after_transition_start;
    volatile uint16_t meter_first_rx_after_transition_planned_gpio;
    volatile uint16_t meter_first_rx_after_transition_actual_gpio;
    volatile uint16_t meter_first_rx_after_transition_data;
    volatile uint16_t meter_first_rx_after_transition_tx;
    volatile uint16_t meter_first_rx_after_transition_echo;
    volatile uint8_t  meter_first_rx_after_transition_busy;
    volatile uint8_t  meter_first_rx_after_transition_discard;
    volatile uint8_t  meter_first_rx_after_transition_frame[FPGA_RX_FRAME_SIZE];
    volatile uint32_t meter_first_rx_after_transition_h2_bytes;
    volatile uint8_t  meter_first_rx_after_transition_h2_done;

    /* ── acquisition (fpga_acq.c) ──────────────────────────────────── */
    volatile uint16_t spi3_ok_count;  /* records read */
    volatile uint16_t spi3_total_timeouts;  /* records refused since boot: a transfer timed out during the read */
    volatile uint16_t spi3_hw_timeouts;  /* SPI3 byte transfers that timed out since boot; each returned 0xFF instead of bus data */
    volatile uint8_t  diag_ch1_raw[4];  /* first 4 bytes of the last CH1 read */
    volatile uint8_t  diag_ch2_raw[4];  /* first 4 bytes of the last CH2 read */

    /* ── bring-up and upload ───────────────────────────────────────── */
    volatile uint8_t  init_hs[12];  /* replies during the toolkit's SPI3 upload ([11] unused) */
    volatile uint32_t h2_bytes_sent;  /* design bytes sent by the last upload */
    volatile uint32_t upload_len;  /* design bytes the last upload had to send */
    volatile uint8_t  upload_result;  /* a fpga_upload_result_t */
    volatile uint8_t  h2_upload_done;  /* 1 = the upload sent every byte */
    volatile uint8_t  h2_close_status;  /* byte read after the 0x3A close of the toolkit's SPI3 upload */
    volatile uint8_t  core_ident[CORE_IDENT_REPLY_LEN];  /* reply to op E2 after a load of our core (all 0: not asked) */
    volatile uint8_t  scope_status[4];  /* reply to op 03 after the upload */
    volatile uint8_t  acq_hdr_ch1[3];  /* the 3 header bytes of the last op 04 reply */
    volatile uint8_t  acq_hdr_ch2[3];  /* the same of the last op 05 reply */
    volatile uint32_t pc0_last_tick;  /* tick of the last PC0 edge */
    volatile int32_t  acq_last_rot;  /* rotation applied to the last published record, -1 = none */
    volatile uint32_t acq_last_latency_ms;  /* PC0 edge to publication of the last held record */
    volatile uint32_t acq_poll_reads;  /* reads that found no held record, total */
    volatile uint32_t acq_polls_last;  /* such reads before the last held record */
    volatile uint32_t pc0_edges;  /* falling edges of PC0 (record ready), counted in EXINT0 */
    volatile uint8_t  cfg_status_reg[4];  /* configuration STATUS register (0x41) after the upload, as clocked.
                                            * Bits: 0 CRC error, 1 bad command, 2 ID verify failed,
                                            * 13 DONE, 15 READY, 16 POR. All FF = the FPGA does not drive MISO. */
    volatile uint8_t  edit_mode_status[4];  /* STATUS read right after 0x15 (toolkit, probe_edit) */

    /* Replies of the configuration port, 8 bytes each, read at /256 (the only
     * SPI3 divider at which that port's replies are readable). probe_id_bit:
     * the bit offset at which IDCODE 0x0120681B was found, -1 = not found. */
    volatile uint8_t  probe_idcode[8];  /* 0x11 IDCODE, before the upload */
    volatile uint8_t  probe_noop[8];  /* 0x00, the control */
    volatile uint8_t  probe_status[8];  /* 0x41 STATUS */
    volatile uint8_t  probe_user[8];  /* 0x13 USERCODE */
    volatile uint8_t  probe_idcode_post[8];  /* 0x11 again, after 0x15 */
    volatile int8_t   probe_id_bit;

    /* IDCODE read after the last design byte and before the port is closed
     * (0x3A), so the FPGA still answers it after a good upload. Printed by
     * the bench shell only; DONE in cfg_status_reg is the result. */
    volatile uint8_t  probe_idcode_close[8];
    volatile int8_t   probe_id_bit_close;

    /* Bytes read back during the toolkit's SPI3 upload, by value, and the
     * bytes after its close. */
    volatile uint32_t h2_rx_00_count;
    volatile uint32_t h2_rx_ff_count;
    volatile uint32_t h2_rx_other_count;
    volatile uint8_t  h2_close_rx_len;
    volatile uint8_t  h2_close_rx[6];
} fpga_state_t;

extern fpga_state_t fpga;   /* defined in fpga.c */

/* Bring up SPI3, USART2 and the control pins, upload the design, identify
 * the core and write its registers. Call after the clocks are set and before
 * the scheduler starts. */
void fpga_init(void);

/* Create the driver's tasks: acquisition (fpga), the meter link's transmit
 * (dvom_TX), receive (dvom_RX) and poll (meter_poll). Call after fpga_init()
 * and before the scheduler starts. */
void fpga_create_tasks(void);

#endif /* FPGA_H */
