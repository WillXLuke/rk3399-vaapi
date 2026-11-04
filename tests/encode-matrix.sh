#!/bin/bash
# 10-second encode matrix: profiles x resolutions (CQP) + browser-like CBR.
# Reports PSNR against the source and flags blank/green frames.
set -euo pipefail
DEV=${VAAPI_DEVICE:-/dev/dri/renderD128}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
fail=0

psnr() { # <decoded.yuv> <source.yuv> <WxH>
	python3 - "$1" "$2" "$3" <<'PY'
import sys, numpy as np
dec, src, res = sys.argv[1:4]
W,H = map(int, res.split('x')); fs = W*H*3//2
a = np.fromfile(src, dtype=np.uint8); b = np.fromfile(dec, dtype=np.uint8)
n = min(len(a), len(b))//fs
mse = 0.0; blank = 0
for k in range(n):
    x = a[k*fs:(k+1)*fs].astype(np.int32); y = b[k*fs:(k+1)*fs].astype(np.int32)
    mse += float(((x-y)**2).mean())
    if y[:W*H].mean() < 10: blank += 1
p = 10*np.log10(255**2/(mse/n)) if n and mse else 99
print(f"frames={n} PSNR={p:.2f} blank={blank}")
sys.exit(0 if (n and p > 35 and blank == 0) else 1)
PY
}

for res in 640x480 1280x720; do
  ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc2=size=$res:rate=30 -t 10 \
	-pix_fmt yuv420p -f rawvideo -y "$TMP/src.yuv"
  for prof in constrained_baseline main high; do
    out="$TMP/${res}_${prof}.h264"
    ffmpeg -hide_banner -loglevel error -vaapi_device "$DEV" -f lavfi \
	-i "testsrc2=size=$res:rate=30" -t 10 -vf format=nv12,hwupload \
	-c:v h264_vaapi -profile:v $prof -qp 26 -bf 0 -g 60 -f h264 -y "$out"
    ffmpeg -hide_banner -loglevel error -i "$out" -pix_fmt yuv420p -f rawvideo -y "$TMP/dec.yuv"
    printf '%-10s %-20s ' "$res" "$prof"
    if psnr "$TMP/dec.yuv" "$TMP/src.yuv" "$res"; then :; else echo "  FAIL"; fail=1; fi
  done
done

# Browser-like: constrained baseline + CBR
ffmpeg -hide_banner -loglevel error -vaapi_device "$DEV" -f lavfi \
	-i "testsrc2=size=1280x720:rate=30" -t 10 -vf format=nv12,hwupload \
	-c:v h264_vaapi -profile:v constrained_baseline -b:v 2500k -bf 0 -g 30 \
	-f h264 -y "$TMP/cbr.h264"
ffmpeg -hide_banner -loglevel error -i "$TMP/cbr.h264" -f null -
KBPS=$(( $(stat -c%s "$TMP/cbr.h264") * 8 / 10 / 1000 ))
echo "CBR target 2500 kbps -> ${KBPS} kbps (accept 1500..4000)"
[ "$KBPS" -ge 1500 ] && [ "$KBPS" -le 4000 ] || fail=1
exit $fail
