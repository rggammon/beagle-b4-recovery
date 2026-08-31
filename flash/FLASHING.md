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

## 2. Boot the flasher

Insert the SD, **hold USER1**, power on. The flasher's U-Boot recognises itself (it sees
`rootfs.ubi` on the card) and drops straight to the `BeagleBoard #` prompt with a banner:

```
== NAND FLASHER SD ==
This SD writes NAND and does not boot a kernel.
At the prompt, run:  run flashall
```

It deliberately does **not** try to boot a kernel, so you won't get cryptic UBI errors
(`unsupported on-flash UBI format`, `Bad magic!`) from an unflashed NAND.

## 3. Flash everything

```
run flashall
```

No `uEnv.txt` import is needed — the macros live in the U-Boot environment, so `flashall`
works straight from the shell. It writes, in order:

```
== MLO ==      MLO-nand        -> NAND 0x0
== U-BOOT ==   u-boot-nand.img -> NAND 0x80000
== ENV ==      (erases the saved env so U-Boot uses its baked-in default)
== ROOTFS ==   rootfs.ubi      -> NAND 0x680000 (mtd4), in two chunks (see below)
====== NAND FLASH COMPLETE ======
```

**This rewrites MLO *and* U-Boot**, not just the rootfs. That matters: an older U-Boot in
NAND uses a different `mtdparts` layout and looks for the rootfs at the wrong offset
(symptom: `unsupported on-flash UBI format` / `failed to attach mtd5`). `flashall` replaces
it, so the U-Boot and rootfs layouts always match.

Each stage must end with `… OK`. If a stage errors, just re-run `run flashall` (it
re-erases). Individual stages are also available: `run flashmlo`, `run flashub`,
`run flashenv`, `run flashrootfs`.

### Why the rootfs is written in two chunks

The rootfs image (~112 MB) is larger than what fits in RAM in one `fatload`: this
UBIFS-capable U-Boot reserves ~32 MB for malloc, leaving only ~95 MB loadable. UBI is
position-independent (it scans every block and rebuilds the volume from each PEB's
headers), so `flashrootfs` splits the image and writes the halves to two NAND offsets with
a 1 MB gap (larger than any bad-block skip, so the halves can't overlap). The split point
is computed from the actual file size (`fatsize` + `setexpr`), so it adapts automatically
if the rootfs grows or shrinks — no hand-editing of chunk sizes.

## 4. Boot it

Power off, remove the SD, power on **without USER1**. The ROM loads MLO/U-Boot from NAND
and shows a 3 s boot menu:

```
SD/MMC (auto; falls back to NAND)   <- default; tries SD, then NAND
NAND recovery (trilobite)
USB
U-Boot shell
```

With no SD present the default falls through to **NAND recovery** and boots to
`trilobite login:` (root / your key). Confirm the clockevent line shows
`13000000 Hz at …@49032000` (the ab4 timer fix) and that `sleep`/timers work.

## Notes

- A `Loading Environment from NAND... *** Warning - bad CRC` message is benign — the NAND
  U-Boot boots from its baked-in default environment (`== ENV ==` erased the saved env).
- `flashall` handles the rootfs 2-chunk split automatically; MLO/U-Boot/Env are single
  writes.
- The two `nand write.trimffs` chunks must each end with `… bytes written: OK`.
