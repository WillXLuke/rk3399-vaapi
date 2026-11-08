#!/bin/sh
# Build libva-v4l2 and install it as the system's v4l2_drv_video.so.
#
#   ./scripts/build-vaapi-driver.sh [--install]
set -eu

HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE/vaapi-driver"

meson setup build --buildtype=release
ninja -C build

if [ "${1:-}" = "--install" ]; then
    cp build/src/v4l2_drv_video.so /usr/lib/aarch64-linux-gnu/dri/
    echo "installed; check with: LIBVA_DRIVER_NAME=v4l2 vainfo"
fi
