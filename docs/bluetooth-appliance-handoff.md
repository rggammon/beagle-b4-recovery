# Handoff: Bluetooth appliance SD image (warm-start for a new chat)

Goal: build a bootable **SD-card appliance** for the BeagleBoard B4 that runs a
Bluetooth service, reusing this recovery repo's SD path as the base image. Start a
fresh chat pointed at `c:\src\beagle-b4-recovery` and paste the relevant bits below.

## What already exists to reuse (this repo)

The recovery repo already produces a working **SD appliance** alongside the NAND flasher.
The appliance target is the base — the kernel, U-Boot, and DTB are identical hardware, so
only the rootfs package set + overlay + a service need to change.

- `flash/build-sdcard.sh` — assembles the ext4-root SD appliance image.
- `flash/uEnv-sdcard.txt` — SD boot env (ext4 root, not the NAND/UBI path).
- `rootfs/build.sh` + `rootfs/packages.txt` — Alpine armv7 ("trilobite") rootfs builder.
- `rootfs/overlay/` — files layered onto the rootfs (e.g. `etc/udhcpc/udhcpc.conf`
  with `RESOLV_CONF="/run/resolv.conf"`).
- `rootfs/post-install.sh` — post-unpack fixups (e.g. `ln -sf /run/resolv.conf /etc/resolv.conf`).
- `kernel/` — builds `zImage` **and** `omap3-beagle-ab4.dtb` (the B4 32 kHz timer fix).
- `uboot/` — U-Boot 2024.07 (`omap3_beagle`), 3 patches incl. `0003-spl-nand-page-size.patch`
  (SPL NAND fix; only needed for NAND self-boot, not SD boot — but harmless to keep).

### Two ways to structure the appliance

- **(a) In-repo variant** — add a `bluetooth`/appliance package set + overlay and a second
  sdcard target. Least duplication; kernel/U-Boot/DTB shared. Recommended.
- **(b) Sibling repo** — new repo that consumes the recovery SD image as an artifact base.
  Cleaner if the appliance diverges a lot; more plumbing.

## Base-image / build facts the new chat needs

- **Build host:** `geoduck-tools` LXC (Debian Trixie). Workspace on host:
  `/mnt/scratch/geoduck-tmp/beagle/` = SMB `\\geoduck\scratch\geoduck-tmp\beagle\`.
  - SSH aliases: `geoduck-tools` (root) and `geoduck-tools-ryan` (ryan@…:2222).
  - github.com from the container is normally reachable but was **intermittent/throttled**
    during the recovery work — if a clone/fetch fails, retry or mirror; Alpine + kernel.org
    mirrors are reliable. (Don't assume github is hard-blocked.)
  - **NEVER use `/tmp`** on geoduck-tools (ramfs, fills fast) — use `/mnt/scratch/geoduck-tmp/`.
- **CI:** GitHub Actions `build.yml`, **manual dispatch only** (`workflow_dispatch`; version
  tags `v*` also build+release). Trigger with `gh workflow run build --ref main`. Produces
  two artifacts: `beagle-nand-flasher` and `beagle-sdcard`.
- **CRLF→LF:** scripts are edited on Windows (CRLF) but must be LF on Linux. Commit with
  `git -c core.autocrlf=false`; the README-warning is benign. When deploying a script to the
  container directly, convert CRLF→LF first (or `dos2unix`).

## Hardware / boot facts (carry over verbatim)

- BeagleBoard OMAP3530-GP ES2.1, **Rev B4**, 128 MB RAM, 256 MB NAND (Micron MT29F2G16ABD,
  HAM1 ECC), no RTC battery.
- Console: UART3 = `ttyS2` (primary). SD boot: hold **USER1** at power-on (ROM MMC-first).
- SD appliance root is **ext4** (`uEnv-sdcard.txt`); the NAND recovery path uses UBI
  (`ubi.mtd=4`), which the appliance does not need.
- The DTB in use is mainline `omap3-beagle-ab4.dtb` (B4 timer fix). Clockevent should read
  `13000000 Hz` — that's the signal the ab4 DTB is live.

## First decisions to nail in the new chat

1. **Which BT stack + profile** the appliance runs (BlueZ + which daemon/agent; A2DP sink?
   HID? serial/SPP? GATT peripheral?). This scopes `packages.txt` and the service.
2. **Bluetooth adapter**: onboard? none on the B4 — likely a **USB BT dongle** on the MUSB
   OTG port (already configured **host mode** + `USB_STORAGE`; may need `USB_BLUETOOTH`/btusb
   in the kernel config and the `bluez` firmware/packages).
3. **Auto-start**: an OpenRC service in `rootfs/overlay/etc/init.d/` + `rc-update` in
   `post-install.sh`.
4. **Repo boundary**: variant-in-repo (a) vs sibling repo (b) — see above.

## Status at handoff

- Final recovery build running/last-good: `main` @ `453b69f` (hybrid
  `nand_page_size()` = `writesize ?: CONFIG_SYS_NAND_PAGE_SIZE`). NAND self-boot verified on
  hardware; SD appliance boots to `trilobite login:`. resolv.conf fix is in the overlay
  (fresh `beagle-sdcard` artifact includes it).
