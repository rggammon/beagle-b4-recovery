#!/bin/sh
# Runs INSIDE the target rootfs (alpine-make-rootfs --script-chroot).
# Enables services, pre-generates ssh host keys (so sshd starts on a read-only root),
# and locks down sshd to key-only root.
set -eu

# entropy seed runs from inittab (::sysinit:/sbin/rng-seed) before OpenRC, not as a
# service -- haveged still runs as a service for ongoing entropy.
rc-update add haveged boot        2>/dev/null || true
rc-update add networking default  2>/dev/null || true
rc-update add sshd default        2>/dev/null || true
rc-update add bluetooth default   2>/dev/null || true
rc-update add chronyd default     2>/dev/null || true

# Pre-generate host keys at build time -- a read-only root can't create them at boot.
ssh-keygen -A

# sshd: key-only root login.
sed -i 's/^#\?PermitRootLogin.*/PermitRootLogin prohibit-password/' /etc/ssh/sshd_config
sed -i 's/^#\?PasswordAuthentication.*/PasswordAuthentication no/'  /etc/ssh/sshd_config

# root: passwordless on the (trusted, physical) serial console; ssh stays key-only
# (PasswordAuthentication no above). `passwd -u` alone leaves a locked/`!` field on a
# never-set account, which `login` still rejects -- so blank the shadow field outright.
passwd -u root 2>/dev/null || true
sed -i 's|^root:[^:]*:|root::|' /etc/shadow

# Read-only root: DHCP DNS lands on tmpfs (see /etc/udhcpc/udhcpc.conf RESOLV_CONF).
# /run is a tmpfs OpenRC mounts at boot, so this resolves once udhcpc writes it.
ln -sf /run/resolv.conf /etc/resolv.conf
