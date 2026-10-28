# Linux desktop preview; isolated objects never overwrite Dreamcast builds.
ROOT := $(abspath ..)
OUT := $(ROOT)/build/host
EXAMPLE ?= 1st_person
# The project folder: examples/<name> or games/<name>.
SRC := $(firstword $(wildcard $(ROOT)/examples/$(EXAMPLE) $(ROOT)/games/$(EXAMPLE)))
CC := gcc
CXX := g++
ARCH_FLAGS := -m32 -DGPU_PVR_VRAM_MB=8
SDL_CFLAGS := $(shell pkg-config --cflags sdl3)
GPU_PVR_DIR := $(CURDIR)/third_party/gpu_pvr
GPU_PVR_OUT := $(OUT)/gpu_pvr
SHZ := $(CURDIR)/third_party/sh4zam
.DEFAULT_GOAL := all
include third_party/gpu_pvr/gpu_pvr.mk

CPPFLAGS := -DDMS_HOST -I$(CURDIR)/include -I$(ROOT) -I$(SHZ)/include $(GPU_PVR_CFLAGS) $(SDL_CFLAGS)
CFLAGS := $(ARCH_FLAGS) -std=gnu2x -O2 -g -msse2 -mfpmath=sse -ffp-contract=off -MMD -MP
ENGINE := dc_engine dc_input dc_camera dc_model dc_player dc_draw2d dc_draw dc_debug collision
SHZ_SRC := shz_matrix shz_quat shz_xmtrx sw/shz_xmtrx_sw sw/shz_complex_sw
OBJS := $(addprefix $(OUT)/dms/,$(addsuffix .o,$(ENGINE))) \
        $(addprefix $(OUT)/sh4zam/,$(addsuffix .o,$(SHZ_SRC))) \
        $(OUT)/kos_fog.o $(OUT)/kos_mod.o $(OUT)/platform.o $(OUT)/$(EXAMPLE)/main.o

.PHONY: all run assets
all: $(OUT)/$(EXAMPLE)/dms-preview
$(OUT)/dms/%.o: $(ROOT)/dms/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(OUT)/sh4zam/%.o: $(SHZ)/source/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(OUT)/platform.o: platform.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(OUT)/kos_%.o: $(GPU_PVR_DIR)/kos_%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(OUT)/$(EXAMPLE)/main.o: $(SRC)/main.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Dmain=dms_example_main -DASSETS='"$(SRC)/pc/"' -c $< -o $@
$(OUT)/$(EXAMPLE)/dms-preview: $(OBJS) $(GPU_PVR_LIB)
	$(CXX) $(ARCH_FLAGS) $^ -lSDL3 -lpthread -lm -o $@
run: all assets
	$(OUT)/$(EXAMPLE)/dms-preview

# Asset conversion is a native tool and needs no KOS environment.
GLBS := $(wildcard $(SRC)/assets/*/*.glb)
DMS := $(patsubst $(SRC)/assets/%.glb,$(SRC)/pc/%.dms,$(GLBS))
assets: $(DMS)
$(ROOT)/tools/converter/strippy: $(ROOT)/tools/converter/main.cpp $(ROOT)/tools/converter/main.h
	$(MAKE) -C $(ROOT)/tools/converter
$(SRC)/pc/%.dms: $(SRC)/assets/%.glb $(ROOT)/tools/converter/strippy
	@mkdir -p $(@D)
	cp $< $(@D)/
	cd $(@D) && $(ROOT)/tools/converter/strippy $(notdir $<)
	rm $(@D)/$(notdir $<)
-include $(OBJS:.o=.d)
