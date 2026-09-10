# OMAP3 SGX103 DDK 1.6 Presentation — Later Stages (6–8)

Satellite of the [DDK 1.6 `dc_nohw` presentation plan](ddk16-dcnohw-presentation-plan.md).

These stages are **gated on Stage 5** — the transparent userspace presenter
(`sgxmode`) driving an **unmodified** game via the `dc_nohw` swap-notify (Stages
0–5 live in the main plan). They are kept out of the main plan so it stays
focused on the active stage. The architecture (DisplayClass↔DRM layering,
display-ownership invariant, buffer-state protocol), Handoff Card, and
engineering rules live in the main plan and apply throughout.

Stage 6 moves the flip **in-kernel** (daemonless endgame); Stage 5's userspace
flip loop is its working prototype. Stage 7 validates a real game unchanged;
Stage 8 packages the appliance.

## Stage 6: In-Kernel Flip Endgame (`omapdrm_present`)

Remove the presenter from the frame path. `dc_nohw` flips `omapdrm` **directly**
from its DisplayClass flip handler — no daemon, no userspace master in the flip
path, no swap-notify hop.

**The bridge.** `omapdrm` exports a small entry point:

```c
/* omapdrm — new, GPL-only */
int omapdrm_present(struct drm_framebuffer *fb,
                    void (*flip_done)(void *cookie), void *cookie);
```

It performs a `drm_atomic_helper_commit` of the primary plane to `fb`, and the
vblank flip-done fires `flip_done(cookie)`. `dc_nohw`'s `ProcessFlip` (its
registered `DC_FLIP_COMMAND` handler) calls it:

- **Swapchain create:** register each `dc_nohw` buffer as an `omapdrm`
  framebuffer once → `fb[K]`.
- **Per swap:** `omapdrm_present(fb[K], flip_done_cb, cookie)`.
- **flip-done callback:** `pfnPVRSRVCmdComplete(cookie)` — the buffer is now
  reusable.
- **Swapchain destroy:** unregister the framebuffers.

A driver's own in-kernel commit needs **no** DRM master, so nothing in the frame
path is a master. `dc_nohw` gains a module dependency on `omapdrm` (as it already
depends on `pvrsrvkm`): `dc_nohw` → `{pvrsrvkm, omapdrm}`. This requires a small
`omapdrm` patch (`EXPORT_SYMBOL_GPL(omapdrm_present)`), acceptable for this
appliance fork. **Prototype the register/flip/pace mechanics in Stage 5
userspace first** — the ioctl sequence there maps one-to-one onto the exported
call.

**Parking `fbcon` without a userspace flip master.** `fbcon` is an in-kernel
`drm_client`; there is no clean in-kernel arbitration against `dc_nohw`'s
commits. Two options:

- **6a (bring-up):** a tiny userspace helper holds DRM master as a _parking
  token_ (no flips) for the app's lifetime — `SET_MASTER` suspends `fbcon`; the
  driver's in-kernel commit bypasses the master check and still drives pixels;
  `DROP_MASTER` on exit restores the console. Keeps a console fallback on the
  other VTs.
- **6b (ship):** unbind `fbcon` on the panel (`/sys/class/vtconsole/vtcon*/bind`
  → 0, or `fbcon=map`) so `dc_nohw` is the sole CRTC user — no master anywhere.
  No panel console; admin over serial/SSH.

**`vtrun` — the generic launcher (replaces `openvt`).** What remains of
`sgxmode` once its flip loop moves into `dc_nohw` is not SGX-specific: set up a
graphics VT and run a fullscreen app. There is no minimal stock tool for this
(`openvt` does the VT switch but not `KD_GRAPHICS`/`K_OFF`/master; `logind`/
`seatd` is a heavyweight seat manager). `vtrun` **replaces** `openvt` rather than
wrapping it: it folds in the VT allocation (`VT_OPENQRY` → free VT → activate →
`VT_DISALLOCATE`) so the `kbd` package is not required, and it sets
`KD_GRAPHICS`/`K_OFF` **before** activating to avoid the console text-flash on
switch. It optionally takes the 6a parking master. `vtrun` is literally `sgxmode`
minus the flip loop and swap-notify, generalized:

```
vtrun <app>   # VT_OPENQRY, KD_GRAPHICS + K_OFF (pre-activate), [SET_MASTER],
              # fork/exec app, wait; on exit restore KD_TEXT/K_XLATE,
              # DROP_MASTER, switch back, VT_DISALLOCATE
```

### Pass Criteria

- An unmodified game displays via the in-kernel path with **no** presenter
  daemon and no userspace master in the frame path.
- `omapdrm_present` commits pace to vblank; flip-done drives
  `pfnPVRSRVCmdComplete` exactly once per swap.
- Swapchain create/destroy register/unregister framebuffers without leaks;
  process failure and recovery keep buffer reuse correct.
- `fbcon` parking (6a) or unbind (6b) is clean and reversible.
- DMA-fence / vblank tracing shows `omapdrm` presenting the `dc_nohw` buffers.

## Stage 7: Application Validation

Move from the synthetic cube to OpenQuartz or GLQuake, unchanged, through the
Stage 6 path.

- Compile the game **soft-float** from source (the DDK userland is soft-float).
- A hard-float→soft-float GLES shim is a documented **fallback only**, for closed
  hard-float binaries — not the default path.

### Pass Criteria

- The application runs continuously for at least 30 minutes, unmodified.
- Input (via the graphics VT / evdev), shutdown, and restart behave predictably.
- Frame pacing, SGX completion, KMS latency, memory use, and dropped frames are
  recorded.
- No fallback to software rendering occurs.

## Stage 8: Packaging and Appliance

Integrate the validated stack into the image as a boot-to-launcher appliance.

### Deliverables

- Reproducible kernel patches and configuration; version-matched DDK 1.6 SGX103
  runtime staging.
- `dc_nohw` (buffer provider + swap-notify), the `omapdrm_present` patch,
  `sgxmode` (prototype) and `vtrun` (generic launcher), and test programs.
- A serial launcher menu that runs each app under `vtrun` (one fullscreen app at
  a time); `fbcon` kept (6a) or unbound (6b) per build.
- Automated Stage 0 smoke test and longer soak test.
- B4 KMS configuration and a separate Pandora display-validation checklist.
- Recovery instructions that preserve the existing DDK 1.4 fallback.
