#!/bin/sh
# Runs INSIDE the target rootfs (alpine-make-rootfs --script-chroot).
# Enables services, pre-generates ssh host keys (so sshd starts on a read-only root),
# and locks down sshd to key-only root.
set -eu

# entropy: rngd (rng-tools) is started early from inittab (::sysinit:/sbin/rng-seed),
# feeding the kernel pool from the OMAP hardware TRNG (/dev/hwrng, driver built in =y),
# then keeps running -- no OpenRC service needed (replaces the old haveged workaround).
rc-update add networking default  2>/dev/null || true
rc-update add sshd default        2>/dev/null || true
rc-update add bluetooth default   2>/dev/null || true
rc-update add chronyd default     2>/dev/null || true

# Pre-generate host keys at build time -- a read-only root can't create them at boot.
ssh-keygen -A

# sshd: key-only root login.
sed -i 's/^#\?PermitRootLogin.*/PermitRootLogin prohibit-password/' /etc/ssh/sshd_config
sed -i 's/^#\?PasswordAuthentication.*/PasswordAuthentication no/'  /etc/ssh/sshd_config

# bash is installed; make it root's default login shell (nicer interactive recovery shell).
sed -i '/^root:/ s#:/bin/[^:]*$#:/bin/bash#' /etc/passwd

# root: passwordless on the (trusted, physical) serial console; ssh stays key-only
# (PasswordAuthentication no above). `passwd -u` alone leaves a locked/`!` field on a
# never-set account, which `login` still rejects -- so blank the shadow field outright.
passwd -u root 2>/dev/null || true
sed -i 's|^root:[^:]*:|root::|' /etc/shadow

# Read-only root: DHCP DNS lands on tmpfs (see /etc/udhcpc/udhcpc.conf RESOLV_CONF).
# /run is a tmpfs OpenRC mounts at boot, so this resolves once udhcpc writes it.
ln -sf /run/resolv.conf /etc/resolv.conf
