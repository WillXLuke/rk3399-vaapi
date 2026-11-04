#!/bin/bash
# Install the rebuilt videodev + hantro modules, backing up the originals.
set -euo pipefail
SRC=${1:?usage: $0 <kernel-source-tree>}
REPO=$(cd "$(dirname "$0")/.." && pwd)
KVER=$(uname -r)
BK=/var/backups/rk3399-vaapi/$KVER
mkdir -p "$BK"

VD_SRC="$SRC/drivers/media/v4l2-core/videodev.ko"
HV_SRC="$REPO/kernel/hantro-encoder/hantro-vpu.ko"
VD_DST="/lib/modules/$KVER/kernel/drivers/media/v4l2-core/videodev.ko"
HV_DST="/lib/modules/$KVER/kernel/drivers/media/platform/verisilicon/hantro-vpu.ko"

[ -f "$BK/videodev.ko" ]         || cp -a "$VD_DST" "$BK/videodev.ko"
[ -f "$BK/hantro-vpu.ko" ]       || cp -a "$HV_DST" "$BK/hantro-vpu.ko"

cp -f "$VD_SRC" "$VD_DST"
cp -f "$HV_SRC" "$HV_DST"
depmod -a "$KVER"
echo "installed; originals in $BK"
echo "reload with: sudo $REPO/scripts/load-media-stack.sh"
