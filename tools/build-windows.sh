#!/usr/bin/env bash
# Windows build, cross-compiled from Linux with MinGW-w64. Produces one self-contained .exe.
# Needs: cmake, curl, zip, mingw-w64 (apt install mingw-w64).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="$ROOT/.deps/windows"
SDL_VER=2.30.9
mkdir -p "$DEPS"
SDL="$DEPS/SDL2-$SDL_VER/x86_64-w64-mingw32"
if [ ! -d "$SDL" ]; then
  curl -sSL -o "$DEPS/sdl.tgz" "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-devel-$SDL_VER-mingw.tar.gz"
  tar -C "$DEPS" -xzf "$DEPS/sdl.tgz"
fi
cmake -S "$ROOT" -B "$ROOT/build-windows" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/mingw-w64.cmake" \
  -DSDL2_DIR="$SDL/lib/cmake/SDL2" -DSLIMEPUSH_STATIC_SDL=ON
cmake --build "$ROOT/build-windows" -j"$(nproc)"
x86_64-w64-mingw32-strip "$ROOT/build-windows/SlimePush.exe"

OUT="$ROOT/dist"; mkdir -p "$OUT/SlimePush"
cp "$ROOT/build-windows/SlimePush.exe" "$ROOT/README.md" "$OUT/SlimePush/"
(cd "$OUT" && rm -f SlimePush-windows.zip && zip -qr SlimePush-windows.zip SlimePush)
rm -rf "$OUT/SlimePush"
echo "Built $OUT/SlimePush-windows.zip"
