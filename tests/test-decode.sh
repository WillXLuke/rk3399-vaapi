#!/bin/bash
# Decode smoke test: VA-API H.264 decode must match software decode.
set -euo pipefail
DEV=${VAAPI_DEVICE:-/dev/dri/renderD128}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc2=size=1280x720:rate=30 \
	-t 2 -c:v libx264 -profile:v high -bf 3 -pix_fmt yuv420p "$TMP/in.mp4"

ffmpeg -hide_banner -loglevel error -hwaccel vaapi -hwaccel_device "$DEV" \
	-hwaccel_output_format vaapi -i "$TMP/in.mp4" \
	-vf hwdownload,format=nv12 -f framemd5 "$TMP/hw.md5"
ffmpeg -hide_banner -loglevel error -i "$TMP/in.mp4" -pix_fmt nv12 \
	-f framemd5 "$TMP/sw.md5"

if diff <(grep -v '^#' "$TMP/hw.md5") <(grep -v '^#' "$TMP/sw.md5") >/dev/null; then
	echo "PASS: hardware decode is bit-exact ($(grep -vc '^#' "$TMP/hw.md5") frames)"
else
	echo "FAIL: hardware decode differs from software" >&2
	exit 1
fi
