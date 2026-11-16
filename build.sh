#!/bin/bash
# Builds the headless test/render tool (native) and the SDL2 simulator.
# SDL2 from Homebrew in /usr/local is x86_64, so the simulator is built for
# x86_64 and runs under Rosetta. Override with ARCH=arm64 if your SDL2 is arm64.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p bin out state
CXX=${CXX:-clang++}
FLAGS="-std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter"
ENGINE="engine/groovebox.cpp engine/synth.cpp"

echo "== render/test tool"
$CXX $FLAGS $ENGINE tools/render.cpp -o bin/render

echo "== simulator"
ARCH=${ARCH:-$(file -L "$(sdl2-config --prefix)/lib/libSDL2.dylib" | grep -q arm64 && echo arm64 || echo x86_64)}
$CXX $FLAGS -arch "$ARCH" $(sdl2-config --cflags) -Wno-deprecated-declarations $ENGINE sim/main.cpp $(sdl2-config --libs) -framework CoreMIDI -framework CoreFoundation -o bin/provs-groove-sim
echo "ok: bin/render, bin/provs-groove-sim ($ARCH)"
