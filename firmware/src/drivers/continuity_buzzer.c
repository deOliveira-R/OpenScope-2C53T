/*
 * OpenScope 2C53T - experimental continuity buzzer drive.
 *
 * Two sources decide when to sound it:
 *
 *   software (default)  a shorted-lead verdict from the decoded reading, as
 *                       before (continuity only).
 *   SoC edge (M4)       what stock does: the SoC's buzzer line reaches EXTI3;
 *                       each edge sounds a fixed pulse of 11 housekeeping ticks
 *                       (EXTI3 ISR 0x08009C10 writes 10 to the buzzer timer's
 *                       compare register and zeroes the counter at 0x20002D67;
 *                       the housekeeping task 0x08040508 counts to 11 and then
 *                       clears the register), and only in Auto and Continuity
 *                       (meter_stock_beep_gate). Off until `meter beep on`,
 *                       because the EXTI3 source pin is not settled (see
 *                       continuity_buzzer_soc_edge_config).
 */

#include "continuity_buzzer.h"
#include "at32f403a_407.h"
#include "meter_data.h"
#include "ui.h"
#include "sys_pwm.h"
#include "sys_prefs.h"

#include "FreeRTOS.h"
#include "task.h"

#define BUZZER_TASK_MS   1U
#define BUZZER_IDLE_MS   5U
#define BUZZER_LATCH_MS  350U

#include "meter_stock.h"

/* Housekeeping tick that 11 pulse ticks are counted in. Stock's task at
 * 0x08040508 is a periodic loop whose period was not measured [H: 10 ms];
 * `meter beep tick <ms>` changes it, and the bench measures the real pulse. */
#define BUZZER_SOC_TICK_MS_DEFAULT 10U

static TaskHandle_t buzzer_task_handle;
static volatile TickType_t buzzer_force_until_tick;
static volatile bool buzzer_task_started;
static volatile bool buzzer_active_now;
static volatile uint32_t buzzer_toggle_count;
static volatile uint32_t buzzer_create_fail_count;

/* SoC-edge mode state. exti3_edges is bumped by the ISR; the task consumes it. */
static volatile bool     soc_edge_mode;
static volatile uint32_t exti3_edges;
static volatile int8_t   soc_port = -1;            /* IOMUX EXINT3 source, -1 = unconfigured */
static volatile uint32_t soc_tick_ms = BUZZER_SOC_TICK_MS_DEFAULT;
static meter_beep_pulse_t soc_pulse;
static volatile uint32_t soc_pulse_count;          /* pulses started (gate passed) */
static volatile uint32_t soc_edges_gated;          /* edges ignored by the gate */

/*
 * THE ONE PLACE THE BUZZER PIN IS DRIVEN.
 *
 * Stock writes 10 to 0x40015434 = TMR11 C1DT and the pin (PB9 = TMR11 CH1,
 * 2 kHz) follows (docs/re/parity2_static.md 4.3, which settles the old TMR4
 * dispute). The timer is owned by sys_pwm.c (parity C15), which also sounds
 * the key click and the boot beep on the same output at the stored volume;
 * this file only holds the SoC/continuity beep at stock's constant duty 10,
 * which ignores the volume setting exactly as stock's EXTI3 ISR does.
 */
static inline void buzzer_pin_set(bool level)
{
    sys_pwm_buzzer_hold(level ? SYS_BEEP_SOC_DUTY : 0u);
}

/* Same rule as the screen (meter_data.c), so the beep and the green SHORT
 * can never disagree. */
extern float meter_continuity_threshold(void);
static bool continuity_short_confirmed(const meter_reading_t *reading)
{
    return meter_continuity_is_short(reading, meter_continuity_threshold());
}

static bool continuity_short_active(TickType_t now)
{
    static TickType_t short_latch_until_tick;
    meter_reading_t reading;

    if (meter_submode != 7 || !meter_data_snapshot(&reading) ||
        !reading.valid || reading.submode != 7) {
        short_latch_until_tick = 0;
        return false;
    }

    if (continuity_short_confirmed(&reading)) {
        short_latch_until_tick = now + pdMS_TO_TICKS(BUZZER_LATCH_MS);
        return true;
    }

    if (reading.result_class == METER_RESULT_OVERLOAD ||
        reading.result_class == METER_RESULT_BLANK ||
        reading.result_class == METER_RESULT_NONE) {
        short_latch_until_tick = 0;
        return false;
    }

    return short_latch_until_tick != 0 &&
           (int32_t)(short_latch_until_tick - now) > 0;
}

void continuity_buzzer_init(void)
{
    /* PB9 is configured by sys_pwm_init() (TMR11 CH1); nothing to do. */
}

static void buzzer_task(void *arg)
{
    (void)arg;

    bool level = false;
    uint32_t seen_edges = exti3_edges;
    TickType_t next_pulse_tick = 0;
    /* Boot beep (stock 0x0802E370) from here, not from main(): before the
     * scheduler nothing would end it, and main() can still busy-wait for
     * seconds (the cold meter transition) after the beep had started. */
    sys_pwm_boot_beep();
    /* Stock's beep source by default: the SoC's buzzer line is PC3 (EXTI3).
     * Unit #2, 2026-11-11: shorted leads in Auto give 4480 edges/s on PC3
     * (a 2.24 kHz tone), open leads 0; PD3/PE3 stay silent. With it, Auto
     * beeps on a short as stock does (the software beep never ran there).
     * `meter beep src|off` still overrides it. */
    if (continuity_buzzer_soc_edge_config(2, 2, true))
        continuity_buzzer_soc_edge_mode_set(true);
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        bool force_active = buzzer_force_until_tick != 0 &&
                            (int32_t)(buzzer_force_until_tick - now) > 0;
        if (!force_active && buzzer_force_until_tick != 0) {
            buzzer_force_until_tick = 0;
        }
        if (soc_edge_mode) {
            /* M4: stock's way. An EXTI3 edge restarts the fixed pulse, but
             * only where the stock gate lets the SoC's line sound at all. */
            uint32_t edges = exti3_edges;
            /* current_mode here means "the meter screen is shown", on
             * purpose: the meter can now run under another screen (the meter
             * link, fpga.c), and there the continuity beeper stays silent. A
             * beep with no meter on screen says nothing a user could act on;
             * a host reads the short from the stream instead. */
            if (edges != seen_edges) {
                seen_edges = edges;
                if (current_mode == MODE_MULTIMETER &&
                    meter_stock_beep_gate_for_submode(meter_submode)) {
                    if (!soc_pulse.active) next_pulse_tick = now + pdMS_TO_TICKS(soc_tick_ms);
                    meter_beep_pulse_edge(&soc_pulse);
                    soc_pulse_count++;
                } else {
                    soc_edges_gated++;
                }
            }
            if (soc_pulse.active &&
                (int32_t)(now - next_pulse_tick) >= 0) {
                (void)meter_beep_pulse_tick(&soc_pulse);
                next_pulse_tick = now + pdMS_TO_TICKS(soc_tick_ms);
            }
            /* The gate also closes a pulse that outlives the function. */
            if (soc_pulse.active &&
                (current_mode != MODE_MULTIMETER ||
                 !meter_stock_beep_gate_for_submode(meter_submode))) {
                soc_pulse.active = false;
                soc_pulse.ticks = 0;
            }
            buzzer_active_now = force_active || soc_pulse.active;
        } else {
            /* The software beep, same rule: only with the meter screen
             * shown. (Until the link policy existed no frame arrived on
             * another screen, so this path needed no gate.) */
            buzzer_active_now = force_active ||
                (current_mode == MODE_MULTIMETER && continuity_short_active(now));
        }

        if (buzzer_active_now) {
            if (!level) buzzer_pin_set(true);   /* steady 2 kHz tone, no gating */
            level = true;
            buzzer_toggle_count++;
            vTaskDelay(pdMS_TO_TICKS(BUZZER_TASK_MS));
        } else {
            if (level) buzzer_pin_set(false);
            level = false;
            vTaskDelay(pdMS_TO_TICKS(BUZZER_IDLE_MS));
        }
        sys_pwm_buzzer_service();   /* ends a finished key click / boot beep */
    }
}

void continuity_buzzer_create_task(void)
{
#ifdef EMULATOR_BUILD
    return;
#else
    if (buzzer_task_handle != NULL) {
        return;
    }
    if (xTaskCreate(buzzer_task, "buzz", 192, NULL, 2,
                    &buzzer_task_handle) == pdPASS) {
        buzzer_task_started = true;
    } else {
        buzzer_task_handle = NULL;
        buzzer_task_started = false;
        buzzer_create_fail_count++;
    }
#endif
}

void continuity_buzzer_force_ms(uint32_t duration_ms)
{
#ifdef EMULATOR_BUILD
    (void)duration_ms;
#else
    if (duration_ms == 0) {
        buzzer_force_until_tick = 0;
        buzzer_active_now = false;
        return;
    }
    if (duration_ms > 5000U) {
        duration_ms = 5000U;
    }
    buzzer_force_until_tick = xTaskGetTickCount() + pdMS_TO_TICKS(duration_ms);
#endif
}

void continuity_buzzer_snapshot(bool *task_started, bool *active,
                                uint32_t *toggle_count,
                                uint32_t *create_fail_count)
{
    if (task_started) {
        *task_started = buzzer_task_started;
    }
    if (active) {
        *active = buzzer_active_now;
    }
    if (toggle_count) {
        *toggle_count = buzzer_toggle_count;
    }
    if (create_fail_count) {
        *create_fail_count = buzzer_create_fail_count;
    }
}

/* ── SoC buzzer line on EXTI3 (M4) ──────────────────────────────────── */

#ifndef EMULATOR_BUILD
/* No FreeRTOS calls: counts the edge, the task does the rest. */
void EXINT3_IRQHandler(void)
{
    exint_flag_clear(EXINT_LINE_3);
    exti3_edges++;
}
#endif

/*
 * Which pin is EXTI3? Stock programs IOMUX EXINTC1 bits [15:12], the init code
 * was not located, and the candidates are port C (PC3, input-pull in the stock
 * caplog [H]), D and E (PE3 is a key-matrix scan line in that log: changing
 * its pull can ghost keys). PA3 is USART2 RX and PB3 is SPI3 SCK here, so
 * neither can be the buzzer line and this function refuses them.
 * Rather than guess at boot, the bench picks it: `meter beep src c` and short
 * the leads in Continuity; the edge counter in `meter beep` moves on the right
 * port. port: 2 = C ... 4 = E, or -1 to switch the line off.
 * edge: 0 = rising, 1 = falling, 2 = both (stock's polarity is unknown).
 */
bool continuity_buzzer_soc_edge_config(int8_t port, uint8_t edge, bool pull_up)
{
#ifdef EMULATOR_BUILD
    (void)port; (void)edge; (void)pull_up;
    return false;
#else
    exint_init_type ei;

    /* A (PA3 = USART2 RX) and B (PB3 = SPI3 SCK) carry the FPGA link here:
     * reconfiguring pin 3 there would cut it. */
    if (port < -1 || (port >= 0 && port < 2) || port > 4 || edge > 2) return false;
    if (port < 0) {
        exint_interrupt_enable(EXINT_LINE_3, FALSE);
        soc_port = -1;
        return true;
    }
    crm_periph_clock_enable(CRM_IOMUX_PERIPH_CLOCK, TRUE);
    {
        static const crm_periph_clock_type clk[5] = {
            CRM_GPIOA_PERIPH_CLOCK, CRM_GPIOB_PERIPH_CLOCK, CRM_GPIOC_PERIPH_CLOCK,
            CRM_GPIOD_PERIPH_CLOCK, CRM_GPIOE_PERIPH_CLOCK };
        static gpio_type *const gp[5] = { GPIOA, GPIOB, GPIOC, GPIOD, GPIOE };
        gpio_init_type cfg;
        crm_periph_clock_enable(clk[port], TRUE);
        gpio_default_para_init(&cfg);
        cfg.gpio_pins = GPIO_PINS_3;
        cfg.gpio_mode = GPIO_MODE_INPUT;
        cfg.gpio_pull = pull_up ? GPIO_PULL_UP : GPIO_PULL_DOWN;
        gpio_init(gp[port], &cfg);
    }
    IOMUX->exintc1_bit.exint3 = (uint32_t)port;
    exint_default_para_init(&ei);
    ei.line_select   = EXINT_LINE_3;
    ei.line_mode     = EXINT_LINE_INTERRUPT;
    ei.line_polarity = edge == 0 ? EXINT_TRIGGER_RISING_EDGE
                     : edge == 1 ? EXINT_TRIGGER_FALLING_EDGE
                                 : EXINT_TRIGGER_BOTH_EDGE;
    ei.line_enable   = TRUE;
    exint_init(&ei);
    exint_flag_clear(EXINT_LINE_3);
    nvic_irq_enable(EXINT3_IRQn, 7, 0);
    soc_port = port;
    return true;
#endif
}

void continuity_buzzer_soc_edge_mode_set(bool on)
{
    soc_edge_mode = on;
    if (!on) { soc_pulse.active = false; soc_pulse.ticks = 0; }
}

void continuity_buzzer_soc_tick_ms_set(uint32_t ms)
{
    if (ms >= 1U && ms <= 1000U) soc_tick_ms = ms;
}

void continuity_buzzer_soc_status(bool *mode_on, int8_t *port, uint32_t *edges,
                                  uint32_t *pulses, uint32_t *gated,
                                  bool *pulse_active, uint32_t *tick_ms)
{
    if (mode_on)      *mode_on = soc_edge_mode;
    if (port)         *port = soc_port;
    if (edges)        *edges = exti3_edges;
    if (pulses)       *pulses = soc_pulse_count;
    if (gated)        *gated = soc_edges_gated;
    if (pulse_active) *pulse_active = soc_pulse.active;
    if (tick_ms)      *tick_ms = soc_tick_ms;
}
