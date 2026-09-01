#!/bin/sh
# Runs INSIDE the target rootfs (alpine-make-rootfs --script-chroot) for the minimal
# WRITABLE appliance base. Unlike the read-only recovery post-install, the root fs is rw,
# so there are no /run tmpfs tricks -- udhcpc writes /etc/resolv.conf directly and apk
# works normally. Enables the base services (incl. udev for module autoload and the
# first-boot growroot), pre-generates ssh host keys, and locks sshd to key-only root.
set -eu

# entropy seed runs from inittab (::sysinit:/sbin/rng-seed) before OpenRC; haveged also
# runs as a service for ongoing entropy (GP RNG fused off on this OMAP3530-GP).
rc-update add haveged   boot     2>/dev/null || true

# udev: cold-plug + autoload modules by modalias (so a hot-plugged USB BT dongle pulls
# btusb without an explicit /etc/modules entry).
rc-update add udev         sysinit 2>/dev/null || true
rc-update add udev-trigger sysinit 2>/dev/null || true
rc-update add udev-settle  sysinit 2>/dev/null || true

rc-update add networking default 2>/dev/null || true
rc-update add sshd       default 2>/dev/null || true
rc-update add chronyd    default 2>/dev/null || true
rc-update add growroot   default 2>/dev/null || true   # first-boot resize2fs, self-disables

# Pre-generate host keys at build time (deterministic first boot; harmless on rw root).
ssh-keygen -A

# sshd: key-only root login.
sed -i 's/^#\?PermitRootLogin.*/PermitRootLogin prohibit-password/' /etc/ssh/sshd_config
sed -i 's/^#\?PasswordAuthentication.*/PasswordAuthentication no/'  /etc/ssh/sshd_config

# root: passwordless on the (trusted, physical) serial console; ssh stays key-only.
# `passwd -u` alone leaves a locked `!` field on a never-set account, which `login`
# still rejects -- so blank the shadow field outright.
passwd -u root 2>/dev/null || true
sed -i 's|^root:[^:]*:|root::|' /etc/shadow
