/*
 * OpenScope 2C53T - which mode the device boots into (parity M6).
 *
 * Pure policy, header-only so tests/test_config_persist.c can drive it. The
 * choice itself (startup_mode) lives in the settings record
 * (device_config_t.startup_mode, CONFIG_MODE_*); main.c asks this after
 * settings_store_init() has restored it.
 */
#ifndef STARTUP_POLICY_H
#define STARTUP_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "ui.h"

/* CONFIG_MODE_* -> startup_mode_t. Stock's Startup-on-Boot menu offers None
 * (home), Multimeter, Oscilloscope and Signal Generator (0x0804C208); boot
 * copies the choice straight into ms[0xF68] (0x0802DF50). Anything else (the
 * settings page, garbage that passed the checksum) is the factory default,
 * the meter. */
static inline startup_mode_t startup_mode_from_config(uint8_t config_mode)
{
    switch (config_mode) {
    case CONFIG_MODE_OSCILLOSCOPE: return STARTUP_SCOPE;
    case CONFIG_MODE_SIGNAL_GEN:   return STARTUP_SIGGEN;
    case CONFIG_MODE_HOME:         return STARTUP_HOME;
    default:                       return STARTUP_METER;
    }
}

static inline uint8_t startup_mode_to_config(startup_mode_t m)
{
    switch (m) {
    case STARTUP_SCOPE:  return CONFIG_MODE_OSCILLOSCOPE;
    case STARTUP_SIGGEN: return CONFIG_MODE_SIGNAL_GEN;
    case STARTUP_HOME:   return CONFIG_MODE_HOME;
    default:             return CONFIG_MODE_MULTIMETER;
    }
}

/* The mode to boot into. A warm-handoff build (FPGA_WARM_HANDOFF_TEST) is a
 * scope-only image: its meter entry points are no-ops (fpga.c), so it always
 * boots into the scope. FPGA_METER_SUBMODES lifts exactly that half, and a
 * meter that works can be booted into (the other choices stay scope there:
 * that build exists to keep the scope posture). */
static inline device_mode_t startup_boot_mode(startup_mode_t m, bool warm_handoff,
                                              bool meter_submodes)
{
    if (warm_handoff) {
        return (meter_submodes && m == STARTUP_METER) ? MODE_MULTIMETER
                                                      : MODE_OSCILLOSCOPE;
    }
    switch (m) {
    case STARTUP_METER:  return MODE_MULTIMETER;
    case STARTUP_HOME:   return MODE_HOME;
    case STARTUP_SIGGEN: return MODE_SIGNAL_GEN;
    default:             return MODE_OSCILLOSCOPE;
    }
}

/* The order the Settings row steps through: stock's (0x0804C208). */
static inline startup_mode_t startup_mode_step(startup_mode_t m, int dir)
{
    static const startup_mode_t order[STARTUP_COUNT] = {
        STARTUP_HOME, STARTUP_METER, STARTUP_SCOPE, STARTUP_SIGGEN
    };
    int i = 0;
    while (i < (int)STARTUP_COUNT && order[i] != m) i++;
    if (i >= (int)STARTUP_COUNT) i = 1;          /* unknown: from the default */
    i = (i + (dir < 0 ? (int)STARTUP_COUNT - 1 : 1)) % (int)STARTUP_COUNT;
    return order[i];
}

#endif /* STARTUP_POLICY_H */
