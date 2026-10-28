# Shared Dreamcast build for every example and game.
#
# A project Makefile sets what differs, then includes this file:
#
#   BAKE = 1                       # bake vertex lighting (AO, shadows) into levels
#   ENGINE_EXTRA = dc_audio        # engine modules beyond the default set
#   SRCS = src/main.c src/game.c   # sources (default: main.c)
#   DEFINES = -DMY_FLAG            # defines for the project's own sources
#   include ../../mk/example.mk
#
# make           convert assets into pc/ and build build/<name>/<name>.elf
# make run       send the ELF with dc-tool, serving pc/ as /pc
# make disc      build build/<name>/<name>.cdi with pc/ as the disc root
# make assets    convert assets/ only
# make clean     remove build/<name>/      make assets-clean   remove pc/
#
# On Linux, the same code on a software PVR and a Dreamcast-sized heap:
# make host      build build/<name>/host/<name>
# make host-run  run it in a window
# make shot      render SHOT_FRAMES frames without a window -> build/<name>/shot.png
# make check     run each scene CHECK_FRAMES frames without a window: a line
#                each (finished? peak RAM? warnings?), no screenshots needed
# make edit      open assets/$(SCENE) (scenes/main.json) in the scene editor
# make scene-shot  that scene drawn by the engine -> build/<name>/scene.png

# Only the Dreamcast build needs KOS; the Linux build and .glb conversion don't.
# Not KOS_CC: environ.sh exports that, and kos-cc runs it, so it must stay sh-elf-gcc.
DC_CC = $(if $(KOS_BASE),kos-cc,$(error KOS_BASE is not set: source the KallistiOS environ.sh first))

DMS_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))..)
NAME ?= $(notdir $(CURDIR))
SRCS ?= main.c
BAKE ?= 0
ENGINE ?= dc_engine dc_input dc_camera dc_model dc_player dc_draw2d dc_draw dc_debug collision dc_scene
ENGINE += $(ENGINE_EXTRA)

# Converted assets go to pc/, read over dcload at /pc or from the disc at /cd.
PC  := pc
OUT := $(DMS_ROOT)/build/$(NAME)
ELF := $(OUT)/$(NAME).elf
CDI := $(OUT)/$(NAME).cdi

CFLAGS += -std=gnu2x -O3 -I$(DMS_ROOT) -I$(CURDIR) \
          -fomit-frame-pointer -flto -fbuiltin -ffast-math -ffp-contract=fast -mfsrra -mfsca \
          -fmerge-all-constants -funroll-loops -fno-PIC -fipa-pta \
          -ftree-vectorize -I$(KOS_BASE)/utils -MMD -MP $(DEFINES)
LIBS = -lsh4zam -lkosutils -lm -Wl,--gc-sections

# Each project builds its own engine objects, so per-project flags never clash.
# Only the project's own sources bake in ASSETS, so disc builds recompile just those.
ENGINE_OBJS = $(addprefix $(OUT)/dms/,$(addsuffix .o,$(ENGINE)))
OBJS        = $(addprefix $(OUT)/dc/,$(SRCS:.c=.o))
DISC_OBJS   = $(addprefix $(OUT)/disc/,$(SRCS:.c=.o))

.PHONY: all assets assets-clean clean run disc dist host host-run shot check
all: assets $(ELF)

$(OUT)/dms/%.o: $(DMS_ROOT)/dms/%.c
	@mkdir -p $(@D)
	$(DC_CC) $(CFLAGS) -c $< -o $@
$(OUT)/dc/%.o: %.c
	@mkdir -p $(@D)
	$(DC_CC) $(CFLAGS) -c $< -o $@
$(OUT)/disc/%.o: %.c
	@mkdir -p $(@D)
	$(DC_CC) $(CFLAGS) -DASSETS='"/cd/"' -c $< -o $@

$(ELF): $(OBJS) $(ENGINE_OBJS)
	$(DC_CC) -o $@ $^ $(LIBS)
$(OUT)/$(NAME)-disc.elf: $(DISC_OBJS) $(ENGINE_OBJS)
	$(DC_CC) -o $@ $^ $(LIBS)

run: all
	$(KOS_LOADER) $(ELF) -c $(PC)

disc: assets $(OUT)/$(NAME)-disc.elf
	mkdcdisc -e $(OUT)/$(NAME)-disc.elf -D $(PC) -o $(CDI) -N

dist: $(ELF)
	$(KOS_STRIP) $(ELF)

# The DC ELF, when KOS is here, tells the Linux build where the heap starts.
HOST_EXE = $(OUT)/host/$(NAME)
SHOT_FRAMES ?= 60
host: assets $(if $(KOS_BASE),$(ELF))
	$(MAKE) -C $(DMS_ROOT)/host PROJECT=$(CURDIR) NAME=$(NAME) SRCS="$(SRCS)" ENGINE="$(ENGINE)" DEFINES="$(DEFINES)"
host-run: host
	$(HOST_EXE)
shot: host
	$(HOST_EXE) --headless --frames $(SHOT_FRAMES) --shot $(OUT)/shot.png
CHECK_FRAMES ?= 300
check: host
	@$(DMS_ROOT)/tools/check.sh $(HOST_EXE) $(CURDIR) $(CHECK_FRAMES)

clean:
	-rm -rf $(OUT)
assets-clean:
	-rm -rf $(PC)

# --- Assets: assets/<dir>/<file> -> pc/<dir>/<file> ---------------------------
STRIPPY = $(DMS_ROOT)/tools/converter/strippy
DCACONV = $(DMS_ROOT)/tools/converter/dcaconv/dcaconv
PVRTEX  = $(KOS_BASE)/utils/pvrtex/pvrtex
STRIPPY_FLAGS = $(if $(filter 1,$(BAKE)),-bake,)
# Long, mono, looping, for streaming (PCM16: proven; ADPCM is 4x smaller).
MUSIC_FLAGS = -f PCM16 --long --loop

# GENERATED_GLBS: models a project's own rule creates under assets/.
GLBS  = $(sort $(wildcard assets/*/*.glb) $(GENERATED_GLBS))
PNGS  = $(wildcard assets/*/*.png)
AUDIO = $(wildcard assets/*/*.wav) $(wildcard assets/*/*.mp3)
BINS  = $(wildcard assets/*/*.bin)
SCENES = $(wildcard assets/*/*.json)

assets: $(patsubst assets/%.glb,$(PC)/%.dms,$(GLBS)) \
        $(patsubst assets/%.png,$(PC)/%.dt,$(PNGS)) \
        $(patsubst assets/%,$(PC)/%.dca,$(basename $(AUDIO))) \
        $(patsubst assets/%,$(PC)/%,$(BINS) $(SCENES))

$(STRIPPY):
	$(MAKE) -C $(DMS_ROOT)/tools/converter
$(DCACONV):
	$(MAKE) -C $(DMS_ROOT)/tools/converter/dcaconv

$(PC)/%.dms: assets/%.glb | $(STRIPPY)
	@mkdir -p $(@D)
	@echo "=== Converting $< $(if $(filter 1,$(BAKE)),(BAKED),) ==="
	cp $< $(@D)/
	cd $(@D) && $(STRIPPY) $(STRIPPY_FLAGS) $(notdir $<) && rm -f $(notdir $<)

$(PC)/%.dt: assets/%.png
	@mkdir -p $(@D)
	$(PVRTEX) -i $< -o $@ -f auto

# Sound effects fit one AICA channel (dcaconv resamples anything over 2^16 samples).
$(PC)/music/%.dca: assets/music/%.wav | $(DCACONV)
	@mkdir -p $(@D)
	$(DCACONV) -i $< -o $@ $(MUSIC_FLAGS)
$(PC)/music/%.dca: assets/music/%.mp3 | $(DCACONV)
	@mkdir -p $(@D)
	$(DCACONV) -i $< -o $@ $(MUSIC_FLAGS)
$(PC)/%.dca: assets/%.wav | $(DCACONV)
	@mkdir -p $(@D)
	$(DCACONV) -i $< -o $@
$(PC)/%.dca: assets/%.mp3 | $(DCACONV)
	@mkdir -p $(@D)
	$(DCACONV) -i $< -o $@

$(PC)/%.bin: assets/%.bin
	@mkdir -p $(@D)
	cp $< $@

# Scenes (dc_scene) are used as they are.
$(PC)/%.json: assets/%.json
	@mkdir -p $(@D)
	cp $< $@

-include $(OBJS:.o=.d) $(DISC_OBJS:.o=.d) $(ENGINE_OBJS:.o=.d)

# The scene editor on this project (tools/editor), and its scene drawn by the
# engine to a PNG without a window: make scene-shot SCENE=scenes/main.json
EDITOR = $(DMS_ROOT)/build/editor/dms-editor
SCENE ?= scenes/main.json
.PHONY: edit scene-shot
$(EDITOR): FORCE
	$(MAKE) -C $(DMS_ROOT)/tools/editor
FORCE:
edit: assets $(EDITOR)
	$(EDITOR) $(CURDIR) --scene $(SCENE)
scene-shot: assets $(EDITOR)
	$(EDITOR) $(CURDIR) --scene $(SCENE) --shot $(OUT)/scene.png
