# features.mk - the feature manifest: what an image is built with.
#
# One block per feature, always the same keys. The Makefile reads this file
# once; the size report (scripts/features_report.py) and the host scripts
# (scripts/scope_stream.py: HELLO feature bits) read it too, so keep every
# line in the plain form `NAME_key := value`.
#
#   FEATURE_LIST += NAME
#   NAME_desc      one line for `features` / `make features` (no quotes, no commas)
#   NAME_default   0/1 in a plain `make`
#   NAME_requires  features that must also be on   (upper-case names)
#   NAME_conflicts features that must be off
#   NAME_srcs      sources compiled only when the feature is on
#   NAME_incs      include paths, added in EVERY build (the feature's header
#                  supplies the inline no-ops and empty row macros when off,
#                  so core files include it without an #if)
#   NAME_defs      extra compiler defines when on
#   NAME_shared    for the size report: the feature's symbols inside shared
#                  objects, `object.o:regex` (space separated)
#   NAME_bit       number in the HELLO feature mask and in `features`.
#                  NEVER reused, never renumbered; a retired feature keeps
#                  its number.
#
# Every FEATURE_<NAME> is always defined, as 0 or 1. Code tests `#if
# FEATURE_<NAME>`, never `#ifdef`; -Wundef makes a misspelt switch a warning.
#
# Choosing:   make WITH="stream" WITHOUT="usbdrive"     (lower or upper case)
#             make PROFILE=bench
#             a named target takes its profile from PROFILE_OF_<target> below.
# Order of precedence: default < profile < WITH/WITHOUT on the command line.
# The feature set is fixed while the Makefile is read: an off feature's
# sources are not compiled at all.
#
# Adding a feature (the whole recipe):
#   1. a block here (next free NAME_bit);
#   2. its sources in its own files, listed in NAME_srcs;
#   3. one header that declares the feature when `#if FEATURE_<NAME>` and
#      otherwise supplies `static inline` no-ops and empty `<NAME>_SHELL_ROWS`
#      / `<NAME>_SCREEN_ROWS` macros (template: src/stream/stream.h);
#   4. core code includes that header and calls / places the rows with no #if;
#   5. `make features-ab FEATURE=name`, and write the result on the block.
#
# Tools:  make features          the manifest and what this invocation enables
#         make features-report   per-feature flash/RAM of the image in build/
#         make features-ab FEATURE=name [AB_TARGET=app-coldtrace-meter]

FEATURE_LIST :=

# ── stream ────────────────────────────────────────────────────────────
# A/B 2026-11-14 (app-coldtrace-meter): 5,784 B flash, 152 B RAM.
FEATURE_LIST += STREAM
STREAM_desc      := Binary frames to the host on the shell USB channel
STREAM_default   := 0
STREAM_requires  :=
STREAM_conflicts :=
STREAM_srcs      := src/stream/stream.c src/stream/stream_frame.c
STREAM_incs      := -Isrc/stream
STREAM_defs      :=
STREAM_shared    := usb_debug.o:cmd_stream|usb_debug_stream_
STREAM_bit       := 0

# ── FFT, waterfall, split view ────────────────────────────────────────
# Does NOT build when off today (main.c uses names ui.h declares only with
# the FFT); making the off state build is a later task. CMSIS-DSP is used
# when the library is present (auto-detected).
FEATURE_LIST += FFT
FFT_desc      := FFT and waterfall and split views
FFT_default   := 1
FFT_requires  :=
FFT_conflicts :=
FFT_srcs      := src/dsp/fft.c src/dsp/fft_live.c src/dsp/fft_source.c src/dsp/fft_test_signals.c
FFT_incs      :=
FFT_defs      :=
FFT_shared    := scope_ui.o:fft_|waterfall|draw_split main.o:fft_|waterfall arm_bitreversal2.o:.
FFT_bit       := 1
CMSIS_DSP = CMSIS-DSP
ifneq ($(wildcard $(CMSIS_DSP)/Include/arm_math.h),)
  FFT_srcs += $(CMSIS_DSP)/Source/TransformFunctions/arm_rfft_fast_f32.c
  FFT_srcs += $(CMSIS_DSP)/Source/TransformFunctions/arm_rfft_fast_init_f32.c
  FFT_srcs += $(CMSIS_DSP)/Source/TransformFunctions/arm_cfft_f32.c
  FFT_srcs += $(CMSIS_DSP)/Source/TransformFunctions/arm_cfft_init_f32.c
  FFT_srcs += $(CMSIS_DSP)/Source/CommonTables/arm_common_tables.c
  FFT_srcs += $(CMSIS_DSP)/Source/CommonTables/arm_const_structs.c
  FFT_srcs += $(CMSIS_DSP)/Source/TransformFunctions/arm_cfft_radix8_f32.c
  FFT_srcs += $(CMSIS_DSP)/Source/ComplexMathFunctions/arm_cmplx_mag_f32.c
  FFT_srcs += $(CMSIS_DSP)/Source/BasicMathFunctions/arm_scale_f32.c
  FFT_incs += -I$(CMSIS_DSP)/Include
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
  FFT_defs += -DUSE_CMSIS_DSP -DARM_MATH_CM4 $(FFT_TABLE_DEFS)
endif

# ── USB drive ─────────────────────────────────────────────────────────
# USB Sharing (parity C16). On: the unit BOOTS AS THE PLAIN CDC DEVICE (the
# library's cdc_desc.c: byte for byte the device of a build without this
# feature) and re-enumerates as CDC + a read-only mass-storage LUN synthesised
# from the screenshot gallery only when asked (`usb share on`, Settings "USB
# Sharing"). If the host does not configure the composite device within 3 s it
# comes back CDC-only by itself (src/util/usb_mode_sm.h), so the shell cannot
# be lost. Off leaves the composite code out entirely.
# usb_debug.c still tests the old name: the Makefile also passes
# -DUSB_MSC=<0|1> with the same value until that file is converted.
FEATURE_LIST += USBDRIVE
USBDRIVE_desc      := Gallery as a read-only USB disk (USB Sharing)
USBDRIVE_default   := 1
USBDRIVE_requires  :=
USBDRIVE_conflicts :=
USBDRIVE_srcs      := src/drivers/usb_composite.c src/drivers/usb_msc.c src/util/gallery_vfat.c \
                      src/util/msc_scsi.c src/util/usb_comp_desc.c src/util/usb_mode_sm.c
USBDRIVE_incs      :=
USBDRIVE_defs      :=
USBDRIVE_shared    := usb_debug.o:cmd_usb$$|usb_share|usbmode
USBDRIVE_bit       := 2

# ── profiles: what a named image contains ─────────────────────────────
# PROFILE_<name>_with / _without change the defaults above.
PROFILE_default_with    :=
PROFILE_default_without :=
# bench: the bench image, everything on.
PROFILE_bench_with      := stream
PROFILE_bench_without   :=

# Named targets and their profile (any target not listed: default).
PROFILE_OF_app-coldtrace-meter := bench

######################################################################
# Evaluation. Nothing below is per feature.
######################################################################
_fm_empty :=
_fm_space := $(_fm_empty) $(_fm_empty)
_fm_comma := ,
_fm_squote := '
_fm_dquote := "
_fm_lc = $(subst A,a,$(subst B,b,$(subst C,c,$(subst D,d,$(subst E,e,$(subst F,f,$(subst G,g,$(subst H,h,$(subst I,i,$(subst J,j,$(subst K,k,$(subst L,l,$(subst M,m,$(subst N,n,$(subst O,o,$(subst P,p,$(subst Q,q,$(subst R,r,$(subst S,s,$(subst T,t,$(subst U,u,$(subst V,v,$(subst W,w,$(subst X,x,$(subst Y,y,$(subst Z,z,$(1)))))))))))))))))))))))))))

_fm_goal_profiles := $(sort $(foreach g,$(MAKECMDGOALS),$(PROFILE_OF_$(g))))
ifneq ($(word 2,$(_fm_goal_profiles)),)
$(error features.mk: the goals name more than one profile ($(_fm_goal_profiles)); build them one at a time)
endif
ifneq ($(_fm_goal_profiles),)
PROFILE ?= $(_fm_goal_profiles)
endif
PROFILE ?= default
ifeq ($(origin PROFILE_$(PROFILE)_with),undefined)
$(error features.mk: unknown PROFILE '$(PROFILE)')
endif

# Old spellings, kept for one release: STREAM_BENCH=0, USB_MSC=0, FEATURES=
# (without the word FFT).
FEATURES ?= FFT
_fm_legacy_without :=
ifeq ($(strip $(STREAM_BENCH)),0)
_fm_legacy_without += stream
endif
ifeq ($(strip $(USB_MSC)),0)
_fm_legacy_without += usbdrive
endif
ifeq ($(findstring FFT,$(FEATURES)),)
_fm_legacy_without += fft
endif

_fm_names        := $(call _fm_lc,$(FEATURE_LIST))
_fm_cmd_with     := $(call _fm_lc,$(WITH))
_fm_cmd_without  := $(call _fm_lc,$(WITHOUT)) $(_fm_legacy_without)
_fm_prof_with    := $(call _fm_lc,$(PROFILE_$(PROFILE)_with))
_fm_prof_without := $(call _fm_lc,$(PROFILE_$(PROFILE)_without))
_fm_unknown := $(filter-out $(_fm_names),$(_fm_cmd_with) $(_fm_cmd_without) $(_fm_prof_with) $(_fm_prof_without))
ifneq ($(strip $(_fm_unknown)),)
$(error features.mk: unknown feature(s): $(_fm_unknown) (known: $(_fm_names)))
endif
ifneq ($(strip $(filter $(_fm_cmd_with),$(_fm_cmd_without))),)
$(error features.mk: both WITH and WITHOUT: $(filter $(_fm_cmd_with),$(_fm_cmd_without)))
endif

# _fm_state: 0/1 of feature $(1) (lower-case $(2)) given the with/without
# lists $(3)/$(4) over the profile and the default.
_fm_state = $(strip $(if $(filter $(2),$(4)),0,$(if $(filter $(2),$(3)),1,$(if $(filter $(2),$(_fm_prof_without)),0,$(if $(filter $(2),$(_fm_prof_with)),1,$($(1)_default))))))

define _fm_eval
$(1)_name := $$(call _fm_lc,$(1))
FEATURE_$(1) := $$(call _fm_state,$(1),$$($(1)_name),$$(_fm_cmd_with),$$(_fm_cmd_without))
ifeq ($$(filter 0 1,$$(FEATURE_$(1))),)
$$(error features.mk: $(1)_default must be 0 or 1)
endif
ifneq ($$(findstring $$(_fm_squote),$$($(1)_desc))$$(findstring $$(_fm_dquote),$$($(1)_desc))$$(findstring $$(_fm_comma),$$($(1)_desc)),)
$$(error features.mk: $(1)_desc must not contain quotes or commas)
endif
ifeq ($$(strip $$($(1)_bit)),)
$$(error features.mk: $(1)_bit is missing)
endif
endef
$(foreach f,$(FEATURE_LIST),$(eval $(call _fm_eval,$(f))))

define _fm_check
ifeq ($$(FEATURE_$(1)),1)
$$(foreach r,$$($(1)_requires),$$(if $$(filter 1,$$(FEATURE_$$(r))),,$$(error features.mk: $$($(1)_name) requires $$(call _fm_lc,$$(r)), which is off)))
$$(foreach c,$$($(1)_conflicts),$$(if $$(filter 1,$$(FEATURE_$$(c))),$$(error features.mk: $$($(1)_name) conflicts with $$(call _fm_lc,$$(c)), which is on),))
endif
endef
$(foreach f,$(FEATURE_LIST),$(eval $(call _fm_check,$(f))))

_fm_bits := $(foreach f,$(FEATURE_LIST),$($(f)_bit))
ifneq ($(words $(_fm_bits)),$(words $(sort $(_fm_bits))))
$(error features.mk: a NAME_bit is used twice ($(_fm_bits)))
endif

# What the Makefile uses.
FEATURES_ON   := $(strip $(foreach f,$(FEATURE_LIST),$(if $(filter 1,$(FEATURE_$(f))),$($(f)_name))))
FEATURES_OFF  := $(strip $(foreach f,$(FEATURE_LIST),$(if $(filter 1,$(FEATURE_$(f))),,$($(f)_name))))
FEATURE_SRCS  := $(foreach f,$(FEATURE_LIST),$(if $(filter 1,$(FEATURE_$(f))),$($(f)_srcs)))
FEATURE_INCS  := $(foreach f,$(FEATURE_LIST),$($(f)_incs))
FEATURE_DEFS  := $(foreach f,$(FEATURE_LIST),-DFEATURE_$(f)=$(FEATURE_$(f)) $(if $(filter 1,$(FEATURE_$(f))),$($(f)_defs)))
# usb_debug.c's old name for the USB drive switch (see the USBDRIVE block).
FEATURE_DEFS  += -DUSB_MSC=$(FEATURE_USBDRIVE)
# Shell arithmetic for the mask (make has none): (1<<0)|(1<<2)|...
FEATURES_MASK_EXPR := 0$(foreach f,$(FEATURE_LIST),$(if $(filter 1,$(FEATURE_$(f))),|(1<<$($(f)_bit))))

# The feature set WITHOUT the profile: defaults + command line. It names the
# object stamp (Makefile): named image targets always `clean` first, so only
# a change made on the command line can meet objects of another set.
FEATURES_STAMP_KEY := $(subst $(_fm_space),_,$(strip set $(foreach f,$(FEATURE_LIST),$(if $(filter 1,$(strip $(if $(filter $($(f)_name),$(_fm_cmd_without)),0,$(if $(filter $($(f)_name),$(_fm_cmd_with)),1,$($(f)_default))))),$($(f)_name)))))
