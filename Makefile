# Top-level shortcuts. Each project also builds on its own: make -C examples/teapot
#
# make teapot           build one example or game (assets + ELF)
# make teapot/run       any project target: run, disc, assets, clean, assets-clean
# make all              build every project
# make host EXAMPLE=x   Linux desktop preview of an example (needs SDL3)
# make converter        build the .glb -> .dms converter

PROJECTS := $(patsubst %/Makefile,%,$(wildcard examples/*/Makefile games/*/Makefile))
NAMES    := $(notdir $(PROJECTS))
dir_of    = $(filter %/$(1),$(PROJECTS))

.PHONY: all list clean host converter $(NAMES)
all: $(NAMES)
list:
	@echo $(NAMES)
$(NAMES):
	$(MAKE) -C $(call dir_of,$@)
TARGETS  := run disc assets clean assets-clean dist
$(foreach n,$(NAMES),$(foreach t,$(TARGETS),$(eval $(n)/$(t): ; $$(MAKE) -C $(call dir_of,$(n)) $(t))))

host:
	$(MAKE) -C host run
converter:
	$(MAKE) -C tools/converter

clean:
	-rm -rf build
