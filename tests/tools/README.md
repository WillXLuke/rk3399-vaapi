# Encoder API-level tests

`enc_damage_test.c` mirrors what compositors (KasmVNC, VNC servers) do:
upload the first frame in full, then only damaged rectangles, relying on the
surface contents persisting between frames.  It also writes the exact frames
it feeds the encoder to a raw NV12 file, so the encoded result can be compared
bit-for-bit.

Build and run:

```sh
gcc -O2 -o enc_damage_test enc_damage_test.c -lva -lva-drm
./enc_damage_test out.h264 src_nv12.yuv 640 480 60
ffmpeg -i out.h264 -pix_fmt nv12 -f rawvideo dec.yuv
# compare src_nv12.yuv and dec.yuv frame by frame (expect > 40 dB PSNR)
```
