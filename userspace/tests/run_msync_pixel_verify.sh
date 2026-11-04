#!/bin/bash
# run_msync_pixel_verify.sh — verify decoded pixel correctness post-msync-removal.
#
# iter5 sweep commit d3a299b removed msync(MS_SYNC|MS_INVALIDATE) from the
# CAPTURE buffer DQBUF path alongside the iter1 patch-0010 hex-dump diagnostic.
# iter5 Phase 5 sonnet caveat C3 flagged: no formal pixel-correctness check
# was done. This script is that check.
#
# Approach:
#   1. SW reference: ffmpeg libavcodec H.264 decode of bbb_1080p30_h264.mp4,
#      first 100 frames, NV12 raw output -> sw_ref.yuv.
#   2. HW subject: same input through our v4l2_request driver via
#      ffmpeg -hwaccel vaapi -hwaccel_output_format vaapi
#             -i ... -vf hwdownload,format=nv12 -f rawvideo -pix_fmt nv12
#      Captures the post-DQBUF buffer through libva readback, exercising
#      the same code path we removed msync from.
#   3. Compare: byte-for-byte cmp + per-frame sha256.
#
# Pass: byte-for-byte identical (or per-frame sha matches) -> msync
# verifiably unnecessary on this hardware/kernel; iter5 sonnet C3 closes.
# Fail: divergence; restore msync in surface.c, re-run, document outcome.
#
# Usage: ./run_msync_pixel_verify.sh [fixture_path]
# If no argument, defaults to /home/mfritsche/fourier-test/bbb_1080p30_h264.mp4

set -eu

FIXTURE="${1:-/home/mfritsche/fourier-test/bbb_1080p30_h264.mp4}"
N_FRAMES=100
WORKDIR=$(mktemp -d -t msync_verify.XXXXXX)
trap 'rm -rf "$WORKDIR"' EXIT

if [[ ! -f "$FIXTURE" ]]; then
	echo "FAIL: fixture not found: $FIXTURE" >&2
	exit 2
fi

# Probe fixture dimensions for crop alignment of the HW path.
# Hantro pads height to MB boundaries (16-line align); FFmpeg SW decode
# returns crop-aligned (visible) frame size. Without explicit cropping
# on the HW side, hwdownload + format=nv12 emits MB-padded frames, which
# would diverge in size from SW even if pixels are correct.
read FIXTURE_W FIXTURE_H < <(ffprobe -v error -select_streams v:0 \
	-show_entries stream=width,height -of csv=p=0 "$FIXTURE" \
	| tr ',' ' ')
if [[ -z "${FIXTURE_W:-}" || -z "${FIXTURE_H:-}" ]]; then
	echo "FAIL: ffprobe could not read width/height from $FIXTURE" >&2
	exit 2
fi

echo "Fixture: $FIXTURE ($FIXTURE_W x $FIXTURE_H)"
echo "Frames:  $N_FRAMES"
echo "Workdir: $WORKDIR"
echo

# 1. SW reference
echo "[1/3] FFmpeg SW decode -> sw_ref.yuv"
ffmpeg -hide_banner -loglevel error -y \
	-i "$FIXTURE" \
	-frames:v "$N_FRAMES" \
	-f rawvideo -pix_fmt nv12 \
	"$WORKDIR/sw_ref.yuv"
SW_BYTES=$(stat -c %s "$WORKDIR/sw_ref.yuv")
SW_SHA=$(sha256sum "$WORKDIR/sw_ref.yuv" | cut -d' ' -f1)
echo "    sw_ref.yuv: $SW_BYTES bytes, sha256=$SW_SHA"

# 2. HW subject via libva v4l2_request
# Explicit crop=$FIXTURE_W:$FIXTURE_H after hwdownload normalizes any
# MB-padding the HW driver applies (hantro pads height to multiples of
# 16). Without this crop, an iter6+ correct decode could falsely
# diverge in total byte count from the SW reference.
echo "[2/3] FFmpeg HW decode via v4l2_request driver -> hw_capture.yuv"
env LIBVA_DRIVER_NAME=v4l2_request \
    LIBVA_V4L2_REQUEST_VIDEO_PATH=/dev/video1 \
    LIBVA_V4L2_REQUEST_MEDIA_PATH=/dev/media0 \
    ffmpeg -hide_banner -loglevel error -y \
	-hwaccel vaapi -hwaccel_output_format vaapi \
	-i "$FIXTURE" \
	-vf "hwdownload,format=nv12,crop=$FIXTURE_W:$FIXTURE_H:0:0" \
	-frames:v "$N_FRAMES" \
	-f rawvideo -pix_fmt nv12 \
	"$WORKDIR/hw_capture.yuv"
HW_BYTES=$(stat -c %s "$WORKDIR/hw_capture.yuv")
HW_SHA=$(sha256sum "$WORKDIR/hw_capture.yuv" | cut -d' ' -f1)
echo "    hw_capture.yuv: $HW_BYTES bytes, sha256=$HW_SHA"
echo

# 3. Compare
echo "[3/3] Compare"
if [[ "$SW_BYTES" -ne "$HW_BYTES" ]]; then
	# Diagnose stride/padding artifacts before declaring pixel
	# corruption. With explicit crop in step 2 this should not
	# happen, but if a future kernel change shifts the alignment
	# we want a clear diagnostic, not a false pixel-corruption
	# accusation.
	EXPECTED_SW=$(( FIXTURE_W * FIXTURE_H * 3 / 2 * N_FRAMES ))
	for PAD in 16 32; do
		PADDED_H=$(( (FIXTURE_H + PAD - 1) / PAD * PAD ))
		EXPECTED_PADDED=$(( FIXTURE_W * PADDED_H * 3 / 2 * N_FRAMES ))
		if [[ "$HW_BYTES" -eq "$EXPECTED_PADDED" ]]; then
			echo "DIAGNOSTIC: HW size $HW_BYTES matches MB-padded layout" >&2
			echo "  ($FIXTURE_W x $PADDED_H, $PAD-line align). The crop=$FIXTURE_W:$FIXTURE_H" >&2
			echo "  filter step did not normalize. Check FFmpeg version / hwdownload behavior." >&2
			echo "  This is a stride artifact, not pixel corruption." >&2
			exit 3
		fi
	done
	echo "FAIL: size mismatch (SW=$SW_BYTES vs HW=$HW_BYTES, expected $EXPECTED_SW)" >&2
	echo "      Different frame count or NV12 packing — investigate." >&2
	exit 1
fi

if [[ "$SW_SHA" == "$HW_SHA" ]]; then
	echo "PASS: byte-for-byte identical."
	echo "      msync removal verified safe on this hardware/kernel."
	exit 0
fi

# Per-frame divergence analysis on full-buffer mismatch.
echo "Buffer-level sha differs. Computing per-frame divergence..."
FRAME_SIZE=$(( SW_BYTES / N_FRAMES ))
DIVERGENT=0
for ((i = 0; i < N_FRAMES; i++)); do
	OFFSET=$(( i * FRAME_SIZE ))
	SW_FRAME_SHA=$(dd if="$WORKDIR/sw_ref.yuv" bs="$FRAME_SIZE" \
		count=1 skip="$i" 2>/dev/null | sha256sum | cut -d' ' -f1)
	HW_FRAME_SHA=$(dd if="$WORKDIR/hw_capture.yuv" bs="$FRAME_SIZE" \
		count=1 skip="$i" 2>/dev/null | sha256sum | cut -d' ' -f1)
	if [[ "$SW_FRAME_SHA" != "$HW_FRAME_SHA" ]]; then
		DIVERGENT=$(( DIVERGENT + 1 ))
		[[ "$DIVERGENT" -le 5 ]] && \
			echo "    frame $i: SW=$SW_FRAME_SHA HW=$HW_FRAME_SHA"
	fi
done

echo "FAIL: $DIVERGENT / $N_FRAMES frames diverge from SW reference."
echo "      Action: restore msync(MS_SYNC|MS_INVALIDATE) in surface.c"
echo "      RequestSyncSurface DQBUF path; re-run this script."
exit 1
