#!/bin/bash
# Runs the tests.
#   SDK=/path/to/PluginSDK/include tests/run.sh [extracted game data folder]
# The plugin test is built for Windows like the plugin itself and run under Wine.
# The mod test needs game data you extracted yourself (see tests/mod.test.js); without the
# folder it is skipped.
set -e
cd "$(dirname "$0")/.."
SDK="${SDK:-../PluginSDK/include}"
[ -f "$SDK/D2RLPlugin/api.h" ] || { echo "Plugin SDK headers not found at $SDK (set SDK=...)"; exit 1; }
mkdir -p plugin/build
x86_64-w64-mingw32-g++ -std=c++20 -O1 -Wall -Wextra -fno-exceptions -fno-rtti -Wno-unknown-pragmas -Wno-missing-field-initializers \
  -I "$SDK" -I plugin -static -o plugin/build/plugin_test.exe tests/plugin_test.cpp -lwinmm
WINEDEBUG=-all wine plugin/build/plugin_test.exe plugin/horror-night.toml
if [ -n "$1" ]; then
	node tests/mod.test.js "$1"
else
	echo "(mod test skipped: no game data folder given)"
fi
