#!/bin/sh
# Build the Devuan + PowerVR SGX530 root filesystem (glibc, sysvinit) as an ext4 image.
#
# Two-phase bootstrap so the maemo GPU packages don't drag in the whole Hildon desktop:
#   Phase 1: a clean Devuan daedalus armhf base (mmdebstrap, no maemo repo).
#   Phase 2: add the maemo-leste repo and explicitly install ONLY the SGX/GLES userspace.
# Then graft the 7.2 SGX kernel modules (pvrsrvkm) + a small overlay, and make an ext4.
#
# The GPU userspace is Imagination's closed DDK (sgx-ddk-um-ti343x, core rev 1.2.1) from
# maemo-leste; the kernel side (pvrsrvkm) is grafted from kernel/build-devuan.sh, and the
# core-rev whitelist patch lets the 1.2.1 ukernel run on the B4 1.0.3 silicon.
#
# NO swap is baked in -- add it on the board later (e.g. a swapfile or a USB stick).
#
# Run as root (mounts + chroot). Env: OUT (default ../out), WORK (default ../build-devuan),
# ARCH_DEB (armhf). Consumes OUT/modroot-devuan (from kernel/build-devuan.sh).
set -eu

here=$(cd "$(dirname "$0")" && pwd)
out=${OUT:-$here/../out}
work=${WORK:-$here/../build-devuan}
keys="$here/keys-devuan"
R="$work/rootfs-devuan"
mods="$out/modroot-devuan"
mkdir -p "$out" "$work"

[ "$(id -u)" = 0 ] || { echo "run as root (sudo -E $0)" >&2; exit 1; }
[ -d "$mods" ] || { echo "missing $mods (run kernel/build-devuan.sh first)" >&2; exit 1; }

DEVKR="$keys/devuan-archive-keyring.gpg"

# Host apt/sqv: allow SHA1 for the Devuan daedalus release key (older signature).
cat > "$work/allow-sha1.toml" <<'POL'
[hash_algorithms.sha1]
collision_resistance = "always"
second_preimage_resistance = "always"
POL
export SEQUOIA_CRYPTO_POLICY="$work/allow-sha1.toml"

# Dearmor the maemo repo keys for the chroot's gpgv.
gpg --dearmor < "$keys/maemo-main-repo-key.asc" > "$work/maemo-main.gpg"
gpg --dearmor < "$keys/maemo-extras-key.asc"    > "$work/maemo-extras.gpg"

cleanup() {
    for mp in $(awk '{print $2}' /proc/mounts | grep "^$R" | sort -r); do
        umount -R "$mp" 2>/dev/null || umount -lf "$mp" 2>/dev/null || true
    done
}
trap cleanup EXIT
if [ -d "$R" ]; then cleanup; rm -rf "$R"; fi

echo "=== PHASE 1: clean Devuan daedalus base (no maemo repo) ==="
mmdebstrap --arch="${ARCH_DEB:-armhf}" --variant=apt \
    --keyring="$DEVKR" \
    --components="main" \
    --include="sysvinit-core,eudev,kmod,ifupdown,isc-dhcp-client,iproute2,openssh-server,ca-certificates,haveged,e2fsprogs" \
    --aptopt='APT::Sandbox::User "root"' \
    --aptopt='APT::Install-Recommends "false"' \
    --aptopt='Acquire::Retries "5"' \
    --aptopt='Acquire::Check-Valid-Until "false"' \
    daedalus "$R" \
    "deb http://deb.devuan.org/merged daedalus main"
echo "PHASE1 base: $(du -sh "$R" | cut -f1)"

echo "=== PHASE 2: add maemo repo + install ONLY the SGX/GLES userspace ==="
cp "$work/maemo-main.gpg"   "$R/etc/apt/trusted.gpg.d/maemo-main.gpg"
cp "$work/maemo-extras.gpg" "$R/etc/apt/trusted.gpg.d/maemo-extras.gpg"
cp "$DEVKR"                 "$R/etc/apt/trusted.gpg.d/devuan-archive-keyring.gpg"
echo "deb https://maedevu.maemo.org/leste daedalus main" > "$R/etc/apt/sources.list.d/maemo.list"
echo 'Acquire::ForceIPv4 "true";' > "$R/etc/apt/apt.conf.d/99force-ipv4"
cp /etc/resolv.conf "$R/etc/resolv.conf"
mount --bind /proc "$R/proc"; mount --bind /sys "$R/sys"
mount --bind /dev "$R/dev";   mount --bind /dev/pts "$R/dev/pts"
chroot "$R" apt-get -o APT::Sandbox::User=root -o Acquire::Check-Valid-Until=false update
chroot "$R" apt-get -o APT::Sandbox::User=root -y --no-install-recommends install \
    sgx-ddk-um-ti343x sgx-ddk-um-tools libgles2-mesa libegl1-mesa libegl-mesa0 \
    libgl1-mesa-dri libgbm1 mesa-utils kmscube drm-info
cleanup

echo "=== GRAFT: 7.2 SGX modules ==="
krel=$(ls "$mods/lib/modules" | head -1)
rm -rf "$R/lib/modules"; mkdir -p "$R/lib/modules"
cp -a "$mods/lib/modules/$krel" "$R/lib/modules/$krel"
rm -f "$R/lib/modules/$krel/build" "$R/lib/modules/$krel/source"
depmod -b "$R" "$krel"

echo "=== GRAFT: system config ==="
echo "root:beagle" | chroot "$R" chpasswd
sed -i 's/^#*PermitRootLogin.*/PermitRootLogin yes/' "$R/etc/ssh/sshd_config" 2>/dev/null || true
echo beagle > "$R/etc/hostname"
grep -q beagle "$R/etc/hosts" || echo "127.0.1.1 beagle" >> "$R/etc/hosts"
[ -f "$R/etc/inittab" ] || echo "id:2:initdefault:" > "$R/etc/inittab"
grep -q ttyS2 "$R/etc/inittab" || echo "T2:23:respawn:/sbin/agetty -L 115200 ttyS2 vt100" >> "$R/etc/inittab"
cat > "$R/etc/network/interfaces" <<'NET'
auto lo
iface lo inet loopback
allow-hotplug eth0
iface eth0 inet dhcp
NET
cat > "$R/etc/fstab" <<'FST'
/dev/mmcblk0p2 / ext4 rw,noatime 0 1
FST

echo "=== GRAFT: mesa pvr override + gpu-test helper ==="
echo 'export MESA_LOADER_DRIVER_OVERRIDE=pvr' > "$R/etc/profile.d/mesa-pvr.sh"
cat > "$R/root/gpu-test.sh" <<'GPU'
#!/bin/sh
# Quick SGX530 sanity check: init services, then a surfaceless GLES render probe.
echo "== dri devices =="; ls -l /dev/dri 2>&1
echo "== sgx module =="; modprobe pvrsrvkm_omap3_sgx530_121 2>/dev/null; lsmod | grep -i pvr
echo "== pvrsrvinit =="; command -v pvrsrvinit >/dev/null && pvrsrvinit && echo "(ran)" || echo "(no pvrsrvinit)"
export MESA_LOADER_DRIVER_OVERRIDE=pvr EGL_PLATFORM=surfaceless
echo "== eglinfo =="; command -v eglinfo >/dev/null && eglinfo 2>&1 | grep -iE 'vendor|render|version' | head || echo "(no eglinfo)"
GPU
chmod +x "$R/root/gpu-test.sh"

echo "=== GRAFT: root ssh key ==="
mkdir -p "$R/root/.ssh"; chmod 700 "$R/root/.ssh"
# Replace with YOUR public key(s). This one is the original maintainer's.
cat > "$R/root/.ssh/authorized_keys" <<'KEY'
ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIBW+Ivk8bMA/yJmGd9XaEoa5b/JzGHEnXPn3lApRCjm0 claude-code@geoduck-truenas
KEY
chmod 600 "$R/root/.ssh/authorized_keys"

echo "=== build ext4 (loop-free) ==="
rm -f "$out/rootfs-devuan.ext4"
sz=$(du -sb "$R" | cut -f1); mb=$(( sz / 1048576 + 400 ))
truncate -s "${mb}M" "$out/rootfs-devuan.ext4"
mkfs.ext4 -F -q -m1 -d "$R" "$out/rootfs-devuan.ext4"
echo ">> $out/rootfs-devuan.ext4 (${mb}M, kernel $krel)"
