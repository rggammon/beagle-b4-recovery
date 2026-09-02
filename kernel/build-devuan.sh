#!/bin/sh
# Build the OpenPVRSGX kernel (Linux 7.2 mainline + the committed PowerVR SGX DDK) for
# the Devuan/SGX image: zImage + omap3-beagle-ab4.dtb (the C70/32 kHz timer fix) +
# modules, including the pvrsrvkm SGX530 module (CONFIG_SGX_OMAP=m).
#
# Patches applied (kernel/patches-devuan/): the DDK core-rev whitelist (0005) so the
# closed ti343x 1.2.1 ukernel runs on the early B4 SGX530 r1.0.3 silicon, and the
# provisional omap_hsmmc DMAE/PBIAS workaround (0002), pending same-kernel isolation.
# 7.2's mainline sources + the ab4 DTB otherwise cover this board.
#
# The DDK sources are committed on the linux+pvrsgx branch, so a plain clone tracks them
# (no submodule / separate download needed).
#
# Env: CROSS_COMPILE (default arm-linux-gnueabihf-), JOBS (default nproc), WORK, OUT,
#      OPENPVRSGX_REF (branch or commit to build; default linux+pvrsgx).
set -eu

REPO=https://github.com/openpvrsgx-devgroup/linux_openpvrsgx
REF=${OPENPVRSGX_REF:-linux+pvrsgx}
CROSS=${CROSS_COMPILE:-arm-linux-gnueabihf-}
JOBS=${JOBS:-$(nproc)}
here=$(cd "$(dirname "$0")" && pwd)
work=${WORK:-$here/../build-devuan}
out=${OUT:-$here/../out}
src="$work/openpvrsgx-src"
mkdir -p "$work" "$out"

# Shallow clone of the kernel + committed DDK (reused on re-runs).
if [ ! -d "$src/.git" ]; then
    rm -rf "$src"
    git clone --depth 1 --branch "$REF" "$REPO" "$src"
fi
cd "$src"

# DDK core-rev whitelist + provisional omap_hsmmc DMAE/PBIAS workaround.
# -l --fuzz=3: the hsmmc patch is ported from the 6.6 tree, so line offsets differ.
for p in "$here"/patches-devuan/*.patch; do
    if patch -p1 -l -R --dry-run -f <"$p" >/dev/null 2>&1; then
        echo ">> already applied: $(basename "$p")"
    else
        echo ">> applying $(basename "$p")"
        patch -p1 -l --fuzz=3 <"$p"
    fi
done

cp "$here/config-devuan" .config
make ARCH=arm CROSS_COMPILE="$CROSS" olddefconfig

# zImage + the ab4 DTB (= the C70/32 kHz timer fix) + modules (pvrsrvkm SGX530).
make ARCH=arm CROSS_COMPILE="$CROSS" -j"$JOBS" zImage ti/omap/omap3-beagle-ab4.dtb modules

cp arch/arm/boot/zImage "$out/zImage-devuan"
cp arch/arm/boot/dts/ti/omap/omap3-beagle-ab4.dtb "$out/omap3-beagle-ab4-devuan.dtb"

# Module tree for the rootfs graft (pvrsrvkm_omap3_sgx530_121.ko + friends). Strip debug
# info and drop the dangling build/source symlinks at the host build tree.
rm -rf "$out/modroot-devuan"
make ARCH=arm CROSS_COMPILE="$CROSS" INSTALL_MOD_PATH="$out/modroot-devuan" INSTALL_MOD_STRIP=1 modules_install
rm -f "$out"/modroot-devuan/lib/modules/*/build "$out"/modroot-devuan/lib/modules/*/source
echo ">> devuan kernel #$(cat include/config/kernel.release 2>/dev/null || echo '?') -> $out/{zImage-devuan,omap3-beagle-ab4-devuan.dtb,modroot-devuan}"
