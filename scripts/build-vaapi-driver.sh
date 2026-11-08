#!/bin/sh
# Build libva-v4l2 and optionally install it as the system's v4l2_drv_video.so.
#
#   ./scripts/build-vaapi-driver.sh [--install]
#
# GCC 15 on some arm64 boards dies with "internal compiler error: Illegal
# instruction"; the script prefers gcc-14 when it is installed.
set -eu

HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE/vaapi-driver"

if [ -z "${CXX:-}" ] && command -v g++-14 >/dev/null 2>&1; then
    export CC=gcc-14 CXX=g++-14
fi

if [ -d build ]; then
    meson setup build --reconfigure --buildtype=release
else
    meson setup build --buildtype=release
fi
ninja -C build

if [ "${1:-}" = "--install" ]; then
    install -m755 build/src/v4l2_drv_video.so /usr/lib/aarch64-linux-gnu/dri/
    echo "installed; check with: LIBVA_DRIVER_NAME=v4l2 vainfo"
fi
