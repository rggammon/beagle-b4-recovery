# OMAP3 SGX103 DDK Recovery History

## Purpose

This document preserves the provenance, comparison results, discarded paths,
and archive research behind the active
[DDK 1.6 presentation plan](ddk16-dcnohw-presentation-plan.md). It is reference
material, not the implementation checklist.

## Hardware and Kernel

The validation target is an original BeagleBoard Rev B4 with:

- OMAP3530 ES2.1 and SGX530 revision 1.0.3 (`SGX_CORE_REV=103`, raw revision
  `0x10003`).
- 128 MB physical RAM.
- Linux `7.2.0-g2342ce92fdde-dirty`.
- Module vermagic
  `7.2.0-g2342ce92fdde-dirty SMP mod_unload modversions ARMv6 p2v8`.

The maintained kernel tree is
`/mnt/scratch/geoduck-tmp/beagle/openpvrsgx-src` on `geoduck-tools-ryan`.
The board is reachable as `root@192.168.50.245` using
`~/.ssh/geoduck_truenas`.

## DDK Comparison

The same workload was used for each stack: one GLES2 context, two alternating
1024x600 RGBA4 FBO renderbuffers, `glClear`, and `glFinish` per frame. Medians
exclude the first eight warm-up frames.

| Stack              | GPU firmware target | Target 0 median | Target 1 median | Frames over 500 ms |
| ------------------ | ------------------- | --------------: | --------------: | -----------------: |
| DDK `1.4.14.2616`  | SGX103              |        0.205 ms |        0.205 ms |           0 of 112 |
| DDK `1.6.16.3977`  | SGX103              |        0.204 ms |        0.204 ms |           0 of 112 |
| DDK `1.17.4948957` | SGX121              |         1.46 ms |       808.27 ms |          56 of 112 |

DDK 1.17 also stalled in a same-target control and triggered 13 SGX watchdog
recoveries. The behavior does not require KMS scanout, PRIME fencing, or two
physical display buffers. The unmatched SGX121 firmware/userspace combination
is unsuitable for the B4's SGX103 core.

DDK 1.4 and DDK 1.6 have both passed the Stage 0 rendering baseline. DDK 1.6 is
the active implementation target because it is the newest recovered TI release
with a complete SGX103 payload. DDK 1.4 remains the rollback baseline.

## DDK 1.4 Baseline

DDK `1.4.14.2616` uses the exact SGX103 soft-float userspace recovered from
OpenPandora SuperZaxxon 1.76. Its Linux 7.2 Services port initializes, registers
IRQ 21 as `SGX ISR`, and renders without completion stalls.

Maintained source:

- Worktree: `/mnt/scratch/geoduck-tmp/beagle/openpvrsgx-ddk14`
- Branch: `users/rgammon/pvrsgx-1.4.14.2616`
- DDK directory: `drivers/gpu/drm/pvrsgx/1.4.14.2616`

Important compatibility changes included modern memory, timer, module, and
procfs APIs; removal of obsolete `clk_set_parent(sgx_fck, core_ck)`; and
`FOP_UNSIGNED_OFFSET` for legacy mmap handles.

The headless DisplayClass provider used with DDK 1.4 was adapted from the DDK
1.8 `dc_nohw` source. Earlier notes incorrectly attributed it to DDK 1.5.
Recovered DDK 1.5 contains Intel EMGD display-class code, and DDK 1.7 contains
an MRST framebuffer driver; `dc_nohw` first appears in the available DDK 1.8
tree.

## DDK 1.6 Recovery

DDK `1.6.16.3977` was recovered from TI Graphics SDK `4.03.00.02`:

- SDK: `/home/ryan/Graphics_SDK_4_03_00_02`
- Kernel source: `GFX_Linux_KM`
- SGX103 release runtime: `gfx_rel_es2.x`
- SGX103 debug runtime: `gfx_dbg_es2.x`

The release binaries report:

- Version `1.6.16.3977`
- Build directory `omap3430_linux`
- SGX530 with `SGX_CORE_REV=103`
- ARM softfp ABI
- Build date 2011-03-07

The userspace being tested came directly from the TI SDK, not from an Angstrom
IPK. The release kernel and userspace bridge numbering proved consistent. An
earlier apparent bridge mismatch was caused by temporary diagnostics inserted
between unbraced `if` statements and their bodies, which made error paths
unconditional.

Maintained source:

- Worktree: `/mnt/scratch/geoduck-tmp/beagle/openpvrsgx-ddk16`
- Branch: `users/rgammon/pvrsgx-1.6.16.3977`
- DDK directory: `drivers/gpu/drm/pvrsgx/1.6.16.3977`

The first two commits preserve source provenance:

1. `2d28cad7f` imports TI's pristine `GFX_Linux_KM` source.
2. `4b8d6bf07` applies the corrected Angstrom `kernel-30.patch`.

The recovered source and completed baseline are preserved as four reviewable
commits:

1. `2d28cad7f`: pristine TI DDK import.
2. `4b8d6bf07`: corrected Angstrom Linux 3.0 display patch.
3. `91f00a69f`: Linux 7.2 Services port.
4. `9858b321a`: DDK 1.8-derived `dc_nohw` provider and build integration.

## Angstrom Control Image

A bootable 2012 Angstrom image is preserved at:

`/mnt/scratch/geoduck-tmp/beagle/angstrom-beagle-2012.01.11.img.gz`

The expanded image contains:

- Linux `3.0.14+`.
- `omap3-sgx-modules 1.6.16.3977-r114a`.
- An original `pvrsrvkm.ko` built from TI Graphics SDK `4.03.00.02`.
- Cached feed records for `libgles-omap3_4.03.00.02-r18` and its ES2/WSEGL
  subpackages.

The proprietary userspace packages were advertised but not installed in that
image. The original feed hostname no longer resolves, and exact IPKs were not
found in the queried Internet Archive captures. Booting the image with its
period kernel/module and the TI SDK ES2 userspace remains an optional historical
control, not a prerequisite for the active plan.

## Newest SGX103 Release

TI Graphics SDK `4.05.00.03` contains DDK `1.6.16.4117`, but its payload has
only ES3, ES5, ES6, and ES8 variants. It has no ES2 payload. DDK
`1.6.16.3977` is therefore the newest known TI DDK release with complete SGX103
userspace and firmware.

No matching DDK 1.6 Pandora distribution was found.

## Other OMAP3 Sources Surveyed

A product name or OMAP part number is not sufficient evidence of SGX103. A
candidate must provide a live core revision, matching build metadata, or a
verified ES2 payload.

- BeagleBoard Rev A-B4: SGX103 confirmed on hardware.
- Pandora Classic: SGX103 confirmed; DDK 1.4 runtime recovered.
- Nokia N900/N9xx: available packaging initializes SGX121, not SGX103.
- TI OMAP3430 Zoom/OMAP3 EVM: plausible but unconfirmed.
- Always Innovating Touch Book: plausible but unconfirmed.
- IGEPv2 and Gumstix Overo: product families span OMAP3530 and later parts;
  available artifacts do not prove SGX103.
- Palm Pre and other OMAP3430 handsets: unconfirmed.

## Discarded Approaches

- Do not run DDK 1.17 SGX121 firmware as the final renderer on SGX103.
- Do not port `omaplfb`; it depends on obsolete fbdev, OMAP DSS, and VRFB APIs.
- Do not infer proprietary EGL object layouts or decode SGX command buffers.
- Do not treat `bufferclass_ti` as a replacement for a DisplayClass provider.
- Do not use the manually constructed PVR2D fill benchmark for performance;
  that experiment caused BIF/watchdog recovery.
- A full-frame CPU copy remains a diagnostic fallback, not the primary
  presentation design.

## Related Documentation

- [DDK 1.6 presentation plan](ddk16-dcnohw-presentation-plan.md)
- [DDK 1.6 Linux 7.2 port notes](ddk16-linux72-port-notes.md)
- [BTT HDMI7 and OMAP DRM notes](btt-hdmi7-omapdrm.md)
- [General recovery lessons](lessons-learned.md)
