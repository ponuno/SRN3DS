# Isolated ARM build; does not touch the main Makefile or shared source list.
DEVKITPRO ?= /opt/devkitpro
DEVKITARM ?= $(DEVKITPRO)/devkitARM
CTRULIB ?= $(DEVKITPRO)/libctru
CC := $(DEVKITARM)/bin/arm-none-eabi-gcc
3DSXTOOL ?= $(DEVKITPRO)/tools/bin/3dsxtool.exe

TARGET := texetc1
BUILD := build
ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS := -g -O2 -Wall -Wextra -std=gnu11 $(ARCH) -D__3DS__ \
          -I../../../source -I$(CTRULIB)/include
LDFLAGS := -specs=3dsx.specs -g $(ARCH) -Wl,--gc-sections
LIBS := -L$(CTRULIB)/lib -lcitro3d -lctru -lm

OFILES := $(BUILD)/main.o $(BUILD)/d3d9_texcache.o $(BUILD)/d3d9_texture_data.o

.PHONY: all clean
all: $(TARGET).3dsx

$(TARGET).3dsx: $(TARGET).elf
	$(3DSXTOOL) $< $@

$(TARGET).elf: $(OFILES)
	$(CC) $(LDFLAGS) $(OFILES) $(LIBS) -o $@

$(BUILD)/main.o: main.c ../../../source/d3d9_texcache.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/d3d9_texcache.o: ../../../source/d3d9_texcache.c ../../../source/d3d9_texcache.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/d3d9_texture_data.o: ../../../source/d3d9_texture_data.c ../../../source/d3d9_texture_data.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD) $(TARGET).elf $(TARGET).3dsx
