/*
 * OpenScope 2C53T - shared between the files of the FPGA driver
 *
 * fpga.c, fpga_boot.c, fpga_acq.c, scope_frontend.c, meter_link.c and the
 * toolkit's re_fpga_driver.c. Nothing else includes this; the public
 * interface is fpga.h.
 */
#ifndef FPGA_INTERNAL_H
#define FPGA_INTERNAL_H

#include "fpga.h"
#include "flash_fs.h"
#include "../util/fpga_slot.h"
#include "../util/cal_backup_core.h"
#include "fpga_meter_plan.h"
#include "meter_data.h"
#include "meter_stock.h"
#include "scope_trigger.h"
#include "scope_vpos.h"
#include "../dsp/trig_edge.h"
#include "../dsp/trig_edge_reg.h"
#include "../dsp/interleave.h"
#include "../dsp/interleave_cal.h"
#include "../dsp/trig_policy.h"
#include "../dsp/acq_timing.h"
#include "../dsp/acq_cfg.h"
#include "../ui/ui.h"
#include "../ui/scope_state.h"
#include "../ui/scope_timebase.h"
#include "../ui/scope_mask.h"
#include "at32f403a_407.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════
 * Hardware Register Access
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * SPI3 register access.
 * AT32F403A SPI3 is at APB1 + 0x3C00 (same address as GD32's SPI2/STM32 SPI3).
 * PB3/PB4/PB5 map here after JTAG remap. SPI4 is a DIFFERENT peripheral
 * at APB1 + 0x4000 — do not confuse them.
 */
#define FPGA_SPI       ((spi_type *)SPI3_BASE)

/* GPIO bit operations */
#define PB6_MASK        (1 << 6)   /* SPI3 CS */

#define PB11_MASK       (1 << 11)  /* FPGA active mode */

#define PC6_MASK        (1 << 6)   /* FPGA SPI enable */

/* CS control macros */
#define SPI3_CS_ASSERT()    (GPIOB->clr = PB6_MASK)   /* PB6 LOW */

#define SPI3_CS_DEASSERT()  (GPIOB->scr = PB6_MASK)   /* PB6 HIGH */

typedef struct {
    uint8_t frame[FPGA_RX_FRAME_SIZE];
    uint32_t frame_count;
    uint32_t tx_count;
    uint32_t echo_count;
    uint32_t mode_sequence_count;
    uint8_t mode_sequence_submode;
    uint8_t discard_remaining;
    uint8_t transition_busy;
} fpga_meter_rx_event_t;

/* Stack depths in words. Measured on unit #2 with `tasks` (2026-11-14) after
 * a key walk over every screen: dvom_RX had 9 words left of 128 and dvom_TX
 * 13 of 64 (the frame decode and the transition code run on them). Doubled;
 * 192 words of heap. */
#define FPGA_DVOM_TX_STACK 128

#define FPGA_DVOM_RX_STACK 256

/* ═══════════════════════════════════════════════════════════════════
 * SPI3 Low-Level Transfer
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * Full-duplex SPI3 byte exchange.
 * Sends tx_byte, returns the byte received simultaneously.
 * Matches stock firmware's optimized TXE/RXNE polling pattern.
 *
 * On poll expiry this returns 0xFF — indistinguishable from a genuine
 * all-ones byte, which is the project's signature defect shape (a stable
 * plausible wrong number). It cannot return a status without changing every
 * call site, so instead each expiry bumps fpga.spi3_hw_timeouts; bulk readers
 * snapshot that counter around their read loop and refuse to publish a frame
 * whose bytes were partly fabricated (audit 2026-08-20, P0.3). In practice
 * the flags are MCU-side (master clocks regardless of the FPGA), so expiries
 * mean the SPI peripheral itself is wedged/disabled — rare, and exactly when
 * a confident flat trace would be the worst possible output.
 */
static inline uint8_t spi3_xfer(uint8_t tx_byte)
{
    volatile uint32_t timeout;

    /* Wait for TX buffer empty */
    timeout = 100000;
    while (!(FPGA_SPI->sts & SPI_I2S_TDBE_FLAG)) {
        if (--timeout == 0) { fpga.spi3_hw_timeouts++; return 0xFF; }
    }
    FPGA_SPI->dt = tx_byte;

    /* Wait for RX buffer not empty */
    timeout = 100000;
    while (!(FPGA_SPI->sts & SPI_I2S_RDBF_FLAG)) {
        if (--timeout == 0) { fpga.spi3_hw_timeouts++; return 0xFF; }
    }
    return (uint8_t)FPGA_SPI->dt;
}

/* Set the SPI3 baud-rate divider (CTRL1 bits [5:3]); SPE goes off and on
 * around the change. br: 0 = /2 (60 MHz, what fpga_init() programs),
 * 1 = /4 ... 7 = /256. The FPGA's configuration port answers correctly only
 * at a slow clock (its IDCODE reads as garbage at /2 and clean at /256), so
 * reads of that port use 7. */
static inline void spi3_set_br(uint32_t br)
{
    FPGA_SPI->ctrl1 &= ~(1u << 6);              /* SPE = 0 */
    FPGA_SPI->ctrl1 = (FPGA_SPI->ctrl1 & ~(7u << 3)) | ((br & 7u) << 3);
    FPGA_SPI->ctrl1 |= (1u << 6);               /* SPE = 1 */
}

/* ═══════════════════════════════════════════════════════════════════
 * SysTick Delay (pre-RTOS, matches stock firmware timing)
 * ═══════════════════════════════════════════════════════════════════ */

static inline void systick_delay_us(uint32_t us)
{
    /* Use SysTick for precise microsecond delays.
     * Stock firmware uses this between boot phases. */
    uint32_t ticks = (system_core_clock / 1000000) * us;
    SysTick->LOAD = ticks - 1;
    SysTick->VAL = 0;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;
    while (!(SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk)) {}
    SysTick->CTRL = 0;
}

static inline void systick_delay_ms(uint32_t ms)
{
    while (ms--) {
        systick_delay_us(1000);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * Build B (2026-08-13, `make guest-configB`) — true bit-bang transplant
 * of the maksidze/Stlkv 2C23T-V0.4 loader that cold-started the 2C53T FPGA
 * on issue #18 (STATUS 0x00039020 → 0x0003F460, DONE_FINAL set, 4/4 boots).
 *
 * Ported byte-for-byte from Stlkv/OpenScope-2C23T-2C53T-port `2c53t-port`
 * (src/fpga.c fpga53_v04_configure): GPIO-mode PB3/4/5/6 (NOT SPI3 AF),
 * mode-3 MSB-first, and the V0.4 framing —
 *   IDCODE(0x11) → USERCODE(0x13) → STATUS(0x41) → INIT_ADDR(0x12 00)
 *   → CONFIG_ENABLE(0x15 00) → CS-LOW 0x3B + full payload → STATUS → 0x3A.
 * NOTE the V0.4 sequence has NO 0x05 ERASE_SRAM prelude (unlike our
 * hardware-SPI sequence) and NO reset pulse (the 2C53T reset pin maps to
 * the POWER button). This is the last remaining variable after Build A:
 * GPIO-mode/bit-bang clocking vs hardware-SPI AF. If A held and B breaks
 * the wall, GPIO-vs-AF is the answer; if both hold, the difference is off
 * our pins entirely. Reads land in the same fpga.* fields Build A uses, so
 * the LCD overlay verdict is identical: CFG line D1 = DONE_FINAL.
 * ═══════════════════════════════════════════════════════════════════ */
#define BB_SCK   (1u << 3)   /* PB3 */

#define BB_MISO  (1u << 4)   /* PB4 */

#define BB_MOSI  (1u << 5)   /* PB5 */

#define BB_CS    (1u << 6)   /* PB6 */

/* meter_link.c */
extern QueueHandle_t     usart_tx_queue;
extern QueueHandle_t meter_rx_queue;
extern TaskHandle_t      tx_task_handle;
extern TaskHandle_t      rx_task_handle;
void usart2_send_frame(const uint8_t *frame);
void fpga_record_tx_frame(const uint8_t *frame);
void fpga_usart_tx_task(void *pv);
void fpga_usart_rx_task(void *pv);
extern TaskHandle_t       meter_poll_handle;
void fpga_meter_poll_task(void *pv);

/* fpga_acq.c */
extern TaskHandle_t      acq_task_handle;
void fpga_scope_delay_ms(uint32_t ms);
extern uint8_t *acq_stage_ch1;
extern uint8_t *acq_stage_ch2;
extern volatile bool acq_reprime_req;
extern volatile bool    acq_design_ours;
void acq_core_ok_set(bool ok);
extern volatile uint8_t acq_edge_reg;
void fpga_acq_task(void *pv);

/* scope_frontend.c */
void fpga_set_scope_frontend_ranges(const scope_state_t *ss);

/* fpga_boot.c */
void bb_cmd16(uint8_t hi, uint8_t lo);
void fpga_boot_upload(void);

#endif /* FPGA_INTERNAL_H */
