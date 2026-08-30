# Flashing the recovery image to NAND

The build produces `beagle-nand-flasher.img.xz` — a self-contained SD card image that
boots its own U-Boot and carries all the NAND payloads (MLO, U-Boot, and the read-only
UBIFS rootfs which contains the kernel + DTB in `/boot`).

Flashing is a **manual, on-hardware** step (it writes the on-board NAND). You need
serial console access (UART3 = `ttyS2`, 115200 8N1) and an SD card.

## 0. Recovery net (read this first)

Holding **USER1** at power-on forces the ROM to boot from SD (MMC-first). That is your
undo button: if a NAND flash goes wrong, hold USER1 and boot the flasher SD again.

## 1. Write the SD

```sh
xz -dc beagle-nand-flasher.img.xz | sudo dd of=/dev/sdX bs=4M conv=fsync   # sdX = your SD reader
```

(On Windows, use balenaEtcher or Rufus with the decompressed `.img`.)

## 2. Boot the flasher and get to the U-Boot prompt

Insert the SD, **hold USER1**, power on. The flasher U-Boot loads its `uEnv.txt` and
drops to the `BeagleBoard #` prompt (it does not auto-flash).

## 3. Flash MLO / U-Boot / Env

```
run flashall
```

This writes `== MLO ==`, `== U-BOOT ==`, `== ENV ==` — and *attempts* `== ROOTFS ==`.
**The ROOTFS step will fail** with `** Reading file would overwrite reserved memory **`
because the 111 MB rootfs can't fit in RAM in one `fatload` (this UBIFS-capable U-Boot
reserves ~32 MB for malloc, leaving only ~95 MB loadable — check `bdinfo`). That is
expected; do the rootfs in two steps next.

## 4. Flash the rootfs in two chunks

UBI is position-independent (it scans every block and rebuilds the volume from each
PEB's headers), so we split the image at a PEB boundary and write the halves to two NAND
offsets with a 1 MB gap (larger than any bad-block skip, so the halves can't overlap):

```
nand erase 0x680000 0xf980000
fatload mmc 0:1 0x80008000 rootfs.ubi 0x3000000 0
nand write.trimffs 0x80008000 0x680000 0x3000000
fatload mmc 0:1 0x80008000 rootfs.ubi 0x3f80000 0x3000000
nand write.trimffs 0x80008000 0x3780000 0x3f80000
```

- Erase all of mtd4 (Filesystem).
- Load the first 48 MB (`0x3000000`) → write to NAND `0x680000`.
- Load the remaining ~63.5 MB (`0x3f80000`, from file offset `0x3000000`) → write to NAND
  `0x3780000` (= `0x680000 + 0x3000000 + 0x100000` gap).

Each `nand write.trimffs` must end with `… bytes written: OK`.

> If your `rootfs.ubi` is a different size, adjust: chunk1 = a PEB-aligned (128 KiB)
> size that fits in ~90 MB; chunk2 size = filesize − chunk1; chunk2 NAND offset =
> `0x680000 + chunk1 + 0x100000`.

## 5. Boot it

Power off, remove the SD, power on **without USER1**. The ROM loads MLO/U-Boot from
NAND, U-Boot attaches `ubi0`, reads `/boot/nand-boot.txt`, and shows a 3 s menu:

```
SD/MMC appliance (auto; falls back to NAND)   <- default; tries SD, then NAND
NAND recovery (trilobite)
USB
U-Boot shell
```

With no SD present it falls through to **NAND recovery** and boots to `trilobite login:`
(root / your key). Confirm the clockevent line shows `13000000 Hz at …@49032000` (the
ab4 timer fix) and that `sleep`/timers work.

## Notes

- A `Loading Environment from NAND... *** Warning - bad CRC` message is benign — the NAND
  U-Boot boots from its baked-in `CONFIG_BOOTCOMMAND`.
- Only mtd4 (rootfs) needs the 2-chunk dance; MLO/U-Boot/Env fit in one `fatload`.
- To change the boot default later, on the running board: `mount -o remount,rw /`, edit
  `/boot/nand-boot.txt`, `mount -o remount,ro /`.
