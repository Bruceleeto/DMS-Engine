TARGET = test.elf
OBJS = main.o ../dms/dc_engine.o ../dms/dc_input.o ../dms/dc_camera.o ../dms/dc_model.o \
       ../dms/dc_player.o ../dms/dc_draw2d.o ../dms/dc_debug.o ../dms/collision.o \
       romdisk.o

# Converted assets go here and get baked into the romdisk (mounted at /rd)
OUTPUT_DIR = romdisk
KOS_ROMDISK_DIR = $(OUTPUT_DIR)

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

.PHONY: all assets assets-clean clean rm-elf run dist

all: rm-elf $(TARGET)

# Build strippy if needed
$(STRIPPY):
	@echo "Building strippy converter..."
	$(MAKE) -C $(CONVERTER_DIR)

# make assets - convert all .glb in assets/ -> .dms + .dt in OUTPUT_DIR
GLBS = $(wildcard assets/*/*.glb)
DMS  = $(patsubst assets/%.glb,$(OUTPUT_DIR)/%.dms,$(GLBS))

assets: $(DMS)

$(OUTPUT_DIR)/%.dms: assets/%.glb | $(STRIPPY)
	@mkdir -p $(@D)
	@echo "=== Converting $< $(if $(filter 1,$(BAKE)),(BAKED),) ==="
	cp $< $(@D)/
	cd $(@D) && $(abspath $(STRIPPY)) $(STRIPPY_FLAGS) $(notdir $<) && rm -f $(notdir $<)

# Rebuild the romdisk image whenever a converted asset changes
romdisk.img: $(DMS)

include $(KOS_BASE)/Makefile.rules

clean: rm-elf
	-rm -f $(OBJS) romdisk.img

assets-clean:
	-rm -rf $(OUTPUT_DIR) romdisk.img romdisk.o

rm-elf:
	-rm -f $(TARGET)

$(TARGET): $(OBJS)
	kos-cc -o $(TARGET) $(OBJS) -lsh4zam -lkosutils -lm \
           -Wl,--gc-sections

run: $(TARGET)
	$(KOS_LOADER) $(TARGET)

dist: $(TARGET)
	-rm -f $(OBJS)
	$(KOS_STRIP) $(TARGET)