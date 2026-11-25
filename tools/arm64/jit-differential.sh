#!/bin/sh
# Builds tests/jit-differential.cpp for arm64 Linux inside the frond-arm64-cross image and runs it under
# qemu-aarch64. /src is the checkout with a copy of core/ over core/; /work is the build folder.
# Arguments go to the test (default: --seeds 1:3000). BUILD_ONLY=1 stops after the build.
# FROND_ARM64_FASTMEM_CHECKS=1 also runs the arena fault and FCSE checks.
set -eu
cd /src
JOBS=${JOBS:-8}
TRIPLE=aarch64-linux-gnu
export PKG_CONFIG_LIBDIR=/usr/lib/$TRIPLE/pkgconfig:/usr/share/pkgconfig
SDL_CFLAGS=$(pkg-config --cflags sdl2)
SDL_LIBS=$(pkg-config --libs sdl2)
TOOLS="CC_NATIVE=$TRIPLE-gcc CXX_NATIVE=$TRIPLE-g++ LD_NATIVE=$TRIPLE-g++ AR_NATIVE=$TRIPLE-ar RANLIB_NATIVE=$TRIPLE-ranlib"
DYN=/work/dynarmic
cmake -S vendor/dynarmic -B $DYN -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER=$TRIPLE-gcc -DCMAKE_CXX_COMPILER=$TRIPLE-g++ -DBoost_INCLUDE_DIR=/usr/include \
    -DDYNARMIC_TESTS=OFF -DDYNARMIC_USE_BUNDLED_EXTERNALS=ON -DDYNARMIC_WARNINGS_AS_ERRORS=OFF \
    -DDYNARMIC_FRONTENDS=A32 -DDYNARMIC_USE_PRECOMPILED_HEADERS=OFF
cmake --build $DYN --target dynarmic --parallel "$JOBS"
JIT="FROND_DYNARMIC_SRC=/src/vendor/dynarmic"
DYN_LIBS="$DYN/src/dynarmic/libdynarmic.a $DYN/externals/mcl/src/libmcl.a $DYN/externals/fmt/libfmt.a"
JIT_LIBS="FROND_DYNARMIC_LIBS=$DYN_LIBS"
make -j"$JOBS" -C core/src/common bin $TOOLS "CFLAGS_NATIVE=-O2 -g $SDL_CFLAGS"
make -j"$JOBS" -C core/src/common net $TOOLS
make -j"$JOBS" -C core/src/uarm bin $TOOLS "CFLAGS_NATIVE=-O2 -g $SDL_CFLAGS" \
    "LDFLAGS_NATIVE=$SDL_LIBS -lSDL2_image -lreadline -lcurl -lpthread -lresolv" "$JIT" "$JIT_LIBS"
find core/src/uarm/.build -name '*.o' ! -path '*/native/*' > /work/core.rsp
link_core_test() {
    $TRIPLE-g++ -std=c++17 -O2 -g -Icore/src/uarm/uarm -Icore/src/uarm -Icore/src/common \
        "$1" @/work/core.rsp core/src/common/libcommon-net.a core/src/common/libcommon.a \
        -lpthread $(pkg-config --libs slirp) $DYN_LIBS -lreadline -lresolv -o "$2"
}

link_core_test tests/jit-differential.cpp /work/test-jit-differential
if [ "${FROND_ARM64_FASTMEM_CHECKS:-0}" = 1 ]; then
    core_uarm=core/src/uarm/uarm
    $TRIPLE-g++ -std=c++17 -O2 -DFROND_DYNARMIC -D_FILE_OFFSET_BITS=64 -Wall -Wextra -Werror \
        -I"$core_uarm" -Ivendor/dynarmic/src -Ivendor/dynarmic/externals/mcl/include \
        tests/jit-fastmem-arm64.cpp "$core_uarm/jit_fastmem_arena.cpp" "$core_uarm/guest_memory.cpp" \
        $DYN_LIBS -lpthread -o /work/test-jit-fastmem-arm64
    link_core_test tests/arm920-fcse.cpp /work/test-arm920-fcse
fi
[ "${BUILD_ONLY:-0}" = 1 ] && exit 0
if [ "${FROND_ARM64_FASTMEM_CHECKS:-0}" = 1 ]; then
    FROND_JIT_FASTMEM=1 qemu-aarch64 -L /usr/aarch64-linux-gnu /work/test-jit-fastmem-arm64
    qemu-aarch64 -L /usr/aarch64-linux-gnu /work/test-arm920-fcse 2>/work/arm920-fcse.err || \
        { tail -10 /work/arm920-fcse.err; exit 1; }
fi
[ $# -gt 0 ] || set -- --seeds 1:3000
echo "qemu-aarch64 test-jit-differential $*"
qemu-aarch64 -L /usr/aarch64-linux-gnu /work/test-jit-differential "$@" 2>/work/jit-differential.err || \
    { grep -v ' failed (pc=' /work/jit-differential.err | tail -5; exit 1; }
