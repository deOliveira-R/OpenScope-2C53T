/*
 * OpenScope 2C53T - the link to the meter chip (USART2)
 *
 * The multimeter is a separate system-on-chip, the maker's part ("the meter
 * chip", "SoC" in names). The MCU talks to it on USART2 at 9600 baud:
 * 10-byte command frames out; 12-byte data frames (header 5A A5) and 10-byte
 * echo frames (header AA 55) in, on one stream. PC11 high enables the chip's
 * side of the link, and every rise of PC11 restarts the chip.
 *
 * The chip acts on a command frame only when its header is AA 55: we call
 * such a frame "obeyed". With header 00 00 the chip ignores the command but
 * still answers the traffic with data frames, which the keepalive relies on.
 *
 * This file builds, sends and receives the frames, changes the meter
 * function, drives the input multiplexer pins and switches the link on and
 * off. It also fills diagnostic history rings and has debug helpers for the
 * shell. Shared items: fpga_internal.h.
 *
 * How the parts work together:
 *   - Senders put items (a command word, obeyed or unobeyed, or a raw
 *     frame) on usart_tx_queue. Nothing else reaches the wire.
 *   - fpga_usart_tx_task ("dvom_TX") takes one item, builds its frame in
 *     link_tx_frame, enables the TX interrupt and waits for the handler's
 *     signal that the frame is out.
 *   - USART2_IRQHandler sends the TX bytes and assembles received bytes into
 *     frames. A data frame goes to meter_rx_queue as a copied event; an echo
 *     frame only updates the echo counters.
 *   - fpga_usart_rx_task ("dvom_RX") takes data frames from meter_rx_queue
 *     and hands them to meter_data, skipping those that arrive during a
 *     function change and a few after it.
 *   - fpga_meter_poll_task runs the function changes other tasks request
 *     (one can take seconds) and sends the 4 Hz keepalive.
 */

#include "fpga_internal.h"

meter_link_state_t mlink;

/* The frame being sent and the position in it (dvom_TX fills, the USART2
 * handler sends), and the frame being received (the handler only). */
static volatile uint8_t link_tx_frame[FPGA_TX_FRAME_SIZE];
static volatile uint8_t link_tx_index;
static volatile uint8_t link_rx_buf[FPGA_RX_FRAME_SIZE];
static volatile uint8_t link_rx_index;

/* Low byte of the last frame sent obeyed: the echo check compares against
 * this, not last_tx_frame, which the unobeyed keepalive overwrites between
 * a command and its echo. */
static volatile uint8_t link_last_obeyed_tx_lo;
static volatile bool link_last_obeyed_tx_valid;

/* Times of the link. None of these is a measured limit of the chip; they are
 * the values the link has run with, named so that they are in one place. */
#define METER_KEEPALIVE_PERIOD_MS   250u   /* the unobeyed keepalive: 4 per second */
#define METER_LINK_RESET_QUIET_MS    20u   /* PC11 low and USART2 off during a link reset */
#define METER_FRONTEND_SETTLE_MS     20u   /* after the relay pins are written, before the wake words */
#define METER_WAKE_WORD_GAP_MS       10u   /* after each wake word */
#define METER_WAKE_END_GAP_MS        20u   /* after the last wake word of a transition */
#define METER_ACTIVATION_END_GAP_MS  50u   /* after the last wake word of a link-on */
#define METER_DEBUG_MAX_DELAY_MS   5000u   /* longest delay a debug helper accepts */

/* USART ctrl1 bits, as raw masks (the AT32 HAL only has MAKE_VALUE forms). */
#define USART_CTRL1_RDBFIEN   (1 << 5)   /* RX buffer full interrupt enable */

#define USART_CTRL1_TDBEIEN   (1 << 7)   /* TX buffer empty interrupt enable */

#define USART_CTRL1_UEN       (1 << 13)  /* USART enable */

static void fpga_send_meter_mode_sequence(uint8_t submode);

/* Shell override of the probe tail byte (0x07 or 0x0A); -1 reads PC7. */
volatile int16_t  fpga_meter_probe_tail_override = -1;

/* FreeRTOS handles */
QueueHandle_t     usart_tx_queue  = NULL;  /* meter_tx_item_t; dvom_TX is its only reader */

QueueHandle_t meter_rx_queue = NULL;  /* fpga_meter_rx_event_t: a copied data frame */

TaskHandle_t      tx_task_handle  = NULL;

TaskHandle_t      rx_task_handle  = NULL;

static volatile bool meter_transition_busy = false;

static volatile bool meter_moves_relays;      /* `meter relays on`: meter entry also moves the scope's range relays */

/* Shell switches: see fpga_meter_fast_switch_set() and fpga_meter_keepalive_set(). */
static volatile bool meter_fast_switch_enabled = true;

static volatile bool meter_keepalive_enabled = true;

/* Set when a full transition ended with the chip awake and the selector
 * echoed; cleared on every PC11 edge. */
static volatile bool meter_soc_live_flag = false;

/* The meter link (fpga_meter_link_set): unknown until the display task's
 * first evaluation, then off or on. */
enum { METER_LINK_ST_UNKNOWN = 0, METER_LINK_ST_OFF, METER_LINK_ST_ON };

static volatile uint8_t meter_link_state = METER_LINK_ST_UNKNOWN;

/* The function the transition in flight is for (0xFF: none that counts), so a
 * link-on that finds one running need not ask again. */
static volatile uint8_t meter_transition_target = 0xFF;

volatile uint8_t meter_frame_discard_count;            /* data frames still to drop after a change */

volatile uint32_t meter_transition_frame_skip_count;   /* data frames dropped during a change */

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
     * Drain and reset the link before a full transition, in stock's order:
     * disable USART2 and its interrupts, suspend dvom_TX and dvom_RX, empty
     * both queues and the byte indices, drop PC11 (the chip restarts), then
     * enable USART2 again and resume the tasks. The 20 ms quiet window is
     * conservative local policy, not a stock number. (tx_index = 0 holds back
     * data frames until dvom_TX has sent a frame: see USART2_IRQHandler.)
     */
    ctrl1 = USART2->ctrl1;
    USART2->ctrl1 = ctrl1 & ~(USART_CTRL1_UEN |
                              USART_CTRL1_RDBFIEN |
                              USART_CTRL1_TDBEIEN);

    if (tx_task_handle != NULL) vTaskSuspend(tx_task_handle);
    if (rx_task_handle != NULL) vTaskSuspend(rx_task_handle);

    if (usart_tx_queue != NULL) xQueueReset(usart_tx_queue);
    if (meter_rx_queue != NULL) xQueueReset(meter_rx_queue);

    link_tx_index = 0;
    link_rx_index = 0;
    mlink.rx_frame_valid = false;
    (void)USART2->sts;
    (void)USART2->dt;

    GPIOC->clr = (1U << 11);
    meter_soc_live_flag = false;       /* the chip restarts when PC11 rises again */
    fpga_scope_delay_ms(METER_LINK_RESET_QUIET_MS);

    USART2->ctrl1 = (ctrl1 | USART_CTRL1_UEN | USART_CTRL1_RDBFIEN) &
                    ~USART_CTRL1_TDBEIEN;
    /*
     * Always set RE and TE and unmask the interrupt here. With the link off,
     * USART2 sits at ctrl1 = 0 with the interrupt masked
     * (fpga_usart_scope_enable(false)). A transition that runs before the
     * link-on has re-armed it would otherwise restore UEN on a dead
     * transceiver: the TX count climbs, nothing reaches the wire, and every
     * selector try times out (measured on unit #2: CTRL1 0x20A0 when it
     * failed, 0x202C when good).
     */
    USART2->ctrl1 |= (1U << 2) | (1U << 3);   /* RE, TE */
    NVIC_SetPriority(USART2_IRQn, 5);
    NVIC_EnableIRQ(USART2_IRQn);

    if (rx_task_handle != NULL) vTaskResume(rx_task_handle);
    if (tx_task_handle != NULL) vTaskResume(tx_task_handle);
}

/* ═══════════════════════════════════════════════════════════════════
 * Diagnostic history rings, read by the shell. Nothing here may feed
 * a range or value decision.
 * ═══════════════════════════════════════════════════════════════════ */

static void fpga_record_tx_cmd(uint8_t cmd_hi, uint8_t cmd_lo)
{
    uint8_t idx = mlink.tx_cmd_history_head & 0x0F;

    mlink.tx_cmd_hi_history[idx] = cmd_hi;
    mlink.tx_cmd_lo_history[idx] = cmd_lo;
    mlink.tx_cmd_history_head = (uint8_t)((idx + 1U) & 0x0F);
    if (mlink.tx_cmd_history_count < 16U) {
        mlink.tx_cmd_history_count++;
    }
}

static void fpga_record_tx_frame(const uint8_t *frame)
{
    uint8_t idx = mlink.tx_frame_history_head;

    memcpy((void *)mlink.last_tx_frame, frame, FPGA_TX_FRAME_SIZE);
    memcpy((void *)mlink.tx_frame_history[idx], frame, FPGA_TX_FRAME_SIZE);
    mlink.tx_frame_history_tx_count[idx] = mlink.tx_count;
    mlink.tx_frame_history_head =
        (uint8_t)((idx + 1U) % FPGA_TX_FRAME_HISTORY);
    if (mlink.tx_frame_history_count < FPGA_TX_FRAME_HISTORY) {
        mlink.tx_frame_history_count++;
    }

    /* A second ring for every frame except the 0x0509 keepalive, which would
     * otherwise push the last selector and setup words out of the first. */
    if (frame[2] != 0x05U || frame[3] != FPGA_CMD_METER_START) {
        idx = mlink.tx_control_frame_history_head;
        memcpy((void *)mlink.tx_control_frame_history[idx], frame,
               FPGA_TX_FRAME_SIZE);
        mlink.tx_control_frame_history_tx_count[idx] = mlink.tx_count;
        mlink.tx_control_frame_history_head =
            (uint8_t)((idx + 1U) % FPGA_TX_FRAME_HISTORY);
        if (mlink.tx_control_frame_history_count < FPGA_TX_FRAME_HISTORY) {
            mlink.tx_control_frame_history_count++;
        }
    }
}

static void fpga_record_rx_raw_byte(uint8_t byte, uint8_t rx_index_before)
{
    uint8_t idx = mlink.rx_raw_history_head;

    mlink.rx_raw_history[idx] = byte;
    mlink.rx_raw_history_tx_count[idx] = mlink.tx_count;
    mlink.rx_raw_history_tx_index[idx] = link_tx_index;
    mlink.rx_raw_history_rx_index[idx] = rx_index_before;
    mlink.rx_raw_history_head =
        (uint8_t)((idx + 1U) % FPGA_RX_RAW_HISTORY);
    if (mlink.rx_raw_history_count < FPGA_RX_RAW_HISTORY) {
        mlink.rx_raw_history_count++;
    }
}

static void fpga_record_rx_data_frame(void)
{
    uint8_t idx = mlink.rx_frame_history_head;

    /* Every complete data frame as the interrupt handler receives it, with
     * the link counters of that moment, including frames dvom_RX will skip. */
    memcpy((void *)mlink.rx_frame_history[idx], (const void *)mlink.rx_frame,
           FPGA_RX_FRAME_SIZE);
    mlink.rx_history_frame_count[idx] = mlink.last_rx_frame_count;
    mlink.rx_history_tx_count[idx] = mlink.last_rx_tx_count;
    mlink.rx_history_echo_count[idx] = mlink.last_rx_echo_count;
    mlink.rx_history_sequence_count[idx] = mlink.last_rx_mode_sequence_count;
    mlink.rx_history_sequence_submode[idx] = mlink.last_rx_mode_sequence_submode;
    mlink.rx_history_discard_remaining[idx] = mlink.last_rx_discard_remaining;
    mlink.rx_history_transition_busy[idx] = mlink.last_rx_transition_busy;
    mlink.rx_frame_history_head =
        (uint8_t)((idx + 1U) % FPGA_RX_FRAME_HISTORY);
    if (mlink.rx_frame_history_count < FPGA_RX_FRAME_HISTORY) {
        mlink.rx_frame_history_count++;
    }
}

/* The ten multiplexer pins as a bit mask (bit 0 PC12 .. bit 9 PA6): planned
 * from a state, or read live from the pins. */
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
    uint8_t idx = mlink.meter_transition_history_head;

    /* One entry per full transition: the planned and the read-back
     * multiplexer pins, the words sent, and the TX and data frame counts
     * before and after the sequence. */
    mlink.meter_transition_history_submode[idx] = submode;
    mlink.meter_transition_history_config[idx] =
        plan->has_config_word ? plan->config_word : 0;
    mlink.meter_transition_history_selector[idx] = plan->selector_word;
    mlink.meter_transition_history_apply[idx] =
        plan->has_apply_word ? plan->apply_word : 0;
    mlink.meter_transition_history_bank[idx] =
        plan->has_command_bank_prefix ? 1U : 0U;
    mlink.meter_transition_history_bank_first[idx] =
        plan->has_command_bank_prefix ? plan->command_bank_first : 0U;
    mlink.meter_transition_history_bank_second[idx] =
        plan->has_command_bank_prefix ? plan->command_bank_second : 0U;
    mlink.meter_transition_history_probe[idx] =
        plan->has_probe_detect ?
        (uint16_t)((GPIOC->idt & (1U << 7)) ? 0x0507U : 0x050AU) : 0;
    mlink.meter_transition_history_start[idx] = plan->start_word;
    mlink.meter_transition_history_sequence_count[idx] =
        mlink.meter_mode_sequence_count;
    mlink.meter_transition_history_tx_before[idx] = tx_before;
    mlink.meter_transition_history_tx_after[idx] = mlink.tx_count;
    mlink.meter_transition_history_frame_before[idx] = frame_before;
    mlink.meter_transition_history_frame_after[idx] = mlink.frame_count;
    mlink.meter_transition_history_planned_gpio[idx] = planned_gpio;
    mlink.meter_transition_history_actual_gpio[idx] = actual_gpio;
    mlink.meter_transition_history_head =
        (uint8_t)((idx + 1U) % FPGA_METER_TRANSITION_HISTORY);
    if (mlink.meter_transition_history_count < FPGA_METER_TRANSITION_HISTORY) {
        mlink.meter_transition_history_count++;
    }
}

static void fpga_arm_meter_first_rx_latch(
    uint8_t submode,
    const fpga_meter_transition_plan_t *plan,
    uint16_t planned_gpio,
    uint16_t actual_gpio)
{
    /* Arm a one-shot latch: the interrupt handler keeps the first data frame
     * after this transition together with the transition's words and pins
     * (fpga_capture_meter_first_rx_latch). */
    mlink.meter_first_rx_after_transition_valid = 0;
    mlink.meter_first_rx_after_transition_submode = submode;
    mlink.meter_first_rx_after_transition_seq = mlink.meter_mode_sequence_count;
    mlink.meter_first_rx_after_transition_config =
        plan->has_config_word ? plan->config_word : 0;
    mlink.meter_first_rx_after_transition_selector = plan->selector_word;
    mlink.meter_first_rx_after_transition_apply =
        plan->has_apply_word ? plan->apply_word : 0;
    mlink.meter_first_rx_after_transition_probe =
        plan->has_probe_detect ?
        (uint16_t)(0x0500U | fpga_probe_cmd_byte()) : 0;
    mlink.meter_first_rx_after_transition_start = plan->start_word;
    mlink.meter_first_rx_after_transition_planned_gpio = planned_gpio;
    mlink.meter_first_rx_after_transition_actual_gpio = actual_gpio;
    mlink.meter_first_rx_after_transition_armed = 1;
}

static void fpga_capture_meter_first_rx_latch(void)
{
    if (mlink.meter_first_rx_after_transition_armed == 0) {
        return;
    }

    memcpy((void *)mlink.meter_first_rx_after_transition_frame,
           (const void *)mlink.rx_frame, FPGA_RX_FRAME_SIZE);
    mlink.meter_first_rx_after_transition_data = mlink.last_rx_frame_count;
    mlink.meter_first_rx_after_transition_tx = mlink.last_rx_tx_count;
    mlink.meter_first_rx_after_transition_echo = mlink.last_rx_echo_count;
    mlink.meter_first_rx_after_transition_busy = mlink.last_rx_transition_busy;
    mlink.meter_first_rx_after_transition_discard =
        mlink.last_rx_discard_remaining;
    mlink.meter_first_rx_after_transition_valid = 1;
    mlink.meter_first_rx_after_transition_armed = 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * Command frames
 *
 * 10 bytes: [hdr0][hdr1] [cmd_hi][cmd_lo] [0 0 0 0 0] [checksum],
 * checksum = (cmd_hi + cmd_lo) & 0xFF. Header AA 55 when obeyed (the chip
 * echoes the word and acts on it), else 00 00 (measured on units #1 and #2).
 * `meter hdr off` sends even obeyed frames with 00 00, as a bench negative
 * control: a transition must then get no echo and change nothing.
 * ═══════════════════════════════════════════════════════════════════ */
static volatile bool meter_tx_header_aa55 = true;

void fpga_meter_tx_header_set(bool aa55) { meter_tx_header_aa55 = aa55; }

bool fpga_meter_tx_header_get(void)      { return meter_tx_header_aa55; }

/*
 * The one place a command frame is built (dvom_TX calls it), so the header
 * switch and the guard below apply to every word. Only a raw frame of the
 * toolkit's `usart raw` goes out as typed.
 */
static void meter_build_tx_frame(uint8_t *frame, uint8_t cmd_hi, uint8_t cmd_lo,
                                 bool obey)
{
    /*
     * Guard: 0x02A0, 0x0503 and 0x0501 belong to stock's hidden calibration
     * sequence for the meter chip; obeyed, they may rewrite its calibration.
     * Never send them obeyed. An obeyed request for one goes out with the
     * 00 00 header instead and is counted, whoever asked for it.
     */
    if (obey && meter_word_is_cal_wizard((uint16_t)(((uint16_t)cmd_hi << 8) | cmd_lo))) {
        obey = false;
        mlink.meter_cal_word_refused++;
        mlink.meter_cal_word_last = (uint16_t)(((uint16_t)cmd_hi << 8) | cmd_lo);
    }
    memset(frame, 0, FPGA_TX_FRAME_SIZE);
    if (meter_tx_header_aa55 && obey) {
        frame[0] = 0xAAU;
        frame[1] = 0x55U;
        /* The word the chip may echo; the echo check compares against it. */
        link_last_obeyed_tx_lo = cmd_lo;
        link_last_obeyed_tx_valid = true;
    }
    frame[2] = cmd_hi;
    frame[3] = cmd_lo;
    /* bytes[4-8] stay 0: stock's builder does not set them for simple commands. */
    frame[9] = (cmd_lo + cmd_hi) & 0xFF;
}

/* Every frame to the meter chip goes through usart_tx_queue and is sent by
 * dvom_TX: there is no other way onto the wire, so two frames cannot
 * interleave. false = the queue stayed full for `wait`. */
static bool meter_tx_enqueue(const meter_tx_item_t *item, TickType_t wait)
{
    return xQueueSend(usart_tx_queue, item, wait) == pdTRUE;
}

static bool meter_tx_word(uint8_t cmd_hi, uint8_t cmd_lo, bool obey, TickType_t wait)
{
    const meter_tx_item_t item = {
        .kind = obey ? METER_TX_WORD_OBEYED : METER_TX_WORD_UNOBEYED,
        .bytes = { cmd_hi, cmd_lo },
    };
    return meter_tx_enqueue(&item, wait);
}

/* A hand-made 10-byte frame, sent as given (the toolkit's `usart raw`). */
bool meter_tx_raw_frame(const uint8_t *frame)
{
    meter_tx_item_t item = { .kind = METER_TX_RAW };
    memcpy(item.bytes, frame, FPGA_TX_FRAME_SIZE);
    return meter_tx_enqueue(&item, 0);
}

/* Send one word, then wait delay_ms. A sequence's words must not be lost,
 * so this waits for queue space (METER_TX_QUEUE_WAIT_MS: the queue's ten
 * frames take about 104 ms to send). */
static void fpga_timed_send_cmd_ex(uint8_t cmd_hi, uint8_t cmd_lo,
                                   uint32_t delay_ms, bool obey)
{
    (void)meter_tx_word(cmd_hi, cmd_lo, obey, pdMS_TO_TICKS(METER_TX_QUEUE_WAIT_MS));
    fpga_scope_delay_ms(delay_ms);
}

/*
 * Timed send, unobeyed (00 00 header): traffic that makes the chip send data
 * frames without acting on the word. The transition's setup words go this
 * way and must stay unobeyed: obeyed, 0x050A selects Capacitance, 0x0514
 * selects Auto and 0x0509 puts the chip's display in a state that is not a
 * measurement. A command to the chip goes through fpga_wire_send_word() or
 * fpga_send_cmd() instead.
 */
static void fpga_timed_send_cmd(uint8_t cmd_hi, uint8_t cmd_lo, uint32_t delay_ms)
{
    fpga_timed_send_cmd_ex(cmd_hi, cmd_lo, delay_ms, false);
}

/* A word to the meter chip, obeyed (AA 55 unless `meter hdr off`). Used for
 * the selector of a transition and of the debug boot-order replay. */
void fpga_wire_send_word(uint16_t word, uint32_t delay_ms)
{
    fpga_timed_send_cmd_ex((uint8_t)(word >> 8), (uint8_t)(word & 0xFF),
                           delay_ms, true);
}

static uint8_t fpga_probe_cmd_byte(void)
{
    /*
     * The probe tail byte, as stock chooses it: 0x07 when PC7 is high, 0x0A
     * when it is low (stock V1.2.0). Reading PC7 as "probe present" is our
     * interpretation, not proven. The shell can force either byte.
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

/* ═══════════════════════════════════════════════════════════════════
 * Input multiplexer pins
 * ═══════════════════════════════════════════════════════════════════ */

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
     * Write the ten pins of a state computed by fpga_meter_plan (from stock's
     * two writers: PC12/PE4/PE5/PE6 and PA15/PA10/PB10/PB11). The plan keeps
     * PB9 and PA6 low: stock only shows them configured as outputs, low.
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

/* Debug (`meter mux-arms`): apply one arm of each of stock's two multiplexer
 * writers and report the planned and the read-back pins. Never used by a
 * function change. */
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

/* Raise PB11 (FPGA active), PC6 (FPGA SPI enable) and PC11 (meter link),
 * then, only with `meter relays on`, write the submode's multiplexer pins. */
static void fpga_set_meter_frontend_for_submode(uint8_t submode)
{
    fpga_meter_mux_gpio_state_t mux_state;

    GPIOB->scr = PB11_MASK;
    GPIOC->scr = PC6_MASK;
    GPIOC->scr = (1U << 11);
    meter_soc_live_flag = false;   /* PC11 edge outside a finished transition */

    /* An invalid submode gets the plan's baseline state. */
    (void)fpga_meter_mux_gpio_state_for_submode(submode, &mux_state);
    /* These pins are also the scope's range relays, and the meter chip reads
     * the same in every state of them (bench 2026-11-14, unit #2: DCV
     * 1.6017 V, 121.5 Ohm, continuity, diode, all ten scope ranges on both
     * channels). So by default they are left alone and the scope keeps
     * capturing while the meter runs. `meter relays on` writes them, for A/B
     * tests. */
    if (meter_moves_relays) fpga_apply_meter_mux_gpio_state(&mux_state);
}

/* Stock's wake words in stock's order: 0x0508 (configure), 0x0509 (start)
 * and the probe word, all unobeyed (see fpga_timed_send_cmd). The one place
 * they are sent; returns the probe word's low byte. */
static uint8_t meter_send_wake_words(void)
{
    const uint8_t probe_cmd = fpga_probe_cmd_byte();

    fpga_timed_send_cmd(0x05, 0x08, METER_WAKE_WORD_GAP_MS);
    fpga_timed_send_cmd(0x05, FPGA_CMD_METER_START, METER_WAKE_WORD_GAP_MS);
    fpga_timed_send_cmd(0x05, probe_cmd, METER_WAKE_WORD_GAP_MS);
    return probe_cmd;
}

/* What a reinit sends right after its link reset: the wake words, then 0x0514. */
static void fpga_send_meter_wake_preamble(void)
{
    fpga_set_meter_frontend_for_submode(0);
    fpga_scope_delay_ms(METER_FRONTEND_SETTLE_MS);
    (void)meter_send_wake_words();
    fpga_timed_send_cmd(0x05, FPGA_CMD_METER_VAR_14, METER_WAKE_END_GAP_MS);
}

/* ═══════════════════════════════════════════════════════════════════
 * Debug transitions for the shell (`meter boot-sequence`,
 * `meter pc11-timing`). Never used by a function change.
 * ═══════════════════════════════════════════════════════════════════ */

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
    if (delay_ms > METER_DEBUG_MAX_DELAY_MS) {
        delay_ms = METER_DEBUG_MAX_DELAY_MS;
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

    /* Stock's wake words 0x0508, 0x0509 and the probe word (unobeyed), then
     * the submode's selector (obeyed) followed by delay_ms. The 0x0508 is
     * recorded in the apply-word field. */
    mlink.meter_mode_sequence_count++;
    mlink.meter_mode_sequence_submode = submode;
    mlink.meter_mode_selector_word = plan.selector_word;
    mlink.meter_mode_apply_word = 0x0508U;
    mlink.meter_mode_probe_word = (uint16_t)(0x0500U | probe_cmd);
    mlink.meter_mode_start_word = 0x0509U;

    (void)meter_send_wake_words();
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
    if (low_ms > METER_DEBUG_MAX_DELAY_MS || high_ms > METER_DEBUG_MAX_DELAY_MS) {
        return false;
    }

    plan = fpga_meter_transition_plan_for_submode(submode);
    if (!fpga_meter_mux_gpio_state_for_submode(submode, &mux_state)) {
        return false;
    }

    /* A transition with chosen PC11 timing: PC11 stays low for low_ms while
     * the multiplexer pins are written, then high for high_ms before the
     * normal word sequence. Unlike a real transition it does not wait for
     * the chip to wake. */
    meter_data_invalidate(submode);
    fpga_meter_reset_transport();          /* leaves PC11 low */

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
 * USART2 interrupt handler
 *
 *   TX: sends link_tx_frame (10 bytes, position in tx_index), then turns
 *       its own interrupt off.
 *   RX: finds a 5A A5 or AA 55 header and collects the rest of the frame
 *       in link_rx_buf.
 * ═══════════════════════════════════════════════════════════════════ */

void USART2_IRQHandler(void)
{
    /* TX: send next byte from frame buffer */
    if ((USART2->ctrl1 & USART_CTRL1_TDBEIEN) && (USART2->sts & USART_TDBE_FLAG)) {
        if (link_tx_index < FPGA_TX_FRAME_SIZE) {
            USART2->dt = link_tx_frame[link_tx_index++];
        } else {
            /* All bytes handed to the USART: stop, and tell dvom_TX. */
            BaseType_t woken = pdFALSE;
            USART2->ctrl1 &= ~USART_CTRL1_TDBEIEN;
            vTaskNotifyGiveFromISR(tx_task_handle, &woken);
            portYIELD_FROM_ISR(woken);
        }
    }

    /* RX: receive and assemble frame */
    if (USART2->sts & USART_RDBF_FLAG) {
        mlink.rx_byte_count++;
        uint8_t byte = (uint8_t)USART2->dt;
        uint8_t rx_index_before = link_rx_index;

        /* Every received byte, before any header check (diagnostic ring). */
        fpga_record_rx_raw_byte(byte, rx_index_before);

        if (link_rx_index == 0) {
            /* Looking for frame header first byte */
            if (byte == FPGA_RX_DATA_HDR_0 || byte == FPGA_RX_ECHO_HDR_0) {
                if (byte == FPGA_RX_DATA_HDR_0) {
                    mlink.rx_sync_data_start_count++;
                } else {
                    mlink.rx_sync_echo_start_count++;
                }
                link_rx_buf[0] = byte;
                link_rx_index = 1;
            } else {
                mlink.rx_sync_stray_count++;
            }
        } else if (link_rx_index == 1) {
            /* Validate header second byte */
            if ((link_rx_buf[0] == FPGA_RX_DATA_HDR_0 && byte == FPGA_RX_DATA_HDR_1) ||
                (link_rx_buf[0] == FPGA_RX_ECHO_HDR_0 && byte == FPGA_RX_ECHO_HDR_1)) {
                if (link_rx_buf[0] == FPGA_RX_DATA_HDR_0) {
                    mlink.rx_sync_data_header_count++;
                } else {
                    mlink.rx_sync_echo_header_count++;
                }
                link_rx_buf[1] = byte;
                link_rx_index = 2;
            } else {
                /* Invalid header: start over */
                mlink.rx_sync_bad_second_count++;
                link_rx_index = 0;
            }
        } else {
            link_rx_buf[link_rx_index++] = byte;

            /* Check for complete frame */
            if (link_rx_buf[0] == FPGA_RX_DATA_HDR_0 &&
                link_rx_index >= FPGA_RX_FRAME_SIZE) {
                /*
                 * Drop a data frame that completes while a command frame is
                 * still being sent, as stock does (it checks the same TX byte
                 * index). Since a link reset sets tx_index to 0, this also
                 * drops every data frame until dvom_TX has sent one frame.
                 */
                if (link_tx_index < FPGA_TX_FRAME_SIZE) {
                    mlink.rx_data_tx_busy_drop_count++;
                    link_rx_index = 0;
                    return;
                }

                /* Complete data frame (12 bytes). dvom_RX gets its own copy in
                 * the queue event, so a later frame cannot overwrite the bytes
                 * before the task has parsed them. mlink.rx_frame keeps the
                 * latest for diagnostics. */
                fpga_meter_rx_event_t event;
                memcpy((void *)mlink.rx_frame, (const void *)link_rx_buf,
                       FPGA_RX_FRAME_SIZE);
                memcpy(event.frame, (const void *)link_rx_buf,
                       FPGA_RX_FRAME_SIZE);
                mlink.rx_frame_valid = true;
                mlink.frame_count++;
                /* The link state at this frame's arrival. dvom_RX uses
                 * transition_busy and mode_sequence_submode from the event;
                 * the rest is for diagnostics. */
                mlink.last_rx_frame_count = mlink.frame_count;
                mlink.last_rx_tx_count = mlink.tx_count;
                mlink.last_rx_echo_count = mlink.echo_count;
                mlink.last_rx_mode_sequence_count = mlink.meter_mode_sequence_count;
                mlink.last_rx_mode_sequence_submode = mlink.meter_mode_sequence_submode;
                mlink.last_rx_discard_remaining = meter_frame_discard_count;
                mlink.last_rx_transition_busy = meter_transition_busy ? 1U : 0U;
                event.frame_count = mlink.last_rx_frame_count;
                event.tx_count = mlink.last_rx_tx_count;
                event.echo_count = mlink.last_rx_echo_count;
                event.mode_sequence_count = mlink.last_rx_mode_sequence_count;
                event.mode_sequence_submode = mlink.last_rx_mode_sequence_submode;
                event.discard_remaining = mlink.last_rx_discard_remaining;
                event.transition_busy = mlink.last_rx_transition_busy;
                fpga_record_rx_data_frame();
                fpga_capture_meter_first_rx_latch();
                link_rx_index = 0;

                /* To dvom_RX, once the scheduler runs. A full queue drops the frame. */
                if (meter_rx_queue != NULL &&
                    xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
                    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
                    (void)xQueueSendFromISR(meter_rx_queue, &event,
                                            &xHigherPriorityTaskWoken);
                    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
                }

            } else if (link_rx_buf[0] == FPGA_RX_ECHO_HDR_0 &&
                       link_rx_index >= FPGA_RX_ECHO_FRAME_SIZE) {
                /*
                 * Complete echo frame (10 bytes). Valid, as stock checks it,
                 * when byte[3] is the low byte of the command and byte[7] is
                 * 0xAA. The command is the last obeyed one, not the last frame
                 * sent: unobeyed frames get no echo. A valid echo is what
                 * confirms a selector (fpga_send_selector_confirmed).
                 */
                memcpy((void *)mlink.last_rx_echo_frame, (const void *)link_rx_buf,
                       FPGA_RX_ECHO_FRAME_SIZE);
                if (link_last_obeyed_tx_valid &&
                    link_rx_buf[3] == link_last_obeyed_tx_lo &&
                    link_rx_buf[7] == 0xAAU) {
                    mlink.rx_echo_valid_count++;
                    mlink.echo_count++;
                } else {
                    mlink.rx_echo_bad_count++;
                }
                link_rx_index = 0;
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * FreeRTOS Tasks
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * dvom_TX: the only sender. Takes one item from usart_tx_queue, builds its
 * frame (a word through meter_build_tx_frame, a raw frame as given), starts
 * the interrupt-driven send and waits for the handler to say that the last
 * byte has gone to the USART. The wait is bounded, because a link reset
 * (fpga_meter_reset_transport) can abandon a frame.
 */
void fpga_usart_tx_task(void *pv)
{
    (void)pv;
    meter_tx_item_t item;

    for (;;) {
        xQueueReceive(usart_tx_queue, &item, portMAX_DELAY);

        uint8_t frame[FPGA_TX_FRAME_SIZE];
        if (item.kind == METER_TX_RAW) {
            memcpy(frame, item.bytes, FPGA_TX_FRAME_SIZE);
        } else {
            mlink.tx_count++;
            fpga_record_tx_cmd(item.bytes[0], item.bytes[1]);
            meter_build_tx_frame(frame, item.bytes[0], item.bytes[1],
                                 item.kind == METER_TX_WORD_OBEYED);
        }
        link_tx_index = 0;
        memcpy((void *)link_tx_frame, frame, FPGA_TX_FRAME_SIZE);
        fpga_record_tx_frame((const uint8_t *)link_tx_frame);

        (void)ulTaskNotifyTake(pdTRUE, 0);              /* forget an abandoned frame's signal */
        USART2->ctrl1 |= USART_CTRL1_TDBEIEN;           /* the handler sends the 10 bytes */
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(METER_TX_FRAME_TIMEOUT_MS));
    }
}

/*
 * dvom_RX: takes each data frame from meter_rx_queue and hands it to
 * meter_data_process_frame(), except frames that arrived during a function
 * change and the discard count after one. By design it never sends a range
 * command based on the decoded value.
 */
void fpga_usart_rx_task(void *pv)
{
    (void)pv;

    for (;;) {
        fpga_meter_rx_event_t event;

        xQueueReceive(meter_rx_queue, &event, portMAX_DELAY);

        extern volatile uint8_t meter_submode;   /* the UI's function (main.c) */
        if (!fpga_meter_rx_frame_should_parse(event.transition_busy != 0U,
                                              &meter_frame_discard_count,
                                              &meter_transition_frame_skip_count)) {
            continue;
        }

        /* Decode with the function the chip was last switched to, which can
         * differ from the UI's while a change is pending. */
        uint8_t parse_submode = meter_submode;
        if (fpga_meter_submode_is_valid(event.mode_sequence_submode)) {
            parse_submode = event.mode_sequence_submode;
        }
        meter_data_set_function_confirmed(fpga_meter_soc_live() && mlink.meter_selector_confirmed);
        meter_data_process_frame(event.frame, parse_submode);
    }
}

/*
 * The keepalive: the chip sends data frames only while there is traffic on
 * the wire (measured: with no command obeyed, 6 frames per second; with no
 * traffic, none). So the poll task sends the start word 0x0509 every 250 ms
 * while the link is serving. It must be unobeyed: obeyed, each one starts a
 * new measurement and autorange, and the relays click continuously.
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

    (void)fpga_send_cmd_unobeyed((uint8_t)(plan.start_word >> 8),
                                 (uint8_t)(plan.start_word & 0x00FFU));
}

/* Function changes run in the poll task, not in the task that asks for them:
 * a full transition waits for the meter chip to restart, about 1.4 s and up
 * to about 5 s when it does not wake or answer (the bounds further down),
 * and would freeze the buttons that long. The UI changes function at once
 * and shows dashes; the poll task applies only the newest request, so a run
 * of presses costs one transition. */
TaskHandle_t       meter_poll_handle;

static volatile int16_t   meter_mode_request = -1;

/* A function change asked of the poll task has not been taken up yet. With
 * fpga_meter_transition_busy(): the meter function is (about to be) changing. */
bool fpga_meter_mode_request_pending(void) { return meter_mode_request >= 0; }

void fpga_request_meter_mode(uint8_t submode)
{
    meter_mode_request = submode;
    xTaskNotifyGive(meter_poll_handle);
}

/* The poll task serves the meter while its link is on, or while the meter
 * screen is shown: a key's request (input_mode_switch, input task) arrives
 * one frame before the display task turns the link on, and the meter screen
 * always wants the link (meter_link_policy.h). */
static bool meter_link_serving(void)
{
    extern volatile device_mode_t current_mode;  /* from ui.h via main.c */
    return current_mode == MODE_MULTIMETER ||
           meter_link_state == METER_LINK_ST_ON;
}

void fpga_meter_poll_task(void *pv)
{
    (void)pv;
    extern volatile uint8_t meter_submode;

    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(METER_KEEPALIVE_PERIOD_MS));  /* or a request */
        int16_t req = meter_mode_request;
        if (req >= 0 && fpga.initialized) {
            meter_mode_request = -1;
            if (meter_link_serving()) {
                fpga_set_meter_mode((uint8_t)req);
                /* If the link went off during the change, the display task
                 * has already switched it off, but the transition raised
                 * PC11 again: switch it off once more. */
                if (!meter_link_serving())
                    fpga_set_meter_mux(false);
            }
            continue;
        }
        if (fpga.initialized && meter_link_serving() &&
            !meter_transition_busy) {
            if (fpga_meter_needs_activation) {
                /* Stock's activation words, once after each link-on
                 * (fpga_set_meter_mux(true)); fpga_init sends none. They
                 * normally follow the link-on's transition, so they must be
                 * unobeyed (see fpga_timed_send_cmd). */
                fpga_meter_needs_activation = false;
                (void)meter_send_wake_words();
                fpga_timed_send_cmd(0x05, FPGA_CMD_METER_VAR_14, METER_ACTIVATION_END_GAP_MS);
            }
            if (meter_keepalive_enabled)
                fpga_send_meter_poll_sequence(meter_submode);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * Link on and off: PC11, USART2 and the shared front end
 *
 * PC11 high enables the meter chip's side of the link; with it low the
 * chip sends nothing (bench A/B/A: 276 bytes received, 0, 276). Stock
 * drives it high in meter mode and low in scope mode.
 * ═══════════════════════════════════════════════════════════════════ */

/* Set by a link-on; the poll task then sends the activation words once. */
volatile bool fpga_meter_needs_activation = false;

/* True from a link-on with `meter relays on` until the link-off has put the
 * scope's front-end posture back. */
static volatile bool meter_holds_fe;

bool fpga_meter_holds_fe(void) { return meter_holds_fe; }

void fpga_meter_relays_set(bool on) { meter_moves_relays = on; }

bool fpga_meter_relays_get(void)    { return meter_moves_relays; }

void fpga_set_meter_mux(bool enable)
{
    GPIOC->cfghr = (GPIOC->cfghr & ~(0xFu << 12)) | (0x3u << 12);  /* PC11 PP 50MHz */
    if (enable) GPIOC->scr = (1u << 11);
    else        GPIOC->clr = (1u << 11);
    meter_soc_live_flag = false;

    /* PC11 alone is not enough: fpga_init leaves USART2 disabled, so a
     * link-on must also enable USART2 and have the poll task send the
     * activation words. A link-off disables USART2 again. */
    fpga_usart_scope_enable(enable);
    if (enable) fpga_meter_needs_activation = true;

    /*
     * The range relays are shared with the scope. A link-off puts the
     * scope's relay settings back, and only those: no USART words and
     * nothing that touches the FPGA's capture state.
     */
    if (!enable) {
        GPIOB->scr = PB11_MASK;   /* PB11 HIGH: FPGA active */
        fpga_set_scope_frontend_ranges();
        /* The posture is the scope's again. If a scope setting was stored
         * while the meter held the front end, the model now re-sends SPI3
         * registers 0x01/0x06/0x08 (model_hw_fw.c). Nothing is written here,
         * so a hop between screens other than the meter costs nothing. */
        meter_holds_fe = false;
        fpga_scope_posture_gen++;
    } else {
        meter_holds_fe = meter_moves_relays;
    }
}

/* ── The meter link ───────────────────────────────────────────────────────
 *
 * PC11 (every rise restarts the meter chip, about 1.4 s), USART2 and the
 * poll task's serving, as one on/off resource. It follows
 * meter_link_want_on() (meter_link_policy.h), which the display task
 * evaluates every frame and hands to fpga_meter_link_set(), its only
 * caller. (The poll task may also call fpga_set_meter_mux(false) after a
 * transition, when the link went off during it.)
 *
 * What fpga_meter_link_set() does:
 *   - off or unknown to on: fpga_set_meter_mux(true), then it asks the poll
 *     task for the stored function, unless a request is already waiting, a
 *     transition for that function is in flight (one started since the
 *     link last went off), or the chip is live on it. A key's meter entry
 *     and the boot path post their own request first, so they still cost
 *     one request and one transition.
 *   - on to on: nothing. fpga_set_meter_mux(true) on a live link would
 *     reset USART2, forget that the chip is live (so the next function
 *     change would be a full restart) and re-send the activation words.
 *   - on or unknown to off: fpga_set_meter_mux(false).
 * The first evaluation after boot always runs one of the two.
 */
bool fpga_meter_link_on(void) { return meter_link_state == METER_LINK_ST_ON; }

/* True when nothing needs asking: a request is waiting (the newest wins), or
 * the chip runs this function or is being switched to it. */
static bool meter_link_function_covered(uint8_t submode)
{
    if (meter_mode_request >= 0) return true;                       /* the newest request wins */
    if (meter_transition_busy) return meter_transition_target == submode;
    return fpga_meter_soc_live() && mlink.meter_mode_sequence_submode == submode;
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
        /* Before the poll task exists, a request would run the whole
         * transition here, on the display task; skip it then. */
        if (!covered && meter_poll_handle != NULL) fpga_request_meter_mode(submode);
    } else {
        if (meter_link_state == METER_LINK_ST_OFF) return;
        meter_link_state = METER_LINK_ST_OFF;
        /* A transition in flight is about to lose PC11 and USART2, so it no
         * longer counts as applying the function: the next link-on asks
         * again (served once this one has run out). A transition started
         * after this point sets its own target. */
        if (meter_transition_busy) meter_transition_target = 0xFF;
        fpga_set_meter_mux(false);
    }
}

/* The meter screen was entered (input task, input_mode_switch). With the link
 * off, request the function. With the link on and the chip running the
 * function, do nothing: no PC11 edge, no selector word. A link that is on but
 * not live (a transition that timed out) gets a retry here. */
void fpga_meter_screen_entered(uint8_t submode)
{
    if (meter_link_state == METER_LINK_ST_ON && meter_link_function_covered(submode))
        return;
    fpga_request_meter_mode(submode);
}

/* Enable USART2 at FPGA_USART_BAUD (its clock is half the core clock) with
 * the RX interrupt, or disable it with ctrl1 = 0 and the interrupt masked.
 * Called by link on and off and by `fpga usart on|off`; the shell reads
 * the result back with fpga_usart_ctrl1() and fpga_usart_baudr(). */
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

/* Create the link's two queues and three tasks. Every image that has the
 * meter screen calls this once before the scheduler starts, so the senders
 * and the function-change request below always have their queue and task.
 * Each task is gated on the link (meter_link_serving), so with the link off
 * USART2 is quiet. */
void meter_link_create_tasks(void)
{
    usart_tx_queue = xQueueCreate(METER_TX_QUEUE_LEN, sizeof(meter_tx_item_t));
    meter_rx_queue = xQueueCreate(METER_RX_QUEUE_LEN, sizeof(fpga_meter_rx_event_t));
    xTaskCreate(fpga_usart_tx_task,    "dvom_TX",   FPGA_DVOM_TX_STACK, NULL, 2, &tx_task_handle);
    xTaskCreate(fpga_usart_rx_task,    "dvom_RX",   FPGA_DVOM_RX_STACK, NULL, 3, &rx_task_handle);
    xTaskCreate(fpga_meter_poll_task,  "meter_poll", FPGA_METER_POLL_STACK, NULL, 2, &meter_poll_handle);
}

/* `fpga diag clear`: zero the link's counters, last frames and histories. */
void meter_link_diag_reset(void)
{
    taskENTER_CRITICAL();
    mlink.tx_count = 0;
    mlink.rx_byte_count = 0;
    mlink.frame_count = 0;
    mlink.echo_count = 0;
    mlink.rx_sync_data_start_count = 0;
    mlink.rx_sync_echo_start_count = 0;
    mlink.rx_sync_data_header_count = 0;
    mlink.rx_sync_echo_header_count = 0;
    mlink.rx_sync_bad_second_count = 0;
    mlink.rx_sync_stray_count = 0;
    mlink.rx_data_tx_busy_drop_count = 0;
    mlink.rx_echo_valid_count = 0;
    mlink.rx_echo_bad_count = 0;
    memset((void *)mlink.rx_raw_history, 0, sizeof(mlink.rx_raw_history));
    memset((void *)mlink.rx_raw_history_tx_count, 0,
           sizeof(mlink.rx_raw_history_tx_count));
    memset((void *)mlink.rx_raw_history_tx_index, 0,
           sizeof(mlink.rx_raw_history_tx_index));
    memset((void *)mlink.rx_raw_history_rx_index, 0,
           sizeof(mlink.rx_raw_history_rx_index));
    mlink.rx_raw_history_head = 0;
    mlink.rx_raw_history_count = 0;
    mlink.rx_frame_valid = false;
    memset((void *)mlink.rx_frame, 0, sizeof(mlink.rx_frame));
    memset((void *)mlink.last_rx_echo_frame, 0, sizeof(mlink.last_rx_echo_frame));
    memset((void *)mlink.last_tx_frame, 0, sizeof(mlink.last_tx_frame));
    memset((void *)mlink.tx_frame_history, 0, sizeof(mlink.tx_frame_history));
    memset((void *)mlink.tx_frame_history_tx_count, 0,
           sizeof(mlink.tx_frame_history_tx_count));
    mlink.tx_frame_history_head = 0;
    mlink.tx_frame_history_count = 0;
    memset((void *)mlink.tx_control_frame_history, 0,
           sizeof(mlink.tx_control_frame_history));
    memset((void *)mlink.tx_control_frame_history_tx_count, 0,
           sizeof(mlink.tx_control_frame_history_tx_count));
    mlink.tx_control_frame_history_head = 0;
    mlink.tx_control_frame_history_count = 0;
    memset((void *)mlink.rx_frame_history, 0, sizeof(mlink.rx_frame_history));
    memset((void *)mlink.rx_history_frame_count, 0, sizeof(mlink.rx_history_frame_count));
    memset((void *)mlink.rx_history_tx_count, 0, sizeof(mlink.rx_history_tx_count));
    memset((void *)mlink.rx_history_echo_count, 0, sizeof(mlink.rx_history_echo_count));
    memset((void *)mlink.rx_history_sequence_count, 0, sizeof(mlink.rx_history_sequence_count));
    memset((void *)mlink.rx_history_sequence_submode, 0, sizeof(mlink.rx_history_sequence_submode));
    memset((void *)mlink.rx_history_discard_remaining, 0, sizeof(mlink.rx_history_discard_remaining));
    memset((void *)mlink.rx_history_transition_busy, 0, sizeof(mlink.rx_history_transition_busy));
    mlink.rx_frame_history_head = 0;
    mlink.rx_frame_history_count = 0;
    memset((void *)mlink.meter_transition_history_submode, 0, sizeof(mlink.meter_transition_history_submode));
    memset((void *)mlink.meter_transition_history_config, 0, sizeof(mlink.meter_transition_history_config));
    memset((void *)mlink.meter_transition_history_selector, 0, sizeof(mlink.meter_transition_history_selector));
    memset((void *)mlink.meter_transition_history_apply, 0, sizeof(mlink.meter_transition_history_apply));
    memset((void *)mlink.meter_transition_history_bank, 0, sizeof(mlink.meter_transition_history_bank));
    memset((void *)mlink.meter_transition_history_bank_first, 0, sizeof(mlink.meter_transition_history_bank_first));
    memset((void *)mlink.meter_transition_history_bank_second, 0, sizeof(mlink.meter_transition_history_bank_second));
    memset((void *)mlink.meter_transition_history_probe, 0, sizeof(mlink.meter_transition_history_probe));
    memset((void *)mlink.meter_transition_history_start, 0, sizeof(mlink.meter_transition_history_start));
    memset((void *)mlink.meter_transition_history_sequence_count, 0, sizeof(mlink.meter_transition_history_sequence_count));
    memset((void *)mlink.meter_transition_history_tx_before, 0, sizeof(mlink.meter_transition_history_tx_before));
    memset((void *)mlink.meter_transition_history_tx_after, 0, sizeof(mlink.meter_transition_history_tx_after));
    memset((void *)mlink.meter_transition_history_frame_before, 0, sizeof(mlink.meter_transition_history_frame_before));
    memset((void *)mlink.meter_transition_history_frame_after, 0, sizeof(mlink.meter_transition_history_frame_after));
    memset((void *)mlink.meter_transition_history_planned_gpio, 0, sizeof(mlink.meter_transition_history_planned_gpio));
    memset((void *)mlink.meter_transition_history_actual_gpio, 0, sizeof(mlink.meter_transition_history_actual_gpio));
    mlink.meter_transition_history_head = 0;
    mlink.meter_transition_history_count = 0;
    taskEXIT_CRITICAL();
}

/* ═══════════════════════════════════════════════════════════════════
 * Sending words from other files
 * ═══════════════════════════════════════════════════════════════════ */

/* An obeyed command to the meter chip. Does not wait: pdFALSE when the queue
 * is full. Callable once fpga_create_tasks() has run. */
BaseType_t fpga_send_cmd(uint8_t cmd_high, uint8_t cmd_low)
{
    return meter_tx_word(cmd_high, cmd_low, true, 0) ? pdTRUE : pdFALSE;
}

/* The same with the 00 00 header: traffic the chip does not act on. */
BaseType_t fpga_send_cmd_unobeyed(uint8_t cmd_high, uint8_t cmd_low)
{
    return meter_tx_word(cmd_high, cmd_low, false, 0) ? pdTRUE : pdFALSE;
}

/* ═══════════════════════════════════════════════════════════════════
 * Function changes
 *
 * A full transition drops PC11, so the meter chip restarts in its power-on
 * Auto function, and it ignores commands until it has booted (measured on
 * unit #2, 2026-09-13: a selector sent 20 ms after the restart got no echo
 * and changed nothing; the same word a second later worked). So the
 * transition waits until the chip sends a data frame, sending unobeyed
 * keepalives meanwhile (without traffic the chip sends no frames), and then
 * re-sends the selector until an echo confirms it. `meter hdr` prints the
 * wake time and the number of tries.
 * ═══════════════════════════════════════════════════════════════════ */
#define FPGA_METER_SOC_WAKE_TIMEOUT_MS   3000u  /* measured wake 1380 ms; 1500 was once too short */
#define FPGA_METER_SOC_WAKE_SETTLE_MS     200u
#define FPGA_METER_SOC_WAKE_POLL_MS        10u
#define FPGA_METER_SELECTOR_ECHO_WAIT_MS  300u
#define FPGA_METER_SELECTOR_TRIES           5u

/* Wait until a data frame has arrived since frame_before, up to the
 * timeout, with an unobeyed keepalive every 250 ms; then settle. Leaves the
 * wake time in mlink.meter_soc_wake_ms (0xFFFF: timed out). */
static void fpga_meter_wait_for_soc(uint16_t frame_before)
{
    uint32_t waited = 0;
    uint32_t since_keepalive = 0;

    mlink.meter_soc_wake_ms = 0xFFFFu;  /* timeout until proven otherwise */
    while (waited < FPGA_METER_SOC_WAKE_TIMEOUT_MS) {
        if (mlink.frame_count != frame_before) {
            mlink.meter_soc_wake_ms = (uint16_t)waited;
            break;
        }
        if (waited + FPGA_METER_SOC_WAKE_POLL_MS >= FPGA_METER_SOC_WAKE_TIMEOUT_MS) {
            mlink.meter_soc_wake_timeouts++;
        }
        if (since_keepalive >= METER_KEEPALIVE_PERIOD_MS) {
            (void)fpga_send_cmd_unobeyed(0x05, FPGA_CMD_METER_START);
            since_keepalive = 0;
        }
        fpga_scope_delay_ms(FPGA_METER_SOC_WAKE_POLL_MS);
        waited += FPGA_METER_SOC_WAKE_POLL_MS;
        since_keepalive += FPGA_METER_SOC_WAKE_POLL_MS;
    }
    fpga_scope_delay_ms(FPGA_METER_SOC_WAKE_SETTLE_MS);
}

/* Send the selector obeyed and wait for a valid echo; up to
 * FPGA_METER_SELECTOR_TRIES tries. Leaves the outcome in
 * mlink.meter_selector_attempts and mlink.meter_selector_confirmed. */
static void fpga_send_selector_confirmed(uint16_t word, uint32_t settle_ms)
{
    mlink.meter_selector_attempts = 0;
    mlink.meter_selector_confirmed = 0;
    for (uint8_t attempt = 0; attempt < FPGA_METER_SELECTOR_TRIES; attempt++) {
        uint16_t echo_before = mlink.rx_echo_valid_count;
        uint32_t waited = 0;

        mlink.meter_selector_attempts++;
        fpga_wire_send_word(word, 0);
        while (waited < FPGA_METER_SELECTOR_ECHO_WAIT_MS) {
            if (mlink.rx_echo_valid_count != echo_before) {
                mlink.meter_selector_confirmed = 1;
                break;
            }
            fpga_scope_delay_ms(FPGA_METER_SOC_WAKE_POLL_MS);
            waited += FPGA_METER_SOC_WAKE_POLL_MS;
        }
        if (mlink.meter_selector_confirmed) break;
    }
    if (!mlink.meter_selector_confirmed) {
        mlink.meter_selector_unconfirmed_total++;
    }
    fpga_scope_delay_ms(settle_ms);
}

/* The probe word 0x0507 or 0x050A (fpga_probe_cmd_byte), unobeyed. */
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
        mlink.meter_mode_sequence_count++;
        mlink.meter_mode_sequence_submode = submode;
        mlink.meter_mode_config_word = 0;
        mlink.meter_mode_selector_word = FPGA_METER_INVALID_SELECTOR_WORD;
        mlink.meter_mode_apply_word = 0;
        mlink.meter_mode_probe_word = 0;
        mlink.meter_mode_start_word = 0;
        return;
    }

    mlink.meter_mode_sequence_count++;
    mlink.meter_mode_sequence_submode = submode;
    mlink.meter_mode_config_word = 0;
    mlink.meter_mode_selector_word = plan.selector_word;
    mlink.meter_mode_apply_word = plan.apply_word;  /* always 0, see the plan */
    mlink.meter_mode_probe_word = plan.has_probe_detect ? probe_word : 0;
    mlink.meter_mode_start_word = plan.start_word;
    /*
     * The words of a transition, from the plan (fpga_meter_plan.c): the bank
     * prefix 0x00 0x00, 0x00 0x2C (submodes 7 continuity and 8 diode), the
     * configure word 0x0508 (submode 0, DC voltage), the selector, the probe
     * word and the start word 0x0509. The settle time after each is
     * conservative local policy.
     */
    if (plan.has_command_bank_prefix) {
        fpga_timed_send_cmd(0x00, plan.command_bank_first, plan.settle_ms);
        fpga_timed_send_cmd(0x00, plan.command_bank_second, plan.settle_ms);
    }
    if (plan.has_config_word) {
        mlink.meter_mode_config_word = plan.config_word;
        fpga_timed_send_cmd((uint8_t)(plan.config_word >> 8),
                            (uint8_t)(plan.config_word & 0x00FFU),
                            plan.settle_ms);
    }
    /* The only obeyed word of a transition. The others must stay unobeyed
     * (see fpga_timed_send_cmd), or they would undo the selector. */
    fpga_send_selector_confirmed(plan.selector_word, plan.settle_ms);
    if (plan.has_probe_detect) {
        fpga_timed_send_probe_detect(10);
    }
    fpga_timed_send_cmd((uint8_t)(plan.start_word >> 8),
                        (uint8_t)(plan.start_word & 0x00FFU),
                        plan.settle_ms);
}

/* The full transition, for every function change that cannot be a fast
 * switch, and for reinit (wake words right after the reset): reset the link,
 * raise PC11, wait for the chip, send the words, then drop the plan's
 * discard count of data frames. */
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

    fpga_meter_reset_transport();
    if (wake_preamble) {
        fpga_send_meter_wake_preamble();
    }
    (void)fpga_meter_mux_gpio_state_for_submode(submode, &planned_mux);
    planned_gpio = fpga_meter_mux_gpio_mask_from_state(&planned_mux);
    tx_before = mlink.tx_count;
    frame_before = mlink.frame_count;
    fpga_set_meter_frontend_for_submode(submode);
    actual_gpio = fpga_meter_mux_gpio_mask_live();
    fpga_scope_delay_ms(plan.settle_ms);
    fpga_meter_wait_for_soc(frame_before);
    fpga_send_meter_mode_sequence(submode);
    fpga_record_meter_transition_snapshot(submode, &plan, planned_gpio,
                                          actual_gpio, tx_before, frame_before);
    fpga_arm_meter_first_rx_latch(submode, &plan, planned_gpio, actual_gpio);
    fpga_meter_discard_next_frames(plan.discard_frames);
    /* The chip woke and echoed the selector: later function changes can be
     * a fast switch. */
    meter_soc_live_flag = (mlink.meter_soc_wake_ms != 0xFFFFu) &&
                          mlink.meter_selector_confirmed;
    meter_transition_busy = false;
}

void fpga_meter_fast_switch_set(bool on) { meter_fast_switch_enabled = on; }

bool fpga_meter_fast_switch_get(void)    { return meter_fast_switch_enabled; }

void fpga_meter_keepalive_set(bool on)   { meter_keepalive_enabled = on; }

bool fpga_meter_keepalive_get(void)      { return meter_keepalive_enabled; }

bool fpga_meter_soc_live(void)
{
    /* The flag alone is not enough: PC11 low anywhere means the chip is off
     * or restarting. Check the pin's output latch. */
    return meter_soc_live_flag && (GPIOC->odt & (1U << 11)) != 0U;
}

/*
 * Fast switch: one obeyed selector word per function change, as stock does
 * (stock capture logs). PC11 stays high, no pin moves (the chip switches its
 * own relays), the chip does not restart, and no other word follows. Only
 * when the chip is live and `meter fast` is on.
 *
 * Returns false when a fast switch is not possible or the chip never echoed
 * the word; the caller then runs the full transition.
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
    mlink.meter_mode_sequence_count++;
    mlink.meter_mode_sequence_submode = submode;
    mlink.meter_mode_selector_word = plan.selector_word;
    mlink.meter_mode_config_word = 0;
    mlink.meter_mode_apply_word = 0;
    mlink.meter_mode_probe_word = 0;
    mlink.meter_mode_start_word = 0;

    fpga_send_selector_confirmed(plan.selector_word, 0);

    if (!mlink.meter_selector_confirmed) {
        mlink.meter_fast_fallback_count++;
        meter_transition_busy = false;
        return false;
    }
    fpga_meter_discard_next_frames(plan.discard_frames);
    mlink.meter_fast_switch_count++;
    meter_transition_busy = false;
    return true;
}

void fpga_set_meter_mode(uint8_t submode)
{
    if (!fpga.initialized) return;

    /* The signal generator (DAC2, PA5) is left running, as stock does. */

    if (fpga_meter_fast_switch(submode)) return;
    fpga_apply_meter_transition(submode, false);
}

/* `fpga meter reinit`: a full transition with stock's wake words. */
void fpga_meter_reinit(uint8_t submode)
{
    if (!fpga.initialized) return;
    fpga_apply_meter_transition(submode, true);
}
