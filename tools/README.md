# SGX DDK test kit

Probes and harnesses for validating the PowerVR SGX530 DDK on the OMAP3530 B4.
The softfp probes (`*.c`) `dlopen` the DDK EGL/GLES libraries at runtime, so they
carry no link-time GL dependency — only a libc/loader compatible with the DDK
userspace.

## Runtime layout the harness expects (on the target)

`test-sgx-ddk16.sh` drives the DDK 1.6 Stage 0 flow against two roots:

- `DDK16_ROOT` (default `/opt/ti-ddk16`): DDK modules (`module/pvrsrvkm.ko`,
  `module/dcnohw.ko`), the es2.x runtime (`runtime/`), and `runtime/pvrsrvinit`.
- `ARMEL_ROOT` (default `/opt/pandora-armel`): a modern armel libc + `ld-linux.so.3`
  and the compiled probes (`bin/sgx-pbuffer-latency`).

Two loaders are used deliberately: the DDK's own `ld-linux.so.3` runs `pvrsrvinit`
(legacy libc), and the armel loader runs the probe (so freshly built probes get a
libc that satisfies their symbol versions while still resolving the DDK GL libs).

On the Devuan recovery board this session used a self-contained equivalent instead:
the `soak16` bundle at `/root/s16` (Angstrom eglibc 2.12.2 softfp + es2.x GL libs +
`run.sh`). See [../docs/sgx-ddk16-stall-followups.md](../docs/sgx-ddk16-stall-followups.md)
for the quick per-image recipe.

## Contents

| File | Purpose |
| --- | --- |
| `test-sgx-ddk16.sh` | DDK 1.6 Stage 0 harness: load stack, `pvrsrvinit`, `dcnohw`, soak the probe, scan dmesg for `HWRecovery\|BIF\|watchdog\|Oops\|BUG\|fault`. |
| `test-sgx-ddk14.sh` | Same, for the DDK 1.4 rollback baseline. |
| `test-sgx-ddk16-matrix.sh` | Sweeps probe parameters (alternate / fbo / texture / rebind). |
| `sgx-pbuffer-latency.c` | The Stage 0 probe: two alternating pbuffers, per-frame FBO attachment rebind, `glClear`/`glFinish`; reports `over_500ms` and max latency. |
| `sgx-render-test.c` | Surfaceless FBO → renderbuffer → clear → `glReadPixels` (pixel-readback correctness). |
| `pvr2d-latency.c` | PVR2D blit latency probe. |
| `run-angstrom-sgx-test.sh` | Native Angstrom vendor-stack control (baseline comparison). |
| `trace-sgx-flip.sh` | Flip/present tracing helper. |
| `mmc-live-diag.sh` | MMC live diagnostics (board bring-up, not GPU). |
| `mesa-bookworm-compat.c` | Mesa/GBM compatibility probe. |

## Building a probe (softfp, matches the es2.x ABI)

```sh
arm-linux-gnueabi-gcc -mfloat-abi=softfp -O2 -o sgx-pbuffer-latency \
  sgx-pbuffer-latency.c -ldl -lrt
```

Build against a libc compatible with the DDK runtime (e.g. the `pandora-armel`
libc), not the host's modern glibc, or the binary will fail with
`version 'GLIBC_2.xx' not found` against the old es2.x userspace.

## Running Stage 0 (DDK 1.6)

```sh
DDK16_ROOT=/opt/ti-ddk16 ARMEL_ROOT=/opt/pandora-armel \
CYCLES=5 SOAK_SECONDS=180 \
PROBE_ALTERNATE=1 PROBE_USE_FBO=1 PROBE_REBIND_ATTACHMENT=1 \
  ./test-sgx-ddk16.sh
```

Never `kill -9` an SGX render process — it wedges the core (survives module
reload; needs a power-cycle). Let probes exit on their own frame/duration count.
