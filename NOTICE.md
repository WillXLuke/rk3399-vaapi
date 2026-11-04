# Component licensing

The repository's own code is MIT licensed (see `LICENSE`). Some parts are
derived from other projects and keep their own licenses:

| Path | License | Origin |
|---|---|---|
| `userspace/` | MIT | [libva-v4l2-request](https://github.com/bootlin/libva-v4l2-request) (Bootlin) and its "fourier" fork |
| `kernel/hantro-encoder/` | GPL-2.0-only | Linux kernel 7.3.1 `verisilicon` driver + [Bootlin `hantro/h264-encoding-v5.11`](https://github.com/bootlin/linux/tree/hantro/h264-encoding-v5.11) |
| `kernel/rkvdec-module/` | GPL-2.0-only | Linux kernel 7.3.1 `drivers/media/platform/rockchip/rkvdec` |

Each source file carries its own SPDX identifier. `kernel/` cannot be
relicensed under MIT because it is a derivative work of the Linux kernel.
