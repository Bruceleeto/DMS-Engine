#
# gpu_pvr: the KOS PVR API on libpvr (a software PVR), for running
# Dreamcast PVR code on PC.  Not used on the Dreamcast itself.
#
# Include from another Makefile after setting:
#   GPU_PVR_DIR   path to this directory
#   GPU_PVR_OUT   object directory
#   ARCH_FLAGS    must give a 32-bit build (-m32): the shim keeps host
#                 pointers in 32-bit PVR addresses, like KOS does
#   SDL_CFLAGS
#
# Provides:
#   GPU_PVR_CFLAGS  include paths for code using <dc/pvr.h>
#   GPU_PVR_LIB     static library to link (needs SDL3, -lpthread, libstdc++)
#

GPU_PVR_CFLAGS = -I$(GPU_PVR_DIR) -I$(GPU_PVR_DIR)/kos

GPU_PVR_LIB = $(GPU_PVR_OUT)/libgpu_pvr.a

GPU_PVR_CXX_SRC = libpvr/pvr.cpp libpvr/ta.cpp libpvr/core.cpp libpvr/core_gl.cpp libpvr/texture.cpp kos_pvr.cpp
GPU_PVR_C_SRC   = kos_prim.c pvr_host.c

GPU_PVR_OBJ = $(patsubst %,$(GPU_PVR_OUT)/%.o,$(GPU_PVR_CXX_SRC) $(GPU_PVR_C_SRC))

# core_gl.cpp runs CORE on the host GPU (BT3_PVR_RENDER=soft: the tiles)
# libpvr is the hot path on PC; keep it optimised even in DEBUG builds
GPU_PVR_OPT = -O2 -msse2 -mfpmath=sse

$(GPU_PVR_OUT)/%.cpp.o: $(GPU_PVR_DIR)/%.cpp
	@echo "PVR_CXX $<"
	@mkdir -p $(@D)
	$(Q)$(CXX) $(ARCH_FLAGS) $(GPU_PVR_OPT) -std=c++17 -w -MMD -I$(GPU_PVR_DIR) $(SDL_CFLAGS) -c $< -o $@

$(GPU_PVR_OUT)/%.c.o: $(GPU_PVR_DIR)/%.c
	@echo "PVR_CC $<"
	@mkdir -p $(@D)
	$(Q)$(CC) $(ARCH_FLAGS) $(GPU_PVR_OPT) -w -MMD $(GPU_PVR_CFLAGS) $(SDL_CFLAGS) -c $< -o $@

$(GPU_PVR_LIB): $(GPU_PVR_OBJ)
	@echo "AR $@"
	$(Q)rm -f $@
	$(Q)ar rcs $@ $^

-include $(GPU_PVR_OBJ:.o=.d)
