#!/usr/bin/env bash
set -euo pipefail

root=$PWD
out=${FROND_OBJ:-"$root/build/out"}
common=$root/core/src/common
evidence=${CARD_LOCK_TEST_DIR:-"$root/build/linked-card-lock"}
mkdir -p "$out" "$evidence"
fixture=$(mktemp -d "$evidence/case.XXXXXXXX")
trap 'rm -rf -- "$fixture"' EXIT

mode=$1
cc=${CC:-gcc}
cxx=${CXX:-g++}
flags=(-Wl,--wrap=flock)
runner=(timeout -k 2s 60s)
if [[ $mode == wine || $mode == windows ]]; then
    flags=(-D_FILE_OFFSET_BITS=64 -static -I"$common/winshim" -Wl,--wrap=rename -Wl,--wrap=LockFileEx)
    if [[ $mode == wine ]]; then
        cc=${MINGW_CC:-x86_64-w64-mingw32-gcc}
        cxx=${MINGW_CXX:-x86_64-w64-mingw32-g++-posix}
        export WINEPREFIX=${WINEPREFIX:-"$evidence/wine"}
        export WINEDEBUG=${WINEDEBUG:--all}
        export WINEDLLOVERRIDES=${WINEDLLOVERRIDES:-"wineboot.exe,mscoree,mshtml="}
        runner+=("${WINELOADER:-wine}")
    fi
    mode+=.exe
fi

"$cc" -O1 -I"$common" -c "$common/zip/miniz.c" -o "$out/linked-card-miniz-$mode.o"
cd core/src/cloudpilot
mapfile -t dirs < <(sed -n 's/^\s*\(-I[^ ]*\).*/\1/p' Makefile)
"$cxx" -std=c++17 -O1 -ffunction-sections -fdata-sections -Wl,--gc-sections "${flags[@]}" \
    -DEMULATION_LEVEL=EMULATION_UNIX -Ifrond -Inative -I../uarm/native -I../uarm/uarm "${dirs[@]}" \
    "$root/tests/linked-card-lock.cpp" emulator/hardware/EmHAL.cpp emulator/ExternalStorage.cpp \
    emulator/assert_native.cpp emulator/stacktrace.cpp "$common/md5.cpp" "$common/CardImage.cpp" \
    "$common/Logging.cpp" "$common/savestate/Chunk.cpp" "$common/savestate/ChunkProbe.cpp" \
    "$out/linked-card-miniz-$mode.o" -o "$out/linked-card-lock-$mode"
cd "$fixture"
ulimit -c 0
"${runner[@]}" "$out/linked-card-lock-$mode"
