/*
 * OpenScope 2C53T - Battery Voltage Monitor
 *
 * PB1 / ADC1 Channel 9 through a resistor divider on the PCB.
 *
 * Design:
 *   - battery_update() called once per second from timer callback
 *   - Takes ONE ADC reading per call
 *   - External power: PC7 low (input, pull-up), not a voltage threshold
 *   - The gauge logic (average, percent, critical) is battery_gauge.c
 *   - battery_percent() and battery_is_charging() return cached state
 */

#include "battery.h"
#include "battery_gauge.h"
#include "at32f403a_407.h"

#define DIVIDER_MV_PER_COUNT  1611  /* 3.3V ref, 12-bit ADC, ~2:1 divider */

static volatile uint8_t adc_ready = 0;
static batt_gauge_t gauge;

void battery_adc_init(void)
{
    crm_periph_clock_enable(CRM_ADC1_PERIPH_CLOCK, TRUE);
    crm_adc_clock_div_set(CRM_ADC_DIV_6);

    gpio_init_type gpio_cfg;
    gpio_default_para_init(&gpio_cfg);
    gpio_cfg.gpio_pins = GPIO_PINS_1;
    gpio_cfg.gpio_mode = GPIO_MODE_ANALOG;
    gpio_init(GPIOB, &gpio_cfg);

    /* PC7: external-power line, low while USB power is present. */
    crm_periph_clock_enable(CRM_GPIOC_PERIPH_CLOCK, TRUE);
    gpio_default_para_init(&gpio_cfg);
    gpio_cfg.gpio_pins = GPIO_PINS_7;
    gpio_cfg.gpio_mode = GPIO_MODE_INPUT;
    gpio_cfg.gpio_pull = GPIO_PULL_UP;
    gpio_init(GPIOC, &gpio_cfg);

    adc_base_config_type adc_cfg;
    adc_base_default_para_init(&adc_cfg);
    adc_cfg.sequence_mode = FALSE;
    adc_cfg.repeat_mode = FALSE;
    adc_cfg.data_align = ADC_RIGHT_ALIGNMENT;
    adc_cfg.ordinary_channel_length = 1;
    adc_base_config(ADC1, &adc_cfg);

    adc_ordinary_channel_set(ADC1, ADC_CHANNEL_9, 1, ADC_SAMPLETIME_239_5);
    adc_ordinary_conversion_trigger_set(ADC1, ADC12_ORDINARY_TRIG_SOFTWARE, TRUE);
    adc_enable(ADC1, TRUE);

    /* Calibration with timeout */
    adc_calibration_init(ADC1);
    for (volatile uint32_t t = 0; t < 100000; t++)
        if (!adc_calibration_init_status_get(ADC1)) break;
    adc_calibration_start(ADC1);
    for (volatile uint32_t t = 0; t < 100000; t++)
        if (!adc_calibration_status_get(ADC1)) break;

    adc_ready = 1;

    /* Prime with initial readings */
    for (unsigned i = 0; i < BATT_AVG_SIZE; i++)
        battery_update();
}

static uint16_t last_good_mv = 3700;  /* default until first good reading */

static uint16_t adc_read_mv_once(void)
{
    adc_ordinary_software_trigger_enable(ADC1, TRUE);
    for (volatile uint32_t t = 0; t < 240000; t++) {
        if (adc_flag_get(ADC1, ADC_CCE_FLAG) != RESET) {
            adc_flag_clear(ADC1, ADC_CCE_FLAG);
            uint16_t raw = adc_ordinary_conversion_data_get(ADC1);
            uint16_t mv = (uint16_t)((uint32_t)raw * DIVIDER_MV_PER_COUNT / 1000);
            /* Sanity check: 1S LiPo + USB range is ~3000-5500mV */
            if (mv >= 2500 && mv <= 6000) {
                last_good_mv = mv;
                return mv;
            }
            return last_good_mv;  /* out of range — use last good */
        }
    }
    return last_good_mv;  /* timeout — use last good */
}

void battery_update(void)
{
    if (!adc_ready) return;
    /* PC7 is low while USB power is present. */
    const bool ext_now = (GPIOC->idt & GPIO_PINS_7) == 0u;
    batt_gauge_step(&gauge, adc_read_mv_once(), ext_now);
}

uint16_t battery_read_mv(void)       { return gauge.rest_mv; }
uint16_t battery_raw_mv(void)        { return gauge.raw_mv; }
uint8_t  battery_percent(void)       { return gauge.percent; }
uint8_t  battery_is_critical(void)   { return gauge.critical ? 1u : 0u; }
uint8_t  battery_is_charging(void)   { return gauge.external ? 1u : 0u; }
