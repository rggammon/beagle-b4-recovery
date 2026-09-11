#!/bin/sh
# Build the Devuan + PowerVR SGX530 root filesystem (glibc, sysvinit) as an ext4 image.
#
# Two-phase bootstrap so the GPU packages don't drag in a desktop environment:
#   Phase 1: a clean Devuan daedalus armhf base (mmdebstrap, no maemo repo).
#   Phase 2: install the local matched DDK 1.6 armel packages, then use the
#            maemo-leste repo only for the SGX-aware Mesa/GBM components.
# Then graft the 7.2 SGX kernel modules (pvrsrvkm) + a small overlay, and make an ext4.
#
# The GPU userspace and kernel are both DDK 1.6.16.3977. The proprietary soft-float
# userspace is isolated under /opt/sgx-ddk16 and runs with Devuan's armel libc.
#
# NO swap is baked in -- add it on the board later (e.g. a swapfile or a USB stick).
#
# Run as root (mounts + chroot). Env: OUT (default ../out), WORK (default ../build-devuan),
# ARCH_DEB (armhf), SGX_DDK16_DEB_DIR. Consumes OUT/modroot-devuan (from
# kernel/build-devuan.sh) and the sgx-ddk16-um/tools armel packages.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
out=${OUT:-$here/../out}
work=${WORK:-$here/../build-devuan}
keys="$here/keys-devuan"
R="$work/rootfs-devuan"
mods="$out/modroot-devuan"
ddk16_deb_dir=${SGX_DDK16_DEB_DIR:-}
cross=${CROSS_COMPILE:-arm-linux-gnueabihf-}
mkdir -p "$out" "$work"

[ "$(id -u)" = 0 ] || { echo "run as root (sudo -E $0)" >&2; exit 1; }
[ -d "$mods" ] || { echo "missing $mods (run kernel/build-devuan.sh first)" >&2; exit 1; }
[ -d "$ddk16_deb_dir" ] || {
    echo 'set SGX_DDK16_DEB_DIR to the directory containing the DDK 1.6 packages' >&2
    exit 1
}
ddk16_um=$(find "$ddk16_deb_dir" -maxdepth 1 -type f -name 'sgx-ddk16-um_*_armel.deb' -print -quit)
ddk16_tools=$(find "$ddk16_deb_dir" -maxdepth 1 -type f -name 'sgx-ddk16-tools_*_armel.deb' -print -quit)
[ -n "$ddk16_um" ] || { echo "missing sgx-ddk16-um armel package in $ddk16_deb_dir" >&2; exit 1; }
[ -n "$ddk16_tools" ] || { echo "missing sgx-ddk16-tools armel package in $ddk16_deb_dir" >&2; exit 1; }
[ "$(dpkg-deb -f "$ddk16_um" Architecture)" = armel ] || { echo "DDK runtime package is not armel" >&2; exit 1; }
[ "$(dpkg-deb -f "$ddk16_tools" Architecture)" = armel ] || { echo "DDK tools package is not armel" >&2; exit 1; }
[ "$(dpkg-deb -f "$ddk16_um" Version)" = "$(dpkg-deb -f "$ddk16_tools" Version)" ] || {
    echo "DDK runtime and tools package versions do not match" >&2
    exit 1
}

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
    for mp in "$R/dev/pts" "$R/dev" "$R/sys" "$R/proc"; do
        if mountpoint -q "$mp"; then
            umount "$mp" 2>/dev/null || umount -l "$mp" 2>/dev/null || true
        fi
    done
}
trap cleanup EXIT
if [ -d "$R" ]; then cleanup; rm -rf "$R"; fi

echo "=== PHASE 1: clean Devuan daedalus base (no maemo repo) ==="
mmdebstrap --arch="${ARCH_DEB:-armhf}" --variant=apt \
    --keyring="$DEVKR" \
    --components="main" \
    --include="sysvinit-core,eudev,kmod,ifupdown,isc-dhcp-client,iproute2,iputils-ping,chrony,openssh-server,ca-certificates,e2fsprogs,usbutils,ethtool" \
    --aptopt='APT::Sandbox::User "root"' \
    --aptopt='APT::Install-Recommends "false"' \
    --aptopt='Acquire::Retries "5"' \
    --aptopt='Acquire::Check-Valid-Until "false"' \
    daedalus "$R" \
    "deb http://deb.devuan.org/merged daedalus main"
echo "PHASE1 base: $(du -sh "$R" | cut -f1)"

echo "=== PHASE 2: install matched DDK 1.6 + SGX-aware Mesa components ==="
cp "$work/maemo-main.gpg"   "$R/etc/apt/trusted.gpg.d/maemo-main.gpg"
cp "$work/maemo-extras.gpg" "$R/etc/apt/trusted.gpg.d/maemo-extras.gpg"
cp "$DEVKR"                 "$R/etc/apt/trusted.gpg.d/devuan-archive-keyring.gpg"
echo "deb https://maedevu.maemo.org/leste daedalus main" > "$R/etc/apt/sources.list.d/maemo.list"
echo 'Acquire::ForceIPv4 "true";' > "$R/etc/apt/apt.conf.d/99force-ipv4"
cp /etc/resolv.conf "$R/etc/resolv.conf"
mount --bind /proc "$R/proc"; mount --bind /sys "$R/sys"
mount --bind /dev "$R/dev";   mount --bind /dev/pts "$R/dev/pts"
chroot "$R" dpkg --add-architecture armel
chroot "$R" apt-get -o APT::Sandbox::User=root -o Acquire::Check-Valid-Until=false update

# Package service scripts cannot run until the custom kernel modules are grafted.
cat > "$R/usr/sbin/policy-rc.d" <<'SHIM'
#!/bin/sh
exit 101
SHIM
chmod 755 "$R/usr/sbin/policy-rc.d"
install -m 0644 "$ddk16_um" "$R/tmp/sgx-ddk16-um_armel.deb"
install -m 0644 "$ddk16_tools" "$R/tmp/sgx-ddk16-tools_armel.deb"
chroot "$R" apt-get -o APT::Sandbox::User=root -y --no-install-recommends install \
    /tmp/sgx-ddk16-um_armel.deb /tmp/sgx-ddk16-tools_armel.deb \
    libgles2-mesa libegl1-mesa libegl-mesa0 \
    libgl1-mesa-dri libgbm1 mesa-utils kmscube drm-info
rm -f "$R/usr/sbin/policy-rc.d" "$R/tmp/sgx-ddk16-um_armel.deb" \
    "$R/tmp/sgx-ddk16-tools_armel.deb"

echo "=== VERIFY: matched DDK 1.6 soft-float runtime ==="
chroot "$R" dpkg --print-foreign-architectures | grep -qx armel
for package in sgx-ddk16-um:armel sgx-ddk16-tools:armel \
    libc6:armel libgcc-s1:armel libstdc++6:armel; do
    [ "$(chroot "$R" dpkg-query -W -f='${Status}' "$package")" = "install ok installed" ] || {
        echo "required package is not fully installed: $package" >&2
        exit 1
    }
done
chroot "$R" dpkg-query -W -f='${binary:Package} ${Architecture} ${Version} ${db:Status-Abbrev}\n' \
    sgx-ddk16-um:armel sgx-ddk16-tools:armel \
    libc6:armel libgcc-s1:armel libstdc++6:armel
[ -f "$R/opt/sgx-ddk16/lib/libEGL.so" ]
[ -f "$R/opt/sgx-ddk16/lib/libGLESv2.so" ]
[ -f "$R/opt/sgx-ddk16/lib/libpvrPVR2D_FLIPWSEGL.so" ]
[ -x "$R/usr/bin/sgx-ddk16-run" ]
[ "$(readlink "$R/usr/bin/pvrsrvinit")" = sgx-ddk16-run ]
[ -x "$R/etc/init.d/powervr" ]
powervr_enabled=false
for link in "$R"/etc/rc[2345].d/S*powervr; do
    if [ -L "$link" ]; then
        powervr_enabled=true
        break
    fi
done
[ "$powervr_enabled" = true ] || { echo "powervr SysV service is not enabled" >&2; exit 1; }
grep -Fxq 'WindowSystem=libpvrPVR2D_FLIPWSEGL.so' "$R/etc/powervr.ini"
grep -Fxq 'options dcnohw present=2' "$R/etc/modprobe.d/sgx-ddk16.conf"
if chroot "$R" dpkg-query -W -f='${db:Status-Abbrev}\n' \
    sgx-ddk-um-ti343x sgx-ddk-um-tools 2>/dev/null | grep -q '^ii'; then
    echo "mismatched Maemo SGX userspace is still installed" >&2
    exit 1
fi

# The packaged Mesa enables SGX but omits its omapdrm display-driver alias. Build
# that alias from the exact Maemo-Leste source, then build a newer kmscube whose
# framebuffer path handles DRM_FORMAT_MOD_INVALID correctly.
chroot "$R" apt-get -o APT::Sandbox::User=root -y --no-install-recommends install \
    libdrm-dev libgbm-dev libegl-dev libgles-dev libstdc++-12-dev \
    zlib1g-dev libexpat1-dev
cat > "$work/armhf.ini" <<EOF
[binaries]
c = ['${cross}gcc', '--sysroot=$R']
cpp = ['${cross}g++', '--sysroot=$R']
ar = '${cross}ar'
strip = '${cross}strip'
pkg-config = 'pkg-config'

[properties]
sys_root = '$R'
pkg_config_libdir = ['$R/usr/lib/arm-linux-gnueabihf/pkgconfig', '$R/usr/share/pkgconfig']
needs_exe_wrapper = true

[host_machine]
system = 'linux'
cpu_family = 'arm'
cpu = 'armv7'
endian = 'little'
EOF

mesa_archive="$work/mesa_22.3.6+sgx2.orig.tar.gz"
mesa_src="$work/mesa-src"
mesa_build="$work/mesa-build"
rm -rf "$mesa_src" "$mesa_build" "$mesa_archive"
timeout 180 wget --tries=3 --timeout=30 -O "$mesa_archive" \
    https://maedevu.maemo.org/leste/pool/main/m/mesa/mesa_22.3.6+sgx2.orig.tar.gz
echo 'f023f52de624ac3ed7162e3ea19d94e1b707457ff6b59dac1d0db8c74781e21f  '"$mesa_archive" | sha256sum -c -
mkdir -p "$mesa_src"
tar -xzf "$mesa_archive" -C "$mesa_src" --strip-components=1
cp "$here/../tools/mesa-bookworm-compat.c" \
    "$mesa_src/src/gallium/targets/dri/mesa-bookworm-compat.c"
sed -i "s/files('target.c'),/files('target.c', 'mesa-bookworm-compat.c'),/" \
    "$mesa_src/src/gallium/targets/dri/meson.build"
grep -q "files('target.c', 'mesa-bookworm-compat.c')" \
    "$mesa_src/src/gallium/targets/dri/meson.build"
meson setup "$mesa_build" "$mesa_src" --cross-file "$work/armhf.ini" \
    -Dgallium-drivers=sgx -Dgallium-sgx-alias=omapdrm \
    -Dvulkan-drivers= -Ddri-drivers= -Dplatforms=null -Dglx=disabled \
    -Degl=enabled -Dgbm=enabled -Dllvm=disabled -Dshared-glapi=enabled \
    -Dgles1=disabled -Dgles2=enabled -Dosmesa=false -Dvalgrind=disabled \
    -Dbuild-tests=false
ninja -C "$mesa_build" src/gallium/targets/dri/omapdrm_dri.so
install -m 644 "$mesa_build/src/gallium/targets/dri/omapdrm_dri.so" \
    "$R/usr/lib/arm-linux-gnueabihf/dri/omapdrm_dri.so"

# Daedalus' kmscube passes DRM_FORMAT_MOD_INVALID to omapdrm as a real modifier.
kmscube_src="$work/kmscube-src"
kmscube_build="$work/kmscube-build"
kmscube_ref=f60e50e887d3c49e91ac9b06d8199b36152632fa
rm -rf "$kmscube_src" "$kmscube_build"
attempt=1
while ! timeout 180 git clone https://gitlab.freedesktop.org/mesa/kmscube.git "$kmscube_src"; do
    rm -rf "$kmscube_src"
    [ "$attempt" -lt 3 ] || { echo "failed to clone kmscube after $attempt attempts" >&2; exit 1; }
    attempt=$((attempt + 1))
done
git -C "$kmscube_src" checkout --detach "$kmscube_ref"
meson setup "$kmscube_build" "$kmscube_src" \
    --cross-file "$work/armhf.ini" -Dgstreamer=disabled
ninja -C "$kmscube_build" kmscube
install -m 755 "$kmscube_build/kmscube" "$R/usr/local/bin/kmscube"
chroot "$R" apt-get -o APT::Sandbox::User=root -y purge \
    libdrm-dev libgbm-dev libegl-dev libgles-dev libstdc++-12-dev \
    zlib1g-dev libexpat1-dev

chroot "$R" update-rc.d chrony defaults
cat > "$R/etc/init.d/beagle-memory" <<'SYSV'
#!/bin/sh
### BEGIN INIT INFO
# Provides:          beagle-memory
# Required-Start:    $local_fs
# Required-Stop:
# Default-Start:     2 3 4 5
# Default-Stop:      0 1 6
# Short-Description: Enable zram and optional USB storage
### END INIT INFO

case "${1:-}" in
    start)
        modprobe zram
        if [ "$(cat /sys/block/zram0/disksize)" = 0 ]; then
            echo $((64 * 1024 * 1024)) > /sys/block/zram0/disksize
            mkswap -L zram0 /dev/zram0 >/dev/null
        fi
        swapon -p 100 /dev/zram0 2>/dev/null || true

        mkdir -p /mnt/usb
        usb_data=$(blkid -L beagle-usb 2>/dev/null || true)
        usb_swap=$(blkid -L beagle-swap 2>/dev/null || true)
        [ -z "$usb_data" ] || mountpoint -q /mnt/usb || mount "$usb_data" /mnt/usb
        [ -z "$usb_swap" ] || swapon -p 10 "$usb_swap" 2>/dev/null || true
        ;;
    stop)
        swapoff -L beagle-swap 2>/dev/null || true
        swapoff /dev/zram0 2>/dev/null || true
        umount /mnt/usb 2>/dev/null || true
        ;;
    restart|force-reload)
        "$0" stop
        "$0" start
        ;;
    *)
        echo "Usage: $0 {start|stop|restart|force-reload}" >&2
        exit 1
        ;;
esac
SYSV
chmod 755 "$R/etc/init.d/beagle-memory"
chroot "$R" update-rc.d beagle-memory defaults
cleanup
for mp in "$R/dev/pts" "$R/dev" "$R/sys" "$R/proc"; do
    if mountpoint -q "$mp"; then
        echo "failed to unmount $mp" >&2
        exit 1
    fi
done

echo "=== GRAFT: 7.2 SGX modules ==="
krel=$(ls "$mods/lib/modules" | head -1)
rm -rf "$R/lib/modules"; mkdir -p "$R/lib/modules"
cp -a "$mods/lib/modules/$krel" "$R/lib/modules/$krel"
rm -f "$R/lib/modules/$krel/build" "$R/lib/modules/$krel/source"
depmod -b "$R" "$krel"

pvrsrvkm_module=$(find "$R/lib/modules/$krel" -type f -name 'pvrsrvkm.ko' -print -quit)
dcnohw_module=$(find "$R/lib/modules/$krel" -type f -name 'dcnohw.ko' -print -quit)
omapdrm_module=$(find "$R/lib/modules/$krel" -type f -name 'omapdrm.ko' -print -quit)
[ -n "$pvrsrvkm_module" ] || { echo "image is missing pvrsrvkm.ko" >&2; exit 1; }
[ -n "$dcnohw_module" ] || { echo "image is missing dcnohw.ko" >&2; exit 1; }
[ -n "$omapdrm_module" ] || { echo "image is missing omapdrm.ko" >&2; exit 1; }
grep -a -q 'omapdrm_present' "$omapdrm_module" || {
    echo "omapdrm.ko is missing the Stage 6 presentation helpers" >&2
    exit 1
}
grep -a -q 'omapdrm_present' "$dcnohw_module" || {
    echo "dcnohw.ko is not linked to the Stage 6 presentation helpers" >&2
    exit 1
}

echo "=== GRAFT: system config ==="
echo "root:beagle" | chroot "$R" chpasswd
sed -i 's/^#*PermitRootLogin.*/PermitRootLogin yes/' "$R/etc/ssh/sshd_config" 2>/dev/null || true
echo beagle > "$R/etc/hostname"
grep -q beagle "$R/etc/hosts" || echo "127.0.1.1 beagle" >> "$R/etc/hosts"
grep -q '^makestep ' "$R/etc/chrony/chrony.conf" || echo 'makestep 1.0 3' >> "$R/etc/chrony/chrony.conf"
grep -q '^rtcsync$' "$R/etc/chrony/chrony.conf" || echo 'rtcsync' >> "$R/etc/chrony/chrony.conf"
[ -f "$R/etc/inittab" ] || echo "id:2:initdefault:" > "$R/etc/inittab"
grep -q ttyS2 "$R/etc/inittab" || echo "T2:23:respawn:/sbin/agetty -L 115200 ttyS2 vt100" >> "$R/etc/inittab"
grep -q ttyS1 "$R/etc/inittab" || echo "T1:23:respawn:/sbin/agetty -L 115200 ttyS1 vt100" >> "$R/etc/inittab"
# getty on the framebuffer console (tty1) -> a login on the HDMI panel (omapdrm fbcon).
grep -q "tty1" "$R/etc/inittab" || echo "1:2345:respawn:/sbin/agetty --noclear tty1 linux" >> "$R/etc/inittab"
cat > "$R/etc/network/interfaces" <<'NET'
auto lo
iface lo inet loopback
auto eth0
iface eth0 inet dhcp
NET
cat > "$R/etc/fstab" <<'FST'
/dev/mmcblk0p2 / ext4 rw,noatime 0 1
FST
cat > "$R/etc/modules" <<'MODULES'
omap_rng
phy-twl4030-usb
omap2430
usb-storage
usbhid
asix
r8152
MODULES

echo "=== GRAFT: mesa pvr override + gpu-test helper ==="
rm -f "$R/etc/profile.d/mesa-pvr.sh"
cat > "$R/root/gpu-test.sh" <<'GPU'
#!/bin/sh
# Read-only first-boot check for the matched DDK 1.6 stack. The powervr boot
# service owns pvrsrvinit; do not run it a second time against initialized services.
set -eu

echo "== packages =="
dpkg-query -W -f='${binary:Package} ${Architecture} ${Version} ${db:Status-Abbrev}\n' \
    sgx-ddk16-um:armel sgx-ddk16-tools:armel \
    libc6:armel libgcc-s1:armel libstdc++6:armel

echo "== modules =="
lsmod | grep -E '^(pvrsrvkm|dcnohw)'
printf 'dcnohw present='; cat /sys/module/dcnohw/parameters/present

echo "== TI Services test =="
log=/tmp/sgx-ddk16-services-test.log
if timeout 30 sgx-ddk16-run services_test >"$log" 2>&1; then
    grep -E 'DDK version|End loop' "$log"
    echo "SGX DDK 1.6 services: PASS"
else
    status=$?
    tail -40 "$log"
    echo "SGX DDK 1.6 services: FAIL (status $status)" >&2
    exit "$status"
fi
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
