#!/bin/bash
# Configure the trilobite appliance rootfs (run on geoduck-tools).
set -e
cd /mnt/scratch/geoduck-tmp/beagle/rootfs-nand

# hostname
echo trilobite > etc/hostname

# inittab: login on ttyS2 (console) AND ttyS1
cat > etc/inittab <<'EOF'
::sysinit:/sbin/openrc sysinit
::sysinit:/sbin/openrc boot
::wait:/sbin/openrc default
ttyS2::respawn:/sbin/getty -L 115200 ttyS2 vt100
ttyS1::respawn:/sbin/getty -L 115200 ttyS1 vt100
::ctrlaltdel:/sbin/reboot
::shutdown:/sbin/openrc shutdown
::shutdown:/bin/umount -a -r
EOF

# network: eth0 DHCP
mkdir -p etc/network
cat > etc/network/interfaces <<'EOF'
auto lo
iface lo inet loopback

auto eth0
iface eth0 inet dhcp
EOF

# services in default runlevel (inittab waits for CRNG readiness via sbin/rng-seed)
mkdir -p etc/runlevels/default
for s in sshd networking; do [ -f etc/init.d/$s ] && ln -sf /etc/init.d/$s etc/runlevels/default/$s; done
[ -f etc/init.d/bluetooth ] && ln -sf /etc/init.d/bluetooth etc/runlevels/default/bluetooth

# modules to load at boot: USB host glue + NIC + BT (musb_hdrc pulled as dep)
cat > etc/modules <<'EOF'
phy-twl4030-usb
omap2430
r8152
btusb
EOF

# root: authorize SSH key (from geoduck_truenas)
mkdir -p root/.ssh && chmod 700 root/.ssh
echo 'ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIBW+Ivk8bMA/yJmGd9XaEoa5b/JzGHEnXPn3lApRCjm0 claude-code@geoduck-truenas' > root/.ssh/authorized_keys
chmod 600 root/.ssh/authorized_keys

# sshd: key-only root, no password auth
if grep -qE '^#?PermitRootLogin' etc/ssh/sshd_config; then
  sed -i 's/^#\?PermitRootLogin.*/PermitRootLogin prohibit-password/' etc/ssh/sshd_config
else echo 'PermitRootLogin prohibit-password' >> etc/ssh/sshd_config; fi
grep -q '^PasswordAuthentication no' etc/ssh/sshd_config || echo 'PasswordAuthentication no' >> etc/ssh/sshd_config

# passwordless root on the (trusted, physical) serial console; ssh stays key-only
sed -i 's|^root:[^:]*:|root::|' etc/shadow

echo "=== config summary ==="
echo "hostname: $(cat etc/hostname)"
echo "gettys:"; grep getty etc/inittab
echo "default runlevel:"; ls etc/runlevels/default/
echo "root shadow:"; grep '^root:' etc/shadow
echo "sshd:"; grep -E 'PermitRootLogin|PasswordAuthentication' etc/ssh/sshd_config

