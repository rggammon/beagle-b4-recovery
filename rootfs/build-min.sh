#!/bin/sh
# Build the MINIMAL, WRITABLE Alpine appliance base as an ext4 block image.
# This is the 3rd image (alongside the two read-only recovery images): a small rw root you
# customize on the board with `apk add ...`. Same kernel/U-Boot/DTB as recovery; the extra
# =m drivers (e.g. Bluetooth btusb) are installed as loadable modules under /lib/modules.
#
# Consumes out/{zImage,omap3-beagle-ab4.dtb,modroot/lib/modules/*} (run kernel/build.sh first).
# Env: ALPINE_BRANCH (default v3.24), SLACK_MB (free space over content, default 300), WORK, OUT.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
out=${OUT:-$here/../out}
work=${WORK:-$here/../build}
AMR_VER=v0.7.0
ALPINE_BRANCH=${ALPINE_BRANCH:-v3.24}
SLACK_MB=${SLACK_MB:-300}
mkdir -p "$work" "$out"
cd "$work"

# 1. base rootfs + packages, with in-chroot config (services, ssh host keys, sshd).
[ -x alpine-make-rootfs ] || wget -qO alpine-make-rootfs \
  "https://raw.githubusercontent.com/alpinelinux/alpine-make-rootfs/$AMR_VER/alpine-make-rootfs"
chmod +x alpine-make-rootfs
rm -rf rootfs-min

# armv7 APKINDEX is signed by a per-arch key alpine-make-rootfs doesn't embed; hand apk the
# armv7 key set via --keys-dir (same fetch the recovery build uses).
akeys="$work/alpine-keys/usr/share/apk/keys/armv7"
if [ ! -d "$akeys" ]; then
    base="https://dl-cdn.alpinelinux.org/alpine/$ALPINE_BRANCH/main/x86_64"
    ak=$(wget -qO- "$base/" | grep -oE 'alpine-keys-[0-9.]+-r[0-9]+\.apk' | sort -u | tail -1)
    rm -rf "$work/alpine-keys"; mkdir -p "$work/alpine-keys"
    wget -qO- "$base/$ak" | tar -xz -C "$work/alpine-keys"
fi

sudo APK_OPTS="--no-progress --arch armv7" ./alpine-make-rootfs \
    --branch "$ALPINE_BRANCH" \
    --keys-dir "$akeys" \
    --packages "$(cat "$here/packages-min.txt")" \
    --script-chroot \
    rootfs-min "$here/post-install-min.sh"

# 2. overlay (writable fstab, inittab w/ rng-seed, growroot service, authorized_keys...).
sudo cp -a "$here/overlay-min/." rootfs-min/
sudo chmod 700 rootfs-min/root/.ssh; sudo chmod 600 rootfs-min/root/.ssh/authorized_keys
sudo chmod 755 rootfs-min/sbin/rng-seed rootfs-min/etc/init.d/growroot

# 3. kernel + ab4 dtb into /boot.
sudo mkdir -p rootfs-min/boot
sudo cp "$out/zImage" rootfs-min/boot/zImage
sudo cp "$out/omap3-beagle-ab4.dtb" rootfs-min/boot/omap3-beagle-ab4.dtb

# 4. loadable modules (from kernel/build.sh `modules_install`); depmod ran there already.
if [ -d "$out/modroot/lib/modules" ]; then
    sudo cp -a "$out/modroot/lib/modules" rootfs-min/lib/
    echo ">> modules: $(ls "$out/modroot/lib/modules")"
else
    echo "WARNING: $out/modroot/lib/modules missing -- run kernel/build.sh; no modules installed" >&2
fi

# 5. writable ext4 sized to content + SLACK_MB (growroot expands it to the card on 1st boot).
used_mb=$(( ($(sudo du -sk rootfs-min | cut -f1) + 1023) / 1024 ))
size_mb=$(( used_mb + SLACK_MB ))
rm -f "$out/rootfs-min.ext4"
truncate -s "${size_mb}M" "$out/rootfs-min.ext4"
sudo mkfs.ext4 -F -q -m 1 -L rootfs -d rootfs-min "$out/rootfs-min.ext4"
echo ">> rootfs-min.ext4 (${size_mb}M, $(stat -c%s "$out/rootfs-min.ext4") bytes) -> $out/"
