#!/bin/bash
# Restore the distribution's original videodev/hantro modules.
set -euo pipefail
KVER=$(uname -r)
BK=/var/backups/rk3399-vaapi/$KVER
if [ ! -f "$BK/videodev.ko" ] || [ ! -f "$BK/hantro-vpu.ko" ]; then
	echo "no backup in $BK; reinstall the kernel package instead" >&2
	exit 1
fi
cp -f "$BK/videodev.ko"   /lib/modules/$KVER/kernel/drivers/media/v4l2-core/videodev.ko
cp -f "$BK/hantro-vpu.ko" /lib/modules/$KVER/kernel/drivers/media/platform/verisilicon/hantro-vpu.ko
depmod -a "$KVER"
echo "restored; reboot recommended"
