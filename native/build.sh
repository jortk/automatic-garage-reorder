#!/usr/bin/env bash
set -euo pipefail
SELF=$(readlink -f "$0")
cd "$(dirname "$SELF")"

if command -v zig >/dev/null; then
    ZIG=(zig)
else
    ZIG=(nix run nixpkgs#zig --)
fi

export ZIG_GLOBAL_CACHE_DIR=$PWD/build/zig-cache ZIG_LOCAL_CACHE_DIR=$PWD/build/zig-cache
mkdir -p build

REPO=${SWOFTY_REPO:-../..}
SDK=${SDK_INCLUDE:-$REPO/packages/sdk/include}
GAME=${GAME_INCLUDE:-$REPO/native/include}
SAFETYHOOK_DIR=${SAFETYHOOK_DIR:-$REPO/native/third_party/safetyhook}
SAFETYHOOK_OBJ=${SAFETYHOOK_OBJ:-$REPO/native/build/safetyhook.o}
ZYDIS_OBJ=${ZYDIS_OBJ:-$REPO/native/build/Zydis.o}

echo "Running unit tests..."
"${ZIG[@]}" c++ -target x86_64-windows-gnu -std=c++23 -O2 \
    -o test_runner.exe test/garage_order_logic_test.cpp
./test_runner.exe
rm -f test_runner.exe test_runner.pdb

echo "Compiling pitlane_garage_order.dll..."
"${ZIG[@]}" c++ -target x86_64-windows-gnu -std=c++23 -O2 -Wall -Wextra -Wno-nullability-completeness \
    -I"$GAME" -I"$SDK" -I"$SAFETYHOOK_DIR" -D__USE_MINGW_ANSI_STDIO=1 -shared -s \
    -o pitlane_garage_order.dll src/pitlane_garage_order.cpp "$SAFETYHOOK_OBJ" "$ZYDIS_OBJ"

rm -f ./*.lib ./*.pdb
echo "built $(pwd)/pitlane_garage_order.dll"
