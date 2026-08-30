#!/bin/sh
# Build U-Boot 2024.07 for the BeagleBoard NAND recovery target.
#
# v2024.07 is the LAST mainline release that still carries omap3_beagle (the board was
# removed in the v2024.10 merge window for missing the CONFIG_DM_I2C deadline). One
# unified binary serves BOTH roles: the flasher's SD boot AND the NAND appliance, because
# SPL follows the ROM boot device -- so there is no separate SD-boot variant.
#
# Patches are tiny: a defconfig delta and the bootmenu env (see uboot/patches/). No
# board.c changes -- musb host is config-driven (CONFIG_USB_MUSB_HOST) under driver model.
#
# Env: CROSS_COMPILE (default arm-linux-gnueabihf-), JOBS, WORK, OUT.
set -eu

UVER=2024.07
CROSS=${CROSS_COMPILE:-arm-linux-gnueabihf-}
JOBS=${JOBS:-$(nproc)}
here=$(cd "$(dirname "$0")" && pwd)
work=${WORK:-$here/../build}
out=${OUT:-$here/../out}
mkdir -p "$work" "$out"
cd "$work"

tarball="u-boot-$UVER.tar.bz2"
[ -f "$tarball" ] || wget -qO "$tarball" "https://ftp.denx.de/pub/u-boot/$tarball"
rm -rf "u-boot-$UVER"
tar xf "$tarball"
cd "u-boot-$UVER"

# 0001 = defconfig delta (MMC blk-count, BOOTM_LEN, musb host, bootmenu, mtdparts, EFI off)
# 0002 = bootmenu env (inline trilobite bootargs; `ubi part rootfs 2048` VID-hdr offset)
for p in "$here"/patches/*.patch; do
    echo ">> applying $(basename "$p")"
    patch -p1 < "$p"
done

make ARCH=arm CROSS_COMPILE="$CROSS" omap3_beagle_defconfig
# retry once: the SPL lds/fixdep step can lose a race under -j on a fresh tree
make ARCH=arm CROSS_COMPILE="$CROSS" -j"$JOBS" \
  || make ARCH=arm CROSS_COMPILE="$CROSS" -j"$JOBS"

# Same binary for both roles; the flasher writes MLO-nand/u-boot-nand.img to NAND and
# boots MLO/u-boot.img from SD -- identical bytes.
cp MLO "$out/MLO";         cp u-boot.img "$out/u-boot.img"
cp MLO "$out/MLO-nand";    cp u-boot.img "$out/u-boot-nand.img"
echo ">> u-boot $UVER -> $out/{MLO,u-boot.img,MLO-nand,u-boot-nand.img} (identical)"
