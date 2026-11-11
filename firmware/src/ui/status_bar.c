/*
 * OpenScope 2C53T - Status Bar and Splash Screen UI
 */

#include "ui.h"
#include "lcd.h"
#include "font.h"
#include "theme.h"
#include "scope_state.h"
#include "scope_cal.h"
#include "scope_timebase.h"
#include "scope_roll.h"
#include "battery.h"
#include "at32f403a_407.h"
#include "fpga.h"
#include <stdio.h>
#include <string.h>

/* Track whether status bar has been drawn once (first draw needs full clear) */
static uint8_t status_bar_drawn = 0;

/* The trigger summary slot of the top bar (scope mode only): right-aligned at
 * x = TRIG_SLOT_RIGHT, clear of the mode name before it (ends ~162) and of
 * "2C53T" after it (starts ~233). The widest text, "Norm F C2", is 60 px
 * (font_small advances), the slot is 66. Redrawn only when it changes. */
#define TRIG_SLOT_X      166
#define TRIG_SLOT_W      66
#define TRIG_SLOT_RIGHT  (TRIG_SLOT_X + TRIG_SLOT_W - 2)
static char trig_slot_last[12];

/* Draw the top status bar */
void draw_status_bar(void)
{
    const theme_t *th = theme_get();

    /* Full clear only on first draw or mode change */
    if (!status_bar_drawn) {
        lcd_fill_rect(0, 0, LCD_WIDTH, 16, th->status_bar_bg);
        font_draw_string(4, 2, "OpenScope", th->ch2, th->status_bar_bg, &font_small);
        font_draw_string_right(LCD_WIDTH - 50, 2, "2C53T",
                               th->text_primary, th->status_bar_bg, &font_small);
        status_bar_drawn = 1;
        trig_slot_last[0] = '\x01';          /* force the slot to repaint */
    }

    /* Mode indicator (pad to fixed width to overwrite old text) */
    const char *mode_names[] = { "SCOPE ", "METER ", "SIGGEN", "SETUP " };
    font_draw_string(120, 2, mode_names[current_mode],
                     th->success, th->status_bar_bg, &font_small);

    /* Trigger mode / edge / source (stock's status line item 10). */
    {
        char t[12] = "";
        if (current_mode == MODE_OSCILLOSCOPE)
            scope_trigger_summary(scope_state_get(), t, sizeof(t));
        if (strcmp(t, trig_slot_last) != 0) {
            lcd_fill_rect(TRIG_SLOT_X, 0, TRIG_SLOT_W, 16, th->status_bar_bg);
            if (t[0])
                font_draw_string_right(TRIG_SLOT_RIGHT, 2, t, th->text_primary,
                                       th->status_bar_bg, &font_small);
            strcpy(trig_slot_last, t);
        }
    }

    /* Battery status */
    {
        char buf[12];
        if (battery_is_critical()) {
            /* Flash LOW warning — alternates red/dark each second */
            uint16_t color = (uptime_seconds & 1) ? th->warning : th->status_bar_bg;
            snprintf(buf, sizeof(buf), " LOW");
            font_draw_string_right(LCD_WIDTH - 4, 2, buf,
                                   color, th->status_bar_bg, &font_small);
        } else if (battery_is_charging()) {
            snprintf(buf, sizeof(buf), " CHG");
            font_draw_string_right(LCD_WIDTH - 4, 2, buf,
                                   th->success, th->status_bar_bg, &font_small);
        } else {
            uint8_t pct = battery_percent();
            snprintf(buf, sizeof(buf), "%3d%%", pct);
            uint16_t bat_color = (pct <= 15) ? th->warning :
                                 (pct <= 30) ? 0xFE00 : th->success;
            font_draw_string_right(LCD_WIDTH - 4, 2, buf,
                                   bat_color, th->status_bar_bg, &font_small);
        }
    }

    /* Auto-shutdown at critical battery — protect the LiPo cell */
    if (battery_is_critical()) {
        static uint8_t shutdown_timer = 0;
        shutdown_timer++;
        if (shutdown_timer >= 30) {  /* 30 seconds of critical = shutdown */
            lcd_fill_rect(40, 80, 240, 80, 0x0000);
            font_draw_string_center(160, 95, "Battery Critical",
                                    th->warning, 0x0000, &font_large);
            font_draw_string_center(160, 130, "Shutting down...",
                                    th->text_secondary, 0x0000, &font_medium);
            /* Brief delay so user can see the message */
            for (volatile uint32_t d = 0; d < 100000000; d++);
            GPIOC->clr = (1U << 9);  /* PC9 LOW = power off */
            while (1);
        }
    }
}

/* Force full redraw on next status bar update (call on theme/mode change) */
void status_bar_invalidate(void)
{
    status_bar_drawn = 0;
}

/* One channel's info-bar field: "<n>:<vdiv> <coupling>[ 10X]". The probe is
 * shown only when it is not 1X (it scales every volts readout, so a silent
 * 10X is the dangerous one). If the field would run off the screen the probe
 * is dropped rather than wrapped. */
static void draw_channel_field(uint16_t x, const char *n, const char *vdiv,
                               const channel_state_t *c, uint16_t color, uint16_t bg)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%s:%s %s%s", n, vdiv, coupling_labels[c->coupling],
             c->probe == PROBE_10X ? " 10X" : "");
    if (x + font_string_width(buf, &font_small) > LCD_WIDTH)
        snprintf(buf, sizeof(buf), "%s:%s %s", n, vdiv, coupling_labels[c->coupling]);
    font_draw_string(x, LCD_HEIGHT - 14, buf, color, bg, &font_small);
}

/* Draw the bottom info bar */
void draw_info_bar(void)
{
    const theme_t *th = theme_get();
    /* The meter's bottom edge is its softkey bar (softkey UI P1). */
    if (current_mode == MODE_MULTIMETER) {
        meter_softkeys_draw();
        return;
    }
    lcd_fill_rect(0, LCD_HEIGHT - 16, LCD_WIDTH, 16, th->status_bar_bg);

    uint16_t ib = th->status_bar_bg;
    switch (current_mode) {
    case MODE_OSCILLOSCOPE:
#ifdef FEATURE_FFT
        if (scope_view == SCOPE_VIEW_FFT) {
            const char *win_names[] = { "Rect", "Hann", "Hamm", "BHar", "Flat" };
            const fft_config_t *cfg = fft_get_config();
            font_draw_string(4, LCD_HEIGHT - 14, "FFT 4096pt",
                             th->ch1, ib, &font_small);
            font_draw_string(80, LCD_HEIGHT - 14,
                             (cfg->window < FFT_WINDOW_COUNT) ? win_names[cfg->window] : "?",
                             th->success, ib, &font_small);
            if (cfg->avg_count > 0)
                font_draw_string(120, LCD_HEIGHT - 14, "AVG",
                                 th->ch2, ib, &font_small);
            if (cfg->max_hold)
                font_draw_string(150, LCD_HEIGHT - 14, "MH",
                                 th->warning, ib, &font_small);
            font_draw_string(220, LCD_HEIGHT - 14, "PRM:View",
                             th->text_secondary, ib, &font_small);
        } else
#endif
        {
            const scope_state_t *ss = scope_state_get();
            char buf[24];

            /*
             * CH1: volts/div + coupling (+ probe when it is not 1X).
             *
             * The volts/div shown is DERIVED from the bench-measured gain for
             * this channel on this range (scope_cal.h), not read out of
             * vdiv_table. The nominal labels there ("5mV" ... "5V") were never
             * traced to a measurement and are 2.7x-3.5x out on the three
             * ranges we have cross-validated, so a range labelled "2V" was
             * ruling a 2.8 V division. Uncalibrated ranges read "--", and
             * provisional ones carry a leading '~'.
             *
             * Layout (font_small advances, 320 px): the trigger summary moved
             * to the top bar, which frees the middle. The channel fields are
             * "1:..." / "2:..." (colour says which), at most ~102 px with the
             * probe, so CH1 sits at x=2, the timebase at 128, CH2 at 216.
             */
            char vdiv1[12];
            scope_cal_range_label(1u, ss->ch1.vdiv_idx, vdiv1, sizeof(vdiv1));
            draw_channel_field(2, "1", vdiv1, &ss->ch1,
                               ss->ch1.enabled ? th->ch1 : th->text_secondary, ib);

            /*
             * Timebase, DERIVED from the measured sample rate and this
             * renderer's 25 samples per division (stock's, parity C5), x2 for
             * a merged record and times the zoom at 0x00-0x03 — not read out
             * of timebase_table. Codes with no measured rate read "--".
             */
            char tb[12];
            if (ss->roll) {
                /* Roll (C11): 25 points per 32 px division, so stock's own
                 * label is what the grid means (scope_roll.h). */
                scope_roll_label(ss->roll_step, tb, sizeof(tb));
                snprintf(buf, sizeof(buf), "H=%s", tb);
            } else {
                scope_timebase_view_label(ss->timebase_idx,
                                          fpga_acq_record_interleaved(),
                                          tb, sizeof(tb));
                snprintf(buf, sizeof(buf), "H=%s", tb);
            }
            font_draw_string(128, LCD_HEIGHT - 14, buf,
                             th->text_primary, ib, &font_small);

            /* CH2: volts/div + coupling. Per-channel by construction — CH2's
             * frontend measures a different gain from CH1's on the same range
             * index, which is why the label cannot come from a shared table. */
            char vdiv2[12];
            scope_cal_range_label(2u, ss->ch2.vdiv_idx, vdiv2, sizeof(vdiv2));
            draw_channel_field(216, "2", vdiv2, &ss->ch2,
                               ss->ch2.enabled ? th->ch2 : th->text_secondary, ib);
        }
        break;
    case MODE_SIGNAL_GEN:
        font_draw_string(4, LCD_HEIGHT - 14, "Sine 1.000kHz",
                         th->ch1, ib, &font_small);
        font_draw_string(200, LCD_HEIGHT - 14, "3.3Vpp",
                         th->success, ib, &font_small);
        break;
    case MODE_SETTINGS:
        font_draw_string(4, LCD_HEIGHT - 14, "MENU/SELECT to navigate",
                         th->text_secondary, ib, &font_small);
        break;
    default:
        break;
    }
}

/* Boot splash.
 *
 * If a generated splash_image.h exists (see scripts/generate_splash.py — any
 * PNG/JPG dropped at firmware/assets/splash.png, letterboxed to 320x240 and
 * RLE-compressed at build-prep time), draw that. Otherwise fall back to the
 * original text splash, so the build never depends on the asset pipeline. */
#if defined(__has_include)
#  if __has_include("splash_image.h")
#    include "splash_image.h"
#  endif
#endif

void draw_splash(void)
{
#ifdef SPLASH_IMAGE_H
    /* Stream the RLE image straight into one full-screen window. Records are
     * (uint8 run, uint16 LE color); runs cross row boundaries freely. */
    lcd_set_window(0, 0, SPLASH_IMAGE_WIDTH, SPLASH_IMAGE_HEIGHT);
    const uint8_t *p   = splash_image_rle;
    const uint8_t *end = splash_image_rle + sizeof(splash_image_rle);
    while (p + 3 <= end) {
        uint8_t  run   = p[0];
        uint16_t color = (uint16_t)(p[1] | ((uint16_t)p[2] << 8));
        p += 3;
        while (run--)
            lcd_write_data(color);
    }
#else
    lcd_clear(COLOR_BLACK);

    /* Title - large font, centered */
    font_draw_string_center(LCD_WIDTH / 2, 50, "OpenScope 2C53T",
                            COLOR_CYAN, COLOR_BLACK, &font_large);

    /* Version */
    font_draw_string_center(LCD_WIDTH / 2, 85, "Custom Firmware v0.1",
                            COLOR_WHITE, COLOR_BLACK, &font_medium);

    /* Hardware info */
    font_draw_string_center(LCD_WIDTH / 2, 125, "AT32F403A | FreeRTOS",
                            COLOR_GRAY, COLOR_BLACK, &font_small);
    font_draw_string_center(LCD_WIDTH / 2, 142, "ST7789V 320x240",
                            COLOR_GRAY, COLOR_BLACK, &font_small);

    /* Repo */
    font_draw_string_center(LCD_WIDTH / 2, 190, "github.com/",
                            COLOR_DARK_GRAY, COLOR_BLACK, &font_small);
    font_draw_string_center(LCD_WIDTH / 2, 206, "DavidClawson/OpenScope-2C53T",
                            COLOR_DARK_GRAY, COLOR_BLACK, &font_small);
#endif
}
