# SGX DDK test kit

Probes and harnesses for validating the PowerVR SGX530 DDK on the OMAP3530 B4.
The softfp probes (`*.c`) `dlopen` the DDK EGL/GLES libraries at runtime, so they
carry no link-time GL dependency — only a libc/loader compatible with the DDK
userspace.

## Runtime layout the harness expects (on the target)

`test-sgx-ddk16.sh` drives the DDK 1.6 Stage 0 flow with **mixed-libc loader
isolation** (proven 2026-09-06 on the Devuan board):

- `KMOD_DIR` (default `/lib/modules/$(uname -r)/kernel/drivers/gpu/drm/pvrsgx/1.6.16.3977`):
  the DDK modules built for the **running** kernel — `pvrsrvkm.ko` at the top and
  `services4/3rdparty/dc_nohw/dcnohw.ko`. Use the kernel-tree modules, not the
  ti-ddk16 tarball's prebuilt `.ko` (wrong vermagic).
- `GL_ROOT` (default `/root/s16/gl`): the es2.x softfp GL libs **plus the 1.6
  WSEGL** (see below). This is the `soak16` bundle's `gl/` dir.
- `ARMEL_ROOT` (default `/opt/pandora-armel`): a modern armel libc (glibc 2.34+)
  - `ld-linux.so.3` and the compiled probe (`bin/sgx-pbuffer-latency`).
- `INIT_CMD` (default `/root/s16/run.sh pvrsrvinit`): runs `pvrsrvinit` under the
  DDK's own legacy loader.

The armel loader runs the probe so its `GLIBC_2.34` symbols resolve, while it
`dlopen`s the old softfp GL libs from `GL_ROOT`. `pvrsrvinit` only succeeds right
after a fresh module load; a second run reports "already initialised" (harmless).

### The WSEGL requirement (the Stage 0 blocker)

The DDK EGL loads a **WSEGL window-system module** at `eglInitialize`, named by
`/etc/powervr.ini` `WindowSystem=` (else the compiled-in default
`libpvrPVR2D_FLIPWSEGL.so`). The `soak16`/`sgxus` GL bundle ships **without any
WSEGL**, so every EGL client (`eglinfo`, `gles2test1`, the probe) fails with a
NULL / `EGL_BAD_DISPLAY` display — while the PVR2D path
(`sgx_render_flip_test`) works fine.

Stage the matching **1.6.16.3977 softfp** WSEGL from
`beagle-archive/angstrom-sgx-test.tar` (`opt/ti-ddk16/runtime/libpvrPVR2D_*WSEGL.so`,
`libEGL.so` byte-identical to soak16's) into `GL_ROOT`, then select `FLIPWSEGL`:

```sh
printf '[default]\nWindowSystem=libpvrPVR2D_FLIPWSEGL.so\n' > /etc/powervr.ini
```

Use `FLIPWSEGL`, not `FRONTWSEGL` (the latter sizes a front buffer from dc_nohw's
bogus geometry and OOMs immediately). The 1.4 WSEGL and the `gfx_rel_es{3,5,6,8}.x`
WSEGL do **not** work here (version mismatch / hard-float, respectively).

The harness writes `powervr.ini` and checks `GL_ROOT/$WSEGL` for you.

## Contents

| File                       | Purpose                                                                                                                                                                                                                                                                                                                                   |
| -------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `test-sgx-ddk16.sh`        | DDK 1.6 Stage 0 harness/data-collector: fresh module reload, `pvrsrvinit`, ensure WSEGL + `powervr.ini`, run the probe once, report frames done / OOM / CMA consumed / IRQ delta / slow frames / dmesg faults.                                                                                                                            |
| `test-sgx-ddk14.sh`        | Same, for the DDK 1.4 rollback baseline.                                                                                                                                                                                                                                                                                                  |
| `test-sgx-ddk16-matrix.sh` | Sweeps probe parameters (alternate / fbo / texture / rebind).                                                                                                                                                                                                                                                                             |
| `sgx-pbuffer-latency.c`    | The Stage 0 probe: two alternating pbuffers, per-frame FBO attachment rebind, `glClear`/`glFinish`; reports `over_500ms` and max latency.                                                                                                                                                                                                 |
| `sgx-window-swap.c`        | The Stage 1 probe: real EGL **window** surface on the `dc_nohw` swapchain. `SGX_SWAP=0` = Phase 1A allocation-only (render + `glFinish`, no swap); default = Phase 1B `eglSwapBuffers` cycling. `SGX_TRIANGLE=1`/`SGX_DEPTH=1` add shaders/VBO/depth. `SGX_CLEAR=0xAARRGGBB` renders a fixed colour. No FBO, no pbuffer, no presentation. |
| `dc_nohw_export_test.c`    | The Stage 2 smoke: opens `/dev/dc_nohw_export`, `QUERY_ABI`, `EXPORT_BUFFER(index)` → DMA-BUF FD, `mmap`s it, and reads back the pixels the Stage 1 probe rendered. `dc_nohw_export_test [index] [expected_argb]`; `DC_HOLD=<s>` holds the fd (tests the `rmmod` guard).                                                                  |
| `sgx-render-test.c`        | Surfaceless FBO → renderbuffer → clear → `glReadPixels` (pixel-readback correctness).                                                                                                                                                                                                                                                     |
| `pvr2d-latency.c`          | PVR2D blit latency probe.                                                                                                                                                                                                                                                                                                                 |
| `run-angstrom-sgx-test.sh` | Native Angstrom vendor-stack control (baseline comparison).                                                                                                                                                                                                                                                                               |
| `trace-sgx-flip.sh`        | Flip/present tracing helper.                                                                                                                                                                                                                                                                                                              |
| `mmc-live-diag.sh`         | MMC live diagnostics (board bring-up, not GPU).                                                                                                                                                                                                                                                                                           |
| `mesa-bookworm-compat.c`   | Mesa/GBM compatibility probe.                                                                                                                                                                                                                                                                                                             |

## Building a probe (softfp, matches the es2.x ABI)

```sh
arm-linux-gnueabi-gcc -mfloat-abi=softfp -O2 -o sgx-pbuffer-latency \
  sgx-pbuffer-latency.c -ldl -lrt
```

Build against a libc compatible with the DDK runtime (e.g. the `pandora-armel`
libc), not the host's modern glibc, or the binary will fail with
`version 'GLIBC_2.xx' not found` against the old es2.x userspace.

### Native OpenPandora (DDK 1.4, glibc 2.9, no on-device compiler)

The Pandora's native stack is glibc **2.9** with no compiler/headers/crt. Cross-build
a glibc-2.9-compatible binary by linking against Debian **squeeze** armel eglibc 2.11
(all symbol versions ≤ 2.9), bypassing the absolute-path `libc.so` GROUP script:

```sh
# sq/root = extracted libc6_2.11.3-4_armel.deb + libc6-dev_2.11.3-4_armel.deb
CC=arm-linux-gnueabi-gcc; G=$(dirname $($CC -print-file-name=crtbegin.o))
$CC -c -O2 -fno-stack-protector -D_TIME_BITS=32 -D_FILE_OFFSET_BITS=32 \
    -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -o probe.o sgx-pbuffer-latency.c
$CC -nostdlib -Wl,--dynamic-linker=/lib/ld-linux.so.3 -Wl,--no-as-needed -o probe \
  sq/root/usr/lib/crt1.o sq/root/usr/lib/crti.o $G/crtbegin.o probe.o \
  sq/root/lib/libc.so.6 sq/root/lib/libpthread.so.0 sq/root/lib/librt.so.1 \
  sq/root/lib/libdl.so.2 sq/root/lib/ld-linux.so.3 sq/root/usr/lib/libc_nonshared.a \
  -lgcc $G/crtend.o sq/root/usr/lib/crtn.o
# verify: readelf --dyn-syms probe | grep -o 'GLIBC_[0-9.]*' | sort -u   # -> GLIBC_2.4 only
```

`-D_TIME_BITS=32` is required (modern Debian armel headers default to 64-bit
`time_t`, redirecting `clock_gettime` to the missing `__clock_gettime64`).

## Probe environment flags

- `SGX_REBIND_ATTACHMENT` (default 1): re-attach the FBO color target every frame
  (the churn that OOMs our **Devuan/CMA** 1.6 port; clean on native stacks). `0` =
  attach once (clean baseline).
- `SGX_SKIP_RENDER=1`: re-attach but skip `glClear`/`glFinish` (the attach call
  alone does not leak).
- `SGX_WINDOW=1`: use a fullscreen **fbdev window** surface (native window handle
  `0`, override with `SGX_NATIVE_WINDOW`) instead of a pbuffer — required on native
  stacks (omaplfb) that expose ES2 only on WINDOW configs. Stop the display
  manager first so the framebuffer is free.
- `SGX_DUMP_CONFIGS=1`: dump every EGL config's `surface_type`/`renderable`/rgba
  and exit (diagnose missing pbuffer/ES2 configs).
- `SGX_DURATION_SECONDS`, `SGX_SUMMARY_ONLY`: time-bounded run / one-line summary.

### Stage 1 window-surface probe (`sgx-window-swap.c`)

Same softfp cross-build and loader-isolation as above (built on geoduck with
`arm-linux-gnueabi-gcc-14 -O2 -Wall -Wextra -Werror ... -ldl`, run under the
`pandora-armel` loader with the 1.6 GL libs). Requires `dcnohw.ko` loaded,
`pvrsrvinit` run after a fresh insmod, and `powervr.ini` = `FLIPWSEGL`.

```sh
# Phase 1A: swapchain allocation only (no swap), 5 create/destroy cycles
SGX_SWAP=0 /opt/pandora-armel/lib/ld-linux.so.3 \
  --library-path /opt/pandora-armel/lib:/root/s16/gl \
  /opt/pandora-armel/bin/sgx-window-swap 30 5

# Phase 1B: swap cycling soak (5 cycles x 30s), one-line summary
SGX_SUMMARY_ONLY=1 SGX_DURATION_SECONDS=30 /opt/pandora-armel/lib/ld-linux.so.3 \
  --library-path /opt/pandora-armel/lib:/root/s16/gl \
  /opt/pandora-armel/bin/sgx-window-swap 999999 5
```

Args: `frames_per_cycle [cycles]` (frames ignored when `SGX_DURATION_SECONDS`
set). Flags: `SGX_SWAP` (default 1), `SGX_TRIANGLE`, `SGX_DEPTH`,
`SGX_NATIVE_WINDOW` (default 0), `SGX_SUMMARY_ONLY`, `SGX_DURATION_SECONDS`.
Reports `swaps`, `average_swap_us`, `max_swap_us`, `over_500ms`, and the
per-cycle surface geometry.

## Running Stage 0 (DDK 1.6)

Reproduce the leak (per-frame `glFramebufferTexture2D` attachment churn):

```sh
FRAMES=120 PROBE_REBIND_ATTACHMENT=1 ./test-sgx-ddk16.sh
```

Clean baseline (attach once, then clear/finish — no churn):

```sh
FRAMES=120 PROBE_REBIND_ATTACHMENT=0 ./test-sgx-ddk16.sh
```

### Finding (2026-09-07, RESOLVED — no real leak; the OOM was transient)

- Render-to-texture attachment churn (`texture=1 rebind=1`) is **clean on every stack tested** — it does
  **not** leak:
  - **DDK 1.4** (OpenPandora native): 120 frames, MemFree flat
    ([../out/sgx-fbo-texture-ddk14-pandora.csv](../out/sgx-fbo-texture-ddk14-pandora.csv)).
  - **DDK 1.6** (native Ångström 3.0.14, byte-identical blob): 120 frames, memory recovers mid-run
    ([../out/sgx-fbo-texture-ddk16-angstrom.csv](../out/sgx-fbo-texture-ddk16-angstrom.csv)); `strace` shows
    **balanced `ALLOC_DEVICEMEM` ↔ `FREE_DEVICEMEM`** (the blob frees every frame —
    [../out/sgx-fbo-ioctl-ddk16-angstrom.txt](../out/sgx-fbo-ioctl-ddk16-angstrom.txt)).
  - **DDK 1.6** (our Devuan port, fresh boot, `cma=48M`): **200 frames continuous + 8×40 back-to-back,
    `CmaFree` flat, no OOM**.
- The 2026-09-06 "OOM ~40 frames, `CmaFree` drains to exhaustion" was a **transient CMA-fragmentation state**
  (entangled with a `cma=64M` destabilization experiment), **not** a blob/port/version leak. It does not
  reproduce on a clean boot.
- Practical residue only: don't churn FBO attachments per frame (Imagination's documented antipattern) — no
  real workload does. `rebind=0` (stable attachment) is clean at native parity
  ([../out/sgx-fbo-norebind-ddk16.csv](../out/sgx-fbo-norebind-ddk16.csv)).

Never `kill -9` an SGX render process — it wedges the core (survives module
reload; needs a power-cycle). Let probes exit on their own frame/duration count.
