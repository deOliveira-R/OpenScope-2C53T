/*
 * OpenScope 2C53T - reverse-engineering toolkit: pins of unknown function
 * (FEATURE_RETOOLS)
 *
 *   pinlog   which pin follows an event (a cable, a key, a relay)?
 * The handler was moved here unchanged from the bench shell
 * (src/drivers/shell_bench.c). Rows: shell_re.h. Guide: docs/re/README.md.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "at32f403a_407.h"
#include "FreeRTOS.h"
#include "task.h"

#include "usb_debug.h"
#include "fpga.h"
#define SHELL_FEATURE_FILE   /* shell_internal.h: the shell core's helper names */
#include "shell_internal.h"
#include "shell_bench.h"     /* the bench shell's bus helpers */
#include "shell_re.h"

/* pinlog [start]. Once a second (usb_debug_pinlog_tick, from the
 * 1 s timer) store every GPIO port's input word and the last battery ADC
 * result, so a pin that follows "USB attached" can be found: start it,
 * unplug the cable for ~10 s, plug it back, then `pinlog` prints the rows. */
#define PINLOG_N 90
static struct { uint16_t t; uint16_t idt[5]; uint16_t adc; } s_pinlog[PINLOG_N];
static volatile uint8_t s_pinlog_n;
static volatile uint8_t s_pinlog_on;
static uint16_t s_pinlog_t;

/* Rows are stored only on a change: a quiet pin flips (the LCD bus on D/E,
 * PA6, PB4-PB6, PB8 (PWM, backlight) and PC0 strobe are masked out) or the battery ADC moves by 12
 * counts (~20 mV). So the log can stay armed for hours. */
void retools_pinlog_tick(void)
{
    if (!s_pinlog_on) return;
    s_pinlog_t++;
    if (s_pinlog_n >= PINLOG_N) return;
    const uint16_t a = (uint16_t)GPIOA->idt, b = (uint16_t)GPIOB->idt, c = (uint16_t)GPIOC->idt;
    const uint16_t adc = (uint16_t)(ADC1->odt & 0xFFFu);
    const uint8_t i = s_pinlog_n;
    if (i > 0) {
        const int d = (int)adc - (int)s_pinlog[i - 1].adc;
        if (((a ^ s_pinlog[i - 1].idt[0]) & (uint16_t)~(1u << 6)) == 0 &&
            ((b ^ s_pinlog[i - 1].idt[1]) & (uint16_t)~0x0170u) == 0 &&
            ((c ^ s_pinlog[i - 1].idt[2]) & (uint16_t)~(1u << 0)) == 0 &&
            d > -12 && d < 12)
            return;
    }
    s_pinlog[i].t = s_pinlog_t;
    s_pinlog[i].idt[0] = a; s_pinlog[i].idt[1] = b; s_pinlog[i].idt[2] = c;
    s_pinlog[i].idt[3] = (uint16_t)GPIOD->idt;
    s_pinlog[i].idt[4] = (uint16_t)GPIOE->idt;
    s_pinlog[i].adc    = adc;
    s_pinlog_n = (uint8_t)(i + 1u);
}

void cmd_pinlog(const char *args)
{
    while (*args == ' ') args++;
    if (strncmp(args, "start", 5) == 0) {
        s_pinlog_on = 0; s_pinlog_n = 0; s_pinlog_t = 0; s_pinlog_on = 1;
        usb_debug_printf("pinlog: armed, %d change rows\r\n", PINLOG_N);
        return;
    }
    const uint8_t n = s_pinlog_n;
    usb_debug_printf("pinlog: %u rows at %u s (s: A B C D E adc)\r\n", (unsigned)n, (unsigned)s_pinlog_t);
    for (uint8_t i = 0; i < n; i++)
        usb_debug_printf("%u: %04X %04X %04X %04X %04X %u\r\n", (unsigned)s_pinlog[i].t,
                         s_pinlog[i].idt[0], s_pinlog[i].idt[1], s_pinlog[i].idt[2],
                         s_pinlog[i].idt[3], s_pinlog[i].idt[4], (unsigned)s_pinlog[i].adc);
}
