/*
 * Host tests for brightness / sound / auto power-off (parity C15,
 * src/util/sys_prefs.c). Level laws are stock's (docs/re/parity2_static.md
 * 4.2/4.3): brightness 25..100 step 5, volume 0..10, auto-off 0/15/30/60 min.
 * Run: make test-sys-prefs
 */
#include <stdio.h>
#include <stdint.h>
#include "sys_prefs.h"

static int run, passed;
#define CHECK(name, cond) do { run++; if (cond) { passed++; printf("  %-66s[PASS]\n", name); } \
                               else { printf("  %-66s[FAIL]\n", name); } } while (0)

int main(void)
{
    printf("system preferences\n");

    /* brightness */
    CHECK("clamp: 0 and >100 fall back to 100", sys_bright_clamp(0) == 100 && sys_bright_clamp(101) == 100 && sys_bright_clamp(255) == 100);
    CHECK("clamp: below 25 -> 25", sys_bright_clamp(1) == 25 && sys_bright_clamp(24) == 25);
    CHECK("clamp: floors to the 5 grid", sys_bright_clamp(27) == 25 && sys_bright_clamp(79) == 75 && sys_bright_clamp(80) == 80);
    {
        int ok = 1;
        uint8_t l = 25;
        for (int i = 0; i < 40; i++) { l = sys_bright_step(l, +1); if (l < 25 || l > 100) ok = 0; }
        CHECK("step up saturates at 100, never above", ok && l == 100);
        for (int i = 0; i < 40; i++) { l = sys_bright_step(l, -1); if (l < 25 || l > 100) ok = 0; }
        CHECK("step down saturates at 25, never below", ok && l == 25);
        l = 25; int n = 0;
        while (l < 100) { l = sys_bright_step(l, +1); n++; }
        CHECK("25 -> 100 takes 15 steps of 5", n == 15);
    }
    CHECK("step: off-grid start joins the grid", sys_bright_step(78, +1) == 80 && sys_bright_step(78, -1) == 70);
    CHECK("ccr: duty = CCR/(ARR+1) = percent", sys_bright_ccr(100) == SYS_BACKLIGHT_ARR + 1 && sys_bright_ccr(50) == 50
          && sys_bright_ccr(25) == 25);
    CHECK("ccr: 100 % is a constant-high output (CCR > ARR)", sys_bright_ccr(100) > SYS_BACKLIGHT_ARR);

    /* volume */
    CHECK("volume clamp: 0..10 kept, >10 -> factory 1", sys_vol_clamp(0) == 0 && sys_vol_clamp(10) == 10 && sys_vol_clamp(11) == 1);
    CHECK("volume step saturates at 0 and 10",
          sys_vol_step(10, +1) == 10 && sys_vol_step(0, -1) == 0 && sys_vol_step(4, +1) == 5 && sys_vol_step(4, -1) == 3);
    CHECK("volume 10 duty stays within ARR 24 (40 %)", SYS_VOL_MAX < SYS_BUZZER_ARR && SYS_BEEP_SOC_DUTY <= SYS_VOL_MAX);

    /* auto-off table */
    CHECK("auto-off clamp: table kept, anything else off",
          sys_autooff_clamp(0) == 0 && sys_autooff_clamp(15) == 15 && sys_autooff_clamp(30) == 30
          && sys_autooff_clamp(60) == 60 && sys_autooff_clamp(5) == 0 && sys_autooff_clamp(255) == 0);
    CHECK("auto-off step saturates (arrows)", sys_autooff_step(60, +1, false) == 60 && sys_autooff_step(0, -1, false) == 0
          && sys_autooff_step(0, +1, false) == 15 && sys_autooff_step(30, -1, false) == 15);
    CHECK("auto-off step wraps (OK)", sys_autooff_step(60, +1, true) == 0 && sys_autooff_step(0, -1, true) == 60);
    CHECK("auto-off labels", sys_autooff_label(0)[0] == 'O' && sys_autooff_label(15)[0] == '1' && sys_autooff_label(7)[0] == 'O');

    /* live values clamp on set */
    sys_prefs_set_brightness(33); sys_prefs_set_volume(99); sys_prefs_set_autooff_mins(7);
    CHECK("live setters clamp", sys_prefs_brightness() == 30 && sys_prefs_volume() == 1 && sys_prefs_autooff_mins() == 0);

    /* beep timer */
    {
        sys_beep_t b = { 0, 0 };
        CHECK("beep idle: 0", sys_beep_poll(&b, 1000) == 0);
        sys_beep_start(&b, 1000, 3, SYS_BEEP_MS);
        CHECK("beep sounds for the whole window", sys_beep_poll(&b, 1000) == 3 && sys_beep_poll(&b, 1000 + SYS_BEEP_MS - 1) == 3);
        CHECK("beep ends at 110 ms and stays off", sys_beep_poll(&b, 1000 + SYS_BEEP_MS) == 0 && sys_beep_poll(&b, 1000 + 5) == 0);
        sys_beep_start(&b, 0xFFFFFFF0u, 5, 110);   /* wraps */
        CHECK("beep survives the 32-bit tick wrap", sys_beep_poll(&b, 0xFFFFFFF0u + 50u) == 5 && sys_beep_poll(&b, 0xFFFFFFF0u + 110u) == 0);
    }

    /* idle timer state machine */
    {
        sys_autooff_t a;
        sys_autooff_init(&a, 15, 0);
        CHECK("15 min: not before the limit", !sys_autooff_poll(&a, 15u * 60000u - 1u));
        CHECK("15 min: fires at the limit", sys_autooff_poll(&a, 15u * 60000u));
        CHECK("fires exactly once", !sys_autooff_poll(&a, 15u * 60000u + 1000u));
        sys_autooff_init(&a, 15, 0);
        sys_autooff_activity(&a, 10u * 60000u);
        CHECK("a key press restarts the count", !sys_autooff_poll(&a, 15u * 60000u) && !sys_autooff_poll(&a, 25u * 60000u - 1u)
              && sys_autooff_poll(&a, 25u * 60000u));
        sys_autooff_init(&a, 0, 0);
        CHECK("Off never fires", !sys_autooff_poll(&a, 0xFFFFFFFFu));
        sys_autooff_init(&a, 60, 0);
        CHECK("60 min limit", !sys_autooff_poll(&a, 3599999u) && sys_autooff_poll(&a, 3600000u));
        sys_autooff_init(&a, 30, 0xFFFF0000u);
        CHECK("idle timer survives the tick wrap", !sys_autooff_poll(&a, 0xFFFF0000u + 30u * 60000u - 1u)
              && sys_autooff_poll(&a, 0xFFFF0000u + 30u * 60000u));
        sys_autooff_init(&a, 15, 0);
        sys_autooff_set_minutes(&a, 30, 20u * 60000u);
        CHECK("changing the limit restarts the count", !sys_autooff_poll(&a, 20u * 60000u + 29u * 60000u)
              && sys_autooff_poll(&a, 50u * 60000u));
    }

    printf("\n%d/%d passed\n", passed, run);
    return passed == run ? 0 : 1;
}
