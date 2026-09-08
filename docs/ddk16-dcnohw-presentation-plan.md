# OMAP3 SGX103 DDK 1.6 Presentation Plan

## Goal

Present a rotating GLES2 cube and then an OpenGL game such as OpenQuartz on
OMAP3 SGX530 revision 1.0.3 hardware using TI DDK `1.6.16.3977` on Linux 7.2.

The presentation boundary is the open-source `dc_nohw` DisplayClass provider:

```text
DDK 1.6 EGL window surface
  -> dc_nohw swapchain buffer
  -> DMA-BUF export
  -> renderer/presenter protocol
  -> omapdrm GEM import
  -> KMS page flip
```

DDK 1.6 and `dc_nohw` own the render buffers. A separate open presenter owns
KMS state and scanout. The implementation must not port `omaplfb`, inspect
proprietary EGL objects, or decode SGX command streams.

Historical recovery, DDK comparisons, and rejected alternatives are in
[SGX103 DDK recovery history](sgx103-ddk-recovery-history.md). Linux 7.2 API
changes and the DDK build-option audit are in
[DDK 1.6 Linux 7.2 port notes](ddk16-linux72-port-notes.md). The existing B4
KMS/display investigation is in [BTT HDMI7 and OMAP DRM notes](btt-hdmi7-omapdrm.md).

## Current Status

**Active stage:** Stage 3 (KMS import + CPU-pattern scanout). The display is
ready — `omapdrm` KMS, `DVI-D-1` connected at 1024x600 (exact `dc_nohw`
geometry), `/dev/fb0` is omapdrm's own `drmfb`. Stage 2 (DMA-BUF export) is
**core validated** (export + `mmap` readback shows the exact rendered pixels,
leak-free, working unload guard); Stage 1 window surface remains functionally
passing.

**Original Stage 0 baseline:** passed for DDK 1.6 and, independently, DDK 1.4.

| Stack                      | Target 0 median | Target 1 median | Frames over 500 ms |
| -------------------------- | --------------: | --------------: | -----------------: |
| DDK `1.4.14.2616`, SGX103  |        0.205 ms |        0.205 ms |           0 of 112 |
| DDK `1.6.16.3977`, SGX103  |        0.204 ms |        0.204 ms |           0 of 112 |
| DDK `1.17.4948957`, SGX121 |         1.46 ms |       808.27 ms |          56 of 112 |

DDK 1.6 is the primary implementation target because it is the newest known TI
release with a complete SGX103 payload and matches DDK 1.4 performance. DDK 1.4
remains the rollback baseline. Neither stack has yet passed Stage 1: the pbuffer
benchmark required `dc_nohw` registration but did not create an EGL window
surface or exercise a DisplayClass swapchain.

**Resolved since the original baseline (see
[sgx-ddk16-stall-followups.md](sgx-ddk16-stall-followups.md)):**

- **Module-unload deadlock / stall (patches 0008 + 0009).** The running kernel
  `7.2.0-ge2b9d0eea907-dirty` carries the SGX-IRQ fix (request INTC virq 37, the
  DT-remapped view of hwirq 21) and the APM/power-lock fix that let `ISR_ID`
  workqueue callers take the nonblocking Services resource lock without first
  waiting on the custom OMAP power mutex. Fresh-boot lifecycle cycles and the
  2000-frame render soak now load, run, and unload both modules cleanly.
- **Item #7, the render-to-texture "leak" — no leak.** FBO/texture attachment
  churn is balanced on every stack (DDK 1.4 Pandora, DDK 1.6 Ångström, DDK 1.6
  Devuan), strace-confirmed `ALLOC_DEVICEMEM` ↔ `FREE_DEVICEMEM` per frame. The
  earlier "OOM at ~40 frames" was a transient CMA-fragmentation state, not a
  driver leak.

**Fresh Stage 0 CMA stress (Devuan, `ge2b9d0eea907`, `cma=48M`).** A 180-second
render-to-texture attachment-churn soak (`texture=1 rebind_attachment=1`) ran
**98,855 frames with zero frames over 500 ms**. `CmaFree` stayed flat and in
fact ended higher than it began (30,716 kB → 34,720 kB); `MemFree` recovered
(50,688 kB → 55,604 kB); no OOM. The **CMA exhaustion did not reproduce.** A
single `HWRecoveryResetSGX` fired during teardown with **all BIF fault registers
zero** (`EUR_CR_BIF_FAULT: 00000000`) — the benign idle/teardown watchdog
artifact, not a memory fault like the historical `0x0F0AB000`.

**Long real-geometry soak (Devuan, `ge2b9d0eea907`).** `sgx_render_flip_test
-nf -f 100000 -ser 1 -tpf 100` ran **all 100,000 serialized frames, `RC=0`**, at
a **28.6 ms/frame mean (native parity;** native Ångström was 27–32 ms). **Zero
`TIMEOUT (retrying)`** (the IRQ stall stays fixed), **zero `HWRecovery` / BIF
fault / OOM** in dmesg, and `MemFree`/`CmaFree` flat across the ~47-minute run.
This clears the earlier "2 recoveries + 2 frames over 500 ms across ~97k frames"
report — the recovery and stall failures are gone on the fixed kernel. (This
harness reports the frame mean, not a per-frame max; the absence of any recovery
or completion timeout is the pass signal.)

**Stage 0 is closed.** Stage 1 is gated only on building an EGL **window**
surface against `dc_nohw` and exercising the DisplayClass swapchain — neither
stack has done that yet (the pbuffer benchmark registered `dc_nohw` but never
created a window surface).

## Handoff Card

### Systems

- Development host: `geoduck-tools-ryan`
- B4 target: `root@192.168.50.245`
- SSH key: `~/.ssh/geoduck_truenas`
- Hardware: BeagleBoard Rev B4, OMAP3530 ES2.1, SGX530 SGX103, 128 MB RAM
- Running kernel: `7.2.0-ge2b9d0eea907-dirty` (carries SGX-IRQ patch 0008 and
  APM patch 0009)
- Required vermagic:
  `7.2.0-ge2b9d0eea907-dirty SMP mod_unload modversions ARMv6 p2v8`

### Maintained Source

- Kernel tree: `/mnt/scratch/geoduck-tmp/beagle/openpvrsgx-src`
- DDK 1.6 worktree: `/mnt/scratch/geoduck-tmp/beagle/openpvrsgx-ddk16`
- Branch: `users/rgammon/pvrsgx-1.6.16.3977`
- DDK source: `drivers/gpu/drm/pvrsgx/1.6.16.3977`
- `dc_nohw`: `services4/3rdparty/dc_nohw`

Reviewable baseline commits:

- `91f00a69f`: Linux 7.2 Services port.
- `9858b321a`: DDK 1.8-derived `dc_nohw` provider and build integration.

The worktree is clean at the Stage 0 checkpoint. Do not edit generated or
staged copies as canonical source.

### Target Runtime

The runtime came directly from TI Graphics SDK `4.03.00.02`; it is not a
Pandora build. The initial development staging name has been corrected:

- Release runtime: `/opt/ti-ddk16/runtime`
- Legacy init libc bundle: `/opt/ti-ddk16/lib`
- Test binaries and modern armel libc: `/opt/pandora-armel`
- Modules: `/opt/ti-ddk16/module/pvrsrvkm.ko` and `dcnohw.ko`

Use only `gfx_rel_es2.x` for acceptance testing. The TI debug userspace has a
different debug build-option mask and is diagnostic-only.

### Integrated Build

```sh
make -C /mnt/scratch/geoduck-tmp/beagle/openpvrsgx-ddk16 \
  ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- \
  LOCALVERSION=-ge2b9d0eea907-dirty \
  M=drivers/gpu/drm/pvrsgx \
  CONFIG_SGX=m CONFIG_SGX_OMAP=m \
  CONFIG_PVRSGX_1_6_16_3977=y \
  CONFIG_PVRSGX_1_6_16_3977_DC_NOHW=m \
  modules
```

## Architecture

### DisplayClass Boundary

The closed DDK 1.6 EGL/WSEGL stack expects a Services DisplayClass device. The
adapted DDK 1.8 `dc_nohw` provider satisfies that contract and owns a bounded
set of known linear buffers. It allocates a system buffer and up to three
swapchain back buffers, associates Services sync data with each buffer, receives
swap commands, and reports command completion.

The useful open boundary is `DC_NOHW_BUFFER`, not an arbitrary EGL texture or
FBO. Every buffer selected for an EGL window surface is therefore identifiable
without knowledge of proprietary EGL layouts.

### Ownership Invariant

A backing allocation must remain valid while any of these references exists:

```text
dc_nohw swapchain buffer
  <- PowerVR Services reference
  <- DMA-BUF exporter reference
  <- presenter GEM/framebuffer reference
  <- active KMS plane reference
```

Shutdown order is the reverse:

1. Disable or replace the active KMS plane.
2. Destroy framebuffer and imported GEM references.
3. Close DMA-BUF FDs and attachments.
4. Destroy the DisplayClass swapchain.
5. Unload `dc_nohw`, then Services.

### Buffer State

The protocol must enforce:

```text
FREE -> RENDERING -> READY -> QUEUED -> SCANNING -> FREE
```

`pfnPVRSRVCmdComplete` means the DisplayClass command has completed and the
buffer may be reused according to the protocol. It must be called exactly once
per accepted command, including controlled shutdown and error paths.

## Stage 0: Stable Rendering Baseline

**Status: PASSED.** The release DDK initializes, `dc_nohw` registers, the
alternating-FBO test completes without stalls or recovery, and the extended CMA
stress soak (98,855 frames, zero over 500 ms, `CmaFree` flat, no OOM) does
**not** reproduce the historical exhaustion. The module-unload stall and the
item #7 "leak" are resolved. The long real-geometry soak
(`sgx_render_flip_test -nf -f 100000 -ser 1`) then ran all **100,000 frames,
`RC=0`, 28.6 ms/frame mean (native parity), zero completion timeouts, zero
HWRecovery/BIF fault/OOM** — clearing the earlier recovery/latency report.

### Preserve Reviewable Checkpoints

- Keep the completed Services and `dc_nohw` baseline commits separate.
- Begin every stage from a clean tree and keep its implementation,
  instrumentation, and tests in reviewable commits with one stated purpose.
- Keep temporary diagnostics out of final commits. If a diagnostic must remain,
  make it opt-in and commit it separately from behavioral changes.
- Do not combine Stage 1 instrumentation with Stage 2 export or later presenter
  work.
- Record module hashes and vermagic with each hardware result.
- Do not include generated `.o`, `.ko`, `.cmd`, `Module.symvers`, or staging
  files in source commits unless repository policy explicitly requires them.
- Before advancing a stage, require a clean diff, successful focused build,
  Stage 0 regression result, and a commit that can be reviewed or reverted
  without taking unrelated work with it.

### Release-Runtime Discipline

- Use only TI `gfx_rel_es2.x` for acceptance tests.
- Keep its `pvrsrvinit`, `libsrv_init.so`, `libsrv_um.so`, EGL/GLES libraries,
  and SGX103 uKernel together.
- Use the isolated legacy libc only for `pvrsrvinit`.
- Use the modern armel loader/libc for locally compiled probes while putting
  DDK 1.6 libraries first in the graphics-library search path.
- Do not treat the debug userspace's build-option mismatch as a release defect.

### Lifecycle and Soak Guard

Before and after each later stage:

1. Perform at least five clean cycles of load Services, run `pvrsrvinit`, load
   `dc_nohw`, run the probe, unload `dc_nohw`, and unload Services.
2. Run one alternating-FBO process continuously for at least three minutes.
3. Record memory before and after the cycles to detect leaked pages or handles.
4. Confirm the `SGX ISR` is registered on Linux **virq 37** (INTC hwirq 21) with a
   rising count while initialized. (Raw IRQ 21 was the pre-`0008` bug: on the DT
   kernel it binds virq 21 = hwirq 5, the wrong line — see the stall follow-ups.)
5. Reject any BIF fault, `HWRecoveryResetSGX`, watchdog recovery, Oops, BUG,
   build-option mismatch, or process hang.

### Stage 1 Test Contract

Create a dedicated window-surface probe rather than overloading the pbuffer
benchmark. The probe must:

- Use only documented EGL and open WSEGL/PVR2D interfaces.
- Create a native window token in the form expected by the open WSEGL sample
  interface; derive it from open source or SDK samples, not proprietary EGL
  memory.
- Request two 1024x600 32-bit window buffers initially.
- Render an unmistakably changing color or simple rotating cube.
- In Phase 1A (`SGX_SWAP=0`) prove swapchain allocation with no swap; in Phase
  1B call `eglSwapBuffers` repeatedly and close cleanly.
- Remain separate from DMA-BUF export and KMS presentation.

### Instrumentation Contract

Stage 1 instrumentation must be opt-in, bounded, and removable. Record:

- Swapchain creation/destruction count and requested buffer count.
- Buffer index plus open CPU/system address identity.
- Associated `PVRSRV_SYNC_DATA` identity.
- Swap command sequence and buffer index.
- Command-completion sequence and status.

Instrument `CreateDCSwapChain`, `GetDCBuffers`, `SwapToDCBuffer`,
`DestroyDCSwapChain`, and the open command-completion call site. Prefer trace
points or debugfs counters. Do not add unbounded per-frame kernel logs, restore
legacy procfs diagnostics, or inspect proprietary objects.

### Game-Representative Coverage

The end goal is a real GLES title (OpenQuartz/GLQuake-class, or a MonoGame 2D
demo). Those apps exercise paths the FBO/pbuffer probes do not, so add these to
the Stage 0 smoke before trusting a game run. Each is cheap and isolates one SGX
subsystem:

- **Depth buffer.** Request a config with a 16- or 24-bit depth buffer and run a
  depth-tested draw. Games rely on the depth attachment and its tiler/ISP path;
  the current clear-only probe never allocates one.
- **Texture upload + sampling.** `glTexImage2D` a non-trivial texture (RGBA and
  a compressed/paletted format if available) and sample it, including mipmaps.
  GLQuake streams lightmaps every frame; MonoGame uploads sprite atlases.
- **Blending / alpha.** Enable `GL_BLEND` and draw overlapping translucent quads
  (2D sprite/HUD path). Confirms the ISP blend path and framebuffer read-back.
- **Shader compile + link (GLES2).** Compile and link a non-trivial
  vertex/fragment pair and check the info logs. The USSE compiler is a distinct
  failure surface from raw draws.
- **Sustained windowed swap cadence.** Once Stage 1 lands, drive `eglSwapBuffers`
  at a fixed target rate for several minutes and record frame pacing and dropped
  frames — the metric a game actually feels.
- **Vertex buffers / index draws.** Draw from a VBO with `glDrawElements` rather
  than immediate-style client arrays, matching how a game submits geometry.

The existing `SGX_WINDOW` path in `tools/sgx-pbuffer-latency.c` (EGL window
surface over fbdev, native handle 0) is the natural seed for the Stage 1
dc_nohw window-surface probe: pointing it at `dc_nohw` drives the DisplayClass
swapchain directly.

### Stage 0 Acceptance

- Release `pvrsrvinit` exits zero.
- `dcnohw.ko` registers successfully.
- Alternating-FBO medians remain comparable between both targets.
- No frame exceeds 500 ms.
- Lifecycle and soak checks show no leak or recovery.
- Clean source checkpoints exist before Stage 1 instrumentation begins.

## Stage 1: Headless EGL Window Surface

**Status: 1A and 1B functionally passing; kernel instrumentation pending.** A
real DDK 1.6 EGL window surface owns the known `dc_nohw` swapchain buffers,
split into two separable phases so a failure in one does not mask the other.
Both phases are driven by the dedicated probe `tools/sgx-window-swap.c` (no FBO,
no pbuffer, no presentation). Creating a window surface already forces WSEGL to
allocate the DisplayClass swapchain, so **allocation** and **swap cycling** are
independent and are proven separately.

**Results (2026-09-07, Devuan `ge2b9d0eea907`, FLIPWSEGL, native handle 0):**

- **1A** — `eglCreateWindowSurface` succeeds on `dc_nohw`; surface reports
  1024x600; renderer `PowerVR SGX 530`, DDK `1.6.16.3977`. Five create/destroy
  cycles, `RC=0`, memory flat, dmesg clean.
- **1B** — `eglSwapBuffers` completes on the no-hardware display class. A 5-cycle
  × 30-second soak ran **18,754 swaps, `RC=0`**, avg 7.5 ms/swap (~133 fps),
  `over_500ms=0`, and **memory pinned flat** (MemFree 36,348 kB / CmaFree
  27,408 kB) across all five create/destroy cycles — no per-swap leak, no fault
  or recovery. A `SGX_TRIANGLE=1 SGX_DEPTH=1` variant (shader compile/link, VBO,
  depth buffer, `glDrawArrays`) also renders and swaps cleanly.
- **Still open:** the _formal_ 1B proofs — `SwapToDCBuffer` monotonic buffer
  rotation and exactly-once command completion — need the kernel-side
  instrumentation below. The functional soak (18k swaps, no hang, flat memory)
  strongly implies both, but the counters make it explicit.

### Phase 1A: Swapchain Allocation (no swap)

Run the probe with `SGX_SWAP=0`: create the window surface, make it current,
render one frame, `glFinish`, destroy, and repeat the create/destroy cycle at
least five times. No `eglSwapBuffers`.

Pass criteria:

- `eglCreateWindowSurface` succeeds against `dc_nohw` (native handle 0,
  FLIPWSEGL) and reports the requested 1024x600 geometry.
- `CreateDCSwapChain` + `GetDCBuffers` expose only the bounded known
  `DC_NOHW_BUFFER` set (two 32-bit buffers).
- Repeated create/destroy cycles leave memory flat with no recovery, fault,
  Oops, BUG, or pbuffer regression.

### Phase 1B: Swap Cycling

Only after 1A passes. Default `SGX_SWAP=1`: drive `eglSwapBuffers` repeatedly so
`SwapToDCBuffer` rotates through the buffer set and each swap command completes.

Pass criteria:

- `SwapToDCBuffer` alternates through the bounded set with monotonic sequences.
- Every accepted swap command is completed exactly once.
- The probe swaps for at least 60 seconds and exits cleanly, repeatedly.
- No memory growth, recovery, fault, Oops, BUG, or pbuffer regression occurs.

### Failure Boundary

Use the bounded Stage 0 instrumentation (`CreateDCSwapChain`, `GetDCBuffers`,
`SwapToDCBuffer`, `DestroyDCSwapChain`, command-completion). Do not begin
DMA-BUF work if Phase 1A requires proprietary EGL inspection or produces
ambiguous buffer identity — that must be resolved first. **Phase 1B may be
deferred or reordered relative to export:** DMA-BUF export (Stage 2) depends on
1A allocation (stable buffers by index), not on 1B swap cycling. If swap
completion on the no-hardware display class proves problematic, move export
ahead and revisit swap cycling under the KMS presenter.

## Stage 2: DMA-BUF Export

**Status: core validated.** Export a known `dc_nohw` swapchain buffer — the same
`DC_NOHW_BUFFER` that Stage 1 allocates and swaps — as a Linux DMA-BUF FD through
a narrow open control interface. This is the bridge out of the closed Services
world toward KMS scanout, and it needs no SGX rendering to validate. It depends
only on Stage 1 Phase 1A (stable buffers by index).

**Results (2026-09-07, openpvrsgx commit `7f52bc9b`, tool
`tools/dc_nohw_export_test.c`):** `/dev/dc_nohw_export` (miscdevice) exposes
`QUERY_ABI` (returned 1024×600, stride 4096, `fourcc 0x34325241` = ARGB8888,
3 buffers, 2,457,600 B) and `EXPORT_BUFFER(index)` → a DMA-BUF FD. After the
Stage 1 probe rendered a solid `0xff3366cc`, the test `mmap`ed the exported FD
and read back **`pixel[0]=0xff3366cc` on buffers 0 and 1** — the FD names the
real render target (bytes `cc6633ff` confirm ARGB8888/BGRA LE). 20 export/close
cycles left `CmaFree` flat (no leak); `rmmod` is refused while an export FD is
held and succeeds once released (the `owner=THIS_MODULE` unload guard enforces
the Ownership Invariant). Buffers live at module scope, so a retained export
survives the renderer exiting — no deferred-free machinery needed.

### What `dc_nohw` actually provides

`dc_nohw` was switched to **contiguous CMA** buffers as Stage 2 groundwork
(openpvrsgx commit `6418c37f`), because the OMAP3 DISPC has no IOMMU/TILER and
can only scan out physically contiguous memory — so zero-copy scanout (Stage 3)
requires it, and it makes the exporter trivial:

- The swapchain owns a system buffer plus up to `DC_NOHW_MAX_BACKBUFFERS` (3)
  back buffers in `DC_NOHW_SWAPCHAIN.asBackBuffers[]`, each a `DC_NOHW_BUFFER`
  with a known `ui32BufferSize` and a single `sSysAddr` (physical address).
- Each buffer is now **physically contiguous, non-cached CMA** memory
  (`dma_alloc_coherent()` against a synthetic `platform_device` with a 32-bit
  mask, pulling from the global `cma=48M`). `DC_NOHW_DISCONTIG_BUFFERS` is
  removed. There is a single DMA address per buffer.
- Pixel format is `PVRSRV_PIXEL_FORMAT_ARGB8888` → `DRM_FORMAT_ARGB8888`;
  width/height/stride are queryable (`GET_BUFFER_DIMENSIONS`), so the exporter
  publishes geometry without guessing.
- Validated on the B4: Stage 1 1A+1B still pass (1,750 swaps, ~133 fps), ~7 MB
  of CMA consumed and fully returned on unload (no leak).

### Initial UAPI

A small `miscdevice` (e.g. `/dev/dc_nohw_export`) with fixed-width ioctls, not an
extension of the proprietary Services bridge:

- `QUERY_ABI` — ABI version and current swapchain geometry (width, height,
  stride, DRM `fourcc`, buffer count, buffer size).
- `ENUM_BUFFERS` — stable session-local buffer indices for the current swapchain.
- `EXPORT_BUFFER(index)` — return a DMA-BUF FD for that buffer.
- `QUERY_BUFFER(index)` — read-only buffer state and sequence counters.

Because the exporter must read `dc_nohw`'s private swapchain/buffer table, it
lives inside (or directly beside) `dc_nohw` and exposes a small in-module
enumeration API rather than reaching in from an unrelated module. Debugfs may
mirror diagnostics but is not the FD-export path.

### Kernel Work

- Add exporter state and per-buffer reference counting keyed by the
  `DC_NOHW_BUFFER`.
- Build the `sg_table` with `dma_get_sgtable()` from each buffer's coherent
  allocation (a single contiguous entry), and implement `mmap` with
  `dma_mmap_coherent()`.
- Implement `attach`, `detach`, `map_dma_buf`, `unmap_dma_buf`,
  `begin/end_cpu_access`, `mmap`, and `release` for the current kernel's
  `dma_buf_ops`. The backing is already **non-cached coherent**, so CPU-access
  cache maintenance is minimal — but the importer's mapping attributes must match
  (write-combine / non-cached) to avoid ARMv7 mismatched-attribute aliasing.
- Publish DRM format, width, height, stride, and allocation size explicitly from
  the queried dimensions.
- Keep the backing allocation alive until Services, every DMA-BUF, attachment,
  imported framebuffer, and scanout reference is gone (the Ownership Invariant).
- Reject module unload while any export remains.

### Pass Criteria

- Every swapchain buffer exports repeatedly by index.
- A test tool `mmap`s an exported FD and reads back the exact pixels the Stage 1
  probe rendered into that buffer (clear colour or triangle) — end-to-end proof
  the FD names the real render target.
- Attach/map/unmap and export/close cycles succeed with memory flat (no leak).
- Closing the renderer does not invalidate an intentionally retained export;
  closing the final export releases its reference exactly once.
- Invalid indices, stale sessions, process death, and partial failures clean up
  deterministically; module unload is refused while exports are open.

### Probe

Add a `dc_nohw_export` smoke to the tooling: open the miscdevice, enumerate
buffers, export each, `mmap`, and checksum the pixels while the Stage 1 probe
drives `eglSwapBuffers`, confirming the mapped contents change per swap. No KMS
yet — this isolates DMA-BUF identity, page mapping, and lifetime.

## Stage 3: KMS Import and CPU-Pattern Scanout

**Status: next.** Put pixels on the B4 display for the first time. A hard-float
presenter imports a DMA-BUF into `omapdrm`, wraps a DRM framebuffer, and drives
KMS scanout — filled by the **CPU** (colour bars), not SGX. This isolates the
display path (import, format, stride, mode set, flip) from the renderer.

### Confirmed display state (B4)

- `omapdrm` KMS is loaded; `/dev/dri/card0` present; `/dev/fb0` is omapdrm's own
  `drmfb` emulation (no competing `omapfb`).
- Connector **`DVI-D-1` is connected at `1024x600`** — the exact `dc_nohw`
  buffer geometry (ARGB8888, stride 4096). A sink is attached, so output is
  observable.
- OMAP3 DISPC scans out physically contiguous memory (no IOMMU/TILER); the
  Stage 2 contiguous-CMA buffers satisfy this directly.
- The board has `libdrm.so.2` but no dev headers, `modetest`, or compiler, so the
  presenter is cross-built hard-float (`arm-linux-gnueabihf`) using **raw DRM
  UAPI ioctls** — no libdrm link dependency.

### Phase 3A: KMS bring-up with a dumb buffer

Prove the display pipeline independently of `dc_nohw`. Use
`DRM_IOCTL_MODE_CREATE_DUMB` + `MAP_DUMB`, CPU-fill colour bars, `ADDFB2`
(ARGB8888), and `MODE_SETCRTC` on the `DVI-D-1` CRTC at 1024x600. Legacy
`SetCrtc` first; atomic KMS can come later.

Pass criteria:

- The presenter becomes DRM master (yielding the `drmfb` console) and sets the
  mode without error.
- Correct colour bars appear on the DVI-D display.
- Clean teardown; no kernel warning or hang.

### Phase 3B: Import and scan out a `dc_nohw` buffer

Open `/dev/dc_nohw_export`, `EXPORT_BUFFER` → DMA-BUF FD, import with
`DRM_IOCTL_PRIME_FD_TO_HANDLE`, `ADDFB2` over the imported handle, CPU-fill
colour bars through the buffer's `mmap`, and `SetCrtc`. This is the first
zero-copy path: the same contiguous CMA pages the SGX renderer will later write
are scanned out by DISPC.

Pass criteria:

- `PRIME_FD_TO_HANDLE` imports the buffer and `ADDFB2` accepts it for scanout
  (the key `omapdrm` import risk to retire).
- CPU-written colour bars in the imported buffer appear on screen.
- Double-buffer flips (`PAGE_FLIP` between two exported buffers) run at the
  display cadence.
- Presenter exit restores the console; no use-after-free, leak, or corruption.

### Scope notes

- **Self-contained:** Stage 3 needs no SGX and no renderer process. The
  `SOCK_SEQPACKET`/`SCM_RIGHTS` FD hand-off is deferred to Stage 4, where the
  soft-float EGL renderer owns the session and the presenter owns scanout.
- **Master coordination:** the presenter takes DRM master; do not run an EGL
  window session on `fb0` at the same time (Stage 4 keeps the renderer offscreen
  in the `dc_nohw` buffers while the presenter owns the display).
- Tool: `tools/dc_nohw_kms_present.c` (hard-float, raw ioctls).

## Later Stages (4–8)

**Gated on Stage 3.** Synchronous and fenced GLES presentation, application
validation, and packaging remain in the satellite so this plan stays focused on
the active stage:

- [Presentation later stages (4–8)](ddk16-presentation-stages-4-8.md) — Stage 4
  synchronous GLES presentation, Stage 5 explicit fences, Stage 6 implicit sync,
  Stage 7 application validation (OpenQuartz/game), Stage 8 packaging.

The architecture, ownership invariant, buffer-state protocol, and engineering rules
below apply throughout those stages.

## Engineering Rules

1. Work in the DDK 1.6 OpenPVRSGX worktree, not generated bundles.
2. Keep SGX103-only configuration and version matching strict.
3. Make each stage a separate reviewable patch series. Checkpoint proven
   behavior before adding the next stage, and never mix temporary diagnostics
   with the permanent implementation.
4. Run the Stage 0 probe after every kernel-side change.
5. Keep instrumentation bounded and removable.
6. Do not port `omaplfb` or revive obsolete procfs diagnostics.
7. Do not inspect proprietary EGL/GLES structures or command streams.
8. Keep soft-float rendering and hard-float presentation separated by documented
   IPC and DMA-BUF interfaces.
9. Treat cache management, synchronization, ownership, and lifetime as separate
   correctness requirements.
10. Reject unrelated refactors while the staged validation path is incomplete.

## Immediate Next Actions

1. Write `tools/dc_nohw_kms_present.c` (hard-float, raw DRM ioctls): enumerate
   resources, find the `DVI-D-1` connector/CRTC, become DRM master.
2. Phase 3A: dumb-buffer colour bars via `CREATE_DUMB` + `MAP_DUMB` + `ADDFB2` +
   `MODE_SETCRTC` at 1024x600 — prove the display pipeline.
3. Phase 3B: `EXPORT_BUFFER` from `/dev/dc_nohw_export`, `PRIME_FD_TO_HANDLE`,
   `ADDFB2` over the imported handle, CPU-fill via `mmap`, `SetCrtc` — retire the
   `omapdrm` import risk.
4. Add `PAGE_FLIP` double-buffering between two exported buffers at the display
   cadence; confirm clean teardown restores the console.
5. Run Stage 3 on the B4; capture whether bars appear and any dmesg warnings.
6. Checkpoint the passing Stage 3 presenter before beginning Stage 4 (connect
   the SGX renderer via the `SCM_RIGHTS` FD hand-off).

## Explicit Non-Goals

- Supporting SGX121 firmware on SGX103 hardware.
- Porting legacy `omaplfb`, fbdev, OMAP DSS, or VRFB internals.
- Reverse engineering proprietary EGL, GLES, firmware, or command buffers.
- Exporting arbitrary proprietary allocations.
- Combining the soft-float renderer with the hard-float presenter in one
  process.

## Acronyms

- **DisplayClass**: PowerVR Services display-provider interface.
- **DMA-BUF**: Linux shared-buffer framework.
- **DMA reservation (`dma_resv`)**: fence container associated with a shared
  buffer.
- **DRM/KMS**: Linux graphics memory sharing and display mode-setting APIs.
- **FBO**: OpenGL framebuffer object.
- **GEM**: DRM graphics memory manager/object model.
- **SGX103**: SGX530 core revision 1.0.3.
- **UMD**: user-mode driver.
- **WSEGL**: PowerVR window-system EGL integration layer.
