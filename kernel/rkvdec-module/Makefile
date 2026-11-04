# Out-of-tree build of the Rockchip rkvdec V4L2 stateless decoder
# (kernel 7.3.1, drivers/media/platform/rockchip/rkvdec).
#
# Object list is identical to upstream's Makefile, which builds the single
# module "rockchip-vdec" from these 12 objects.  The module name cannot be
# "rkvdec" while rkvdec.o itself is one of the composite objects (kbuild then
# drops the self-referential prerequisite and links an empty module), so we
# keep the upstream name and rely on a modprobe alias rkvdec -> rockchip-vdec.
obj-m := rockchip-vdec.o

rockchip-vdec-y := \
	rkvdec.o \
	rkvdec-cabac.o \
	rkvdec-h264.o \
	rkvdec-h264-common.o \
	rkvdec-hevc.o \
	rkvdec-hevc-common.o \
	rkvdec-rcb.o \
	rkvdec-vdpu381-h264.o \
	rkvdec-vdpu381-hevc.o \
	rkvdec-vdpu383-h264.o \
	rkvdec-vdpu383-hevc.o \
	rkvdec-vp9.o
