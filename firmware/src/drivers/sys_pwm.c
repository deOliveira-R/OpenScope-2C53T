/*
 * sys_pwm.c - TMR10 backlight and TMR11 buzzer (parity C15).
 *
 * Stock (0x0802B0CE..0x0802B2F0): PB8 = TMR10 CH1, ARR 99, 1 MHz count, PWM
 * duty = brightness percent. PB9 = TMR11 CH1, ARR 24, 50 kHz count (2 kHz
 * tone), C1DT = volume 0..10. Stock uses PWM mode 2 with inverted polarity;
 * PWM mode A with normal polarity is the same waveform. Neither pin is
 * remapped; the bench shows it by the backlight reacting.
 */

#include "sys_pwm.h"
#include "sys_prefs.h"

#ifndef EMULATOR_BUILD

#include "at32f403a_407.h"
#include "FreeRTOS.h"
#include "task.h"

extern unsigned int system_core_clock;   /* main.c forces 240 MHz (AHB) */

static sys_beep_t   g_click;
static volatile uint8_t g_hold;          /* continuity / SoC line duty */
static uint8_t      g_buzzer_now = 0xFF; /* last value written to C1DT */
static uint8_t      g_ready;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void pwm_channel(tmr_type *t, uint32_t arr, uint32_t psc, uint32_t duty)
{
    tmr_output_config_type oc;

    tmr_base_init(t, arr, psc);
    tmr_cnt_dir_set(t, TMR_COUNT_UP);
    tmr_output_default_para_init(&oc);
    oc.oc_mode         = TMR_OUTPUT_CONTROL_PWM_MODE_A;
    oc.oc_polarity     = TMR_OUTPUT_ACTIVE_HIGH;
    oc.oc_output_state = TRUE;
    tmr_output_channel_config(t, TMR_SELECT_CHANNEL_1, &oc);
    tmr_channel_value_set(t, TMR_SELECT_CHANNEL_1, duty);
    tmr_output_channel_buffer_enable(t, TMR_SELECT_CHANNEL_1, TRUE);
    tmr_period_buffer_enable(t, TRUE);
    tmr_output_enable(t, TRUE);            /* BDT.MOE: TMR10/11 need it */
    tmr_counter_enable(t, TRUE);
}

static void pin_af(uint16_t pin)
{
    gpio_init_type g;
    gpio_default_para_init(&g);
    g.gpio_pins           = pin;
    g.gpio_mode           = GPIO_MODE_MUX;
    g.gpio_out_type       = GPIO_OUTPUT_PUSH_PULL;
    g.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(GPIOB, &g);
}

void sys_pwm_init(void)
{
    crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_TMR10_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_TMR11_PERIPH_CLOCK, TRUE);

    /* Timer clock = AHB clock (APB2 /2, doubled). Stock: PSC = clk/1e6 - 1
     * and clk/50000 - 1. */
    pwm_channel(TMR10, SYS_BACKLIGHT_ARR, system_core_clock / 1000000u - 1u,
                sys_bright_ccr(sys_prefs_brightness()));
    pwm_channel(TMR11, SYS_BUZZER_ARR, system_core_clock / 50000u - 1u, 0);
    g_buzzer_now = 0;
    g_ready = 1;

    pin_af(GPIO_PINS_8);   /* backlight: high as GPIO until this point */
    pin_af(GPIO_PINS_9);   /* buzzer */
}

void sys_pwm_apply_brightness(void)
{
    if (!g_ready) return;
    tmr_channel_value_set(TMR10, TMR_SELECT_CHANNEL_1, sys_bright_ccr(sys_prefs_brightness()));
}

void sys_pwm_backlight_off(void)
{
    if (!g_ready) return;
    tmr_channel_value_set(TMR10, TMR_SELECT_CHANNEL_1, 0);
}

static void beep_start(void)
{
    uint8_t vol = sys_prefs_volume();
    if (vol == 0u || !g_ready) return;
    taskENTER_CRITICAL();
    sys_beep_start(&g_click, now_ms(), vol, SYS_BEEP_MS);
    taskEXIT_CRITICAL();
    sys_pwm_buzzer_service();
}

void sys_pwm_click(void)      { beep_start(); }
void sys_pwm_boot_beep(void)  { beep_start(); }

void sys_pwm_buzzer_hold(uint8_t duty)
{
    g_hold = duty;
    sys_pwm_buzzer_service();
}

void sys_pwm_buzzer_service(void)
{
    if (!g_ready) return;
    taskENTER_CRITICAL();
    uint8_t d = sys_beep_poll(&g_click, now_ms());
    if (g_hold > d) d = g_hold;
    if (d > SYS_BUZZER_ARR) d = SYS_BUZZER_ARR;
    if (d != g_buzzer_now) {
        g_buzzer_now = d;
        tmr_channel_value_set(TMR11, TMR_SELECT_CHANNEL_1, d);
    }
    taskEXIT_CRITICAL();
}

#else  /* EMULATOR_BUILD: no timers */

void sys_pwm_init(void) {}
void sys_pwm_apply_brightness(void) {}
void sys_pwm_backlight_off(void) {}
void sys_pwm_click(void) {}
void sys_pwm_boot_beep(void) {}
void sys_pwm_buzzer_hold(uint8_t duty) { (void)duty; }
void sys_pwm_buzzer_service(void) {}

#endif
