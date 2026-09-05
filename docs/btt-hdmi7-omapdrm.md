# BTT HDMI7 on OMAP3 DRM

This tracks display bring-up for the BIGTREETECH HDMI7 V1.2 (1024x600) on the
original BeagleBoard B4. It is separate from SGX rendering: `pvrsrvkm` renders on
one DRM device, while `omapdrm` owns the display controller and DVI scanout.

## Hardware and DRM path

The display path is:

```text
OMAP3530 DISPC/DPI -> TFP410 DVI transmitter -> HDMI cable -> BTT HDMI7 V1.2
```

Linux exposes two DRM devices in the Devuan GPU image:

- `/dev/dri/card0`: PowerVR SGX (`pvrsrvkm`), render-oriented and without KMS
  resources.
- `/dev/dri/card1`: OMAP DRM (`omapdrm`), with the DVI connector, CRTCs, planes,
  dumb buffers, and PRIME import/export.

The BTT display does not provide usable EDID over this board's DVI DDC path.
Without a forced mode, OMAP DRM has no mode to drive.

## Failed mode

The first Devuan image used:

```text
video=DVI-D-1:1024x600@60e
```

The `e` flag correctly force-enabled the DVI connector, but Linux generated a
non-reduced 1024x600 CVT timing:

```text
pixel clock 48.924 MHz
horizontal: 1024 1064 1168 1312
vertical:   600  601  604  622
```

This has a 104-pixel horizontal sync pulse (`1168 - 1064`). OMAP3 DISPC can
encode at most a 64-pixel sync pulse. DRM therefore pruned the mode as
`MODE_BAD`. Enabling DRM debug also showed all fallback DVI modes rejected;
their horizontal sync pulses were likewise over the OMAP3 limit.

The relevant validation path is:

- `drivers/gpu/drm/omapdrm/omap_crtc.c`: `omap_crtc_mode_valid()`
- `drivers/gpu/drm/omapdrm/dss/dispc.c`: `dispc_mgr_check_timings()` and
  `_dispc_lcd_timings_ok()`

The result was a connected DVI connector with no active CRTC, and `kmscube`
failed with `could not find mode`.

## Current solution

The staged boot argument is:

```text
video=DVI-D-1:1024x600MR@60e
```

`M` explicitly requests VESA CVT calculation and `R` requests reduced blanking.
CVT reduced blanking uses a 32-pixel horizontal sync pulse, within OMAP3's
64-pixel limit. The setting is maintained in
`flash/uEnv-sdcard-devuan.txt` and copied to `uEnv.txt` in the FAT boot
partition.

This mode is hardware-validated. OMAP DRM reports a connected 1024x600
connector and fbcon switches to a 128x37 framebuffer console. Verify with:

```sh
dmesg | grep -iE 'forcing DVI|mode not supported|Cannot find'
cat /sys/class/drm/card1-DVI-D-1/status
cat /sys/class/drm/card1-DVI-D-1/modes
drm_info | sed -n '/DVI-D/,/Encoders/p'
```

Expected results are `connected`, a `1024x600` mode, no `MODE_BAD` message for
that mode, and an active CRTC.

For detailed mode-pruning diagnostics:

```sh
echo 0x1ff > /sys/module/drm/parameters/debug
kmscube -D /dev/dri/card1
dmesg | tail -100
```

## SGX rendering on HDMI

Hardware-accelerated rendering is visible on the panel. The working path uses
OMAP DRM for KMS and an `omapdrm_dri.so` alias for Mesa's Series5 SGX driver:

```sh
kmscube -D /dev/dri/by-path/platform-omapdrm.0-card -v 1024x600
```

Maemo-Leste's Mesa source supports `-Dgallium-sgx-alias=omapdrm`, but its
Daedalus binary package does not enable it. The image builds and installs that
single alias from the checksum-pinned matching Mesa source.

The image reserves `cma=48M`: the proprietary display path requests a
contiguous 4,800-page (18.75 MiB) buffer pool, in addition to OMAP framebuffer
allocations. A 16 MiB CMA pool cannot satisfy it, and 32 MiB was fragile after
other allocations. CMA remains available for movable pages while idle.

Daedalus' packaged `kmscube` incorrectly passes `DRM_FORMAT_MOD_INVALID` to
OMAP DRM. The image builds pinned upstream commit
`f60e50e887d3c49e91ac9b06d8199b36152632fa`, whose framebuffer path correctly
falls back to unmodified `drmModeAddFB2`.

Measured results:

- Surfaceless SGX clear/readback: correct `PowerVR SGX 530` output.
- 1024x600 synchronized offscreen clears: about 142.5 fps.
- Windowed HDMI cube: about 2.3 fps, dominated by the legacy display
  swapchain/synchronization path rather than SGX fill rate.
- Continuous rendering is stable. Mesa/DDK process teardown can trigger SGX
  watchdog recovery; the GPU recovers and remains usable.

Instrumented legacy-KMS timing at 1024x600 averaged:

- Scene draw: about 171 ms.
- `eglSwapBuffers`: about 2.9 ms.
- `gbm_surface_lock_front_buffer`: about 0.2 ms.
- `drmModePageFlip`: about 3.0 ms to queue.
- DRM page-flip event wait: about 446 ms.

A control test using two CPU-filled OMAP dumb framebuffers, with no EGL, GBM,
Mesa, or SGX, completed 120 page flips at 46.67 fps with a 21.43 ms average
event wait and no kernel errors. OMAP scanout and event delivery are therefore
healthy. The long wait appears only for SGX-rendered buffers, pointing to GPU
buffer readiness or implicit-fence signaling in the DDK/Mesa integration.

OMAP DRM rejects `DRM_MODE_PAGE_FLIP_ASYNC` with `EINVAL`. Queuing legacy page
flips without `DRM_MODE_PAGE_FLIP_EVENT` leaves this driver busy indefinitely,
so neither path is a usable synchronization bypass. EGL swap pacing is not the
primary delay because `eglSwapBuffers` itself takes only a few milliseconds.

The SGX completion interrupt path works despite the core-revision exception. On
a clean boot, IRQ 37 (`SGX ISR`) increased from 2 to 520 while a test completed
500 synchronized 64x64 render/readback frames. Pixel readback was correct, the
test exited successfully, and `/proc/pvr/queue` was empty afterward. This is
approximately one hardware completion interrupt per synchronized frame, so the
SGX uKernel, LISR/MISR path, and proprietary sync counters are making prompt
forward progress. It does not rule out a display-buffer-specific defect, but it
makes a generally missed or late uKernel interrupt an unlikely explanation for
the 446 ms page-flip wait.

The 446 ms userspace measurement starts after `drmModePageFlip()` queues a
nonblocking atomic commit and ends when its event is delivered. It therefore
combines two kernel phases: waiting for an implicit framebuffer fence, if one
exists, and the OMAP `DISPC GO`/vblank completion. The CPU dumb-buffer control
measures the latter path at about 21 ms, so most of the additional 425 ms occurs
before ordinary scanout completion.

OpenPVRSGX 1.17 contains an enabled `SUPPORT_DMABUF` adapter that can add a PVR
`dma_fence` to a dma-buf's `dma_resv`. It snapshots the PVR read/write pending
counters and signals the fence when the corresponding complete counters catch
up. OMAP DRM calls `drm_gem_plane_helper_prepare_fb()` and the atomic helper
waits for that fence before programming the plane.

Hardware tracing proves this path is active. Native dma-fence tracepoints showed
OMAP's atomic worker waiting on sequential `driver=pvr timeline=PVR` fences.
After startup, waits alternated between roughly 0.75-0.86 seconds and 0.01-0.03
seconds; this alternating pattern accounts for the approximately 446 ms average
event wait. Each long fence remained unsignalled until a roughly 0.75-second
gap in SGX IRQ 37 ended. The IRQ then queued `do_fence_work` on the `PVR Linux
Fence` workqueue, `dma_fence_signaled` followed within about 2-9 ms, and the
waiter woke immediately. This rules out OMAP scanout, atomic-worker starvation,
and a late fence workqueue as the dominant delay.

Counter instrumentation then resolved the remaining distinction. All display
fences were destinations with pending/complete values of `0/0/N`: neither read
counter participated, and each fence waited for exactly the next write value,
not a value accidentally stamped too far ahead. At signal, `WriteOpsComplete`
equalled that saved `WriteOpsPending` value. The Linux adapter therefore chooses
the right counter threshold and signals promptly when SGX completes it.

Per-frame GBM instrumentation also showed that the delay follows one physical
swapchain buffer exactly. Framebuffer 64 was always slow at roughly 0.75-0.85
seconds, while framebuffer 63 was always about 0.025-0.036 seconds. Replacing the
cube draw with a single alternating clear preserved the same result, ruling out
geometry and shader cost. The same cube workload was also slow offscreen, so the
fence is exposing SGX completion latency rather than creating it. The remaining
root cause is below the fence adapter: one display/share buffer's SGX write or
mapping completes very late. Its allocation, SGX virtual address/PTEs, and kick
parameters should be compared with the fast buffer.

Separate from frame presentation, process teardown repeatedly triggers SGX
watchdog recovery with `EUR_CR_BIF_FAULT` at `0x03397000` (and occasionally
`0x020d7000`) and `No PDE found` for the exiting kmscube context. These recoveries
occur tens of seconds after the measured frame waits, so they do not cause the
per-frame delay, but they confirm a memory-lifetime/MMU defect and can fragment
CMA enough that the next 4,800-page display-pool allocation fails. A manual
`echo 1 > /proc/sys/vm/compact_memory` recovered one such boot.

`tools/trace-sgx-flip.sh` uses kprobe events to split the phases without a
custom kernel. Copy it to the board and run a short, frame-limited invocation:

```sh
chmod +x trace-sgx-flip.sh
./trace-sgx-flip.sh kmscube -D /dev/dri/by-path/platform-omapdrm.0-card \
    -v 1024x600 -c 10 > /tmp/sgx-flip.trace
```

Interpret the monotonic timestamps and fence identity as follows:

- `pvr_process` proves this submission used the PVR dma-buf fence adapter.
- Matching `dma_fence_wait_start`, `dma_fence_signaled`, and
  `dma_fence_wait_end` `(driver, timeline, context, seqno)` values give the
  exact PVR fence wait.
- A prompt matching `fence_signal`, followed much later by `omap_flush`, places
  it in atomic-worker scheduling or a preceding commit dependency.
- Prompt `omap_flush` followed much later by `omap_vblank` places it in OMAP
  `DISPC GO` completion.
- No `pvr_process` means the render target did not use this fence adapter; the
  legacy GEM/DDK synchronization path must then be traced instead.

### OpenPandora comparison

An original OMAP3530 OpenPandora with the same SGX revision `1.0.3` provides a
useful reference. Its Angstrom 2010.4 image uses DDK `1.4.14.2514` and the
legacy `pvrsrvkm` + `omaplfb` + `bufferclass_ti` display-class stack. The
800x480 RGB565 framebuffer has three virtual screens, and EGL selects
`libpvrPVR2D_FRONTWSEGL.so` by default.

Pandora GLMark2-ES 2014.03 measured at fullscreen 800x480:

- Clear with `glFinish`: about 1,415 fps.
- Clear with full readback: about 185 fps.
- Clear with display swap: about 21 fps.
- Horse geometry with `glFinish`: about 47 fps.
- Horse geometry with full readback: about 86 fps.
- Horse geometry with display swap: about 20 fps.

Selecting `libpvrPVR2D_FLIPWSEGL.so` instead of `FRONTWSEGL` left clear/swap
at about 21 fps. The benchmark and `pvr2d_test` both exited without PVR kernel
faults. This confirms that presentation is expensive even in TI's contemporary
stack, but the BeagleBoard's roughly 2.3 fps is not an SGX530 rendering limit;
the modern DRM/Mesa/DDK presentation path adds another order of magnitude.

The Pandora package is archived as
`out/pandora-sgx103-ddk-1.4.14.2514.tar.gz` (MD5
`c17bef042ca98c001438cddbf1d59162`). Its 155-file payload includes the ES2.0
and ES3.0 userspace variants, `pvrsrvinit`, `pvrsrvkm`, `omaplfb`,
`bufferclass_ti`, package metadata, configuration, and the matching kernel
image.

The latest official Pandora full-flash image is SuperZaxxon 1.76, built on
2017-01-07. It is archived locally as `out/SuperZaxxon176.zip` (SHA-256
`b22b93b0f3c209e192638615e2f7b63962f87714ea94a53720593b7ece15920e`).
Its UBIFS root image passed the bundled MD5 check. For original OMAP3530/SGX
revision 1.0.3 hardware, its init script selects the ES2.0 DDK
`1.4.14.2616`, a newer exact-core build than the DDK `1.4.14.2514` recovered
from the installed 2010 image. ES3.0 targets SGX revision 1.2.1 with the same
DDK release, while ES5.0 targets SGX revision 1.2.5 with DDK `1.6.16.3977` for
DM3730 units. SuperZaxxon 1.76 uses kernel 3.2.84 for its normal boot.

SuperZaxxon 1.76 remains soft-float EABI. All three `pvrsrvinit` variants
request `/lib/ld-linux.so.3` and lack `Tag_ABI_VFP_args`; the ES2.0 build
metadata explicitly says `SGX_CORE_REV=103`. The init script has support for
optional libraries under `/usr/lib/arm-linux-gnueabihf`, but the full-flash
image contains no such directory or hard-float SGX binaries. The `HF` suffix
on older Zaxxon firmware names therefore must not be treated as evidence of an
ARM hard-float userspace.

TI later published an explicitly hard-float Graphics SDK, version
`5.01.01.02` (DDK `1.10.2359475`, built 2015-01-29). The original installer is
still available from TI as
`Graphics_SDK_setuplinux_hardfp_5_01_01_02.bin`; its published and locally
verified MD5 is `94bcb31ea7eb50df1dfa4037055b638e`. This is not a hidden
hard-float SGX 1.0.3 build. TI's matching OpenEmbedded recipe states that the
release supports only hard-float calling convention and maps OMAP3 to
`gfx_rel_es3.x`. It defines no `gfx_rel_es2.x` location, unlike SDK 4.00, which
explicitly packaged ES2.x for `SGX_CORE_REV=103`. Thus the surviving TI release
history shows that hard-float support arrived after early OMAP3530 ES2/SGX
1.0.3 support had been dropped.

OpenPandora's own build history confirms that this was a practical community
blocker. Commit `651a05e7779ca23ce2bf9cae769980fd4b2834a0`, dated 2013-12-31,
is titled `oe now defaults to hardfp, set tune to softfp to make old sgx
drivers work`. It explicitly restored the `armv7a-neon` softfp tune after
OpenEmbedded changed its default. Three months earlier, TI's meta-ti commit
`499fba5e6c634c061d377971a73d1e0116676257` added automatic selection between
separate softfp and hardfp SDK 4.09 installers. That release maps OMAP3 only to
`gfx_rel_es3.x` and DDK `1.9.2188537`; it contains no ES2.x target. The public
history therefore has no overlap between an ES2/SGX 1.0.3 payload and a
hard-float OMAP3 userspace.

The revision guard remains meaningful but is not proof that the 1.2.1 uKernel
causes the observed delay. DDK 1.17's SGX530 errata table recognizes revisions
1.2.0, 1.2.1, 1.2.5, and 1.3.0, but has no 1.0.3 case. An older DDK 1.5 SGX530
table does recognize both 1.0.3 and 1.2.1; both select the visible host-side
workarounds `BRN_22934` and `BRN_28889`. The generated uKernel and feature
definitions can still differ by core revision, so the mismatch remains a
credible cause, but no uniquely missing 1.0.3 host workaround has yet been
identified. The trace also shows that IRQ 37 is delivered promptly once the
slow write completes; the unexplained interval is before completion, not Linux
delivering an already-raised interrupt late.

A full softfp operating system is not required for a controlled old-DDK test.
Linux executes softfp and hard-float ARM EABI5 processes with the same syscall
ABI; each process merely needs its matching dynamic loader and libraries. The
SuperZaxxon image provides `/lib/ld-linux.so.3`, glibc 2.9, and the complete
ES2.0 userspace, which can live under an isolated root on a hard-float system.
The TI SDK also provides the matching DDK 1.4 kernel Services source under
`GFX_Linux_KM`. A useful experiment is therefore:

1. Port/build DDK `1.4.14.2616` `pvrsrvkm` for a disposable kernel or use a
   known-compatible older kernel.
2. Run `pvrsrvinit` and a render benchmark from the isolated SuperZaxxon softfp
   root, without replacing the main hard-float root filesystem.
3. Compare repeated allocations and offscreen completion timing. The current
   stack already reproduces the fast/slow alternation offscreen, so this test
   does not initially require legacy `omaplfb` scanout.

The first Linux 7.2 port of DDK `1.4.14.2616` now builds and passes module
post-processing for OMAP3530 with `SGX_CORE_REV=103`. Hardware validation on
the Rev B4 reached successful ES2 `pvrsrvinit` completion and installed IRQ 21
as `SGX ISR`, with no kernel fault. Two additional compatibility changes were
required after compilation: the legacy attempt to reparent `sgx_fck` to
`core_ck` had to be removed because the modern OMAP clock provider already
owns that relationship, and the character-device file operations must set
`FOP_UNSIGNED_OFFSET` so PVR's high-bit mmap handles are not rejected with
`EOVERFLOW`.

The bundled `pvr2d_test` is not an offscreen Services test: without the
intentionally omitted `omaplfb` display-class module it exits after printing
`PowerVR device not found`. Exact firmware and Services initialization are
therefore validated, but command-completion timing still needs either a small
Services/SGX offscreen client or a port of the legacy display-class module.

A controlled offscreen A/B test now compares the two DDKs with the same GLES2
workload: one context, a 1024x600 FBO, two alternating RGBA4 renderbuffers,
`glClear`, and `glFinish` after every frame. After eight warmup frames, exact
DDK `1.4.14.2616` had identical 0.205 ms median completion for both targets;
their worst frames were 7.22 ms and 4.75 ms, with no frame over 500 ms. The
same workload on DDK `1.17.4948957` had a 1.46 ms median for target 0 but an
808.27 ms median for target 1; all 56 measured target-1 frames exceeded 500
ms. A same-renderbuffer control on DDK 1.17 still stalled on exactly half the
frames (56 of 112 over 500 ms), while the DDK 1.4 control had none. DDK 1.17
also triggered 13 hardware recoveries during the two 120-frame controls.

This rules out OMAP scanout, PRIME fences, and a particular physical display
buffer as requirements for the alternating delay. DDK 1.4 and DDK 1.6 both
pass the SGX103 rendering baseline; DDK 1.6 is the active presentation target
because it is the later matched release. No Linux-side compatibility fix found
while porting either stack explains or repairs the DDK 1.17 completion pattern.

The active OMAP3 SGX103 presentation design, which keeps the final display
adapter separable and uses this board's established OMAP DRM path first, is in
[`ddk16-dcnohw-presentation-plan.md`](ddk16-dcnohw-presentation-plan.md).

Hard-float applications still cannot `dlopen` the softfp EGL/GLES libraries.
Using this stack for ordinary hard-float applications would require either an
out-of-process GL command bridge or a new hard-float user-mode driver. The
former is effectively a remote GLES implementation; the latter requires
reimplementing much more than command-buffer serialization, including memory
management, shader compilation, Services bridges, synchronization, and the
SGXMKIF/uKernel protocol. No completed SGX5 community implementation was found;
OpenPVRSGX deliberately preserves and modernizes the GPL kernel half while
continuing to rely on matched proprietary userspace and uKernel binaries.

This stack cannot be combined with the current DDK `1.17.4948957` as-is. The
matching 1.0.3 uKernel is embedded in the old 87,864-byte `pvrsrvinit`, whose
`.data` section is 75,080 bytes and whose build metadata specifies
`SGX_CORE_REV=103`. The current 5,544-byte `pvrsrvinit` instead loads
`libsrv_init.so.1.17.4948957`, which contains the current initialization
payload. The uKernel is therefore userspace-supplied, but it is not standalone
firmware.

There are three independent compatibility boundaries:

- The old modules have vermagic `2.6.27.46-omap1 mod_unload modversions ARMv7`
  and cannot load into Linux 7.2.
- Pandora userspace is soft-float EABI and requests `/lib/ld-linux.so.3`; the
  Devuan image is hard-float EABI and uses `/lib/ld-linux-armhf.so.3`. The
  archive does not contain the old runtime loader or C library.
- DDK 1.17 checks the uKernel's SGXMKIF structure sizes, build options, core
  revision, DDK version, and DDK build. Those shared structures define the
  command CCB, host control, TA/3D commands, render contexts, transfer commands,
  and synchronization protocol. Bypassing the checks would not make DDK 1.4
  firmware compatible with DDK 1.17 kernel Services or GLES command producers.

The old initializer's 15 PVR/SGX imports are all still exported by the current
DDK, so the high-level initialization API retained its shape. That makes a
research port conceivable, but does not establish binary ABI compatibility for
the structures passed through those calls. Likewise, 512 kernel symbol names
are common between the old and current `pvrsrvkm` binaries, while the current
module has hundreds of additional symbols and modern dma-buf/fence integration.

Two experiments are defensible:

1. Boot the archived kernel and complete DDK 1.4 userspace on a separate card.
   This is the lowest-risk way to test the exact 1.0.3 firmware on the Beagle,
   but uses fbdev/`omaplfb`, not the current OMAP DRM/GBM/fence path.
2. Port the complete DDK 1.4 kernel Services and a soft-float userspace runtime
   to a separate modern-kernel image, then add modern dma-buf/fence support.
   This is a substantial driver port, not a firmware swap.

The useful modern-stack target would instead be a DDK `1.17.4948957`
`libsrv_init`/userspace build generated for SGX530 revision 1.0.3. It would keep
the Services, SGXMKIF, Mesa, dma-buf, and fence ABIs aligned while changing only
the hardware-specific uKernel build. No such ti343x binary is present in the
current package; its available build targets revision 1.2.1.

If CVT reduced blanking is rejected or the panel does not lock to it, the next
fallback is a fixed `panel-dpi` timing in the Beagle DT or a captured custom EDID
blob. Do not return to non-reduced CVT; its sync width is known to exceed the
controller limit.
