# MMC/SD: DMA multiblock reads on Beagle B4 (OMAP3530 ES2.1)

## TL;DR

- The SD card sits on the OMAP3 **MMC1** controller (`mmc@4809c000`, Linux host
  `mmc0`, block device `mmcblk0`). In the device tree this is the `&mmc1` node.
- Silicon **erratum 2.1.1.128** (SPRZ278F) breaks multiple-block reads on MMC1/MMC2 —
  but the advisory scopes it explicitly to **polling and interrupt (PIO) mode**, and TI
  lists **"no workaround."**
- The usual Linux mitigation, the `ti,omap3-pre-es3-hsmmc` compatible, sidesteps it by
  **disabling multiblock reads entirely** — every read becomes a single-block `CMD17`
  (~0.5 MB/s). Writes stay multiblock (`CMD25`) and are unaffected.
- We instead use the standard `ti,omap3-hsmmc` compatible and rely on the controller's
  **DMA** data path, which does not use the MPU FIFO-drain sequence the erratum
  describes. **Validated on hardware:** multiblock reads work, ~5.4× faster than
  single-block, byte-for-byte correct, and no CRC/`-EILSEQ` errors — including under a
  concurrent `apt` install hammering the card.

This is implemented in [kernel/patches-devuan/0001-omap3-beagle-board-usb-mmc-nand.patch](../kernel/patches-devuan/0001-omap3-beagle-board-usb-mmc-nand.patch)
(`&mmc1 { compatible = "ti,omap3-hsmmc"; ... }`).

## The erratum (SPRZ278F, Advisory 2.1.1.128)

Full text in [docs/omap-errata-sprz278f.txt](omap-errata-sprz278f.txt) (search
"2.1.1.128"). Revisions affected: **2.1 and earlier** — the Beagle B4 is OMAP3530
**ES2.1**, so it is in scope. Paraphrased:

> The multiple block read transfer, **in polling and interrupt mode**, does not work
> correctly on MMC1 and MMC2. A Data CRC error is generated due to corrupted data when
> the read buffer (two 512-byte portions) is full. If the buffer is not free, the MMC
> controller stops the clock and the card stops sending data; the clock is re-enabled
> only when one portion is emptied. The output clock and data-enable are generated on
> the same internal clock edge, and hold buffers on the data-enable signal make it
> arrive *after* the first clock edge on restart — so the first datum after a
> clock-restart is not sampled. The failure is pattern- and bus-width-dependent
> (e.g. write `0x2800`, read back `0x0801`). MMC3 is not affected (different IP
> integration). **Workaround: none.**

The key phrase is "**No data are read by the MPU into the read buffer**": the corruption
is a property of the **MPU (CPU) draining the FIFO** while the controller stalls and
restarts the read clock.

## Why DMA is immune

The erratum's failure mechanism is a *flow-control* artifact of the PIO read path:

1. In PIO the **CPU** empties the controller's 2×512-byte read FIFO reactively (on the
   buffer-full interrupt, or by polling).
2. There is latency between "FIFO full" and "CPU has drained a portion," so the
   controller frequently **stops the read clock** to back-pressure the card.
3. On each clock restart, the data-enable-vs-clock-edge glitch drops the first sampled
   datum → CRC error. Because it happens on *every* stall/restart, PIO multiblock reads
   are effectively unusable on affected silicon.

Under **DMA**, that sequence does not occur:

- The OMAP **sDMA** engine drains the FIFO via hardware DMA request/acknowledge (the SD
  node wires `dmas = <&sdma 61>, <&sdma 62>` for rx/tx), not the MPU. The DMA controller
  services the FIFO fast and deterministically, so the buffer-full → clock-stop →
  restart-glitch condition the advisory describes is not entered the same way.
- The advisory is written entirely around the MPU read path ("No data are read by the
  MPU…"); it makes no claim about the DMA path.

This is a *bet*, not a TI-blessed workaround — but it is (a) consistent with the
advisory's own PIO scoping and (b) empirically validated below. The `omap_hsmmc` driver
uses DMA for data transfers whenever DMA channels are available (they are here — no PIO
fallback in `dmesg`), so ordinary reads take the immune path.

## `pre-es3-hsmmc` quirk vs. our change

| | `ti,omap3-pre-es3-hsmmc` (old) | `ti,omap3-hsmmc` (ours) |
| --- | --- | --- |
| Multiblock **reads** (`CMD18`) | disabled → single-block `CMD17` only | enabled, over DMA |
| Multiblock **writes** (`CMD25`) | multiblock (unaffected by erratum) | multiblock |
| Read throughput (this board) | ~0.5 MB/s | **~2.7–3.0 MB/s** |
| Erratum exposure | avoided by not doing multiblock reads | avoided by using the DMA path |

The pre-es3 quirk trades throughput for guaranteed safety. Our change keeps the safety
(via DMA) and recovers the throughput.

## Validation (on the reflashed CI image)

Kernel `7.2.0-g214a35fbc02e-dirty`, SD card = SanDisk `SD02G` (2 GB), 4-bit bus,
25 MHz, DMA confirmed (sDMA reqs 61/62; `omap-dma-engine` active; no PIO fallback).

- **DT flip took effect:** `mmc@4809c000/compatible = ti,omap3-hsmmc`.
- **Throughput:** raw `O_DIRECT` read `dd if=/dev/mmcblk0 bs=1M count=64 iflag=direct`
  = **2.7 MB/s**; buffered read-ahead ≈ **3.0 MB/s** — vs the old single-block ~0.5 MB/s
  (**~5.4×**).
- **Integrity:** a 24 MiB varied file hashed 3× with `drop_caches` between reads →
  **identical SHA-256** every time. No corruption.
- **No errors:** zero `-EILSEQ`/CRC/timeout in `dmesg`, including while a concurrent
  `apt` install was doing heavy SD I/O.

## Throughput headroom ("MMC should be faster")

3 MB/s is well below the bus ceiling, and read-ahead did not help — so the limit is the
**clock and the card**, not multiblock pipelining:

- **25 MHz clock cap.** The DT sets `max-frequency = <25000000>` (a conservative
  carry-over). 4-bit @ 25 MHz ≈ 12.5 MB/s of raw bus bandwidth; we use ~24% of it.
  SD high-speed mode is 50 MHz, which would double the bus ceiling.
- **Old, slow card.** The `SD02G` is a ~2008 Class 2–4 card whose flash reads at only a
  few MB/s regardless of bus clock; it is the likely bottleneck at 25 MHz.

Follow-ups to go faster (not done here):

1. Raise `max-frequency` 25 → 50 MHz (SD HS). Card- and signal-integrity-dependent; the
   B4's SD routing at 50 MHz is unverified and the `SD02G` may not sustain it — test
   with the same hash/throughput harness before trusting it.
2. Use a modern SD card — the biggest single win at the current 25 MHz clock.

## Risk / caveats

- The DMA-immunity argument is empirical + inference from the advisory scope, not a TI
  guarantee. The erratum is pattern- and width-dependent, so the integrity harness
  (varied data, repeated cache-dropped hashes) is the real assurance — re-run it after
  any card or clock change.
- If a future config loses the DMA channels (driver change, DT edit), `omap_hsmmc` falls
  back to **PIO** and the erratum returns. `dmesg` should never show a PIO/DMA-request
  failure for `4809c000.mmc`; the safe fallback is to restore
  `compatible = "ti,omap3-pre-es3-hsmmc"`.
