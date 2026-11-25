TARGET := Skyrim_N3DS
BUILD := build
# `tools/build.ps1 -Mode default -Isolated irbench` sets BUILD=build_irbench;
# use it to select the ARM11 AOT-vs-IR microbenchmark without adding a mode to
# build.ps1 (owned elsewhere).
ifeq ($(strip $(BUILD)),build_irbench)
IRBENCH := 1
endif
ifneq ($(strip $(GRAPHICS)),)
ifneq ($(strip $(LIFT_STAGE)$(LIFT_GEN)$(BOOT_SMOKE)),)
$(error GRAPHICS is a separate build mode)
endif
TARGET := Skyrim_N3DS_graphics
BUILD := build_graphics
endif
ifneq ($(strip $(AUDIO)),)
ifneq ($(strip $(GRAPHICS)$(LIFT_STAGE)$(LIFT_GEN)$(BOOT_SMOKE)$(PILOT_GEN)$(TESV_SLICE)$(GRAPHICS_COM)),)
$(error AUDIO is a separate build mode)
endif
TARGET := Skyrim_N3DS_audio
BUILD := build_audio
endif
ifneq ($(strip $(LIFT_STAGE)),)
BUILD := build_lift
TARGET := Skyrim_N3DS_lift
endif
ifneq ($(strip $(LIFT_GEN)),)
ifeq ($(strip $(LIFT_STAGE)),)
$(error LIFT_GEN requires LIFT_STAGE)
endif
BUILD := build_generated
TARGET := Skyrim_N3DS_generated
endif
ifneq ($(strip $(BOOT_SMOKE)),)
ifeq ($(strip $(LIFT_STAGE)),)
$(error BOOT_SMOKE requires LIFT_STAGE)
endif
ifneq ($(strip $(LIFT_GEN)),)
$(error BOOT_SMOKE and LIFT_GEN are separate build modes)
endif
BUILD := build_boot
TARGET := Skyrim_N3DS_boot
endif
ifneq ($(strip $(GRAPHICS_COM)),)
ifeq ($(strip $(LIFT_STAGE)),)
$(error GRAPHICS_COM requires LIFT_STAGE)
endif
ifneq ($(strip $(GRAPHICS)$(LIFT_GEN)$(BOOT_SMOKE)),)
$(error GRAPHICS_COM is a separate build mode)
endif
BUILD := build_graphics_com
TARGET := Skyrim_N3DS_graphics_com
endif
ifneq ($(strip $(TESV_SLICE)),)
ifeq ($(strip $(LIFT_STAGE)),)
$(error TESV_SLICE requires LIFT_STAGE)
endif
ifeq ($(strip $(TESV_GEN)),)
$(error TESV_SLICE requires TESV_GEN)
endif
ifneq ($(strip $(GRAPHICS)$(GRAPHICS_COM)$(LIFT_GEN)$(BOOT_SMOKE)$(PILOT_GEN)),)
$(error TESV_SLICE is a separate build mode)
endif
BUILD := build_tesv
TARGET := Skyrim_N3DS_tesv
endif
ifneq ($(strip $(PILOT_GEN)),)
ifeq ($(strip $(LIFT_STAGE)),)
$(error PILOT_GEN requires LIFT_STAGE)
endif
ifneq ($(strip $(GRAPHICS)$(GRAPHICS_COM)$(LIFT_GEN)$(BOOT_SMOKE)),)
$(error PILOT_GEN is a separate build mode)
endif
BUILD := build_pilot
TARGET := Skyrim_N3DS_pilot
endif
ifneq ($(strip $(VA_PROBE)),)
ifneq ($(strip $(GRAPHICS)$(GRAPHICS_COM)$(LIFT_GEN)$(BOOT_SMOKE)$(PILOT_GEN)$(TESV_SLICE)$(AUDIO)),)
$(error VA_PROBE is a separate build mode)
endif
BUILD := build_va
TARGET := Skyrim_N3DS_va
endif
ifneq ($(strip $(IRBENCH)),)
BUILD := build_irbench
TARGET := Skyrim_N3DS_irbench
endif
TOPDIR ?= $(CURDIR)
DEVKITARM ?= $(DEVKITPRO)/devkitARM
include $(DEVKITARM)/3ds_rules

ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
OPT ?= -O2
CFLAGS := -g $(OPT) -Wall -Wextra -std=gnu11 -mword-relocations -ffunction-sections $(ARCH) $(INCLUDE) -D__3DS__
ifdef TESV_ARM_STEPS
CFLAGS += -DTESV_ARM_STEPS=$(TESV_ARM_STEPS)
endif
ifneq ($(strip $(LIFT_STAGE)),)
CFLAGS += -DN3DS_LIFT_TEST -fwrapv -fno-strict-aliasing -Wno-misleading-indentation
endif
ifneq ($(strip $(TESV_FORCE_MEMPROBE)),)
CFLAGS += -DTESV_FORCE_MEMPROBE
endif
LDFLAGS := -specs=3dsx.specs -g $(ARCH) -Wl,--gc-sections
LIBS := -lctru -lm
ifneq ($(strip $(GRAPHICS)$(GRAPHICS_COM)),)
LIBS := -lcitro3d -lctru -lm
endif
ifneq ($(strip $(AUDIO)),)
# OpenDSP replaces the ndsp firmware, so it must come before libctru.
LIBS := -lopendsp -lctru -lm
endif
ifneq ($(strip $(TESV_SLICE)),)
# TESV uses the real PICA backend when available, with OpenDSP before libctru.
LIBS := -lcitro3d -lopendsp -lctru -lm
endif

ifneq ($(notdir $(CURDIR)),$(BUILD))
export OUTPUT := $(CURDIR)/$(TARGET)
export VPATH := $(CURDIR)/source
export DEPSDIR := $(CURDIR)/$(BUILD)
export INCLUDE := -I$(CURDIR)/source -I$(CTRULIB)/include
export LIBPATHS := -L$(CTRULIB)/lib
ifneq ($(strip $(AUDIO)$(TESV_SLICE)),)
export INCLUDE += -I$(PORTLIBS)/include
export LIBPATHS += -L$(PORTLIBS)/lib
endif
export LD := $(CC)
export OFILES := main.o probe.o guest.o pe_image.o pe_imports.o guest_heap.o guest_call.o guest_process.o
ifneq ($(strip $(IRBENCH)),)
export VPATH += $(CURDIR)/tests $(CURDIR)/staged
export INCLUDE += -I$(CURDIR)/tests -I$(CURDIR)/staged
export OFILES := ir_bench_main.o ir_aot_fixture.o guest.o n3ds_lift_runtime.o ir_core.o ir_decode.o ir_exec.o ir_container.o fpu.o
endif
ifneq ($(strip $(VA_PROBE)),)
export OFILES := va_probe_main.o guest.o
endif
ifneq ($(strip $(PILOT_GEN)),)
override export PILOT_GEN := $(abspath $(PILOT_GEN))
ifeq ($(wildcard $(PILOT_GEN)/pilot_config.h),)
$(error Missing pilot_config.h; run tools/prepare_pilot.py)
endif
export VPATH += $(PILOT_GEN)
export INCLUDE += -I$(PILOT_GEN)
export OFILES := pilot_main.o n3ds_pilot.o guest.o guest_heap.o pe_image.o pe_imports.o guest_process.o $(patsubst %.c,%.o,$(notdir $(wildcard $(PILOT_GEN)/pilot_*.c)))
endif
ifneq ($(strip $(GRAPHICS)),)
export OFILES := graphics_main.o d3d9_core.o d3d9_texcache.o d3d9_texture_data.o d3d9_bc.o d3d9_vertex.o d3d9_adapter.o d3d9_null.o d3d9_citro3d.o d3d9_color_tev.o d3d9_demo_scene.o d3d9_tev.o d3d9_pica_tev.o d3d9_pica_citro3d.o d3d9_menu_light.o d9shader.shbin.o d9shader_color.shbin.o
export INCLUDE += -I$(CURDIR)/$(BUILD)
endif
ifneq ($(strip $(GRAPHICS_COM)),)
export OFILES := graphics_com_main.o d3d9_core.o d3d9_texcache.o d3d9_texture_data.o d3d9_bc.o d3d9_vertex.o d3d9_adapter.o d3d9_null.o d3d9_shader.o d3d9_tev.o d3d9_menu_closure.o d3d9_pica_tev.o d3d9_pica_citro3d.o d3d9_menu_light.o d3d9_citro3d.o d3d9_color_tev.o d3d9_demo_scene.o d3d9_com.o d3d9_com_demo.o d9shader.shbin.o d9shader_color.shbin.o d3d9_capture.o guest.o guest_heap.o pe_imports.o
export INCLUDE += -I$(CURDIR)/$(BUILD)
endif
ifneq ($(strip $(AUDIO)),)
# The demo talks to the same win32_audio module the guest uses.
export OFILES := audio_main.o win32_audio.o guest.o
export INCLUDE += -I$(CURDIR)/staged
endif
ifneq ($(strip $(LIFT_STAGE)),)
override export LIFT_STAGE := $(abspath $(LIFT_STAGE))
export VPATH += $(abspath $(LIFT_STAGE)) $(CURDIR)/tests
export INCLUDE += -I$(abspath $(LIFT_STAGE))
export OFILES += n3ds_lift_runtime.o ir_core.o ir_decode.o ir_exec.o ir_container.o rt_prof.o fpu.o win32_shims.o win32_sync.o win32_user.o win32_locale.o win32_shell.o win32_audio.o win32_thread.o win32_hostctx.o win32_input.o win32_steam.o win32_registry.o
ifeq ($(strip $(GRAPHICS_COM)$(PILOT_GEN)$(TESV_SLICE)),)
ifneq ($(strip $(BOOT_SMOKE)),)
export OFILES += n3ds_boot.o boot_smoke.o
else
export OFILES += win32_smoke.o
ifneq ($(strip $(LIFT_GEN)),)
export LIFT_GEN := $(abspath $(LIFT_GEN))
export VPATH += $(abspath $(LIFT_GEN))
export INCLUDE += -I$(abspath $(LIFT_GEN))
export OFILES += generated_smoke.o $(patsubst %.c,%.o,$(notdir $(wildcard $(LIFT_GEN)/fixture_*.c)))
else
export OFILES += lift_smoke.o
endif
endif
endif
endif
ifneq ($(strip $(TESV_SLICE)),)
override export TESV_GEN := $(abspath $(TESV_GEN))
export VPATH += $(TESV_GEN)
export INCLUDE += -I$(TESV_GEN)
export OFILES := n3ds_boot.o tesv_slice_main.o n3ds_pilot.o guest.o guest_heap.o pe_image.o pe_imports.o guest_process.o n3ds_lift_runtime.o ir_core.o ir_decode.o ir_exec.o ir_container.o rt_prof.o fpu.o win32_shims.o win32_sync.o win32_user.o win32_locale.o win32_shell.o win32_audio.o win32_audio_3ds.o win32_thread.o win32_hostctx.o win32_input.o win32_steam.o win32_registry.o d3d9_core.o d3d9_texcache.o d3d9_texture_data.o d3d9_bc.o d3d9_vertex.o d3d9_adapter.o d3d9_shader.o d3d9_tev.o d3d9_menu_closure.o d3d9_pica_tev.o d3d9_pica_citro3d.o d3d9_menu_light.o d3d9_com.o d3d9_null.o d3d9_citro3d.o d3d9_color_tev.o d3d9_capture.o d9shader.shbin.o d9shader_color.shbin.o $(patsubst %.c,%.o,$(notdir $(wildcard $(TESV_GEN)/tesv_*.c)))
export INCLUDE += -I$(CURDIR)/$(BUILD)
endif
.PHONY: all clean
all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
$(BUILD):
	@mkdir -p $@
clean:
	@rm -f $(BUILD)/*.o $(BUILD)/*.d $(TARGET).elf $(TARGET).3dsx
else
$(OUTPUT).3dsx: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)
ifneq ($(strip $(GRAPHICS)$(GRAPHICS_COM)$(TESV_SLICE)),)
# Both vertex shaders are compiled from picasso source in source/ and embedded
# with bin2s.  The colour shader feeds D9Backend.draw_colored.
PICASSO ?= $(DEVKITPRO)/tools/bin/picasso
%.shbin: %.v.pica
	$(PICASSO) -o $@ $<
d3d9_citro3d.o: d9shader_shbin.h d9shader_color_shbin.h
%.shbin.o %_shbin.h: %.shbin
	$(bin2o)
endif
-include $(DEPSDIR)/*.d
endif



