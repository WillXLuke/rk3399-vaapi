# Encoder/decoder optimization research

Baseline on the NanoPC-T4: pixelflux 720p VA-API encode took **24.2 ms/frame**;
pure NV12 encode took 12.7 ms; 4K60 Main10 decode 50 fps.  The work below
looked for the remaining latency in the RGB upload/conversion and in the
V4L2/VA-API submission paths.

## Sources consulted

1. https://raw.githubusercontent.com/FFmpeg/FFmpeg/master/libavcodec/vaapi_encode.c
   FFmpeg's VA-API encoder.  `vaapi_encode_issue()` calls `vaBeginPicture` /
   `vaRenderPicture` / `vaEndPicture`; `vaapi_encode_wait()` either uses
   `vaSyncBuffer` (VA-API 1.9+, async_encode) or `vaSyncSurface`.  FFmpeg
   determines `async_encode` by probing `vaSyncBuffer`, so the driver's
   synchronous `vaEndPicture` is what fixes the pipeline depth at one picture.
2. https://lists.ffmpeg.org/pipermail/ffmpeg-devel/2021-December/290364.html
   and https://ffmpeg.org/pipermail/ffmpeg-devel/2022-February/293188.html
   FFmpeg-devel async_depth series for vaapi_encode: more in-flight pictures
   improve throughput, but only when the driver implements asynchronous
   completion/sync.  This driver has no output-buffer sync implementation, so
   just raising FFmpeg's async_depth would not help.
3. https://trac.ffmpeg.org/wiki/Hardware/VAAPI
   Decode-only benchmarks should use `-f null -`; `hwdownload` is explicitly
   called out as adding large overhead.  This matches the measurement that
   4K60 decode-to-null is 50-62 fps while 4K60 P010 download is 5.7 fps.
4. https://raw.githubusercontent.com/airockchip/librga/main/docs/Rockchip_Developer_Guide_RGA_EN.md
   RGA2-Enhance (RK3399) supports RGB->YUV 4:2:0 with BT.601/BT.709, 2
   pixels/cycle.  This looked like an alternative to the CPU converter.
5. /var/tmp/kp/linux-7.3.3/drivers/media/platform/rockchip/rga/
   The in-tree RGA V4L2 driver only exposes MMAP/DMABUF (no USERPTR) and its
   format table has no 10-bit NV15/P010, so an RGA conversion would still need
   a full input copy and cannot fix the Main10 download path.
6. https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/dev-mem2mem.html
   V4L2 M2M queue semantics: separate OUTPUT/CAPTURE queues, STREAMON both,
   QBUF/DQBUF; drivers arbitrate the hardware per file handle.  The encoder is
   a stateful M2M device, the decoder uses requests.
7. https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/dev-stateless-decoder.html
   The decoder's per-frame controls and `V4L2_BUF_FLAG_ERROR` contract.  The
   driver's current `endPicture` waits for each request, which explains the
   bursty decoder throughput and the 0.5-1 s setup cost on a 5 s clip.
8. https://lwn.net/Articles/1033200/
   rkvdec HEVC backend, including the known concurrent-session limitation.  The
   kernel decoder is the throughput ceiling for HEVC; VA-API can only add
   per-frame userspace overhead.
9. https://raw.githubusercontent.com/intel/libva/master/va/va.h and
   https://raw.githubusercontent.com/intel/libva/master/va/va_dec_hevc.h
   `vaPutImage`, image formats, surface formats, and the HEVC slice contract.
   `vaPutImage` is the natural place to convert an RGB upload once; the VPP
   then only needs to hand NV12 to the encoder.
10. https://patches.linaro.org/project/linux-devicetree/cover/20210719205242.18807-1-ezequiel@collabora.com/
    Hantro VPU2 H.264 support series.  The VPU2 encoder is a fixed-function
    10 ms/frame block at 720p; the remaining frame budget is upload,
    conversion, copies and synchronisation.
11. https://developer.arm.com/documentation/den0018/a/
    ARM NEON programmer's guide; used for the deinterleaving `vld4`/`vuzp`
    RGB32->NV12 kernel and the 64-bit NV15 unpack.
12. https://www.kernel.org/doc/html/latest/scheduler/sched-capacity.html
    CPU capacity/big.LITTLE scheduling.  On RK3399 the A72s have capacity 1024
    and the A53s 395; the ondemand governor otherwise leaves a conversion
    thread on an A53 (and the A72s at 600 MHz).
13. https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/pixfmt-yuv-planar.html
    NV15 is packed 15 bpp while P010 is a 16-bit container.  The Main10
    download therefore requires a CPU unpack when the capture buffer is read.
14. https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/pixfmt-compressed.html
    HEVC parsed-slice (`S265`) format and the stateless decoder pipeline.

## Findings

* The 24 ms pixelflux number is not the VPU2 encoder: a raw NV12 encode is
  12.4-13.5 ms/frame (74-81 fps at 720p) and the hardware job is ~9-10 ms.
* The scalar RGB32->NV12 conversion plus the extra RGBA copy accounted for
  ~11 ms.  A same-size NEON kernel is 2 ms standalone; four threads only reach
  1.35 ms (memory-bound), so a thread pool was not worth the complexity.
* Converting inside `vaPutImage` and handing the VPP an already-NV12 frame
  removes the RGBA buffer copy; the VPP becomes a move.
* The conversion thread usually lands on an A53.  A scoped affinity hint to
  the highest-capacity CPUs (only when the caller's mask already allows them)
  gives a consistent improvement; pinning the whole consumer to A72 is the
  best case.
* `vaCreateImage` was allocating with `calloc` and freeing through `delete`
  (mismatched), and zeroing 25 MB per 4K P010 frame.  Fixed with a `free`
  deleter and uninitialised image buffers.
* P010 download remains slow because the V4L2 rkvdec capture buffer is
  uncached DMA memory: reading 18.7 MB from it takes ~164 ms/frame.  The
  decoding itself is unaffected; a GPU/dmabuf consumer avoids this read.
* RGA2's V4L2 interface has no 10-bit formats and requires an input copy, so
  it would not beat the NEON path; not integrated.
* Async VA-API encode would need `vaSyncBuffer`/deferred completion and a
  rework of the coded-buffer lifetime; FFmpeg only enables async_depth when
  `vaSyncBuffer` is implemented.  Not attempted in this pass.
