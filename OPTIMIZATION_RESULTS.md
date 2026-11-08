# Encoder/decoder optimization results

Device: NanoPC-T4, RK3399, 4 GB, kernel 7.3.3-nanopc-t4.
Consumers: FFmpeg 8 and pixelflux 2.2.3/Selkies.
Driver: `libva-v4l2-rk3399` 1.0.4 (`LIBVA_DRIVER_NAME=v4l2`).

## Encode latency: pixelflux 720p, 8 s live desktop

`pixelflux` reports `capture_ns`, `encode_start_ns`, `encode_end_ns` for each
frame.  Values are medians over ~240 frames.

| Path | encode_start -> encode_end |
| --- | --- |
| baseline, scalar RGB32->NV12 | **24.15 ms** |
| baseline, second run | 24.25 ms |
| 1.0.4, default affinity | **17.4-17.9 ms** |
| 1.0.4, consumer pinned to A72 (`taskset -c 4,5`) | **15.4 ms** |

That is a 26-29% reduction unpinned and 36% pinned to the big cores.  The
pixelflux target is 30 fps (33.3 ms); the optimized path has substantial
headroom even at 60 fps (16.7 ms) when pinned.

## Pure encoder throughput (NV12 input, no RGB conversion)

5 s, 150 frames, 1280x720@30, `h264_vaapi`, 8 Mbps:

| Version | wall time | fps | per frame |
| --- | --- | --- | --- |
| baseline | 1.898 s | 79.0 | 12.65 ms |
| 1.0.4 | 1.862-2.030 s | 73.9-80.6 | 12.4-13.5 ms |

1920x1080@30, 150 frames: 43.2 fps (23.15 ms/frame).
The ~9-10 ms VPU2 hardware job is now the dominant term; remaining userspace
time is the NV12 upload into the V4L2 output planes and bitstream copy.

## 5 s dynamic video from `/home/durian/Videos`

Source: `hevc_4k60P_main10_10bit_420_eos_r5c_1.mp4` (3840x2160, 59.94 fps,
Main10).  A 5 s 1280x720@30 yuv420p derivative was made through the hardware
decoder and used for the encode test; the original is used for decode.

| Test | Command | Result |
| --- | --- | --- |
| Encode 5 s 720p30 | `ffmpeg ... -c:v h264_vaapi` | 150 frames, 73.9-80.6 fps, 12.4-13.5 ms/frame |
| Decode 5 s 4K60 Main10 | `ffmpeg -hwaccel vaapi ... -f null -` | 50.5 fps (ondemand), 54.2 fps (performance governor) |
| Decode 5 s 720p Main | same, `hevc8.mp4` | **295 fps** |
| Decode 32.53 s 4K60 Main10 | same | 62.3-62.4 fps with performance governor |
| Download 5 s 4K60 Main10 to P010 | `hwdownload,format=p010le` | 5.7 fps (uncached V4L2 read) |

The 4K60 5 s figure includes ~0.7 s of decoder setup; the 32.5 s run reaches
62 fps end-to-end with the CPU governor at performance.

## Correctness

* 720p HEVC Main output remains bit-exact against software decode:
  MD5 `e93bb2ecc3ec0d794eccbcb93e4ec758`, `PASS`.
* The optimized pixelflux stream decodes cleanly: 121/121 frames, 1280x720,
  and a decoded frame shows no corruption.
* The 5 s encoded stream has 150/150 frames and decodes without errors.
* 4K60 Main10 P010 output remains bit-exact against software:
  MD5 `34bd5020d4ff8b2e0b7e355ec19543c1` (390 frames).

## What changed

1. **NEON RGB32->NV12** for same-size frames, bit-identical BT.709 limited
   range arithmetic.  Scalar fallback retained for scaling.
2. **Convert in `vaPutImage`**, not later in the VPP.  The VPP now takes the
   already-NV12 host frame (`std::move`) instead of copying RGBA and converting
   it again.
3. **Scoped big-core affinity** for the conversion only: use the
   highest-capacity CPUs that the caller's affinity already allows, restore
   the mask afterwards.  If the caller is pinned to A53 cores, nothing changes.
4. **Full-plane luma memcpy** into the V4L2 output buffer instead of 720/1080
   row memcpys.
5. **Image buffer allocator fix**: `calloc` memory was released with `delete`;
   `VAImageBufferType` now uses `malloc`/`free` and is not zeroed, avoiding a
   25 MB memset per 4K Main10 `vaCreateImage`.
6. **Decoder setup**: 20 capture buffers instead of 24, saving ~90 MB at 4K10
   and a little allocation time, while still covering FFmpeg's 20-surface pool.

## Limits

* 4K60 Main10 P010 **download** is 5.7 fps because the rkvdec capture buffer is
  uncached DMA memory; decode-to-null is 50-62 fps.  GPU/dmabuf consumers do not
  pay this cost.
* RGA2 was evaluated; its V4L2 driver exposes no 10-bit formats and requires an
  input copy, so it was not used.
* Asynchronous VA-API encode would need `vaSyncBuffer`/deferred completion; not
  implemented in this pass.
