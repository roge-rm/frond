# Debian trixie amd64 with arm64 multiarch libraries, for cross building the arm64 package.
FROM debian:trixie
RUN dpkg --add-architecture arm64 && apt-get update && apt-get install -y --no-install-recommends \
    crossbuild-essential-arm64 make cmake pkg-config python3 dpkg-dev file qemu-user libboost-dev \
    qt6-base-dev:arm64 qt6-base-dev-tools libsdl2-dev:arm64 libsdl2-image-dev:arm64 \
    libreadline-dev:arm64 libcurl4-openssl-dev:arm64 libslirp-dev:arm64 zlib1g-dev:arm64 \
    && rm -rf /var/lib/apt/lists/*
