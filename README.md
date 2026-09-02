# beagle-b4-recovery

Three reproducible Linux images for the original (Rev A–B4) **BeagleBoard**
(TI OMAP3530): two Alpine recovery images for SD/NAND and a writable Devuan image
with working PowerVR SGX530 hardware acceleration.

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

**Three** downloadable images — two Alpine recovery nets plus a Devuan GPU image:

- **`beagle-sdcard.img.xz`** — a **run-from-SD appliance** (FAT boot + **ext4** root).
  Write it to a card, boot it, done — NAND is never touched.
- **`beagle-nand-flasher.img.xz`** — boot it to **flash the appliance into NAND**
  (`run flashall`), after which the board runs headless from NAND with **no SD card**.
- **`beagle-sdcard-devuan.img.xz`** — a **Devuan + PowerVR SGX530** image (FAT boot +
  **read-write ext4** glibc root) that actually **drives the GPU** (hardware GLES2 via
  the closed Imagination DDK). Built from a separate OpenPVRSGX 7.2 kernel — see the
  [GPU section](#gpu-powervr-sgx530--hardware-accelerated-on-the-devuan-image). This is
  the image to customize with `apt install ...`.

The two Alpine recovery images share these pieces:

- **U-Boot 2024.07** (the last mainline release that still carries `omap3_beagle`;
  musb host mode, `ENV_IS_IN_NAND`, a boot menu) — one binary serves both the SD boot
  and the NAND copies (SPL follows the ROM boot device).
- **Linux 6.6.152** (pristine mainline + 4 small board patches) built with
  **`omap3-beagle-ab4.dtb`** (the timer fix).
- An **Alpine Linux** armv7 root filesystem ("trilobite"), read-only, packaged as
  **UBIFS** for NAND and **ext4** for SD (same tree), with a small entropy seed so boot
  is fast on this hardware.

Boot flow after flashing (3 s menu, headless-safe):
`SD/MMC appliance (auto) → falls back to NAND recovery` · `NAND recovery (trilobite)` ·
`USB` · `U-Boot shell`.

## Which image should I use?

| Image | Runs from | Root filesystem | GPU | Best for |
| ----- | --------- | --------------- | --- | -------- |
| `beagle-sdcard.img.xz` | SD card | Alpine ext4, read-only | software rendering | bootable recovery without touching NAND |
| `beagle-nand-flasher.img.xz` | on-board NAND after flashing from SD | Alpine UBIFS, read-only | software rendering | permanent, headless, SD-free recovery |
| `beagle-sdcard-devuan.img.xz` | SD card | Devuan ext4, read-write | PowerVR SGX530 GLES2 | a customizable system and GPU workloads |

The two Alpine images boot the **same** Linux 6.6 kernel, U-Boot, and `trilobite`
rootfs; only the storage medium and root filesystem differ. The Devuan image uses its
own OpenPVRSGX 7.2 kernel and glibc userspace. Holding **USER1** at power-on forces an
SD boot, providing a recovery path if the NAND installation is damaged.

## Quick start

- **Download** `beagle-sdcard.img.xz`, `beagle-nand-flasher.img.xz`, or
  `beagle-sdcard-devuan.img.xz` from the [Releases](../../releases) page, or
- **Build them yourself** — run `gh workflow run build.yml` for both Alpine recovery
  images or `gh workflow run build-devuan.yml` for the Devuan GPU image. The equivalent
  local stages on a Linux host with an ARM cross-toolchain are:

  The Alpine workflow defaults to `https://mirrors.edge.kernel.org/alpine`; select a
  different `alpine_mirror` when manually dispatching it, or set `ALPINE_MIRROR` for a
  local build.

  ```sh
  ./kernel/build.sh     # download 6.6.152, apply patches+config, build zImage + ab4 dtb + modules
  ./uboot/build.sh      # download 2024.07, apply 3 patches, build MLO/u-boot (one binary, SD + NAND)
  ./rootfs/build.sh     # alpine-make-rootfs + overlay -> rootfs.ubi (NAND) AND rootfs.ext4 (SD, ro)
  ./flash/build-flasher.sh    # assemble beagle-nand-flasher.img.xz  (flash to NAND)
  ./flash/build-sdcard.sh     # assemble beagle-sdcard.img.xz        (run from SD, read-only)

  # GPU image (separate 7.2 kernel + Devuan userspace; see the GPU section):
  ./kernel/build-devuan.sh        # clone OpenPVRSGX 7.2, apply SGX/MMC fixes, build zImage + modules
  sudo -E ./rootfs/build-devuan.sh # mmdebstrap Devuan + SGX DDK -> rootfs-devuan.ext4
  ./flash/build-sdcard-devuan.sh   # assemble beagle-sdcard-devuan.img.xz
  ```

Then write either SD image (`beagle-sdcard.img.xz` or
`beagle-sdcard-devuan.img.xz`) to a card and boot it, or use
`beagle-nand-flasher.img.xz` to **flash the Alpine recovery to NAND** following
[`flash/FLASHING.md`](flash/FLASHING.md). NAND flashing is a two-step write because
this board's U-Boot cannot hold the whole rootfs in RAM at once.

## Repo layout

```
kernel/     patches/ + config + build.sh   (builds zImage, omap3-beagle-ab4.dtb = the fix, AND modules)
            patches-devuan/ + config-devuan + build-devuan.sh   (OpenPVRSGX 7.2 GPU kernel)
uboot/      patches/ (3) + build.sh   (U-Boot 2024.07: defconfig delta + bootmenu env + SPL NAND fix)
rootfs/     packages.txt + overlay/ + configure.sh + build.sh   (Alpine -> ro UBIFS + ro ext4)
            keys-devuan/ + build-devuan.sh   (Devuan + SGX DDK -> rw ext4, via mmdebstrap)
flash/      ubinize.cfg + build-flasher.sh + build-sdcard.sh + build-sdcard-devuan.sh + uEnv*.txt + FLASHING.md
tools/      sgx-render-test.c   (self-contained EGL/GLES2 SGX530 render probe)
docs/       b4-c70-32khz.md, lessons-learned.md, omap-errata-sprz278f.txt,
            elinux-beagleboard-community.html, beagle-c70-capacitor.jpg
.github/workflows/build.yml + build-devuan.yml
```

## Patches

The timer fix itself is **not** a patch — it's building the mainline
`omap3-beagle-ab4.dtb`. The kernel patch sets below contain the additional board and
SGX fixes needed by each image.

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

### Devuan GPU kernel (against OpenPVRSGX Linux 7.2) — `kernel/patches-devuan/`

`kernel/build-devuan.sh` applies these two patches to the OpenPVRSGX `linux+pvrsgx`
branch. The AB4 timer correction is still supplied by building
`omap3-beagle-ab4.dtb`; it is not an additional patch.

- **`0002-omap_hsmmc-pbias-settle-dmae-gate.patch`** — ports the recovery kernel's
  OMAP HSMMC fix to 7.2: wait 20 ms for PBIAS/rail settling and set `DMAE` only for
  commands with a data phase. On this board it improved measured SD throughput from
  approximately 0.63 to 2.5 MB/s read and 0.10 to 8.5 MB/s write.
- **`0005-pvrsgx-corerev-b4-exception.patch`** — adds the exact hardware/software pair
  `(0x10003, 0x10201)` to the DDK's existing core-revision exception table, allowing
  the B4's SGX530 1.0.3 silicon to use the available ti343x 1.2.1 ukernel while leaving
  all other revision mismatches fatal.

### U-Boot (against U-Boot 2024.07) — `uboot/patches/`

U-Boot **2024.07** is the last mainline release that still contains `omap3_beagle`
(removed in the v2024.10 merge window for missing the `CONFIG_DM_I2C` deadline — see
[docs/lessons-learned.md](docs/lessons-learned.md)). By 2024.07 mainline already carries
the HAM1 NAND ECC, `ENV_IS_IN_NAND`, and the NAND geometry, so the port is just **three
small patches** — no `board.c` changes (musb host is config-driven under driver model).

- **`0001-omap3_beagle_defconfig.patch`** — `configs/omap3_beagle_defconfig`:
  `SYS_MMC_MAX_BLK_COUNT=1` (SPL FAT read on this board's marginal MMC — single-block
  reads), `SYS_BOOTM_LEN=0x2000000` (the >8 MiB kernel needs a bigger reservation, else
  the handoff resets at _Starting kernel_), musb **host** (drop `USB_MUSB_GADGET`/gadget
  fastboot, add `USB_MUSB_HOST` + `USB_STORAGE`), `CMD_BOOTMENU`+`MENU`, our
  `MTDPARTS_DEFAULT` (rootfs @ `0x680000`), `BOOTCOMMAND="bootmenu 3"`, and disable
  `EFI_LOADER` (drops the "No EFI system partition" spam).
- **`0002-omap3_beagle-bootmenu-env.patch`** — `include/configs/omap3_beagle.h`: the
  boot menu env. `bootnand` sets the trilobite bootargs **inline** (`console=ttyS2…
ubi.mtd=4… ro rootwait`) rather than via a `nandargs` var (the stock header defines
  `nandargs` too, and it wins — which silently booted with no console). `nandmount` uses
  `ubi part rootfs 2048` (the x16-NAND UBI's VID-header offset). `usb stop` precedes each
  `bootz` for a clean kernel handoff.
- **`0003-spl-nand-page-size.patch`** — `drivers/mtd/nand/raw/nand_spl_simple.c`: makes
  the NAND **self-boot** chain work at all (ROM → SPL → U-Boot from NAND). After the 2023
  unified `spl_load()` rework the SPL aligns the U-Boot read to `nand_page_size()`, which
  returns `mtd->writesize` — a field only populated by `nand_scan()`. The OMAP3 SPL runs
  from 64 KB SRAM and can't fit `nand_scan()` (the very reason it uses the minimal
  `nand_spl_simple` reader), so `writesize` stays 0, `ALIGN(size, 0)` collapses to 0, and
  the SPL loads a zero-length U-Boot (_SPL: failed to boot from all boot devices_). Fall
  back to the statically-known `CONFIG_SYS_NAND_PAGE_SIZE` when `writesize` is 0
  (`writesize ?: CONFIG_SYS_NAND_PAGE_SIZE`) — the page size the reader already uses
  everywhere else. Without it only SD boot works; NAND self-boot is dead. (Latent mainline
  bug: modern SoCs that _can_ fit `nand_scan` in SPL populate `writesize` and never hit it.)

All three patches are `git diff -u` output — apply with `patch -p1` (the build script does this).

## Hardware

- BeagleBoard OMAP3530-GP ES2.1, **Rev Ax/Bx (B4)**, 128 MB RAM, 256 MB NAND
  (Micron MT29F2G16ABD, HAM1 ECC), no RTC battery.
- Console: UART3 = `ttyS2` (primary), UART2 = `ttyS1` (expansion header).
- Recovery net: hold **USER1** at power-on to boot from SD (ROM MMC-first) if NAND is bad.

## GPU (PowerVR SGX530) — hardware-accelerated on the Devuan image

The OMAP3530's **PowerVR SGX530** GPU **is** driven — but only on the separate
**`beagle-sdcard-devuan.img.xz`** image, not on the Alpine recovery images. Series5 SGX
has no open Mesa/Gallium driver; the only way to run it is Imagination's proprietary DDK
(closed GLES/EGL userspace + closed GPU microcode "ukernel"), which is version-locked to
a **glibc** userspace and a specific kernel driver. So the recovery images (musl/Alpine,
mainline 6.6, drivers `=y`) stay **software-rendered** (Mesa `llvmpipe`), and the GPU
lives in its own image:

- **Kernel** (`kernel/build-devuan.sh`): the **OpenPVRSGX** `linux+pvrsgx` tree (Linux
  7.2) with the `pvrsrvkm` SGX530 driver built as a module, plus the same
  `omap3-beagle-ab4.dtb` timer fix. Beyond the DDK core-rev patch below, it carries one
  board fix — `0002` (`omap_hsmmc` DMAE gating + PBIAS settle), without which this
  marginal board's SD is crippled to ~0.6 MB/s. The DDK sources are committed on that
  branch, so a plain clone builds it.
- **Userspace** (`rootfs/build-devuan.sh`): Devuan daedalus (glibc, sysvinit) + the
  maemo-leste **ti343x DDK** (`sgx-ddk-um-ti343x`) and Mesa's `pvr` DRI loader.
  Bootstrapped with a two-phase `mmdebstrap` so only the GLES stack is pulled in, not the
  Hildon desktop.
- **The B4 catch — and the one patch that fixes it:** this early board's SGX530 reports
  silicon **core revision 1.0.3** (`0x10003`), but the only available ti343x ukernel is
  built for **1.2.1** (`0x10201`), so the DDK's `SGXDevInitCompatCheck()` refuses to init
  with `PVRSRV_ERROR_BUILD_MISMATCH`.
  `kernel/patches-devuan/0005-pvrsgx-corerev-b4-exception.patch` adds our `(hw, sw)` pair
  to the DDK's built-in `aui32CoreRevExceptions[]` whitelist, so the check is skipped for
  *exactly* this combination while any other genuine mismatch still fails.
- **Verified on hardware:** a surfaceless GLES2 render reports
  `GL_RENDERER = "PowerVR SGX 530"` with correct pixel read-back (`tools/sgx-render-test.c`
  is a self-contained `dlopen` EGL/GLES2 probe — no headers or network needed). Offscreen
  rendering works today; the on-screen/KMS display path (`omapdrm` + PRIME) is still a
  work in progress.

Related community efforts this builds on / references:

- **OpenPVRSGX** — the _GPL kernel_ driver (`pvrsrvkm`) modernized for mainline `drm`
  (used here): <https://github.com/openpvrsgx-devgroup/linux_openpvrsgx>
- **Mesa PowerVR (`pvr`)** — the _open_ PowerVR driver, but **Rogue/AXE (Series6+) only**,
  not Series5 SGX: <https://docs.mesa3d.org/drivers/powervr.html>
- **`sgx540-reversing`** — clean-room RE of the SGX540 (same Series5 USSE family): USSE
  disassembler, shader dumping, goal of rendering without the closed libGL:
  <https://codeberg.org/Garnet/sgx540-reversing>
- **`gma500-reverse-engineering`** — RE of the Intel GMA500 (SGX545) driver:
  <https://github.com/TCVM/gma500-reverse-engineering>

## References

- BeagleBoard Community wiki, **Issue #22** (32 kHz clock / capacitor **C70**):
  <https://elinux.org/BeagleBoard_Community#Revision_B> — local copy in `docs/`.
- 2008 mailing-list thread, "GPT1 timer issue aka UART3 hang on Beagle Board":
  <https://forum.beagleboard.org/t/gpt1-timer-issue-aka-uart3-hang-issue-on-beagle-board/2962>
- Greg Kroah-Hartman, stable backport of _"ARM: dts: Fix timer regression for
  beagleboard revision c"_ (the mainline timer-clocking fix, on LKML):
  <https://lkml.org/lkml/2022/2/14/855> (5.16) · <https://lkml.org/lkml/2022/2/14/538> (5.10).
- Mainline `omap3-beagle-ab4.dts` (the fix), Linux `arch/arm/boot/dts/ti/omap/`.

## License

Build scripts, patches, and docs in this repo: MIT (see `LICENSE`). Downloaded U-Boot,
Linux, and Alpine sources retain their own licenses.
