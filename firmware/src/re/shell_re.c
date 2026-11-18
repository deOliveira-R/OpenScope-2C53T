/*
 * OpenScope 2C53T - reverse-engineering toolkit: shell commands (FEATURE_RETOOLS)
 *
 * See shell_re.h for what belongs here and docs/re/README.md for the method.
 *
 * re pinsweep: which MCU pin is wired to the FPGA's RECONFIG_N?
 *
 * A Gowin FPGA that is running a design does not accept a new one over its
 * configuration port until RECONFIG_N is pulsed low (or power is cycled).
 * On an unknown board that pin has to be found. The sweep pulses one
 * candidate at a time and reads the configuration port's STATUS register:
 *   phase P  the STATUS moved after the pulse alone: the pulse made the
 *            FPGA drop or reload its design. That is RECONFIG_N.
 *   phase C  it moved only after CONFIG_ENABLE (opcode 0x15) was sent: the
 *            pulse opened the configuration port.
 * Every STATUS read is paired with an IDCODE read through the same path;
 * a read whose IDCODE is wrong is thrown away, never taken as a change
 * (a pin that closes the port shows up as "port not readable" instead).
 * The STATUS is sampled for a while after each step, not once: a part that
 * reloads from its own flash is back to the first value within milliseconds.
 *
 * Board data (candidates, pins never to pulse, IDCODE): pin_sweep_table.h.
 * Each pin's mode and output level are put back before the next one.
 * After a hit the FPGA may hold no design: `fpga reload builtin`.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "at32f403a_407.h"
#include "FreeRTOS.h"
#include "task.h"

#include "usb_debug.h"
#define SHELL_FEATURE_FILE   /* shell_internal.h: the shell core's helper names */
#include "shell_internal.h"
#include "shell_bench.h"     /* the config-port read and the pin-mode helpers */
#include "shell_re.h"
#include "pin_sweep_table.h"

/* Gowin configuration-port opcodes (Gowin TN653 / openFPGALoader gowin.cpp). */
#define GW_OP_READ_IDCODE     0x11u
#define GW_OP_CONFIG_ENABLE   0x15u
#define GW_OP_READ_STATUS     0x41u
/* SPI3 clock divider code for /256 (about 470 kHz): the only clock at which
 * this board's configuration-port reads are right (bench, 2026-08). */
#define RE_CFGPORT_BAUD_DIV256  7u

/* Sweep timing. RECONFIG_N needs 25 ns low (Gowin UG290); the rest is how
 * long a reload could take to show and how often to look. */
#define RE_SWEEP_LOW_MS         10u   /* candidate held low */
#define RE_SWEEP_RELEASE_MS     1u    /* after release, before the first read */
#define RE_SWEEP_SAMPLES        12u   /* STATUS reads per phase */
#define RE_SWEEP_SAMPLE_GAP_MS  10u   /* between them: 120 ms watched per phase */

#define RE_GPIO_MODE_OUT_PP_50MHZ  0x3u   /* CNF/MODE nibble: push-pull output */
#define RE_STATUS_UNREADABLE       0xFFFFFFFFUL

static gpio_type *re_port(char letter)
{
    switch (letter) {
    case 'A': return GPIOA;
    case 'B': return GPIOB;
    case 'C': return GPIOC;
    case 'D': return GPIOD;
    case 'E': return GPIOE;
    default:  return NULL;
    }
}

/* STATUS, or RE_STATUS_UNREADABLE when the IDCODE read beside it is wrong. */
static uint32_t re_status_anchored(void)
{
    const uint32_t id = spi3_gowin_read_reg(GW_OP_READ_IDCODE);
    const uint32_t st = spi3_gowin_read_reg(GW_OP_READ_STATUS);
    return (id == RE_FPGA_IDCODE) ? st : RE_STATUS_UNREADABLE;
}

/* Watch the STATUS for one phase. Returns true at the first valid value
 * that differs from the baseline (*seen); *unreadable counts bad reads. */
static bool re_watch(uint32_t baseline, uint32_t *seen, unsigned *unreadable)
{
    for (unsigned k = 0; k < RE_SWEEP_SAMPLES; k++) {
        const uint32_t st = re_status_anchored();
        if (st == RE_STATUS_UNREADABLE) (*unreadable)++;
        else if (st != baseline) { *seen = st; return true; }
        vTaskDelay(pdMS_TO_TICKS(RE_SWEEP_SAMPLE_GAP_MS));
    }
    return false;
}

static void re_pinsweep_list(void)
{
    usb_debug_printf("re pinsweep: %u candidates (src/re/pin_sweep_table.h)\r\n", (unsigned)RE_SWEEP_N);
    for (unsigned i = 0; i < RE_SWEEP_N; i++)
        usb_debug_printf("  P%c%-2u  %s\r\n", RE_SWEEP_CANDIDATES[i].port,
                         (unsigned)RE_SWEEP_CANDIDATES[i].pin, RE_SWEEP_CANDIDATES[i].why);
    usb_debug_printf("never pulsed (%u pins):", (unsigned)RE_SWEEP_EXCLUDED_N);
    for (unsigned i = 0; i < RE_SWEEP_EXCLUDED_N; i++)
        usb_debug_printf(" P%c%u", RE_SWEEP_EXCLUDED[i].port, (unsigned)RE_SWEEP_EXCLUDED[i].pin);
    usb_send_str("\r\n`re pinsweep run` pulses each candidate low for 10 ms. A hit may leave the FPGA\r\n"
                 "without a design: `fpga reload builtin` puts the capture core back.\r\n");
}

void cmd_re_pinsweep(const char *args)
{
    while (*args == ' ') args++;
    if (*args == '\0') { re_pinsweep_list(); return; }
    if (strcmp(args, "run") != 0) { usb_send_str("usage: re pinsweep [run]\r\n"); return; }

    const uint32_t saved_ctrl1 = spi3_set_baud(RE_CFGPORT_BAUD_DIV256);
    const uint32_t baseline = re_status_anchored();
    if (baseline == RE_STATUS_UNREADABLE) {
        usb_send_str("re pinsweep: the configuration port does not answer with the IDCODE; nothing pulsed\r\n");
        (void)spi3_set_baud((saved_ctrl1 >> 3) & 7u);
        return;
    }
    usb_debug_printf("re pinsweep: baseline STATUS %08lX\r\n", (unsigned long)baseline);

    unsigned hits = 0, port_lost = 0;
    for (unsigned i = 0; i < RE_SWEEP_N; i++) {
        const char    letter = RE_SWEEP_CANDIDATES[i].port;
        const uint8_t pin    = RE_SWEEP_CANDIDATES[i].pin;
        gpio_type    *port   = re_port(letter);
        if (port == NULL || !re_pin_sweepable(letter, pin)) {
            usb_debug_printf("  P%c%-2u  skipped: %s\r\n", letter, (unsigned)pin,
                             re_pin_excluded(letter, pin) ? re_pin_excluded(letter, pin) : "not a pin");
            continue;
        }
        const uint32_t mask = 1u << pin;

        /* the pin as found: its mode nibble and its output latch */
        const uint8_t  saved_mode  = gpio_cfg_nibble(port, pin);
        const uint32_t saved_level = port->odt & mask;

        port->scr = mask;                                   /* high before it becomes an output */
        gpio_cfg_nibble_set(port, pin, RE_GPIO_MODE_OUT_PP_50MHZ);
        port->clr = mask;                                   /* low: the pulse */
        vTaskDelay(pdMS_TO_TICKS(RE_SWEEP_LOW_MS));
        port->scr = mask;
        vTaskDelay(pdMS_TO_TICKS(RE_SWEEP_RELEASE_MS));

        uint32_t seen = 0; unsigned unreadable = 0; char phase = '-';
        if (re_watch(baseline, &seen, &unreadable)) {
            phase = 'P';
        } else {
            GPIOB->clr = (1u << 6);                         /* SPI3 chip select (PB6) low */
            (void)spi3_raw_xfer(GW_OP_CONFIG_ENABLE);
            (void)spi3_raw_xfer(0x00);
            GPIOB->scr = (1u << 6);
            if (re_watch(baseline, &seen, &unreadable)) phase = 'C';
        }

        /* put the pin back before anything is reported */
        gpio_cfg_nibble_set(port, pin, saved_mode);
        if (saved_level) port->scr = mask; else port->clr = mask;

        if (phase != '-') {
            hits++;
            usb_debug_printf("  P%c%-2u  HIT, phase %c: STATUS %08lX\r\n", letter, (unsigned)pin, phase,
                             (unsigned long)seen);
        } else if (unreadable) {
            port_lost++;
            usb_debug_printf("  P%c%-2u  port not readable in %u reads (not a hit; a pin that closes the port looks like this)\r\n",
                             letter, (unsigned)pin, unreadable);
        } else {
            usb_debug_printf("  P%c%-2u  no change\r\n", letter, (unsigned)pin);
        }
    }
    (void)spi3_set_baud((saved_ctrl1 >> 3) & 7u);
    usb_debug_printf("re pinsweep: %u hits, %u with the port not readable, of %u candidates%s\r\n",
                     hits, port_lost, (unsigned)RE_SWEEP_N,
                     hits ? ". The FPGA may hold no design now: `fpga reload builtin`" : "");
}
