#!/usr/bin/env bash
# Portable Linux / Steam Deck build: static SDL2, runs on glibc 2.29+ (every SteamOS 3 release).
# Uses zig as the C/C++ compiler so the binary targets an old glibc.
# Needs: cmake, git, python3 + pip, and the X11/Wayland/ALSA/Pulse/udev dev headers (see CI).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="$ROOT/.deps/linux"
SDL_TAG=release-2.30.9
mkdir -p "$DEPS"
python3 -m ziglang version >/dev/null 2>&1 || pip install --user ziglang==0.13.0 || pip install --break-system-packages ziglang==0.13.0

TC="$DEPS/zig"; mkdir -p "$TC"
for t in cc c++; do
  n=$([ "$t" = cc ] && echo zcc || echo zcxx)
  printf '#!/bin/sh\nexec python3 -m ziglang %s -target x86_64-linux-gnu.2.31 -idirafter /usr/include -idirafter /usr/include/x86_64-linux-gnu "$@"\n' "$t" > "$TC/$n"
done
printf '#!/bin/sh\nexec python3 -m ziglang ar "$@"\n' > "$TC/zar"
printf '#!/bin/sh\nexec python3 -m ziglang ranlib "$@"\n' > "$TC/zranlib"
chmod +x "$TC"/*
ZIG="-DCMAKE_C_COMPILER=$TC/zcc -DCMAKE_CXX_COMPILER=$TC/zcxx -DCMAKE_AR=$TC/zar -DCMAKE_RANLIB=$TC/zranlib"

if [ ! -f "$DEPS/sdl/lib/libSDL2.a" ]; then
  [ -d "$DEPS/SDL" ] || git clone -q --depth 1 --branch "$SDL_TAG" https://github.com/libsdl-org/SDL.git "$DEPS/SDL"
  cmake -S "$DEPS/SDL" -B "$DEPS/SDL-build" -DCMAKE_BUILD_TYPE=Release $ZIG \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF -DSDL_PIPEWIRE=OFF -DSDL_LIBSAMPLERATE=OFF \
    -DCMAKE_INSTALL_PREFIX="$DEPS/sdl"
  cmake --build "$DEPS/SDL-build" -j"$(nproc)"
  cmake --install "$DEPS/SDL-build"
fi

cmake -S "$ROOT" -B "$ROOT/build-linux" -DCMAKE_BUILD_TYPE=Release $ZIG \
  -DSDL2_DIR="$DEPS/sdl/lib/cmake/SDL2" -DSLIMEPUSH_STATIC_SDL=ON
cmake --build "$ROOT/build-linux" -j"$(nproc)"
strip "$ROOT/build-linux/SlimePush" 2>/dev/null || true

OUT="$ROOT/dist"; mkdir -p "$OUT/SlimePush"
cp "$ROOT/build-linux/SlimePush" "$ROOT/README.md" "$OUT/SlimePush/"
tar -C "$OUT" -czf "$OUT/SlimePush-linux-steamdeck.tar.gz" SlimePush
rm -rf "$OUT/SlimePush"
echo "Built $OUT/SlimePush-linux-steamdeck.tar.gz"
