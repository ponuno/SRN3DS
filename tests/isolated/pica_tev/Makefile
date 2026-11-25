# Isolated ARM build for the PICA200 TEV adapter.  This Makefile is not part of
# the main build and does not modify its source list or generated shader inputs.
DEVKITPRO ?= /opt/devkitpro
DEVKITARM ?= $(DEVKITPRO)/devkitARM
CTRULIB ?= $(DEVKITPRO)/libctru
CC := $(DEVKITARM)/bin/arm-none-eabi-gcc
3DSXTOOL ?= $(DEVKITPRO)/tools/bin/3dsxtool

TARGET := pica_tev_arm
BUILD := build
ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS := -g -O2 -Wall -Wextra -Werror -std=gnu11 $(ARCH) -D__3DS__ \
          -I../../../source -I$(CTRULIB)/include
LDFLAGS := -specs=3dsx.specs -g $(ARCH) -Wl,--gc-sections
LIBS := -L$(CTRULIB)/lib -lcitro3d -lctru -lm

OFILES := $(BUILD)/main.o $(BUILD)/d3d9_tev.o \
          $(BUILD)/d3d9_pica_tev.o $(BUILD)/d3d9_pica_citro3d.o

.PHONY: all clean
all: $(TARGET).3dsx

$(TARGET).3dsx: $(TARGET).elf
	$(3DSXTOOL) $< $@

$(TARGET).elf: $(OFILES)
	$(CC) $(LDFLAGS) $(OFILES) $(LIBS) -o $@

$(BUILD)/main.o: main.c ../../../source/d3d9_pica_citro3d.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/d3d9_tev.o: ../../../source/d3d9_tev.c ../../../source/d3d9_tev.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/d3d9_pica_tev.o: ../../../source/d3d9_pica_tev.c ../../../source/d3d9_pica_tev.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/d3d9_pica_citro3d.o: ../../../source/d3d9_pica_citro3d.c ../../../source/d3d9_pica_citro3d.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD) $(TARGET).elf $(TARGET).3dsx
