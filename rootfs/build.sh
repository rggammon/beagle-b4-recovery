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

# alpine-make-rootfs only embeds the x86_64 signing keys, but each arch's APKINDEX is
# signed by a per-arch key -- so armv7 fails with "UNTRUSTED signature". Fetch the
# all-arch alpine-keys and hand apk the armv7 set via --keys-dir.
akeys="$work/alpine-keys/usr/share/apk/keys/armv7"
if [ ! -d "$akeys" ]; then
    base="https://dl-cdn.alpinelinux.org/alpine/$ALPINE_BRANCH/main/x86_64"
    ak=$(wget -qO- "$base/" | grep -oE 'alpine-keys-[0-9.]+-r[0-9]+\.apk' | sort -u | tail -1)
    rm -rf "$work/alpine-keys"; mkdir -p "$work/alpine-keys"
    wget -qO- "$base/$ak" | tar -xz -C "$work/alpine-keys"
fi

# v0.7.0 has no --arch flag: set the target arch via apk (APK_OPTS reaches every apk
# call incl. --initdb, which writes /etc/apk/arch). The armv7 chroot runs under qemu binfmt.
sudo APK_OPTS="--no-progress --arch armv7" ./alpine-make-rootfs \
    --branch "$ALPINE_BRANCH" \
    --keys-dir "$akeys" \
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

# 4. read-only UBIFS -> UBI image (for the NAND appliance).
sudo mkfs.ubifs -m 2048 -e 126976 -c 1900 -x lzo -o rootfs.ubifs -r rootfs
ubinize -o "$out/rootfs.ubi" -p 128KiB -m 2048 -s 2048 "$here/../flash/ubinize.cfg"
echo ">> rootfs.ubi ($(stat -c%s "$out/rootfs.ubi") bytes) -> $out/"

# 5. same tree as an ext4 block image (for the SD-card appliance). ext4 is built into
#    the kernel; squashfs is not. Mounted read-only, so it behaves like the NAND root.
sudo sed -i 's|^ubi0:rootfs / ubifs .*|/dev/mmcblk0p2 / ext4 ro,relatime 0 1|' rootfs/etc/fstab
rm -f "$out/rootfs.ext4"
truncate -s 400M "$out/rootfs.ext4"
sudo mkfs.ext4 -F -q -m 0 -L rootfs -d rootfs "$out/rootfs.ext4"
echo ">> rootfs.ext4 ($(stat -c%s "$out/rootfs.ext4") bytes) -> $out/"
