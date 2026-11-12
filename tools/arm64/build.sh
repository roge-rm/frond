#!/bin/sh
# Cross builds the arm64 core, desktop and package inside the frond-arm64-cross image.
# The checkout is at /src with build/arm64/source, a copy of core/, mounted over core/.
set -eu
cd /src
JOBS=${JOBS:-10}
TRIPLE=aarch64-linux-gnu
export PKG_CONFIG_LIBDIR=/usr/lib/$TRIPLE/pkgconfig:/usr/share/pkgconfig
SDL_CFLAGS=$(pkg-config --cflags sdl2)
SDL_LIBS=$(pkg-config --libs sdl2)
TOOLS="CC_NATIVE=$TRIPLE-gcc CXX_NATIVE=$TRIPLE-g++ LD_NATIVE=$TRIPLE-g++ AR_NATIVE=$TRIPLE-ar RANLIB_NATIVE=$TRIPLE-ranlib"
# Dynarmic for the Fast engine, from vendor/dynarmic (arm64 back end).
DYN=/src/build/arm64/dynarmic
grep -qx 'CMAKE_HOME_DIRECTORY:INTERNAL=/src/vendor/dynarmic' $DYN/CMakeCache.txt 2>/dev/null || rm -rf $DYN
cmake -S vendor/dynarmic -B $DYN -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER=$TRIPLE-gcc -DCMAKE_CXX_COMPILER=$TRIPLE-g++ -DBoost_INCLUDE_DIR=/usr/include \
    -DDYNARMIC_TESTS=OFF -DDYNARMIC_USE_BUNDLED_EXTERNALS=ON -DDYNARMIC_WARNINGS_AS_ERRORS=OFF \
    -DDYNARMIC_FRONTENDS=A32 -DDYNARMIC_USE_PRECOMPILED_HEADERS=OFF
cmake --build $DYN --target dynarmic --parallel "$JOBS"
JIT="FROND_DYNARMIC_SRC=/src/vendor/dynarmic"
JIT_LIBS="FROND_DYNARMIC_LIBS=$DYN/src/dynarmic/libdynarmic.a $DYN/externals/mcl/src/libmcl.a $DYN/externals/fmt/libfmt.a"
make -j"$JOBS" -C core/src/common bin $TOOLS "CFLAGS_NATIVE=-O2 -g $SDL_CFLAGS"
# The modem and phones' slirp side, linked into both cores and the Bluetooth phone.
make -j"$JOBS" -C core/src/common net $TOOLS
make -j"$JOBS" -C core/src/uarm bin $TOOLS "CFLAGS_NATIVE=-O2 -g $SDL_CFLAGS" \
    "LDFLAGS_NATIVE=$SDL_LIBS -lSDL2_image -lreadline -lcurl -lpthread -lresolv" "$JIT" "$JIT_LIBS"
mkdir -p build/arm64/out
cp core/src/uarm/cp-uarm build/arm64/out/frond
make -j"$JOBS" -C core/src/cloudpilot FROND_68K=1 frond-68k $TOOLS
cp core/src/cloudpilot/frond-68k build/arm64/out/frond-68k
cmake -S desktop -B build/arm64/desktop -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFROND_PREPARED_SOURCE=/src/core \
    -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER=$TRIPLE-gcc -DCMAKE_CXX_COMPILER=$TRIPLE-g++ \
    -DCMAKE_FIND_ROOT_PATH=/usr/lib/$TRIPLE -DCMAKE_LIBRARY_ARCHITECTURE=$TRIPLE \
    -DQT_HOST_PATH=/usr -DQT_HOST_PATH_CMAKE_DIR=/usr/lib/x86_64-linux-gnu/cmake
cmake --build build/arm64/desktop --target frond-desktop --parallel "$JOBS"
cp build/arm64/desktop/frond-desktop build/arm64/out/frond-desktop
# btvirt, This Frond's Bluetooth radio, from the patched BlueZ checkout: the same files the Android build uses,
# plus ad.c and uuid.c for the phone's patched bthost.
BLUEZ=/src/build/bluetooth-provider
BT=/src/build/arm64/btvirt
rm -rf $BT && mkdir -p $BT
printf '#define VERSION "5.87-frond"\n#define HAVE_LINUX_TYPES_H 1\n#define HAVE_LINUX_IF_ALG_H 1\n#define HAVE_SYS_RANDOM_H 1\n#define HAVE_GETRANDOM 1\n' > $BT/config.h
ln -s $BLUEZ/lib/bluetooth $BT/bluetooth
for f in emulator/main.c emulator/serial.c emulator/server.c emulator/vhci.c emulator/btdev.c emulator/bthost.c \
         emulator/smp.c emulator/phy.c emulator/le.c lib/bluetooth/bluetooth.c src/shared/queue.c src/shared/util.c \
         src/shared/crypto.c src/shared/ecc.c src/shared/io-mainloop.c src/shared/timeout-mainloop.c \
         src/shared/mainloop.c src/shared/mainloop-notify.c src/shared/ad.c lib/bluetooth/uuid.c; do
    $TRIPLE-gcc -O2 -DHAVE_CONFIG_H -I$BT -I$BLUEZ -c $BLUEZ/$f -o $BT/$(echo $f | tr / _).o
done
$TRIPLE-gcc $BT/*.o -o build/arm64/out/btvirt
# The Bluetooth phone: bluetooth-opp-peer --dun on bthost with the config patch, and the modem, PPP and slirp.
PHONE=/src/build/arm64/phone
rm -rf $PHONE && mkdir -p $PHONE/emulator
cp $BLUEZ/emulator/bthost.c $PHONE/emulator/bthost.c
patch --batch -p1 -d $PHONE -i /src/tools/bluez-bthost-config.patch
SLIRP=$(pkg-config --cflags --libs slirp)
$TRIPLE-gcc -std=gnu11 -O2 -DHAVE_CONFIG_H -I$BT -I$BLUEZ -I$BLUEZ/emulator -c $PHONE/emulator/bthost.c -o $PHONE/bthost.o
$TRIPLE-gcc -std=gnu11 -O2 -DHAVE_CONFIG_H -I$BT -I$BLUEZ -I$BLUEZ/emulator -Itools -c tools/bluetooth-opp-peer.c -o $PHONE/peer.o
$TRIPLE-g++ -std=c++17 -O2 -Icore/src/common $SLIRP -c tools/bluetooth-dun.cpp -o $PHONE/dun.o
$TRIPLE-g++ $PHONE/peer.o $PHONE/dun.o $PHONE/bthost.o $(ls $BT/*.o | grep -v -e emulator_main -e emulator_bthost -e emulator_serial \
    -e emulator_server -e emulator_vhci -e emulator_btdev -e emulator_phy -e emulator_le) \
    core/src/common/libcommon-net.a core/src/common/libcommon.a $SLIRP -o build/arm64/out/bluetooth-phone
export QEMU_LD_PREFIX=/
python3 tools/package-deb.py --core build/arm64/out/frond --core-68k build/arm64/out/frond-68k --desktop build/arm64/out/frond-desktop \
    --btvirt build/arm64/out/btvirt --phone build/arm64/out/bluetooth-phone --architecture arm64 --strip $TRIPLE-strip --runner qemu-aarch64
