# NanoVG on SGX530 + on-board compile support — handoff

Handoff for the **deb-building / CI-pipeline chat**. Stage 7 pivoted away from a
game (OpenQuartz/gl4es) to **NanoVG** — a tiny MIT antialiased 2D vector canvas
with a native **GLES2** backend (`-DNANOVG_GLES2`). It is C, soft-float-clean,
and its bundled `demo.c` is an animated UI (clock, graphs, color wheel, images,
text) that doubles as a moving GLES2 workload for the `dc_nohw` present path.

Two work items:

1. **On-board compile support** — ship a soft-float **armel** toolchain + the DDK
   dev headers in the Devuan image so small programs build on the target.
2. **NanoVG in the pipeline** — cross-build the GLES2 demo against the DDK and run
   it through the Stage 6 `omapdrm_present` path.

## Verified board facts (2026-09-11, image = CI run 34560129398)

- Booted: Devuan 5 (daedalus), kernel `7.2.0-g432466d86a40-dirty`, root rw ext4 on
  `/dev/mmcblk0p2`. DDK `1.6.16.3977` (`sgx-ddk16-um:armel`, `sgx-ddk16-tools:armel`).
- Modules `pvrsrvkm`, `dcnohw` (`present=2`), `omapdrm` loaded; `/dev/pvrsrvkm` present.
- `sgx-ddk16-run services_test` → **rc=0** (DDK path healthy).
- DDK GLES/EGL present at `/opt/sgx-ddk16/lib/`: `libEGL.so`, `libGLESv2.so`,
  `libGLES_CM.so`, `libIMGegl.so`, `libpvrPVR2D_{FLIP,BLIT,FRONT,LINUXFB}WSEGL.so`,
  `libsrv_um.so`, `libsrv_init.so`.
- **No compiler, no make/git, no EGL/GLES2 headers on the board.**
  `sgx-ddk16-dev` is **not** installed. RAM: **106 MB total** (~61 MB free), plus
  the image's 64 MB zram swap (`beagle-memory` init service).
- dpkg arch: primary **armhf** (hard-float), foreign **armel** (soft-float) enabled.
  Devuan sources: `daedalus main` only (no contrib/non-free).

The decisive constraint: the base userland is **armhf hard-float**, but the DDK is
**armel soft-float** under `/opt/sgx-ddk16`. Anything that calls the DDK GLES2 must
be built **armel soft-float**. armhf CPUs execute armel (EABI soft-float) binaries
natively, so an armel cross toolchain runs fine on the board and its output runs
locally.

## Item 1 — on-board compile support (rootfs/build-devuan.sh)

Add to the Phase-2 chroot install (after the DDK um/tools install):

- **Toolchain:** `gcc-12:armel` + `make` + `libc6-dev:armel` + `pkg-config`,
  installed via the already-enabled **armel multiarch**. This is a native armel
  (soft-float) compiler that runs on the armhf board and emits the soft-float ABI
  the DDK needs, at the board's own glibc 2.36 (so no `fmodf@GLIBC_2.38`-style
  version skew — that only bites when building on a newer-glibc host). The Debian
  **cross** metapackage `crossbuild-essential-armel`/`gcc-arm-linux-gnueabi` is
  **not** in daedalus main (verified `(none)` on the board), so it is not used.
  The "not co-installable" concern does not apply here: the board ships **no**
  armhf gcc, so nothing else owns `/usr/bin/gcc`. `rootfs/build-devuan.sh`
  registers `arm-linux-gnueabi-gcc` and `cc` via `update-alternatives` →
  `arm-linux-gnueabi-gcc-12`.
- **DDK dev files:** install `sgx-ddk16-dev` (already produced by `make package`
  into `dist/`; add it to the `find … -name 'sgx-ddk16-dev_*_armel.deb'` discovery
  and the `apt-get install` line next to um/tools). It stages Khronos EGL/GLES2
  headers under `/usr/include/sgx-ddk16` and pkg-config `egl.pc`/`glesv2.pc` in
  `/usr/lib/arm-linux-gnueabi/pkgconfig` pointing `-L/opt/sgx-ddk16/lib`.

Ship a convenience wrapper `/usr/local/bin/sgx-cc` so a build is one command:

```sh
#!/bin/sh
# Compile a small soft-float armel program against the DDK 1.6 EGL/GLES2.
export PKG_CONFIG_PATH=/usr/lib/arm-linux-gnueabi/pkgconfig
exec arm-linux-gnueabi-gcc "$@" \
    $(pkg-config --cflags glesv2 egl) \
    $(pkg-config --libs glesv2 egl) \
    -Wl,-rpath,/opt/sgx-ddk16/lib
```

Verify block (mirror the existing checks): assert `arm-linux-gnueabi-gcc` exists,
`pkg-config --exists glesv2 egl` in the armel path succeeds, and a 5-line EGL
`main()` compiles and links against `/opt/sgx-ddk16/lib`.

Notes / gotchas:
- Weight: the cross toolchain + headers add a few hundred MB to the image — fine on
  the SD, but keep it to a build-tools set (don't pull a full desktop).
- RAM: only **small** programs are in scope. Single-file `-O2` compiles fit within
  106 MB + zram; NanoVG's `nanovg.c`/`demo.c` are moderate — compile one TU at a
  time. If a link/compile OOMs, add a USB swap (`beagle-swap` label is already
  honored by `beagle-memory`).
- Loader: the DDK debs bring Devuan's `libc6:armel`, so armel binaries run under the
  system loader natively — the old `/opt/pandora-armel` loader isolation the
  `tools/*.c` probes used should **not** be needed here. Confirm with a hello-world
  before assuming.

## Item 2 — NanoVG build in the pipeline

Cross-build on the CI host with the same soft-float toolchain (`arm-linux-gnueabi-gcc`)
against a sysroot carrying the DDK libs + `sgx-ddk16-dev` headers, then ship the
binary (and, if wanted, the source) in the image. Or build on-board via `sgx-cc`
once Item 1 lands.

- **Source:** pin a `memononen/nanovg` commit. Compile `src/nanovg.c` + `example/demo.c`
  + `example/perf.c` with `-DNANOVG_GLES2`, including `nanovg_gl.h` /
  `nanovg_gl_utils.h`. Fonts (Roboto) and test images are under `example/`.
- **Drop GLFW.** The stock `example_gles2.c` uses GLFW (desktop X11/Wayland); this
  board has no X. Replace only the ~40-line window/context bootstrap with a **DDK
  EGL null-window shim** modeled on `tools/sgx-window-swap.c` (which already drives
  a real EGL window surface on the `dc_nohw` swapchain with `eglSwapBuffers`):
  - `powervr.ini` must select `WindowSystem=libpvrPVR2D_FLIPWSEGL.so` (already set
    in the image). **FLIPWSEGL**, not FRONTWSEGL (FRONT OOMs on dc_nohw geometry).
  - **Request a stencil buffer** in the EGL config — NanoVG's crisp AA path needs
    it (`EGL_STENCIL_SIZE >= 8`); pass `NVG_STENCIL_STROKES`/`NVG_ANTIALIAS`.
  - Keep `demo.c` unchanged; loop `nvgBeginFrame → renderDemo → nvgEndFrame →
    eglSwapBuffers` — the swap goes through `dc_nohw` → `omapdrm_present` (vblank).
- **Launch:** run under the console-park/`vtrun` path (Stage 6) so `fbcon` yields.
- **Metrics:** `perf.c` gives an on-screen FPS + GPU-timer graph — use it for the
  Stage 7 pacing/latency/dropped-frame numbers.

### Pass criteria (Stage 7)

- The animated NanoVG demo renders on the panel through the in-kernel
  `omapdrm_present` path (`present=2`), no software fallback.
- Runs ≥ 30 min; memory flat; no `HWRecovery`/BIF/Oops in dmesg.
- Clean exit restores the console; `dcnohw` unloads.
- Frame pacing, SGX completion, and dropped frames recorded (via `perf.c`).

## Integration references

- `rootfs/build-devuan.sh` — Phase-2 chroot install + verify blocks (where the
  toolchain/dev deb and `sgx-cc` wrapper go).
- `sgx-ddk16/` — `make package` builds `sgx-ddk16-{um,tools,dev}_*_armel.deb` into
  `dist/`; `scripts/stage-dev.sh` defines the header/pkg-config layout.
- `tools/sgx-window-swap.c` — the EGL window-surface + `dc_nohw` swap reference for
  the GLFW replacement shim.
- `.github/workflows/build-devuan.yml` — where the DDK packages, kernel, and rootfs
  are assembled; `SGX_DDK16_DEB_DIR` already flows into the rootfs build.
