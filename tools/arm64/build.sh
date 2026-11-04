#!/bin/sh
# Cross builds the arm64 core, desktop and package inside the frond-arm64-cross image.
# The checkout is at /src with build/arm64/source mounted over build/source.
set -eu
cd /src
JOBS=${JOBS:-10}
TRIPLE=aarch64-linux-gnu
export PKG_CONFIG_LIBDIR=/usr/lib/$TRIPLE/pkgconfig:/usr/share/pkgconfig
SDL_CFLAGS=$(pkg-config --cflags sdl2)
SDL_LIBS=$(pkg-config --libs sdl2)
TOOLS="CC_NATIVE=$TRIPLE-gcc CXX_NATIVE=$TRIPLE-g++ LD_NATIVE=$TRIPLE-g++ AR_NATIVE=$TRIPLE-ar RANLIB_NATIVE=$TRIPLE-ranlib"
# Dynarmic for the Fast engine, from the source make dynarmic-source exported (arm64 back end).
DYN=/src/build/arm64/dynarmic
cmake -S build/dynarmic-trial/src -B $DYN -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER=$TRIPLE-gcc -DCMAKE_CXX_COMPILER=$TRIPLE-g++ -DBoost_INCLUDE_DIR=/usr/include \
    -DDYNARMIC_TESTS=OFF -DDYNARMIC_USE_BUNDLED_EXTERNALS=ON -DDYNARMIC_WARNINGS_AS_ERRORS=OFF \
    -DDYNARMIC_FRONTENDS=A32 -DDYNARMIC_USE_PRECOMPILED_HEADERS=OFF
cmake --build $DYN --target dynarmic --parallel "$JOBS"
JIT="FROND_DYNARMIC_SRC=/src/build/dynarmic-trial/src"
JIT_LIBS="FROND_DYNARMIC_LIBS=$DYN/src/dynarmic/libdynarmic.a $DYN/externals/mcl/src/libmcl.a $DYN/externals/fmt/libfmt.a"
make -j"$JOBS" -C build/source/src/common bin $TOOLS "CFLAGS_NATIVE=-O2 -g $SDL_CFLAGS"
make -j"$JOBS" -C build/source/src/uarm bin $TOOLS "CFLAGS_NATIVE=-O2 -g $SDL_CFLAGS" \
    "LDFLAGS_NATIVE=$SDL_LIBS -lSDL2_image -lreadline -lcurl -lpthread -lresolv" "$JIT" "$JIT_LIBS"
mkdir -p build/arm64/out
cp build/source/src/uarm/cp-uarm build/arm64/out/frond
# The 68k runner is built but not packaged yet.
make -j"$JOBS" -C build/source/src/cloudpilot FROND_68K=1 frond-68k $TOOLS
cp build/source/src/cloudpilot/frond-68k build/arm64/out/frond-68k
cmake -S desktop -B build/arm64/desktop -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER=$TRIPLE-gcc -DCMAKE_CXX_COMPILER=$TRIPLE-g++ \
    -DCMAKE_FIND_ROOT_PATH=/usr/lib/$TRIPLE -DCMAKE_LIBRARY_ARCHITECTURE=$TRIPLE \
    -DQT_HOST_PATH=/usr -DQT_HOST_PATH_CMAKE_DIR=/usr/lib/x86_64-linux-gnu/cmake
cmake --build build/arm64/desktop --target frond-desktop --parallel "$JOBS"
cp build/arm64/desktop/frond-desktop build/arm64/out/frond-desktop
export QEMU_LD_PREFIX=/
python3 tools/package-deb.py --core build/arm64/out/frond --desktop build/arm64/out/frond-desktop \
    --architecture arm64 --strip $TRIPLE-strip --runner qemu-aarch64
