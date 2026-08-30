# Lessons learned (early BeagleBoard OMAP3530, Rev B4)

Hard-won notes worth remembering for this board and this class of work.

## The timer bug specifically
- **A dead clockevent looks like "USB/network/disk hangs but never times out."** If
  submitted work never completes *and* never times out, suspect the kernel tick, not
  the peripheral. Confirm with `cat /proc/interrupts` twice at idle: if the
  `clockevent` line is frozen while `date` advances, the tick is dead.
- **Clocksource ≠ clockevent.** The free-running 32 kHz counter (clocksource) kept
  working the whole time, so `date` always looked fine — very misleading.
- The freeze is **load/reprogram-rate dependent**: an 8×`sleep 1` fork storm wedges it
  in seconds–minutes; true idle can survive hours. Don't conclude "fixed" from an idle
  soak — always stress it.
- **`omap3-beagle-ab4.dts` is the answer for early boards.** Check your board revision
  (U-Boot prints "Beagle Rev Ax/Bx"); Rev A–B4 need the ab4 DTB. See `b4-c70-32khz.md`.

## Debugging methodology that worked
- **Debug on a RAM-root image, never the live rw NAND rootfs.** Repeated freeze +
  power-cut with uncommitted UBIFS writes corrupts the NAND master node. A RAM-root
  (initramfs) shell survives any storage/USB wedge and lets you capture the frozen state.
- **Have a second, USB-independent console.** UART2 (`ttyS1`) on the expansion header,
  via a cheap FTDI, survives every USB/DPLL hiccup — the most reliable access.
- **Register captures beat theories.** `devmem2` on the GP timer registers (base
  `0x48318000`: TISR 0x18, TIER 0x1c, TCLR 0x24 [bit0 ST/bit1 AR], TCRR 0x28, TLDR 0x2c,
  TWPS 0x34) is what finally showed "timer running, counter stranded far from overflow,
  no IRQ pending" — i.e. a failed reload, not a lost interrupt.
- **Cross-check against an old kernel.** Booting the 2011 Ångström 3.0.14 image and
  reproducing the freeze proved it was hardware, not a 6.x regression — and its old
  `omap2_gp_timer` used the same 32 kHz GPTIMER1.
- **Search the vendor forums/wiki, early.** The definitive answer (Issue #22 / C70) was
  a 2008 forum thread + the eLinux wiki. A web search would have saved days.

## NAND flashing on this board
- **U-Boot with UBIFS support reserves a big (~32 MB) malloc pool**, so only ~95 MB of
  the 128 MB RAM is loadable via `fatload`. A 111 MB `rootfs.ubi` will **not** load in
  one shot (`** Reading file would overwrite reserved memory **`). Check `bdinfo` (the
  stack `sp` is the load ceiling).
- **Write big UBI images in two chunks with a gap.** UBI is position-independent (it
  scans every block and rebuilds from VID headers), so split the image at a PEB
  boundary and write the halves to two NAND offsets with a ~1 MB gap (bigger than the
  max bad-block skip). See `flash/FLASHING.md`.
- **`flashall` fails silently on the rootfs step** if the load is refused: the `&&`
  chain skips the `nand write` but still prints "COMPLETE." MLO/U-Boot/Env write fine;
  only the rootfs needs the manual 2-chunk write.
- **`nand write.trimffs`** (not plain `nand write`) for UBI images — trims trailing 0xFF
  pages and is bad-block aware.
- A **bad-CRC NAND environment warning is benign** here — the NAND U-Boot has its boot
  command baked into `CONFIG_BOOTCOMMAND`.
- Keep the **USER1 → SD** boot (ROM MMC-first) as the recovery net when reflashing NAND.

## Read-only appliance rootfs (Alpine + OpenRC)
- **Root cause of past corruption was the freeze**, not UBIFS. Still, ship the recovery
  rootfs **read-only** (UBIFS `ro` + tmpfs for `/tmp`, `/var/log`) so a power-yank can
  never corrupt it. To edit config later: `mount -o remount,rw /`, edit, remount `ro`.
- The OpenRC **`root` service honors the `ro` fstab option** (`case … *,ro,*) ;;`), so
  `ro` in fstab + `ro` bootarg keeps it read-only with no service surgery.
- **sshd needs host keys** and can't generate them on a ro root → **pre-generate**
  `/etc/ssh/ssh_host_*_key` at build time. (On an already-flashed board: one-time
  `remount,rw ; ssh-keygen -A ; rc-service sshd restart ; remount,ro`.)
- **udhcpc can't write `/etc/resolv.conf` on a ro root** → DNS-by-name silently fails
  (ping-by-IP still works). Point it at tmpfs: `/etc/udhcpc/udhcpc.conf` with
  `RESOLV_CONF="/run/resolv.conf"` + symlink `/etc/resolv.conf -> /run/resolv.conf`
  (`/run` is a tmpfs OpenRC mounts at boot). Alpine's `default.script` sources that conf.

## USB on this board (MUSB host)
- **The single MUSB port supplies very little current.** A gigabit USB NIC + hubs + a
  flash disk on it will brown out under load: the *whole* USB tree disconnects at once
  (you'll see the top hub `usb 1-1: USB disconnect` take everything below it, then a
  full re-enumerate). Use a **powered hub** — that was the actual fix, not the NIC.
- **Prefer a USB2 100 Mbit ASIX (AX88772, `asix`) over a gigabit RTL8153 (`r8152`).**
  The RTL8153 draws more *and* its driver does firmware resets that MUSB recovers from
  poorly; the ASIX is lower-power and far gentler on MUSB. (`rtl8153a-4.fw ... error -2`
  is a harmless missing PHY patch — `eth0` still comes up.)
- With a **powered** hub, device *count* stops mattering for power; only shared bus
  bandwidth and MUSB's limited endpoints/DMA channels scale with active devices — both
  fine for a NIC + a disk. Keep the hub tree shallow (MUSB dislikes deep TT chains).
- Build the common USB-NIC drivers **built-in (`=y`)**, not modules: `kernel/build.sh`
  doesn't `modules_install`, so `=m` NIC drivers wouldn't reach the rootfs.

## GitHub Actions / repo hygiene
- **Commit shell scripts with the exec bit set**, or the Linux runner fails with
  `./x.sh: Permission denied` (exit 126). Files added from Windows are mode `100644`;
  fix with `git update-index --chmod=+x <script>` (don't rely on the working-tree bit).
- Benign ro/RTC noise to ignore on a recovery box: udhcpc can't write `/etc/resolv.conf`
  (no DNS-by-name; reach it by IP), "Unable to save dependency cache" (regenerated each
  boot), and **clock skew** (no RTC battery → clock starts at 1970 until chrony/NTP).

## Entropy on OMAP3530-GP
- The **GP part has its RNG fused off**, and kernel `jitterentropy` fails to init on
  the Cortex-A8 ("requirements: 9"), so `getrandom()` at boot blocks for ~4 minutes.
- OpenRC's dependency-cache step blocks on the CRNG **before any service starts**, so a
  normal service can't seed early enough. **Seed before OpenRC**: run `haveged` (and a
  blocking `/dev/random` read) as the **first `::sysinit:` line in `/etc/inittab`**
  (`sbin/rng-seed`). CRNG then seeds at ~8 s instead of ~262 s. (Cleaner than an
  `init=` wrapper — keeps standard `init=/sbin/init`.)

## Build environment quirks (host)
- Scripts edited on Windows need CRLF→LF before running on the board/host (a stray `\r`
  gives `$'\r': command not found`).
- **Cross-building U-Boot 2024.07 needs `swig` + `python3-dev`** (pylibfdt); the SPL
  `.lds`/fixdep step occasionally loses a `-j` race on a fresh tree — just re-run `make`
  (`uboot/build.sh` retries once).

## U-Boot 2024.07 on this board
- **Three board-specific gotchas that all masquerade as a boot "hang":** (1)
  `SYS_MMC_MAX_BLK_COUNT=1`, else SPL can't read `u-boot.img` off FAT (*Error reading
  cluster* — marginal MMC, single-block reads only); (2) `ubi part rootfs 2048` — the
  x16-NAND UBI's VID-header offset (else *bad VID header offset 2048, expected 512*);
  (3) `SYS_BOOTM_LEN=0x2000000` — the >8 MiB kernel needs the bigger reservation or the
  board **resets** right at *Starting kernel* (2019's `ti_armv7_common.h` set this;
  2024.07's default 8 MiB is too small).
- **A U-Boot env var can be silently shadowed.** Our `nandargs` (added early in
  `CFG_EXTRA_ENV_SETTINGS`) was overridden by the stock header's `nandargs` (last
  definition wins), so `bootargs` became `console=${console}` *unexpanded* → the kernel
  booted with **no console** and merely *looked* hung. Set critical bootargs **inline**,
  or use a name that can't collide. (`printenv <var>` at the prompt reveals the shadow.)
- **Don't `usb start` before booting the kernel on this board.** Warming USB (EHCI/musb)
  before `bootz` intermittently wedged the handoff — this board's USB is part of the same
  C70/DPLL defect — and the ab4 DTB already handles the DPLL for the kernel. Keep a
  `usb stop` before `bootz` for the USB menu entry's sake.
