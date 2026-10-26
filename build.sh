#!/bin/sh
# Quick native build against your system's SDL2 (needs cmake, a C++17 compiler, SDL2 dev files).
# For the release builds see tools/build-linux.sh and tools/build-windows.sh.
set -e
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
echo "Built: build/SlimePush"
