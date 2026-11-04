#!/bin/bash
# Build the encoder-capable hantro driver and the encode-capable videodev
# against a configured kernel source tree matching the running kernel.
# usage: build-kernel-modules.sh /path/to/linux-7.3.1
set -euo pipefail
SRC=${1:?usage: $0 <kernel-source-tree>}
REPO=$(cd "$(dirname "$0")/.." && pwd)
KVER=$(uname -r)

if [ ! -f "$SRC/.config" ]; then
	cp "/boot/config-$KVER" "$SRC/.config"
	make -C "$SRC" olddefconfig
	make -C "$SRC" modules_prepare
fi

# Encode UAPI + v4l2-ctrl type support (the control types live in videodev).
cp "$REPO/kernel/hantro-encoder/uapi/v4l2-controls.h" "$SRC/include/uapi/linux/"
cp "$REPO/kernel/hantro-encoder/uapi/videodev2.h"     "$SRC/include/uapi/linux/"
cp "$REPO/kernel/hantro-encoder/uapi/v4l2-ctrls-core.c" "$SRC/drivers/media/v4l2-core/"
cp "$REPO/kernel/hantro-encoder/uapi/v4l2-ctrls-defs.c" "$SRC/drivers/media/v4l2-core/"

make -C "$SRC" KBUILD_MODPOST_WARN=1 drivers/media/v4l2-core/videodev.ko
make -C "$SRC" M="$REPO/kernel/hantro-encoder" KBUILD_MODPOST_WARN=1 modules

echo "built: $SRC/drivers/media/v4l2-core/videodev.ko"
echo "built: $REPO/kernel/hantro-encoder/hantro-vpu.ko"
echo "next: sudo $REPO/scripts/install-kernel-modules.sh $SRC"
