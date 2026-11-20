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
 * SPI3, the bus to the FPGA, at 0x40003C00 (APB1 + 0x3C00). PB3/PB4/PB5 reach
 * it once JTAG is turned off. SPI4 (APB1 + 0x4000) is a different peripheral.
 */
#define FPGA_SPI       ((spi_type *)SPI3_BASE)

/* GPIO bit masks */
#define PB6_MASK        (1 << 6)   /* SPI3 CS, software-driven, active LOW */

#define PB11_MASK       (1 << 11)  /* CH2 range relay, bit 1 (scope_frontend.c) */

#define PC6_MASK        (1 << 6)   /* FPGA supply enable: LOW switches the FPGA off */

/* SPI3 CTRL1: master (bit 2), mode 3 (CPOL bit 1, CPHA bit 0), software
 * chip select held inactive (bits 9 and 8), divider /2, 8 bits, MSB first.
 * CTRL2 is 0: transfers are polled, no interrupt and no DMA request. */
#define SPI3_CTRL1_MODE3_MASTER  ((1u << 0) | (1u << 1) | (1u << 2) | (1u << 8) | (1u << 9))
#define SPI3_CTRL1_ENABLE        (1u << 6)

/* The three states of the SPI3 pins (fpga_boot.c). */
void spi3_pins_peripheral(void);
void spi3_pins_gpio(void);
void spi3_pins_released(void);

/* CS control macros */
#define SPI3_CS_ASSERT()    (GPIOB->clr = PB6_MASK)   /* PB6 LOW */

#define SPI3_CS_DEASSERT()  (GPIOB->scr = PB6_MASK)   /* PB6 HIGH */

/* One received meter frame and its counters, passed from the USART2 receive
 * interrupt to fpga_usart_rx_task through meter_rx_queue. */
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
 * visiting every screen: dvom_RX had 9 words left of 128 and dvom_TX 13 of
 * 64. Both doubled, which costs 192 words of heap. */
#define FPGA_DVOM_TX_STACK 128
#define FPGA_DVOM_RX_STACK 256
/* The acquisition task, and the poll task (meter function changes run on
 * it). Chosen, not measured to their limit; `tasks` shows what is left. */
#define FPGA_ACQ_STACK        256
#define FPGA_METER_POLL_STACK 192

/* ═══════════════════════════════════════════════════════════════════
 * SPI3 Low-Level Transfer
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * Full-duplex SPI3 byte exchange, polled: wait for TX empty, write, wait for
 * RX full, read. Returns the byte received while tx_byte went out.
 *
 * On a poll timeout it returns 0xFF, which looks like a real byte. So each
 * timeout also increments fpga.spi3_hw_timeouts; bulk readers compare that
 * counter before and after a read and drop a record with made-up bytes. The
 * flags depend only on the MCU (the master clocks whatever the FPGA does), so
 * a timeout means the SPI peripheral itself is disabled or stuck.
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
 * reads of that port use 7. The control writes after an upload also use 7. */
static inline void spi3_set_br(uint32_t br)
{
    FPGA_SPI->ctrl1 &= ~(1u << 6);              /* SPE = 0 */
    FPGA_SPI->ctrl1 = (FPGA_SPI->ctrl1 & ~(7u << 3)) | ((br & 7u) << 3);
    FPGA_SPI->ctrl1 |= (1u << 6);               /* SPE = 1 */
}

/* ═══════════════════════════════════════════════════════════════════
 * SysTick busy-wait delays, for boot before the scheduler starts.
 * They reprogram SysTick and stop it, so they must not run once FreeRTOS
 * owns SysTick; fpga_scope_delay_ms() picks the right delay for either case.
 * ═══════════════════════════════════════════════════════════════════ */

static inline void systick_delay_us(uint32_t us)
{
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
 * Bit-bang pins of the design upload (fpga_boot.c). The SPI3 pins, used
 * as plain GPIO, mode 3, MSB first. The sequence, after the loader by
 * Stlkv (OpenScope-2C23T-2C53T-port):
 *   IDCODE 0x11, USERCODE 0x13, STATUS 0x41, INIT_ADDR 0x12 00,
 *   CONFIG_ENABLE 0x15 00, 0x3B plus the whole design in one CS-low
 *   frame, STATUS, CONFIG_DISABLE 0x3A 00.
 * There is no reset pulse: on the 2C53T the FPGA's reset pin is wired to
 * the POWER button.
 * ═══════════════════════════════════════════════════════════════════ */
#define BB_SCK   (1u << 3)   /* PB3 */

#define BB_MISO  (1u << 4)   /* PB4 */

#define BB_MOSI  (1u << 5)   /* PB5 */

#define BB_CS    (1u << 6)   /* PB6 */

/* meter_link.c */
/* An item of the meter link's transmit queue: a command word (bytes[0] high,
 * bytes[1] low) sent obeyed or unobeyed, or a whole frame sent as given. */
typedef enum { METER_TX_WORD_OBEYED, METER_TX_WORD_UNOBEYED, METER_TX_RAW } meter_tx_kind_t;
typedef struct {
    uint8_t kind;                       /* a meter_tx_kind_t */
    uint8_t bytes[FPGA_TX_FRAME_SIZE];
} meter_tx_item_t;
#define METER_TX_QUEUE_LEN  10u
#define METER_RX_QUEUE_LEN   8u    /* data frames waiting for dvom_RX */
/* One frame is 10 bytes of 10 bits at 9600 baud: 10.4 ms. */
#define METER_TX_FRAME_MS          11u
#define METER_TX_FRAME_TIMEOUT_MS  (2u * METER_TX_FRAME_MS)                    /* dvom_TX gives a frame up */
#define METER_TX_QUEUE_WAIT_MS     (METER_TX_QUEUE_LEN * METER_TX_FRAME_MS)    /* a full queue has drained */
bool meter_tx_raw_frame(const uint8_t *frame);
extern QueueHandle_t     usart_tx_queue;
extern QueueHandle_t meter_rx_queue;
extern TaskHandle_t      tx_task_handle;
extern TaskHandle_t      rx_task_handle;
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
void fpga_set_scope_frontend_ranges(void);

/* fpga_boot.c */
void bb_cmd16(uint8_t hi, uint8_t lo);
void fpga_boot_upload(void);

#endif /* FPGA_INTERNAL_H */
