#!/bin/bash
# Reload the media stack so the new modules are picked up.
set -euo pipefail
for m in rockchip_vdec hantro_vpu rockchip_rga; do modprobe -r "$m" 2>/dev/null || true; done
for m in v4l2_mem2mem videobuf2_dma_contig videobuf2_dma_sg videobuf2_v4l2 \
	 videobuf2_memops videobuf2_common v4l2_jpeg v4l2_vp9 v4l2_h264; do
	modprobe -r "$m" 2>/dev/null || true
done
# Some systems need snd_usb_audio off to release the media controller.
modprobe -r snd_usb_audio 2>/dev/null || true
modprobe -r videodev mc 2>/dev/null || true

modprobe videodev
modprobe hantro_vpu
modprobe rockchip_vdec 2>/dev/null || true
modprobe rockchip_rga 2>/dev/null || true
modprobe snd_usb_audio 2>/dev/null || true
for v in /sys/class/video4linux/video*; do
	echo "$(basename "$v"): $(cat "$v/name" 2>/dev/null)"
done
