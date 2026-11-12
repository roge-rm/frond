#!/bin/sh
# Builds the binaries and the AppDir for one AppImage inside the frond-appimage-ARCH image.
# The checkout is at /src and core/ is copied to build/appimage/ARCH/source.
# ARCH is x86_64 or aarch64; aarch64 is cross built.
set -eu
cd /src
JOBS=${JOBS:-10}
ARCH=${ARCH:?}
TRIPLE=$ARCH-linux-gnu
WORK=/src/build/appimage/$ARCH
SRC=$WORK/source/src
if [ "$ARCH" = x86_64 ]; then
    CROSS=""
    TOOLCHAIN=""
    QTHOST=""
    RUNNER=""
    JIT_EXTRA="$WORK/dynarmic/externals/zydis/libZydis.a $WORK/dynarmic/externals/zydis/zycore/libZycore.a"
else
    export PKG_CONFIG_LIBDIR=/usr/lib/$TRIPLE/pkgconfig:/usr/share/pkgconfig
    CROSS="$TRIPLE-"
    TOOLCHAIN="-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=$ARCH -DCMAKE_C_COMPILER=${CROSS}gcc -DCMAKE_CXX_COMPILER=${CROSS}g++"
    QTHOST="-DCMAKE_FIND_ROOT_PATH=/usr/lib/$TRIPLE -DCMAKE_LIBRARY_ARCHITECTURE=$TRIPLE -DQT_HOST_PATH=/usr -DQT_HOST_PATH_CMAKE_DIR=/usr/lib/x86_64-linux-gnu/cmake"
    RUNNER="qemu-$ARCH"
    export QEMU_LD_PREFIX=/
    JIT_EXTRA=""
fi
CC="${CROSS}gcc"
CXX="${CROSS}g++"
# The C++ runtime is linked into the cores and the phone. The desktop app uses the system's, as Qt does.
STATIC="-static-libstdc++ -static-libgcc"
SDL_CFLAGS=$(pkg-config --cflags sdl2)
SDL_LIBS=$(pkg-config --libs sdl2)
# Debian 12's libslirp 4.7 calls slirp_ssize_t plain ssize_t, which is what 4.8 defines it as.
COMPAT=""
pkg-config --atleast-version=4.8 slirp || COMPAT="-Dslirp_ssize_t=ssize_t"
mk() { make -j"$JOBS" "CC_NATIVE=$CC $COMPAT" "CXX_NATIVE=$CXX $COMPAT" "AR_NATIVE=${CROSS}ar" "RANLIB_NATIVE=${CROSS}ranlib" "$@"; }
# Dynarmic for the Fast engine, from vendor/dynarmic.
DYN=$WORK/dynarmic
grep -qx 'CMAKE_HOME_DIRECTORY:INTERNAL=/src/vendor/dynarmic' $DYN/CMakeCache.txt 2>/dev/null || rm -rf $DYN
cmake -S vendor/dynarmic -B $DYN -G Ninja -DCMAKE_BUILD_TYPE=Release $TOOLCHAIN -DBoost_INCLUDE_DIR=/usr/include \
    -DDYNARMIC_TESTS=OFF -DDYNARMIC_USE_BUNDLED_EXTERNALS=ON -DDYNARMIC_WARNINGS_AS_ERRORS=OFF \
    -DDYNARMIC_FRONTENDS=A32 -DDYNARMIC_USE_PRECOMPILED_HEADERS=OFF
cmake --build $DYN --target dynarmic --parallel "$JOBS"
JIT="FROND_DYNARMIC_SRC=/src/vendor/dynarmic"
JIT_LIBS="FROND_DYNARMIC_LIBS=$DYN/src/dynarmic/libdynarmic.a $DYN/externals/mcl/src/libmcl.a $DYN/externals/fmt/libfmt.a $JIT_EXTRA"
mk -C $SRC/common bin "CFLAGS_NATIVE=-O2 -g $SDL_CFLAGS"
# The modem and phones' slirp side, linked into both cores and the Bluetooth phone.
mk -C $SRC/common net
mk -C $SRC/uarm bin "LD_NATIVE=$CXX $STATIC" "CFLAGS_NATIVE=-O2 -g $SDL_CFLAGS" \
    "LDFLAGS_NATIVE=$SDL_LIBS -lSDL2_image -lreadline -lcurl -lpthread -lresolv" "$JIT" "$JIT_LIBS"
OUT=$WORK/out
mkdir -p $OUT
cp $SRC/uarm/cp-uarm $OUT/frond
mk -C $SRC/cloudpilot FROND_68K=1 frond-68k "LD_NATIVE=$CXX $STATIC"
cp $SRC/cloudpilot/frond-68k $OUT/frond-68k
cmake -S desktop -B $WORK/desktop -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo $TOOLCHAIN $QTHOST -DFROND_PREPARED_SOURCE=$WORK/source
cmake --build $WORK/desktop --target frond-desktop --parallel "$JOBS"
cp $WORK/desktop/frond-desktop $OUT/frond-desktop
# btvirt, This Frond's Bluetooth radio, from the patched BlueZ checkout: the same files as tools/arm64/build.sh.
BLUEZ=/src/build/bluetooth-provider
BT=$WORK/btvirt
rm -rf $BT && mkdir -p $BT
printf '#define VERSION "5.87-frond"\n#define HAVE_LINUX_TYPES_H 1\n#define HAVE_LINUX_IF_ALG_H 1\n#define HAVE_SYS_RANDOM_H 1\n#define HAVE_GETRANDOM 1\n' > $BT/config.h
ln -s $BLUEZ/lib/bluetooth $BT/bluetooth
for f in emulator/main.c emulator/serial.c emulator/server.c emulator/vhci.c emulator/btdev.c emulator/bthost.c \
         emulator/smp.c emulator/phy.c emulator/le.c lib/bluetooth/bluetooth.c src/shared/queue.c src/shared/util.c \
         src/shared/crypto.c src/shared/ecc.c src/shared/io-mainloop.c src/shared/timeout-mainloop.c \
         src/shared/mainloop.c src/shared/mainloop-notify.c src/shared/ad.c lib/bluetooth/uuid.c; do
    $CC -O2 -DHAVE_CONFIG_H -I$BT -I$BLUEZ -c $BLUEZ/$f -o $BT/$(echo $f | tr / _).o
done
$CC -static-libgcc $BT/*.o -o $OUT/btvirt
# The Bluetooth phone: bluetooth-opp-peer --dun on bthost with the config patch, and the modem, PPP and slirp.
PHONE=$WORK/phone
rm -rf $PHONE && mkdir -p $PHONE/emulator
cp $BLUEZ/emulator/bthost.c $PHONE/emulator/bthost.c
patch --batch -p1 -d $PHONE -i /src/tools/bluez-bthost-config.patch
SLIRP=$(pkg-config --cflags --libs slirp)
$CC -std=gnu11 -O2 -DHAVE_CONFIG_H -I$BT -I$BLUEZ -I$BLUEZ/emulator -c $PHONE/emulator/bthost.c -o $PHONE/bthost.o
$CC -std=gnu11 -O2 -DHAVE_CONFIG_H -I$BT -I$BLUEZ -I$BLUEZ/emulator -Itools -c tools/bluetooth-opp-peer.c -o $PHONE/peer.o
$CXX $COMPAT -std=c++17 -O2 -I$SRC/common $SLIRP -c tools/bluetooth-dun.cpp -o $PHONE/dun.o
$CXX $STATIC $PHONE/peer.o $PHONE/dun.o $PHONE/bthost.o $(ls $BT/*.o | grep -v -e emulator_main -e emulator_bthost -e emulator_serial \
    -e emulator_server -e emulator_vhci -e emulator_btdev -e emulator_phy -e emulator_le) \
    $SRC/common/libcommon-net.a $SRC/common/libcommon.a $SLIRP -o $OUT/bluetooth-phone
for binary in frond frond-68k frond-desktop; do
    reported=$(QT_QPA_PLATFORM=offscreen $RUNNER $OUT/$binary --version)
    [ "${reported##* }" = "$(cat VERSION)" ] || { echo "$binary reports '$reported', expected $(cat VERSION)" >&2; exit 1; }
done
python3 -I tools/appimage/stage.py --arch "$ARCH" --binaries "$OUT" --appdir "$WORK/Frond.AppDir" --strip "${CROSS}strip"
