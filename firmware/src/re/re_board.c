/*
 * OpenScope 2C53T - reverse-engineering toolkit: pins of unknown function
 * and the serial link to another chip (FEATURE_RETOOLS)
 *
 *   pinlog      which pin follows an event (a cable, a key, a relay)?
 *   usart raw   what does the chip on USART2 answer to these ten bytes?
 * The handlers were moved here unchanged from the bench shell
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
#include "re_fpga_driver.h"

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

/* usart raw: the link's counters before the frame and what moved after it. */
typedef struct {
    uint16_t tx_count;
    uint16_t rx_byte_count;
    uint16_t frame_count;
    uint16_t echo_count;
    uint16_t spi3_ok_count;
} fpga_diag_snapshot_t;

static void fpga_diag_snapshot_take(fpga_diag_snapshot_t *snap)
{
    snap->tx_count = fpga.tx_count;
    snap->rx_byte_count = fpga.rx_byte_count;
    snap->frame_count = fpga.frame_count;
    snap->echo_count = fpga.echo_count;
    snap->spi3_ok_count = fpga.spi3_ok_count;
}

static void fpga_diag_print_delta(const fpga_diag_snapshot_t *before)
{
    uint16_t tx_delta = fpga.tx_count - before->tx_count;
    uint16_t rx_delta = fpga.rx_byte_count - before->rx_byte_count;
    uint16_t frame_delta = fpga.frame_count - before->frame_count;
    uint16_t echo_delta = fpga.echo_count - before->echo_count;
    uint16_t spi_delta = fpga.spi3_ok_count - before->spi3_ok_count;

    usb_debug_printf("Delta: TX %+d RX %+d DF %+d EF %+d SPI %+d\r\n",
                     (int)tx_delta, (int)rx_delta, (int)frame_delta,
                     (int)echo_delta, (int)spi_delta);

    if ((frame_delta > 0 || echo_delta > 0) && fpga.rx_frame_valid) {
        usb_debug_printf("RX:");
        for (int i = 0; i < FPGA_RX_FRAME_SIZE; i++) {
            usb_debug_printf(" %02X", fpga.rx_frame[i]);
        }
        usb_send_str("\r\n");
    }
}

void cmd_usart_raw(const char *args)
{
    /* Parse exactly 10 space-separated hex bytes for a raw USART2 frame */
    uint8_t frame[10];
    int count = 0;
    fpga_diag_snapshot_t before;

    const char *p = args;
    while (*p && count < 10) {
        while (*p == ' ') p++;
        if (!*p) break;
        uint32_t val;
        if (parse_hex32(p, &val) != 0 || val > 0xFF) {
            usb_debug_printf("ERR: bad hex byte at '%s'\r\n", p);
            return;
        }
        frame[count++] = (uint8_t)val;
        while (*p && *p != ' ') p++;
    }

    if (count != 10) {
        usb_send_str("Usage: usart raw <10 hex bytes>\r\n"
                      "  e.g.: usart raw 00 00 00 0B 01 00 00 00 00 0B\r\n"
                      "  Format: [hdr0][hdr1][cmd_hi][cmd_lo][p1][p2][p3][p4][p5][cksum]\r\n");
        return;
    }

    if (!usart_tx_wire_live()) return;

    usb_debug_printf("TX raw:");
    for (int i = 0; i < 10; i++)
        usb_debug_printf(" %02X", frame[i]);
    usb_send_str("\r\n");

    fpga_diag_snapshot_take(&before);
    fpga_send_raw_frame(frame);

    /* Wait for response */
    vTaskDelay(pdMS_TO_TICKS(200));
    fpga_diag_print_delta(&before);
}
