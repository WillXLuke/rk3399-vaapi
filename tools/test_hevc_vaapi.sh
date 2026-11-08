#!/bin/sh
# Decode an HEVC stream through this VA-API driver and report throughput.
#
#   ./tools/test_hevc_vaapi.sh <file.mp4> [--compare] [--require-4k60]
#
# --compare       also decode with the software decoder and require bit-exact
#                 NV12 output (the two MD5s must match).
# --require-4k60  fail unless the stream is 3840x2160 or larger, at 60 fps,
#                 and at least 30 seconds long.
set -eu

HERE=$(cd "$(dirname "$0")/.." && pwd)
FILE=${1:?usage: $0 <hevc-file> [--compare] [--require-4k60]}
shift || true
COMPARE=0
REQUIRE=0
for arg in "$@"; do
    case "$arg" in
    --compare) COMPARE=1 ;;
    --require-4k60) REQUIRE=1 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

[ -f "$FILE" ] || { echo "no such file: $FILE" >&2; exit 2; }

export LIBVA_DRIVER_NAME=${LIBVA_DRIVER_NAME:-v4l2}
DEVICE=${VAAPI_DEVICE:-/dev/dri/renderD128}
LOG=$(mktemp)
PROGRESS=$(mktemp)
trap 'rm -f "$LOG" "$PROGRESS"' EXIT

info=$(ffprobe -v error -select_streams v:0 \
    -show_entries stream=codec_name,profile,width,height,pix_fmt,avg_frame_rate \
    -show_entries format=duration -of default=noprint_wrappers=1 "$FILE")
codec=$(printf '%s\n' "$info" | sed -n 's/^codec_name=//p')
profile=$(printf '%s\n' "$info" | sed -n 's/^profile=//p')
width=$(printf '%s\n' "$info" | sed -n 's/^width=//p')
height=$(printf '%s\n' "$info" | sed -n 's/^height=//p')
pix_fmt=$(printf '%s\n' "$info" | sed -n 's/^pix_fmt=//p')
rate=$(printf '%s\n' "$info" | sed -n 's/^avg_frame_rate=//p')
duration=$(printf '%s\n' "$info" | sed -n 's/^duration=//p')
fps=$(awk -v r="$rate" 'BEGIN { split(r, a, "/"); printf "%.3f", a[1] / (a[2] ? a[2] : 1) }')

echo "stream : $FILE"
echo "codec  : $codec $profile ${width}x${height} ${pix_fmt} @ ${fps} fps, ${duration}s"

echo "driver :"
vainfo --display drm --device "$DEVICE" 2>/dev/null | grep -E 'Driver version|VAProfileHEVC' || {
    echo "HEVC profile not advertised by $LIBVA_DRIVER_NAME" >&2
    exit 1
}

echo "decoding ..."
start=$(date +%s%N)
if ! ffmpeg -hide_banner -loglevel error -nostats -progress "$PROGRESS" \
    -hwaccel vaapi -hwaccel_device "$DEVICE" -hwaccel_output_format vaapi \
    -an -i "$FILE" -f null - 2>"$LOG"; then
    echo "decode FAILED" >&2
    tail -20 "$LOG" >&2
    exit 1
fi
end=$(date +%s%N)
ms=$(( (end - start) / 1000000 ))
frames=$(sed -n 's/^frame=//p' "$PROGRESS" | tail -1)
[ -n "$frames" ] || frames=0
fps_out=$(awk -v f="$frames" -v ms="$ms" 'BEGIN { if (ms) printf "%.2f", f * 1000 / ms; else print "?" }')

echo "frames : $frames in ${ms} ms (${fps_out} fps end-to-end)"
[ "$frames" -gt 0 ] || { echo "no frames decoded" >&2; exit 1; }

if [ "$REQUIRE" = 1 ]; then
    ok=1
    [ "$width" -ge 3840 ] && [ "$height" -ge 2160 ] || { echo "not 4K: ${width}x${height}" >&2; ok=0; }
    awk -v f="$fps" 'BEGIN { exit !(f >= 59.94) }' || { echo "not 60 fps: $fps" >&2; ok=0; }
    awk -v d="$duration" 'BEGIN { exit !(d >= 30.0) }' || { echo "shorter than 30 s: $duration" >&2; ok=0; }
    [ "$ok" = 1 ] || exit 1
fi

if [ "$COMPARE" = 1 ]; then
    case "$pix_fmt" in
    *10*) compare_fmt=p010le ;;
    *) compare_fmt=nv12 ;;
    esac
    echo "comparing against software decode ($compare_fmt) ..."
    hw_md5=$(ffmpeg -hide_banner -loglevel error -hwaccel vaapi \
        -hwaccel_device "$DEVICE" -hwaccel_output_format vaapi -i "$FILE" \
        -map 0:v:0 -vf hwdownload,format=$compare_fmt -f md5 - 2>/dev/null | sed -n 's/^MD5=//p')
    sw_md5=$(ffmpeg -hide_banner -loglevel error -i "$FILE" \
        -map 0:v:0 -vf format=$compare_fmt -f md5 - 2>/dev/null | sed -n 's/^MD5=//p')
    if [ -n "$hw_md5" ] && [ "$hw_md5" = "$sw_md5" ]; then
        echo "output : bit-exact with software decode (MD5 $hw_md5)"
    else
        echo "output : MISMATCH (hw=$hw_md5 sw=$sw_md5)" >&2
        exit 1
    fi
fi

echo "PASS"
