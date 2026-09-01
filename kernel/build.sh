#!/bin/sh
# Build Linux 6.6.152 (pristine mainline) with the board patches and the ab4 DTB.
# The timer fix is NOT a patch — it's building omap3-beagle-ab4.dtb.
#
# Env: CROSS_COMPILE (default arm-linux-gnueabihf-), JOBS (default nproc), WORK, OUT.
set -eu

KVER=6.6.152
CROSS=${CROSS_COMPILE:-arm-linux-gnueabihf-}
JOBS=${JOBS:-$(nproc)}
here=$(cd "$(dirname "$0")" && pwd)
work=${WORK:-$here/../build}
out=${OUT:-$here/../out}
mkdir -p "$work" "$out"
cd "$work"

tarball="linux-$KVER.tar.xz"
[ -f "$tarball" ] || wget -qO "$tarball" "https://cdn.kernel.org/pub/linux/kernel/v6.x/$tarball"
rm -rf "linux-$KVER"
tar xf "$tarball"
cd "linux-$KVER"

# Board patches (USB host / EHCI-off / NAND partitions / MMC pinmux, PBIAS+DMAE,
# NAND ONFI x16 geometry, twl4030-usb probe-defer). Generated with `git diff`.
for p in "$here"/patches/*.patch; do
    echo ">> applying $(basename "$p")"
    patch -p1 < "$p"
done

cp "$here/config" .config
make ARCH=arm CROSS_COMPILE="$CROSS" olddefconfig

# zImage + the ab4 DTB (= the C70/32 kHz timer fix for early boards)
make ARCH=arm CROSS_COMPILE="$CROSS" -j"$JOBS" zImage ti/omap/omap3-beagle-ab4.dtb

cp arch/arm/boot/zImage "$out/zImage"
cp arch/arm/boot/dts/ti/omap/omap3-beagle-ab4.dtb "$out/omap3-beagle-ab4.dtb"
echo ">> kernel #$(cat include/config/kernel.release 2>/dev/null || echo '?') -> $out/{zImage,omap3-beagle-ab4.dtb}"

# Loadable modules (=m drivers, e.g. Bluetooth btusb). The recovery images build every
# boot-critical driver =y and ignore these; the minimal writable appliance
# (rootfs/build-min.sh) installs this tree into /lib/modules. Strip debug info; drop the
# build/source symlinks that dangle at the host build tree. Needs `depmod` (kmod) on host.
make ARCH=arm CROSS_COMPILE="$CROSS" -j"$JOBS" modules
rm -rf "$out/modroot"
make ARCH=arm CROSS_COMPILE="$CROSS" INSTALL_MOD_PATH="$out/modroot" INSTALL_MOD_STRIP=1 modules_install
rm -f "$out"/modroot/lib/modules/*/build "$out"/modroot/lib/modules/*/source
echo ">> modules -> $out/modroot/lib/modules/$(cat include/config/kernel.release 2>/dev/null || echo '?')/"
