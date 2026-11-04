#!/bin/bash
# Encode smoke test: VA-API H.264 encode, then decode the result.
set -euo pipefail
DEV=${VAAPI_DEVICE:-/dev/dri/renderD128}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

vainfo --display drm --device "$DEV" | grep -q VAEntrypointEncSlice \
	|| { echo "FAIL: no VAEntrypointEncSlice in vainfo" >&2; exit 1; }

ffmpeg -hide_banner -loglevel error -vaapi_device "$DEV" -f lavfi \
	-i testsrc2=size=640x480:rate=25 -t 1 -vf format=nv12,hwupload \
	-c:v h264_vaapi -qp 26 -bf 0 -f h264 "$TMP/out.h264"

ffmpeg -hide_banner -loglevel error -i "$TMP/out.h264" -f null -
FRAMES=$(ffprobe -v error -count_frames -select_streams v:0 \
	-show_entries stream=nb_read_frames -of csv=p=0 "$TMP/out.h264")
[ "$FRAMES" = "25" ] || { echo "FAIL: expected 25 frames, got $FRAMES" >&2; exit 1; }
echo "PASS: H.264 encode produced a decodable 25-frame stream ($(stat -c%s "$TMP/out.h264") bytes)"
