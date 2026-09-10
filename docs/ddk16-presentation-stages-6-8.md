# OMAP3 SGX103 DDK 1.6 Presentation — Later Stages (6–8)

Satellite of the [DDK 1.6 `dc_nohw` presentation plan](ddk16-dcnohw-presentation-plan.md).

These stages are **gated on Stage 5** — the transparent userspace presenter
(`sgxmode`) driving an **unmodified** game via the `dc_nohw` swap-notify (Stages
0–5 live in the main plan). They are kept out of the main plan so it stays
focused on the active stage. The architecture (DisplayClass↔DRM layering,
display-ownership invariant, buffer-state protocol), Handoff Card, and
engineering rules live in the main plan and apply throughout.

Stage 6 moves the flip **in-kernel** (daemonless endgame); Stage 5a's userspace
flip loop is its working prototype. **Phase 5b was skipped** — its buffer-state
pacing is validated directly here as **Stage 6b** (driven by the real in-kernel
vblank flip-done, not a throwaway `sgxmode` ioctl loop). Stage 6 is split into
**6a** (in-kernel *mailbox* flip: prove the `omapdrm` export + in-kernel atomic
commit, complete immediately — same ghosting as 5a) and **6b** (paced: hold
completion until vblank flip-done — ghosting gone). Stage 7 validates a real game
unchanged; Stage 8 packages the appliance.

**Iteration model (`omapdrm` is `CONFIG_DRM_OMAP=m`).** The whole DRM stack is
modular on the board (`omapdrm`, `drm`, `drm_kms_helper`, `drm_display_helper`,
`ti_tfp410`, `display_connector`), so the `omapdrm` export patch is a **module
rebuild**, not a kernel reflash: rebuild `omapdrm.ko` (matched vermagic, the same
utsrelease-pin discipline as `dcnohw.ko`), build `dcnohw.ko` against `omapdrm`'s
new `Module.symvers` so the `omapdrm_present` CRC matches, deploy both, and
**reboot** to load them (runtime `rmmod omapdrm` is impractical — `fbcon`/DVI
chain hold it — but a reboot is cheap).

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
vblank flip-done fires `flip_done(cookie)`. Because `dc_nohw`'s buffers are CMA
`dma_buf`s, `omapdrm` must also expose an **import** helper (the in-kernel
equivalent of `sgxmode`'s `PRIME_FD_TO_HANDLE` + `ADDFB2`):

```c
/* omapdrm — new, GPL-only */
struct drm_framebuffer *omapdrm_import_dmabuf(struct dma_buf *dbuf,
                                              u32 width, u32 height,
                                              u32 pitch, u32 fourcc);
void omapdrm_release_fb(struct drm_framebuffer *fb);
```

`dc_nohw`'s `ProcessFlip` (its registered `DC_FLIP_COMMAND` handler) drives it:

- **Swapchain create:** register each `dc_nohw` buffer as an `omapdrm`
  framebuffer once → `fb[K]` (the buffers are module-scope, so this may even be
  done once at init).
- **Per swap:** `omapdrm_present(fb[K], flip_done_cb, cookie)`.
- **flip-done callback:** `pfnPVRSRVCmdComplete(cookie)` — the buffer is now
  reusable.
- **Swapchain destroy:** unregister the framebuffers.

**`ProcessFlip` context.** Today `ProcessFlip` calls `Flip()` then
`pfnPVRSRVCmdComplete(hCmdCookie, IMG_FALSE)` **inline**. `drm_atomic_helper_commit`
takes modeset locks (may sleep), so the commit is issued from a **workqueue**
(safe regardless of `ProcessFlip`'s context). `pfnPVRSRVCmdComplete` is
ISR/MISR-safe (hardware DCs complete swaps from the vblank handler), so **6b**
calls it from the `omapdrm` flip-done callback; **6a** calls it inline
(fire-and-forget mailbox).

A driver's own in-kernel commit needs **no** DRM master, so nothing in the frame
path is a master. `dc_nohw` gains a module dependency on `omapdrm` (as it already
depends on `pvrsrvkm`): `dc_nohw` → `{pvrsrvkm, omapdrm}`. This requires a small
`omapdrm` patch (`EXPORT_SYMBOL_GPL(omapdrm_present)` + `omapdrm_import_dmabuf`),
acceptable for this appliance fork. Stage 5a already proved the
register/flip/pace mechanics in userspace (`sgxmode`) — the ioctl sequence there
maps one-to-one onto the exported calls.

**Parking `fbcon` without a userspace flip master.** `fbcon` is an in-kernel
`drm_client`; there is no clean in-kernel arbitration against `dc_nohw`'s
commits. Two console options (independent of the 6a/6b phase split above):

- **Console-park (bring-up):** a tiny userspace helper holds DRM master as a
  _parking token_ (no flips) for the app's lifetime — `SET_MASTER` suspends
  `fbcon`; the driver's in-kernel commit bypasses the master check and still
  drives pixels; `DROP_MASTER` on exit restores the console. Keeps a console
  fallback on the other VTs. (`sgxmode` in a no-flip “park” mode serves as this
  helper for 6a.)
- **Console-unbind (ship):** unbind `fbcon` on the panel
  (`/sys/class/vtconsole/vtcon*/bind` → 0, or `fbcon=map`) so `dc_nohw` is the
  sole CRTC user — no master anywhere. No panel console; admin over serial/SSH.

**`vtrun` — the generic launcher (replaces `openvt`).** What remains of
`sgxmode` once its flip loop moves into `dc_nohw` is not SGX-specific: set up a
graphics VT and run a fullscreen app. There is no minimal stock tool for this
(`openvt` does the VT switch but not `KD_GRAPHICS`/`K_OFF`/master; `logind`/
`seatd` is a heavyweight seat manager). `vtrun` **replaces** `openvt` rather than
wrapping it: it folds in the VT allocation (`VT_OPENQRY` → free VT → activate →
`VT_DISALLOCATE`) so the `kbd` package is not required, and it sets
`KD_GRAPHICS`/`K_OFF` **before** activating to avoid the console text-flash on
switch. It optionally takes the console-park master. `vtrun` is literally
`sgxmode` minus the flip loop and swap-notify, generalized:

```
vtrun <app>   # VT_OPENQRY, KD_GRAPHICS + K_OFF (pre-activate), [SET_MASTER],
              # fork/exec app, wait; on exit restore KD_TEXT/K_XLATE,
              # DROP_MASTER, switch back, VT_DISALLOCATE
```

### Phase 6a: In-kernel mailbox flip — VALIDATED (2026-09-09)

Prove the new in-kernel path with **immediate** completion (mailbox, like 5a):
`dc_nohw` imports its buffers as `omapdrm` framebuffers, `ProcessFlip` schedules
a workqueue `omapdrm_present(fb[K])` and completes the swap inline. Uses the
console-park helper so `fbcon` yields. Same cross-buffer ghosting as 5a is
acceptable — the point is to retire the `omapdrm` export + in-kernel atomic
commit risk with **no** presenter daemon in the flip path.

**Result:** `omapdrm` gained `omapdrm_import_dmabuf` / `omapdrm_release_fb` /
`omapdrm_present` (`EXPORT_SYMBOL_GPL`, new `omap_present.c`; a module-global
`drm_device` is stashed in `omapdrm_init`). `dc_nohw` gained an opt-in present
path (`dc_nohw_present.c`, module param `present=1`): on the first swap it
imports all back buffers as `omapdrm` framebuffers once, and each `ProcessFlip`
queues an ordered-workqueue `omapdrm_present(fb[latest])` (blocking atomic commit
of the primary plane) while completing the swap inline. Both modules rebuilt with
matched vermagic; `omapdrm` is `=m` so this was a module rebuild + reboot (no
kernel reflash), and `dcnohw.ko` was built against `omapdrm`'s `Module.symvers`
(`KBUILD_EXTRA_SYMBOLS`) so the export CRCs match. On the B4 (`present=1`,
`omapdrm` refcount rose to 2 confirming the link), an **unmodified**
`sgx-window-swap` (`SGX_TRIANGLE=1 SGX_DEPTH=1`) ran 30 s (3059 swaps, ~102 fps)
as the child of `sgxmode` **in a no-flip `SGXMODE_PARK` mode** (holds master to
suspend `fbcon`, no swap-notify, no flips). The **rotating triangle animated on
the panel driven entirely in-kernel**, dmesg clean, clean exit. Expected 5a-style
double/ghost triangle (mailbox) remains — fixed by 6b.

Pass criteria:

- [x] An unmodified game displays via the in-kernel flip — no `sgxmode` flip
      loop, no userspace master in the flip path (only the idle console-park
      token).
- [x] `omapdrm_import_dmabuf` + `omapdrm_present` register and commit the
      `dc_nohw` buffers; the `omapdrm` refcount and clean dmesg confirm the
      in-kernel commit path. (Formal DMA-fence/vblank tracing deferred to 6b.)
- [x] Child exit and console-park drop restore `fbcon` cleanly.
- [ ] Stage 0 lifecycle/soak re-test (both `omapdrm.ko` and `dcnohw.ko`
      changed) — **still owed** before closing Stage 6.

### Phase 6b: Paced in-kernel flip

Add vblank pacing: `ProcessFlip` does **not** complete inline; the `omapdrm`
flip-done callback (vblank) calls `pfnPVRSRVCmdComplete` — exactly once per swap,
restoring the `FREE → RENDERING → READY → QUEUED → SCANNING → FREE` buffer-state
protocol and vsync back-pressure. This is the pacing 5b would have done, now
driven by the real in-kernel flip-done. Ghosting is gone.

Pass criteria:

- `omapdrm_present` commits pace to vblank; flip-done drives
  `pfnPVRSRVCmdComplete` **exactly once** per swap; the buffer-state protocol
  holds (no overwrite-while-scanning, no ghosting).
- Process failure and recovery keep buffer reuse correct; a sustained
  multi-minute run stays memory-flat with no recovery.
- SGX completion, in-kernel commit, and KMS flip latency are recorded
  separately.
- Console-park (bring-up) and console-unbind (ship) are each clean and
  reversible.

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
