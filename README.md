# beagle-b4-recovery

A reproducible **recovery Linux image** for the original (Rev A–B4) **BeagleBoard**
(TI OMAP3530), packaged for the on-board **NAND** so the board always boots to a
usable shell — no SD card required.

Its reason to exist is one specific, nasty hardware quirk of these early boards,
and the fix for it:

> **Early BeagleBoards (Rev A – < B5) have a bad/extra capacitor (C70) that makes
> the 32.768 kHz oscillator unreliable.** Because mainline Linux clocks a GP timer
> (GPTIMER1) from that 32 kHz source as the system **clockevent**, the timer
> intermittently stops re-arming under load — the kernel tick dies, `sleep`/timers
> hang, and the console appears to "lock up after a while." It is **not** an OMAP
> silicon erratum (it's a board defect), which is why it's absent from SPRZ278F and
> only ever seen on BeagleBoards.

**The fix** is to stop using the 32 kHz timer for the clockevent. Mainline already
ships a device tree for exactly these boards — **`omap3-beagle-ab4.dts`** — which
moves the clockevent to **GPTIMER2 clocked from `sys_ck` (13 MHz)** and the
clocksource to GPTIMER12. This project simply builds and ships that DTB (plus a few
unrelated board fixes for USB/MMC/NAND), so the timer never wedges.

See [`docs/b4-c70-32khz.md`](docs/b4-c70-32khz.md) for the full root-cause story and
[`docs/lessons-learned.md`](docs/lessons-learned.md) for the debugging notes.

## What it builds

A single self-contained **NAND flasher SD image** (`beagle-nand-flasher.img.xz`) that
carries:

- **U-Boot 2019.04** (musb host mode, `ENV_IS_IN_NAND`, a boot menu) — both an SD copy
  and the NAND copies (MLO + u-boot).
- **Linux 6.6.152** (pristine mainline + 4 small board patches) built with
  **`omap3-beagle-ab4.dtb`** (the timer fix).
- An **Alpine Linux** armv7 root filesystem ("trilobite") as a **read-only UBIFS**
  image, with a small entropy seed so boot is fast on this hardware.

Boot flow after flashing (3 s menu, headless-safe):
`SD/MMC appliance (auto) → falls back to NAND recovery` · `NAND recovery (trilobite)` ·
`USB` · `U-Boot shell`.

## Quick start

- **Download** a prebuilt `beagle-nand-flasher.img.xz` from the
  [Releases](../../releases) page, or
- **Build it yourself** — trigger the GitHub Actions workflow manually
  (Actions → **build** → *Run workflow*, or `gh workflow run build.yml`), or run the
  stages locally on a Linux host with an ARM cross-toolchain:

  ```sh
  ./kernel/build.sh     # download 6.6.152, apply patches+config, build zImage + ab4 dtb
  ./uboot/build.sh      # download 2019.04, apply patches+config, build MLO/u-boot (SD + NAND)
  ./rootfs/build.sh     # alpine-make-rootfs + overlay -> read-only UBIFS (rootfs.ubi)
  ./flash/build-flasher.sh   # assemble beagle-nand-flasher.img.xz
  ```

Then **flash it to the board's NAND** following [`flash/FLASHING.md`](flash/FLASHING.md)
(a two-step write, because this board's U-Boot can't hold the whole rootfs in RAM at once).

## Repo layout

```
kernel/     patches/ + config + build.sh   (builds zImage AND omap3-beagle-ab4.dtb = the fix)
uboot/      patches/ + config + set-nand-bootcmd.py + build.sh
rootfs/     packages.txt + overlay/ + configure.sh + build.sh   (Alpine -> ro UBIFS)
flash/      ubinize.cfg + build-flasher.sh + FLASHING.md
docs/       b4-c70-32khz.md, lessons-learned.md, omap-errata-sprz278f.txt,
            elinux-beagleboard-community.html, beagle-c70-capacitor.jpg
.github/workflows/build.yml
```

## Patches

The timer fix itself is **not** a patch — it's building the mainline
`omap3-beagle-ab4.dtb`. The patches below are small, unrelated board fixes needed to
get USB/MMC/NAND working reliably on this specific early board.

### Kernel (against Linux 6.6.152) — `kernel/patches/`

- **`0001-omap3-beagle-board-usb-mmc-nand.patch`** — `omap3-beagle.dts`: disable the
  unused EHCI/`hsusb2` host PHY and `usbhshost` (this board only uses the MUSB OTG
  port) and switch the OTG port to **host mode** (`mode = <1>`); give `mmc1` an explicit
  4-bit pinmux, `non-removable`, and a 25 MHz cap for stable SD timings; give the TWL
  PMIC an explicit `fck` clock.
- **`0002-omap_hsmmc-pbias-settle-dmae-gate.patch`** — `omap_hsmmc.c`: add a 20 ms
  PBIAS/rail settle after power-on (marginal board), and only set the DMA-enable command
  bit (`DMAE`) when the command actually has a **data** phase — avoids arming DMA on
  non-data commands.
- **`0003-nand_ids-mt29f2g16abd-onfi-x16.patch`** — `nand_ids.c`: add an explicit
  full-ID entry for this board's Micron **MT29F2G16ABD** (256 MiB, 1.8 V, x16) so it's
  detected with the correct 2K page / 128K erase / 16-bit geometry.
- **`0004-phy-twl4030-usb-probe-defer.patch`** — `phy-twl4030-usb.c`: return the real
  regulator error (allowing probe **deferral** instead of a hard fail), and **never
  autosuspend the USB PHY** — hold the probe-time runtime-PM reference. On this marginal
  board the DPLL re-lock after an autosuspend power-down intermittently times out;
  keeping the PHY powered (like the old board-file driver) locks the DPLL once and never
  re-cycles it. This is the counterpart to the USB flakiness the C70 note warns about.

### U-Boot (against U-Boot 2019.04) — `uboot/patches/`

- **`0001-makefile-hostcflags-fcommon.patch`** — add `-fcommon` to `HOSTCFLAGS` so the
  host tools build under GCC 10+ (which defaults to `-fno-common`).
- **`0002-beagle-musb-host-mode.patch`** — `board/ti/beagle/beagle.c`: set
  `musb_hdrc.fifo_mode=5` for the OTG port, scan a second expansion EEPROM at `0x51`
  (loop-through / uLCD boards), and set the SPL boot order **NAND → MMC1 → UART** so a
  NAND-first board still falls back to SD/serial.
- **`0003-nand-env-bootmenu.patch`** — `include/configs/omap3_beagle.h`: switch NAND ECC
  to **HAM1** (uniform ROM → U-Boot → kernel, no ECC switching), put the environment in
  NAND (`ENV_OFFSET 0x260000`), rework the boot env into the menu-driven flow, and cap
  `SYS_MMC_MAX_BLK_COUNT` to work around an MMC multi-block quirk.

All patches are `git diff` output — apply with `patch -p1` (the build scripts do this).

## Hardware

- BeagleBoard OMAP3530-GP ES2.1, **Rev Ax/Bx (B4)**, 128 MB RAM, 256 MB NAND
  (Micron MT29F2G16ABD, HAM1 ECC), no RTC battery.
- Console: UART3 = `ttyS2` (primary), UART2 = `ttyS1` (expansion header).
- Recovery net: hold **USER1** at power-on to boot from SD (ROM MMC-first) if NAND is bad.

## References

- BeagleBoard Community wiki, **Issue #22** (32 kHz clock / capacitor **C70**):
  <https://elinux.org/BeagleBoard_Community#Revision_B> — local copy in `docs/`.
- 2008 mailing-list thread, "GPT1 timer issue aka UART3 hang on Beagle Board":
  <https://forum.beagleboard.org/t/gpt1-timer-issue-aka-uart3-hang-issue-on-beagle-board/2962>
- Greg Kroah-Hartman, stable backport of *"ARM: dts: Fix timer regression for
  beagleboard revision c"* (the mainline timer-clocking fix, on LKML):
  <https://lkml.org/lkml/2022/2/14/855> (5.16) · <https://lkml.org/lkml/2022/2/14/538> (5.10).
- Mainline `omap3-beagle-ab4.dts` (the fix), Linux `arch/arm/boot/dts/ti/omap/`.

## License

Build scripts, patches, and docs in this repo: MIT (see `LICENSE`). Downloaded U-Boot,
Linux, and Alpine sources retain their own licenses.
