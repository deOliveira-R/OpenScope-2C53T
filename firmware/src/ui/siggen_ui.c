/*
 * OpenScope 2C53T - Signal Generator UI
 *
 * Shows stock's parameters (waveform, frequency, amplitude, duty) and the
 * 100-sample cycle the DAC is actually fed, so the preview is the output,
 * not a separate rendering of it.
 */

#include "ui.h"
#include "lcd.h"
#include "font.h"
#include "theme.h"
#include "signal_gen.h"

#include <stdio.h>

void draw_siggen_screen(uint32_t frame)
{
    (void)frame;
    const theme_t *th = theme_get();
    lcd_fill_rect(0, 16, LCD_WIDTH, LCD_HEIGHT - 32, th->background);

    const siggen_config_t *cfg = siggen_get_config();
    const uint16_t *cycle = siggen_get_cycle();

    /* Preview: two cycles across 280 px, DAC codes 0..4095 over 60 px
     * (rows 45..105), so amplitude and the Half/Full pedestal read true. */
    const uint16_t x0 = 20, w = 280, y_bot = 105, h = 60;
    for (uint16_t x = 0; x < w; x++) {
        uint32_t i = ((uint32_t)x * 2u * SIGGEN_CYCLE_SAMPLES / w) % SIGGEN_CYCLE_SAMPLES;
        uint16_t py = (uint16_t)(y_bot - (uint32_t)cycle[i] * h / 4095u);
        lcd_set_pixel(x0 + x, py, th->ch1);
        lcd_set_pixel(x0 + x, py + 1, th->ch1);
    }

    uint16_t param_y = 120;
    uint16_t label_x = 20;
    uint16_t value_x = 120;
    char buf[32];

    font_draw_string(label_x, param_y, "Waveform",
                     th->text_secondary, th->background, &font_small);
    font_draw_string(value_x, param_y, siggen_wave_name(cfg->waveform),
                     th->text_primary, th->background, &font_medium);

    /* Frequency: the requested value; when running, the value TMR7
     * really produces if it differs (quantised by the 240 MHz clock). */
    if (cfg->frequency_hz == 0)
        snprintf(buf, sizeof buf, "0 Hz (off)");
    else
        snprintf(buf, sizeof buf, "%lu Hz", (unsigned long)cfg->frequency_hz);
    font_draw_string(label_x, param_y + 22, "Frequency",
                     th->text_secondary, th->background, &font_small);
    font_draw_string(value_x, param_y + 20, buf,
                     th->text_primary, th->background, &font_medium);
    siggen_timing_t t;
    if (siggen_get_timing(&t) && t.actual_mhz != cfg->frequency_hz * 1000u) {
        snprintf(buf, sizeof buf, "= %lu.%03lu", (unsigned long)(t.actual_mhz / 1000u),
                 (unsigned long)(t.actual_mhz % 1000u));
        font_draw_string(value_x + 110, param_y + 22, buf,
                         th->text_secondary, th->background, &font_small);
    }

    /* Amplitude in stock's 0.1 V unit: peak-to-peak for the periodic
     * types, the level for DC; Half/Full swing half of it on a pedestal. */
    snprintf(buf, sizeof buf, "%u.%u V", cfg->amplitude_dv / 10u, cfg->amplitude_dv % 10u);
    font_draw_string(label_x, param_y + 44, "Amplitude",
                     th->text_secondary, th->background, &font_small);
    font_draw_string(value_x, param_y + 42, buf,
                     th->text_primary, th->background, &font_medium);

    if (siggen_wave_uses_duty(cfg->waveform)) {
        snprintf(buf, sizeof buf, "%u%%", cfg->duty_pct);
        font_draw_string(label_x, param_y + 66,
                         cfg->waveform == SIGGEN_SAWTOOTH ? "Symmetry" : "Duty",
                         th->text_secondary, th->background, &font_small);
        font_draw_string(value_x, param_y + 64, buf,
                         th->text_primary, th->background, &font_medium);
    }

    if (cfg->output_enabled && cfg->frequency_hz != 0) {
        font_draw_string(value_x, param_y + 88, "OUTPUT ON",
                         th->success, th->background, &font_large);
    } else {
        font_draw_string(value_x, param_y + 88, "OUTPUT OFF",
                         th->warning, th->background, &font_large);
    }

    font_draw_string(label_x, param_y + 112, "PRM:Freq UP/DN:Amp L/R:Duty SEL:Wave OK:On",
                     th->text_secondary, th->background, &font_small);
}
