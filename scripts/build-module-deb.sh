#!/bin/sh
# Package the patched hantro-vpu.ko and the rkvdec module for the kernel they
# were built against.
#
#   ./scripts/build-module-deb.sh <hantro-vpu.ko> <rockchip-vdec.ko> [output-dir]
set -eu

[ $# -ge 2 ] || { echo "usage: $0 <hantro-vpu.ko> <rockchip-vdec.ko> [output-dir]" >&2; exit 1; }
OUT=${3:-.}
VER=1.0.2
KVER=$(modinfo -F vermagic "$1" | awk '{print $1}')
PKG=rk3399-vpu-modules-${KVER}
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

mkdir -p "$STAGE/DEBIAN" "$STAGE/lib/modules/$KVER/updates"
install -m644 "$1" "$STAGE/lib/modules/$KVER/updates/hantro-vpu.ko"
install -m644 "$2" "$STAGE/lib/modules/$KVER/updates/rockchip-vdec.ko"

cat > "$STAGE/DEBIAN/control" <<CTL
Package: $PKG
Version: $VER-1
Architecture: arm64
Maintainer: William Luke <126307463+WillXLuke@users.noreply.github.com>
Section: kernel
Priority: optional
Homepage: https://github.com/WillXLuke/rk3399-vaapi
Conflicts: hantro-vpu-rk3399-modules-$KVER
Replaces: hantro-vpu-rk3399-modules-$KVER
Description: RK3399 VPU modules: H.264 encoder and RKVDU decoder for Linux $KVER
 Patched verisilicon/hantro driver, adding H.264 encoding on the RK3399 VPU2
 and fixing the JPEG encoder's size limit to the hardware's 8176x8176, plus
 the rkvdec driver for H.264/HEVC/VP9 decoding. Built against Linux $KVER.
CTL

cat > "$STAGE/DEBIAN/postinst" <<'POST'
#!/bin/sh
set -e
depmod -a "$(uname -r)" || true
exit 0
POST
chmod 755 "$STAGE/DEBIAN/postinst"

dpkg-deb --build --root-owner-group "$STAGE" "$OUT/${PKG}_${VER}-1_arm64.deb"
echo "built $OUT/${PKG}_${VER}-1_arm64.deb"
