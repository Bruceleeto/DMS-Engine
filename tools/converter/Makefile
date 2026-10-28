TRISTIPPER_DIR = libs/TriStripper
MESHOPT_DIR = meshoptimizer/src
MESHOPT_SRC = $(MESHOPT_DIR)/simplifier.cpp $(MESHOPT_DIR)/allocator.cpp

all: library main

library:
	$(MAKE) -C $(TRISTIPPER_DIR)

main: library
	c++ -O3 -fopenmp main.cpp $(MESHOPT_SRC) -o strippy -L$(TRISTIPPER_DIR) -lTriStripper -lSDL2 -lGL -lGLU -lm
clean:
	$(MAKE) -C $(TRISTIPPER_DIR) clean
	rm -f strippy

.PHONY: all library main clean
