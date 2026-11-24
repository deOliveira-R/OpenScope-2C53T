/*
 * OpenScope 2C53T - entry into our bootloader, and its boot handshake
 *
 * Two RAM words are shared with the bootloader at 0x08000000: the update
 * request and its count of boots that have not checked in. Both sit just below
 * _estack, which startup does not clear. Interrupts use that stack later, so
 * the words only mean something across a hand-over.
 */

#include "dfu_boot.h"
#include "at32f403a_407.h"

#define DFU_MAGIC_ADDR   ((volatile uint32_t *)0x20037FE0)
#define DFU_MAGIC_VALUE  0xDEADBEEF

/* The boot count, as the bootloader defines it: magic in the upper half. */
#define BOOT_COUNTER_ADDR    ((volatile uint32_t *)0x20037FDC)
#define BOOT_COUNTER_MAGIC   0xB0070000

/* Hand the chip to the bootloader at 0x08000000 WITHOUT a reset.
 *
 * On the 2C53T the supply is latched by PC9 (power hold), which the bootloader
 * drives high first thing. A system reset returns PC9 to an input, the latch
 * drops and the scope switches off unless POWER happens to be held (bench,
 * 2026-11-10): every `reboot bootloader` and every flash then needed a button
 * press. A direct jump keeps PC9 driven, so updates can run unattended.
 *
 * The chip is made quiet first, as the bootloader's own jump_to_app() does:
 * interrupts and SysTick off, all NVIC enables and pendings cleared, DMA
 * channels stopped (a running transfer would scribble on the bootloader's
 * RAM), and the USB peripheral reset so the host sees a disconnect and
 * re-enumerates the HID updater. Clocks are left as they are: the bootloader's
 * system_clock_config() starts with crm_reset(). We are in a FreeRTOS task
 * (thread mode, PSP), so CONTROL goes back to MSP before the new stack is set. */
static void dfu_jump_to_bootloader(void) __attribute__((noreturn));
static void dfu_jump_to_bootloader(void)
{
    const uint32_t base = 0x08000000u;
    uint32_t sp = *(const volatile uint32_t *)base;
    uint32_t pc = *(const volatile uint32_t *)(base + 4u);

    __disable_irq();
    __set_BASEPRI(0u);           /* a crash hook may arrive with it raised */
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;
    SCB->ICSR = SCB_ICSR_PENDSVCLR_Msk | SCB_ICSR_PENDSTCLR_Msk;
    for (int i = 0; i < 8; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFu;
        NVIC->ICPR[i] = 0xFFFFFFFFu;
    }
    dma_channel_type *const ch[] = {
        DMA1_CHANNEL1, DMA1_CHANNEL2, DMA1_CHANNEL3, DMA1_CHANNEL4, DMA1_CHANNEL5,
        DMA1_CHANNEL6, DMA1_CHANNEL7, DMA2_CHANNEL1, DMA2_CHANNEL2, DMA2_CHANNEL3,
        DMA2_CHANNEL4, DMA2_CHANNEL5, DMA2_CHANNEL6, DMA2_CHANNEL7,
    };
    for (unsigned i = 0; i < sizeof(ch) / sizeof(ch[0]); i++) ch[i]->ctrl_bit.chen = 0;
    crm_periph_reset(CRM_USB_PERIPH_RESET, TRUE);
    crm_periph_reset(CRM_USB_PERIPH_RESET, FALSE);

    if ((sp & 0xFFF00000u) != 0x20000000u || pc < base || pc >= base + 0x4000u) {
        NVIC_SystemReset();      /* no sane bootloader there: the old way */
    }
    SCB->VTOR = base;
    __set_CONTROL(0);            /* privileged, MSP, no FP context */
    __ISB();
    __set_MSP(sp);
    __enable_irq();              /* as after reset; every NVIC line is off */
    ((void (*)(void))pc)();
    for (;;) { }
}

void dfu_request_reboot(void) {
    *DFU_MAGIC_ADDR = DFU_MAGIC_VALUE;
    __DSB();
    dfu_jump_to_bootloader();
}

void dfu_restart_app(void) {
    *DFU_MAGIC_ADDR = 0u;        /* a plain boot of the app, not the updater */
    *BOOT_COUNTER_ADDR = 0u;     /* the stack reuses this word: never read it as a count */
    __DSB();
    dfu_jump_to_bootloader();
}

void boot_validate(void) {
    /* The bootloader adds one before every jump here and opens SAFE MODE at
     * three, on an entry where RAM survived. Without this write, successive
     * updates in one power-on would add up to a false SAFE MODE. */
    *BOOT_COUNTER_ADDR = BOOT_COUNTER_MAGIC;  /* count = 0, magic preserved */
    __DSB();
}
