/* sys_prefs.c - see sys_prefs.h. Pure logic, host-tested (tests/test_sys_prefs.c). */

#include "sys_prefs.h"

static uint8_t g_brightness = SYS_BRIGHT_DEFAULT;
static uint8_t g_volume     = SYS_VOL_DEFAULT;
static uint8_t g_autooff    = 0;

static const uint8_t autooff_table[SYS_AUTOOFF_COUNT] = { 0, 15, 30, 60 };
static const char *const autooff_labels[SYS_AUTOOFF_COUNT] = {
    "Off", "15 min", "30 min", "60 min"
};

uint8_t sys_bright_clamp(uint8_t raw)
{
    if (raw == 0u || raw > SYS_BRIGHT_MAX) return SYS_BRIGHT_DEFAULT;
    if (raw < SYS_BRIGHT_MIN) return SYS_BRIGHT_MIN;
    return (uint8_t)(raw - (raw % SYS_BRIGHT_STEP));
}

uint8_t sys_bright_step(uint8_t level, int dir)
{
    uint8_t l = sys_bright_clamp(level);
    if (dir > 0 && l <= SYS_BRIGHT_MAX - SYS_BRIGHT_STEP) l = (uint8_t)(l + SYS_BRIGHT_STEP);
    if (dir < 0 && l >= SYS_BRIGHT_MIN + SYS_BRIGHT_STEP) l = (uint8_t)(l - SYS_BRIGHT_STEP);
    return l;
}

uint16_t sys_bright_ccr(uint8_t level)
{
    return sys_bright_clamp(level);   /* percent of ARR+1 = 100 */
}

uint8_t sys_vol_clamp(uint8_t raw)
{
    return (raw <= SYS_VOL_MAX) ? raw : SYS_VOL_DEFAULT;
}

uint8_t sys_vol_step(uint8_t vol, int dir)
{
    uint8_t v = sys_vol_clamp(vol);
    if (dir > 0 && v < SYS_VOL_MAX) v++;
    if (dir < 0 && v > 0) v--;
    return v;
}

static unsigned autooff_index(uint8_t mins)
{
    for (unsigned i = 0; i < SYS_AUTOOFF_COUNT; i++)
        if (autooff_table[i] == mins) return i;
    return 0;
}

uint8_t sys_autooff_clamp(uint8_t mins)
{
    return autooff_table[autooff_index(mins)];
}

uint8_t sys_autooff_step(uint8_t mins, int dir, bool wrap)
{
    int i = (int)autooff_index(mins) + (dir > 0 ? 1 : dir < 0 ? -1 : 0);
    if (i < 0) i = wrap ? (int)SYS_AUTOOFF_COUNT - 1 : 0;
    if (i >= (int)SYS_AUTOOFF_COUNT) i = wrap ? 0 : (int)SYS_AUTOOFF_COUNT - 1;
    return autooff_table[i];
}

const char *sys_autooff_label(uint8_t mins)
{
    return autooff_labels[autooff_index(mins)];
}

uint8_t sys_prefs_brightness(void)   { return g_brightness; }
uint8_t sys_prefs_volume(void)       { return g_volume; }
uint8_t sys_prefs_autooff_mins(void) { return g_autooff; }
void sys_prefs_set_brightness(uint8_t raw)  { g_brightness = sys_bright_clamp(raw); }
void sys_prefs_set_volume(uint8_t raw)      { g_volume = sys_vol_clamp(raw); }
void sys_prefs_set_autooff_mins(uint8_t raw){ g_autooff = sys_autooff_clamp(raw); }

void sys_beep_start(sys_beep_t *b, uint32_t now_ms, uint8_t duty, uint32_t dur_ms)
{
    b->end_ms = now_ms + dur_ms;
    b->duty   = duty;
}

uint8_t sys_beep_poll(sys_beep_t *b, uint32_t now_ms)
{
    if (b->duty == 0u) return 0u;
    if ((int32_t)(now_ms - b->end_ms) >= 0) {
        b->duty = 0u;
        return 0u;
    }
    return b->duty;
}

void sys_autooff_init(sys_autooff_t *a, uint8_t mins, uint32_t now_ms)
{
    a->limit_ms = (uint32_t)sys_autooff_clamp(mins) * 60000u;
    a->last_ms  = now_ms;
    a->fired    = false;
}

void sys_autooff_set_minutes(sys_autooff_t *a, uint8_t mins, uint32_t now_ms)
{
    sys_autooff_init(a, mins, now_ms);   /* a changed limit restarts the count */
}

void sys_autooff_activity(sys_autooff_t *a, uint32_t now_ms)
{
    a->last_ms = now_ms;
    a->fired   = false;
}

bool sys_autooff_poll(sys_autooff_t *a, uint32_t now_ms)
{
    if (a->limit_ms == 0u || a->fired) return false;
    if ((uint32_t)(now_ms - a->last_ms) >= a->limit_ms) {
        a->fired = true;
        return true;
    }
    return false;
}
