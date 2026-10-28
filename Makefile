# Top-level shortcuts. Each project also builds on its own: make -C examples/teapot
#
# make teapot           build one example or game (assets + ELF)
# make teapot/run       any project target: run, disc, assets, clean, assets-clean,
#                       host (Linux build), host-run, shot (PNG of frame 60)
# make all              build every project
# make converter        build the .glb -> .dms converter
# make editor           build the scene editor (make <name>/edit opens it)

PROJECTS := $(patsubst %/Makefile,%,$(wildcard examples/*/Makefile games/*/Makefile))
NAMES    := $(notdir $(PROJECTS))
dir_of    = $(filter %/$(1),$(PROJECTS))

.PHONY: all list clean converter editor engine $(NAMES)
all: $(NAMES)
list:
	@echo $(NAMES)
# The engine first, once, so projects built side by side don't race to build it
$(NAMES): engine
	$(MAKE) -C $(call dir_of,$@)
engine:
	$(MAKE) -f mk/engine.mk
TARGETS  := run disc flycast assets clean assets-clean dist host host-run shot check edit scene-shot
$(foreach n,$(NAMES),$(foreach t,$(TARGETS),$(eval $(n)/$(t): ; $$(MAKE) -C $(call dir_of,$(n)) $(t))))

converter:
	$(MAKE) -C tools/converter
editor:
	$(MAKE) -C tools/editor

clean:
	-rm -rf build
