# pvrsrvkm firmware-watchdog `HWRecovery` storm on vector rendering — handoff

Handoff for the **kernel/DDK debugging chat**. Under a **2D vector-render workload**
(NanoVG / femtovg: gradient + rounded, anti-aliased, stencil-then-cover fills) the
DDK 1.6 SGX530 stack fires a **storm of `HWRecoveryResetSGX`** — roughly one reset
per rendered frame — even though the microkernel is making forward progress. This
supersedes the earlier "idle `SGXOSTimer` false-positive / patch-0009 APM" framing
(that file, `pvrsrvkm-watchdog-idle-false-positive.md`, was wrong and has been
removed). It is **separate** from the `PVRSRVProcessQueues` teardown use-after-free
in [pvrsrvkm-queue-teardown-oops.md](pvrsrvkm-queue-teardown-oops.md).

## Summary

- The recoveries are **firmware(ukernel)-initiated**, serviced by
  `SGX_MISRHandler`, **not** the host `SGXOSTimer` watchdog.
- They fire under a specific workload — **NanoVG/femtovg vector fills** — and
  **not** under solid fills, clears, cached-image blits, or a hand-built vanilla
  GLES2 repro (draw-count / blend / texture / uniform / stencil / VBO churn all
  tested clean on a fresh board).
- The SGX is **not truly hung**: the EDM task counter advances and the kernel CCB
  drains frame-to-frame; the dump only _looks_ idle because it is captured after
  the frame completes.
- On a **pristine OpenPandora (DDK 1.4.14.2514, kernel 2.6.27, same SGX530
  silicon)** the same NanoVG workload runs **with zero HWRecovery**. So the storm
  is **not intrinsic to the SGX530 silicon** — but that single A/B has multiple
  confounds and does **not** cleanly isolate "DDK 1.6 vs 1.4" (see caveats).

## What was ruled out (do not re-chase)

Repeatedly isolated on a freshly power-cycled board:

- **Host `SGXOSTimer` watchdog / idle false-positive / patch-0009 APM latency.**
  Decoded the `SGX Host control` block: `ui32HostDetectedLockups = 0` (host
  watchdog never fired) while `ui32uKernelDetectedLockups` climbs +1 per reset and
  `ui32InterruptFlags = HWR`. All recoveries are the **`SGX_MISRHandler`** path
  obeying a firmware HWR request — so any host-side `SGXOSTimer` busy-gate is inert
  for this bug.
- **Idle / frame-rate / per-swap GPU time.** A vanilla window+swap render at
  1.1 fps (≈900 ms/frame) is clean; the fastest and slowest _solid_ renders are
  both clean. Not rate, not idle gaps, not per-swap duration.
- **Raw draw-call count, fill-rate, blend, texture binds, uniform churn, stencil
  -then-cover, dynamic-VBO re-upload** — each added to a vanilla EGL/GLES2 probe,
  all clean at 24–48 draws/frame on a fresh board. (An earlier "stencil-24 wedge"
  was **cumulative session degradation**, not stencil — retracted.)
- **Display/flip pacing mode.** `present=1` (mailbox) and `present=2` (paced
  in-kernel flip) storm identically — the flip _pacing_ is not the variable.

## The firmware-watchdog signature

From an instrumented `SGXDumpDebugInfo` during a storm (≈19–20 events per short
run, one per frame):

- `EUR_CR_EVENT_STATUS = 0x20000000` = `EUR_CR_EVENT_STATUS_TIMER_MASK`
  (`sgx530defs.h`) — only the internal periodic **TIMER**, not a completion/fault
  event.
- All **BIF** registers zero (`BIF_FAULT = 0`) — no memory fault; cores idle, not
  fetching.
- Kernel CCB `WO == RO` (drained) and **`EDM_TASK_REG0` increments across the
  storm** (e.g. 0xB3→0xBA→…→0x113), with the CCB write offset advancing ~+5 per
  reset — i.e. the ukernel **retires a frame's work each time and makes forward
  progress**.
- `SGX Host control`: `ui32PowerStatus` not `NO_WORK` (ukernel thinks it has
  work), `ui32uKernelDetectedLockups` rising, `ui32HostDetectedLockups = 0`,
  `ui32InterruptFlags = 0x1 (HWR)`.

Reading: the microkernel's own lockup watchdog trips **around the vector-fill
render** and requests host recovery; by the time the host dumps state the render
has completed, so it looks deceptively idle. This is a **false lockup relative to
intent** (progress is being made), not a genuine hang.

## The trigger and the reproducer

The trigger lives in the **NanoVG/femtovg rendering algorithm** (stencil-then-cover
fills + gradient-ramp paint textures + the uber-shader with a per-fragment scissor
mask + dynamic VBO re-upload) — the _combination_, which a simplified hand-built
probe never reproduced.

**NanoVG is the portable C reproducer** (femtovg is a Rust port of NanoVG, so the
GL command stream is the same). `tools/nanovg-demo.c` renders gradient rounded-rect

- stencil fills = the storm pattern.

* **B4 (DDK 1.6):** `nanovg-demo` → **20 HWRecovery in ~120 frames** (storms,
  TIMER signature, run slowed to ~2 fps by recovery overhead).
* Build (B4 glibc 2.36): `.symver fmodf→GLIBC_2.4` for the `fmodf@2.38` skew, plus
  `-Wl,--unresolved-symbols=ignore-in-shared-libs` for the DDK stubs. Run:
  `LD_LIBRARY_PATH=/opt/sgx-ddk16/lib /root/nanovg-demo-b4 <frames>`.
* Build (Pandora glibc 2.9): cross-link against a Debian **squeeze eglibc 2.11.3**
  sysroot with `-D_TIME_BITS=32` (dodge the Debian 64-bit-`time_t`
  `__clock_gettime64@2.34` redirect), `librt.so.1` (old glibc keeps
  `clock_gettime` there), explicit pre-multiarch CRT + `libc_nonshared.a`. Result
  references only `GLIBC_2.4`. (Full command in repo memory.)

## The A/B — and its caveats

| stack                  | kernel / DDK                | NanoVG workload         | HWRecovery      |
| ---------------------- | --------------------------- | ----------------------- | --------------- |
| B4 (our port)          | 7.2.0 / DDK 1.6.16.3977     | 120 frames              | **20 (storms)** |
| OpenPandora (pristine) | 2.6.27.46 / DDK 1.4.14.2514 | 200 frames + 500 frames | **0**           |

Same SGX530 silicon (rev 1.0.3). So the storm is **not something the SGX530 must
do** — a stock vendor stack handles the identical stencil-fill workload cleanly.

**But this does not isolate "DDK 1.6 vs 1.4."** The two stacks differ in several
ways, any of which could be the real cause:

- **Kernel** — 2.6.27 vs our 7.2 port (host-side MISR / power-management differ).
- **DDK / ukernel firmware version** — 1.4.14 vs 1.6.16.
- **APM configuration** — the B4 carries patch 0009 (`SYS_SGX_ACTIVE_POWER_LATENCY_MS`
  1 → 500 ms). **Weak candidate:** the storm dump shows `ui32PowerStatus != NO_WORK`
  (the ukernel believes it has pending work) and fires during continuous
  rendering, so the SGX is _not_ in the idle state APM acts on — the 1 ms/500 ms
  latency has nothing to power down, so it is unlikely to be the differentiator.
  (Also: 0009 _reduces_ power transitions, and the idle/rate framing was already
  disproven — the trigger is the vector-fill workload, not idle gaps.)
- **Display / flip-completion path** — Pandora `omaplfb` + `FRONTWSEGL`
  (direct front-buffer scanout) vs B4 `dc_nohw` + `FLIPWSEGL` + paced in-kernel
  present. The ukernel watchdog fires on "pending work not completing," which can
  be flip-completion-sensitive — a **strong** candidate given the signature.

(Note: the earlier "DDK 1.4 doesn't leak where 1.6 OOMs" observation is **not**
valid corroboration — that OOM was later shown to be CMA fragmentation, not a 1.6
leak. See the "NO LEAK" verdict in the native-control notes.)

## Recommended next step (for the pvrsrvr chat)

**Instrument what the ukernel is blocked on when it raises HWR.** The signature
(`ui32InterruptFlags = HWR`, `ui32PowerStatus != NO_WORK`, `uKernelDetectedLockups`
rising, EDM counter still advancing, BIF clean) says the microkernel _believes it
has outstanding work that isn't completing_ under the vector-fill workload. The
existing dump shows _that_ it's stuck but not _on what_. Extend `SGXDumpDebugInfo`
/ the `SGX_MISRHandler` path to dump, at the moment of the firmware HWR request:

- the ukernel's per-data-master pending state (TA / 3D / 2D render counts, the
  outstanding kick/render IDs from the `SGXMKIF_HOST_CTL` / ukernel status
  buffer), and
- the sync / flip-completion the ukernel is waiting on (WriteOps pending vs
  complete on the relevant sync).

That identifies the completion that never arrives — which is the actual bug
(most likely a **render/flip completion-delivery** issue in the 1.6 ukernel or in
our 1.6 host path: `dc_nohw` present + MISR + the 7.2 port), and points straight
at the fix.

### Secondary / lower-value

- **DDK 1.4 on the B4's 7.2 kernel** would isolate DDK-version from the kernel,
  but 1.4 is unlikely to be the answer and rebasing the openpvrsgx 1.4 branch to
  1.6 parity is significant work — not worth it as a diagnostic.
- **APM sanity check (expected null):** revert 0009 (500 ms → 1 ms) and re-run.
  Per the ukernel-has-pending-work signature this should _not_ change the storm;
  it only cheaply falsifies APM as a factor.
- Loading DDK **1.6 on the Pandora** is _not_ practical (7.2 module won't load on
  2.6.27; 1.6 likely never targeted that kernel; different display class).

## Relationship to the teardown UAF

Distinct bug. The `PVRSRVProcessQueues` Oops is a host-side use-after-free during
command-queue teardown; this is a firmware-watchdog false-lockup during steady-state
vector rendering. Keep the two fixes independently reviewable.

## Environment / source / assets

- B4: BeagleBoard B4, OMAP3530 ES2.1, SGX530 SGX103, 192.168.50.245, kernel
  `7.2.0-g214a35fbc02e-dirty`, DDK 1.6.16.3977, soft-float GL at
  `/opt/sgx-ddk16/lib`.
- Pandora: OpenPandora, SGX530 rev 1.0.3, kernel 2.6.27.46, Angstrom 2010.4, DDK
  1.4.14.2514, glibc 2.9, fb0 = RGB565 800×480, `powervr.ini = FRONTWSEGL`.
- Source of interest (DDK 1.6.16.3977):
  `services4/srvkm/devices/sgx/sgxinit.c` — `SGX_MISRHandler` (the reset path
  here), `HWRecoveryResetSGX`, `SGXOSTimer` (the _other_, host watchdog — inert
  for this), `SGXDumpDebugInfo`.
- Reproducer: `tools/nanovg-demo.c` (+ the NanoVG source tree on geoduck under
  `nanovg/`). Full B4 and Pandora build recipes are in repo memory.
