# Isolated ARM11/Azahar timing-profile build.
# This Makefile is not referenced by the repository's main Makefile.
ROOT := $(abspath ../../..)
DEVKITPRO ?= /opt/devkitpro
DEVKITARM ?= $(DEVKITPRO)/devkitARM
CTRULIB ?= $(DEVKITPRO)/libctru
CC := $(DEVKITARM)/bin/arm-none-eabi-gcc
3DSXTOOL ?= $(DEVKITPRO)/tools/bin/3dsxtool

TARGET := armtime
BUILD := build
ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS := -g -O2 -Wall -Wextra -std=gnu11 $(ARCH) -D__3DS__ \
          -fwrapv -fno-strict-aliasing -Wno-misleading-indentation \
          -ffunction-sections -fdata-sections \
          -I$(ROOT)/source -I$(ROOT)/staged -I$(ROOT)/tests -I$(CTRULIB)/include
LDFLAGS := -specs=3dsx.specs -g $(ARCH) -Wl,--gc-sections
LIBS := -L$(CTRULIB)/lib -lctru -lm

# Runtime and fixture sources are read-only dependencies.  All harness code and
# build rules live in this isolated directory.
OFILES := $(BUILD)/main.o $(BUILD)/ir_aot_fixture.o $(BUILD)/guest.o \
          $(BUILD)/n3ds_lift_runtime.o $(BUILD)/ir_core.o $(BUILD)/ir_decode.o \
          $(BUILD)/ir_exec.o $(BUILD)/fpu.o

.PHONY: all clean
all: $(TARGET).3dsx

$(TARGET).3dsx: $(TARGET).elf
	$(3DSXTOOL) $< $@

$(TARGET).elf: $(OFILES)
	$(CC) $(LDFLAGS) $(OFILES) $(LIBS) -o $@

$(BUILD)/main.o: main.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ir_aot_fixture.o: $(ROOT)/tests/ir_aot_fixture.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/guest.o: $(ROOT)/source/guest.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/n3ds_lift_runtime.o: $(ROOT)/source/n3ds_lift_runtime.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ir_core.o: $(ROOT)/source/ir_core.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ir_decode.o: $(ROOT)/source/ir_decode.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ir_exec.o: $(ROOT)/source/ir_exec.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/fpu.o: $(ROOT)/staged/fpu.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD) $(TARGET).elf $(TARGET).3dsx
