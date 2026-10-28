# gmod2glb: Garry's Mod models to .glb (then strippy makes .dms of them)
CXXFLAGS := -std=gnu++17 -O2 -g -Wall -Wno-unused-result

MESHOPT := ../converter/meshoptimizer/src
SRC := main.cpp vfs.cpp material.cpp mdl.cpp bsp.cpp $(MESHOPT)/simplifier.cpp $(MESHOPT)/allocator.cpp

gmod2glb: $(SRC) vfs.h material.h mdl.h bsp.h glb.h
	$(CXX) $(CXXFLAGS) $(SRC) -o $@ -lz -llzma

clean:
	rm -f gmod2glb

.PHONY: clean
