/*
 * OpenScope 2C53T - the meter chip's link (meter_link.c)
 *
 * The multimeter is a separate chip. The MCU talks to it on USART2 (PA2 TX,
 * PA3 RX, 9600 baud): 10-byte command frames out, 12-byte data frames and
 * 10-byte echoes of obeyed commands back. PC11 high enables the chip's side.
 * The functions keep the fpga_ prefix of the file they came from.
 */
#ifndef METER_LINK_H
#define METER_LINK_H

#include <stdint.h>
#include <stdbool.h>
#include "fpga_meter_plan.h"
#include "FreeRTOS.h"

/* ── frames ─────────────────────────────────────────────────────────── */

#define FPGA_USART_BAUD       9600
#define FPGA_TX_FRAME_SIZE    10
#define FPGA_TX_FRAME_HISTORY 16
#define FPGA_RX_FRAME_SIZE    12
#define FPGA_RX_FRAME_HISTORY 8
#define FPGA_RX_RAW_HISTORY   32
#define FPGA_METER_TRANSITION_HISTORY 4

#define FPGA_RX_DATA_HDR_0    0x5A   /* data frame: 5A A5 */
#define FPGA_RX_DATA_HDR_1    0xA5
#define FPGA_RX_ECHO_HDR_0    0xAA   /* echo frame: AA 55 */
#define FPGA_RX_ECHO_HDR_1    0x55
#define FPGA_RX_ECHO_FRAME_SIZE 10

/* Low bytes of command words (the high byte is 0x05). The function selectors
 * are in fpga_meter_plan.c, stock_meter_word_low_for_submode[]. */
#define FPGA_CMD_METER_START  0x09   /* obeyed, it puts the chip's display into a
                                      * non-measurement state ("EF"); sent
                                      * unobeyed it is the keepalive's traffic */
#define FPGA_CMD_METER_NOPROBE 0x0A  /* the chip's capacitance selector; never
                                      * send it obeyed outside a function change */
#define FPGA_CMD_METER_VAR_14 0x14   /* the chip's own auto-everything function */

/* A command word is sent obeyed or unobeyed. Obeyed, the frame carries the
 * AA 55 header and the chip acts on it. Unobeyed, it carries 00 00: the chip
 * ignores the command, but any traffic makes it send data frames (measured:
 * transmit stopped, data frames stopped). The keepalive must be unobeyed, or
 * every poll restarts a measurement and its autorange. */

/* Create the link's queues and tasks (transmit dvom_TX, receive dvom_RX,
 * poll meter_poll). Call once before the scheduler starts, in every image
 * that has the meter screen. */
void meter_link_create_tasks(void);

/* ── sending ────────────────────────────────────────────────────────── */

/* Every frame goes through one queue and one task (dvom_TX). Queue an obeyed
 * command; does not block: pdFALSE if the queue is full. Callable once
 * fpga_create_tasks() has run. */
BaseType_t fpga_send_cmd(uint8_t cmd_high, uint8_t cmd_low);
/* Queue a frame the chip will not act on (00 00 header). */
BaseType_t fpga_send_cmd_unobeyed(uint8_t cmd_high, uint8_t cmd_low);
/* Send one obeyed word and wait delay_ms. */
void fpga_wire_send_word(uint16_t word, uint32_t delay_ms);

/* The header of obeyed frames: true = AA 55 (default; the chip requires it,
 * measured on two units), false = 00 00, a negative control for the bench. */
void fpga_meter_tx_header_set(bool aa55);
bool fpga_meter_tx_header_get(void);

/* Bench override of the last word of a function change: -1 = none, else the
 * low byte to send (0x07 or FPGA_CMD_METER_NOPROBE). */
extern volatile int16_t  fpga_meter_probe_tail_override;

/* ── the link ───────────────────────────────────────────────────────── */

/* The link is PC11, USART2 and the poll task's serving, together on or off.
 * Only the display task calls fpga_meter_link_set(); it does nothing when
 * the link is already in the state asked for, so a live link is never
 * restarted. */
void    fpga_meter_link_set(bool on);
bool    fpga_meter_link_on(void);
/* PC11 and the meter's use of the front end, without the rest. */
void    fpga_set_meter_mux(bool enable);
/* True from fpga_set_meter_mux(true) until the scope's front-end settings
 * are put back (scope_frontend.h, fpga_scope_posture_gen). */
bool    fpga_meter_holds_fe(void);
extern volatile bool fpga_meter_needs_activation;

/* Whether entering the meter also moves the scope's range relays the way
 * stock does (PC12, PE4..6, PA15, PA10, PB10, PB11, PB9, PA6). Default off:
 * the chip does not depend on them (measured 2026-11-14), and leaving them
 * lets the scope capture while the meter runs. */
void    fpga_meter_relays_set(bool on);
bool    fpga_meter_relays_get(void);

/* Enable or disable USART2 as fpga_init would, and read its registers back,
 * for the shell. */
void     fpga_usart_scope_enable(bool on);
uint32_t fpga_usart_ctrl1(void);
uint32_t fpga_usart_baudr(void);

/* ── function changes ───────────────────────────────────────────────── */

/* Ask the poll task to switch the chip to `submode`. Returns at once; the
 * newest request wins. */
void     fpga_request_meter_mode(uint8_t submode);
/* A request is still waiting to be taken up. */
bool fpga_meter_mode_request_pending(void);
/* The blocking form: send the function's word and wait for the chip. */
void fpga_set_meter_mode(uint8_t submode);
/* The meter screen was entered (input task): request the stored function
 * unless the link is already on and running it. */
void    fpga_meter_screen_entered(uint8_t submode);
/* A function change is in progress; frames that arrive meanwhile are
 * discarded and counted below. */
bool fpga_meter_transition_busy(void);
extern volatile uint8_t  meter_frame_discard_count;
extern volatile uint32_t meter_transition_frame_skip_count;

/* A function change with one word, as stock does it: no PC11 drop, no relay
 * writes, no restart of the chip. Used while the chip is already up; the
 * first entry and a fast change whose echo does not come run the full
 * sequence (PC11 low and high again, about 1.4 s). Default on. */
void fpga_meter_fast_switch_set(bool on);
bool fpga_meter_fast_switch_get(void);
/* The 4 Hz unobeyed keepalive of the poll task. Default on. */
void fpga_meter_keepalive_set(bool on);
bool fpga_meter_keepalive_get(void);
/* A fast change is possible now: PC11 high, the chip woke and answered. */
bool fpga_meter_soc_live(void);

/* The words fpga_set_meter_mode() would send for a submode. Reads only. */
fpga_meter_selector_t fpga_meter_expected_selectors(uint8_t submode);
/* Run the full sequence for `submode` again, from the shell. */
void fpga_meter_reinit(uint8_t submode);

/* ── bench diagnostics (shell `meter mux-arms`, `boot-sequence`, `pc11-timing`) ── */

/* Set the range relays to the two stock "mux" states given and report the
 * planned and the read-back pin masks. */
bool fpga_debug_apply_meter_mux_arms(uint8_t portc_porte_mux,
                                     uint8_t porta_portb_mux,
                                     uint16_t *planned_gpio,
                                     uint16_t *actual_gpio);
/* Replay stock's boot order of words (0x0508, 0x0509, 0x0507 or 0x050A, the
 * selector) with delay_ms between them. */
bool fpga_debug_send_meter_boot_order(uint8_t submode, uint32_t delay_ms,
                                      uint16_t *planned_gpio,
                                      uint16_t *actual_gpio);
/* Hold PC11 low for low_ms, raise it, wait high_ms, then send the submode's
 * words. */
bool fpga_debug_send_meter_pc11_timing(uint8_t submode,
                                       uint32_t low_ms,
                                       uint32_t high_ms,
                                       uint16_t *planned_gpio,
                                       uint16_t *actual_gpio);

/* State, counters and histories of the meter link (meter_link.c writes; the shell reads). */
typedef struct {
    /* The latest complete data frame from the meter chip. */
    volatile uint8_t  rx_frame[FPGA_RX_FRAME_SIZE];
    volatile bool     rx_frame_valid;
    /* The command frame being sent, and the receive state machine. */
    volatile uint8_t  tx_frame[FPGA_TX_FRAME_SIZE];
    volatile uint8_t  tx_index;
    volatile uint8_t  rx_buf[FPGA_RX_FRAME_SIZE];
    volatile uint8_t  rx_index;
    volatile uint16_t frame_count;  /* data frames (5A A5) */
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
} meter_link_state_t;

extern meter_link_state_t mlink;   /* defined in meter_link.c */

#endif /* METER_LINK_H */
