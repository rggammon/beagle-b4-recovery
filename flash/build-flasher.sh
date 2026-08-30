#!/bin/sh
# Assemble the self-contained NAND flasher SD image from out/ artifacts.
# Produces beagle-nand-flasher.img.xz: FAT16 SD (MBR + partition @1MiB) carrying the
# SD-boot MLO/u-boot, the NAND MLO-nand/u-boot-nand.img, rootfs.ubi, and uEnv.txt flash
# macros. Uses mtools/sfdisk (no loop devices needed).
#
# NOTE (draft): needs an SD-boot u-boot (out/MLO, out/u-boot.img) in addition to the
# NAND variant (out/MLO-nand, out/u-boot-nand.img). See uboot/build.sh TODO.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
out=${OUT:-$here/../out}
img=${IMG:-$out/beagle-nand-flasher.img}
# 120 MiB fits a "128MB" SD card. Payload is ~112 MiB (rootfs.ubi) + ~1.4 MiB
# (both U-Boots), so this is near the floor -- shrink the UBI rootfs before going lower.
SIZE_MB=${SIZE_MB:-120}

truncate -s "${SIZE_MB}M" "$img"
printf 'label: dos\nunit: sectors\n\nstart=2048, type=c, bootable\n' | sfdisk "$img"

part="$out/fat.img"
truncate -s "$((SIZE_MB - 1))M" "$part"
mkfs.vfat -F16 -n NANDFLASH "$part"

# MLO must be the first file written (OMAP boot ROM requirement).
mcopy -i "$part" "$out/MLO"             ::MLO             2>/dev/null || echo "WARN: out/MLO (SD-boot u-boot) missing"
mcopy -i "$part" "$out/u-boot.img"      ::u-boot.img      2>/dev/null || echo "WARN: out/u-boot.img missing"
mcopy -i "$part" "$out/MLO-nand"        ::MLO-nand
mcopy -i "$part" "$out/u-boot-nand.img" ::u-boot-nand.img
mcopy -i "$part" "$out/rootfs.ubi"      ::rootfs.ubi
mcopy -i "$part" "$here/uEnv.txt"       ::uEnv.txt

dd if="$part" of="$img" bs=1M seek=1 conv=notrunc
rm -f "$part"
xz -T0 -f -k "$img"
echo ">> $img.xz ($(stat -c%s "$img.xz") bytes)"
