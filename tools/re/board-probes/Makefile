# Board probes: small bare-metal images for finding out what an unknown
# AT32F403A board has on its pins. One probe per image, no RTOS.
#
#   make                  # PROBE=hwtest: power hold, backlight, LCD colour bars
#   make PROBE=btntest    # all GPIO input registers on the LCD
#   make PROBE=uartsniff  # every USART as a 9600 baud receiver, bytes on the LCD
#
# The image links at 0x08000000, the start of flash on a chip with no
# bootloader. There is no flash rule on purpose: writing there replaces a
# bootloader, so choose the address and the method yourself (docs/re/README.md).
#
# It uses the vendor library and the clock setup of the firmware tree; nothing
# in firmware/ is written (objects go to BUILD_DIR here).

PROBE ?= hwtest
BUILD_DIR ?= build

FIRMWARE := ../../../firmware
AT32_LIB := $(FIRMWARE)/at32f403a_lib/libraries
AT32_DRV := $(AT32_LIB)/drivers
AT32_CMSIS := $(AT32_LIB)/cmsis/cm4
AT32_STARTUP := $(AT32_CMSIS)/device_support/startup/gcc
LDSCRIPT ?= $(AT32_STARTUP)/linker/AT32F403AxG_FLASH.ld

PREFIX = arm-none-eabi-
CC = $(PREFIX)gcc
AS = $(PREFIX)gcc -x assembler-with-cpp
CP = $(PREFIX)objcopy
SZ = $(PREFIX)size
NANO_SPECS := $(shell f="$(shell $(CC) -print-file-name=nano.specs 2>/dev/null)"; test "$$f" != "nano.specs" -a -f "$$f" && printf '%s' '-specs=nano.specs')
NOSYS_SPECS := $(shell f="$(shell $(CC) -print-file-name=nosys.specs 2>/dev/null)"; test "$$f" != "nosys.specs" -a -f "$$f" && printf '%s' '-specs=nosys.specs')

C_SOURCES = \
	$(PROBE).c \
	$(FIRMWARE)/src/at32f403a_407_clock.c \
	$(AT32_CMSIS)/device_support/system_at32f403a_407.c \
	$(AT32_DRV)/src/at32f403a_407_crm.c \
	$(AT32_DRV)/src/at32f403a_407_gpio.c \
	$(AT32_DRV)/src/at32f403a_407_flash.c \
	$(AT32_DRV)/src/at32f403a_407_misc.c \
	$(AT32_DRV)/src/at32f403a_407_xmc.c \
	$(AT32_DRV)/src/at32f403a_407_usart.c \
	$(AT32_DRV)/src/at32f403a_407_wdt.c

ASM_SOURCES = $(AT32_STARTUP)/startup_at32f403a_407.s

C_INCLUDES = \
	-I$(FIRMWARE)/include \
	-I$(FIRMWARE)/src \
	-I$(AT32_DRV)/inc \
	-I$(AT32_CMSIS)/device_support \
	-I$(AT32_CMSIS)/core_support \
	-I$(AT32_CMSIS)

MCU = -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
CFLAGS = $(MCU) $(C_INCLUDES) -DAT32F403AVGT7 -Os -Wall -fdata-sections -ffunction-sections
LDFLAGS = $(MCU) $(NANO_SPECS) $(NOSYS_SPECS) -T$(LDSCRIPT) -Wl,--gc-sections -Wl,-Map=$(BUILD_DIR)/$(PROBE).map,--cref

all: $(BUILD_DIR)/$(PROBE).bin

OBJECTS = $(addprefix $(BUILD_DIR)/,$(notdir $(C_SOURCES:.c=.o)))
OBJECTS += $(addprefix $(BUILD_DIR)/,$(notdir $(ASM_SOURCES:.s=.o)))
vpath %.c $(sort $(dir $(C_SOURCES)))
vpath %.s $(sort $(dir $(ASM_SOURCES)))

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/%.o: %.s | $(BUILD_DIR)
	$(AS) -c $(MCU) $< -o $@

$(BUILD_DIR)/$(PROBE).elf: $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	$(SZ) $@

$(BUILD_DIR)/$(PROBE).bin: $(BUILD_DIR)/$(PROBE).elf
	$(CP) -O binary $< $@
	@echo "Built: $@ ($$(wc -c < $@) bytes)"

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
