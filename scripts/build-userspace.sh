#!/bin/bash
# Build the VA-API driver (decode + H.264 encode) from userspace/.
set -euo pipefail
cd "$(dirname "$0")/.."
HDRS=${KERNEL_HDRS:-}
if [ -z "$HDRS" ]; then
	# The encoder needs the stateless-encode UAPI. Point KERNEL_HDRS at a
	# `make headers_install` tree from the patched kernel source, or leave
	# unset if /usr/include already has V4L2_CID_STATELESS_H264_ENCODE_*.
	if ! grep -qs STATELESS_H264_ENCODE_PARAMS /usr/include/linux/v4l2-controls.h; then
		echo "warning: system UAPI has no H.264 encode controls; set KERNEL_HDRS" >&2
	fi
fi
cd userspace
rm -rf build
meson setup build --buildtype=release --prefix=/usr \
	-Dlibdir=lib/aarch64-linux-gnu ${HDRS:+-Dkernel_headers="$HDRS"}
meson compile -C build
echo "built: userspace/build/src/v4l2_request_drv_video.so"
