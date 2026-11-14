/*
 * OpenScope 2C53T - Signal Generator UI
 *
 * Shows stock's parameters (waveform, frequency, amplitude, duty) and the
 * 100-sample cycle the DAC is actually fed, so the preview is the output,
 * not a separate rendering of it.
 *
 * Painting only (display task): the screen is siggen_screen.c, which owns
 * the frequency editor's cursor; the values come from the generator's owner
 * (signal_gen.c, the backing store of the gen.* properties).
 */

#include "ui.h"
#include "lcd.h"
#include "font.h"
#include "theme.h"
#include "signal_gen.h"
#include "siggen_edit.h"
#include "siggen_screen.h"

#include <stdio.h>

void siggen_ui_draw(void)
{
    const siggen_edit_t editor = *siggen_ui_editor();   /* one look at the cursor per paint */
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

    /* Frequency: the requested value, as a digit field while the editor is
     * open (cursor digit highlighted). The value TMR7 really produces is
     * shown whenever it differs, running or not: the timer divides the
     * 240 MHz clock, so low frequencies quantise (1 Hz gives 2.2 Hz). */
    font_draw_string(label_x, param_y + 22, "Frequency",
                     th->text_secondary, th->background, &font_small);
    if (editor.active) {
        char d[2] = { 0, 0 };
        uint16_t x = value_x;
        for (int i = (int)SIGGEN_EDIT_DIGITS - 1; i >= 0; i--) {
            /* The live value, which the editor tracks key by key; a shell
             * `siggen freq` while the editor is open shows at once. */
            d[0] = (char)('0' + siggen_edit_digit_of(cfg->frequency_hz, (uint8_t)i));
            bool cur = ((uint8_t)i == editor.digit);
            font_draw_string(x, param_y + 20, d,
                             cur ? th->background : th->text_primary,
                             cur ? th->highlight : th->background, &font_medium);
            x += 12;
        }
        font_draw_string(x + 4, param_y + 20, "Hz", th->text_primary, th->background,
                         &font_medium);
    } else {
        if (cfg->frequency_hz == 0)
            snprintf(buf, sizeof buf, "0 Hz (off)");
        else
            snprintf(buf, sizeof buf, "%lu Hz", (unsigned long)cfg->frequency_hz);
        font_draw_string(value_x, param_y + 20, buf,
                         th->text_primary, th->background, &font_medium);
    }
    siggen_timing_t t;
    if (siggen_preview_timing(&t) && t.actual_mhz != cfg->frequency_hz * 1000u) {
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

    /* Boot policy (ours; stock always restores): long AUTO toggles it.
     * In the label column: "OUTPUT OFF" in font_large spans value_x..+144,
     * so anything right of it would not fit in 320 px. */
    font_draw_string(label_x, param_y + 96, cfg->start_on ? "Boot: ON" : "Boot: OFF",
                     th->text_secondary, th->background, &font_small);

    if (editor.active) {
        font_draw_string(label_x, param_y + 112, "LT/RT:Digit UP/DN:Value OK:Done MENU:Undo",
                         th->text_secondary, th->background, &font_small);
    } else {
        font_draw_string(label_x, param_y + 112, "PRM:Freq UP/DN:Amp L/R:Duty SEL:Wave OK*:Run",
                         th->text_secondary, th->background, &font_small);
    }
}
