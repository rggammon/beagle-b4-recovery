# OMAP3 SGX103 DDK 1.6 Presentation — Later Stages (4–8)

Satellite of the [DDK 1.6 `dc_nohw` presentation plan](ddk16-dcnohw-presentation-plan.md).

These stages are **gated on Stage 3** — a working KMS presenter that scans a
`dc_nohw` DMA-BUF out on the B4 display (Stages 1–3 live in the main plan). They
are kept out of the main plan so it stays focused on the active stage. The
architecture (DisplayClass boundary, ownership invariant, buffer-state protocol),
Handoff Card, and engineering rules live in the main plan and apply throughout.

## Stage 4: Synchronous GLES Presentation

Connect the Stage 1 window renderer to the Stage 3 presenter using the exported
swapchain buffers. Use explicit ready/free protocol messages and conservative
Services completion before notifying the presenter.

The fixed-width protocol must include ABI version, session ID, buffer index,
frame sequence, dimensions, stride, DRM format, and message type. Reject stale
sessions and non-monotonic sequences.

### Pass Criteria

- The rendered changing pattern or cube appears on the B4 display.
- No buffer enters `RENDERING` while it is `QUEUED` or `SCANNING`.
- `pfnPVRSRVCmdComplete` is issued only when the buffer is reusable.
- At least 60 seconds of presentation completes without recovery or corruption.
- SGX completion latency and KMS commit/flip latency are recorded separately.

## Stage 5: Explicit Linux Fences

Replace the conservative synchronous wait with a fence derived from the
buffer's Services write counters.

For serial $N$, signal completion only when
$\text{WriteOpsComplete} \geq N$. The fence must be advanced from the Services
completion path after firmware status is processed, not merely when an SGX
interrupt occurs. Recovery and teardown must signal outstanding fences with an
error.

Export the fence as a sync-file FD and use the target plane's explicit input
fence property when available. Fence completion does not replace DMA cache
ownership transitions or the buffer state protocol.

### Pass Criteria

- The presenter can queue before SGX completion.
- Scanout waits until the corresponding fence signals.
- Scheduling jitter does not cause tearing or premature buffer reuse.
- Recovery and shutdown resolve every outstanding fence.

## Stage 6: Implicit Synchronization

Attach the SGX completion fence to the exported DMA-BUF's `dma_resv` as its
write fence. Let a compatible display-driver import path wait through normal
framebuffer preparation.

### Pass Criteria

- The renderer/presenter protocol no longer transports a fence FD.
- DMA-fence tracing shows `omapdrm` waiting on the PowerVR fence.
- Explicit-fence and synchronous modes remain available as diagnostic controls.
- Buffer reuse and teardown remain correct under process failure and recovery.

## Stage 7: Application Validation

Move from the synthetic cube to OpenQuartz or another appropriate OpenGL game.
Keep the same EGL window, swapchain, export, and presenter path.

### Pass Criteria

- The application runs continuously for at least 30 minutes.
- Input, resize policy, shutdown, and restart behave predictably.
- Frame pacing, SGX completion, KMS latency, memory use, and dropped frames are
  recorded.
- No fallback to software rendering occurs.

## Stage 8: Packaging and Portability

Integrate the validated stack into the Devuan image and retain platform-neutral
renderer/export protocol boundaries.

### Deliverables

- Reproducible kernel patches and configuration.
- Version-matched DDK 1.6 SGX103 runtime staging.
- `dc_nohw`, exporter UAPI documentation, presenter, and test programs.
- Automated Stage 0 smoke test and longer soak test.
- B4 KMS configuration and a separate Pandora display-validation checklist.
- Recovery instructions that preserve the existing DDK 1.4 fallback.
