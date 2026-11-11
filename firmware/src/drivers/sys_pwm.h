/*
 * sys_pwm.h - backlight (PB8 / TMR10 CH1) and buzzer (PB9 / TMR11 CH1) drive,
 * the way stock does it (docs/re/parity2_static.md 4.2 and 4.3). The levels,
 * clamps and beep timing are in src/util/sys_prefs.c.
 */
#ifndef SYS_PWM_H
#define SYS_PWM_H

#include <stdint.h>

/* Bring both timers up. Call once, after settings_store_init() (so the stored
 * level is known) and before the scheduler. Backlight: the pin was held high
 * as a GPIO until now; the compare register is loaded with the stored level
 * before the pin is handed to the timer, so there is no dark gap. */
void sys_pwm_init(void);

/* Push sys_prefs brightness to the timer (after an edit). */
void sys_pwm_apply_brightness(void);
/* Power-off sequencing: stock writes C1DT = 0 before the rail drops. */
void sys_pwm_backlight_off(void);

/* Key click: a fixed-length beep at the stored volume; a volume of 0 is silent.
 * Safe from any task. The beep ends in sys_pwm_buzzer_service(). */
void sys_pwm_click(void);
/* Boot beep (stock 0x0802E370): same beep, called once the scheduler runs. */
void sys_pwm_boot_beep(void);
/* Continuity / SoC beep line: a constant duty (10) independent of the volume
 * (stock EXTI3 ISR 0x08009C10), held until released with 0. */
void sys_pwm_buzzer_hold(uint8_t duty);
/* Apply the highest of the active sources to TMR11 C1DT and end finished
 * beeps. Called from the buzzer task every loop; cheap. */
void sys_pwm_buzzer_service(void);

#endif /* SYS_PWM_H */
