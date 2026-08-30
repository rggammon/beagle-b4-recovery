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
- Cross-building this U-Boot 2019.04 with GCC 14 needs `HOSTCFLAGS += -fcommon` (bundled
  `dtc` hits `multiple definition of yylloc`). Do **not** pass it via `HOST_EXTRACFLAGS`
  (that clobbers the tools' include paths).
