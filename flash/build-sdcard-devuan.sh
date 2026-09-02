#!/bin/sh
# Assemble the Devuan + SGX530 SD-card image: FAT16 boot (p1) + ext4 root (p2).
# Same U-Boot as the recovery images; the 7.2 SGX kernel + Devuan/GLES ext4 root differ.
# Runs from SD only -- no NAND write. Uses sfdisk/mtools/dd (no loop devices needed).
#
# Consumes out/{MLO,u-boot.img,zImage-devuan,omap3-beagle-ab4-devuan.dtb,rootfs-devuan.ext4}.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
out=${OUT:-$here/../out}
img=${IMG:-$out/beagle-sdcard-devuan.img}
BOOT_MB=${BOOT_MB:-64}

root="$out/rootfs-devuan.ext4"
[ -f "$root" ] || { echo "missing $root (run rootfs/build-devuan.sh first)" >&2; exit 1; }
rootmb=$(( ($(stat -c%s "$root") + 1048575) / 1048576 ))

boot_start=2048                       # p1 at 1 MiB
boot_sectors=$(( BOOT_MB * 2048 ))
root_start=$(( boot_start + boot_sectors ))
total_mb=$(( 1 + BOOT_MB + rootmb + 1 ))

truncate -s "${total_mb}M" "$img"
printf 'label: dos\nunit: sectors\n\nstart=%d, size=%d, type=c, bootable\nstart=%d, type=83\n' \
    "$boot_start" "$boot_sectors" "$root_start" | sfdisk "$img" >/dev/null

# p1: FAT16 boot. MLO first (OMAP ROM). The ab4 dtb is copied as omap3-beagle.dtb (the
# name the boot menu's `bootmmc` loads); uEnv.txt sets the rw ext4-root bootargs.
fat="$out/_devuan-fat.img"
truncate -s "${BOOT_MB}M" "$fat"
mkfs.vfat -F16 -n BOOT "$fat" >/dev/null
mcopy -i "$fat" "$out/MLO"                          ::MLO
mcopy -i "$fat" "$out/u-boot.img"                   ::u-boot.img
mcopy -i "$fat" "$out/zImage-devuan"                ::zImage
mcopy -i "$fat" "$out/omap3-beagle-ab4-devuan.dtb"  ::omap3-beagle.dtb
mcopy -i "$fat" "$here/uEnv-sdcard-devuan.txt"      ::uEnv.txt
dd if="$fat" of="$img" bs=512 seek="$boot_start" conv=notrunc >/dev/null 2>&1
rm -f "$fat"

# p2: ext4 root (raw copy into the partition).
dd if="$root" of="$img" bs=512 seek="$root_start" conv=notrunc >/dev/null 2>&1

xz -T0 -f -k "$img"
echo ">> $img.xz ($(stat -c%s "$img.xz") bytes)"
