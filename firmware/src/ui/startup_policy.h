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

/* CONFIG_MODE_* -> startup_mode_t. The Startup-on-Boot menu offers scope and
 * meter only: anything else (a record from a build that stored the signal
 * generator or settings, or garbage that passed the checksum) is the
 * factory default, the meter. */
static inline startup_mode_t startup_mode_from_config(uint8_t config_mode)
{
    return (config_mode == CONFIG_MODE_OSCILLOSCOPE) ? STARTUP_SCOPE
                                                     : STARTUP_METER;
}

static inline uint8_t startup_mode_to_config(startup_mode_t m)
{
    return (m == STARTUP_SCOPE) ? CONFIG_MODE_OSCILLOSCOPE
                                : CONFIG_MODE_MULTIMETER;
}

/* The mode to boot into. A warm-handoff build (FPGA_WARM_HANDOFF_TEST) is a
 * scope-only image: its meter entry points are no-ops (fpga.c), so it always
 * boots into the scope. FPGA_METER_SUBMODES lifts exactly that half, and a
 * meter that works can be booted into. */
static inline device_mode_t startup_boot_mode(startup_mode_t m, bool warm_handoff,
                                              bool meter_submodes)
{
    if (warm_handoff && !meter_submodes) return MODE_OSCILLOSCOPE;
    return (m == STARTUP_METER) ? MODE_MULTIMETER : MODE_OSCILLOSCOPE;
}

#endif /* STARTUP_POLICY_H */
