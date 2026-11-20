/*
 * OpenScope 2C53T - the meter chip's link (USART2)
 *
 * Frames to and from the meter chip, its three tasks, function changes and
 * the input multiplexer. Split out of fpga.c; shared items: fpga_internal.h.
 */

#include "fpga_internal.h"

/* USART ctrl1 bit masks (AT32 HAL uses MAKE_VALUE macros, we need raw bits) */
#define USART_CTRL1_RDBFIEN   (1 << 5)   /* RX buffer full interrupt enable */

#define USART_CTRL1_TDBEIEN   (1 << 7)   /* TX buffer empty interrupt enable */

#define USART_CTRL1_UEN       (1 << 13)  /* USART enable */

static void fpga_send_meter_mode_sequence(uint8_t submode);

volatile int16_t  fpga_meter_probe_tail_override = -1;

/* FreeRTOS handles */
QueueHandle_t     usart_tx_queue  = NULL;  /* 32-bit: hi|lo + OBEY bit16 */

QueueHandle_t meter_rx_queue = NULL;  /* Complete immutable 12-byte data frames */

TaskHandle_t      tx_task_handle  = NULL;

TaskHandle_t      rx_task_handle  = NULL;

static volatile bool meter_transition_busy = false;

static volatile bool meter_moves_relays;      /* `meter relays on`: old projection (A/B) */

/* M1/M2 state: see fpga_meter_fast_switch_set() and fpga_meter_keepalive_set(). */
static volatile bool meter_fast_switch_enabled = true;

static volatile bool meter_keepalive_enabled = true;

/* Set when a full transition ended with the SoC awake and the selector echoed;
 * cleared whenever PC11 is dropped (fpga_meter_reset_transport). */
static volatile bool meter_soc_live_flag = false;

/* The meter link (fpga.h, fpga_meter_link_set): unknown until the display
 * task's first evaluation, then off or on. The function the transition in
 * flight is for, so a link-on that finds one running need not ask again. */
enum { METER_LINK_ST_UNKNOWN = 0, METER_LINK_ST_OFF, METER_LINK_ST_ON };

static volatile uint8_t meter_link_state = METER_LINK_ST_UNKNOWN;

static volatile uint8_t meter_transition_target = 0xFF;

volatile uint8_t meter_frame_discard_count;

volatile uint32_t meter_transition_frame_skip_count;

static uint8_t fpga_probe_cmd_byte(void);

bool fpga_meter_transition_busy(void)
{
    return meter_transition_busy;
}

static void fpga_meter_discard_next_frames(uint8_t count)
{
    meter_frame_discard_count = count;
}

static void fpga_meter_reset_transport(void)
{
    uint32_t ctrl1;

    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) return;

    /*
     * DMM transition drain/reset.
     *
     * Stock evidence in analysis_v120/usart2_isr_state_machine.md shows two
     * USART2 frame families on one stream: 12-byte 0x5A/0xA5 meter data and
     * 10-byte 0xAA/0x55 command echoes. The meter-mode command-table note also
     * tracks the stock PC11 meter-MUX gate and the separate Port C/E and
     * Port A/B frontend writers. During a local mode switch we therefore stop
     * RX/TX IRQs, suspend both DVOM transport tasks, reset pending queues and
     * byte indices, drop PC11, then re-enable USART2 before resuming the DVOM
     * tasks and sending the new stock 0x05xx selector sequence.  Stock runtime
     * mode switching at 0x0800741A clears CTRL1 bit 0x2000 (UEN) while it
     * suspends `dvom_TX`/`dvom_RX`, resets 0x20002D7C and 0x20002D74, and
     * clears PC11; the enable tail at 0x08007360 sets UEN again before task
     * resume and PC11 assertion.  The exact 20 ms quiet window and later
     * two-frame discard are conservative local policy, not recovered stock
     * constants; the values are exported through `meter frontend`/`meter
     * mux-stream` so future stock traces can replace them instead of hiding a
     * guess here.
     */
    ctrl1 = USART2->ctrl1;
    USART2->ctrl1 = ctrl1 & ~(USART_CTRL1_UEN |
                              USART_CTRL1_RDBFIEN |
                              USART_CTRL1_TDBEIEN);

    if (tx_task_handle != NULL) vTaskSuspend(tx_task_handle);
    if (rx_task_handle != NULL) vTaskSuspend(rx_task_handle);

    if (usart_tx_queue != NULL) xQueueReset(usart_tx_queue);
    if (meter_rx_queue != NULL) xQueueReset(meter_rx_queue);

    fpga.tx_index = 0;
    fpga.rx_index = 0;
    fpga.rx_frame_valid = false;
    (void)USART2->sts;
    (void)USART2->dt;

    GPIOC->clr = (1U << 11);
    meter_soc_live_flag = false;       /* the SoC restarts when PC11 rises again */
    fpga_scope_delay_ms(20);

    USART2->ctrl1 = (ctrl1 | USART_CTRL1_UEN | USART_CTRL1_RDBFIEN) &
                    ~USART_CTRL1_TDBEIEN;
    /*
     * Do not inherit a dead transceiver (EXP-205, unit #2). On a silent-scope
     * build, scope entry parks USART2 with ctrl1 = 0 and the IRQ masked. When
     * a meter transition runs before fpga_set_meter_mux(true) has re-armed it
     * -- a race a shell mode switch to the meter loses about one time in three -- the
     * restore above re-enables UEN and RDBFIEN on a ctrl1 that had TE and RE
     * clear, with the NVIC line still masked: TX count climbs, the wire is
     * dark, the SoC never speaks, and every selector retry times out
     * (measured: CTRL1 0x20A0 in the failed state, 0x202C in the good one).
     * The transition is about to talk to the SoC, so it owns the transceiver.
     */
    USART2->ctrl1 |= (1U << 2) | (1U << 3);   /* RE, TE */
    NVIC_SetPriority(USART2_IRQn, 5);
    NVIC_EnableIRQ(USART2_IRQn);

    if (rx_task_handle != NULL) vTaskResume(rx_task_handle);
    if (tx_task_handle != NULL) vTaskResume(tx_task_handle);
}

/* ═══════════════════════════════════════════════════════════════════
 * USART2 Byte-Level TX (used during boot, before tasks are running)
 * ═══════════════════════════════════════════════════════════════════ */

static void usart2_send_byte(uint8_t b)
{
    volatile uint32_t timeout = 100000;
    while (!(USART2->sts & USART_TDBE_FLAG)) {
        if (--timeout == 0) return;
    }
    USART2->dt = b;
}

void usart2_send_frame(const uint8_t *frame)
{
    for (int i = 0; i < FPGA_TX_FRAME_SIZE; i++) {
        usart2_send_byte(frame[i]);
    }
    /* Wait for transmit complete */
    volatile uint32_t timeout = 100000;
    while (!(USART2->sts & USART_TDC_FLAG)) {
        if (--timeout == 0) break;
    }
}

static void fpga_record_tx_cmd(uint8_t cmd_hi, uint8_t cmd_lo)
{
    uint8_t idx = fpga.tx_cmd_history_head & 0x0F;

    fpga.tx_cmd_hi_history[idx] = cmd_hi;
    fpga.tx_cmd_lo_history[idx] = cmd_lo;
    fpga.tx_cmd_history_head = (uint8_t)((idx + 1U) & 0x0F);
    if (fpga.tx_cmd_history_count < 16U) {
        fpga.tx_cmd_history_count++;
    }
}

void fpga_record_tx_frame(const uint8_t *frame)
{
    uint8_t idx = fpga.tx_frame_history_head;

    memcpy((void *)fpga.last_tx_frame, frame, FPGA_TX_FRAME_SIZE);
    memcpy((void *)fpga.tx_frame_history[idx], frame, FPGA_TX_FRAME_SIZE);
    fpga.tx_frame_history_tx_count[idx] = fpga.tx_count;
    fpga.tx_frame_history_head =
        (uint8_t)((idx + 1U) % FPGA_TX_FRAME_HISTORY);
    if (fpga.tx_frame_history_count < FPGA_TX_FRAME_HISTORY) {
        fpga.tx_frame_history_count++;
    }

    /*
     * Control TX ring.
     *
     * The meter poll task emits 0x0509 continuously, so the ordinary TX ring can
     * be all poll frames by the time a settled DMM trace is captured. Keep a
     * second diagnostic-only ring for non-poll frames so low-DCV/live experiments
     * can still see the last selector/apply/setup commands that preceded the
     * wrong producer frame. This must not feed range decisions.
     */
    if (frame[2] != 0x05U || frame[3] != FPGA_CMD_METER_START) {
        idx = fpga.tx_control_frame_history_head;
        memcpy((void *)fpga.tx_control_frame_history[idx], frame,
               FPGA_TX_FRAME_SIZE);
        fpga.tx_control_frame_history_tx_count[idx] = fpga.tx_count;
        fpga.tx_control_frame_history_head =
            (uint8_t)((idx + 1U) % FPGA_TX_FRAME_HISTORY);
        if (fpga.tx_control_frame_history_count < FPGA_TX_FRAME_HISTORY) {
            fpga.tx_control_frame_history_count++;
        }
    }
}

static void fpga_record_rx_raw_byte(uint8_t byte, uint8_t rx_index_before)
{
    uint8_t idx = fpga.rx_raw_history_head;

    fpga.rx_raw_history[idx] = byte;
    fpga.rx_raw_history_tx_count[idx] = fpga.tx_count;
    fpga.rx_raw_history_tx_index[idx] = fpga.tx_index;
    fpga.rx_raw_history_rx_index[idx] = rx_index_before;
    fpga.rx_raw_history_head =
        (uint8_t)((idx + 1U) % FPGA_RX_RAW_HISTORY);
    if (fpga.rx_raw_history_count < FPGA_RX_RAW_HISTORY) {
        fpga.rx_raw_history_count++;
    }
}

static void fpga_record_rx_data_frame(void)
{
    uint8_t idx = fpga.rx_frame_history_head;

    /*
     * Producer-side RX ring.
     *
     * This records every complete 0x5A/0xA5 data frame as it crosses from the
     * FPGA USART stream into firmware ownership, including frames later ignored
     * by transition/discard logic. It is deliberately diagnostic-only: the ring
     * proves where the low-DCV mismatch appears, not selector decisions or
     * value-shaped correction factors.
     */
    memcpy((void *)fpga.rx_frame_history[idx], (const void *)fpga.rx_frame,
           FPGA_RX_FRAME_SIZE);
    fpga.rx_history_frame_count[idx] = fpga.last_rx_frame_count;
    fpga.rx_history_tx_count[idx] = fpga.last_rx_tx_count;
    fpga.rx_history_echo_count[idx] = fpga.last_rx_echo_count;
    fpga.rx_history_sequence_count[idx] = fpga.last_rx_mode_sequence_count;
    fpga.rx_history_sequence_submode[idx] = fpga.last_rx_mode_sequence_submode;
    fpga.rx_history_discard_remaining[idx] = fpga.last_rx_discard_remaining;
    fpga.rx_history_transition_busy[idx] = fpga.last_rx_transition_busy;
    fpga.rx_frame_history_head =
        (uint8_t)((idx + 1U) % FPGA_RX_FRAME_HISTORY);
    if (fpga.rx_frame_history_count < FPGA_RX_FRAME_HISTORY) {
        fpga.rx_frame_history_count++;
    }
}

static uint16_t fpga_meter_mux_gpio_mask_from_state(
    const fpga_meter_mux_gpio_state_t *state)
{
    uint16_t mask = 0;

    if (state->pc12) mask |= (1U << 0);
    if (state->pe4)  mask |= (1U << 1);
    if (state->pe5)  mask |= (1U << 2);
    if (state->pe6)  mask |= (1U << 3);
    if (state->pa15) mask |= (1U << 4);
    if (state->pa10) mask |= (1U << 5);
    if (state->pb10) mask |= (1U << 6);
    if (state->pb11) mask |= (1U << 7);
    if (state->pb9)  mask |= (1U << 8);
    if (state->pa6)  mask |= (1U << 9);
    return mask;
}

static uint16_t fpga_meter_mux_gpio_mask_live(void)
{
    uint16_t mask = 0;

    if (GPIOC->idt & (1U << 12)) mask |= (1U << 0);
    if (GPIOE->idt & (1U << 4))  mask |= (1U << 1);
    if (GPIOE->idt & (1U << 5))  mask |= (1U << 2);
    if (GPIOE->idt & (1U << 6))  mask |= (1U << 3);
    if (GPIOA->idt & (1U << 15)) mask |= (1U << 4);
    if (GPIOA->idt & (1U << 10)) mask |= (1U << 5);
    if (GPIOB->idt & (1U << 10)) mask |= (1U << 6);
    if (GPIOB->idt & PB11_MASK)  mask |= (1U << 7);
    if (GPIOB->idt & (1U << 9))  mask |= (1U << 8);
    if (GPIOA->idt & (1U << 6))  mask |= (1U << 9);
    return mask;
}

static void fpga_record_meter_transition_snapshot(
    uint8_t submode,
    const fpga_meter_transition_plan_t *plan,
    uint16_t planned_gpio,
    uint16_t actual_gpio,
    uint16_t tx_before,
    uint16_t frame_before)
{
    uint8_t idx = fpga.meter_transition_history_head;

    /*
     * DMM transition/apply trace.
     *
     * Capture the planned mux projection and the actual GPIO levels observed
     * immediately after the frontend writer, then pair them with the selector
     * words and producer counters surrounding the USART sequence. This is the
     * runtime evidence bridge between the stock-like writer/apply path and the
     * later producer RX frames; it must never drive value/range decisions.
     */
    fpga.meter_transition_history_submode[idx] = submode;
    fpga.meter_transition_history_config[idx] =
        plan->has_config_word ? plan->config_word : 0;
    fpga.meter_transition_history_selector[idx] = plan->selector_word;
    fpga.meter_transition_history_apply[idx] =
        plan->has_apply_word ? plan->apply_word : 0;
    fpga.meter_transition_history_bank[idx] =
        plan->has_command_bank_prefix ? 1U : 0U;
    fpga.meter_transition_history_bank_first[idx] =
        plan->has_command_bank_prefix ? plan->command_bank_first : 0U;
    fpga.meter_transition_history_bank_second[idx] =
        plan->has_command_bank_prefix ? plan->command_bank_second : 0U;
    fpga.meter_transition_history_probe[idx] =
        plan->has_probe_detect ?
        (uint16_t)((GPIOC->idt & (1U << 7)) ? 0x0507U : 0x050AU) : 0;
    fpga.meter_transition_history_start[idx] = plan->start_word;
    fpga.meter_transition_history_sequence_count[idx] =
        fpga.meter_mode_sequence_count;
    fpga.meter_transition_history_tx_before[idx] = tx_before;
    fpga.meter_transition_history_tx_after[idx] = fpga.tx_count;
    fpga.meter_transition_history_frame_before[idx] = frame_before;
    fpga.meter_transition_history_frame_after[idx] = fpga.frame_count;
    fpga.meter_transition_history_planned_gpio[idx] = planned_gpio;
    fpga.meter_transition_history_actual_gpio[idx] = actual_gpio;
    fpga.meter_transition_history_head =
        (uint8_t)((idx + 1U) % FPGA_METER_TRANSITION_HISTORY);
    if (fpga.meter_transition_history_count < FPGA_METER_TRANSITION_HISTORY) {
        fpga.meter_transition_history_count++;
    }
}

static void fpga_arm_meter_first_rx_latch(
    uint8_t submode,
    const fpga_meter_transition_plan_t *plan,
    uint16_t planned_gpio,
    uint16_t actual_gpio)
{
    /*
     * Arm a one-shot producer latch at the transition/apply boundary.
     *
     * Low-DCV failures are visible in the 12-byte DMM frame before display
     * formatting. Freezing the first producer frame after a transition lets the
     * bench trace say whether the wrong digits appeared immediately after the
     * selected mux/command/H2 state, without turning the trace into a
     * value-shaped correction loop.
     */
    fpga.meter_first_rx_after_transition_valid = 0;
    fpga.meter_first_rx_after_transition_submode = submode;
    fpga.meter_first_rx_after_transition_seq = fpga.meter_mode_sequence_count;
    fpga.meter_first_rx_after_transition_config =
        plan->has_config_word ? plan->config_word : 0;
    fpga.meter_first_rx_after_transition_selector = plan->selector_word;
    fpga.meter_first_rx_after_transition_apply =
        plan->has_apply_word ? plan->apply_word : 0;
    fpga.meter_first_rx_after_transition_probe =
        plan->has_probe_detect ?
        (uint16_t)(0x0500U | fpga_probe_cmd_byte()) : 0;
    fpga.meter_first_rx_after_transition_start = plan->start_word;
    fpga.meter_first_rx_after_transition_planned_gpio = planned_gpio;
    fpga.meter_first_rx_after_transition_actual_gpio = actual_gpio;
    fpga.meter_first_rx_after_transition_h2_bytes = fpga.h2_bytes_sent;
    fpga.meter_first_rx_after_transition_h2_done = fpga.h2_upload_done;
    fpga.meter_first_rx_after_transition_armed = 1;
}

static void fpga_capture_meter_first_rx_latch(void)
{
    if (fpga.meter_first_rx_after_transition_armed == 0) {
        return;
    }

    memcpy((void *)fpga.meter_first_rx_after_transition_frame,
           (const void *)fpga.rx_frame, FPGA_RX_FRAME_SIZE);
    fpga.meter_first_rx_after_transition_data = fpga.last_rx_frame_count;
    fpga.meter_first_rx_after_transition_tx = fpga.last_rx_tx_count;
    fpga.meter_first_rx_after_transition_echo = fpga.last_rx_echo_count;
    fpga.meter_first_rx_after_transition_busy = fpga.last_rx_transition_busy;
    fpga.meter_first_rx_after_transition_discard =
        fpga.last_rx_discard_remaining;
    fpga.meter_first_rx_after_transition_valid = 1;
    fpga.meter_first_rx_after_transition_armed = 0;
}

/*
 * Build and send a USART command frame (10 bytes).
 * Format: [hdr0][hdr1] [cmd_hi][cmd_lo] [0..0] [checksum]
 * Checksum = (cmd_hi + cmd_lo) & 0xFF
 *
 * THE HEADER IS AA 55 (issue #15; EXP-25 replicated it on unit #1 A/B/A/B/A,
 * echoes 0/3/0/3/0). With AA 55 the meter SoC echoes every accepted word and
 * switches function; with 00 00 -- what this project sent from 2026-04 to
 * 2026-09 -- it stays silent and holds its power-on auto mode. That is why
 * `echo_frames` sat at 0 since EXP-05 and why every meter reading before
 * 2026-09-12 was the SoC's auto mode, whatever the UI said.
 *
 * The header defaults ON, and it landed in the same commit as the corrected
 * word table: with a wrong table an obeyed frame is worse than a discarded
 * one. `meter hdr off` remains as the NEGATIVE CONTROL for the bench (a
 * transition under 00 00 must produce zero echoes and leave the SoC's
 * function where it was).
 *
 * The header only goes on frames sent OBEYED (FPGA_TX_OBEY_BIT). Everything
 * this firmware transmitted before 2026-09-12 as scope/siggen/config traffic
 * still goes out 00 00, bit-identical to the traffic the SoC answered for five
 * months, so the flip changes exactly one thing on the wire: the selector
 * word of a meter transition, plus whatever the operator sends by hand.
 */
static volatile bool meter_tx_header_aa55 = true;

void fpga_meter_tx_header_set(bool aa55) { meter_tx_header_aa55 = aa55; }

bool fpga_meter_tx_header_get(void)      { return meter_tx_header_aa55; }

/*
 * THE ONE PLACE A METER TX FRAME IS BUILT.
 *
 * It did not used to be. Until 2026-09-12 the ten bytes were assembled
 * independently in two places — here, and inline in the dvom_TX drain task —
 * and the two copies even carried the same wrong comment about byte[8].
 * EXP-25 found this the expensive way: the `meter hdr on` toggle patched this
 * copy, reported "AA 55" back, and the wire kept sending 00 00, because every
 * `usart tx` goes through the queue and the task's private copy. The run was
 * VOID, not negative, and only the `last_tx_frame` readback caught it.
 *
 * If you add a third transmit path, call this. Do not assemble bytes.
 */
static void meter_build_tx_frame(uint8_t *frame, uint8_t cmd_hi, uint8_t cmd_lo,
                                 bool obey)
{
    /*
     * GUARD (M7 is not implemented on purpose). 0x02A0 / 0x0503 / 0x0501 are
     * the words of stock's hidden SoC calibration sequence (key slots 7 and 2
     * in Ohm, 0x0800D6AC..0x0800D79A); obeyed, they may rewrite the SoC's
     * calibration. Scope/siggen replays send two of them with the 00 00 header
     * the SoC ignores, and keep doing so. An OBEYED request is downgraded to
     * the ignored header and counted, so it can never reach the SoC from the
     * shell, a transition or a future caller.
     */
    if (obey && meter_word_is_cal_wizard((uint16_t)(((uint16_t)cmd_hi << 8) | cmd_lo))) {
        obey = false;
        fpga.meter_cal_word_refused++;
        fpga.meter_cal_word_last = (uint16_t)(((uint16_t)cmd_hi << 8) | cmd_lo);
    }
    memset(frame, 0, FPGA_TX_FRAME_SIZE);
    if (meter_tx_header_aa55 && obey) {
        frame[0] = 0xAAU;
        frame[1] = 0x55U;
        /* Remember what the meter could have acted on, for echo validation. */
        fpga.last_obeyed_tx_lo = cmd_lo;
        fpga.last_obeyed_tx_valid = true;
    }
    frame[2] = cmd_hi;
    frame[3] = cmd_lo;
    /* bytes[4-8] stay 0: stock's builder does not set them for simple commands. */
    frame[9] = (cmd_lo + cmd_hi) & 0xFF;
}

static void usart2_send_cmd_ex(uint8_t cmd_hi, uint8_t cmd_lo, bool obey)
{
    uint8_t frame[FPGA_TX_FRAME_SIZE];
    fpga_record_tx_cmd(cmd_hi, cmd_lo);
    fpga.tx_count++;
    meter_build_tx_frame(frame, cmd_hi, cmd_lo, obey);
    fpga_record_tx_frame(frame);
    usart2_send_frame(frame);
}

/* Polled, OBEYED: a deliberate command to the meter SoC. */
static void usart2_send_cmd(uint8_t cmd_hi, uint8_t cmd_lo)
{
    usart2_send_cmd_ex(cmd_hi, cmd_lo, true);
}

static void fpga_timed_send_cmd_ex(uint8_t cmd_hi, uint8_t cmd_lo,
                                   uint32_t delay_ms, bool obey)
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING && usart_tx_queue != NULL) {
        uint32_t item = ((uint32_t)cmd_hi << 8) | cmd_lo |
                        (obey ? FPGA_TX_OBEY_BIT : 0U);

        /* Scope reinit is a deliberate control path, so it's worth waiting
         * briefly for queue space instead of silently dropping commands. */
        (void)xQueueSend(usart_tx_queue, &item, pdMS_TO_TICKS(100));
    } else {
        usart2_send_cmd_ex(cmd_hi, cmd_lo, obey);
    }

    fpga_scope_delay_ms(delay_ms);
}

/*
 * Timed send, UNOBEYED (00 00 header).
 *
 * This is the path every scope, siggen and mode-entry sequence in this file has
 * always used, and until 2026-09-12 all of it went out 00 00 and was discarded
 * by the meter SoC while still soliciting its data frames (EXP-26: the SoC
 * answers traffic, not commands). Keeping it unobeyed keeps that wire
 * bit-identical now that the header is real. What would otherwise have become
 * live the moment the header flipped: 0x050A (Capacitance) as the "PC7-low
 * probe tail", 0x0514 (Auto) as "variant setup", 0x0509 (a display state),
 * 0x0508, the 0x00 0x2C bank prefix, and every non-0x05 scope word whose
 * effect on the SoC nobody has measured. A command to the SoC is sent with
 * fpga_wire_send_word() or fpga_send_cmd() instead.
 */
static void fpga_timed_send_cmd(uint8_t cmd_hi, uint8_t cmd_lo, uint32_t delay_ms)
{
    fpga_timed_send_cmd_ex(cmd_hi, cmd_lo, delay_ms, false);
}

/* A stock word to the meter SoC, OBEYED (AA 55 when the header is on). This is
 * what a meter transition's selector and the debug boot-order replay's
 * selector use. */
void fpga_wire_send_word(uint16_t word, uint32_t delay_ms)
{
    fpga_timed_send_cmd_ex((uint8_t)(word >> 8), (uint8_t)(word & 0xFF),
                           delay_ms, true);
}

static uint8_t fpga_probe_cmd_byte(void)
{
    /*
     * Stock probe-command branch, not DMM range/calibration state.
     *
     * In V1.2.0 `FUN_0800B908`, the meter-basic arm at 0x0800B9D6, the extended
     * arm at 0x0800BACE, and the variant arm at 0x0800BC32 all materialize
     * GPIOC IDR (`0x40011008`), shift PC7 into the sign bit, and choose command
     * tail 0x07 when PC7 is high or 0x0A when PC7 is low.  That is digital
     * probe/tail sequencing for the USART2 command path.  It is not a recovered
     * analog mux/range writer, low-DCV correction, or factory calibration
     * coefficient; the physical "probe present" label remains a hardware
     * interpretation layered on top of the stock branch polarity.
     */
    if (fpga_meter_probe_tail_override == 0x07 ||
        fpga_meter_probe_tail_override == FPGA_CMD_METER_NOPROBE) {
        return (uint8_t)fpga_meter_probe_tail_override;
    }
    return (GPIOC->idt & (1U << 7)) ? 0x07 : FPGA_CMD_METER_NOPROBE;
}

fpga_meter_selector_t fpga_meter_expected_selectors(uint8_t submode)
{
    fpga_meter_transition_plan_t plan =
        fpga_meter_transition_plan_for_submode(submode);
    fpga_meter_selector_t selectors;

    selectors.function_selector = plan.stock_mode;
    selectors.range_selector =
        (plan.selector_word != FPGA_METER_INVALID_SELECTOR_WORD) ?
        (uint8_t)(plan.selector_word & 0x00FFU) : 0U;
    selectors.voltage_function_axis = plan.voltage_function_axis;
    return selectors;
}

static void fpga_gpio_write_level(gpio_type *gpio, uint32_t mask, uint8_t high)
{
    if (high) {
        gpio->scr = mask;
    } else {
        gpio->clr = mask;
    }
}

static void fpga_apply_meter_mux_gpio_state(const fpga_meter_mux_gpio_state_t *state)
{
    /*
     * Analog frontend mux projection. The recovered stock GPIO writers
     * FUN_080018a4 and FUN_08001a58 consume ms[0x02]/ms[0x03] and write
     * PC12/PE4/PE5/PE6 plus PA15/PA10/PB10/PB11. The open firmware applies the
     * tested fpga_meter_plan projection directly so production writes cannot
     * drift away from the state-machine table. This is not a recovered stock
     * runtime DMM mux writer: the guarded saved-config default ms[0x02]=5 /
     * ms[0x03]=5 is persistence evidence only, and the stock APP has no static
     * literal/function-pointer refs to either mux writer beyond guarded direct
     * callsites. The low-DCV mismatch still
     * needs a new writer, trace, H2/apply proof, or factory-calibration source.
     * PB9/PA6 remain low because stock currently proves output configuration
     * only, not mode-specific assertion.
     */
    fpga_gpio_write_level(GPIOC, (1U << 12), state->pc12);
    fpga_gpio_write_level(GPIOE, (1U << 4), state->pe4);
    fpga_gpio_write_level(GPIOE, (1U << 5), state->pe5);
    fpga_gpio_write_level(GPIOE, (1U << 6), state->pe6);
    fpga_gpio_write_level(GPIOA, (1U << 15), state->pa15);
    fpga_gpio_write_level(GPIOA, (1U << 10), state->pa10);
    fpga_gpio_write_level(GPIOB, (1U << 10), state->pb10);
    fpga_gpio_write_level(GPIOB, PB11_MASK, state->pb11);
    fpga_gpio_write_level(GPIOB, (1U << 9), state->pb9);
    fpga_gpio_write_level(GPIOA, (1U << 6), state->pa6);
}

bool fpga_debug_apply_meter_mux_arms(uint8_t portc_porte_mux,
                                     uint8_t porta_portb_mux,
                                     uint16_t *planned_gpio,
                                     uint16_t *actual_gpio)
{
    fpga_meter_mux_gpio_state_t mux_state;

    if (!fpga_meter_mux_gpio_state_for_stock_mux_arms(portc_porte_mux,
                                                       porta_portb_mux,
                                                       &mux_state)) {
        return false;
    }

    /*
     * Debug-only mux-arm apply.
     *
     * The unresolved low-DCV failure is upstream of stock-visible decimal
     * decoding. This hook lets the USB shell apply explicit stock mux-writer
     * arms and immediately compare the resulting producer frame against the
     * planned/live GPIO masks. Keeping this as an explicit diagnostic prevents
     * a live sweep from becoming an unreviewed production range heuristic.
     */
    GPIOB->scr = PB11_MASK;
    GPIOC->scr = PC6_MASK;
    GPIOC->scr = (1U << 11);
    meter_soc_live_flag = false;   /* PC11 edge outside a finished transition */
    if (planned_gpio != 0) {
        *planned_gpio = fpga_meter_mux_gpio_mask_from_state(&mux_state);
    }
    fpga_apply_meter_mux_gpio_state(&mux_state);
    if (actual_gpio != 0) {
        *actual_gpio = fpga_meter_mux_gpio_mask_live();
    }
    return true;
}

static void fpga_set_meter_frontend_for_submode(uint8_t submode)
{
    fpga_meter_mux_gpio_state_t mux_state;

    GPIOB->scr = PB11_MASK;
    GPIOC->scr = PC6_MASK;
    GPIOC->scr = (1U << 11);
    meter_soc_live_flag = false;   /* PC11 edge outside a finished transition */

    /*
     * Invalid local submodes still apply the baseline state returned by the
     * model, then emit no selector/apply word in the transition path. That
     * keeps the hardware side fail-closed instead of falling through to a
     * fabricated frontend split.
     */
    (void)fpga_meter_mux_gpio_state_for_submode(submode, &mux_state);
    /* Bench 2026-11-14 (unit #2): the meter SoC reads the same in every
     * state of these pins (DCV 1.6017 V, 121.5 Ohm, continuity, diode, all
     * ten scope ranges on both channels), and they are the scope's range
     * relays: moving them here is what stopped the scope's capture in the
     * meter. Left alone by default, so the scope front end stays live and
     * both instruments run together. `meter relays on` restores the old
     * projection for A/B. */
    if (meter_moves_relays) fpga_apply_meter_mux_gpio_state(&mux_state);
}

static void fpga_send_meter_wake_preamble(void)
{
    uint8_t probe_cmd = fpga_probe_cmd_byte();

    fpga_set_meter_frontend_for_submode(0);
    fpga_scope_delay_ms(20);

    /*
     * Boot-time meter bring-up uses cmd_hi=0x05 for this block. Stock
     * V1.2.0 has binary-guarded raw-word materializers for the configure
     * word 0x0508 at 0x080033CA, the start/poll word 0x0509 at 0x08003BA4,
     * and the variant/setup word 0x0514 at 0x08005B7A. The probe word is the
     * same 0x0507/0x050A GPIOC bit-7 branch guarded in FUN_0800B908; see
     * fpga_probe_cmd_byte() for the stock polarity and evidence boundary.
     *
     * That is stock command sequencing evidence only. It is not a recovered
     * analog range writer, low-DCV correction, or factory calibration source.
     *
     * All four go out UNOBEYED (fpga_timed_send_cmd): obeyed, 0x0A would
     * select Capacitance and 0x14 would select Auto right before the
     * transition sends the real selector.
     */
    fpga_timed_send_cmd(0x05, 0x08, 10);
    fpga_timed_send_cmd(0x05, FPGA_CMD_METER_START, 10);
    fpga_timed_send_cmd(0x05, probe_cmd, 10);
    fpga_timed_send_cmd(0x05, FPGA_CMD_METER_VAR_14, 20);
}

bool fpga_debug_send_meter_boot_order(uint8_t submode, uint32_t delay_ms,
                                      uint16_t *planned_gpio,
                                      uint16_t *actual_gpio)
{
    fpga_meter_transition_plan_t plan;
    fpga_meter_mux_gpio_state_t mux_state;
    uint8_t probe_cmd;

    if (!fpga.initialized || !fpga_meter_submode_is_valid(submode)) {
        return false;
    }
    if (delay_ms > 5000U) {
        delay_ms = 5000U;
    }

    plan = fpga_meter_transition_plan_for_submode(submode);
    if (!fpga_meter_mux_gpio_state_for_submode(submode, &mux_state)) {
        return false;
    }
    probe_cmd = fpga_probe_cmd_byte();

    meter_data_invalidate(submode);
    fpga_meter_reset_transport();
    fpga_set_meter_frontend_for_submode(submode);
    if (planned_gpio != 0) {
        *planned_gpio = fpga_meter_mux_gpio_mask_from_state(&mux_state);
    }
    if (actual_gpio != 0) {
        *actual_gpio = fpga_meter_mux_gpio_mask_live();
    }

    /*
     * Replay the stock boot/wake word order against the selected runtime
     * frontend. A previous direct-word live check only proved that adding
     * 0x0508 before the selector was insufficient; this hook tests the exact
     * boot order without rewriting the production transition path.
     */
    fpga.meter_mode_sequence_count++;
    fpga.meter_mode_sequence_submode = submode;
    fpga.meter_mode_selector_word = plan.selector_word;
    fpga.meter_mode_apply_word = 0x0508U;
    fpga.meter_mode_probe_word = (uint16_t)(0x0500U | probe_cmd);
    fpga.meter_mode_start_word = 0x0509U;

    fpga_timed_send_cmd(0x05, 0x08, 10);
    fpga_timed_send_cmd(0x05, FPGA_CMD_METER_START, 10);
    fpga_timed_send_cmd(0x05, probe_cmd, 10);
    fpga_wire_send_word(plan.selector_word, delay_ms);
    fpga_meter_discard_next_frames(plan.discard_frames);

    return true;
}

bool fpga_debug_send_meter_pc11_timing(uint8_t submode,
                                       uint32_t low_ms,
                                       uint32_t high_ms,
                                       uint16_t *planned_gpio,
                                       uint16_t *actual_gpio)
{
    fpga_meter_transition_plan_t plan;
    fpga_meter_mux_gpio_state_t mux_state;

    if (!fpga.initialized || !fpga_meter_submode_is_valid(submode)) {
        return false;
    }
    if (low_ms > 5000U || high_ms > 5000U) {
        return false;
    }

    plan = fpga_meter_transition_plan_for_submode(submode);
    if (!fpga_meter_mux_gpio_state_for_submode(submode, &mux_state)) {
        return false;
    }

    /*
     * PC11 timing probe.
     *
     * Stock runtime drain clears PC11 while the DMM USART/tasks/queues are
     * drained, and the enable path asserts PC11 before entering the mode-init
     * tail. The normal open transition already has correct-looking settled
     * selector/GPIO snapshots, but shorted probes still produce OL/special
     * frames. This diagnostic keeps PC11 low during mux projection, then raises
     * it with an explicit delay before sending the existing stock-like command
     * plan unchanged. A useful result must change the producer frames; this path
     * is not a numeric correction and is intentionally not used by production
     * fpga_set_meter_mode().
     */
    meter_data_invalidate(submode);
    fpga_meter_reset_transport();

    GPIOC->clr = (1U << 11);
    meter_soc_live_flag = false;   /* PC11 edge outside a finished transition */
    GPIOB->scr = PB11_MASK;
    GPIOC->scr = PC6_MASK;
    fpga_apply_meter_mux_gpio_state(&mux_state);
    GPIOC->clr = (1U << 11);
    meter_soc_live_flag = false;   /* PC11 edge outside a finished transition */

    if (planned_gpio != 0) {
        *planned_gpio = fpga_meter_mux_gpio_mask_from_state(&mux_state);
    }
    fpga_scope_delay_ms(low_ms);

    GPIOC->scr = (1U << 11);
    meter_soc_live_flag = false;   /* PC11 edge outside a finished transition */
    fpga_scope_delay_ms(high_ms);

    if (actual_gpio != 0) {
        *actual_gpio = fpga_meter_mux_gpio_mask_live();
    }

    fpga_send_meter_mode_sequence(submode);
    fpga_arm_meter_first_rx_latch(submode, &plan,
                                  planned_gpio != 0 ? *planned_gpio : 0,
                                  actual_gpio != 0 ? *actual_gpio : 0);
    fpga_meter_discard_next_frames(plan.discard_frames);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * USART2 IRQ Handler
 *
 * Called from the USART2 interrupt. Handles both TX and RX:
 *   TX: Pumps bytes from fpga.tx_frame[] (10 bytes, index in tx_index)
 *   RX: Assembles bytes into fpga.rx_buf[], validates frame headers
 * ═══════════════════════════════════════════════════════════════════ */

void USART2_IRQHandler(void)
{
    /* TX: send next byte from frame buffer */
    if ((USART2->ctrl1 & USART_CTRL1_TDBEIEN) && (USART2->sts & USART_TDBE_FLAG)) {
        if (fpga.tx_index < FPGA_TX_FRAME_SIZE) {
            USART2->dt = fpga.tx_frame[fpga.tx_index++];
        } else {
            /* All bytes sent — disable TX interrupt */
            USART2->ctrl1 &= ~USART_CTRL1_TDBEIEN;
        }
    }

    /* RX: receive and assemble frame */
    if (USART2->sts & USART_RDBF_FLAG) {
        fpga.rx_byte_count++;
        uint8_t byte = (uint8_t)USART2->dt;
        uint8_t rx_index_before = fpga.rx_index;

        /*
         * Raw USART2 byte trace, before header filtering.
         *
         * Stock V1.2.0 proves 12-byte `5A A5` data frames and 10-byte `AA 55`
         * echo frames share this RX stream. Live DMM runs currently show
         * `echo_start=0`; this ring answers whether 0xAA is absent on the wire or
         * merely lost to our sync state. It is diagnostic only and must not feed
         * decoder math, mode selection, or calibration.
         */
        fpga_record_rx_raw_byte(byte, rx_index_before);

        if (fpga.rx_index == 0) {
            /* Looking for frame header first byte */
            if (byte == FPGA_RX_DATA_HDR_0 || byte == FPGA_RX_ECHO_HDR_0) {
                if (byte == FPGA_RX_DATA_HDR_0) {
                    fpga.rx_sync_data_start_count++;
                } else {
                    fpga.rx_sync_echo_start_count++;
                }
                fpga.rx_buf[0] = byte;
                fpga.rx_index = 1;
            } else {
                fpga.rx_sync_stray_count++;
            }
        } else if (fpga.rx_index == 1) {
            /* Validate header second byte */
            if ((fpga.rx_buf[0] == FPGA_RX_DATA_HDR_0 && byte == FPGA_RX_DATA_HDR_1) ||
                (fpga.rx_buf[0] == FPGA_RX_ECHO_HDR_0 && byte == FPGA_RX_ECHO_HDR_1)) {
                if (fpga.rx_buf[0] == FPGA_RX_DATA_HDR_0) {
                    fpga.rx_sync_data_header_count++;
                } else {
                    fpga.rx_sync_echo_header_count++;
                }
                fpga.rx_buf[1] = byte;
                fpga.rx_index = 2;
            } else {
                /* Invalid header — restart */
                fpga.rx_sync_bad_second_count++;
                fpga.rx_index = 0;
            }
        } else {
            fpga.rx_buf[fpga.rx_index++] = byte;

            /* Check for complete frame */
            if (fpga.rx_buf[0] == FPGA_RX_DATA_HDR_0 &&
                fpga.rx_index >= FPGA_RX_FRAME_SIZE) {
                /*
                 * Stock USART2 RX does not release a 12-byte DMM data frame to
                 * dvom_RX while the 10-byte TX pump is still active. The
                 * decompiled ISR gate is `usart2_tx_byte_index == 10` plus the
                 * UI exchange lock. The open firmware has no equivalent display
                 * lock, but it does have the same TX byte index. Dropping data
                 * frames that arrive before the command frame finished prevents
                 * command-overlap bytes from becoming plausible voltage/current
                 * readings during DMM mode/range churn; it is producer hygiene,
                 * not a value or range correction.
                 */
                if (fpga.tx_index < FPGA_TX_FRAME_SIZE) {
                    fpga.rx_data_tx_busy_drop_count++;
                    fpga.rx_index = 0;
                    return;
                }

                /* Complete data frame (12 bytes): copy to diagnostic buffer and
                 * enqueue an immutable event for dvom_RX. A binary semaphore over
                 * fpga.rx_frame let later ISR frames overwrite the bytes before
                 * the task parsed them, which made stale/wrong-family DMM frames
                 * indistinguishable from legitimate active-mode data. */
                fpga_meter_rx_event_t event;
                memcpy((void *)fpga.rx_frame, (const void *)fpga.rx_buf,
                       FPGA_RX_FRAME_SIZE);
                memcpy(event.frame, (const void *)fpga.rx_buf,
                       FPGA_RX_FRAME_SIZE);
                fpga.rx_frame_valid = true;
                fpga.frame_count++;
                /*
                 * Producer-side DMM trace anchor.
                 *
                 * The low-DCV blocker is upstream of display formatting: the
                 * 12-byte stock-visible frame already contains the wrong
                 * digits. Capture the transport/selector state at the exact
                 * point that data frame becomes firmware-owned, before the
                 * dvom_RX task parses or rejects it. This is diagnostic only;
                 * it must not feed range decisions or value-shaped fixes.
                 */
                fpga.last_rx_frame_count = fpga.frame_count;
                fpga.last_rx_tx_count = fpga.tx_count;
                fpga.last_rx_echo_count = fpga.echo_count;
                fpga.last_rx_mode_sequence_count = fpga.meter_mode_sequence_count;
                fpga.last_rx_mode_sequence_submode = fpga.meter_mode_sequence_submode;
                fpga.last_rx_discard_remaining = meter_frame_discard_count;
                fpga.last_rx_transition_busy = meter_transition_busy ? 1U : 0U;
                event.frame_count = fpga.last_rx_frame_count;
                event.tx_count = fpga.last_rx_tx_count;
                event.echo_count = fpga.last_rx_echo_count;
                event.mode_sequence_count = fpga.last_rx_mode_sequence_count;
                event.mode_sequence_submode = fpga.last_rx_mode_sequence_submode;
                event.discard_remaining = fpga.last_rx_discard_remaining;
                event.transition_busy = fpga.last_rx_transition_busy;
                fpga_record_rx_data_frame();
                fpga_capture_meter_first_rx_latch();
                fpga.rx_index = 0;

                /* Hand complete frame ownership to dvom_RX (only if RTOS is running). */
                if (meter_rx_queue != NULL &&
                    xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
                    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
                    (void)xQueueSendFromISR(meter_rx_queue, &event,
                                            &xHigherPriorityTaskWoken);
                    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
                }

            } else if (fpga.rx_buf[0] == FPGA_RX_ECHO_HDR_0 &&
                       fpga.rx_index >= FPGA_RX_ECHO_FRAME_SIZE) {
                /*
                 * Complete echo frame (10 bytes).
                 *
                 * Stock validates byte[3] against the just-sent command low byte
                 * and byte[7] against the fixed 0xAA integrity marker. Keep the
                 * same validation as diagnostics so a bad echo cannot be treated
                 * as transport confidence while debugging wrong DMM producer
                 * frames. Data frames are still handled independently above.
                 */
                memcpy((void *)fpga.last_rx_echo_frame, (const void *)fpga.rx_buf,
                       FPGA_RX_ECHO_FRAME_SIZE);
                /* Validate against the last OBEYED command, not the last
                 * frame sent — see fpga.last_obeyed_tx_lo (EXP-27). */
                if (fpga.last_obeyed_tx_valid &&
                    fpga.rx_buf[3] == fpga.last_obeyed_tx_lo &&
                    fpga.rx_buf[7] == 0xAAU) {
                    fpga.rx_echo_valid_count++;
                    fpga.echo_count++;
                } else {
                    fpga.rx_echo_bad_count++;
                }
                fpga.rx_index = 0;
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * FreeRTOS Tasks
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * USART TX Task (dvom_TX equivalent)
 * Receives 2-byte command items from usart_tx_queue, builds 10-byte
 * frames, and initiates interrupt-driven transmission.
 */
void fpga_usart_tx_task(void *pv)
{
    (void)pv;
    uint32_t cmd_item;

    for (;;) {
        xQueueReceive(usart_tx_queue, &cmd_item, portMAX_DELAY);

        uint8_t cmd_lo = cmd_item & 0xFF;
        uint8_t cmd_hi = (cmd_item >> 8) & 0xFF;
        bool    obey   = (cmd_item & FPGA_TX_OBEY_BIT) != 0;

        /* Build TX frame through the SHARED builder. This block used to
         * assemble the bytes itself, which is how EXP-25 came to report a
         * header change that never reached the wire. */
        fpga.tx_count++;
        fpga_record_tx_cmd(cmd_hi, cmd_lo);
        fpga.tx_index = 0;
        {
            uint8_t frame[FPGA_TX_FRAME_SIZE];
            meter_build_tx_frame(frame, cmd_hi, cmd_lo, obey);
            memcpy((void *)fpga.tx_frame, frame, FPGA_TX_FRAME_SIZE);
        }
        fpga_record_tx_frame((const uint8_t *)fpga.tx_frame);

        /* Enable TX interrupt — ISR pumps all 10 bytes */
        USART2->ctrl1 |= USART_CTRL1_TDBEIEN;

        /* Wait for transmission before accepting next command */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/*
 * USART RX Processing Task (dvom_RX equivalent)
 * Wakes on meter_rx_queue when a complete data frame arrives.
 * Parses BCD meter readings and updates the global meter_reading.
 *
 * Stock note: the decompiled FUN_080028e0 path queues dispatcher indices
 * 0x1B/0x1C/0x1E after formatter state changes. Those are not raw USART2
 * bytes, so the RX task must not emit value-shaped "range fixes" from the
 * decoded number. The poll task below keeps the active stock selector/config
 * words alive through the already recovered raw 0x05xx materializers instead.
 */
void fpga_usart_rx_task(void *pv)
{
    (void)pv;

    for (;;) {
        fpga_meter_rx_event_t event;

        /* Block until USART ISR owns a complete data frame. */
        xQueueReceive(meter_rx_queue, &event, portMAX_DELAY);

        /* Parse the meter data from the RX frame.
         * meter_submode is the global from main.c (via ui.h extern). */
        extern volatile uint8_t meter_submode;
        if (!fpga_meter_rx_frame_should_parse(event.transition_busy != 0U,
                                              &meter_frame_discard_count,
                                              &meter_transition_frame_skip_count)) {
            continue;
        }

        uint8_t parse_submode = meter_submode;
        if (fpga_meter_submode_is_valid(event.mode_sequence_submode)) {
            parse_submode = event.mode_sequence_submode;
        }
        /*
         * H2/SPI3 readback remains diagnostics, not an acceptance gate.
         *
         * Stock V1.2.0 evidence proves the H2 TX geometry and the post-H2 SPI3
         * trigger bytes, but no recovered compare/store/branch says "all 0xFF
         * MISO means DMM frames must be killed". Earlier OpenScope code added
         * that global gate while chasing the low-DCV bug; it hid the real USART2
         * producer frames and turned an unresolved H2/apply question into a fake
         * display fix. Let meter_data_process_frame() enforce the stock-visible
         * frame family/range rules, and keep H2/MISO counts in status/trace for
         * the still-unresolved calibration/frontend investigation.
         */
        meter_data_set_function_confirmed(fpga_meter_soc_live() && fpga.meter_selector_confirmed);
        meter_data_process_frame(event.frame, parse_submode);

        /*
         * Range feedback is intentionally not driven from the parsed number.
         *
         * Early bring-up notes speculated about sending 0x1B/0x1C/0x1E after
         * BCD overflow/underflow. That is now a known-bad direction for this
         * DMM port: stock-visible voltage scaling comes from frame metadata
         * (`frame[8].7`, `frame[3].4`, `frame[4].4`, `frame[5].4`) and the
         * active stock-like selector state, while the analog frontend must be
         * set by recovered mux/range writers or safe live traces. Do not infer
         * a new relay/range command from the decoded number here. The current
         * unresolved low-DCV case (`0.200 V` visual vs `0.4366 V` CDC) is a
         * frontend/H2/acceptance evidence problem, not an excuse for a
         * value-shaped feedback loop.
         */
    }
}

/*
 * Meter Poll Task
 *
 * The FPGA meter IC only emits a 12-byte data frame in response to a
 * "start measurement" command (0x00, 0x09). Without a continuous stream
 * of poll commands, the FPGA goes silent within ~5 frames and meter
 * readings freeze.
 *
 * Previously this poll lived inside draw_meter_screen() at meter_ui.c:768,
 * which tied the data acquisition cadence to the display refresh loop.
 * That worked by accident but coupled two unrelated concerns — if the UI
 * stopped redrawing (e.g. debug overlay, menu, backgrounded screen), data
 * flow stopped too.
 *
 * This task decouples the poll from the UI. It runs at ~4 Hz (matched to
 * the FPGA meter IC's natural ~3 Hz data cadence) and only polls while
 * the user is in meter mode.
 *
 * Root-cause analysis: reverse_engineering/analysis_v120/usart2_isr_state_machine.md
 */
static void fpga_send_meter_poll_sequence(uint8_t submode)
{
    fpga_meter_transition_plan_t plan =
        fpga_meter_transition_plan_for_submode(submode);

    if (!fpga_meter_submode_is_valid(submode) ||
        plan.selector_word == FPGA_METER_INVALID_SELECTOR_WORD ||
        plan.start_word == 0) {
        return;
    }

    /*
     * Poll only the stock start word after the transition sequence has
     * selected the mode. Re-sending selector/config/probe words every 250 ms
     * keeps the producer in setup traffic and can hold low-DCV auto/range
     * settling in status-20 transitional frames. Selector/config/apply words
     * belong to fpga_send_meter_mode_sequence(), not to the sample cadence.
     */
    /*
     * KEEPALIVE, NOT A COMMAND (EXP-25, 2026-09-12).
     *
     * This fires 4x/second. Before the header fix every frame was discarded, so
     * the cadence was never exercised against a meter that obeys. The moment
     * the header became correct, each poll was accepted and re-triggered a
     * measurement + autorange — continuous relay actuation, audible across the
     * room, and real mechanical wear.
     *
     * The poll does not need to be obeyed. It only needs the wire to be busy:
     * measured the same session, with the header off and NOT ONE command
     * accepted, data frames still arrived at 6/s, and stopping transmit
     * altogether took them to 0/s. So send it unobeyed.
     */
    (void)fpga_send_cmd_keepalive((uint8_t)(plan.start_word >> 8),
                                  (uint8_t)(plan.start_word & 0x00FFU));
}

/* Function changes run HERE, not in the button task (softkey UI, 2026-10-06).
 * fpga_set_meter_mode() waits for the meter chip to restart: ~1.4 s, up to
 * ~4.5 s (F35), and called from the input task it froze every button for that
 * long. The UI now changes function at once and shows dashes; this task
 * applies the newest request, so a run of presses costs one transition. */
TaskHandle_t       meter_poll_handle;

static volatile int16_t   meter_mode_request = -1;

/* A function change asked of the poll task has not been taken up yet. With
 * fpga_meter_transition_busy(): the meter link is (about to be) switching. */
bool fpga_meter_mode_request_pending(void) { return meter_mode_request >= 0; }

void fpga_request_meter_mode(uint8_t submode)
{
    if (!meter_poll_handle) {            /* no poll task in this build */
        fpga_set_meter_mode(submode);
        return;
    }
    meter_mode_request = submode;
    xTaskNotifyGive(meter_poll_handle);
}

/* The poll task serves the meter while its link is on. The meter screen
 * counts by itself: a key's request (input_mode_switch, input task) arrives one
 * frame before the display task has turned the link on, and the screen
 * being shown always means the link is wanted (meter_link_policy.h). In an
 * image that cannot run the meter under another screen the screen is the
 * only condition, exactly as before (the link state trails a screen change
 * by up to a frame, and the scope side owns the wire from that change on). */
bool fpga_meter_link_background_capable(void);

static bool meter_link_serving(void)
{
    extern volatile device_mode_t current_mode;  /* from ui.h via main.c */
    return current_mode == MODE_MULTIMETER ||
           (meter_link_state == METER_LINK_ST_ON && fpga_meter_link_background_capable());
}

void fpga_meter_poll_task(void *pv)
{
    (void)pv;
    extern volatile uint8_t meter_submode;

    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));  /* ~4 Hz, or a request */
        int16_t req = meter_mode_request;
        if (req >= 0 && fpga.initialized) {
            meter_mode_request = -1;
            if (meter_link_serving()) {
                fpga_set_meter_mode((uint8_t)req);
                /* The link went off while the chip was switching? The display
                 * task already switched it off (and put the scope's front end
                 * back), and this transition just raised PC11 again. */
                if (!meter_link_serving())
                    fpga_set_meter_mux(false);
            }
            continue;
        }
        if (fpga.initialized && meter_link_serving() &&
            !meter_transition_busy) {
            if (fpga_meter_needs_activation) {
                /* Stock's meter activation, sent once on entry rather than at
                 * boot — a scope build never runs fpga_init's meter block. */
                fpga_meter_needs_activation = false;
                /* Unobeyed, same reason as fpga_init's copy: 0x0A selects
                 * Capacitance and 0x14 selects Auto when obeyed, and this
                 * runs right after the transition's real selector. */
                usart2_send_cmd_ex(0x05, 0x08, false);  vTaskDelay(pdMS_TO_TICKS(10));
                usart2_send_cmd_ex(0x05, 0x09, false);  vTaskDelay(pdMS_TO_TICKS(10));
                if (GPIOC->idt & (1U << 7)) usart2_send_cmd_ex(0x05, 0x07, false);
                else                        usart2_send_cmd_ex(0x05, 0x0A, false);
                vTaskDelay(pdMS_TO_TICKS(10));
                usart2_send_cmd_ex(0x05, 0x14, false);  vTaskDelay(pdMS_TO_TICKS(50));
            }
            if (meter_keepalive_enabled)
                fpga_send_meter_poll_sequence(meter_submode);
        }
    }
}

/* PC11 = meter MUX enable. Stock drives it HIGH in meter mode and LOW in scope
 * mode, switching WITH the UI mode; ours pinned it LOW unconditionally
 * (main.c, "PC11 LOW (scope)"), so the meter was dead in every scope build and
 * every USART measurement this project ever took was made with the far end
 * switched off. A/B/A on the bench: HIGH -> 276 bytes received, LOW -> 0,
 * HIGH -> 276. */
volatile bool fpga_meter_needs_activation = false;

static volatile bool meter_holds_fe;          /* mux(true) .. the scope's posture back          */

bool fpga_meter_holds_fe(void) { return meter_holds_fe; }

void fpga_meter_relays_set(bool on) { meter_moves_relays = on; }

bool fpga_meter_relays_get(void)    { return meter_moves_relays; }

void fpga_set_meter_mux(bool enable)
{
    GPIOC->cfghr = (GPIOC->cfghr & ~(0xFu << 12)) | (0x3u << 12);  /* PC11 PP 50MHz */
    if (enable) GPIOC->scr = (1u << 11);
    else        GPIOC->clr = (1u << 11);
    meter_soc_live_flag = false;       /* the SoC reboots on every PC11 rise */

    /* PC11 alone is necessary but NOT sufficient — bench 2026-08-17: with PC11
     * high the relay audibly clicks and the DMM screen still sits frozen,
     * because a coldtrace build leaves USART2 disabled and skips stock's meter
     * activation entirely (fpga_init bails before it, and the dvom/meter tasks
     * are not created). So entering meter mode must also raise the bus and ask
     * for the activation words; the poll task sends them, off the display
     * task's back. Stock's mirror pair is 0x0800E360 (enter) / 0x0800E3E4
     * (exit): UEN, dvom tasks, PC11, in that order. */
    fpga_usart_scope_enable(enable);
    if (enable) fpga_meter_needs_activation = true;

    /*
     * The analog frontend is SHARED. Once meter entry is allowed to posture it
     * per submode (see FPGA_METER_SUBMODES), scope entry has to put the scope's
     * posture back or the scope is wrong after any visit to the meter.
     *
     * Deliberately relays only: no USART sequence, no data_ready reset, and
     * nothing that touches the FPGA's configured/armed state. fpga_scope_reinit()
     * would do all three, and in a coldtrace build the arm state is the thing
     * that took two months to obtain.
     */
    if (!enable) {
        GPIOB->scr = PB11_MASK;   /* PB11 HIGH — FPGA active */
        fpga_set_scope_frontend_ranges(scope_state_get());
        /* The posture is the scope's again: the model re-sends the SPI3
         * registers (0x01/0x06/0x08) a set stored while the meter held the
         * front end (model_hw_fw.c), on the input task. Nothing is written
         * here, so a hop between non-meter screens costs nothing. */
        meter_holds_fe = false;
        fpga_scope_posture_gen++;
    } else {
        meter_holds_fe = meter_moves_relays;
    }
}

/* ── The meter link ───────────────────────────────────────────────────────
 *
 * PC11 (supply/enable of the isolated meter SoC; every rise restarts it,
 * ~1.4 s), USART2 and the poll task's serving, as one on/off resource. It
 * used to follow the meter screen; it now follows the answer of
 * meter_link_want_on() (meter_link_policy.h), which the display task
 * evaluates every frame and hands to fpga_meter_link_set(). That is the only
 * caller, so on/off edges come from one context.
 *
 * Edges, and what does not make one:
 *   - on  (off or unknown -> on): fpga_set_meter_mux(true), then the stored
 *     function is requested from the poll task, unless a request is already
 *     waiting, a transition for that function is in flight (one started
 *     since the link last went off), or the SoC is live on it. The key's
 *     meter entry and the boot path post their own request first, so for
 *     them this is one request and one transition, exactly as before.
 *   - on -> on: nothing at all. fpga_set_meter_mux(true) on a live link
 *     would zero USART2, forget that the SoC is live (the next function
 *     change would be a full restart) and re-send the activation words.
 *     So leaving the meter screen with the link still wanted, and entering
 *     it with the link already on, move neither PC11 nor USART2.
 *   - off (on or unknown -> off): fpga_set_meter_mux(false), as every entry
 *     to another screen did. The first evaluation after boot always runs
 *     one of the two, as the display task's first pass always did.
 */
bool fpga_meter_link_on(void) { return meter_link_state == METER_LINK_ST_ON; }

/* Whether this image can keep the meter running under another screen: the
 * scope side must be silent on USART2 (it shares the wire with the meter
 * SoC), and the meter tasks and function changes must exist. Elsewhere the
 * link follows the screen, whatever the policy says. */
bool fpga_meter_link_background_capable(void)
{
    return true;
}

/* The stored function is what the SoC runs (or is being switched to). */
static bool meter_link_function_covered(uint8_t submode)
{
    if (meter_mode_request >= 0) return true;                       /* the newest request wins */
    if (meter_transition_busy) return meter_transition_target == submode;
    return fpga_meter_soc_live() && fpga.meter_mode_sequence_submode == submode;
}

void fpga_meter_link_set(bool on)
{
    extern volatile uint8_t meter_submode;

    if (on) {
        if (meter_link_state == METER_LINK_ST_ON) return;
        const uint8_t submode = meter_submode;
        const bool covered = meter_link_function_covered(submode);   /* before mux(true) forgets "live" */
        meter_link_state = METER_LINK_ST_ON;      /* before the request: the poll task drops one while off */
        fpga_set_meter_mux(true);
        /* Without a poll task the request would run the whole transition
         * here, on the display task; those images get the function from
         * input_mode_switch's own call, as before. */
        if (!covered && meter_poll_handle != NULL) fpga_request_meter_mode(submode);
    } else {
        if (meter_link_state == METER_LINK_ST_OFF) return;
        meter_link_state = METER_LINK_ST_OFF;
        /* A transition in flight is about to lose PC11 and USART2: it no
         * longer counts as "the function is being applied", so the next
         * link-on asks again (served once this one has run out). A
         * transition started after this point, by a meter entry's own
         * request, counts as before. */
        if (meter_transition_busy) meter_transition_target = 0xFF;
        fpga_set_meter_mux(false);
    }
}

/* The meter screen was entered (input task, input_mode_switch). With the link off
 * this is the request it always posted. With the link already on and the
 * SoC running the stored function there is nothing to do: no PC11 edge, no
 * selector word. A link that is on but not live (a transition that timed
 * out) gets its retry here, as every meter entry used to give one. */
void fpga_meter_screen_entered(uint8_t submode)
{
    if (meter_link_state == METER_LINK_ST_ON && meter_link_function_covered(submode))
        return;
    fpga_request_meter_mode(submode);
}

/* ── USART2 late bring-up (2026-08-17) ───────────────────────────────────
 *
 * Coldtrace builds define FPGA_USART_SILENT_SCOPE, which leaves USART2 fully
 * disabled (UEN clear) so the wire is electrically dark. That was necessary for
 * the config-entry experiments and is now probably obsolete: config entry is
 * solved, and stock's own order is USART2 DISABLED during config (Exp E dumped
 * CTRL1=0x0000002c at the CONFIG_ENABLE instant — RE/TE/RDBFIEN set, UEN clear)
 * and enabled only afterwards.
 *
 * Why this matters for CH2: stock's documented command table puts SCOPE CHANNEL
 * CONFIGURATION on USART2, not SPI3 — cmd 0x01 is "Configure channel, Type 0
 * (CH1) / Type 1 (CH2)", and 0x0B-0x11 are channel/trigger/timebase
 * (FPGA_TASK_ANALYSIS.md). We have spent a month hunting a channel selector on
 * SPI3 while the command that names itself "configure channel" lives on a bus
 * this build switches off.
 *
 * A previous attempt brought USART2 up by hand-poking CTRL1 from the shell and
 * saw no echo frame return. PA2 is configured as AF push-pull even under the
 * silent flag, so that was not a pin artifact — but a hand poke is a weak
 * instrument, so this does the bring-up the same way fpga_init does and REPORTS
 * THE REGISTERS BACK so the precondition is verified rather than assumed. */
void fpga_usart_scope_enable(bool on)
{
    if (on) {
        USART2->ctrl1 = 0;
        USART2->baudr = system_core_clock / 2 / FPGA_USART_BAUD;
        USART2->ctrl1 |= (1 << 2);   /* RE      */
        USART2->ctrl1 |= (1 << 3);   /* TE      */
        USART2->ctrl1 |= (1 << 5);   /* RDBFIEN */
        USART2->ctrl1 |= (1 << 13);  /* UEN     */
        NVIC_SetPriority(USART2_IRQn, 5);
        NVIC_EnableIRQ(USART2_IRQn);
    } else {
        NVIC_DisableIRQ(USART2_IRQn);
        USART2->ctrl1 = 0;
    }
}

uint32_t fpga_usart_ctrl1(void) { return USART2->ctrl1; }

uint32_t fpga_usart_baudr(void) { return USART2->baudr; }

/* ═══════════════════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════════════════ */

BaseType_t fpga_send_cmd(uint8_t cmd_high, uint8_t cmd_low)
{
    if (!fpga.initialized) return pdFALSE;

    /* Use interrupt-driven TX via the dvom_TX task queue.
     * Non-blocking send — don't stall the calling task.
     * The scheduler-state check matters (audit 2026-08-20, P0.5): between
     * fpga_create_tasks() and vTaskStartScheduler() the queue exists with
     * no consumer running, and a sequence longer than its depth of 10 would
     * be cut short without a sign. Before the scheduler, send polled
     * instead (safe: the ISR TX engine cannot be mid-frame before the
     * scheduler starts). fpga_timed_send_cmd() carries this same guard. */
    if (usart_tx_queue != NULL &&
        xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        uint32_t item = ((uint32_t)cmd_high << 8) | cmd_low | FPGA_TX_OBEY_BIT;
        return xQueueSend(usart_tx_queue, &item, 0);  /* non-blocking */
    }
    /* Fallback to polled if queue not created yet / scheduler not started */
    usart2_send_cmd(cmd_high, cmd_low);
    return pdTRUE;
}

BaseType_t fpga_send_cmd_unobeyed(uint8_t cmd_high, uint8_t cmd_low)
{
    if (usart_tx_queue != NULL &&
        xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        uint32_t item = ((uint32_t)cmd_high << 8) | cmd_low;  /* OBEY clear */
        return xQueueSend(usart_tx_queue, &item, 0);
    }
    return pdFALSE;  /* best-effort; never block the caller */
}

BaseType_t fpga_send_cmd_keepalive(uint8_t cmd_high, uint8_t cmd_low)
{
    return fpga_send_cmd_unobeyed(cmd_high, cmd_low);
}

bool fpga_usart_tx_task_exists(void)
{
    return tx_task_handle != NULL;
}

void fpga_send_cmd_direct(uint8_t cmd_high, uint8_t cmd_low)
{
    /* Straight to the polled byte pump — no queue, so nothing can swallow it
     * on a build that never creates dvom_TX. */
    usart2_send_cmd(cmd_high, cmd_low);
}

/*
 * THE SoC RESTARTS ON EVERY TRANSITION, AND IT IS DEAF WHILE IT BOOTS.
 *
 * Measured on unit #2 with the AA 55 header live (2026-09-13, EXP-205):
 * fpga_meter_reset_transport() drops PC11, and the meter SoC comes back in its
 * power-on Auto state -- the frame after a transition is the boot-time "Auto"
 * text again, whatever function had been selected before. Then the selector
 * sent 20 ms later is swallowed: `AA 55 05 0B` left the wire (tx_control
 * history) and no echo ever came back, while not one data frame arrived
 * during the whole sequence (transition history data=558..558). The same word
 * typed by hand a second later was echoed at once and switched the function.
 *
 * So the transition waits for the SoC to speak before it commands it, and it
 * does not take the selector on trust: it re-sends until the echo confirms it.
 * The wait feeds unobeyed keepalive traffic, since a silent wire is the other
 * documented way to get no frames (EXP-26). Numbers are exported for
 * `meter hdr`, so the wake time and the retry count are measurements, not
 * assumptions.
 */
#define FPGA_METER_SOC_WAKE_TIMEOUT_MS   3000u  /* measured wake 1380 ms; one

                                                   * boot->meter entry missed 1500 */
#define FPGA_METER_SOC_WAKE_SETTLE_MS     200u
#define FPGA_METER_SOC_WAKE_POLL_MS        10u
#define FPGA_METER_SELECTOR_ECHO_WAIT_MS  300u
#define FPGA_METER_SELECTOR_TRIES           5u

static void fpga_meter_wait_for_soc(uint16_t frame_before)
{
    uint32_t waited = 0;
    uint32_t since_keepalive = 0;

    fpga.meter_soc_wake_ms = 0xFFFFu;  /* timeout until proven otherwise */
    while (waited < FPGA_METER_SOC_WAKE_TIMEOUT_MS) {
        if (fpga.frame_count != frame_before) {
            fpga.meter_soc_wake_ms = (uint16_t)waited;
            break;
        }
        if (waited + FPGA_METER_SOC_WAKE_POLL_MS >= FPGA_METER_SOC_WAKE_TIMEOUT_MS) {
            fpga.meter_soc_wake_timeouts++;
        }
        if (since_keepalive >= 250u) {
            (void)fpga_send_cmd_unobeyed(0x05, FPGA_CMD_METER_START);
            since_keepalive = 0;
        }
        fpga_scope_delay_ms(FPGA_METER_SOC_WAKE_POLL_MS);
        waited += FPGA_METER_SOC_WAKE_POLL_MS;
        since_keepalive += FPGA_METER_SOC_WAKE_POLL_MS;
    }
    fpga_scope_delay_ms(FPGA_METER_SOC_WAKE_SETTLE_MS);
}

/* Send the selector OBEYED and wait for its echo; retry a bounded number of
 * times. Leaves the outcome in fpga.meter_selector_attempts / _confirmed. */
static void fpga_send_selector_confirmed(uint16_t word, uint32_t settle_ms)
{
    fpga.meter_selector_attempts = 0;
    fpga.meter_selector_confirmed = 0;
    for (uint8_t attempt = 0; attempt < FPGA_METER_SELECTOR_TRIES; attempt++) {
        uint16_t echo_before = fpga.rx_echo_valid_count;
        uint32_t waited = 0;

        fpga.meter_selector_attempts++;
        fpga_wire_send_word(word, 0);
        while (waited < FPGA_METER_SELECTOR_ECHO_WAIT_MS) {
            if (fpga.rx_echo_valid_count != echo_before) {
                fpga.meter_selector_confirmed = 1;
                break;
            }
            fpga_scope_delay_ms(FPGA_METER_SOC_WAKE_POLL_MS);
            waited += FPGA_METER_SOC_WAKE_POLL_MS;
        }
        if (fpga.meter_selector_confirmed) break;
    }
    if (!fpga.meter_selector_confirmed) {
        fpga.meter_selector_unconfirmed_total++;
    }
    fpga_scope_delay_ms(settle_ms);
}

/* Helper: send the stock PC7-gated command tail (shared by meter modes). */
static void fpga_timed_send_probe_detect(uint32_t delay_ms)
{
    fpga_timed_send_cmd(0x05, fpga_probe_cmd_byte(), delay_ms);
}

static void fpga_send_meter_mode_sequence(uint8_t submode)
{
    fpga_meter_transition_plan_t plan =
        fpga_meter_transition_plan_for_submode(submode);
    uint16_t probe_word = (uint16_t)(0x0500U | fpga_probe_cmd_byte());

    if (!fpga_meter_submode_is_valid(submode)) {
        fpga.meter_mode_sequence_count++;
        fpga.meter_mode_sequence_submode = submode;
        fpga.meter_mode_config_word = 0;
        fpga.meter_mode_selector_word = FPGA_METER_INVALID_SELECTOR_WORD;
        fpga.meter_mode_apply_word = 0;
        fpga.meter_mode_probe_word = 0;
        fpga.meter_mode_start_word = 0;
        return;
    }

    fpga.meter_mode_sequence_count++;
    fpga.meter_mode_sequence_submode = submode;
    fpga.meter_mode_config_word = 0;
    fpga.meter_mode_selector_word = plan.selector_word;
    fpga.meter_mode_apply_word = plan.apply_word;  /* always 0 now, see plan */
    fpga.meter_mode_probe_word = plan.has_probe_detect ? probe_word : 0;
    fpga.meter_mode_start_word = plan.start_word;
    /*
     * USART2 selector/start sequence.
     * Stock evidence proves raw meter command words in the 0x05xx family and
     * 10-tick command pacing. The guarded basic materializers are 0x0508
     * (0x080033CA), 0x0509 (0x08003BA4), and 0x0514 (0x08005B7A); selector and
     * apply words are separate digital state-machine evidence. This does not
     * prove that every local submode owns a unique physical selector. The
     * optional apply word mirrors recovered family-switch words (ACV, DCA,
     * continuity, diode). The fixed settle/discard constants are conservative
     * local policy and are reported in debug output instead of being hidden as
     * stock fact.
     */
    if (plan.has_command_bank_prefix) {
        fpga_timed_send_cmd(0x00, plan.command_bank_first, plan.settle_ms);
        fpga_timed_send_cmd(0x00, plan.command_bank_second, plan.settle_ms);
    }
    if (plan.has_config_word) {
        fpga.meter_mode_config_word = plan.config_word;
        fpga_timed_send_cmd((uint8_t)(plan.config_word >> 8),
                            (uint8_t)(plan.config_word & 0x00FFU),
                            plan.settle_ms);
    }
    /*
     * THE ONE OBEYED WORD of a transition. Everything around it (bank prefix,
     * 0x0508, the probe tail, 0x0509) keeps the 00 00 header it always had:
     * the probe tail's low branch is 0x0A, which is the SoC's Capacitance
     * selector, and 0x0509 puts the SoC's display into a state that is not a
     * measurement. Obeyed, either would undo the selector a few ms later.
     */
    fpga_send_selector_confirmed(plan.selector_word, plan.settle_ms);
    if (plan.has_probe_detect) {
        fpga_timed_send_probe_detect(10);
    }
    fpga_timed_send_cmd((uint8_t)(plan.start_word >> 8),
                        (uint8_t)(plan.start_word & 0x00FFU),
                        plan.settle_ms);
}

static void fpga_apply_meter_transition(uint8_t submode, bool wake_preamble)
{
    fpga_meter_transition_plan_t plan =
        fpga_meter_transition_plan_for_submode(submode);
    fpga_meter_mux_gpio_state_t planned_mux;
    uint16_t planned_gpio;
    uint16_t actual_gpio;
    uint16_t tx_before;
    uint16_t frame_before;

    meter_transition_target = submode;
    meter_transition_busy = true;

    meter_data_invalidate(submode);
    if (!fpga_meter_submode_is_valid(submode)) {
        meter_transition_busy = false;
        return;
    }

    /*
     * Single production DMM transition path.
     *
     * Stock evidence proves pause/drain/resume transport shape, selector/apply
     * 0x05xx words, probe/start tail, and saved-state mux writer bodies. Exact
     * settle/discard counts and the runtime analog range writer remain open,
     * so both normal mode switches and reinit use the same conservative local
     * ordering here instead of carrying two handwritten sequences that can
     * diverge around the missing evidence boundary.
     */
    fpga_meter_reset_transport();
    if (wake_preamble) {
        fpga_send_meter_wake_preamble();
    }
    (void)fpga_meter_mux_gpio_state_for_submode(submode, &planned_mux);
    planned_gpio = fpga_meter_mux_gpio_mask_from_state(&planned_mux);
    tx_before = fpga.tx_count;
    frame_before = fpga.frame_count;
    fpga_set_meter_frontend_for_submode(submode);
    actual_gpio = fpga_meter_mux_gpio_mask_live();
    fpga_scope_delay_ms(plan.settle_ms);
    fpga_meter_wait_for_soc(frame_before);
    fpga_send_meter_mode_sequence(submode);
    fpga_record_meter_transition_snapshot(submode, &plan, planned_gpio,
                                          actual_gpio, tx_before, frame_before);
    fpga_arm_meter_first_rx_latch(submode, &plan, planned_gpio, actual_gpio);
    fpga_meter_discard_next_frames(plan.discard_frames);
    /* The SoC spoke (it woke) and took the selector: later function changes
     * can be one word on the live link. */
    meter_soc_live_flag = (fpga.meter_soc_wake_ms != 0xFFFFu) &&
                          fpga.meter_selector_confirmed;
    meter_transition_busy = false;
}

void fpga_meter_fast_switch_set(bool on) { meter_fast_switch_enabled = on; }

bool fpga_meter_fast_switch_get(void)    { return meter_fast_switch_enabled; }

void fpga_meter_keepalive_set(bool on)   { meter_keepalive_enabled = on; }

bool fpga_meter_keepalive_get(void)      { return meter_keepalive_enabled; }

bool fpga_meter_soc_live(void)
{
    /* The flag alone is not enough: a PC11 drop anywhere (scope entry, siggen,
     * fpga_set_meter_mux(false)) leaves the SoC rebooting. Read the pin. */
    return meter_soc_live_flag && (GPIOC->odt & (1U << 11)) != 0U;
}

/*
 * M1: ONE word per function change, like stock.
 *
 * Stock (caplog run2/run3, 0x0800B2D4 left handler, 0x0800D2A2 MOVE): a
 * function change queues one halfword; dvom_TX frames it as
 * AA 55 05 <word> 00 00 00 00 00 <cs> and that is all. PC11 stays high, no
 * frontend pin moves (relay switching is inside the SoC, EXP-205 5e), the SoC
 * does not restart and no keepalive or start word follows. OpenScope used to
 * drop PC11 on every change: a 1.38 s SoC reboot and a mux projection.
 *
 * Returns false (nothing is left half-done) when a fast switch is not
 * possible or the SoC never echoed the word, and the caller runs the full
 * transition, which is the safety net.
 */
static bool fpga_meter_fast_switch(uint8_t submode)
{
    fpga_meter_transition_plan_t plan =
        fpga_meter_transition_plan_for_submode(submode);

    if (!meter_fast_switch_enabled || !fpga_meter_soc_live()) return false;
    if (!fpga_meter_submode_is_valid(submode) ||
        plan.selector_word == FPGA_METER_INVALID_SELECTOR_WORD ||
        meter_word_is_cal_wizard(plan.selector_word)) {
        return false;
    }

    meter_transition_target = submode;
    meter_transition_busy = true;
    meter_data_invalidate(submode);
    fpga.meter_mode_sequence_count++;
    fpga.meter_mode_sequence_submode = submode;
    fpga.meter_mode_selector_word = plan.selector_word;
    fpga.meter_mode_config_word = 0;
    fpga.meter_mode_apply_word = 0;
    fpga.meter_mode_probe_word = 0;
    fpga.meter_mode_start_word = 0;

    fpga_send_selector_confirmed(plan.selector_word, 0);

    if (!fpga.meter_selector_confirmed) {
        fpga.meter_fast_fallback_count++;
        meter_transition_busy = false;
        return false;
    }
    fpga_meter_discard_next_frames(plan.discard_frames);
    fpga.meter_fast_switch_count++;
    meter_transition_busy = false;
    return true;
}

void fpga_set_meter_mode(uint8_t submode)
{
    if (!fpga.initialized) return;

    /* The signal generator keeps running: it is on DAC2/PA5, clear of the
     * scope's DAC1, and stock never stops it on a mode change (its apply
     * 0x08029D40 has no mode-switch caller; the manual has you leave it
     * running and look at it in scope mode). */

    if (fpga_meter_fast_switch(submode)) return;
    fpga_apply_meter_transition(submode, false);
}

void fpga_meter_reinit(uint8_t submode)
{
    if (!fpga.initialized) return;
    fpga_apply_meter_transition(submode, true);
}
