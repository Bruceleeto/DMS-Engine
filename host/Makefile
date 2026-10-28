# Linux build of a project: the same engine and game code on gpu_pvr (a
# software PVR in an SDL3 window), sh4zam's portable backend and the
# Dreamcast's own malloc on the RAM it has free (dcsim/). What runs out of
# memory there runs out here.
#
# Called by mk/example.mk (make host, make host-run, make shot), which passes
# the project's PROJECT, NAME, SRCS, ENGINE and DEFINES.
ROOT := $(abspath ..)
PROJECT ?= $(ROOT)/examples/teapot
PROJECT := $(abspath $(PROJECT))
NAME ?= $(notdir $(PROJECT))
SRCS ?= main.c
ENGINE ?= dc_engine dc_input dc_camera dc_model dc_player dc_draw2d dc_draw dc_debug dc_profile collision dc_scene
# Shared by every project; each project's own objects go beside its DC build.
SHARED := $(ROOT)/build/host
OUT := $(ROOT)/build/$(NAME)/host
EXE := $(OUT)/$(NAME)

CC := gcc
CXX := g++
# 32-bit: gpu_pvr keeps host pointers in 32-bit PVR addresses, as KOS does.
# 8 MB of VRAM is a retail Dreamcast.
VRAM_MB ?= 8
ARCH_FLAGS := -m32 -fno-pie -DGPU_PVR_VRAM_MB=$(VRAM_MB)
SDL_CFLAGS := $(shell pkg-config --cflags sdl3)
GPU_PVR_DIR := $(CURDIR)/third_party/gpu_pvr
GPU_PVR_OUT := $(SHARED)/gpu_pvr
SHZ := $(CURDIR)/third_party/sh4zam
.DEFAULT_GOAL := all
include third_party/gpu_pvr/gpu_pvr.mk

# The heap starts where the Dreamcast build's image ends. Without that build
# (no KOS here), assume a 2 MB image.
DC_ELF := $(ROOT)/build/$(NAME)/$(NAME).elf
SH_NM := /opt/toolchains/dc/sh-elf/bin/sh-elf-nm
HEAP_START := $(shell $(SH_NM) $(DC_ELF) 2>/dev/null | awk '$$3 == "_end" { print "0x" $$1 }')
ifeq ($(HEAP_START),)
HEAP_START := 0x8c200000
$(info host: no $(DC_ELF) to size the heap from; assuming the image ends at $(HEAP_START))
endif
# What KallistiOS mallocs before the game does (iortcw's measure of it).
KOS_BYTES ?= 105472

CPPFLAGS := -DDMS_HOST -I$(CURDIR)/include -I$(ROOT) -I$(SHZ)/include $(GPU_PVR_CFLAGS) $(SDL_CFLAGS)
CFLAGS := $(ARCH_FLAGS) -std=gnu2x -O2 -g -msse2 -mfpmath=sse -ffp-contract=off -MMD -MP
# Engine and game allocations go to the Dreamcast heap.
DCSIM := -include $(CURDIR)/dcsim/dcsim.h
SHZ_SRC := shz_version sw/shz_complex_sw sw/shz_matrix_sw sw/shz_xmtrx_sw

ENGINE_OBJS := $(addprefix $(SHARED)/dms/,$(addsuffix .o,$(ENGINE)))
GAME_OBJS := $(addprefix $(OUT)/,$(SRCS:.c=.o))
OBJS := $(GAME_OBJS) $(ENGINE_OBJS) \
        $(addprefix $(SHARED)/sh4zam/,$(addsuffix .o,$(SHZ_SRC))) \
        $(SHARED)/kos_fog.o $(SHARED)/kos_mod.o $(SHARED)/platform.o $(SHARED)/shim.o $(SHARED)/png.o $(OUT)/dcsim_malloc.o

.PHONY: all
all: $(EXE)

$(SHARED)/dms/%.o: $(ROOT)/dms/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DCSIM) -c $< -o $@
$(SHARED)/sh4zam/%.o: $(SHZ)/source/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(SHARED)/%.o: $(CURDIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(SHARED)/kos_%.o: $(GPU_PVR_DIR)/kos_%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
# Per project: the heap's start comes from that project's Dreamcast image.
# 16-byte blocks (newlib on the SH4 gives 8): x86 copies aligned structs with
# SSE, which faults on 8.
$(OUT)/dcsim_malloc.o: dcsim/dcsim_malloc.c $(wildcard $(DC_ELF))
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -DDCSIM_HEAP_START=$(HEAP_START)u -DDCSIM_KOS_BYTES=$(KOS_BYTES) -DMALLOC_ALIGNMENT=16 -c $< -o $@
$(OUT)/%.o: $(PROJECT)/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DCSIM) $(DEFINES) -Dmain=dms_game_main -DASSETS='"$(PROJECT)/pc/"' -c $< -o $@

$(EXE): $(OBJS) $(GPU_PVR_LIB)
	$(CXX) $(ARCH_FLAGS) -no-pie $^ -lSDL3 -lpthread -lm -o $@

-include $(OBJS:.o=.d)
