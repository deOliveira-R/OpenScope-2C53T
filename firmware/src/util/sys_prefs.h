/*
 * sys_prefs.h - brightness, sound volume and auto power-off (parity C15).
 *
 * Pure logic, no hardware: the level tables and clamps, the live values the
 * settings store captures/restores, the key-click beep timer and the idle
 * power-off timer. src/drivers/sys_pwm.c turns the numbers into TMR10/TMR11
 * duty; the input task feeds activity and polls the timer. Evidence is the
 * static RE in docs/re/parity2_static.md section 4.2/4.3 and the stock
 * settings record (ms[0xF61] brightness, ms[0xF62] volume, ms[0xF65] auto-off).
 */
#ifndef SYS_PREFS_H
#define SYS_PREFS_H

#include <stdint.h>
#include <stdbool.h>

/* ── Brightness: PB8 = TMR10 CH1, ARR 99, duty = percent ─────────────── */
#define SYS_BRIGHT_MIN      25u   /* stock never goes below 25 (0x0800B448) */
#define SYS_BRIGHT_MAX      100u
#define SYS_BRIGHT_STEP     5u
#define SYS_BRIGHT_DEFAULT  100u  /* stock factory value */
#define SYS_BACKLIGHT_ARR   99u   /* 1 MHz count / 100 = 10 kHz */

/* Snap any stored byte to a legal level: 0 or > 100 (blank flash, foreign
 * record) -> default; otherwise clamp to 25..100 and floor to the step grid. */
uint8_t sys_bright_clamp(uint8_t raw);
/* One step: dir > 0 up, dir < 0 down; saturates at 25 and 100. */
uint8_t sys_bright_step(uint8_t level, int dir);
/* Timer compare value for a level (CCR/(ARR+1) = percent). */
uint16_t sys_bright_ccr(uint8_t level);

/* ── Sound: PB9 = TMR11 CH1, ARR 24, 2 kHz, duty = volume/25 ─────────── */
#define SYS_VOL_MAX         10u
#define SYS_VOL_DEFAULT     1u    /* stock factory value */
#define SYS_BUZZER_ARR      24u
#define SYS_BEEP_SOC_DUTY   10u   /* meter SoC beep: constant 10 (EXTI3 ISR) */
#define SYS_BEEP_MS         110u  /* 11 housekeeping ticks of ~10 ms [H] */

uint8_t sys_vol_clamp(uint8_t raw);              /* > 10 -> default */
uint8_t sys_vol_step(uint8_t vol, int dir);      /* saturates at 0 and 10 */

/* ── Auto power-off: stock 0/15/30/60 min ─────────────────────────────── */
#define SYS_AUTOOFF_COUNT   4u
uint8_t  sys_autooff_clamp(uint8_t mins);        /* not in table -> 0 (off) */
uint8_t  sys_autooff_step(uint8_t mins, int dir, bool wrap);
const char *sys_autooff_label(uint8_t mins);     /* "Off", "15 min", ... */

/* Live values (the settings store captures and restores these). */
uint8_t sys_prefs_brightness(void);
uint8_t sys_prefs_volume(void);
uint8_t sys_prefs_autooff_mins(void);
void    sys_prefs_set_brightness(uint8_t raw);   /* clamps */
void    sys_prefs_set_volume(uint8_t raw);
void    sys_prefs_set_autooff_mins(uint8_t raw);

/* ── Beep timer (wrap-safe, ms) ───────────────────────────────────────── */
typedef struct {
    uint32_t end_ms;
    uint8_t  duty;    /* 0 = idle */
} sys_beep_t;

void    sys_beep_start(sys_beep_t *b, uint32_t now_ms, uint8_t duty, uint32_t dur_ms);
/* Duty that should be on the pin now: the beep's duty while it lasts, 0 after. */
uint8_t sys_beep_poll(sys_beep_t *b, uint32_t now_ms);

/* ── Idle power-off timer ─────────────────────────────────────────────
 * Stock resets its idle counter on every handled key event and, at the
 * limit, saves settings then drops PC9. poll() returns true exactly once per
 * expiry; the caller then flushes settings and powers off. A limit of 0
 * minutes never expires. */
typedef struct {
    uint32_t limit_ms;
    uint32_t last_ms;
    bool     fired;
} sys_autooff_t;

void sys_autooff_init(sys_autooff_t *a, uint8_t mins, uint32_t now_ms);
void sys_autooff_set_minutes(sys_autooff_t *a, uint8_t mins, uint32_t now_ms);
void sys_autooff_activity(sys_autooff_t *a, uint32_t now_ms);
bool sys_autooff_poll(sys_autooff_t *a, uint32_t now_ms);

#endif /* SYS_PREFS_H */
