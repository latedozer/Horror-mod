#!/bin/bash
# Builds d2rl-horror-night.dll with mingw-w64 (Arch: pacman -S mingw-w64-gcc).
# Needs the D2RLoader Plugin SDK headers: https://github.com/D2RLoader/PluginSDK
#   SDK=/path/to/PluginSDK/include ./build.sh
# The sounds are not built in: see ../tools/get-sounds.sh.
set -e
cd "$(dirname "$0")"
SDK="${SDK:-../PluginSDK/include}"
[ -f "$SDK/D2RLPlugin/api.h" ] || { echo "Plugin SDK headers not found at $SDK (set SDK=...)"; exit 1; }
mkdir -p build
x86_64-w64-mingw32-windres -I "$SDK" horror-night.rc -O coff -o build/resources.o
x86_64-w64-mingw32-g++ -std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -I "$SDK" \
  -shared -static -static-libgcc -static-libstdc++ \
  -o build/d2rl-horror-night.dll horror_night.cpp build/resources.o -lwinmm
ls -la build/d2rl-horror-night.dll
