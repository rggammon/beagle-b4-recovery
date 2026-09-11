#!/bin/sh
# Build the Devuan + PowerVR SGX530 root filesystem (glibc, sysvinit) as an ext4 image.
#
# Two-phase bootstrap so the GPU packages don't drag in a desktop environment:
#   Phase 1: a clean Devuan daedalus armhf base (mmdebstrap, no maemo repo).
#   Phase 2: install the local matched DDK 1.6 armel packages (softfp, /opt/sgx-ddk16).
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
mkdir -p "$out" "$work"

[ "$(id -u)" = 0 ] || { echo "run as root (sudo -E $0)" >&2; exit 1; }
[ -d "$mods" ] || { echo "missing $mods (run kernel/build-devuan.sh first)" >&2; exit 1; }
[ -d "$ddk16_deb_dir" ] || {
    echo 'set SGX_DDK16_DEB_DIR to the directory containing the DDK 1.6 packages' >&2
    exit 1
}
ddk16_um=$(find "$ddk16_deb_dir" -maxdepth 1 -type f -name 'sgx-ddk16-um_*_armel.deb' -print -quit)
ddk16_tools=$(find "$ddk16_deb_dir" -maxdepth 1 -type f -name 'sgx-ddk16-tools_*_armel.deb' -print -quit)
ddk16_dev=$(find "$ddk16_deb_dir" -maxdepth 1 -type f -name 'sgx-ddk16-dev_*_armel.deb' -print -quit)
[ -n "$ddk16_um" ] || { echo "missing sgx-ddk16-um armel package in $ddk16_deb_dir" >&2; exit 1; }
[ -n "$ddk16_tools" ] || { echo "missing sgx-ddk16-tools armel package in $ddk16_deb_dir" >&2; exit 1; }
[ -n "$ddk16_dev" ] || { echo "missing sgx-ddk16-dev armel package in $ddk16_deb_dir" >&2; exit 1; }
[ "$(dpkg-deb -f "$ddk16_um" Architecture)" = armel ] || { echo "DDK runtime package is not armel" >&2; exit 1; }
[ "$(dpkg-deb -f "$ddk16_tools" Architecture)" = armel ] || { echo "DDK tools package is not armel" >&2; exit 1; }
[ "$(dpkg-deb -f "$ddk16_dev" Architecture)" = armel ] || { echo "DDK dev package is not armel" >&2; exit 1; }
[ "$(dpkg-deb -f "$ddk16_um" Version)" = "$(dpkg-deb -f "$ddk16_tools" Version)" ] || {
    echo "DDK runtime and tools package versions do not match" >&2
    exit 1
}
[ "$(dpkg-deb -f "$ddk16_um" Version)" = "$(dpkg-deb -f "$ddk16_dev" Version)" ] || {
    echo "DDK runtime and dev package versions do not match" >&2
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

echo "=== PHASE 2: install the matched DDK 1.6 soft-float userspace ==="
cp "$DEVKR" "$R/etc/apt/trusted.gpg.d/devuan-archive-keyring.gpg"
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

# The Devuan 'merged' mirror is load-balanced; a backend occasionally 404s a pool
# file its Packages index already lists. Acquire::Retries does not retry a 404, so
# refresh the indices and retry the whole install (a new connection may hit a
# backend that has the file).
apt_install_retry() {
    _n=1
    while :; do
        chroot "$R" apt-get -o APT::Sandbox::User=root -o Acquire::Retries=5 \
            -y --no-install-recommends install "$@" && return 0
        [ "$_n" -lt 4 ] || return 1
        echo ">> apt install failed (attempt $_n); refreshing indices and retrying" >&2
        chroot "$R" apt-get -o APT::Sandbox::User=root -o Acquire::Check-Valid-Until=false update || true
        _n=$((_n + 1))
    done
}

install -m 0644 "$ddk16_um" "$R/tmp/sgx-ddk16-um_armel.deb"
install -m 0644 "$ddk16_tools" "$R/tmp/sgx-ddk16-tools_armel.deb"
install -m 0644 "$ddk16_dev" "$R/tmp/sgx-ddk16-dev_armel.deb"
apt_install_retry \
    /tmp/sgx-ddk16-um_armel.deb /tmp/sgx-ddk16-tools_armel.deb \
    /tmp/sgx-ddk16-dev_armel.deb \
    drm-info

# On-board soft-float armel build toolchain. The board's glibc (2.36) matches the
# target, so DDK-linked programs compile natively with no ABI/glibc-version shim.
# gcc-12:armel installs via the enabled armel multiarch; there is no armhf gcc to
# clash with over /usr/bin/gcc, so the usual co-install conflict does not apply.
# (gcc-arm-linux-gnueabi is not in daedalus main, so the cross metapackage is not.)
apt_install_retry \
    gcc-12:armel make libc6-dev:armel pkg-config
chroot "$R" update-alternatives --install /usr/bin/arm-linux-gnueabi-gcc \
    arm-linux-gnueabi-gcc /usr/bin/arm-linux-gnueabi-gcc-12 50
chroot "$R" update-alternatives --install /usr/bin/cc cc /usr/bin/arm-linux-gnueabi-gcc-12 50

rm -f "$R/usr/sbin/policy-rc.d" "$R/tmp/sgx-ddk16-um_armel.deb" \
    "$R/tmp/sgx-ddk16-tools_armel.deb" "$R/tmp/sgx-ddk16-dev_armel.deb"

# One-command build wrapper for DDK 1.6 EGL/GLES2 programs.
cat > "$R/usr/local/bin/sgx-cc" <<'CC'
#!/bin/sh
# Compile a soft-float armel program against the DDK 1.6 EGL/GLES2 libraries.
export PKG_CONFIG_PATH=/usr/lib/arm-linux-gnueabi/pkgconfig
exec arm-linux-gnueabi-gcc "$@" \
    $(pkg-config --cflags glesv2 egl) \
    $(pkg-config --libs glesv2 egl) \
    -Wl,-rpath,/opt/sgx-ddk16/lib -Wl,-rpath-link,/opt/sgx-ddk16/lib
CC
chmod 755 "$R/usr/local/bin/sgx-cc"

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

echo "=== VERIFY: on-board armel build toolchain + DDK dev headers ==="
[ "$(chroot "$R" dpkg-query -W -f='${Status}' sgx-ddk16-dev:armel)" = "install ok installed" ] || {
    echo "sgx-ddk16-dev is not installed" >&2; exit 1; }
[ -x "$R/usr/bin/arm-linux-gnueabi-gcc-12" ] || { echo "armel gcc-12 missing" >&2; exit 1; }
chroot "$R" sh -c 'command -v arm-linux-gnueabi-gcc >/dev/null' || { echo "arm-linux-gnueabi-gcc missing" >&2; exit 1; }
[ -x "$R/usr/local/bin/sgx-cc" ] || { echo "sgx-cc wrapper missing" >&2; exit 1; }
[ -f "$R/usr/include/sgx-ddk16/GLES2/gl2.h" ] || { echo "GLES2 headers missing" >&2; exit 1; }
[ -f "$R/usr/include/sgx-ddk16/EGL/egl.h" ] || { echo "EGL headers missing" >&2; exit 1; }
[ -f "$R/usr/lib/arm-linux-gnueabi/pkgconfig/glesv2.pc" ] || { echo "glesv2.pc missing" >&2; exit 1; }
printf '#include <EGL/egl.h>\nint main(void){ return eglGetError() != EGL_SUCCESS; }\n' > "$R/tmp/egl-smoke.c"
chroot "$R" sh -c 'cd /tmp && sgx-cc egl-smoke.c -o egl-smoke' || {
    echo "on-board toolchain smoke compile failed" >&2; exit 1; }
[ -f "$R/tmp/egl-smoke" ] || { echo "toolchain smoke binary not produced" >&2; exit 1; }
rm -f "$R/tmp/egl-smoke.c" "$R/tmp/egl-smoke"

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

echo "=== GRAFT: NanoVG on-board build tree ==="
nvg_work="$work/nanovg-src"
nvg_commit=${NANOVG_COMMIT:-ce3bf745eb2d2dbc14a50bf2446783f691ac4353}
rm -rf "$nvg_work"
nvg_attempt=1
while ! timeout 180 git clone https://github.com/memononen/nanovg "$nvg_work"; do
    rm -rf "$nvg_work"
    [ "$nvg_attempt" -lt 3 ] || { echo "failed to clone nanovg after $nvg_attempt attempts" >&2; exit 1; }
    nvg_attempt=$((nvg_attempt + 1))
done
git -C "$nvg_work" -c advice.detachedHead=false checkout "$nvg_commit"
nvg_dst="$R/usr/src/nanovg-demo"
mkdir -p "$nvg_dst/nanovg"
cp "$nvg_work"/src/nanovg.c "$nvg_work"/src/*.h "$nvg_dst/nanovg/"
cp "$here/../tools/nanovg-demo.c" "$nvg_dst/"
cat > "$nvg_dst/Makefile" <<'MK'
# Build the NanoVG demo soft-float armel against the DDK 1.6 EGL/GLES2 libraries.
CC = arm-linux-gnueabi-gcc
CFLAGS = -O2 -Inanovg -I/usr/include/sgx-ddk16
LDFLAGS = -L/opt/sgx-ddk16/lib -Wl,-rpath,/opt/sgx-ddk16/lib -Wl,-rpath-link,/opt/sgx-ddk16/lib
LDLIBS = -lEGL -lGLESv2 -lm

nanovg-demo: nanovg-demo.c nanovg/nanovg.c
	$(CC) $(CFLAGS) -DNANOVG_GLES2_IMPLEMENTATION nanovg-demo.c nanovg/nanovg.c $(LDFLAGS) $(LDLIBS) -o $@

clean:
	rm -f nanovg-demo
MK

echo "=== GRAFT: gpu-test helper ==="
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
