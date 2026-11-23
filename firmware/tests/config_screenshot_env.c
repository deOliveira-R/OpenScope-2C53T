/*
 * What test_config_screenshot links against in place of main.c and the drivers.
 * config.c saves through the property table, so the model links in, and the
 * model reads this state. None of it is under test here.
 */
#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "flash_regions.h"
#include "scope_state.h"
#include "startup_policy.h"
#include "signal_gen.h"
#include "dac_output.h"
#include "ui.h"

volatile bool           math_enabled    = false;
volatile uint8_t        math_op         = 0;
volatile uint8_t        meter_submode   = 0;
volatile uint8_t        meter_layout    = 0;
volatile startup_mode_t startup_mode    = STARTUP_RESTORE;
volatile device_mode_t  current_mode    = MODE_MULTIMETER;
volatile uint8_t        active_channel  = 0;
volatile bool           persist_enabled = false;
volatile scope_view_t   scope_view      = SCOPE_VIEW_TIME;
volatile uint8_t        fuse_type       = 0;
volatile uint8_t        fuse_rating_idx = 0;
volatile uint8_t        fuse_view       = 0;
volatile float          fuse_scan_threshold_mv = 0.0f;

uint8_t key_focus_get(void) { return 0; }

static uint16_t dac_buf[SIGGEN_CYCLE_SAMPLES];
void dac_output_apply(const siggen_timing_t *t) { (void)t; }
void dac_output_stop(void) {}
uint16_t *dac_output_get_buffer(void) { return dac_buf; }
uint32_t dac_output_timer_clock_hz(void) { return 0u; }

/* No chip: the test asserts that a save without storage is refused. */
flash_region_status_t flash_regions_bind_w25q(void) { return FLASH_REGION_ERR_NOT_INIT; }
