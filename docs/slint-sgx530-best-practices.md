# Slint app best practices for SGX530 / DDK 1.6 (OMAP3 B4)

Practical rules for building Slint UIs that stay clean on this GPU. Grounded in
the measured root cause in [sgx530-femtovg-render-storm.md](sgx530-femtovg-render-storm.md).

**Target:** OMAP3530 / SGX530 (SGX103), DDK 1.6.16.3977, Cortex-A8, 106 MB RAM,
FemtoVG renderer over GLES2.

## The one rule everything follows from

> **Never let a frame re-render heavy anti-aliased vector geometry on the GPU.**
> SGX530/DDK 1.6 raises a firmware Hardware Recovery (full GPU reset) when a frame
> issues heavy AA-fringe draws each preceded by a render-state change
> (`glUseProgram`/`glBindTexture`). FemtoVG does exactly that (~48 program binds
> for a 12-tile screen). A storming frame is hard-capped at ~2 fps (0.49 s = the
> reset-recovery latency), one reset per frame, indefinitely.

Every practice below either **caches** the heavy render so it happens once, or
**lightens** the per-frame stream so it stays under the firmware watchdog, or
**bypasses the GPU** entirely.

## Do

1. **Turn on `cache-rendering-hint` for any complex or vector-heavy subtree.**
   This is the highest-leverage change — measured **2.3 fps / storm → 19 fps /
   clean** on the tile demo. It renders the subtree to a texture once, then
   composites it each frame. Apply it to cards, tiles, rounded/gradient panels,
   icon groups, charts — anything static-looking that isn't a plain rectangle.

2. **Animate transforms, not content.** Cached items stay cheap only while the
   animated property is a **transform** (`x`/`y`, `opacity`, `scale`, `rotation`).
   Animating a cached item's *content* (gradient shifting inside it, size change,
   text/color change) **invalidates the cache and re-bakes every frame** — straight
   back to the 2.3 fps storm. Move, fade, and scale cached tiles freely; don't
   animate what's *inside* them.

3. **Prefer solid fills and plain-rectangle gradients on hot paths.** The storm is
   specifically the **gradient × rounded × per-draw-state-change** combination.
   Measured clean at ~9.7–9.8 fps: solid rounded tiles, and gradient *plain*
   rectangles. Measured storm: gradient *rounded* tiles. If a live-updating element
   must animate its content, make it a solid fill or a plain-rect gradient.

4. **Keep the first frame cheap.** Even with caching, the initial cold bake costs a
   ~0.5 s, ~8–9-reset startup transient. Show a trivial splash/solid frame first
   (or stagger the bake) so the user doesn't see the startup flicker.

5. **Minimize distinct paints per frame.** Fewer unique gradients/brushes ⇒ fewer
   program/texture switches ⇒ lighter stream. Reuse brushes; avoid a unique
   gradient per element.

## Don't

- **Don't animate a full-scene vector redraw every frame** (uncached gradient/rounded
  content). That's the storm generator.
- **Don't rely on the GPU as a hard dependency.** Treat SGX530 as an optional
  accelerator; always have the software-renderer path available (below).
- **Don't assume "it renders" means "it's clean."** A storming screen still displays
  — at 2 fps with the GPU resetting every frame. Always check HWR (below).

## The guaranteed-safe fallback: software renderer

`SLINT_BACKEND=winit-software` (or the LinuxKMS software renderer) bypasses the SGX
entirely — **no GPU, so a HWR is impossible.** The cost is CPU rasterization on the
Cortex-A8, acceptable for a low-frame-rate appliance UI. Use it as the default for
screens you can't guarantee are cache-stable, or ship it wholesale if the GPU path
proves too fragile for your UI.

```sh
SLINT_BACKEND=winit-software ./your-app        # winit + CPU renderer
# or the KMS software path on a bare appliance:
SLINT_BACKEND=linuxkms-software ./your-app
```

## Verify every screen is clean (do this per screen)

A screen is clean iff it produces **no per-frame HWR** and runs **above the ~2 fps
storm floor**.

```sh
# Run the screen ~15 s and count GPU resets:
dmesg -C
SLINT_SECONDS=15 ./your-app          # or drive the screen for 15 s
dmesg | grep -c HWRecoveryResetSGX
```

Interpretation:
- **0 resets (or a small one-time startup count, then clean)** and fps well above
  2 → clean. Good.
- **Resets accumulating ~1 per frame** and fps pinned near 2 → the screen is
  storming; apply caching / lighten the paint, or switch that screen to the
  software renderer.

Reference numbers (tile demo, this board):

| Config | fps | HWR (15 s) | Verdict |
|---|---|---|---|
| cache off | 2.3 | 25 (≈1/frame) | storm |
| cache on | 19.1 | 9 (startup only) | clean |
| software renderer | *(measure per app)* | 0 (no GPU) | always safe |

## Quick reference

| Symptom | Cause | Fix |
|---|---|---|
| Screen stuck ~2 fps, dmesg full of `HWRecoveryResetSGX` | Uncached heavy vector re-render each frame | `cache-rendering-hint`; animate transforms only |
| Was clean, storms after adding an animation | Animation dirties cached content → re-bake | Animate transform, not content; or solid/plain-rect paint |
| ~0.5 s flicker + ~8 resets at launch | Cold cache bake | Cheap first frame / staggered bake (cosmetic only) |
| Must live-update content and still storms | gradient × rounded × churn | Solid fill or plain-rect gradient; or software renderer |
| Can't guarantee a screen is safe | GPU fragility | `SLINT_BACKEND=winit-software` |

## Bottom line

Use Slint with **`cache-rendering-hint` on** and **transform-only animation** for the
GPU path (measured 19 fps clean), and keep the **software renderer** as the
can't-fail fallback. Design so most frames are cheap composites, never full-scene
vector re-renders. On this board the GPU is a bonus, not a foundation.
