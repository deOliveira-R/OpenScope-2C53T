#include "lcd_health.h"

lcd_health_t g_lcd_health;

bool lcd_health_pm_ok(uint8_t pm)
{
    const uint8_t need = LCD_PM_SLEEP_OUT | LCD_PM_DISPLAY_ON;
    return (pm & need) == need;
}

bool lcd_health_step(lcd_health_t *h, uint8_t pm, uint32_t now_ms, uint8_t mode)
{
    h->checks++;
    h->last_pm = pm;
    if (lcd_health_pm_ok(pm)) {
        h->streak = 0;
        return false;
    }
    h->bad_reads++;
    if (++h->streak < 2u)
        return false;
    h->streak = 0;
    h->events++;
    h->last_event_ms = now_ms;
    h->event_pm = pm;
    h->event_mode = mode;
    return true;
}
