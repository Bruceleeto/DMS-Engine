# The engine for the Dreamcast, built once into build/dms/ and shared by every
# project (mk/example.mk includes this and links the modules it uses). On its
# own it builds every module: make -f mk/engine.mk (the top-level make all
# does, before the projects, so they don't race to build the same objects).

DMS_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))..)

# Not KOS_CC: environ.sh exports that, and kos-cc runs it, so it must stay sh-elf-gcc.
DC_CC = $(if $(KOS_BASE),kos-cc,$(error KOS_BASE is not set: source the KallistiOS environ.sh first))

DC_CFLAGS := -std=gnu2x -O3 -I$(DMS_ROOT) \
             -fomit-frame-pointer -flto -fbuiltin -ffast-math -ffp-contract=fast -mfsrra -mfsca \
             -fmerge-all-constants -funroll-loops -fno-PIC -fipa-pta \
             -ftree-vectorize -I$(KOS_BASE)/utils -MMD -MP

ENGINE_DIR := $(DMS_ROOT)/build/dms
# (font_data.c is font.h's, included there)
ENGINE_ALL := $(patsubst $(DMS_ROOT)/dms/%.c,$(ENGINE_DIR)/%.o,\
              $(filter-out %/font_data.c,$(wildcard $(DMS_ROOT)/dms/*.c)))

.PHONY: engine
engine: $(ENGINE_ALL)

$(ENGINE_DIR)/%.o: $(DMS_ROOT)/dms/%.c
	@mkdir -p $(@D)
	$(DC_CC) $(DC_CFLAGS) -c $< -o $@

-include $(ENGINE_ALL:.o=.d)
