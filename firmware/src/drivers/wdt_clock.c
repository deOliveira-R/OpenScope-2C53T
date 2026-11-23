/* See wdt_clock.h. */
#include "wdt_clock.h"

#include <stdbool.h>

#include "at32f403a_407.h"
#include "FreeRTOS.h"
#include "task.h"

/* Captures to average: 4000 x 8 LICK cycles is about 0.8 s at 40 kHz. */
#define WDT_CLOCK_CAPTURES      4000u
/* The capture input takes every 8th LICK edge. */
#define WDT_CLOCK_EDGES         8u
/* Timer at half its bus clock: the 16-bit counter then spans 546 us, so 8 LICK
 * cycles fit in it for any rate above about 15 kHz. */
#define WDT_CLOCK_TIMER_DIV     2u
#define WDT_CLOCK_TIMER_PERIOD  0xFFFFu
#define WDT_CLOCK_TIMEOUT_MS    3000u

static uint32_t wdt_clock_timer_hz(void)
{
    crm_clocks_freq_type c;
    crm_clocks_freq_get(&c);
    /* A timer on a divided APB bus runs at twice the bus clock. */
    const uint32_t bus = (c.apb1_freq == c.ahb_freq) ? c.apb1_freq : 2u * c.apb1_freq;
    return bus / WDT_CLOCK_TIMER_DIV;
}

uint32_t wdt_clock_measure_hz(void)
{
    const bool lick_was_on = CRM->ctrlsts_bit.licken != 0u;
    const TickType_t start = xTaskGetTickCount();
    const TickType_t limit = pdMS_TO_TICKS(WDT_CLOCK_TIMEOUT_MS);

    crm_clock_source_enable(CRM_CLOCK_SOURCE_LICK, TRUE);
    while (crm_flag_get(CRM_LICK_STABLE_FLAG) == RESET) {
        if (xTaskGetTickCount() - start > limit) return 0u;
        vTaskDelay(1);
    }

    crm_periph_clock_enable(CRM_TMR5_PERIPH_CLOCK, TRUE);
    tmr_reset(TMR5);
    gpio_pin_remap_config(TMR5CH4_GMUX, TRUE);
    tmr_base_init(TMR5, WDT_CLOCK_TIMER_PERIOD, WDT_CLOCK_TIMER_DIV - 1u);

    tmr_input_config_type in;
    tmr_input_default_para_init(&in);
    in.input_channel_select  = TMR_SELECT_CHANNEL_4;
    in.input_polarity_select = TMR_INPUT_RISING_EDGE;
    in.input_mapped_select   = TMR_CC_CHANNEL_MAPPED_DIRECT;
    in.input_filter_value    = 0u;
    tmr_input_channel_init(TMR5, &in, TMR_CHANNEL_INPUT_DIV_8);
    tmr_channel_enable(TMR5, TMR_SELECT_CHANNEL_4, TRUE);
    tmr_flag_clear(TMR5, TMR_C4_FLAG | TMR_C4_RECAPTURE_FLAG);
    tmr_counter_enable(TMR5, TRUE);

    uint64_t ticks = 0u;
    uint32_t used = 0u, last = 0u;
    bool have_last = false;
    while (used < WDT_CLOCK_CAPTURES && xTaskGetTickCount() - start <= limit) {
        if (tmr_flag_get(TMR5, TMR_C4_FLAG) == RESET) continue;
        const uint32_t now = tmr_channel_value_get(TMR5, TMR_SELECT_CHANNEL_4);
        /* A task switch can miss a capture: then this interval is not one
         * period, so it is dropped and the count restarts from here. */
        const bool missed = tmr_flag_get(TMR5, TMR_C4_RECAPTURE_FLAG) != RESET;
        tmr_flag_clear(TMR5, TMR_C4_RECAPTURE_FLAG);
        if (have_last && !missed) {
            ticks += (now - last) & WDT_CLOCK_TIMER_PERIOD;
            used++;
        }
        last = now;
        have_last = true;
    }

    const uint32_t timer_hz = wdt_clock_timer_hz();
    tmr_counter_enable(TMR5, FALSE);
    tmr_reset(TMR5);
    gpio_pin_remap_config(TMR5CH4_GMUX, FALSE);
    crm_periph_clock_enable(CRM_TMR5_PERIPH_CLOCK, FALSE);
    if (!lick_was_on) crm_clock_source_enable(CRM_CLOCK_SOURCE_LICK, FALSE);

    if (used < WDT_CLOCK_CAPTURES || ticks == 0u) return 0u;
    return (uint32_t)(((uint64_t)timer_hz * WDT_CLOCK_EDGES * used + ticks / 2u) / ticks);
}
