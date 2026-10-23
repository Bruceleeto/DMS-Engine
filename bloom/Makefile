TARGET = test.elf
OBJS = main.o ../dms/dc_engine.o ../dms/dc_input.o ../dms/dc_camera.o ../dms/dc_model.o \
       ../dms/dc_player.o ../dms/dc_draw2d.o ../dms/dc_draw.o ../dms/dc_debug.o ../dms/collision.o

# Converted assets go here and are read over dcload at /pc (run dc-tool with -c pc).
# Assets are read over dcload at /pc (run dc-tool with -c pc).
OUTPUT_DIR = pc

# Set BAKE=1 to bake vertex lighting (AO, shadows)
BAKE = 1

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
DISC_IMAGE = $(notdir $(CURDIR)).cdi

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

assets: $(DMS) $(DTS)

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
# main.o is removed after so the next plain make reads from /pc again.
disc: assets
	$(MAKE) clean
	$(MAKE) $(TARGET) DISC=1
	mkdcdisc -e $(TARGET) -D $(OUTPUT_DIR) -o $(DISC_IMAGE) -N
	-rm -f main.o

dist: $(TARGET)
	-rm -f $(OBJS)
	$(KOS_STRIP) $(TARGET)