#ifndef LCD_HEALTH_H
#define LCD_HEALTH_H
/*
 * LCD panel health: detect an ST7789 that has reset itself under a running MCU.
 *
 * Seen once on unit #2 (2026-11-11): the panel went white with the MCU fine.
 * RDDPM read 0x08 (sleep in, display off) and COLMOD 0x06, its power-on state.
 * App images never initialise the LCD (the bootloader does), so nothing
 * recovered it. The display task now reads RDDPM about once a second. Two bad
 * reads in a row re-send the panel init and force a full redraw. Each event
 * is logged for the cause hunt. Pure logic; host test: tests/test_lcd_health.c.
 */
#include <stdbool.h>
#include <stdint.h>

#define LCD_PM_SLEEP_OUT   0x10u
#define LCD_PM_DISPLAY_ON  0x04u

typedef struct {
    uint32_t checks;        /* RDDPM reads */
    uint32_t bad_reads;     /* reads with sleep-out or display-on clear */
    uint32_t events;        /* re-inits done */
    uint32_t last_event_ms; /* uptime of the last re-init */
    uint8_t  last_pm;       /* last RDDPM value read */
    uint8_t  event_pm;      /* RDDPM that triggered the last re-init */
    uint8_t  event_mode;    /* UI mode at the last re-init */
    uint8_t  streak;        /* consecutive bad reads */
    /* Context of the last event, for the cause hunt (set by the caller). */
    uint32_t event_since_key_ms; /* time since the last key event */
    uint16_t event_key;          /* that key's id (BTN_LONG_FLAG included) */
    uint8_t  event_buzzer;       /* TMR11 C1DT: buzzer duty (0 = silent) */
    uint8_t  event_backlight;    /* TMR10 C1DT: backlight duty */
} lcd_health_t;

extern lcd_health_t g_lcd_health;

/* True when the power mode says the panel is awake and showing. */
bool lcd_health_pm_ok(uint8_t pm);

/* Feed one RDDPM read. Returns true when the caller must re-init the panel
 * (the second consecutive bad read); the event is then recorded. */
bool lcd_health_step(lcd_health_t *h, uint8_t pm, uint32_t now_ms, uint8_t mode);

#endif
