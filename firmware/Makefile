# Makefile for FNIRSI 2C53T Custom Firmware
# Target: Artery AT32F403A (ARM Cortex-M4F @ 240MHz)

######################################################################
# Toolchain
######################################################################
PREFIX  = arm-none-eabi-
CC      = $(PREFIX)gcc
AS      = $(PREFIX)gcc -x assembler-with-cpp
CP      = $(PREFIX)objcopy
SZ      = $(PREFIX)size
OBJDUMP = $(PREFIX)objdump
NANO_SPECS := $(shell f="$(shell $(CC) -print-file-name=nano.specs 2>/dev/null)"; test "$$f" != "nano.specs" -a -f "$$f" && printf '%s' '-specs=nano.specs')
HOST_CC ?= gcc

######################################################################
# Project paths
######################################################################
BUILD_DIR  = build
AT32_LIB   = at32f403a_lib/libraries
AT32_DRV   = $(AT32_LIB)/drivers
AT32_CMSIS = $(AT32_LIB)/cmsis/cm4
FREERTOS   = FreeRTOS

######################################################################
# Source files
######################################################################

# Features: firmware/features.mk is the manifest (included below, where the
# feature sources join C_SOURCES). `make features` lists it.

# Application
C_SOURCES  = src/main.c
C_SOURCES += src/drivers/lcd.c
C_SOURCES += src/ui/status_bar.c
C_SOURCES += src/ui/scope_ui.c
C_SOURCES += src/ui/scope_popup.c
C_SOURCES += src/ui/scope_furniture.c
C_SOURCES += src/ui/scope_view.c
C_SOURCES += src/ui/scope_yt.c
C_SOURCES += src/ui/scope_yt_frame.c
C_SOURCES += src/ui/scope_cursor_draw.c
C_SOURCES += src/ui/canvas.c
C_SOURCES += src/ui/plot_elements.c
C_SOURCES += src/ui/scope_xy.c
C_SOURCES += src/ui/scope_roll_draw.c
C_SOURCES += src/ui/scope_spectrum.c
C_SOURCES += src/ui/scope_waterfall.c
C_SOURCES += src/ui/waterfall_blit.c
C_SOURCES += src/ui/meter_ui.c
C_SOURCES += src/ui/softkey.c
C_SOURCES += src/ui/softkey_draw.c
C_SOURCES += src/ui/meter_screen.c
C_SOURCES += src/ui/fuse_model.c
C_SOURCES += src/ui/siggen_ui.c
C_SOURCES += src/ui/siggen_screen.c
C_SOURCES += src/ui/siggen_edit.c
C_SOURCES += src/ui/settings_ui.c
C_SOURCES += src/ui/settings_screen.c
C_SOURCES += src/ui/home_model.c
C_SOURCES += src/ui/home_ui.c
C_SOURCES += src/ui/home_screen.c
C_SOURCES += src/ui/fuse_ui.c
C_SOURCES += src/ui/scope_state.c
C_SOURCES += src/ui/scope_measure.c
C_SOURCES += src/ui/scope_cal.c
C_SOURCES += src/ui/scope_timebase.c
C_SOURCES += src/ui/scope_cursor.c
C_SOURCES += src/ui/scope_freq.c
C_SOURCES += src/ui/scope_freq_lock.c
C_SOURCES += src/ui/key_model.c
C_SOURCES += src/ui/screen.c
C_SOURCES += src/ui/ui_router.c
C_SOURCES += src/ui/scope_selfcal.c
C_SOURCES += src/tasks/input_handler.c
C_SOURCES += src/tasks/scope_apply.c
C_SOURCES += src/drivers/flash_fs.c
C_SOURCES += src/drivers/flash_regions.c
C_SOURCES += src/drivers/flash_regions_w25q.c
C_SOURCES += src/drivers/watchdog.c
C_SOURCES += src/drivers/wdt_clock.c
C_SOURCES += src/util/health_gap.c
C_SOURCES += src/util/crash_policy.c
C_SOURCES += src/drivers/button_scan.c
C_SOURCES += src/drivers/continuity_buzzer.c
C_SOURCES += src/drivers/dfu_boot.c
C_SOURCES += src/drivers/battery.c
C_SOURCES += src/drivers/battery_gauge.c
C_SOURCES += src/drivers/fpga.c
C_SOURCES += src/drivers/fpga_boot.c
C_SOURCES += src/drivers/fpga_acq.c
C_SOURCES += src/drivers/scope_frontend.c
C_SOURCES += src/drivers/meter_link.c
C_SOURCES += src/drivers/fpga_bitstream_core.c
C_SOURCES += src/drivers/fpga_meter_plan.c
C_SOURCES += src/drivers/dac.c
C_SOURCES += src/drivers/dac_output.c
C_SOURCES += src/drivers/scope_trigger.c
C_SOURCES += src/drivers/scope_vpos.c
C_SOURCES += src/drivers/scope_selfcal_run.c
C_SOURCES += src/dsp/trig_edge.c
C_SOURCES += src/dsp/trig_policy.c
C_SOURCES += src/dsp/acq_cfg.c
C_SOURCES += src/dsp/interleave.c
C_SOURCES += src/dsp/interleave_cal.c
C_SOURCES += src/drivers/meter_data.c
C_SOURCES += src/drivers/meter_stock.c
C_SOURCES += src/drivers/meter_auto.c
C_SOURCES += src/drivers/meter_autoselect.c
C_SOURCES += src/drivers/usb_cdc.c
C_SOURCES += src/drivers/shell.c
C_SOURCES += src/drivers/shell_system.c
C_SOURCES += src/drivers/shell_scope.c
C_SOURCES += src/drivers/shell_cal.c
C_SOURCES += src/drivers/shell_screen.c
C_SOURCES += src/drivers/fw_loader.c
C_SOURCES += src/drivers/rtt.c
C_SOURCES += src/drivers/fault.c
C_SOURCES += src/dsp/signal_gen.c
C_SOURCES += src/dsp/siggen_wave.c
C_SOURCES += src/dsp/shared_mem.c
C_SOURCES += src/ui/persistence.c
C_SOURCES += src/ui/theme.c
C_SOURCES += src/ui/scope_roll.c
C_SOURCES += src/ui/scope_roll_meas.c
C_SOURCES += src/ui/scope_geom.c
C_SOURCES += src/dsp/sinc_interp.c
C_SOURCES += src/dsp/math_channel.c
C_SOURCES += src/dsp/paused_view.c
# src/dsp/bode.c (host test: make test-bode) is not linked: its only caller was
# a demo screen on synthetic data (the Settings entry "Bode Plot"), removed.
C_SOURCES += src/dsp/mask_pf.c
C_SOURCES += src/ui/scope_mask.c
C_SOURCES += src/util/config.c
C_SOURCES += src/util/cal_dump.c
C_SOURCES += src/util/cal_backup_core.c
C_SOURCES += src/util/calpage.c
C_SOURCES += src/util/cal_backup.c
C_SOURCES += src/util/cal_reference.c
C_SOURCES += src/util/settings_store.c src/util/session_store.c
C_SOURCES += src/util/shell_persist.c
C_SOURCES += src/util/lcd_health.c
C_SOURCES += src/util/usercal_rec.c
C_SOURCES += src/util/usercal.c
C_SOURCES += src/util/sys_prefs.c
C_SOURCES += src/drivers/sys_pwm.c
C_SOURCES += src/util/gallery_store.c
C_SOURCES += src/ui/gallery_nav.c
C_SOURCES += src/ui/gallery_ui.c
C_SOURCES += src/ui/gallery_screen.c
C_SOURCES += src/util/logo_store.c
C_SOURCES += src/util/fpga_slot.c
C_SOURCES += src/ui/logo_ui.c
C_SOURCES += src/ui/overlay_screens.c
C_SOURCES += src/ui/overlay_ui.c
# UI round 1, R1-C: measurement strip + plot-edge markers
C_SOURCES += src/ui/measure_strip.c
C_SOURCES += src/ui/plot_markers.c
C_SOURCES += src/decode/decode_uart.c
C_SOURCES += src/decode/decode_i2c.c
C_SOURCES += src/decode/decode_spi.c
C_SOURCES += src/decode/decode_can.c
C_SOURCES += src/decode/decode_kline.c
C_SOURCES += src/decode/can_bus.c
C_SOURCES += src/drivers/can_scope.c
C_SOURCES += src/fonts/font.c
C_SOURCES += src/fonts/font_small.c
C_SOURCES += src/fonts/font_medium.c
C_SOURCES += src/fonts/font_large.c
C_SOURCES += src/fonts/font_xlarge.c
C_SOURCES += src/fonts/font_huge.c
C_SOURCES += src/fonts/font_unit.c
# UI round 1, R1-B: bar glyphs
C_SOURCES += src/ui/icons.c

# ── UI round 1, R1-A: Oscilloscope Settings page (one block, for the merge) ──
# osc_menu_model.c (pure model), osc_menu_ui.c (apply + draw), menu_frame.c (the
# two-pane widget System Settings reuses), meas_select.c (per-channel
# measurement selection, persisted by settings_store.c). Host test:
# make test-osc-menu. The persistence test links meas_select.c too.
C_SOURCES += src/ui/osc_menu_model.c
C_SOURCES += src/ui/osc_menu_ui.c
C_SOURCES += src/ui/osc_menu_screen.c
C_SOURCES += src/ui/scope_screen.c
C_SOURCES += src/ui/menu_frame.c
C_SOURCES += src/ui/meas_select.c
R1A_OSC_MENU_TEST_SRCS = src/ui/osc_menu_model.c src/ui/meas_select.c src/ui/scope_state.c \
                         src/ui/scope_roll.c src/ui/scope_measure.c src/ui/scope_cursor.c \
                         src/ui/scope_timebase.c src/ui/scope_cal.c \
                         src/fonts/font_small.c src/fonts/font_medium.c
R1A_CONFIG_PERSIST_EXTRA = src/ui/meas_select.c
# ── end R1-A block ──

# ── ARCH-1 M1: the property model, live (one block, for the merge) ──
# The registry core (M0) plus the executor (model_exec.c: the queue the input
# task drains), the real hardware ops (model_hw_fw.c) and the shell's
# get/set/list/describe (model_shell.c). Host test: make test-model-exec.
C_SOURCES += src/model/model.c src/model/props_scope.c src/model/props_gen.c
C_SOURCES += src/model/props_meter.c src/model/props_sys.c src/model/model_session.c
C_SOURCES += src/model/session_shell.c
C_SOURCES += src/model/model_exec.c src/model/model_hw_fw.c src/model/model_shell.c
# The meter link policy (pure; host test: make test-meter-link-policy).
C_SOURCES += src/model/meter_link_policy.c
C_INCLUDES_M1 = -Isrc/model
# ── end M1 block ──

# ── ARCH-1 M3: settings persistence from the property table (one block) ──
# The v5 codec table (src/model/model_persist_v5.c): settings_store_capture /
# settings_store_apply and config_init_defaults are loops over it, so config.c
# and settings_store.c now link the model. Host tests: make test-model-persist
# (byte identity against a frozen copy of the hand-written code, over the
# defaults, random live states and random records) and test-config-persist.
C_SOURCES += src/model/model_persist_v5.c
M3_PERSIST_SRCS  = src/model/model_persist_v5.c src/model/model.c src/model/props_scope.c \
                   src/model/props_gen.c src/model/props_meter.c src/model/props_sys.c src/model/model_session.c
M3_PERSIST_LINK  = src/ui/scope_measure.c src/ui/scope_cursor.c src/ui/key_model.c \
                   src/ui/osc_menu_model.c
M3_CONFIG_PERSIST_EXTRA = $(M3_PERSIST_SRCS) $(M3_PERSIST_LINK)
M3_CONFIG_PERSIST_FLAGS = -std=gnu11 -Isrc/model
# ── end M3 block ──

# ── ARCH-1 M2: the UI route (one block, for the merge) ──
# `ui press/long/seq/screen` (src/model/ui_shell.c):
# exact key replay into the key queue, waited for on input_events_done.
# Host test: make test-ui-shell (its rule is next to test-model-exec, below
# `all`, so it never becomes the default goal).
C_SOURCES += src/model/ui_shell.c
# ── end M2 block ──

# ARCH-1 B8: a call made on the task that owns the state (util/task_call.h).
C_SOURCES += src/util/task_call.c

# ── Spectrum views: FFT, waterfall, split (core, every image) ──
# The FFT engine and the live spectrum (src/dsp/fft*.c); the views
# themselves are src/ui/scope_spectrum.c and scope_waterfall.c above. Until
# 2026-11-24 this was the feature FFT (features.mk, bit 1, now retired).
# CMSIS-DSP is used when the library is present (auto-detected).
C_SOURCES += src/dsp/fft.c src/dsp/fft_live.c src/dsp/fft_source.c src/dsp/fft_test_signals.c
CMSIS_DSP = CMSIS-DSP
ifneq ($(wildcard $(CMSIS_DSP)/Include/arm_math.h),)
  C_SOURCES += $(CMSIS_DSP)/Source/TransformFunctions/arm_rfft_fast_f32.c
  C_SOURCES += $(CMSIS_DSP)/Source/TransformFunctions/arm_rfft_fast_init_f32.c
  C_SOURCES += $(CMSIS_DSP)/Source/TransformFunctions/arm_cfft_f32.c
  C_SOURCES += $(CMSIS_DSP)/Source/TransformFunctions/arm_cfft_init_f32.c
  C_SOURCES += $(CMSIS_DSP)/Source/CommonTables/arm_common_tables.c
  C_SOURCES += $(CMSIS_DSP)/Source/CommonTables/arm_const_structs.c
  C_SOURCES += $(CMSIS_DSP)/Source/TransformFunctions/arm_cfft_radix8_f32.c
  C_SOURCES += $(CMSIS_DSP)/Source/ComplexMathFunctions/arm_cmplx_mag_f32.c
  C_SOURCES += $(CMSIS_DSP)/Source/BasicMathFunctions/arm_scale_f32.c
  C_INCLUDES_FFT = -I$(CMSIS_DSP)/Include
  # Only the tables the one transform size needs. FFT_SIZE is a compile-time
  # 4096 (src/dsp/fft.h), but arm_rfft_fast_init_f32() picks its tables with
  # a run-time switch over 32..4096, so without this every size's tables are
  # linked (about 80 KB that no code path reaches). With ARM_DSP_CONFIG_TABLES
  # CMSIS builds only what is named:
  #   ARM_FFT_ALLOW_TABLES                  opens the table blocks at all
  #                                         (arm_common_tables.c, arm_const_structs.c)
  #   ARM_TABLE_TWIDDLECOEF_F32_2048   \    the 2048-point complex FFT inside a
  #   ARM_TABLE_BITREVIDX_FLT_2048     /    4096-point real one (arm_cfft_init_f32.c)
  #   ARM_TABLE_TWIDDLECOEF_RFFT_F32_4096   the real FFT's own twiddles
  # which is exactly the guard of the 4096 case in arm_rfft_fast_init_f32.c.
  # Any other length now makes the init return ARM_MATH_ARGUMENT_ERROR, which
  # fft_init() checks (fft_engine_ok). Host test on these tables:
  # make test-fft-cmsis-tables.
  FFT_TABLE_DEFS = -DARM_DSP_CONFIG_TABLES -DARM_FFT_ALLOW_TABLES \
                   -DARM_TABLE_TWIDDLECOEF_F32_2048 -DARM_TABLE_BITREVIDX_FLT_2048 \
                   -DARM_TABLE_TWIDDLECOEF_RFFT_F32_4096
  C_DEFS_FFT = -DUSE_CMSIS_DSP -DARM_MATH_CM4 $(FFT_TABLE_DEFS)
endif
# ── end spectrum block ──

# ── Features (features.mk: the manifest) ──
# Which features this image has is decided here, once, while the Makefile is
# read: every FEATURE_<NAME> becomes -DFEATURE_<NAME>=0 or =1 (FEATURE_DEFS),
# and only the sources of the features that are on are compiled. Stream:
# src/stream/ (docs/stream_protocol.md), on in the bench image
# (app-coldtrace-meter), host test `make test-stream`. Old spellings still
# accepted: STREAM_BENCH=0, USB_MSC=0 (see features.mk).
include features.mk
C_SOURCES += $(FEATURE_SRCS)
# The feature table of this build (src/util/features.h: `version`, `features`,
# the stream HELLO mask). features.c alone gets the manifest as one X-list:
# every known feature, on or off.
C_SOURCES += src/util/features.c
FEATURE_XLIST = $(foreach f,$(FEATURE_LIST),X($($(f)_name),$(strip $($(f)_bit)),$(FEATURE_$(f)),"$(strip $($(f)_desc))"))
$(BUILD_DIR)/features.o: CFLAGS += '-DFEATURE_XLIST=$(FEATURE_XLIST)'
# ── end features block ──

# AT32 CMSIS + clock config
C_SOURCES += $(AT32_CMSIS)/device_support/system_at32f403a_407.c
C_SOURCES += src/at32f403a_407_clock.c

# AT32 Peripheral Library (only what we need)
C_SOURCES += $(AT32_DRV)/src/at32f403a_407_crm.c
C_SOURCES += $(AT32_DRV)/src/at32f403a_407_gpio.c
C_SOURCES += $(AT32_DRV)/src/at32f403a_407_flash.c
C_SOURCES += $(AT32_DRV)/src/at32f403a_407_misc.c
C_SOURCES += $(AT32_DRV)/src/at32f403a_407_exint.c
C_SOURCES += $(AT32_DRV)/src/at32f403a_407_xmc.c
C_SOURCES += $(AT32_DRV)/src/at32f403a_407_wdt.c
C_SOURCES += $(AT32_DRV)/src/at32f403a_407_debug.c
C_SOURCES += $(AT32_DRV)/src/at32f403a_407_adc.c
C_SOURCES += $(AT32_DRV)/src/at32f403a_407_dac.c
C_SOURCES += $(AT32_DRV)/src/at32f403a_407_tmr.c
C_SOURCES += $(AT32_DRV)/src/at32f403a_407_usb.c
C_SOURCES += $(AT32_DRV)/src/at32f403a_407_acc.c

# USB Device Middleware (CDC virtual COM port)
AT32_USBD = at32f403a_lib/middlewares/usbd_drivers
AT32_CDC  = at32f403a_lib/middlewares/usbd_class/cdc
C_SOURCES += $(AT32_USBD)/src/usbd_core.c
C_SOURCES += $(AT32_USBD)/src/usbd_int.c
C_SOURCES += $(AT32_USBD)/src/usbd_sdr.c
C_SOURCES += $(AT32_CDC)/cdc_class.c
C_SOURCES += $(AT32_CDC)/cdc_desc.c

# USB Sharing (the composite CDC + mass-storage device): feature USBDRIVE in
# features.mk.
# Raw block receive for `logo blk` (both variants).
C_SOURCES += src/util/raw_block.c

# FreeRTOS kernel
C_SOURCES += $(FREERTOS)/tasks.c
C_SOURCES += $(FREERTOS)/queue.c
C_SOURCES += $(FREERTOS)/list.c
C_SOURCES += $(FREERTOS)/timers.c
C_SOURCES += $(FREERTOS)/portable/GCC/ARM_CM4F/port.c
C_SOURCES += $(FREERTOS)/portable/MemMang/heap_4.c

# Startup assembly (AT32F403A - GCC variant)
ASM_SOURCES = $(AT32_CMSIS)/device_support/startup/gcc/startup_at32f403a_407.s

# CMSIS-DSP assembly (bit-reversal)
ifneq ($(wildcard CMSIS-DSP/Include/arm_math.h),)
  ASM_SOURCES += CMSIS-DSP/Source/TransformFunctions/arm_bitreversal2.S
endif

######################################################################
# Include paths
######################################################################
C_INCLUDES  = -Iinclude
C_INCLUDES += -Isrc/drivers
C_INCLUDES += -Isrc/dsp
C_INCLUDES += -Isrc/ui
C_INCLUDES += -Isrc/tasks
C_INCLUDES += -Isrc/util
C_INCLUDES += -Isrc/decode
C_INCLUDES += -Isrc/fonts
C_INCLUDES += -Isrc
C_INCLUDES += $(C_INCLUDES_M1)
C_INCLUDES += $(FEATURE_INCS)
C_INCLUDES += $(C_INCLUDES_FFT)
C_INCLUDES += -I$(AT32_DRV)/inc
C_INCLUDES += -I$(AT32_CMSIS)/device_support
C_INCLUDES += -I$(AT32_CMSIS)/core_support
C_INCLUDES += -I$(FREERTOS)/include
C_INCLUDES += -I$(FREERTOS)/portable/GCC/ARM_CM4F
C_INCLUDES += -I$(AT32_USBD)/inc
C_INCLUDES += -I$(AT32_CDC)

######################################################################
# Compiler flags
######################################################################

# MCU flags: Cortex-M4 with hardware FPU
CPU = -mcpu=cortex-m4
FPU = -mfpu=fpv4-sp-d16
FLOAT-ABI = -mfloat-abi=hard
MCU = $(CPU) -mthumb $(FPU) $(FLOAT-ABI)

# C defines
C_DEFS  = -DAT32F403AVGT7
C_DEFS += $(FEATURE_DEFS)
C_DEFS += $(C_DEFS_FFT)

# Compiler flags
CFLAGS  = $(MCU) $(C_DEFS) $(C_INCLUDES)
CFLAGS += -Wall -Wextra -Wno-unused-parameter -Wundef
CFLAGS += -fdata-sections -ffunction-sections
CFLAGS += -std=gnu11
CFLAGS += -Os -g
# Header dependencies: the compiler writes build/<name>.d beside each object
# (-MMD; -MP adds a phony target per header so a deleted header does not stop
# the build), included at the end of this file. Without them a change made
# only in a header left every object that includes it as it was.
CFLAGS += -MMD -MP

# Assembler flags
ASFLAGS = $(MCU) $(C_DEFS) $(C_INCLUDES)

######################################################################
# Linker flags
######################################################################
LDSCRIPT = ld/at32f403a_app.ld

LDFLAGS  = $(MCU)
LDFLAGS += $(NANO_SPECS)
LDFLAGS += -T$(LDSCRIPT)
LDFLAGS += -Wl,--gc-sections
LDFLAGS += -Wl,-Map=$(BUILD_DIR)/firmware.map,--cref
LDFLAGS += -lc -lm -lnosys

######################################################################
# Build targets
######################################################################
TARGET = firmware

# Plain `make` builds the image the unit runs every day: what
# app-coldtrace-meter builds (profile bench from features.mk, watchdog off),
# without the `clean` that target does first. `make flash` sends it.
#
# WATCHDOG: 1 = main.c starts the hardware watchdog after the tasks are created
# (tested on the unit: crashes, a hang, updates, normal use, the bench commands).
# 0 = never started: the dry run, whose `health` counters show what would have
# tripped it without switching the unit off. Use it to study a new long command.
# The emulator image keeps its (empty) watchdog call: it cannot be run here
# to confirm a change.
WATCHDOG ?= 1
ifneq ($(WATCHDOG),1)
ifeq ($(filter emu,$(MAKECMDGOALS)),)
CFLAGS += -DNO_WATCHDOG
endif
endif

all: $(BUILD_DIR)/$(TARGET).bin

# List of C objects
OBJECTS = $(addprefix $(BUILD_DIR)/,$(notdir $(C_SOURCES:.c=.o)))
vpath %.c $(sort $(dir $(C_SOURCES)))

# List of ASM objects (AT32 uses .s, CMSIS-DSP uses .S)
OBJECTS += $(addprefix $(BUILD_DIR)/,$(notdir $(ASM_SOURCES:.s=.o)))
OBJECTS := $(OBJECTS:.S=.o)
vpath %.s $(sort $(dir $(ASM_SOURCES)))
vpath %.S $(sort $(dir $(ASM_SOURCES)))

# Objects of different images share build/: without this, plain `make` after
# `make app-rtt`, or a change of WITH=/WITHOUT=, links objects compiled
# for the other image. build/image.stamp holds what the objects were compiled
# as (IMAGE_ID, features.mk, plus WATCHDOG); if it differs, or is missing,
# the objects are deleted while the Makefile is read, before any rule runs (a
# timestamp prerequisite cannot do this: make's 1 s mtime resolution on macOS
# misses a switch made within a second of the last compile). The link writes
# the stamp again, because a named image target runs `clean` after this.
# Goals that build no image (tests, checks, the feature tools) never touch it.
IMAGE_STAMP = $(BUILD_DIR)/image.stamp
IMAGE_STAMP_TEXT = $(IMAGE_ID) / watchdog $(WATCHDOG)
NO_IMAGE_GOALS = clean test-% check-% features features-% bootloader flash-bootloader print-%
ifeq ($(filter clean,$(MAKECMDGOALS)),)
ifneq ($(if $(MAKECMDGOALS),$(filter-out $(NO_IMAGE_GOALS),$(MAKECMDGOALS)),all),)
ifneq ($(strip $(shell cat $(IMAGE_STAMP) 2>/dev/null)),$(strip $(IMAGE_STAMP_TEXT)))
$(shell rm -f $(BUILD_DIR)/*.o; mkdir -p $(BUILD_DIR); echo '$(IMAGE_STAMP_TEXT)' > $(IMAGE_STAMP))
endif
endif
endif

# ── Build-time defaults profile (ARCH-1 M5) ─────────────────────────────
# make DEFAULTS=../profiles/<name>.json   (from firmware/; any image target takes it)
# The BUILT-IN DEFAULTS of the settings are the property table's `def` column
# (src/model/model_table.h). A profile (a readable file of settings, written
# by hand or by `scripts/model_config.py from-device`) may replace them for
# one image: scripts/model_config.py checks it against the table and writes
# build/model_profile.h, which model.c (the defaults) and usb_debug.c (the
# `version` line "Defaults profile: <name>") are compiled with. Nothing else
# sees it, and without DEFAULTS neither file does: the image is the same as
# before this hook existed. A saved settings record still wins over the
# defaults; "Reset to defaults" goes back to the profile's.
# This is NOT the feature profile (PROFILE=bench, features.mk): that one
# chooses what code is in the image, this one the default VALUES.
# The stamp makes the two objects rebuild when DEFAULTS changes or goes away.
DEFAULTS ?=
MODEL_PROFILE_OBJS := $(BUILD_DIR)/model.o $(BUILD_DIR)/shell.o
_defaults_now := $(strip $(DEFAULTS))
_defaults_changed := $(shell [ -d $(BUILD_DIR) ] && [ "$$(cat $(BUILD_DIR)/defaults.stamp 2>/dev/null)" != "$(_defaults_now)" ] && echo "$(_defaults_now)" > $(BUILD_DIR)/defaults.stamp && echo changed)
$(BUILD_DIR)/defaults.stamp: | $(BUILD_DIR)
	@echo "$(_defaults_now)" > $@
$(MODEL_PROFILE_OBJS): $(BUILD_DIR)/defaults.stamp
ifneq ($(_defaults_changed),)
# DEFAULTS is not what the objects in build/ were made with: both are made
# again in this run, whatever their timestamps say (two builds within one
# second have equal ones).
.PHONY: defaults-changed
$(MODEL_PROFILE_OBJS): defaults-changed
endif
ifneq ($(_defaults_now),)
MODEL_PROFILE_H := $(BUILD_DIR)/model_profile.h
$(MODEL_PROFILE_H): $(DEFAULTS) ../scripts/model_config.py src/model/model_table.h $(BUILD_DIR)/defaults.stamp | $(BUILD_DIR)
	python3 -I ../scripts/model_config.py build $(DEFAULTS) -o $@
$(MODEL_PROFILE_OBJS): $(MODEL_PROFILE_H)
$(MODEL_PROFILE_OBJS): CFLAGS += -include $(MODEL_PROFILE_H)
endif

$(BUILD_DIR)/%.o: %.c Makefile features.mk | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

-include $(wildcard $(BUILD_DIR)/*.d)

$(BUILD_DIR)/%.o: %.S Makefile features.mk | $(BUILD_DIR)
	$(AS) -c $(ASFLAGS) $< -o $@

$(BUILD_DIR)/%.o: %.s Makefile features.mk | $(BUILD_DIR)
	$(AS) -c $(ASFLAGS) $< -o $@

$(BUILD_DIR)/$(TARGET).elf: $(OBJECTS) Makefile features.mk $(LDSCRIPT)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	$(SZ) $@
	@{ printf 'profile\t%s\n' '$(PROFILE)'; $(foreach f,$(FEATURE_LIST),printf 'feature\t%s\t%s\t%s\t%s\t%s\t%s\n' '$($(f)_name)' '$(FEATURE_$(f))' '$($(f)_bit)' '$(strip $(notdir $(patsubst %.c,%.o,$($(f)_srcs))))' '$(strip $($(f)_shared))' '$(strip $($(f)_desc))';) } > $(BUILD_DIR)/features.built
	@echo '$(IMAGE_STAMP_TEXT)' > $(IMAGE_STAMP)
	@echo "Features: $(if $(FEATURES_ON),$(FEATURES_ON),(none)) (profile $(PROFILE))"

$(BUILD_DIR)/$(TARGET).bin: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/option_bytes48.bin
	$(CP) -O binary --gap-fill 0xFF $< $@
	@echo "Built: $@ ($$(wc -c < $@ | tr -d ' ') bytes)"

# One-shot option-byte blob for first-time DFU setup.
# Writes the AT32F403A User System Data region at 0x1FFFF800 so EOPB0 = 0xFE,
# enabling 224KB SRAM mode (default is 96KB, which is too small for our heap).
#
# The file MUST be exactly 48 bytes — the AT32 ROM DFU descriptor reports the
# USD region as "01*048 e" (one 48-byte sector) and rejects any shorter write
# with "address out of range". The "48" in the filename is literally the byte
# count, not a version suffix.
#
# Layout (byte + 1's complement pairs, per AT32F403A RM + usd_type struct):
#   0x00 FAP      = A5 / 5A   (unlocked — required to write other bytes)
#   0x02 SSB      = FF / 00   (watchdog/standby/stop defaults)
#   0x04 DATA0    = FF / 00
#   0x06 DATA1    = FF / 00
#   0x08 EPP0     = FF / 00   (no write-protected sectors)
#   0x0A EPP1     = FF / 00
#   0x0C EPP2     = FF / 00
#   0x0E EPP3     = FF / 00
#   0x10 EOPB0    = FE / 01   ← 224KB SRAM mode (the only non-default value)
#   0x12 reserved = FF / 00
#   0x14 DATA2..DATA7 = FF / 00  (×6 pairs)
#   0x20 ext_flash_key[8] = 16 × FF  (external flash encryption keys, unused)
#
# Flashed via: dfu-util -a 1 -d 2e3c:df11 -s 0x1FFFF800 -D build/option_bytes48.bin
$(BUILD_DIR)/option_bytes48.bin: Makefile | $(BUILD_DIR)
	@printf '\245\132\377\000\377\000\377\000\377\000\377\000\377\000\377\000\376\001\377\000\377\000\377\000\377\000\377\000\377\000\377\000\377\377\377\377\377\377\377\377\377\377\377\377\377\377\377\377' > $@
	@echo "Built: $@ ($$(wc -c < $@ | tr -d ' ') bytes) — EOPB0=0xFE, 224KB SRAM"

$(BUILD_DIR)/$(TARGET).hex: $(BUILD_DIR)/$(TARGET).elf
	$(CP) -O ihex $< $@

$(BUILD_DIR):
	mkdir -p $@

######################################################################
# Utility targets
######################################################################
clean:
	rm -rf $(BUILD_DIR)

disasm: $(BUILD_DIR)/$(TARGET).elf
	$(OBJDUMP) -d -S $< > $(BUILD_DIR)/$(TARGET).dis

size: $(BUILD_DIR)/$(TARGET).elf
	$(SZ) --format=berkeley $<

# ── Feature tools (features.mk is the manifest) ──
# features         the manifest and what THIS invocation would build
#                  (`make features PROFILE=bench`, `make features WITHOUT=stream`)
# features-report  what the image in $(BUILD_DIR) contains, per feature, from
#                  its map and the record the link wrote (features.built);
#                  fails unless the totals equal arm-none-eabi-size
# features-ab      FEATURE=name: builds AB_TARGET with and without that one
#                  feature (in $(BUILD_DIR)/ab_on and ab_off; the image in
#                  $(BUILD_DIR) itself is left alone) and prints the true
#                  difference
define _fm_nl


endef
# In-band `fwload` ceiling (src/drivers/fw_loader.c: FWL_DATA_MAX).
FWLOAD_MAX ?= 757760
AB_TARGET  ?= app-coldtrace-meter
_ab_feature = $(call _fm_lc,$(FEATURE))

.PHONY: features features-report features-ab
features:
	@echo "profile: $(PROFILE)$(if $(strip $(WITH)), WITH=$(strip $(WITH)))$(if $(strip $(_fm_cmd_without)), WITHOUT=$(strip $(_fm_cmd_without)))"
	@printf '%-10s %-5s %-4s %-8s %-12s %-10s %s\n' feature state bit default requires conflicts description
	$(foreach f,$(FEATURE_LIST),@printf '%-10s %-5s %-4s %-8s %-12s %-10s %s\n' '$($(f)_name)' '$(if $(filter 1,$(FEATURE_$(f))),on,off)' '$(strip $($(f)_bit))' '$(strip $($(f)_default))' '$(if $(strip $($(f)_requires)),$(call _fm_lc,$(strip $($(f)_requires))),-)' '$(if $(strip $($(f)_conflicts)),$(call _fm_lc,$(strip $($(f)_conflicts))),-)' '$(strip $($(f)_desc))'$(_fm_nl))
	@printf 'on: %s\nmask: 0x%08x\n' '$(if $(FEATURES_ON),$(FEATURES_ON),(none))' "$$(( $(FEATURES_MASK_EXPR) ))"
	@echo "profiles: $(patsubst PROFILE_%_with,%,$(filter PROFILE_%_with,$(.VARIABLES)))   targets with a profile: $(foreach v,$(filter PROFILE_OF_%,$(.VARIABLES)),$(patsubst PROFILE_OF_%,%,$(v))=$($(v)))"

features-report:
	@python3 -I ../scripts/features_report.py --build-dir $(BUILD_DIR) --size-tool $(SZ) --fwload-max $(FWLOAD_MAX)

features-ab:
	@test -n "$(_ab_feature)" || { echo "usage: make features-ab FEATURE=name [AB_TARGET=$(AB_TARGET)]"; exit 2; }
	@test -n "$(filter $(_ab_feature),$(_fm_names))" || { echo "features-ab: unknown feature '$(_ab_feature)' (known: $(_fm_names))"; exit 2; }
	@mkdir -p $(BUILD_DIR)
	@echo "features-ab: building $(AB_TARGET) with $(_ab_feature) ..."
	@$(MAKE) --no-print-directory $(AB_TARGET) BUILD_DIR=$(BUILD_DIR)/ab_on FEATURE= \
		WITH="$(strip $(_fm_cmd_with) $(_ab_feature))" WITHOUT="$(strip $(filter-out $(_ab_feature),$(_fm_cmd_without)))" \
		> $(BUILD_DIR)/ab_on.log 2>&1 || { tail -20 $(BUILD_DIR)/ab_on.log; echo "features-ab: the build WITH $(_ab_feature) failed ($(BUILD_DIR)/ab_on.log)"; exit 1; }
	@echo "features-ab: building $(AB_TARGET) without $(_ab_feature) ..."
	@$(MAKE) --no-print-directory $(AB_TARGET) BUILD_DIR=$(BUILD_DIR)/ab_off FEATURE= \
		WITH="$(strip $(filter-out $(_ab_feature),$(_fm_cmd_with)))" WITHOUT="$(strip $(_fm_cmd_without) $(_ab_feature))" \
		> $(BUILD_DIR)/ab_off.log 2>&1 || { tail -20 $(BUILD_DIR)/ab_off.log; echo "features-ab: the build WITHOUT $(_ab_feature) failed ($(BUILD_DIR)/ab_off.log)"; exit 1; }
	@python3 -I ../scripts/features_report.py --ab $(BUILD_DIR)/ab_on $(BUILD_DIR)/ab_off --feature $(_ab_feature) --size-tool $(SZ)

# The shell's command table as THIS invocation's feature set compiles it:
# usb_debug.c through the preprocessor, so the row macros of the features
# (STREAM_SHELL_ROWS, BENCHSHELL_SHELL_ROWS, ...) are expanded or empty.
#   make -s features-shell-table PROFILE=full
# scripts/test_shell_profiles.py reads it per profile (make test-shell-profiles).
SHELL_TABLE_SRC ?= src/drivers/shell.c
.PHONY: features-shell-table test-shell-profiles
features-shell-table:
	@$(CC) -E -P $(filter-out -MMD -MP,$(CFLAGS)) $(SHELL_TABLE_SRC)
test-shell-profiles:
	python3 -I ../scripts/test_shell_profiles.py --make "$(MAKE)"

# Host test of the feature tools: the report's map reader against a synthetic
# map (sums, the unattributed line, a mismatch is refused), and the manifest's
# own rules (profile, WITH/WITHOUT, the old spellings, unknown names, both
# WITH and WITHOUT, requires, conflicts, a bit used twice).
_ft = $(MAKE) --no-print-directory -s features
.PHONY: test-features
test-features:
	python3 -I ../scripts/features_report.py --self-test
	@o="$$($(_ft) PROFILE=default)"; echo "$$o" | grep -E '^stream +off ' >/dev/null && echo "$$o" | grep -E '^usbdrive +on ' >/dev/null || { echo "FAIL: defaults"; exit 1; }
	@o="$$($(_ft))"; echo "$$o" | grep -E '^profile: $(DAILY_PROFILE)$$' >/dev/null && echo "$$o" | grep -E '^stream +on ' >/dev/null || { echo "FAIL: no goal is not the daily profile"; exit 1; }
	@$(_ft) PROFILE=bench | grep -E '^stream +on ' >/dev/null || { echo "FAIL: bench profile"; exit 1; }
	@$(_ft) PROFILE=bench WITHOUT=STREAM | grep -E '^stream +off ' >/dev/null || { echo "FAIL: WITHOUT over profile"; exit 1; }
	@o="$$($(_ft) WITH=stream USB_MSC=0)"; echo "$$o" | grep -E '^stream +on ' >/dev/null && echo "$$o" | grep -E '^usbdrive +off ' >/dev/null || { echo "FAIL: WITH + USB_MSC=0"; exit 1; }
	@$(_ft) PROFILE=bench STREAM_BENCH=0 | grep -E '^stream +off ' >/dev/null || { echo "FAIL: old spellings"; exit 1; }
	@$(_ft) WITHOUT=stream | grep -E '^mask: 0x[0-9a-f]*[02468ace]$$' >/dev/null || { echo "FAIL: stream bit (0) set in the mask with the stream off"; exit 1; }
	@$(_ft) WITH=stream | grep -E '^mask: 0x[0-9a-f]*[13579bdf]$$' >/dev/null || { echo "FAIL: stream bit (0) clear in the mask with the stream on"; exit 1; }
	@! $(_ft) WITH=nosuch >/dev/null 2>&1 || { echo "FAIL: unknown feature accepted"; exit 1; }
	@! $(_ft) PROFILE=nosuch >/dev/null 2>&1 || { echo "FAIL: unknown profile accepted"; exit 1; }
	@! $(_ft) WITH=stream WITHOUT=stream >/dev/null 2>&1 || { echo "FAIL: WITH and WITHOUT accepted"; exit 1; }
	@! $(_ft) PROFILE=default USBDRIVE_requires=STREAM >/dev/null 2>&1 || { echo "FAIL: requires not enforced"; exit 1; }
	@$(_ft) USBDRIVE_requires=STREAM WITH=stream >/dev/null 2>&1 || { echo "FAIL: requires met but refused"; exit 1; }
	@! $(_ft) PROFILE=bench USBDRIVE_conflicts=STREAM >/dev/null 2>&1 || { echo "FAIL: conflicts not enforced"; exit 1; }
	@! $(_ft) USBDRIVE_bit=0 >/dev/null 2>&1 || { echo "FAIL: a bit used twice accepted"; exit 1; }
	@echo "features manifest: rules OK"
# ── end feature tools ──

# Build for emulator (skips HAL clock init that hangs without real hardware).
# main.c provides stub overrides for SystemInit/system_clock_config when
# EMULATOR_BUILD is defined. --allow-multiple-definition lets main.o's
# versions win over the HAL's (main.o is linked first).
emu: CFLAGS += -DEMULATOR_BUILD
emu: LDFLAGS += -Wl,--allow-multiple-definition
emu: LDSCRIPT = ld/at32f403a.ld
emu: clean all

# Flash app via USB HID bootloader (bootloader must be running)
flash: $(BUILD_DIR)/$(TARGET).bin
	python3 ../scripts/flash_preflight.py hid-app --image $< --address 0x08004000
	uv run ../scripts/hid_flash.py $< --preserve-blank-blocks --preserve-blank-blocks-range 0x08006000:0x08007000

# Flash app via DFU (device must be in ROM DFU mode — BOOT0 + reset)
flash-dfu: $(BUILD_DIR)/$(TARGET).bin
	python3 ../scripts/flash_preflight.py rom-dfu-app --image $<
	python3 ../scripts/at32_dfuse_write.py 0x08004000 $< --preserve-blank-pages-range 0x08006000:0x08007000 --leave 0x08004000

# Build and flash the bootloader (one-time, via DFU)
bootloader:
	$(MAKE) -C bootloader

flash-bootloader:
	$(MAKE) -C bootloader
	python3 ../scripts/flash_preflight.py rom-dfu-bootloader --bootloader bootloader/build/bootloader.bin
	dfu-util -a 0 -d 2e3c:df11 -s 0x08000000 -D bootloader/build/bootloader.bin

# Flash both bootloader + app in one go (via ROM DFU, for initial setup)
flash-all: $(BUILD_DIR)/$(TARGET).bin bootloader
	python3 ../scripts/flash_preflight.py rom-dfu-all --bootloader bootloader/build/bootloader.bin --image $(BUILD_DIR)/$(TARGET).bin --option-bytes $(BUILD_DIR)/option_bytes48.bin
	dfu-util -a 0 -d 2e3c:df11 -s 0x08000000 -D bootloader/build/bootloader.bin
	python3 ../scripts/at32_dfuse_write.py 0x08004000 $(BUILD_DIR)/$(TARGET).bin --preserve-blank-pages-range 0x08006000:0x08007000 --leave 0x08004000

# Run in Renode emulator (display only, no input)
renode: emu
	cd ../emulator/renode && /Applications/Renode.app/Contents/MacOS/renode --disable-gui run_openscope.resc

# Run in Renode with interactive input (use with lcd_viewer in another terminal)
renode-interactive: emu
	@echo "Start the LCD viewer in another terminal: cd emulator && ./lcd_viewer"
	cd ../emulator/renode && /Applications/Renode.app/Contents/MacOS/renode --disable-gui run_interactive.resc

# Run in Renode for quick smoke test (5 seconds, then quit)
renode-test: emu
	cd ../emulator/renode && /Applications/Renode.app/Contents/MacOS/renode --disable-gui --console -e \
		"include @run_openscope.resc; sleep 5; pause; cpu PC; quit"

# Soak test — random button fuzzing with fault monitoring (default 2 minutes)
SOAK_DURATION ?= 120
SOAK_SEED ?= 42
soak-test: emu
	python3 ../scripts/soak_test.py --no-build --duration $(SOAK_DURATION) --seed $(SOAK_SEED)

# Quick soak — 30-second smoke test with fuzzing
soak-quick: emu
	python3 ../scripts/soak_test.py --no-build --duration 30 --seed $(SOAK_SEED) --verbose

$(BUILD_DIR)/test_meter_data: tests/test_meter_data.c src/drivers/meter_data.c src/drivers/meter_stock.c src/drivers/meter_stock.h src/drivers/meter_data.h src/drivers/fpga_meter_plan.c src/drivers/fpga_meter_plan.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -Wall -Wextra -DMETER_DATA_HOST_TESTS -Isrc/drivers $< src/drivers/meter_data.c src/drivers/meter_stock.c src/drivers/fpga_meter_plan.c -lm -pthread -o $@

test-meter-data: $(BUILD_DIR)/test_meter_data
	$<

$(BUILD_DIR)/test_meter_auto: tests/test_meter_auto.c src/drivers/meter_auto.c src/drivers/meter_auto.h src/drivers/meter_data.h src/drivers/fpga_meter_plan.c src/drivers/fpga_meter_plan.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -Wall -Wextra -Isrc/drivers $< src/drivers/meter_auto.c src/drivers/fpga_meter_plan.c -lm -o $@

test-meter-auto: $(BUILD_DIR)/test_meter_auto
	$<

$(BUILD_DIR)/test_fpga_meter_plan: tests/test_fpga_meter_plan.c src/drivers/fpga_meter_plan.c src/drivers/fpga_meter_plan.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -Wall -Wextra -Isrc/drivers $< src/drivers/fpga_meter_plan.c -o $@

test-fpga-meter-plan: $(BUILD_DIR)/test_fpga_meter_plan
	$<

$(BUILD_DIR)/test_meter_word_map: tests/test_meter_word_map.c src/drivers/fpga_meter_plan.c src/drivers/fpga_meter_plan.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -Wall -Wextra -Isrc/drivers $< src/drivers/fpga_meter_plan.c -o $@

# The meter function-word map, from issue #15.
#
# ⚠ THIS TARGET IS EXPECTED TO FAIL. It is not broken, and it is deliberately
# NOT part of `test-meter` or any other aggregate — a red aggregate teaches
# people to ignore aggregates. Run it by name.
#
# stock_meter_cmd_low[] ties the right bytes to the wrong functions for 10 of
# our 11 submodes (submode 0 "DCV" sends 0x0514, which is the SoC's Auto). The
# correction is inbound as a contributor PR; when it lands, this target going
# green IS the verification. Do not silence it by patching the table as a side
# errand, and do not flip the AA 55 TX header default while it is still red.
.PHONY: test-meter-word-map
test-meter-word-map: $(BUILD_DIR)/test_meter_word_map
	@echo "note: this target is EXPECTED to fail until the issue #15 word-table correction lands"
	$<

# scope_measure.c formats the 12 parameters through scope_cursor.c (one copy of
# the unit formatting), which pulls in the cal and timebase tables.
$(BUILD_DIR)/test_scope_measure: tests/test_scope_measure.c src/ui/scope_measure.c src/ui/scope_measure.h \
                                 src/ui/scope_cursor.c src/ui/scope_cal.c src/ui/scope_timebase.c | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -Wall -Wextra -Isrc/ui $< src/ui/scope_measure.c \
		src/ui/scope_cursor.c src/ui/scope_cal.c src/ui/scope_timebase.c -lm -o $@

# Measurements the scope badges are allowed to print. Includes two negative
# controls (broken implementations that MUST be caught) — see the file header.
test-scope-measure: $(BUILD_DIR)/test_scope_measure
	$<

$(BUILD_DIR)/test_can_bus: tests/test_can_bus.c src/decode/decode_can.c src/decode/can_bus.c src/decode/can_bus.h src/decode/decode_can.h tests/fixtures/can_bench_records.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -Wall -Wextra -Isrc/decode -Itests $< src/decode/decode_can.c src/decode/can_bus.c -lm -o $@

# CAN bus analyser: real ring-buffer records from a two-node 500 kbit/s bench
# bus at 10 / 5 / 2.5 samples per bit (CRC-checked payloads, ACK vs cut-off),
# plus synthetic frames (CRC-15, ACK/NAK, 29-bit, RTR, clock skew, polarity,
# truncation, spikes, back-to-back frames, every ring rotation).
test-can-bus: $(BUILD_DIR)/test_can_bus
	$<

$(BUILD_DIR)/test_scope_cal: tests/test_scope_cal.c src/ui/scope_cal.c src/ui/scope_cal.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -Wall -Wextra -Isrc/ui $< src/ui/scope_cal.c -lm -o $@

# Per-channel, per-range vertical calibration. Tests the PROPERTIES that stop
# this table failing the way its two hardcoded predecessors did: uncalibrated
# ranges must return exactly zero, out-of-domain channels must refuse rather
# than clamp, the source-scale factor must reach every row uniformly (so one
# calibrated measurement can rescale the instrument), and the MEASURED tier's
# agreement/doubling claims must actually hold.
test-scope-cal: $(BUILD_DIR)/test_scope_cal
	$<

$(BUILD_DIR)/test_fw_loader: tests/test_fw_loader.c src/drivers/fw_loader.c src/drivers/fw_loader.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -Wall -Wextra -DFW_LOADER_HOST_TEST -Isrc/drivers $< src/drivers/fw_loader.c -o $@

# CDC firmware loader: tests the gates between a byte stream and an erased
# app slot (size/CRC/vector/timeout), the staging byte-exactness, and that
# apply() refuses anything not STAGED. The RAM installer itself is compiled
# out on the host and is validated on hardware.
#
# Declared .PHONY here rather than appended to the shared list below: that
# single long line is edited by every branch that adds a target, and it was
# the only merge conflict this work produced, twice. A second .PHONY line is
# equivalent to make and conflicts with nobody.
.PHONY: test-fw-loader
test-fw-loader: $(BUILD_DIR)/test_fw_loader
	$<

$(BUILD_DIR)/test_scope_timebase: tests/test_scope_timebase.c src/ui/scope_timebase.c src/ui/scope_timebase.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -Wall -Wextra -Isrc/ui $< src/ui/scope_timebase.c -lm -o $@

# Horizontal calibration. Guards the same failure shape as the vertical table,
# plus one specific to this axis: code 0x08 has had THREE published sample
# rates and all three were fitted to an incoherent record, so the test fails
# the build if any of them is re-entered.
test-scope-timebase: $(BUILD_DIR)/test_scope_timebase
	$<

# Parity C5: the fast-timebase sin(x)/x interpolator (table == documented
# design, frequency response to 0.4 fs, step overshoot within the ideal
# sinc's own, saturation, edges) and the Y-T column geometry (stock's 25 px
# divisions, 301 columns, trigger column 150 +- 150, zoom-1 mapping identical
# to the pre-C5 buf[toff + x], fill == interpolator).
$(BUILD_DIR)/test_sinc_interp: tests/test_sinc_interp.c src/dsp/sinc_interp.c src/dsp/sinc_interp.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Isrc/dsp $< src/dsp/sinc_interp.c -lm -o $@

.PHONY: test-sinc-interp
test-sinc-interp: $(BUILD_DIR)/test_sinc_interp
	$<

$(BUILD_DIR)/test_scope_geom: tests/test_scope_geom.c src/ui/scope_geom.c src/ui/scope_geom.h \
                              src/dsp/sinc_interp.c src/dsp/sinc_interp.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Isrc/ui -Isrc/dsp $< \
		src/ui/scope_geom.c src/dsp/sinc_interp.c -lm -o $@

.PHONY: test-scope-geom
test-scope-geom: $(BUILD_DIR)/test_scope_geom
	$<

# The canvas draw cycle (ui/canvas.c) against a model of the glass: what is
# sent, what is skipped, holes, and incremental == full.
$(BUILD_DIR)/test_canvas: tests/test_canvas.c src/ui/canvas.c src/ui/canvas.h src/drivers/lcd.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Isrc/ui -Isrc/drivers $< src/ui/canvas.c -o $@

.PHONY: test-canvas
test-canvas: $(BUILD_DIR)/test_canvas
	$<

# The Y-T vertical transform (yt_vmap.h): true scale (stock geometry: 0 V at
# position 0 on the centre line, one division = the range's nominal volts/div,
# rails, clipping); autofit equal to the former autofit, code for code; split
# bands only when both channels autofit.
$(BUILD_DIR)/test_yt_vmap: tests/test_yt_vmap.c src/ui/yt_vmap.h src/ui/scope_cal.c \
                           src/ui/scope_cal.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Isrc/ui $< src/ui/scope_cal.c -lm -o $@

.PHONY: test-yt-vmap
test-yt-vmap: $(BUILD_DIR)/test_yt_vmap
	$<

$(BUILD_DIR)/test_scope_cursor: tests/test_scope_cursor.c src/ui/scope_cursor.c src/ui/scope_cursor.h \
                                src/ui/scope_cal.c src/ui/scope_timebase.c | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -Wall -Wextra -Isrc/ui $< \
		src/ui/scope_cursor.c src/ui/scope_cal.c src/ui/scope_timebase.c -lm -o $@

# Cursor delta readouts. The cursor is the one feature whose whole purpose is
# measurement, and its units used to come from two floats nothing ever updated.
# The tests assert the two properties that stop that returning: the answer must
# MOVE with the measured tables (a constant cannot depend on a code or a range),
# and where there is no calibration there must be no unit. Includes four
# negative controls, among them the historical constants transcribed verbatim.
.PHONY: test-scope-cursor
test-scope-cursor: $(BUILD_DIR)/test_scope_cursor
	$<

$(BUILD_DIR)/test_scope_freq: tests/test_scope_freq.c src/ui/scope_freq.c src/ui/scope_freq.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Isrc/ui $< src/ui/scope_freq.c -lm -o $@

# Acceptance test for EXP-13, run against REAL bench captures rather than
# synthetic sines — a synthetic sine is the case that always worked.
# With no argument it runs BOTH fixture sets: the tuned one, and the held-out
# one whose drive frequencies were chosen after the thresholds were fixed.
test-scope-freq: $(BUILD_DIR)/test_scope_freq
	./$<

# Lock hooks around the shared FFT scratch (shell task vs display task).
.PHONY: test-scope-freq-lock
$(BUILD_DIR)/test_scope_freq_lock: tests/test_scope_freq_lock.c src/ui/scope_freq.c src/ui/scope_freq.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Isrc/ui $< src/ui/scope_freq.c -lm -o $@

test-scope-freq-lock: $(BUILD_DIR)/test_scope_freq_lock
	./$<

# Shell `persist` switch (bench sweeps skip the per-command settings flush).
.PHONY: test-shell-persist
$(BUILD_DIR)/test_shell_persist: tests/test_shell_persist.c src/util/shell_persist.c src/util/shell_persist.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O1 -Wall -Wextra -Werror -Isrc/util $< src/util/shell_persist.c -o $@

test-shell-persist: $(BUILD_DIR)/test_shell_persist
# LCD panel health (detect an ST7789 that reset itself, re-init it).
.PHONY: test-lcd-health
$(BUILD_DIR)/test_lcd_health: tests/test_lcd_health.c src/util/lcd_health.c src/util/lcd_health.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O1 -Wall -Wextra -Werror -Isrc/util $< src/util/lcd_health.c -o $@

test-lcd-health: $(BUILD_DIR)/test_lcd_health
	./$<

# Settings persistence: config.c + settings_store.c against a NOR flash model
# (erase sets 0xFF, program only clears bits, page programs cannot cross a page),
# with power cuts injected into the real write path.
# Proves the round trip, the fallbacks (first boot / torn record / bad checksum /
# foreign version / damage anywhere in the log), a change -> power cycle ->
# verify loop across compaction, and — the one that matters — that the settings
# writer cannot reach a read-only region. See tests/test_config_persist.c.
#
# The same file is built two ways:
#   test_config_persist                SETTINGS_PERSIST_WRITES=1, must pass
#   test_config_persist_nowrite        SETTINGS_PERSIST_WRITES=0, the negative
#                                      control: the change/cycle/verify loop must
#                                      FAIL to persist (spec settings-persistence S3)
# (A third, known-defects build held the #57/#58 power-cut tests until those
# were fixed; they run in test_config_persist now.)
CONFIG_PERSIST_SRCS = src/util/config.c src/util/settings_store.c src/util/session_store.c src/drivers/flash_regions.c \
                      src/ui/theme.c src/ui/scope_state.c src/ui/scope_roll.c src/ui/scope_cal.c src/ui/scope_timebase.c \
                      src/dsp/signal_gen.c src/dsp/siggen_wave.c src/util/sys_prefs.c \
                      $(R1A_CONFIG_PERSIST_EXTRA) $(M3_CONFIG_PERSIST_EXTRA)
CONFIG_PERSIST_DEPS = tests/test_config_persist.c $(CONFIG_PERSIST_SRCS) \
                      src/util/config.h src/util/settings_store.h src/drivers/flash_regions.h
CONFIG_PERSIST_CC   = $(HOST_CC) -std=c11 -Wall -Wextra -Werror -O1 \
                      -Isrc/util -Isrc/drivers -Isrc/ui -Isrc/dsp $(M3_CONFIG_PERSIST_FLAGS)

$(BUILD_DIR)/test_config_persist: $(CONFIG_PERSIST_DEPS) | $(BUILD_DIR)
	$(CONFIG_PERSIST_CC) -DSETTINGS_PERSIST_WRITES=1 $< $(CONFIG_PERSIST_SRCS) -o $@

$(BUILD_DIR)/test_config_persist_nowrite: $(CONFIG_PERSIST_DEPS) | $(BUILD_DIR)
	$(CONFIG_PERSIST_CC) -DSETTINGS_PERSIST_WRITES=0 $< $(CONFIG_PERSIST_SRCS) -o $@

# For scripts/test_config_persist.py: its mutation tests compile these same
# sources with one file replaced, so it asks for the lists instead of
# keeping a copy.
.PHONY: print-config-persist
print-config-persist:
	@echo 'srcs $(CONFIG_PERSIST_SRCS)'
	@echo 'cc $(CONFIG_PERSIST_CC)'

test-config-persist: $(BUILD_DIR)/test_config_persist $(BUILD_DIR)/test_config_persist_nowrite
	$(BUILD_DIR)/test_config_persist
	$(BUILD_DIR)/test_config_persist_nowrite

# Stock meter behaviours that are pure logic (M4 beep gate/pulse, M5 status
# text, degF, range index and rescale, the calibration-wizard word guard).
# Its own .PHONY line, as the comment above test-fw-loader explains.
.PHONY: test-meter-stock
$(BUILD_DIR)/test_meter_stock: tests/test_meter_stock.c src/drivers/meter_stock.c src/drivers/meter_stock.h src/drivers/meter_data.c src/drivers/meter_data.h src/drivers/fpga_meter_plan.c | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -Wall -Wextra -DMETER_DATA_HOST_TESTS -Isrc/drivers $< src/drivers/meter_stock.c src/drivers/meter_data.c src/drivers/fpga_meter_plan.c -lm -pthread -o $@

test-meter-stock: $(BUILD_DIR)/test_meter_stock
	$<

test-stock-meter-literals:
	python3 ../scripts/test_stock_meter_literals.py

test-meter: test-meter-data test-meter-auto test-fpga-meter-plan test-meter-stock test-stock-meter-literals

# Typed units (src/util/units.h). test-units: conversions, clamps, rounding,
# cursor sign, fmt_fixed/parse_fixed. test-units-negative: the same header must
# REJECT unit mixing; each numbered case in tests/test_units_negative.c is
# compiled on its own and must fail, and case 0 (correct use) must compile.
UNITS_NEG_CASES = 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16

$(BUILD_DIR)/test_units: tests/test_units.c src/util/units.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror $< -lm -o $@

# Battery gauge logic (PC7 external power, charging step, percent curve).
$(BUILD_DIR)/test_battery_gauge: tests/test_battery_gauge.c src/drivers/battery_gauge.c src/drivers/battery_gauge.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror tests/test_battery_gauge.c src/drivers/battery_gauge.c -o $@

.PHONY: test-battery-gauge
test-battery-gauge: $(BUILD_DIR)/test_battery_gauge
	$<

.PHONY: test-units test-units-negative
test-units: $(BUILD_DIR)/test_units
	$<

test-units-negative: tests/test_units_negative.c src/util/units.h | $(BUILD_DIR)
	@$(HOST_CC) -std=gnu11 -Wall -Wextra -Werror -fsyntax-only -DCASE=0 $< \
		|| { echo "units-negative: positive control (CASE 0) does not compile"; exit 1; }
	@bad=0; for n in $(UNITS_NEG_CASES); do \
		if $(HOST_CC) -std=gnu11 -Wall -Wextra -Werror -fsyntax-only -DCASE=$$n $< 2>/dev/null; then \
			echo "units-negative: CASE $$n COMPILED (must fail)"; bad=1; \
		fi; \
	done; \
	if [ $$bad -ne 0 ]; then exit 1; fi; \
	echo "units-negative: control compiles, all $(words $(UNITS_NEG_CASES)) mixing cases rejected"

# Typed units in the pure modules (ARCH round, C2). The module headers must
# REJECT the calls the bare-integer interfaces allowed (a raw ADC byte as a
# record code, the offset as a parameter, a cursor difference with no order,
# a gain as a skew, ticks as milliseconds); case 0 is the correct use.
UNITS_C2_NEG_CASES = 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20
UNITS_C2_NEG_HDRS  = src/util/units.h src/ui/scope_measure.h src/ui/scope_roll.h \
                     src/ui/scope_cursor.h src/dsp/trig_edge.h src/dsp/interleave.h \
                     src/util/lcd_health.h
UNITS_C2_NEG_CC    = $(HOST_CC) -std=gnu11 -Wall -Wextra -Werror -fsyntax-only -Isrc/util

.PHONY: test-units-c2-negative
test-units-c2-negative: tests/test_units_c2_negative.c $(UNITS_C2_NEG_HDRS) | $(BUILD_DIR)
	@$(UNITS_C2_NEG_CC) -DCASE=0 $< \
		|| { echo "units-c2-negative: positive control (CASE 0) does not compile"; exit 1; }
	@bad=0; for n in $(UNITS_C2_NEG_CASES); do \
		if $(UNITS_C2_NEG_CC) -DCASE=$$n $< 2>/dev/null; then \
			echo "units-c2-negative: CASE $$n COMPILED (must fail)"; bad=1; \
		fi; \
	done; \
	if [ $$bad -ne 0 ]; then exit 1; fi; \
	echo "units-c2-negative: control compiles, all $(words $(UNITS_C2_NEG_CASES)) mixing cases rejected"

# Property model (src/model/, ARCH-1 M0): the registry core, host only, not
# linked into the firmware yet. The table, lookup/glob, parse/format with
# units, clamp/step/wrap, all-or-nothing multi-set validation, epochs, RO
# refusal, NOT_SET keeps the old value (every PF_HW property), profile lines,
# defaults and instruments, on the real pure modules with fake hardware ops.
MODEL_SRCS      = src/model/model.c src/model/props_scope.c src/model/props_gen.c \
                  src/model/props_meter.c src/model/props_sys.c src/model/model_session.c
MODEL_TEST_LINK = src/ui/scope_state.c src/ui/scope_roll.c src/ui/scope_cal.c src/ui/scope_timebase.c \
                  src/ui/scope_measure.c src/ui/scope_cursor.c src/ui/meas_select.c src/ui/theme.c \
                  src/ui/key_model.c src/ui/osc_menu_model.c \
                  src/dsp/signal_gen.c src/dsp/siggen_wave.c src/util/sys_prefs.c

$(BUILD_DIR)/test_model: tests/test_model.c $(MODEL_SRCS) src/model/model.h src/model/model_table.h \
                         src/util/units.h $(MODEL_TEST_LINK) | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O1 -Wall -Wextra -Werror -Isrc/model -Isrc/util \
		-Isrc/ui -Isrc/dsp -Isrc/drivers $< $(MODEL_SRCS) $(MODEL_TEST_LINK) -lm -o $@

.PHONY: test-model
test-model: $(BUILD_DIR)/test_model
	$<

# Host-side configuration tool (scripts/model_config.py, ARCH-1 M5): its
# reading of model_table.h (the parser, labels, units, the text of every
# value) against the firmware model's own default lines and against the get
# lines and set answers the firmware's shell code prints, profiles/default.json
# against the table, and list / defaults / from-device / to-device / diff /
# build against a fake shell (in-process and over a socket on an ephemeral
# port). No device, no serial port.
# ... and the build-time defaults profile end to end (make DEFAULTS=...): the
# model compiled on the host with the header `model_config.py build` writes
# from tests/fixtures/profile_lab.json must report the profile's defaults.
$(BUILD_DIR)/test_model_profile: tests/test_model.c $(MODEL_SRCS) src/model/model.h src/model/model_table.h \
                                 tests/fixtures/profile_lab.json ../scripts/model_config.py \
                                 $(MODEL_TEST_LINK) | $(BUILD_DIR)
	python3 -I ../scripts/model_config.py build tests/fixtures/profile_lab.json -o $(BUILD_DIR)/test_model_profile.h
	$(HOST_CC) -std=gnu11 -O1 -Wall -Wextra -Werror -Isrc/model -Isrc/util \
		-Isrc/ui -Isrc/dsp -Isrc/drivers -include $(BUILD_DIR)/test_model_profile.h $< $(MODEL_SRCS) \
		$(MODEL_TEST_LINK) -lm -o $@

.PHONY: test-model-config
test-model-config: $(BUILD_DIR)/test_model $(BUILD_DIR)/test_model_profile $(BUILD_DIR)/test_model_exec
	$(BUILD_DIR)/test_model --dump-defaults $(BUILD_DIR)/model_defaults.txt
	$(BUILD_DIR)/test_model_profile --dump-defaults $(BUILD_DIR)/model_defaults_lab.txt
	$(BUILD_DIR)/test_model_exec --dump-shell $(BUILD_DIR)/model_shell_lines.txt
	python3 -I ../scripts/model_config.py --self-test --c-defaults $(BUILD_DIR)/model_defaults.txt \
		--c-shell $(BUILD_DIR)/model_shell_lines.txt \
		--c-profile tests/fixtures/profile_lab.json $(BUILD_DIR)/model_defaults_lab.txt

# Meter link policy (src/model/meter_link_policy.c): the truth table of
# "should the meter link be on" (screen, USB host, meter.link) and the
# debounce of the USB host state. Pure.
$(BUILD_DIR)/test_meter_link_policy: tests/test_meter_link_policy.c src/model/meter_link_policy.c \
                                     src/model/meter_link_policy.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Isrc/model $< src/model/meter_link_policy.c -o $@

.PHONY: test-meter-link-policy
test-meter-link-policy: $(BUILD_DIR)/test_meter_link_policy
	$<

# Executor (ARCH-1 M1, src/model/model_exec.c) behind a host port: queue
# order, seq-tagged result slots, timeout and withdrawal, a late answer never
# read as the next one's, seq wrap, all-or-nothing multi-set, the not-wired
# list, and the shell's get/set lines (every get line parses back as a profile
# line). One case runs a real executor thread (pthreads) for "in progress".
MODEL_EXEC_SRCS = src/model/model_exec.c src/model/model_shell.c
$(BUILD_DIR)/test_model_exec: tests/test_model_exec.c $(MODEL_SRCS) $(MODEL_EXEC_SRCS) \
                              src/model/model.h src/model/model_table.h src/model/model_exec.h \
                              src/model/model_shell.h src/util/units.h $(MODEL_TEST_LINK) | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O1 -Wall -Wextra -Werror -DMODEL_EXEC_TEST -Isrc/model \
		-Isrc/util -Isrc/ui -Isrc/dsp -Isrc/drivers $< $(MODEL_SRCS) $(MODEL_EXEC_SRCS) \
		$(MODEL_TEST_LINK) -lm -lpthread -o $@

.PHONY: test-model-exec
test-model-exec: $(BUILD_DIR)/test_model_exec
	$<

# Settings persistence generated from the property table (ARCH-1 M3): the v5
# codec table against a frozen copy of the hand-written capture / apply /
# defaults it replaced: byte-identical records and identical state over the
# defaults, random live states and random (also hostile) records; the
# per-field out-of-range policy; raw carry of the stored-only bytes; table
# coverage; settings notes by origin.
$(BUILD_DIR)/test_model_persist: tests/test_model_persist.c $(MODEL_SRCS) src/model/model_persist_v5.c \
                                 src/model/model_persist_v5.h src/model/model.h src/model/model_table.h \
                                 src/util/config.c src/util/config.h src/util/settings_store.c src/util/session_store.c \
                                 src/drivers/flash_regions.c $(MODEL_TEST_LINK) | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O1 -Wall -Wextra -Werror -Isrc/model -Isrc/util \
		-Isrc/ui -Isrc/dsp -Isrc/drivers $< $(MODEL_SRCS) src/model/model_persist_v5.c \
		src/util/config.c src/util/settings_store.c src/util/session_store.c src/drivers/flash_regions.c \
		$(MODEL_TEST_LINK) -lm -o $@

.PHONY: test-model-persist
test-model-persist: $(BUILD_DIR)/test_model_persist
	$(BUILD_DIR)/test_model_persist

# Reset to defaults (ARCH-1 M5): the built-in defaults over the user settings
# through the setters; every property back to its default, the hardware ops
# called, the record byte for byte the defaults', the confirm step, and no
# program or erase outside the settings region of a NOR model of the whole
# W25Q (calibration, gallery, logo, firmware cache), a compaction included.
# Protocol decoders, FFT, theme, config core, the waterfall drawing.
DECODE_TEST_SRCS = src/decode/decode_uart.c src/decode/decode_i2c.c src/decode/decode_spi.c src/decode/decode_can.c
DECODE_TEST_DEPS = $(DECODE_TEST_SRCS) src/decode/decoder.h src/decode/decode_uart.h src/decode/decode_i2c.h \
                   src/decode/decode_spi.h src/decode/decode_can.h
CONFIG_TEST_SRCS = $(sort $(CONFIG_PERSIST_SRCS) tests/config_test_env.c)

$(BUILD_DIR)/test_decode: tests/test_decode.c $(DECODE_TEST_DEPS) | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O1 -Wall -Wextra -Werror -Isrc/decode $< $(DECODE_TEST_SRCS) -lm -o $@

$(BUILD_DIR)/test_decode_noisy: tests/test_decode_noisy.c $(DECODE_TEST_DEPS) | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O1 -Wall -Wextra -Werror -Isrc/decode $< $(DECODE_TEST_SRCS) -lm -o $@

$(BUILD_DIR)/test_decode_kline: tests/test_decode_kline.c src/decode/decode_kline.c src/decode/decode_kline.h \
                                src/decode/decode_uart.c src/decode/decode_uart.h src/decode/decoder.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O1 -Wall -Wextra -Werror -Isrc/decode $< src/decode/decode_kline.c src/decode/decode_uart.c -lm -o $@

$(BUILD_DIR)/test_fft: tests/test_fft.c src/dsp/fft.c src/dsp/fft.h src/dsp/fft_test_signals.c src/dsp/fft_test_signals.h \
                       src/dsp/shared_mem.c src/dsp/shared_mem.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O1 -Wall -Wextra -Werror -Isrc/dsp $< src/dsp/fft.c src/dsp/fft_test_signals.c src/dsp/shared_mem.c -lm -o $@

$(BUILD_DIR)/test_theme: tests/test_theme.c src/ui/theme.c src/ui/theme.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O1 -Wall -Wextra -Werror -Isrc/ui -Isrc/drivers $< src/ui/theme.c -o $@

$(BUILD_DIR)/test_health_gap: tests/test_health_gap.c src/util/health_gap.c src/util/health_gap.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O1 -Wall -Wextra -Werror -Isrc/util $< src/util/health_gap.c -o $@

$(BUILD_DIR)/test_crash_policy: tests/test_crash_policy.c src/util/crash_policy.c src/util/crash_policy.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O1 -Wall -Wextra -Werror -Isrc/util $< src/util/crash_policy.c -o $@

.PHONY: test-crash-policy
test-crash-policy: $(BUILD_DIR)/test_crash_policy
	$(BUILD_DIR)/test_crash_policy

.PHONY: test-health-gap
test-health-gap: $(BUILD_DIR)/test_health_gap
	$<

$(BUILD_DIR)/test_config: tests/test_config.c $(CONFIG_TEST_SRCS) src/util/config.h | $(BUILD_DIR)
	$(CONFIG_PERSIST_CC) -DSETTINGS_PERSIST_WRITES=1 $< $(CONFIG_TEST_SRCS) -lm -o $@

$(BUILD_DIR)/test_waterfall_blit: tests/test_waterfall_blit.c src/ui/waterfall_blit.c src/ui/waterfall_blit.h \
                                  src/drivers/lcd.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O1 -Wall -Wextra -Werror -Isrc/ui -Isrc/drivers $< src/ui/waterfall_blit.c -o $@

.PHONY: test-decode test-decode-noisy test-decode-kline test-fft test-theme test-config test-waterfall-blit
test-decode: $(BUILD_DIR)/test_decode
	$<
test-decode-noisy: $(BUILD_DIR)/test_decode_noisy
	$<
test-decode-kline: $(BUILD_DIR)/test_decode_kline
	$<
test-fft: $(BUILD_DIR)/test_fft
	$<
test-theme: $(BUILD_DIR)/test_theme
	$<
test-config: $(BUILD_DIR)/test_config
	$<
test-waterfall-blit: $(BUILD_DIR)/test_waterfall_blit
	$<

$(BUILD_DIR)/test_settings_reset: tests/test_settings_reset.c $(MODEL_SRCS) src/model/model_persist_v5.c \
                                  src/model/model_persist_v5.h src/model/model.h src/model/model_table.h \
                                  src/util/config.c src/util/config.h src/util/settings_store.c src/util/session_store.c \
                                  src/util/settings_store.h src/ui/settings_reset.h \
                                  src/drivers/flash_regions.c $(MODEL_TEST_LINK) | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O1 -Wall -Wextra -Werror -DSETTINGS_PERSIST_WRITES=1 \
		-Isrc/model -Isrc/util -Isrc/ui -Isrc/dsp -Isrc/drivers $< $(MODEL_SRCS) \
		src/model/model_persist_v5.c src/util/config.c src/util/settings_store.c src/util/session_store.c \
		src/drivers/flash_regions.c $(MODEL_TEST_LINK) -lm -o $@

.PHONY: test-settings-reset
test-settings-reset: $(BUILD_DIR)/test_settings_reset
	$<

# Session snapshot (Startup on Boot = Restore; src/model/model_session.c,
# src/util/session_store.c, the boot apply in model.c): membership against
# the exclusion list, the format against an encoder written from its
# description, save -> power cycle -> apply over random states, every
# fallback against the same boot with a blank session log, other tables'
# snapshots, refused applies, the apply order, Startup != Restore, Reset to
# defaults, and the flash writes (one record a save, none otherwise, nothing
# outside the two logs) on a NOR model of the whole W25Q.
$(BUILD_DIR)/test_session: tests/test_session.c $(MODEL_SRCS) src/model/model_persist_v5.c \
                           src/model/model_persist_v5.h src/model/model.h src/model/model_table.h \
                           src/model/model_session.h src/model/session_shell.c src/model/model_shell.h \
                           src/util/config.c src/util/config.h src/util/settings_store.c src/util/session_store.c \
                           src/util/settings_store.h src/util/session_store.h \
                           src/drivers/flash_regions.c $(MODEL_TEST_LINK) | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O1 -Wall -Wextra -Werror -DSETTINGS_PERSIST_WRITES=1 \
		-DSESSION_SHELL_HOST -Isrc/model -Isrc/util -Isrc/ui -Isrc/dsp -Isrc/drivers $< $(MODEL_SRCS) \
		src/model/model_persist_v5.c src/model/session_shell.c src/util/config.c src/util/settings_store.c src/util/session_store.c \
		src/drivers/flash_regions.c $(MODEL_TEST_LINK) -lm -o $@

.PHONY: test-session
test-session: $(BUILD_DIR)/test_session
	$<

# UI route (ARCH-1 M2, src/model/ui_shell.c): the `ui seq` token parser (key
# names, KEY*n, long:KEY, wait:ms, bad tokens, POWER refused, the whole line
# checked before any press). The firmware half (presses, waits) is not built.
$(BUILD_DIR)/test_ui_shell: tests/test_ui_shell.c src/model/ui_shell.c src/model/ui_shell.h \
                           src/ui/key_model.h src/ui/ui.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O1 -Wall -Wextra -Werror -DUI_SHELL_TEST -Isrc/model -Isrc/ui -Isrc/util -Isrc/dsp \
		$< src/model/ui_shell.c -o $@

.PHONY: test-ui-shell
test-ui-shell: $(BUILD_DIR)/test_ui_shell
	$<

# A call made on another task (src/util/task_call.c): the post, the owner's
# serve, the withdrawal on a timeout and the wait for a call that had started.
$(BUILD_DIR)/test_task_call: tests/test_task_call.c src/util/task_call.c src/util/task_call.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O1 -Wall -Wextra -Werror -Isrc/util $< src/util/task_call.c -o $@

.PHONY: test-task-call
test-task-call: $(BUILD_DIR)/test_task_call
	$<

# Redraw gate: counts skipped ticks, live frames and full repaints for the
# scope/siggen screens over scenario traces, and checks that deliberately
# broken gates fail. See tests/test_redraw_gate.c.
$(BUILD_DIR)/test_redraw_gate: tests/test_redraw_gate.c src/ui/redraw_gate.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -Wall -Wextra -Isrc/ui $< -o $@

test-redraw-gate: $(BUILD_DIR)/test_redraw_gate
	$<

# FFT on the live acquisition record (spec fft-live, S1/S3 host half):
# head skip, bin -> Hz only from a known rate, refusal at rate 0, a tone in
# the predicted bin, the window spanning the record — each with a negative
# control. See tests/test_fft_live.c.
$(BUILD_DIR)/test_fft_live: tests/test_fft_live.c src/dsp/fft_live.c src/dsp/fft_live.h \
                            src/dsp/fft.c src/dsp/fft.h src/dsp/shared_mem.c | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Isrc/dsp -Isrc/ui \
		$< src/dsp/fft_live.c src/dsp/fft.c src/dsp/shared_mem.c -lm -o $@

test-fft-live: $(BUILD_DIR)/test_fft_live
	$<

# Which channel the spectrum views analyse (src/dsp/fft_source.c, pure): the
# active channel, its enable, what the published record holds and the
# 250 MS/s merge -> buffer, sample rate, label, "CH2 is off".
$(BUILD_DIR)/test_fft_source: tests/test_fft_source.c src/dsp/fft_source.c src/dsp/fft_source.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O1 -Wall -Wextra -Werror -Isrc/dsp $< src/dsp/fft_source.c -o $@

.PHONY: test-fft-source
test-fft-source: $(BUILD_DIR)/test_fft_source
	$<

# The firmware's FFT path on the host: src/dsp/fft.c with USE_CMSIS_DSP, the
# CMSIS-DSP sources the image links and the same table selection
# (FFT_TABLE_DEFS). The init must succeed for 4096 and a known tone must peak
# in its bin; a length whose tables are not built must be refused, and
# fft.c must then report the engine as not usable. (CMSIS's generic C code
# runs here, not the Cortex-M4 paths; the tables are the same bytes.
# __GNUC_PYTHON__ is CMSIS-DSP's own switch for a host build: no Cortex
# compiler header.)
FFT_CMSIS_HOST_SRCS = CMSIS-DSP/Source/TransformFunctions/arm_rfft_fast_f32.c \
                      CMSIS-DSP/Source/TransformFunctions/arm_rfft_fast_init_f32.c \
                      CMSIS-DSP/Source/TransformFunctions/arm_cfft_f32.c \
                      CMSIS-DSP/Source/TransformFunctions/arm_cfft_init_f32.c \
                      CMSIS-DSP/Source/TransformFunctions/arm_cfft_radix8_f32.c \
                      CMSIS-DSP/Source/CommonTables/arm_common_tables.c \
                      CMSIS-DSP/Source/CommonTables/arm_const_structs.c \
                      CMSIS-DSP/Source/ComplexMathFunctions/arm_cmplx_mag_f32.c \
                      CMSIS-DSP/Source/BasicMathFunctions/arm_scale_f32.c
$(BUILD_DIR)/test_fft_cmsis_tables: tests/test_fft_cmsis_tables.c src/dsp/fft.c src/dsp/fft.h \
                                    src/dsp/shared_mem.c $(FFT_CMSIS_HOST_SRCS) | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror -D__GNUC_PYTHON__ -DUSE_CMSIS_DSP $(FFT_TABLE_DEFS) \
		-Isrc/dsp -ICMSIS-DSP/Include $< src/dsp/fft.c src/dsp/shared_mem.c \
		$(FFT_CMSIS_HOST_SRCS) -lm -o $@

# The same sources with the real FFT's twiddle table left out: the init must
# refuse, and fft.c must report it and transform nothing.
$(BUILD_DIR)/test_fft_cmsis_tables_missing: tests/test_fft_cmsis_tables.c src/dsp/fft.c src/dsp/fft.h \
                                            src/dsp/shared_mem.c $(FFT_CMSIS_HOST_SRCS) | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror -D__GNUC_PYTHON__ -DUSE_CMSIS_DSP -DEXPECT_ENGINE_FAIL \
		$(filter-out -DARM_TABLE_TWIDDLECOEF_RFFT_F32_4096,$(FFT_TABLE_DEFS)) \
		-Isrc/dsp -ICMSIS-DSP/Include $< src/dsp/fft.c src/dsp/shared_mem.c \
		$(FFT_CMSIS_HOST_SRCS) -lm -o $@

.PHONY: test-fft-cmsis-tables
test-fft-cmsis-tables: $(BUILD_DIR)/test_fft_cmsis_tables $(BUILD_DIR)/test_fft_cmsis_tables_missing
	$(BUILD_DIR)/test_fft_cmsis_tables
	$(BUILD_DIR)/test_fft_cmsis_tables_missing

$(BUILD_DIR)/test_fuse: tests/test_fuse.c src/ui/fuse_model.c src/ui/fuse_model.h src/ui/fuse_table.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -Wall -Wextra -Werror -Isrc/ui $< src/ui/fuse_model.c -lm -o $@

# Fuse tester arithmetic, colours and shapes. Keeps the pre-2026-10-06 current
# formula (amps printed as mA) as a negative control that must fail.
test-fuse: $(BUILD_DIR)/test_fuse
	$<

# The trigger anchor, the software edge search and the in-place rotation
# (dsp/trig_edge.c), the anchor on real EXP-55/56 records.
$(BUILD_DIR)/test_trig_edge: tests/test_trig_edge.c src/dsp/trig_edge.c src/dsp/trig_edge.h \
                             tests/fixtures/trig_edge_records.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Isrc/dsp -Itests \
		$< src/dsp/trig_edge.c -lm -o $@

test-trig-edge: $(BUILD_DIR)/test_trig_edge
	$<

# Math channel (parity C12): the eight ops on records with volts/counts units,
# division by ~0, clipping, the 1-2-5 scale; and the persistence buffer, whose
# fade is timed in milliseconds and which is fed from live trace rows.
$(BUILD_DIR)/test_math_channel: tests/test_math_channel.c src/dsp/math_channel.c src/dsp/math_channel.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Isrc/dsp $< src/dsp/math_channel.c -lm -o $@

test-math-channel: $(BUILD_DIR)/test_math_channel
	$<
.PHONY: test-math-channel test-persistence

# The built-in FPGA design is a complete Gowin stream for this chip. See
# tests/test_fpga_bitstream.c.
$(BUILD_DIR)/test_fpga_bitstream: tests/test_fpga_bitstream.c src/drivers/fpga_bitstream_core.c \
                                  src/drivers/fpga_bitstream_core.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O1 -Wall -Wextra -Werror -Isrc/drivers $< src/drivers/fpga_bitstream_core.c -o $@

.PHONY: test-fpga-bitstream
test-fpga-bitstream: $(BUILD_DIR)/test_fpga_bitstream
	$<

$(BUILD_DIR)/test_persistence: tests/test_persistence.c src/ui/persistence.c src/ui/persistence.h src/ui/scope_state.h src/dsp/shared_mem.c | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Isrc/ui -Isrc/dsp $< src/ui/persistence.c src/dsp/shared_mem.c -lm -o $@

test-persistence: $(BUILD_DIR)/test_persistence

# Trigger acquisition policy (upstream #84) and status badge, parity C3: the
# full input space against the pre-C3 loop, the coherence rules by name.
$(BUILD_DIR)/test_trig_policy: tests/test_trig_policy.c src/dsp/trig_policy.c src/dsp/trig_policy.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Isrc/dsp \
		$< src/dsp/trig_policy.c -o $@

.PHONY: test-trig-policy
test-trig-policy: $(BUILD_DIR)/test_trig_policy
	$<

# Merge, level map (line and bench knots) and skew FIR with its image spur (parity ilplus).
$(BUILD_DIR)/test_interleave: tests/test_interleave.c src/dsp/interleave.c src/dsp/interleave.h src/dsp/interleave_skew_taps.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Isrc/dsp \
		$< src/dsp/interleave.c -lm -o $@

test-interleave: $(BUILD_DIR)/test_interleave
	$<

# 250 MS/s self-calibration fit, knots and refusals (parity C2), skew estimate (ilplus).
$(BUILD_DIR)/test_interleave_cal: tests/test_interleave_cal.c src/dsp/interleave_cal.c src/dsp/interleave_cal.h src/dsp/interleave.c src/dsp/interleave.h src/dsp/interleave_skew_taps.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Isrc/dsp \
		$< src/dsp/interleave_cal.c src/dsp/interleave.c -lm -o $@

test-interleave-cal: $(BUILD_DIR)/test_interleave_cal
	$<

# Waveform pass/fail masks (docs/specs/scope/mask-pass-fail.md): real records from
# unit #1, aligned with the firmware's own trig_edge_anchor(); every acceptance
# has a negative control that must come out the other way.
$(BUILD_DIR)/test_mask_pf: tests/test_mask_pf.c src/dsp/mask_pf.c src/dsp/mask_pf.h \
                           src/dsp/trig_edge.c src/dsp/trig_edge.h \
                           tests/fixtures/mask_pf_records.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Isrc/dsp -Itests \
		$< src/dsp/mask_pf.c src/dsp/trig_edge.c -lm -o $@

test-mask-pf: $(BUILD_DIR)/test_mask_pf
	$<

# Signal generator (parity G1/G3/G4): every 100-sample cycle against stock
# V1.2.0's builder run in unicorn (fixture generated by
# scripts/gen_siggen_stock_waves.py), TMR7 DIV/PR at every integer
# frequency vs stock's formula, and the DAC_CTRL composer's channel-1
# pass-through. See tests/test_siggen.c.
$(BUILD_DIR)/test_siggen: tests/test_siggen.c src/dsp/siggen_wave.c src/dsp/siggen_wave.h \
                          tests/fixtures/siggen_stock_waves.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Isrc/dsp -Itests \
		$< src/dsp/siggen_wave.c -lm -o $@

test-siggen: $(BUILD_DIR)/test_siggen
	$<

# DAC owner (src/drivers/dac.c) write log: the fake register block
# (-DHOST_REGS) proves the boot store sequence is the one the drivers made
# before the owner, a CH1 offset write beside the running generator leaves
# channel 2 alone, and generator start/stop leaves channel 1 alone.
# See tests/test_dac.c.
$(BUILD_DIR)/test_dac: tests/test_dac.c src/drivers/dac.c src/drivers/dac.h \
                       src/drivers/scope_trigger.c src/drivers/scope_trigger.h \
                       src/dsp/siggen_wave.c src/dsp/siggen_wave.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror -DHOST_REGS -Isrc/drivers -Isrc/dsp \
		$< src/drivers/dac.c src/drivers/scope_trigger.c src/dsp/siggen_wave.c -lm -o $@

test-dac: $(BUILD_DIR)/test_dac
	$<

# Generator frequency digit editor (parity G4): the pure state machine in
# src/ui/siggen_edit.c, no hardware. See tests/test_siggen_edit.c.
$(BUILD_DIR)/test_siggen_edit: tests/test_siggen_edit.c src/ui/siggen_edit.c src/ui/siggen_edit.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O1 -Wall -Wextra -Werror -Isrc/ui $< src/ui/siggen_edit.c -o $@

.PHONY: test-siggen-edit
test-siggen-edit: $(BUILD_DIR)/test_siggen_edit
	$<

# System preferences (parity C15): brightness / sound level laws, the beep
# timer and the idle power-off state machine. Pure, no hardware.
$(BUILD_DIR)/test_sys_prefs: tests/test_sys_prefs.c src/util/sys_prefs.c src/util/sys_prefs.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O1 -Wall -Wextra -Werror -Isrc/util $< src/util/sys_prefs.c -o $@

.PHONY: test-sys-prefs
test-sys-prefs: $(BUILD_DIR)/test_sys_prefs

# Zoom while paused (parity C14): the ratio, vertical map and 16-tap
# interpolator of src/dsp/paused_view.c, against the stock dumps and formulas.
# Since the C5 merge it also pins the one window plan and that pv_fill at
# ratio 1000/L IS scope_geom_fill (one kernel, sinc_interp.c, for both zooms).
$(BUILD_DIR)/test_paused_view: tests/test_paused_view.c src/dsp/paused_view.c src/dsp/paused_view.h \
                               src/dsp/sinc_interp.c src/dsp/sinc_interp.h \
                               src/ui/scope_geom.c src/ui/scope_geom.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O1 -Wall -Wextra -Werror -Isrc/dsp -Isrc/ui $< src/dsp/paused_view.c \
		src/dsp/sinc_interp.c src/ui/scope_geom.c -lm -o $@

.PHONY: test-paused-view
test-paused-view: $(BUILD_DIR)/test_paused_view
	$<

# Bode sweep maths (step frequency, gain/phase, -3 dB bandwidth) and
# bode_init(), which since the 2026-11-11 RAM diet fills a caller-owned result
# instead of a dead 2.4 KB module copy.
$(BUILD_DIR)/test_bode: tests/test_bode_mask.c src/dsp/bode.c src/dsp/bode.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O1 -Wall -Wextra -Werror -DTEST_BUILD -Isrc/dsp $< src/dsp/bode.c -lm -o $@

.PHONY: test-bode
test-bode: $(BUILD_DIR)/test_bode
	$<

# Shared pool sizing: built for the host (radix-2) layout AND for the
# USE_CMSIS_DSP layout every firmware image links, so both pool sizes and the
# waterfall offset are pinned (RAM diet, 2026-11-11).
$(BUILD_DIR)/test_shmem_layout: tests/test_shmem_layout.c src/dsp/shared_mem.c src/dsp/shared_mem.h src/dsp/fft.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O1 -Wall -Wextra -Werror -Isrc/dsp $< src/dsp/shared_mem.c -o $@
$(BUILD_DIR)/test_shmem_layout_cmsis: tests/test_shmem_layout.c src/dsp/shared_mem.c src/dsp/shared_mem.h src/dsp/fft.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O1 -Wall -Wextra -Werror -DUSE_CMSIS_DSP -Isrc/dsp $< src/dsp/shared_mem.c -o $@

.PHONY: test-shmem-layout
test-shmem-layout: $(BUILD_DIR)/test_shmem_layout $(BUILD_DIR)/test_shmem_layout_cmsis
	$(BUILD_DIR)/test_shmem_layout
	$(BUILD_DIR)/test_shmem_layout_cmsis

.PHONY: all clean disasm size renode soak-test soak-quick flash flash-dfu bootloader flash-bootloader flash-all emu test-meter-data test-meter-auto test-fpga-meter-plan test-stock-meter-literals test-meter test-scope-measure test-scope-cal test-scope-timebase test-scope-freq test-redraw-gate test-fft-live test-trig-edge test-interleave test-mask-pf test-siggen test-fuse test-config-persist guest-caldump guest-coldtrace-la guest-coldtrace-meter test-key-model test-can-bus test-interleave-cal
# Stock calibration page (parity C4): decode, the S12 validation rules, bank
# substitution and the offset-DAC formula against stock's live SWD readings
# on unit #2 (tests/fixtures/calpage_unit2.h), unit #1's clean page
# (src/util/cal_reference.c) as the control, and the gain hand-off to
# scope_cal. See tests/test_calpage.c.
$(BUILD_DIR)/test_calpage: tests/test_calpage.c src/util/calpage.c src/util/calpage.h \
                           src/util/cal_reference.c src/ui/scope_cal.c src/ui/scope_cal.h \
                           tests/fixtures/calpage_unit2.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Isrc/util -Isrc/ui -Itests \
		$< src/util/calpage.c src/util/cal_reference.c src/ui/scope_cal.c -lm -o $@

.PHONY: test-calpage
test-calpage: $(BUILD_DIR)/test_calpage
	$<

# Key model: the long-press state machine the TMR3 ISR runs, and the
# key -> action map the input task asks (src/ui/key_model.h). Both are pure.
$(BUILD_DIR)/test_key_model: tests/test_key_model.c src/ui/key_model.c src/ui/key_model.h \
                             src/ui/scope_state.c src/ui/scope_state.h src/ui/scope_roll.c | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Isrc/ui -Isrc/dsp $< \
		src/ui/key_model.c src/ui/scope_state.c src/ui/scope_roll.c -o $@

.PHONY: test-key-model
test-key-model: $(BUILD_DIR)/test_key_model
	$<

# Home screen (stock's mode-selection page, ms[0xF68] == 0): the pure model
# the input task drives (src/ui/home_model.h): stock's item order, LEFT/RIGHT
# wrap, OK -> mode, the preselection on entry, BACK, and the startup policy's
# home/siggen choices (startup_policy.h).
$(BUILD_DIR)/test_home_model: tests/test_home_model.c src/ui/home_model.c src/ui/home_model.h \
                              src/ui/startup_policy.h src/ui/ui.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Isrc/ui -Isrc/util -Isrc/dsp $< src/ui/home_model.c -o $@

.PHONY: test-home-model
test-home-model: $(BUILD_DIR)/test_home_model
	$<

# R1-A: the Oscilloscope Settings page model, meas_select and the menu_frame
# scroll rule (tests/test_osc_menu.c); labels are measured in the real fonts.
$(BUILD_DIR)/test_osc_menu: tests/test_osc_menu.c $(R1A_OSC_MENU_TEST_SRCS) src/ui/osc_menu_model.h \
                            src/ui/meas_select.h src/ui/menu_frame.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Isrc/ui -Isrc/dsp -Isrc/fonts $< \
		$(R1A_OSC_MENU_TEST_SRCS) -lm -o $@

.PHONY: test-osc-menu
test-osc-menu: $(BUILD_DIR)/test_osc_menu
	$<

# UI round 1, R1-B: bar glyphs (src/ui/icons.h): every glyph inside its box,
# no row past its width, shapes (edge direction, battery bars, the 13 waves
# from the DAC tables), and the status/info bar geometry of status_bar.h
# (slots inside 320 px, no overlap, the widest strings fit in font_small).
$(BUILD_DIR)/test_icons: tests/test_icons.c src/ui/icons.c src/ui/icons.h src/ui/status_bar.h \
                         src/dsp/siggen_wave.c src/dsp/siggen_wave.h src/fonts/font_small.c | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Isrc/ui -Isrc/dsp -Isrc/fonts $< \
		src/ui/icons.c src/dsp/siggen_wave.c src/fonts/font_small.c -o $@

# Fonts: struct integrity, widths, clipping, and font_text_px() (the
# per-pixel text the plot's elements use) == what font_draw_string() sets.
$(BUILD_DIR)/test_font: tests/test_font.c src/fonts/font.c src/fonts/font.h \
                        src/fonts/font_small.c src/fonts/font_medium.c \
                        src/fonts/font_large.c src/fonts/font_xlarge.c | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Isrc/fonts -Isrc/drivers $< src/fonts/font.c \
		src/fonts/font_small.c src/fonts/font_medium.c src/fonts/font_large.c \
		src/fonts/font_xlarge.c -o $@

.PHONY: test-font
test-font: $(BUILD_DIR)/test_font
	$<

.PHONY: test-icons
test-icons: $(BUILD_DIR)/test_icons
	$<

# Roll mode (parity C11): stock's step table (ARR 0x080465A4), the 301-point
# FIFO, the timebase ladder into roll and back, the 25-points-per-division
# geometry, and the amplitude correction at /150 with no m >= 221 gate.
# Also: the roll trigger (stock 0x0803EC2E..0x0803ED18, pair 149/150 of a full
# FIFO, mode and edge rules), the time-ordered FIFO, and the 12 measurements
# on the roll point rate (scope_roll_meas.c).
$(BUILD_DIR)/test_scope_roll: tests/test_scope_roll.c src/ui/scope_roll.c src/ui/scope_roll.h \
                              src/ui/scope_roll_meas.c src/ui/scope_roll_meas.h \
                              src/ui/scope_measure.c src/ui/scope_cursor.c src/ui/scope_timebase.c \
                              src/ui/scope_cal.c src/ui/scope_cal.h src/ui/scope_measure.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Isrc/ui $< \
		src/ui/scope_roll.c src/ui/scope_roll_meas.c src/ui/scope_measure.c \
		src/ui/scope_cursor.c src/ui/scope_timebase.c src/ui/scope_cal.c -lm -o $@

.PHONY: test-scope-roll
test-scope-roll: $(BUILD_DIR)/test_scope_roll
	$<

# Autoset, baseline correction and 50 % centre (parity C9/C10): the pure
# engines, on simulated channels (servo convergence, the refusals, autoset
# decisions on synthetic records), against the real nominal ladder and rate
# table. See tests/test_scope_selfcal.c.
$(BUILD_DIR)/test_scope_selfcal: tests/test_scope_selfcal.c src/ui/scope_selfcal.c src/ui/scope_selfcal.h \
                                 src/ui/scope_cal.c src/ui/scope_cal.h \
                                 src/ui/scope_timebase.c src/ui/scope_timebase.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Isrc/ui $< \
		src/ui/scope_selfcal.c src/ui/scope_cal.c src/ui/scope_timebase.c -lm -o $@

.PHONY: test-scope-selfcal
test-scope-selfcal: $(BUILD_DIR)/test_scope_selfcal
	$<

# On-device screenshots and gallery (parity C16): slot layout inside the
# scratch region, 4-bpp BMP format, <seq>.bmp naming, save/delete/reuse and
# the 3x3 grid keys, against the real region layer on a NOR model. Asserts
# byte for byte that saving never touches flash outside scratch (the cal
# backup at 0xFFD000, the user-cal log at 0xF00000). See tests/test_gallery.c.
$(BUILD_DIR)/test_gallery: tests/test_gallery.c src/util/gallery_store.c src/util/gallery_store.h \
                           src/ui/gallery_nav.c src/ui/gallery_nav.h src/drivers/flash_regions.c \
                           src/drivers/flash_regions.h src/util/cal_backup_core.c | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Isrc/util -Isrc/ui -Isrc/drivers $< \
		src/util/gallery_store.c src/ui/gallery_nav.c src/drivers/flash_regions.c \
		src/util/cal_backup_core.c -o $@

.PHONY: test-gallery
test-gallery: $(BUILD_DIR)/test_gallery
	$<

# Boot logo (parity C15): record format, header CRC, sequential upload, commit
# last, CRC refusal, streaming, and that an upload touches nothing outside the
# logo region. See tests/test_logo.c.
$(BUILD_DIR)/test_logo: tests/test_logo.c src/util/logo_store.c src/util/logo_store.h \
                        src/drivers/flash_regions.c src/drivers/flash_regions.h \
                        src/util/cal_backup_core.c | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Isrc/util -Isrc/drivers $< \
		src/util/logo_store.c src/drivers/flash_regions.c \
		src/util/cal_backup_core.c -o $@

.PHONY: test-logo
test-logo: $(BUILD_DIR)/test_logo

# FPGA design slot (fpga_slot.h): record format, header CRC, sequential upload,
# commit last, CRC refusal, verify, and that an upload touches nothing outside
# the slot's span. Synthetic payloads only. See tests/test_fpga_slot.c.
$(BUILD_DIR)/test_fpga_slot: tests/test_fpga_slot.c src/util/fpga_slot.c src/util/fpga_slot.h \
                        src/drivers/flash_regions.c src/drivers/flash_regions.h \
                        src/util/cal_backup_core.c | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Isrc/util -Isrc/drivers $< \
		src/util/fpga_slot.c src/drivers/flash_regions.c \
		src/util/cal_backup_core.c -o $@

# The trigger edge register rule (dsp/trig_edge_reg.h): never to a design that
# is not ours. See tests/test_trig_edge_reg.c.
$(BUILD_DIR)/test_trig_edge_reg: tests/test_trig_edge_reg.c src/dsp/trig_edge_reg.h src/drivers/fpga_core_regs.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Isrc/dsp -Isrc/drivers $< -o $@

.PHONY: test-trig-edge-reg
test-trig-edge-reg: $(BUILD_DIR)/test_trig_edge_reg
	$(BUILD_DIR)/test_trig_edge_reg

# The pin sweep's board data (src/re/pin_sweep_table.h): no candidate is a pin
# that must never be pulsed (power hold first). See tests/test_re_pin_sweep.c.
$(BUILD_DIR)/test_re_pin_sweep: tests/test_re_pin_sweep.c src/re/pin_sweep_table.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror $< -o $@

.PHONY: test-re-pin-sweep
test-re-pin-sweep: $(BUILD_DIR)/test_re_pin_sweep
	$(BUILD_DIR)/test_re_pin_sweep

# Identity of our capture core (dsp/core_ident.h, op E2). See tests/test_core_ident.c.
$(BUILD_DIR)/test_core_ident: tests/test_core_ident.c src/dsp/core_ident.h src/drivers/fpga_core_regs.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Isrc/dsp -Isrc/drivers $< -o $@

.PHONY: test-core-ident
test-core-ident: $(BUILD_DIR)/test_core_ident
	$(BUILD_DIR)/test_core_ident

# The acquisition's waits, named (dsp/acq_timing.h). See tests/test_acq_timing.c.
$(BUILD_DIR)/test_acq_timing: tests/test_acq_timing.c src/dsp/acq_timing.h src/drivers/fpga_core_regs.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Isrc/dsp -Isrc/drivers $< -o $@

.PHONY: test-acq-timing
test-acq-timing: $(BUILD_DIR)/test_acq_timing
	$(BUILD_DIR)/test_acq_timing

# Status block of our capture core (dsp/core_status.h, op E0). See tests/test_core_status.c.
$(BUILD_DIR)/test_core_status: tests/test_core_status.c src/dsp/core_status.h src/drivers/fpga_core_regs.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Isrc/dsp -Isrc/drivers $< -o $@

.PHONY: test-core-status
test-core-status: $(BUILD_DIR)/test_core_status
	$(BUILD_DIR)/test_core_status

# drivers/fpga_core_regs.h must be what the capture core's package generates (fpga/core/rtl/core_pkg.sv).
.PHONY: test-core-regs-header
test-core-regs-header:
	python3 -I ../fpga/core/tools/gen_c_header.py --check src/drivers/fpga_core_regs.h

.PHONY: test-fpga-slot
test-fpga-slot: $(BUILD_DIR)/test_fpga_slot
	$<

# USB Sharing (parity C16): the read-only FAT12 volume synthesised from the
# gallery (boot sector, FAT12 packing and chains, directory entries, file
# read-through at arbitrary LBAs checked by an independent FAT reader against
# shots saved through the real gallery_store on the NOR model) and the SCSI /
# bulk-only layer (CBW validation, medium states, write refusal, the 13 BOT
# cases). VFAT_IMAGE_OUT=<file> also dumps the image. See tests/test_msc.c.
$(BUILD_DIR)/test_msc: tests/test_msc.c src/util/gallery_vfat.c src/util/gallery_vfat.h \
                       src/util/msc_scsi.c src/util/msc_scsi.h src/util/gallery_store.c \
                       src/util/gallery_store.h src/drivers/flash_regions.c \
                       src/drivers/flash_regions.h src/util/cal_backup_core.c | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Isrc/util -Isrc/drivers $< \
		src/util/gallery_vfat.c src/util/msc_scsi.c src/util/gallery_store.c \
		src/drivers/flash_regions.c src/util/cal_backup_core.c -o $@

.PHONY: test-msc
test-msc: $(BUILD_DIR)/test_msc
	$<

# USB device layer (tests/test_usb.c): the composite descriptor bytes against a
# strict structural checker (+ mutants), the usb_conf.h packet-memory layout,
# the endpoint-register CTR race fix on an rc_w0 model, the USB mode / fallback
# state machine and the `logo blk` raw receiver. Then the cross-compiler
# preprocesses the AT32 library's usbd_int.c with the firmware's flags and
# checks that the CTR clears it compiles are usb_conf.h's fixed ones.
$(BUILD_DIR)/test_usb: tests/test_usb.c src/util/usb_comp_desc.c src/util/usb_comp_desc.h \
                       src/util/usb_mode_sm.c src/util/usb_mode_sm.h src/util/raw_block.c \
                       src/util/raw_block.h src/util/cal_backup_core.c src/usb_conf.h \
                       tests/stubs_usb/at32f403a_407.h tests/stubs_usb/at32f403a_407_usb.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Itests/stubs_usb -Isrc/util -Isrc $< \
		src/util/usb_comp_desc.c src/util/usb_mode_sm.c src/util/raw_block.c \
		src/util/cal_backup_core.c -o $@

.PHONY: test-usb
test-usb: $(BUILD_DIR)/test_usb
	$<
	@$(CC) -E $(CFLAGS) $(AT32_USBD)/src/usbd_int.c -o $(BUILD_DIR)/usbd_int.i
	@grep -q '0x7FFFu' $(BUILD_DIR)/usbd_int.i && grep -q '0xFF7Fu' $(BUILD_DIR)/usbd_int.i \
		|| { echo "test-usb: usbd_int.c does not compile usb_conf.h's CTR clears"; exit 1; }
	@! grep -Eq '&= *0x7FFF|&= *0xFF7F' $(BUILD_DIR)/usbd_int.i \
		|| { echo "test-usb: usbd_int.c still compiles the library's racy CTR clears"; exit 1; }
	@echo "test-usb: usbd_int.c compiles the fixed CTR clears"


# OpenScope's persisted calibration corrections (baseline deltas + interleave
# B correction): the v2 record format (line, knots, skew), v1 migration, its
# CRC, the version gate, corrupt and foreign records, the page fingerprint, and
# save/load/compaction through the real region layer on a NOR model with power
# cuts. See tests/test_usercal.c.
$(BUILD_DIR)/test_usercal: tests/test_usercal.c src/util/usercal_rec.c src/util/usercal_rec.h \
                           src/util/cal_backup_core.c src/util/cal_backup_core.h \
                           src/util/cal_reference.c src/drivers/flash_regions.c src/drivers/flash_regions.h \
                           src/dsp/interleave.c src/dsp/interleave.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Isrc/util -Isrc/drivers $< \
		src/util/usercal_rec.c src/util/cal_backup_core.c src/util/cal_reference.c src/drivers/flash_regions.c \
		src/dsp/interleave.c -o $@

.PHONY: test-usercal
test-usercal: $(BUILD_DIR)/test_usercal
	$<

# UI round 1, R1-C: the measurement strip (no cell overflows its box for the
# widest value the real formatter prints, in the real font_small) and the
# plot-edge marker shapes/placement.
$(BUILD_DIR)/test_measure_strip: tests/test_measure_strip.c src/ui/measure_strip.c src/ui/measure_strip.h \
                                 src/ui/plot_markers.c src/ui/plot_markers.h src/ui/scope_measure.c \
                                 src/ui/scope_cursor.c src/ui/scope_cal.c src/ui/scope_timebase.c \
                                 src/fonts/font.c src/fonts/font_small.c | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Isrc/ui -Isrc/fonts -Isrc/drivers $< \
		src/ui/measure_strip.c src/ui/plot_markers.c src/ui/scope_measure.c src/ui/scope_cursor.c \
		src/ui/scope_cal.c src/ui/scope_timebase.c src/fonts/font.c src/fonts/font_small.c -lm -o $@

.PHONY: test-measure-strip
test-measure-strip: $(BUILD_DIR)/test_measure_strip
	$<

# ═══════════════════════════════════════════════════════════════════════════
# BENCH IMAGE INDEX                (index added 2026-08-13, cut down 2026-11-17)
#
# Everything below this line is a named image. Until 2026-11-17 there was one
# target per experiment of the four months spent on FPGA config entry, kept so
# that every claim in the write-ups could be re-run. The firmware now drives
# only OpenScope's own capture core, and the targets that existed to repeat an
# answered question on the factory FPGA design were removed (git history has
# them; the write-ups stay). What is left:
#
#   app-coldtrace-meter    the daily bench image: cold boot -> configured FPGA
#                          -> live capture, meter submodes, bench profile
#   app-full               the user image (profile `full`)
#   app-rtt                app-coldtrace-meter, debug shell on RTT only
#   app-re                 app-coldtrace-meter plus the reverse-engineering
#                          toolkit (profile `re`, src/re/, docs/re/README.md)
#   guest-coldtrace-meter  app-coldtrace-meter linked for the
#                          factory bootloader
#   guest-coldtrace-la     the same, bit-bang config
#                          SCK slowed for 24 MS/s logic analyzers
#   guest-caldump          read-only dump of the saved-config sector
#
# A guest-* image runs at 0x08007000 under the FNIRSI factory bootloader and
# is flashed through its IAP drive: main.c moves VTOR and skips the watchdog
# when GUEST_BUILD is defined. An app-* image has the HID-bootloader layout.
#
# Full writeups: CLAUDE.md,
# reverse_engineering/analysis_v120/expE_swd_state_diff_2026-07-28.md,
# docs/build_a_config_entry_test.md, docs/fpga_warm_handoff_test.md.
# ═══════════════════════════════════════════════════════════════════════════

# [LIVE] guest-caldump — READ-ONLY diagnostic. Reports MCU flash
# 0x08006000..0x08006FFF, the saved-config sector that is stock's candidate home
# for per-device calibration. Nobody has ever read it on any unit: RDP blocks
# external debuggers, but the CPU reads its own flash normally, so the firmware
# can do what the debugger cannot.
# Writes NOTHING. Links at 0x08007000 so it cannot overlap the region it reads.
# Runs immediately after LCD bring-up and never returns — no FPGA init, no
# scheduler. Report the CRC32 it prints. See src/util/cal_dump.h.
guest-caldump: CFLAGS += -DGUEST_BUILD -DCAL_DUMP_MODE=1
guest-caldump: LDSCRIPT = ld/at32f403a_guest.ld
guest-caldump: clean all

# app-coldtrace-meter: the daily bench image. Cold boot -> the bit-bang config
# loads the capture core -> live capture, in the HID-bootloader layout (vectors
# 0x08004000, code 0x08007000, 0x08006000 left for the calibration page), so
# it runs under OpenScope's bootloader and updates over USB without the IAP
# drive. Plain `make` builds the same image without cleaning first.
# There are no hardware-variant flags any more: every image boots the same
# way (bit-bang configuration of the built-in core, USART2 for the meter
# chip only, all meter functions).
app-coldtrace-meter: clean all

# app-full: the user image. The same code path as app-coldtrace-meter, the
# profile `full` (features.mk): every user-facing feature, without the bench
# shell and the meter diagnostics.
app-full: clean all

# app-rtt: app-coldtrace-meter with the debug shell on RTT only
# (DEBUG_SHELL_RTT_ONLY, 2026-08-11), which cuts the USB CDC half out of the
# console. Measured on the bench unit: usbd_connect_state_get() reports
# CONFIGURED even though CDC never enumerates (error -71), so usb_send_bytes()
# enters the CDC TX path and the shell task stalls there instead of returning
# to poll RTT -- the banner reached the RTT ring (rtt_write runs first) and
# nothing followed. RTT exists to bypass that transport; this stops the tee
# re-introducing it. It needs a debugger attached, so it is of no use on a
# unit with read-out protection set (bench unit #1: attaching kills the CPU).
.PHONY: app-rtt
app-rtt: CFLAGS += -DDEBUG_SHELL_RTT_ONLY=1
app-rtt: clean all

# app-re: app-coldtrace-meter plus the reverse-engineering toolkit (profile
# `re`, features.mk): shell commands that pulse pins and probe buses to find
# out how a board works (`re pinsweep`: which MCU pin reloads the FPGA).
# Not for daily use; docs/re/README.md is the guide.
.PHONY: app-re
app-re: clean all

# guest-coldtrace-meter: app-coldtrace-meter linked for the factory bootloader
# (GUEST_BUILD: vectors moved to 0x08007000, no watchdog; default profile).
guest-coldtrace-meter: CFLAGS += -DGUEST_BUILD
guest-coldtrace-meter: LDSCRIPT = ld/at32f403a_guest.ld
guest-coldtrace-meter: clean all

# [LA 2026-08-15] guest-coldtrace-la -- the bit-bang config clock slowed
# (FPGA_BB_HALF_DELAY=60 nops per half period, ~1-2 MHz SCK) so a 24 MS/s
# logic analyzer can decode the whole config sequence (issue #18: maksidze's
# captures of the undelayed loop were aliased). Otherwise guest-coldtrace-meter.
# A reverse-engineering build knob: docs/re/README.md.
guest-coldtrace-la: CFLAGS += -DGUEST_BUILD -DFPGA_BB_HALF_DELAY=60
guest-coldtrace-la: LDSCRIPT = ld/at32f403a_guest.ld
guest-coldtrace-la: clean all

# Register-owner gate (ARCH round, task A0). Host-only: scans src/**/*.c for writes to
# peripheral registers outside their owner (scripts/reg_owners.txt) and fails on any write
# the baseline does not cover. See docs/arch/owners.md. Not part of `all`.
# The state-owner ratchet (ARCH round, sibling of check-owners): a setting's
# backing variable is stored by its property's setter only. Counts the direct
# stores elsewhere (scripts/check_state_owners.py) against a baseline that
# only shrinks; a new one fails. Part of the gate, next to check-owners.
.PHONY: check-state-owners
check-state-owners:
	cd .. && python3 -I scripts/check_state_owners.py --self-test
	cd .. && python3 -I scripts/check_state_owners.py --src firmware/src

# The FPGA_ADC_OFFSET ratchet (ARCH round, typed units, C2): the raw-to-record
# offset is ADC_REC_OFFSET / rec_from_raw() in util/units.h; the old float
# alias in drivers/fpga.h stays for the capture paths not converted yet (C3).
# Counts its uses per file (scripts/check_adc_offset.py) against a baseline
# that only shrinks; a new use fails. test-adc-offset-ratchet puts it in
# test-all.
.PHONY: check-adc-offset test-adc-offset-ratchet
check-adc-offset:
	cd .. && python3 -I scripts/check_adc_offset.py --self-test
	cd .. && python3 -I scripts/check_adc_offset.py --src firmware/src

test-adc-offset-ratchet: check-adc-offset

.PHONY: check-owners
check-owners:
	cd .. && python3 -I scripts/check_reg_owners.py --self-test \
		--lib-inc firmware/at32f403a_lib/libraries/drivers/inc
	cd .. && python3 -I scripts/check_reg_owners.py --src firmware/src \
		--lib-inc firmware/at32f403a_lib/libraries/drivers/inc

# Screen core (src/ui/screen.h): the router order, overlays over a base, modal
# capture, close/switch and the `ui screen` snapshot; then the firmware's
# table (src/ui/ui_router.c) with every real screen on the real model: the
# stack as the authority, the mode switch, the global keys from every screen
# and layer, the self-cal lock, the repaint and the settings service.
ROUTER_TEST_SCREENS = src/ui/ui_router.c src/ui/home_screen.c src/ui/home_model.c src/ui/settings_screen.c \
                      src/ui/siggen_screen.c src/ui/siggen_edit.c src/ui/meter_screen.c src/ui/softkey.c \
                      src/ui/fuse_model.c src/ui/scope_screen.c src/ui/osc_menu_screen.c src/ui/gallery_screen.c \
                      src/ui/gallery_nav.c src/ui/overlay_screens.c
$(BUILD_DIR)/test_screen_router: tests/test_screen_router.c tests/fw_screens_env.h $(ROUTER_TEST_SCREENS) src/ui/ui_router.h \
                                 src/ui/redraw_gate.h $(SCREEN_TEST_DEPS) | $(BUILD_DIR)
	$(SCREEN_TEST_CC) $< $(ROUTER_TEST_SCREENS) src/ui/screen.c \
		$(MODEL_SRCS) $(MODEL_TEST_LINK) -lm -o $@

.PHONY: test-screen-router
test-screen-router: $(BUILD_DIR)/test_screen_router
	$<

# A settings save is triggered by a changed persisted property, not by a key
# (ARCH-1 B8): every real screen on the real router and model, the real
# settings store and record codec on a NOR flash model. Every key on every
# screen: a changed record is written, an unchanged one is not; the write
# policy against capture-per-key; the boundary flushes.
NOTIFY_TEST_SRCS = src/util/config.c src/util/settings_store.c src/util/session_store.c src/drivers/flash_regions.c \
                   src/model/model_persist_v5.c
$(BUILD_DIR)/test_settings_notify: tests/test_settings_notify.c tests/fw_screens_env.h $(ROUTER_TEST_SCREENS) \
                                   $(NOTIFY_TEST_SRCS) src/util/settings_store.h $(SCREEN_TEST_DEPS) | $(BUILD_DIR)
	$(SCREEN_TEST_CC) $< $(ROUTER_TEST_SCREENS) src/ui/screen.c $(NOTIFY_TEST_SRCS) \
		$(MODEL_SRCS) $(MODEL_TEST_LINK) -lm -o $@

.PHONY: test-settings-notify
test-settings-notify: $(BUILD_DIR)/test_settings_notify
	$<

# Migrated screens (ARCH-1 B2 on): the real screen file on the real router
# and the real property model, with fake hardware ops (tests/
# screen_test_env.h). Home: every key, OK/MENU as one sys.instrument or
# sys.mode set, the wrap over the entry table, open()'s preselection.
SCREEN_TEST_DEPS = tests/screen_test_env.h src/ui/screen.c src/ui/screen.h $(MODEL_SRCS) \
                   src/model/model.h src/model/model_table.h $(MODEL_TEST_LINK)
SCREEN_TEST_CC   = $(HOST_CC) -std=gnu11 -O1 -Wall -Wextra -Werror -Isrc/model -Isrc/util \
                   -Isrc/ui -Isrc/dsp -Isrc/drivers

$(BUILD_DIR)/test_home_screen: tests/test_home_screen.c src/ui/home_screen.c src/ui/home_screen.h \
                               src/ui/home_model.c src/ui/home_model.h $(SCREEN_TEST_DEPS) | $(BUILD_DIR)
	$(SCREEN_TEST_CC) $< src/ui/home_screen.c src/ui/home_model.c src/ui/screen.c \
		$(MODEL_SRCS) $(MODEL_TEST_LINK) -lm -o $@

.PHONY: test-home-screen
test-home-screen: $(BUILD_DIR)/test_home_screen
	$<

# The generator screen on gen.* (ARCH-1 B3): the key table with the editor
# closed and open, every change as a property set (get, epoch, settings
# note), a `set` while the screen is up, and the dac_output calls of every
# key against the pre-B3 handlers over 750 generator states.
$(BUILD_DIR)/test_siggen_screen: tests/test_siggen_screen.c src/ui/siggen_screen.c src/ui/siggen_screen.h \
                                 src/ui/siggen_edit.c src/ui/siggen_edit.h $(SCREEN_TEST_DEPS) | $(BUILD_DIR)
	$(SCREEN_TEST_CC) $< src/ui/siggen_screen.c src/ui/siggen_edit.c src/ui/screen.c \
		$(MODEL_SRCS) $(MODEL_TEST_LINK) -lm -o $@

.PHONY: test-siggen-screen
test-siggen-screen: $(BUILD_DIR)/test_siggen_screen
	$<

# The Settings screens (ARCH-1 B4 / B8): the list, the question of Reset to
# Defaults, Sound and Light and About; every value through its property, the
# pages on the router's stack, and 6000 random keys against a transcription
# of the handlers they replace.
$(BUILD_DIR)/test_settings_screen: tests/test_settings_screen.c src/ui/settings_screen.c src/ui/settings_screen.h \
                                   src/ui/settings_reset.h src/ui/startup_policy.h $(SCREEN_TEST_DEPS) | $(BUILD_DIR)
	$(SCREEN_TEST_CC) $< src/ui/settings_screen.c src/ui/screen.c \
		$(MODEL_SRCS) $(MODEL_TEST_LINK) -lm -o $@

.PHONY: test-settings-screen
test-settings-screen: $(BUILD_DIR)/test_settings_screen
	$<

# The meter screen on meter.* (ARCH-1 B6): the key table on every softkey
# bar, every change as a property set, the View softkey and the fuse type
# keys against `set meter.layout` / `set meter.fusetype` and the pre-B6
# handlers, a `set` while the screen is up painted in place by the tick.
$(BUILD_DIR)/test_meter_screen: tests/test_meter_screen.c src/ui/meter_screen.c src/ui/meter_screen.h \
                                src/ui/softkey.c src/ui/softkey.h src/ui/fuse_model.c $(SCREEN_TEST_DEPS) | $(BUILD_DIR)
	$(SCREEN_TEST_CC) $< src/ui/meter_screen.c src/ui/softkey.c src/ui/fuse_model.c src/ui/screen.c \
		$(MODEL_SRCS) $(MODEL_TEST_LINK) -lm -o $@

.PHONY: test-meter-screen
test-meter-screen: $(BUILD_DIR)/test_meter_screen
	$<

# The Oscilloscope Settings page as a screen (ARCH-1 B5): the binding table,
# every row and item as one property set in both directions, the setters'
# rules and the footer notes, a `set` while the page is open painted in
# place, an idle page painting nothing, the router order, open/jump/close.
$(BUILD_DIR)/test_osc_menu_screen: tests/test_osc_menu_screen.c src/ui/osc_menu_screen.c \
                                   src/ui/osc_menu_screen.h src/ui/osc_menu_model.h $(SCREEN_TEST_DEPS) | $(BUILD_DIR)
	$(SCREEN_TEST_CC) $< src/ui/osc_menu_screen.c src/ui/screen.c \
		$(MODEL_SRCS) $(MODEL_TEST_LINK) -lm -o $@

.PHONY: test-osc-menu-screen
test-osc-menu-screen: $(BUILD_DIR)/test_osc_menu_screen
	$<

# The scope screen on chN.* / tb.* / trig.* / view.* / mask.* (ARCH-1 B7):
# every key in every arrow stage and view against the pre-B7 handlers over
# random states, each change as a property set, a key against the direct
# route's `set`, RUN/STOP and SINGLE, the mask stage, the two display epochs.
$(BUILD_DIR)/test_scope_screen: tests/test_scope_screen.c src/ui/scope_screen.c src/ui/scope_screen.h \
                                src/ui/osc_menu_screen.c src/ui/osc_menu_screen.h $(SCREEN_TEST_DEPS) | $(BUILD_DIR)
	$(SCREEN_TEST_CC) $< src/ui/scope_screen.c src/ui/osc_menu_screen.c src/ui/screen.c \
		$(MODEL_SRCS) $(MODEL_TEST_LINK) -lm -o $@

.PHONY: test-scope-screen
test-scope-screen: $(BUILD_DIR)/test_scope_screen
	$<

# The screenshot gallery as a screen (ARCH-1 B5): navigation, arm / delete
# against the real store on a NOR model, closing, the GRAB order (first,
# never POWER), the epoch and the paint plan the display task executes.
GALLERY_SCREEN_TEST_SRCS = src/ui/gallery_screen.c src/ui/gallery_nav.c src/ui/screen.c src/ui/theme.c \
                           src/util/gallery_store.c src/drivers/flash_regions.c src/util/cal_backup_core.c
$(BUILD_DIR)/test_gallery_screen: tests/test_gallery_screen.c $(GALLERY_SCREEN_TEST_SRCS) \
                                  src/ui/gallery_screen.h src/ui/gallery_nav.h src/ui/screen.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O1 -Wall -Wextra -Werror -Isrc/ui -Isrc/util -Isrc/drivers -Isrc/dsp $< \
		$(GALLERY_SCREEN_TEST_SRCS) -o $@

.PHONY: test-gallery-screen
test-gallery-screen: $(BUILD_DIR)/test_gallery_screen
	$<

# The POWER prompt and the logo as layers (ARCH-1 B5): each show painted once
# by a display task that paints from the epoch, no keys for them or under
# them, POWER to the globals, and what is repainted when a layer goes.
$(BUILD_DIR)/test_overlay_screens: tests/test_overlay_screens.c src/ui/overlay_screens.c \
                                   src/ui/overlay_screens.h src/ui/screen.c src/ui/screen.h src/ui/theme.c | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O1 -Wall -Wextra -Werror -Isrc/ui -Isrc/dsp $< src/ui/overlay_screens.c \
		src/ui/screen.c src/ui/theme.c -o $@

.PHONY: test-overlay-screens
test-overlay-screens: $(BUILD_DIR)/test_overlay_screens
	$<

# Capture configuration (src/dsp/acq_cfg.c): every field counts as a change,
# the settle window (fill + poll gap + settle) after one, a change between
# the notes around a read, slow timebases, tick wrap, and which buffers hold
# their own channel.
$(BUILD_DIR)/test_acq_cfg: tests/test_acq_cfg.c src/dsp/acq_cfg.c src/dsp/acq_cfg.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O1 -Wall -Wextra -Werror $< src/dsp/acq_cfg.c -o $@

.PHONY: test-acq-cfg
test-acq-cfg: $(BUILD_DIR)/test_acq_cfg
	$<

# Stream frame encoder (src/stream/stream_frame.c, FEATURE_STREAM): header and
# CRCs, packet chunking and the pad rule, a sink that refuses mid-frame, every
# payload's length and bytes, PROPS from a property list. It also regenerates
# the cross-check vectors that `scripts/scope_stream.py --selftest` decodes
# (tests/fixtures/stream/) and fails if they differ from the committed ones;
# STREAM_VECTORS_UPDATE=1 rewrites them.
$(BUILD_DIR)/test_stream: tests/test_stream.c src/stream/stream_frame.c src/stream/stream_frame.h | $(BUILD_DIR)
	$(HOST_CC) -std=gnu11 -O1 -Wall -Wextra -Werror -DFEATURE_STREAM=1 -Isrc/stream $< \
		src/stream/stream_frame.c -o $@

.PHONY: test-stream
test-stream: $(BUILD_DIR)/test_stream
	$<

# The host side of the stream (scripts/scope_stream.py, scripts/scope_broker.py):
# demultiplexer and decoders against the C encoder's vectors above (protocol
# 1.3) and the frozen 1.1 vectors (fixtures/stream/v1_1), the HELLO feature
# mask, and the broker against a fake device that speaks 1.2 and then 1.1.
.PHONY: test-stream-host
test-stream-host:
	cd .. && python3 -I scripts/scope_stream.py --selftest
	cd .. && python3 -I scripts/scope_broker.py --selftest

# test-all: every host test, from scratch. Stale binaries are removed first so
# a test that no longer builds cannot pass on an old executable. All tests run
# even after a failure; the target fails if any did, and names them.
# Left out on purpose: the aggregates (test-meter runs targets listed here) and
# test-meter-word-map (documented above as expected to fail until the issue
# #15 word-table correction lands).
TEST_ALL_SKIP := test-all test-meter test-meter-word-map
TEST_ALL_TARGETS = $(filter-out $(TEST_ALL_SKIP),$(sort $(shell sed -n 's/^\(test-[a-z0-9-]*\):.*/\1/p' $(firstword $(MAKEFILE_LIST)))))

.PHONY: test-all
test-all:
	@rm -f $(BUILD_DIR)/test_*
	@fail=""; n=0; \
	for t in $(TEST_ALL_TARGETS); do \
	  n=$$((n+1)); echo "=== $$t"; \
	  $(MAKE) --no-print-directory $$t || fail="$$fail $$t"; \
	done; \
	if [ -n "$$fail" ]; then echo "test-all: FAILED:$$fail"; exit 1; fi; \
	echo "test-all: $$n targets passed"
