#!/bin/sh
# Build the read-only Alpine "trilobite" rootfs as a UBIFS/UBI image.
# Uses alpine-make-rootfs (reproducible Alpine armv7 rootfs). Requires root + qemu-user
# binfmt for the armv7 chroot (the CI workflow sets these up).
#
# Consumes out/zImage and out/omap3-beagle-ab4.dtb (run kernel/build.sh first).
# Env: ALPINE_BRANCH (default v3.24), WORK, OUT.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
out=${OUT:-$here/../out}
work=${WORK:-$here/../build}
AMR_VER=v0.7.0
ALPINE_BRANCH=${ALPINE_BRANCH:-v3.24}
mkdir -p "$work" "$out"
cd "$work"

# 1. base rootfs + packages, with in-chroot config (services, ssh host keys, sshd).
[ -x alpine-make-rootfs ] || wget -qO alpine-make-rootfs \
  "https://raw.githubusercontent.com/alpinelinux/alpine-make-rootfs/$AMR_VER/alpine-make-rootfs"
chmod +x alpine-make-rootfs
rm -rf rootfs
# v0.7.0 has no --arch flag: set the target arch via apk (APK_OPTS reaches every apk
# call incl. --initdb, which writes /etc/apk/arch). The armv7 chroot runs under qemu binfmt.
sudo APK_OPTS="--no-progress --arch armv7" ./alpine-make-rootfs \
    --branch "$ALPINE_BRANCH" \
    --packages "$(cat "$here/packages.txt")" \
    --script-chroot \
    rootfs "$here/post-install.sh"

# 2. overlay (inittab w/ rng-seed, ro fstab, nand-boot.txt, rng-seed, authorized_keys...).
sudo cp -a "$here/overlay/." rootfs/
sudo chmod 700 rootfs/root/.ssh; sudo chmod 600 rootfs/root/.ssh/authorized_keys
sudo chmod 755 rootfs/sbin/rng-seed

# 3. kernel + ab4 dtb into /boot (loaded by U-Boot's ubifsload).
sudo mkdir -p rootfs/boot
sudo cp "$out/zImage" rootfs/boot/zImage
sudo cp "$out/omap3-beagle-ab4.dtb" rootfs/boot/omap3-beagle-ab4.dtb

# 4. read-only UBIFS -> UBI image.
sudo mkfs.ubifs -m 2048 -e 126976 -c 1900 -x lzo -o rootfs.ubifs -r rootfs
ubinize -o "$out/rootfs.ubi" -p 128KiB -m 2048 -s 2048 "$here/../flash/ubinize.cfg"
echo ">> rootfs.ubi ($(stat -c%s "$out/rootfs.ubi") bytes) -> $out/"
