#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
${CC:-gcc} -m32 -O2 -I"$root" -I"$root/kos" -c "$root/kos_prim.c" -o "$build/prim.o"
${CC:-gcc} -m32 -std=gnu99 -O2 -I"$root" -I"$root/kos" -c "$root/test/depth_layers.c" -o "$build/test.o"
${CXX:-g++} -m32 -std=c++17 -O2 -msse2 -mfpmath=sse -pthread \
    -I"$root" -I"$root/kos" "$build/test.o" \
    "$root/kos_pvr.cpp" "$root/libpvr/pvr.cpp" "$root/libpvr/ta.cpp" \
    "$root/libpvr/core.cpp" "$root/libpvr/texture.cpp" "$build/prim.o" \
    -o "$build/depth_layers"
"$build/depth_layers"
