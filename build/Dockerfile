# blip build and test image. Ubuntu 26.04 (glibc, needed by AddressSanitizer in the
# tests); the release binary is static, so the server needs nothing installed.
#   stage "base": toolchain only (tagged blip-build:ubuntu-26.04 by ./build.sh)
#   stage "dist": copies the source and runs `make dist/blip` (used by ./install.sh)
FROM ubuntu:26.04@sha256:d66b366dac61b575bee2aef07e9c26e8070096fbe84a3d4d3eb0da069b5d76db AS base
RUN apt-get update \
 && apt-get install -y --no-install-recommends gcc make libc6-dev libzstd-dev python3 binutils file \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src

FROM base AS dist
COPY Makefile ./
COPY src ./src
RUN make dist/blip
