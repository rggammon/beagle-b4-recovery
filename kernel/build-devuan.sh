#!/bin/sh
# Build the OpenPVRSGX kernel (Linux 7.2 mainline + the committed PowerVR SGX DDK) for
# the Devuan/SGX image: zImage + omap3-beagle-ab4.dtb (the C70/32 kHz timer fix) +
# modules, including the pvrsrvkm SGX530 module (CONFIG_SGX_OMAP=m).
#
# Patches applied (kernel/patches-devuan/): all four recovery-kernel hardware fixes,
# a Linux 7.2 MMC recovery-state fix, and the DDK 1.6 SGX IRQ + APM-latency fixes.
#
# The DDK sources are committed on the fork's DDK 1.6 branch, so a plain clone tracks
# them (no submodule / separate download needed).
#
# Env: CROSS_COMPILE (default arm-linux-gnueabihf-), JOBS (default nproc), WORK, OUT,
#      OPENPVRSGX_REPO (default the rggammon fork), OPENPVRSGX_REF (branch or commit to
#      build; default the hardware-validated DDK 1.6 Stage 6b commit).
set -eu

REPO=${OPENPVRSGX_REPO:-https://github.com/rggammon/linux_openpvrsgx}
REF=${OPENPVRSGX_REF:-432466d86a4093fd80809df01000cadc97640c14}
CROSS=${CROSS_COMPILE:-arm-linux-gnueabihf-}
JOBS=${JOBS:-$(nproc)}
here=$(cd "$(dirname "$0")" && pwd)
work=${WORK:-$here/../build-devuan}
out=${OUT:-$here/../out}
src="$work/openpvrsgx-src"
mkdir -p "$work" "$out"

# Shallow clone of the kernel + committed DDK. Reuse a build tree only while it
# is based on the requested commit; patches and build outputs make it intentionally
# dirty after the first run.
if [ ! -d "$src/.git" ]; then
    rm -rf "$src"
    git init "$src"
    git -C "$src" remote add origin "$REPO"
    git -C "$src" fetch --depth 1 origin "$REF"
    git -C "$src" checkout --detach FETCH_HEAD
else
    git -C "$src" remote set-url origin "$REPO"
    git -C "$src" fetch --depth 1 origin "$REF"
    requested_commit=$(git -C "$src" rev-parse FETCH_HEAD)
    current_commit=$(git -C "$src" rev-parse HEAD)
    if [ "$current_commit" != "$requested_commit" ]; then
        echo ">> requested kernel changed; replacing stale build tree"
        rm -rf "$src"
        git init "$src"
        git -C "$src" remote add origin "$REPO"
        git -C "$src" fetch --depth 1 origin "$REF"
        git -C "$src" checkout --detach FETCH_HEAD
    fi
fi
cd "$src"
echo ">> OpenPVRSGX commit $(git rev-parse HEAD)"

# Recovery hardware fixes + DDK 1.6 SGX IRQ/APM fixes.
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

# Module tree for the rootfs graft (the DDK 1.6 pvrsrvkm.ko + dcnohw.ko display class).
# Strip debug info and drop the dangling build/source symlinks at the host build tree.
rm -rf "$out/modroot-devuan"
make ARCH=arm CROSS_COMPILE="$CROSS" INSTALL_MOD_PATH="$out/modroot-devuan" INSTALL_MOD_STRIP=1 modules_install
rm -f "$out"/modroot-devuan/lib/modules/*/build "$out"/modroot-devuan/lib/modules/*/source
echo ">> devuan kernel #$(cat include/config/kernel.release 2>/dev/null || echo '?') -> $out/{zImage-devuan,omap3-beagle-ab4-devuan.dtb,modroot-devuan}"
