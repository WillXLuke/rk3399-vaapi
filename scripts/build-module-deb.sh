#!/bin/sh
# Package the patched hantro-vpu.ko for the kernel it was built against.
#
#   ./scripts/build-module-deb.sh <path-to-hantro-vpu.ko> [output-dir]
set -eu

KO=${1:?usage: $0 <hantro-vpu.ko> [output-dir]}
OUT=${2:-.}
VER=1.0.0
KVER=$(modinfo -F vermagic "$KO" | awk '{print $1}')
PKG=hantro-vpu-rk3399-modules-${KVER}
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

mkdir -p "$STAGE/DEBIAN" "$STAGE/lib/modules/$KVER/updates"
install -m644 "$KO" "$STAGE/lib/modules/$KVER/updates/hantro-vpu.ko"

cat > "$STAGE/DEBIAN/control" <<CTL
Package: $PKG
Version: $VER-1
Architecture: arm64
Maintainer: William Luke <126307463+WillXLuke@users.noreply.github.com>
Section: kernel
Priority: optional
Homepage: https://github.com/WillXLuke/rk3399-vaapi
Description: RK3399 VPU2 H.264 encoder module for Linux $KVER
 Patched hantro/verisilicon driver adding H.264 encoding on the RK3399 VPU2,
 built against Linux $KVER. Install it alongside libva-v4l2-rk3399.
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
