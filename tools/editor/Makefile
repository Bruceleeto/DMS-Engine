# The scene editor: the engine on gpu_pvr (as host/ builds a game), with
# Dear ImGui round it. Engine objects are host/'s, shared with every game.
#
#   make                 build build/editor/dms-editor
#   make run P=../../examples/teapot
ROOT := $(abspath ../..)
HOST := $(ROOT)/host
SHARED := $(ROOT)/build/host
OUT := $(ROOT)/build/editor
EXE := $(OUT)/dms-editor
P ?= $(ROOT)/examples/teapot

CC := gcc
CXX := g++
VRAM_MB ?= 8
ARCH_FLAGS := -m32 -fno-pie -DGPU_PVR_VRAM_MB=$(VRAM_MB)
SDL_CFLAGS := $(shell pkg-config --cflags sdl3)
GPU_PVR_DIR := $(HOST)/third_party/gpu_pvr
GPU_PVR_OUT := $(SHARED)/gpu_pvr
SHZ := $(HOST)/third_party/sh4zam
IMGUI := $(HOST)/third_party/imgui
GIZMO := $(HOST)/third_party/imguizmo
.DEFAULT_GOAL := all
include $(GPU_PVR_DIR)/gpu_pvr.mk

# No Dreamcast image to size the heap from: a 2 MB one, as host/ assumes.
HEAP_START ?= 0x8c200000
KOS_BYTES ?= 105472

ENGINE := dc_engine dc_input dc_camera dc_model dc_player dc_draw2d dc_draw dc_debug collision dc_scene
SHZ_SRC := shz_version sw/shz_complex_sw sw/shz_matrix_sw sw/shz_xmtrx_sw

CPPFLAGS := -DDMS_HOST -DDMS_ROOT_DIR='"$(ROOT)"' -I$(HOST)/include -I$(ROOT) -I$(ROOT)/dms -I$(HOST) -I$(SHZ)/include $(GPU_PVR_CFLAGS) $(SDL_CFLAGS)
CFLAGS := $(ARCH_FLAGS) -std=gnu2x -O2 -g -msse2 -mfpmath=sse -ffp-contract=off -MMD -MP
CXXFLAGS := $(ARCH_FLAGS) -std=gnu++17 -O2 -g -msse2 -mfpmath=sse -MMD -MP -D_Static_assert=static_assert \
            -I$(IMGUI) -I$(GIZMO)
DCSIM := -include $(HOST)/dcsim/dcsim.h

EDITOR_SRC := main view panels ui play
IMGUI_SRC := imgui imgui_draw imgui_tables imgui_widgets imgui_demo backends/imgui_impl_sdl3 backends/imgui_impl_sdlrenderer3

OBJS := $(addprefix $(OUT)/,$(addsuffix .o,$(EDITOR_SRC))) \
        $(addprefix $(OUT)/imgui/,$(addsuffix .o,$(IMGUI_SRC))) $(OUT)/imguizmo/ImGuizmo.o \
        $(addprefix $(SHARED)/dms/,$(addsuffix .o,$(ENGINE))) \
        $(addprefix $(SHARED)/sh4zam/,$(addsuffix .o,$(SHZ_SRC))) \
        $(SHARED)/kos_fog.o $(SHARED)/kos_mod.o $(SHARED)/shim.o $(SHARED)/png.o $(OUT)/dcsim_malloc.o

.PHONY: all run
all: $(EXE)
run: $(EXE)
	$(EXE) $(P)

$(OUT)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
$(OUT)/imgui/%.o: $(IMGUI)/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(SDL_CFLAGS) $(CXXFLAGS) -c $< -o $@
$(OUT)/imguizmo/%.o: $(GIZMO)/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(OUT)/dcsim_malloc.o: $(HOST)/dcsim/dcsim_malloc.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -DDCSIM_HEAP_START=$(HEAP_START)u -DDCSIM_KOS_BYTES=$(KOS_BYTES) -DMALLOC_ALIGNMENT=16 -c $< -o $@
$(SHARED)/dms/%.o: $(ROOT)/dms/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DCSIM) -c $< -o $@
$(SHARED)/sh4zam/%.o: $(SHZ)/source/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(SHARED)/kos_%.o: $(GPU_PVR_DIR)/kos_%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(SHARED)/%.o: $(HOST)/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(EXE): $(OBJS) $(GPU_PVR_LIB)
	$(CXX) $(ARCH_FLAGS) -no-pie $^ -lSDL3 -lpthread -lm -o $@

-include $(OBJS:.o=.d)
