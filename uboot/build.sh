#!/bin/sh
# Build U-Boot 2019.04 for the BeagleBoard NAND recovery target.
#
# NOTE (draft): the flasher SD carries TWO u-boot variants:
#   - NAND u-boot   (ENV_IS_IN_NAND=y + CMD_BOOTMENU=y, boot menu) -> MLO-nand, u-boot-nand.img
#   - SD-boot u-boot (musb host, used only to run the flasher)     -> MLO,      u-boot.img
# This script builds ONE variant from ./config (the exported NAND config). To produce
# both, build twice with the two .config's (TODO: capture the SD-boot .config too).
#
# Env: CROSS_COMPILE (default arm-linux-gnueabihf-), JOBS, WORK, OUT.
set -eu

UVER=2019.04
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

# Patches: HOSTCFLAGS -fcommon (GCC14/dtc), musb host mode, ENV-in-NAND + boot menu.
for p in "$here"/patches/*.patch; do
    echo ">> applying $(basename "$p")"
    patch -p1 < "$p"
done

cp "$here/config" .config
make ARCH=arm CROSS_COMPILE="$CROSS" olddefconfig

# GCC 14 needs -fcommon for the bundled dtc host tool (also patched into the Makefile).
make ARCH=arm CROSS_COMPILE="$CROSS" -j"$JOBS"

# Bake the NAND boot command (ubi attach -> load /boot/nand-boot.txt -> bootmenu).
[ -f "$here/set-nand-bootcmd.py" ] && python3 "$here/set-nand-bootcmd.py" . || true

cp MLO      "$out/MLO-nand"
cp u-boot.img "$out/u-boot-nand.img"
echo ">> u-boot -> $out/{MLO-nand,u-boot-nand.img}"
