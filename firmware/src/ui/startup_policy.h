/*
 * OpenScope 2C53T - which mode the device boots into (parity M6).
 *
 * Pure policy, header-only so tests/test_config_persist.c can drive it. The
 * choice itself (startup_mode) lives in the settings record
 * (device_config_t.startup_mode, CONFIG_MODE_*); main.c asks this after
 * settings_store_init() has restored it.
 *
 * Restore (ours, and the built-in default): boot into the instrument that
 * was shown at the last orderly power-off (POWER hold, auto power-off). The
 * record keeps that screen in device_config_t.last_screen
 * (STARTUP_SCREEN_*); it is written at power-off only
 * (settings_store_flush_power_off), every other save carries it. Home,
 * Settings, a record from before the byte existed (0) and garbage all mean
 * "no instrument": Restore boots to Home.
 */
#ifndef STARTUP_POLICY_H
#define STARTUP_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "ui.h"

/* device_config_t.last_screen: the instrument screen at the last power-off.
 * device_mode_t + 1 for the three instruments, so that 0 (what every record
 * written before the byte had a name holds) is "none". */
#define STARTUP_SCREEN_NONE    0u   /* Home, Settings, unknown */
#define STARTUP_SCREEN_SCOPE   1u
#define STARTUP_SCREEN_METER   2u
#define STARTUP_SCREEN_SIGGEN  3u
#define STARTUP_SCREEN_MAX     STARTUP_SCREEN_SIGGEN

/* What a power-off writes for the screen shown then. */
static inline uint8_t startup_screen_of_mode(device_mode_t m)
{
    switch (m) {
    case MODE_OSCILLOSCOPE: return STARTUP_SCREEN_SCOPE;
    case MODE_MULTIMETER:   return STARTUP_SCREEN_METER;
    case MODE_SIGNAL_GEN:   return STARTUP_SCREEN_SIGGEN;
    default:                return STARTUP_SCREEN_NONE;
    }
}

/* What a load makes of the record byte: anything else is "none". */
static inline uint8_t startup_screen_clamp(uint8_t raw)
{
    return raw <= STARTUP_SCREEN_MAX ? raw : STARTUP_SCREEN_NONE;
}

/* CONFIG_MODE_* -> startup_mode_t. Stock's Startup-on-Boot menu offers None
 * (home), Multimeter, Oscilloscope and Signal Generator (0x0804C208); boot
 * copies the choice straight into ms[0xF68] (0x0802DF50). Restore is ours.
 * Anything else (the settings page, garbage that passed the checksum) is the
 * meter, stock's factory value and what such a byte has always decoded to
 * (the built-in default is Restore since 2026-11-14; that did not move how
 * a stored value reads). */
static inline startup_mode_t startup_mode_from_config(uint8_t config_mode)
{
    switch (config_mode) {
    case CONFIG_MODE_OSCILLOSCOPE: return STARTUP_SCOPE;
    case CONFIG_MODE_SIGNAL_GEN:   return STARTUP_SIGGEN;
    case CONFIG_MODE_HOME:         return STARTUP_HOME;
    case CONFIG_MODE_RESTORE:      return STARTUP_RESTORE;
    default:                       return STARTUP_METER;
    }
}

static inline uint8_t startup_mode_to_config(startup_mode_t m)
{
    switch (m) {
    case STARTUP_SCOPE:   return CONFIG_MODE_OSCILLOSCOPE;
    case STARTUP_SIGGEN:  return CONFIG_MODE_SIGNAL_GEN;
    case STARTUP_HOME:    return CONFIG_MODE_HOME;
    case STARTUP_RESTORE: return CONFIG_MODE_RESTORE;
    default:              return CONFIG_MODE_MULTIMETER;
    }
}

/* The mode to boot into: every choice is honoured (home and the generator do
 * not touch the FPGA or the scope front end; bench 2026-11-14: the scope front
 * end and the meter link are independent).
 *
 * last_screen (STARTUP_SCREEN_*, the loaded record's) only matters for
 * Restore: the instrument it names, or Home when it names none. Before the
 * record is loaded it is STARTUP_SCREEN_NONE. */
static inline device_mode_t startup_boot_mode(startup_mode_t m, uint8_t last_screen)
{
    switch (m) {
    case STARTUP_METER:  return MODE_MULTIMETER;
    case STARTUP_HOME:   return MODE_HOME;
    case STARTUP_SIGGEN: return MODE_SIGNAL_GEN;
    case STARTUP_RESTORE:
        switch (last_screen) {
        case STARTUP_SCREEN_SCOPE:  return MODE_OSCILLOSCOPE;
        case STARTUP_SCREEN_METER:  return MODE_MULTIMETER;
        case STARTUP_SCREEN_SIGGEN: return MODE_SIGNAL_GEN;
        default:                    return MODE_HOME;
        }
    default:             return MODE_OSCILLOSCOPE;
    }
}

/* The order the Settings row steps through: stock's (0x0804C208), then
 * Restore. */
static inline startup_mode_t startup_mode_step(startup_mode_t m, int dir)
{
    static const startup_mode_t order[STARTUP_COUNT] = {
        STARTUP_HOME, STARTUP_METER, STARTUP_SCOPE, STARTUP_SIGGEN, STARTUP_RESTORE
    };
    int i = 0;
    while (i < (int)STARTUP_COUNT && order[i] != m) i++;
    if (i >= (int)STARTUP_COUNT) i = (int)STARTUP_COUNT - 1;   /* unknown: from the default (Restore) */
    i = (i + (dir < 0 ? (int)STARTUP_COUNT - 1 : 1)) % (int)STARTUP_COUNT;
    return order[i];
}

#endif /* STARTUP_POLICY_H */
