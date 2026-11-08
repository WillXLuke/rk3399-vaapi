#!/bin/sh
# Build the patched hantro/verisilicon modules against a configured kernel tree
# and install them into the running kernel's updates directory.
#
#   ./scripts/build-kernel-modules.sh /path/to/linux-7.3.3 [--install]
set -eu

KERNEL=${1:?usage: $0 <kernel-tree> [--install]}
HERE=$(cd "$(dirname "$0")/.." && pwd)
PATCH="$HERE/patches/0001-media-verisilicon-rk3399-h264-encoder.patch"

if ! grep -q HANTRO_H264_ENCODE_PARAMS "$KERNEL/drivers/media/platform/verisilicon/hantro.h" 2>/dev/null; then
    echo "applying $PATCH"
    (cd "$KERNEL" && patch -p1 < "$PATCH")
fi

make -C "$KERNEL" -j"$(nproc)" ARCH=arm64 KBUILD_MODPOST_WARN=1 \
    M=drivers/media/platform/verisilicon modules

if [ "${2:-}" = "--install" ]; then
    install -D -m644 "$KERNEL/drivers/media/platform/verisilicon/hantro-vpu.ko" \
        "/lib/modules/$(uname -r)/updates/hantro-vpu.ko"
    depmod -a
    modprobe -r hantro_vpu 2>/dev/null || true
    modprobe hantro_vpu
    echo "loaded; encoder node:"
    v4l2-ctl --list-devices | grep -A2 rk3399-vpu-enc || true
fi
