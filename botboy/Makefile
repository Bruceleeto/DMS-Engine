TARGET = botboy.elf
SRCS = main sound scene splash logo menu ui save game robot props enemies screens hud fx demo
OBJS = $(addsuffix .o,$(addprefix src/,$(SRCS))) ../dms/dc_engine.o ../dms/dc_input.o ../dms/dc_camera.o ../dms/dc_model.o \
       ../dms/dc_player.o ../dms/dc_draw2d.o ../dms/dc_draw.o ../dms/dc_debug.o ../dms/dc_particles.o ../dms/dc_audio.o ../dms/collision.o

# Converted assets go here and are read over dcload at /pc (run dc-tool with -c pc).
OUTPUT_DIR = pc

# Set BAKE=1 to bake vertex lighting (AO, shadows)
BAKE = 0

# Path to converter
CONVERTER_DIR = ../converter
STRIPPY = $(CONVERTER_DIR)/strippy
STRIPPY_FLAGS = $(if $(filter 1,$(BAKE)),-bake,)

CFLAGS += -std=gnu2x -O3 -I.. \
          -fomit-frame-pointer -flto -fbuiltin -ffast-math -ffp-contract=fast -mfsrra -mfsca \
          -fmerge-all-constants -funroll-loops -fno-PIC -fipa-pta \
          -ftree-vectorize -I$(KOS_BASE)/utils

# make disc builds with assets read from the disc, not dcload
ifdef DISC
CFLAGS += -DASSETS='"/cd/"'
endif
# make DEV=1: the dev keys in a level, Y for god mode and X+Y to record a demo
ifeq ($(DEV),1)
CFLAGS += -DBOTBOY_DEV
endif
DISC_IMAGE = $(notdir $(CURDIR)).cdi

# Source is in src/; the objects sit beside it.
.PHONY: all assets assets-clean clean rm-elf run dist disc

all: rm-elf $(TARGET)

# Build strippy if needed
$(STRIPPY):
	@echo "Building strippy converter..."
	$(MAKE) -C $(CONVERTER_DIR)

# make assets - convert all .glb in assets/ -> .dms + .dt in OUTPUT_DIR
GLBS = $(wildcard assets/*/*.glb)
DMS  = $(patsubst assets/%.glb,$(OUTPUT_DIR)/%.dms,$(GLBS))

# Images (backgrounds): assets/<dir>/<name>.png -> <name>.dt
PNGS = $(wildcard assets/*/*.png)
DTS  = $(patsubst assets/%.png,$(OUTPUT_DIR)/%.dt,$(PNGS))
PVRTEX = $(KOS_BASE)/utils/pvrtex/pvrtex

# Audio: assets/<dir>/<name>.wav|mp3 -> <name>.dca. Sound effects fit one AICA
# channel (dcaconv resamples anything over 2^16 samples). assets/music/ is
# converted long, mono, looping, for streaming (PCM16: proven; ADPCM is 4x smaller).
DCACONV = $(CONVERTER_DIR)/dcaconv/dcaconv
AUDIO_SRC = $(wildcard assets/*/*.wav) $(wildcard assets/*/*.mp3)
DCAS = $(patsubst assets/%.wav,$(OUTPUT_DIR)/%.dca,$(filter %.wav,$(AUDIO_SRC))) \
       $(patsubst assets/%.mp3,$(OUTPUT_DIR)/%.dca,$(filter %.mp3,$(AUDIO_SRC)))
MUSIC_FLAGS = -f PCM16 --long --loop

# Recorded demos: assets/demos/demo<level>.bin, copied as they are
BINS = $(patsubst assets/%.bin,$(OUTPUT_DIR)/%.bin,$(wildcard assets/*/*.bin))

assets: $(DMS) $(DTS) $(DCAS) $(BINS)

$(OUTPUT_DIR)/%.bin: assets/%.bin
	@mkdir -p $(@D)
	cp $< $@

$(DCACONV):
	@echo "Building dcaconv..."
	$(MAKE) -C $(CONVERTER_DIR)/dcaconv

$(OUTPUT_DIR)/music/%.dca: assets/music/%.wav | $(DCACONV)
	@mkdir -p $(@D)
	$(DCACONV) -i $< -o $@ $(MUSIC_FLAGS)
$(OUTPUT_DIR)/music/%.dca: assets/music/%.mp3 | $(DCACONV)
	@mkdir -p $(@D)
	$(DCACONV) -i $< -o $@ $(MUSIC_FLAGS)
$(OUTPUT_DIR)/%.dca: assets/%.wav | $(DCACONV)
	@mkdir -p $(@D)
	$(DCACONV) -i $< -o $@
$(OUTPUT_DIR)/%.dca: assets/%.mp3 | $(DCACONV)
	@mkdir -p $(@D)
	$(DCACONV) -i $< -o $@

$(OUTPUT_DIR)/%.dt: assets/%.png
	@mkdir -p $(@D)
	$(PVRTEX) -i $< -o $@ -f auto

$(OUTPUT_DIR)/%.dms: assets/%.glb | $(STRIPPY)
	@mkdir -p $(@D)
	@echo "=== Converting $< $(if $(filter 1,$(BAKE)),(BAKED),) ==="
	cp $< $(@D)/
	cd $(@D) && $(abspath $(STRIPPY)) $(STRIPPY_FLAGS) $(notdir $<) && rm -f $(notdir $<)

include $(KOS_BASE)/Makefile.rules

clean: rm-elf
	-rm -f $(OBJS)

assets-clean:
	-rm -rf $(OUTPUT_DIR)

rm-elf:
	-rm -f $(TARGET)

$(TARGET): $(OBJS)
	kos-cc -o $(TARGET) $(OBJS) -lsh4zam -lkosutils -lm \
           -Wl,--gc-sections

run: $(TARGET)
	$(KOS_LOADER) $(TARGET)

# make disc - clean build reading from /cd, then a .cdi with pc/ as the disc root.
# Every object bakes the asset path, so they are all removed after: the next
# plain make rebuilds reading from /pc again (leaving them would link a dcload
# build that opens /cd and hangs on hardware with no disc in).
disc: assets
	$(MAKE) clean
	$(MAKE) $(TARGET) DISC=1
	mkdcdisc -e $(TARGET) -D $(OUTPUT_DIR) -o $(DISC_IMAGE) -N
	-rm -f $(filter src/%,$(OBJS))

dist: $(TARGET)
	-rm -f $(OBJS)
	$(KOS_STRIP) $(TARGET)