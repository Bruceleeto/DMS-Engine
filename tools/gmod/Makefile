# gmod2glb: Garry's Mod models to .glb (then strippy makes .dms of them)
CXXFLAGS := -std=gnu++17 -O2 -g -Wall -Wno-unused-result

gmod2glb: main.cpp vfs.cpp material.cpp mdl.cpp vfs.h material.h mdl.h
	$(CXX) $(CXXFLAGS) main.cpp vfs.cpp material.cpp mdl.cpp -o $@ -lz -llzma

clean:
	rm -f gmod2glb

.PHONY: clean
