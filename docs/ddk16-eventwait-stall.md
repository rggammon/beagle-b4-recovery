# DDK 1.6 per-frame event-wait stall (NanoVG bring-up findings)

Root-cause record for the ~180–200 ms/frame stall hit when driving the SGX530 with
**dynamic per-frame vertex data** (NanoVG) on the DDK 1.6.16.3977 soft-float stack.
Also captures the separate `discard`/AA HW-recovery hang, the `dc_nohw` surface leak,
and the MMC DMA-multiblock-read experiment. Source line references are into the
OpenPVRSGX fork (`rggammon/linux_openpvrsgx`, branch
`users/rgammon/pvrsgx-1.6.16.3977`) DDK 1.6 tree unless noted.

## Status of NanoVG on this stack

- NanoVG (memononen) builds **soft-float armel** on-board against `/opt/sgx-ddk16`
  (`arm-linux-gnueabi-gcc`), links `libEGL`/`libGLESv2`, and initializes: EGL 1.4,
  vendor "Imagination Technologies", renderer **"PowerVR SGX 530"**, 1024×600.
- The DDK-EGL null-window shim (native window handle 0, `FLIPWSEGL`) → `dc_nohw`
  `present=2` path works — the whole soft-float NanoVG → DDK → `dc_nohw` chain is
  wired correctly.
- Test app + instrumentation: [tools/nanovg-demo.c](../tools/nanovg-demo.c).
  Env toggles: `NVG_FLAGS` (0=no AA, 1=AA, 2=stencil-strokes, 3=both), `NVG_MINIMAL`
  (gradient bg only), `NVG_TIMING` (clear/build/flush/gpu/swap split via `glFinish`),
  `NVG_PROF_RF` (in-`renderFlush` state/buf/setup/draw split), `NVG_CLIENTARR`
  (client vertex arrays), `NVG_SUBDATA` (persistent `glBufferSubData`),
  `NVG_DURATION_SECONDS`.

## Issue A — `discard` → SGX Hardware Recovery (AA path)

**Symptom.** With `NVG_ANTIALIAS` on, the SGX fires repeated
`HWRecoveryResetSGX` (`EUR_CR_EVENT_STATUS 0x20000000`, **`BIF_FAULT=0`** → render
lockup, not MMU). ~29 recoveries in 60 s; the app hangs at ~5 fps.

**Isolation (bisection).**

| Config                   | `discard` active?     | Geometry            | HW recoveries |
| ------------------------ | --------------------- | ------------------- | ------------- |
| `NVG_FLAGS=0` full scene | no (`strokeThr = -1`) | all fills + strokes | **0**         |
| `NVG_FLAGS=1` minimal    | yes (`strokeThr ≥ 0`) | one gradient rect   | **29**        |

**Cause.** NanoVG has **no** `GL_OES_standard_derivatives` / `fwidth`; it computes AA
coverage from CPU-generated fringe geometry. The AA toggle gates exactly one shader
line — `if (strokeAlpha < strokeThr) discard;` (`nanovg_gl.h:633`). A single gradient
fill (its AA fringe fragments hit the `discard`) is enough to wedge the SGX530 USSE.
Mechanism (inference, not a TI erratum): SGX is tile-based deferred; `discard` defeats
HSR/early-Z and, in this DDK build, hangs the pixel pipeline.

**Fix.** Ship NanoVG with AA off (`nvgCreateGLES2(0)` / stencil-only) — stable, edges
aliased. AA alternatives without `discard`: MSAA (`EGL_SAMPLES`) or a supersampled FBO,
or patch the shader to drop `discard` and rely on alpha blending (untested).

## Issue B — ~180–200 ms/frame event-wait stall (the main finding)

### Profiling (NVG_TIMING, NVG_PROF_RF)

| Phase                              | Minimal (1 fill) | Full scene     |
| ---------------------------------- | ---------------- | -------------- |
| clear                              | 3.7 ms           | 0.7 ms         |
| build (NanoVG CPU tessellation)    | 1.4 ms           | 13.3 ms        |
| **flush (`nvgEndFrame` → DDK GL)** | **168–176 ms**   | **198–202 ms** |
| gpu (`glFinish`)                   | 0.2 ms           | 0.2 ms         |
| swap (present)                     | 0.6–0.9 ms       | 1.0 ms         |

The GPU is idle-fast (0.2 ms) and the flip is ~1 ms — **not** the SGX, `dc_nohw`, or the
paced present (mailbox `present=1` measured the same ~5 fps as paced `present=2`). The
cost is entirely CPU-side in `nvgEndFrame`.

### It is not data movement

`renderFlush` sub-split put ~all of it in the single per-frame vertex `glBufferData`.
But the cost is the **same ~200 ms** across every upload strategy, and **fixed**
regardless of vertex count (minimal <1 KB ≈ full ~48 KB):

| Path                                         | cost                      |
| -------------------------------------------- | ------------------------- |
| `glBufferData` (new store, `GL_STREAM_DRAW`) | ~198 ms (in "buf")        |
| client vertex arrays (VBO 0)                 | ~203 ms (moves to "draw") |
| `glBufferSubData` (persistent buffer)        | ~202 ms (in "buf")        |

A **static** VBO (uploaded once) renders at ~100 fps on this exact `dc_nohw` path
(`tools/sgx-window-swap.c`, Stage 6a: 3059 swaps, ~102 fps). So the cost is specific to
touching a **dynamic** buffer each frame — a **fixed per-frame stall**, i.e. a wait/timeout,
not a copy/flush.

### Where the stall is — sampling profile + gdb

`/proc/PID/wchan` sampling (80 samples @ 20 ms): **76/80 in kernel `LinuxEventObjectWait`**
(the `pvrsrvkm` event-object wait). Not libc, not userspace compute.

gdb userspace backtraces (the DDK `.so` ship with debug symbols):

```
# glBufferData path (Write-after-Read buffer sync)
ioctl → PVRSRVBridgeCall (pvr_bridge_u.c:242)
      → PVRSRVEventObjectWait (osfunc_um.c:457)
      → WaitUntilResourceIsNotNeeded (kickresource.c:801)
      → KRM_WaitUntilResourceIsNotNeeded (ui32MaxRetries=100, kickresource.c:833)
      → glBufferData (bufobj.c:745) → glnvg__renderFlush → nvgEndFrame

# glDrawArrays path (TA kick)
ioctl → PVRSRVBridgeCall → PVRSRVEventObjectWait (osfunc_um.c:457)
      → SGXGetFreeDeviceSyncList (sgxkick_client.c:1549)
      → SGXKickTA → ScheduleTA → GLES2EmitState → glDrawArrays (drawvarray.c:2412)
```

Both bottom out in the same `PVRSRVEventObjectWait` → kernel `LinuxEventObjectWait`.
That is **why buffer strategy was irrelevant**: every path serializes the CPU on the GPU
via the same event object.

### Kernel side — the timeout, and who signals it

`services4/srvkm/env/linux/`:

- `osfunc.c:82` — `#define EVENT_OBJECT_TIMEOUT_MS (100)`.
- `osfunc.c:2003` `OSEventObjectWait()` → `LinuxEventObjectWait(hOSEventKM, EVENT_OBJECT_TIMEOUT_MS)`.
- `event.c:249` `LinuxEventObjectWait()` — loops `schedule_timeout(100 ms)`, breaks early
  only if `LinuxEventObjectSignal` bumped `sTimeStamp`. Returns `PVRSRV_ERROR_TIMEOUT`
  when the 100 ms runs out.
- `event.c:225` `LinuxEventObjectSignal()` — `atomic_inc(sTimeStamp)` +
  `wake_up_interruptible(sWait)` for each waiter.

Signal trigger (`services4/srvkm/common/pvrsrv.c`):

- `pvrsrv.c:1086` `PVRSRVMISR()` — runs the device MISR, `PVRSRVProcessQueues`, then
  `pvrsrv.c:1111` `OSEventObjectSignal(global event object)` → wakes the waiters.

IRQ/MISR wiring (`services4/srvkm/env/linux/osfunc.c`):

- `osfunc.c:551` `DeviceISRWrapper()` — `PVRSRVDeviceLISR()==TRUE` → returns
  `IRQ_HANDLED` (so `/proc/interrupts` line 37 "SGX ISR" counts up) → `OSScheduleMISR()`.
- MISR mechanism is a **private workqueue** (`Makefile:348 -DPVR_LINUX_MISR_USING_PRIVATE_WORKQUEUE`),
  not a tasklet.

### Diagnosis

The SGX render IRQ **is** firing (~3.8/frame; `/proc/interrupts` line 37 climbs), and the
LISR schedules the MISR. But the stall is a rock-steady **~180–200 ms ≈ two 100 ms
timeouts** per frame — i.e. the waiters are **running out `schedule_timeout` and being
released by the timeout, then re-polling**, _not_ being woken by the signal. So the
render completes, the IRQ lands, but the **MISR → `OSEventObjectSignal` → wake** path is
not releasing the per-frame resource waits (`KRM_WaitUntilResourceIsNotNeeded` for the
vertex buffer, `SGXGetFreeDeviceSyncList` for the TA kick). Same _family_ as kernel patch
`0008` (a completion signal not reaching the waiter), one layer up.

Cheap board probes could not isolate workqueue-latency-vs-missed-signal:
`create_singlethread_workqueue` is serviced by the shared kworker pool (the
`kworker/R-pvr_workqueue` thread is only the rescuer), so per-thread CPU reads 0 even when
the MISR runs. Definitive attribution needs kernel instrumentation.

### Fix candidates (kernel rebuild)

1. **Instrument** (temporary): counters for MISR runs, `OSEventObjectSignal` calls, and
   `LinuxEventObjectWait` timeouts-vs-wakes (+ last wait duration) via `dmesg`/debugfs —
   proves whether waiters time out or wake, and whether the MISR runs per render.
2. **MISR as tasklet** — drop `-DPVR_LINUX_MISR_USING_PRIVATE_WORKQUEUE` so `OSScheduleMISR`
   uses the tasklet path (`osfunc.c:904–952`): MISR in softirq immediately after the IRQ.
3. **Lower `EVENT_OBJECT_TIMEOUT_MS`** (100 → ~2–4 ms, `osfunc.c:82`): even on a missed
   wake, the re-poll returns in ms. Doubles as causation proof (if fps jumps with only
   this, the timeout is confirmed as the cost). Risk: it is a **global** wait timeout, so
   other paths poll more often; confirm no caller treats `TIMEOUT` as fatal.

Build A+B+C together, reflash, measure: ~5 fps → 30–60 fps confirms and fixes; keep the
clean change, drop the instrumentation for the real patch (a peer of `0008`/`0009`).

## Practical upshot for Stage 7 / appliance

- **Continuous animation** with per-frame re-tessellation is ~5 fps until the event-wait
  stall is fixed. **AA must be off** regardless (Issue A).
- **Mostly-static UI** is viable now: cache tessellated geometry in a **static VBO**
  (~100 fps), re-upload only on content change (pay ~200 ms per change). Good for an
  event-driven HMI, not for animation.

## `dc_nohw` surface leak (blocks iterative testing)

`dc_nohw` is single-client: the swapchain + imported `omapdrm` framebuffers are released
only on **clean EGL teardown** (`DCNohwPresentFlush` at swapchain-destroy). A **killed**
or stuck demo never runs teardown → `dcnohw` stays pinned (`users=1`), the display surface
stays "in use", and the next `eglCreateWindowSurface` fails until **reboot** (`rmmod
dcnohw` fails "in use"). Amplified by Issue B: on `SIGTERM` the demo is often parked in a
long KRM retry (up to `100 × 100 ms`), so it lingers holding the surface; `SIGKILL` can't
be caught at all → always leaks.

**Fix (in the kernel rebuild):** release the swapchain/fbs on **owner fd-close** in
`dcnohw.ko` (tie surface lifetime to process/connection death), so a crashed/killed client
is reclaimed automatically. Fixing Issue B also removes this in the happy path (fast clean
teardown).

## MMC DMA-multiblock-read experiment (fold into the same reflash)

Read throughput is ~500 kB/s because OMAP35xx ES2.1 **erratum 2.1.1.128** forces
single-block reads (the AB4 DT sets `mmc1 compatible = "ti,omap3-pre-es3-hsmmc"`; multiblock
CMD18 was hardware-confirmed to corrupt with `-EILSEQ`). Writes are unaffected (multiblock
CMD25, ~5.4 MB/s).

**The lead:** SPRZ278F Advisory 2.1.1.128 scopes the corruption to **"polling and interrupt
mode"** (PIO) — the controller's 1 KB (2×512 B) read buffer fills, the clock stops, and a
data-enable timing glitch drops the first sample on restart. With **DMA** draining the
buffer continuously, the clock should never stop → the glitch may not occur. So
**DMA-based multiblock reads may be erratum-safe**, restoring ~4× read speed legitimately.

**Experiment (integrity-gated):** allow multiblock reads again (DT `ti,omap3-hsmmc` on
mmc1, DMA ensured by patch `0002`'s DMAE gate), then **hash-verify**: read a large known
file many times and compare SHA-256 each pass. Clean across (e.g.) 50 MB × 20 reads → DMA
multiblock is safe here; keep it. Any mismatch → revert to `ti,omap3-pre-es3-hsmmc`
(single-block), we are genuinely read-bound. **Do not ship** until the hash test passes —
TI lists "no workaround", and this board already showed `-EILSEQ`, so proof is mandatory.

## References

- Kernel patches: [kernel/patches-devuan/](../kernel/patches-devuan/) (`0008` SGX IRQ 37,
  `0009` APM latency 500 ms are the closest prior art).
- DDK 1.6 KM source: geoduck worktree
  `/mnt/scratch/geoduck-tmp/beagle/openpvrsgx-ddk16/drivers/gpu/drm/pvrsgx/1.6.16.3977/`.
- Test app: [tools/nanovg-demo.c](../tools/nanovg-demo.c); static-VBO control:
  [tools/sgx-window-swap.c](../tools/sgx-window-swap.c).
- Presentation path: [docs/ddk16-presentation-stages-6-8.md](ddk16-presentation-stages-6-8.md).
