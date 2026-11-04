#!/bin/bash
# Install the VA-API driver system-wide and add DRM-name aliases.
set -euo pipefail
cd "$(dirname "$0")/../userspace"
meson install -C build
DRI=$(pkg-config --variable=libdir libva 2>/dev/null || echo /usr/lib/aarch64-linux-gnu)/dri
cd "$DRI"
for alias in panfrost rockchip v4l2_request; do
	[ -e "${alias}_drv_video.so" ] || ln -sf v4l2_request_drv_video.so "${alias}_drv_video.so"
done
echo "installed to $DRI (aliases: panfrost/rockchip)"
