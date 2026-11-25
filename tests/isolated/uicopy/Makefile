# Isolated ARM/Azahar ui.copy proof.  This does not modify the main build.
DEVKITPRO ?= C:/devkitPro
DEVKITARM ?= $(DEVKITPRO)/devkitARM
CTRULIB ?= $(DEVKITPRO)/libctru
CC := $(DEVKITARM)/bin/arm-none-eabi-gcc
PICASSO ?= $(DEVKITPRO)/tools/bin/picasso.exe
3DSXTOOL ?= $(DEVKITPRO)/tools/bin/3dsxtool.exe

TARGET := uicopy_arm
TARGETS := $(TARGET) rt_switch_arm
BUILD := build
ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS := -g -O2 -Wall -Wextra -std=gnu11 $(ARCH) -D__3DS__ \
          -mword-relocations -ffunction-sections -fwrapv -fno-strict-aliasing -Wno-misleading-indentation \
          -I$(BUILD) -I../../../source -I../../../staged -I$(CTRULIB)/include
LDFLAGS := -specs=3dsx.specs -g $(ARCH) -Wl,--gc-sections
LIBS := -L$(CTRULIB)/lib -lcitro3d -lctru -lm

RUNTIME := guest guest_heap pe_image pe_imports guest_process \
           win32_shims win32_sync win32_user win32_locale win32_shell \
           win32_audio win32_thread win32_input win32_steam win32_registry \
           win32_hostctx n3ds_lift_runtime ir_core ir_decode ir_exec ir_container rt_prof fpu
D3D := d3d9_core d3d9_texcache d3d9_texture_data d3d9_bc d3d9_vertex \
       d3d9_adapter d3d9_null d3d9_shader d3d9_tev d3d9_pica_tev d3d9_com d3d9_capture
PIPELINE := d3d9_pipeline_match d3d9_pipeline_registry d3d9_menu_light
COMMON_OFILES := $(BUILD)/shbin_blob.o $(BUILD)/d3d9_pica_citro3d.o \
                 $(addprefix $(BUILD)/,$(addsuffix .o,$(RUNTIME) $(D3D) $(PIPELINE)))
OFILES := $(BUILD)/main.o $(COMMON_OFILES)
RT_SWITCH_OFILES := $(BUILD)/rt_switch.o $(COMMON_OFILES)

vpath %.c ../../../source ../../../staged

.PHONY: all clean
all: $(addsuffix .3dsx,$(TARGETS))

%.3dsx: %.elf
	$(3DSXTOOL) $< $@

$(TARGET).elf: $(OFILES)
	$(CC) $(LDFLAGS) $(OFILES) $(LIBS) -o $@

rt_switch_arm.elf: $(RT_SWITCH_OFILES)
	$(CC) $(LDFLAGS) $(RT_SWITCH_OFILES) $(LIBS) -o $@

$(BUILD):
	@mkdir -p $@

$(BUILD)/d9shader.shbin: ../../../source/d9shader.v.pica | $(BUILD)
	$(PICASSO) -o $@ $<

$(BUILD)/d9shader_shbin.h: $(BUILD)/d9shader.shbin
	@printf '#pragma once\n#include <stddef.h>\n#include <stdint.h>\nextern const uint8_t d9shader_shbin[];\nextern const uint8_t d9shader_shbin_end[];\n' > $@
	@printf '#define d9shader_shbin_size %s\n' "$$(stat -c %s $<)" >> $@

$(BUILD)/shbin_blob.o: shbin_blob.S $(BUILD)/d9shader.shbin | $(BUILD)
	$(CC) $(ARCH) -c $< -o $@

$(BUILD)/main.o: main.c fixture.h ../../../source/d3d9_citro3d.c \
                 ../../../source/d3d9_pica_citro3d.h $(BUILD)/d9shader_shbin.h | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/rt_switch.o: rt_switch.c fixture.h ../../../source/d3d9_citro3d.c \
                      ../../../source/d3d9_pica_citro3d.h $(BUILD)/d9shader_shbin.h | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf $(BUILD) $(addsuffix .elf,$(TARGETS)) $(addsuffix .3dsx,$(TARGETS))

-include $(BUILD)/*.d
