# SGX530 / DDK 1.6 femtovg render storm — root cause

**Board:** BeagleBoard B4 (OMAP3530 ES2.1, SGX530 core SGX103), Devuan Trixie
armhf, kernel 7.2.0, DDK 1.6.16.3977, 106 MB RAM.
**Control:** stock Pandora (OMAP3530, SGX530 rev 1.0.3, DDK 1.4.14.2514,
kernel 2.6.27) — same silicon family, older DDK.

## TL;DR

Slint's FemtoVG renderer drives the SGX530 into a **per-frame firmware Hardware
Recovery (HWR) storm** — one GPU reset on *every* rendered frame, throttling the
UI to ~2 fps. It is **not** a teardown/kill bug (that was a separate, since-fixed
issue) and **not** anti-aliasing or gradient *textures*. The trigger is a
two-factor interaction, reproduced in dependency-free C:

> **Heavy anti-aliased fringe geometry (tile-spanning AA sliver strips) drawn with
> a render-state change (`glUseProgram` / `glBindTexture`) in front of each draw.**

Remove *either* factor and the storm drops to a flat zero. nanovg draws the
identical picture cleanly because it renders everything under **one** shader
program (no per-draw state regeneration).

## Symptom

- femtovg `tilesredraw` mode (gradient rounded tiles, un-batched): **2.3 fps,
  ~23 HWR** over the run.
- `dmesg`: repeated `PVR_K:(Error): HWRecoveryResetSGX` with **no**
  `SGXOSTimer() detected SGX lockup` line preceding it.
- Cadence is **metronomic**: measured 29 consecutive HWR events spaced
  0.46–0.53 s apart (mean ≈ 0.49 s) — **exactly one reset per frame**, never a
  frame skipped or doubled. The ~0.49 s/frame *is* the reset-recovery latency;
  the storm throttle is the frame time.

## Reset origin — firmware ukernel HWR, not the host watchdog

`HWRecoveryResetSGX` has two callers: the host `SGXOSTimer` (prints
"detected SGX lockup" first) and the `SGX_MISRHandler` ISR (firmware sets
`PVRSRV_USSE_EDM_INTERRUPT_HWR`, prints nothing). Instrumented captures show:

- `BIF_FAULT = 0` — no MMU/memory fault.
- `EUR_CR_EVENT_STATUS` EDM task register **advancing** — the ukernel loop is
  alive and running.
- **0** "detected SGX lockup" messages — the host timer never fired.
- HWR arrives via the **MISR path** — **the firmware raised the recovery itself.**
- Widening the host HWR timeout 10 → 100 ms (patch 0015) changed **nothing** —
  it is not mere impatience; the monitored progress signal stays flat for the
  whole render.

So the ukernel, while demonstrably still executing, decides this specific command
stream is unrecoverable and resets. Its forward-progress / state model cannot
represent "pathological-but-progressing," and its only tool is a full pipeline
reset.

## What was ruled out

| Hypothesis | Test | Result |
|---|---|---|
| Session teardown / SIGKILL bug | kill mid-render, single-clock kmsg markers | Teardown is prompt (~0.17 s) and clean; storm is a render-phase event |
| Anti-aliasing per se | femtovg `tilesrsolid` vs `tilesrsolidnoaa` | **Both clean**, 9.7 fps — AA alone not the trigger |
| Gradient *texture* upload | GL trace: `glTexImage2D` count | **0 in both** clean and storming frames — no gradient texture exists |
| Cold-buffer / warm-up | every window frame is a fresh swap (always "cold") | Storm stops only when *work* lightens (caching), not with warm-up |
| State churn alone | `state-churn.c`: 48 prog + 48 tex switches, trivial geometry | **Clean** — state-change count alone does not storm |
| Per-frame geometry upload | `tilerepro` NOUPLOAD (static VBO) | **Still storms** — upload is innocent |
| Host watchdog too aggressive | patch 0015 widen 10→100 ms | No change — firmware-originated |

## GL-level diff (per frame, via LD_PRELOAD interposer `tools/gl-trace.c`)

| Mode | draws | verts | bytes | glUseProgram | glBindTexture | glTexImage2D | Result |
|---|---|---|---|---|---|---|---|
| femtovg `tilesgrect` (gradient plain rect) | 2 | 16 | 352 | 4 | 7 | 0 | clean 9.8 fps |
| femtovg `tilesredraw` (gradient rounded) | **48** | **2304** | **37 KB** | **48** | **97** | 0 | **STORM 2.3 fps** |
| nanovg (gradient rounded, uber-shader) | 16 | 880 | 14 KB | **2** | 3 | 0 | clean 9.2 fps |

The storming frame is distinguished by **48 draws each preceded by a program /
texture switch**, over heavy rounded-fill + fringe geometry. nanovg draws the
same picture under **2** program binds.

## Dependency-free C repro — `tools/tilerepro.c`

Reconstructs femtovg's `tilesredraw` with no femtovg/nanovg/Slint: N rounded-rect
tiles on a grid, each = a `TRIANGLE_FAN` fill + a 1px `TRIANGLE_STRIP` AA fringe
along the outward normal. Three independent knobs isolate the factors.

Bisection matrix (12 tiles, seg=8, 30 frames, single-clock kmsg phase markers):

| Mode | fringe | per-draw churn | per-frame upload | HWR / 30 frames |
|---|---|---|---|---|
| **FULL** | ✅ | ✅ | ✅ | **30 (storm, 1/frame)** |
| NOCHURN | ✅ | ❌ | ✅ | **0 (clean)** |
| NOFRINGE | ❌ | ✅ | ✅ | **0 (clean)** |
| NOUPLOAD | ✅ | ✅ | ❌ | **31 (storm)** |

**Conclusion:** the storm requires **both** the fringe geometry **and** the
per-draw state change. Neither alone storms. Per-frame upload is irrelevant.

## Mechanism

Every `glUseProgram` / `glBindTexture` forces the driver to regenerate the
PDS / TA render-state metadata for the following draw. That is cheap when the
draw is light (state-churn with trivial triangles → clean). But when the
following draw is a **heavy tile-spanning AA fringe strip** (many slivers →
large per-tile primitive-list work), the combination — *regenerate render state,
then tile a fringe* — repeated ~48× per frame makes no ukernel-monitored progress
within the window, and the firmware raises HWR. Under a **single program**
(nanovg's uber-shader, or `NOCHURN`) the fringe tiles without per-draw regen, the
render advances, and it stays clean.

This is consistent with the vendor's own guidance (*PowerVR Performance
Recommendations*: minimize render-state changes, batch draws) — SGX530 is a
tile-based deferred renderer whose parameter/state management is the scarce
resource, not raw fill rate. Community reports corroborate the shape:
imgtec-forum users see SGX HWR "lockups get worse with larger" output, and TI's
"PowerVR Guilty Lockup" thread confirms the firmware-side watchdog/reset path.

## Why this is disappointing but real

The GPU is not too slow — nanovg renders the same fill rate at 9 fps clean. The
firmware's recovery model is too blunt: it cannot distinguish a legitimately
busy-but-progressing render from a wedge, has no preemption or partial drain, and
fails closed to a full reset. Because the offending pattern is present in *every*
frame's command stream, the reset fires once per frame, deterministically.

## Tuning guidance — how to keep femtovg / Slint on this GPU

Ordered from least to most invasive. All keep the SGX in play except the last.

1. **Enable Slint element caching** (measured fix). `cache-rendering-hint` on the
   tile subtree collapses the per-frame regenerated fringe+state stream into a
   cached texture blit: A/B measured **35 HWR @ 2.3 fps → 8 HWR (startup only)
   @ 20.5 fps**. This is the single highest-leverage change.
2. **Avoid per-element gradient-on-rounded fills on hot paths.** Prefer solid
   fills (`tilesrsolid` clean 9.7 fps) or plain-rect gradients (`tilesgrect`
   clean 9.8 fps). The storm is specifically the gradient × rounded × per-draw
   -state-change combination.
3. **Reduce per-draw state changes** — the nanovg lesson. If touching the
   renderer, batch vector geometry under one shader program / minimize
   program+texture rebinds between fringe draws. nanovg's uber-shader approach is
   immune drawing the identical scene.
4. **Prefer Skia or nanovg over femtovg** for GPU vector UI on SGX530 if the
   renderer is a free choice — both batch far more aggressively (nanovg: 2 program
   binds vs femtovg's 48 for the same frame).
5. **Fall back to Slint's software renderer** (`SLINT_BACKEND=winit-software`, or
   the LinuxKMS software renderer) as the guaranteed-safe path. It bypasses the
   SGX entirely, so no HWR is possible; the trade-off is CPU cost on the
   OMAP3's Cortex-A8, acceptable for a low-frame-rate appliance UI.

## Reproducing

```sh
# Build on-board (softfloat toolchain, glibc 2.36):
sgx-cc -O0 -I/usr/include/sgx-ddk16 tilerepro.c -lEGL -lGLESv2 -lm -o tilerepro
# FULL storms, NOFRINGE / NOCHURN clean:
sgx-ddk16-run ./tilerepro 12 1 1 1 8 30   # tiles fringe churn upload seg frames
sgx-ddk16-run ./tilerepro 12 0 1 1 8 30   # NOFRINGE -> clean
sgx-ddk16-run ./tilerepro 12 1 0 1 8 30   # NOCHURN  -> clean
```

Count resets with a single dmesg clock:

```sh
dmesg -C; dmesg -w & sgx-ddk16-run ./tilerepro 12 1 1 1 8 30; \
  grep -c HWRecoveryResetSGX /dev/kmsg   # or grep the captured follow log
```

## Related tools / files

- `tools/tilerepro.c` — dependency-free storming repro (this doc's evidence).
- `tools/state-churn.c` — negative control (state churn alone, clean).
- `tools/gl-trace.c` — LD_PRELOAD GLES2 per-frame/per-call profiler.
- `tools/tri-inline.c` — earlier nanovg-path repro (found the depth-bit trigger).
- `slint/femtovg-probe/` — femtovg mode matrix (tilesredraw / tilesgrect / …).
- `slint/tile-demo/` — Slint rounded-tile demo with `SLINT_CACHE` A/B toggle.
