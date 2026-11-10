# Debian 12 bookworm, for the AppImages: Qt 6.4 and glibc 2.36, so they run on bookworm and anything newer.
# ARCH=amd64 builds natively, ARCH=arm64 adds arm64 multiarch libraries and the cross compiler.
FROM debian:bookworm
ARG ARCH=amd64
RUN if [ "$ARCH" != amd64 ]; then dpkg --add-architecture $ARCH; CROSS="crossbuild-essential-$ARCH qemu-user"; fi \
    && apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential $CROSS make cmake ninja-build pkg-config python3 file patchelf binutils libboost-dev \
    qt6-base-dev-tools qt6-base-dev:$ARCH qt6-qpa-plugins:$ARCH qt6-wayland:$ARCH libqt6svg6:$ARCH \
    libsdl2-dev:$ARCH libsdl2-image-dev:$ARCH libreadline-dev:$ARCH libcurl4-openssl-dev:$ARCH \
    libslirp-dev:$ARCH zlib1g-dev:$ARCH mtools:$ARCH \
    && rm -rf /var/lib/apt/lists/*
