# Validating NanoVG hard-float through the SGX DDK 1.6 shim — handoff

Goal: prove a **prebuilt hard-float (armhf) NanoVG binary** renders on the
soft-float SGX530 DDK 1.6 through the ABI shim — i.e. a real, draw-heavy app
(gradients, rounded rects, AA strokes, stencil fills), not just the 64×64
`hf-shim-selftest`. This closes item **(b)** of the hard-float shim work.

## Current state (already shipped)

- The shim is baked into the Devuan image: `/opt/sgx-ddk16-hf/lib` (hard-float
  `libEGL.so`/`libGLESv2.so` veneers + `libsgxhf.so.1` + `libSGXm.so.1`) and
  `/opt/sgx-ddk16-hf/ddk` (the libSGXm-first patched DDK), plus the wrapper
  `/usr/bin/sgx-ddk16-hf-run`.
- Verified on-board: `sgx-ddk16-hf-run /root/hf-shim-selftest` → `RESULT: PASS`
  (`center 229 77 25`), deterministic. So EGL init, pbuffer, shader compile
  (float literals via the `libSGXm` `strtod` interposer), a float uniform, and a
  triangle all work through the shim.
- The **soft-float** NanoVG demo already runs on the board:
  `tools/nanovg-demo.c` + `memononen/nanovg` (pinned commit
  `ce3bf745eb2d2dbc14a50bf2446783f691ac4353`), built armel by the on-board
  Makefile at `/usr/src/nanovg-demo`. It draws into a `dc_nohw` EGL **window**
  (native handle 0, FLIPWSEGL, `EGL_STENCIL_SIZE 8`) and `eglSwapBuffers` each
  frame; a bare `./nanovg-demo 60` completes and prints `done` with no presenter.

## The one real pitfall: glibc version skew (do this first)

NanoVG (`nanovg.c` + the demo) calls libm directly — `sinf cosf sqrtf fmodf
atan2f` and `sin`. Those are the **app's own** libm calls in the base process; they
resolve to the board's **hard-float** libm, *not* through the shim (the shim only
covers GLES/EGL, and `libSGXm` lives inside the private dlmopen namespace).

That's ABI-fine (hard-float app + hard-float libm), but there's a **symbol-version**
trap: glibc 2.38 bumped `fmodf`/`fmod` to a new version node, so anything built
against glibc ≥ 2.38 references `fmodf@GLIBC_2.38`, which the board's **Devuan
daedalus glibc 2.36** does not provide → the binary fails to load. geoduck (Trixie,
glibc 2.41) and CI (Ubuntu 24.04, glibc 2.39) are both too new.

Pick one of:

1. **Robust — build against a glibc-2.36 armhf sysroot** (matches the board).
   The Devuan daedalus armhf rootfs *is* glibc 2.36; either build inside it or point
   the cross compiler at it with `--sysroot`. This is what a pipeline/`.deb` build
   should do.
2. **Quick spike — static libm** so no versioned `fmodf` reference survives:
   `-Wl,-Bstatic -lm -Wl,-Bdynamic`. Fine for a one-off validation; embeds the
   build host's libm code, self-contained at run time.

The GLES/EGL float boundary is *not* the problem here — see "ABI reality" below.

## Build (on geoduck)

geoduck has the hard-float toolchain (`arm-linux-gnueabihf-gcc-14`), the DDK
headers, and the built shim libs under `/mnt/scratch/geoduck-tmp/beagle/nanovg`:

- DDK GLES2/EGL headers: `sysroot/usr/include/sgx-ddk16`
- shim libs (link `-lEGL -lGLESv2` against these): `hfshim/lib`
- NanoVG sources: clone `memononen/nanovg` @ `ce3bf745…` and use its `src/nanovg.c`
  + headers; the harness is this repo's `tools/nanovg-demo.c`.

Quick-spike build (static libm to dodge the glibc skew):

```sh
cd /mnt/scratch/geoduck-tmp/beagle/nanovg
git clone https://github.com/memononen/nanovg nvg-src
git -C nvg-src checkout ce3bf745eb2d2dbc14a50bf2446783f691ac4353
# tools/nanovg-demo.c from the repo -> here as nanovg-demo.c
arm-linux-gnueabihf-gcc-14 -O2 -DNANOVG_GLES2_IMPLEMENTATION \
    -I nvg-src/src -I sysroot/usr/include/sgx-ddk16 \
    nanovg-demo.c nvg-src/src/nanovg.c \
    -L hfshim/lib -lEGL -lGLESv2 \
    -Wl,-Bstatic -lm -Wl,-Bdynamic \
    -Wl,-rpath-link,hfshim/lib \
    -o nanovg-demo-hf
# sanity: hard-float, and no glibc symbol newer than 2.36
readelf -A nanovg-demo-hf | grep -i VFP_args          # -> VFP registers
objdump -T nanovg-demo-hf | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail   # <= 2.34/2.36
```

Robust build (2.36 sysroot instead of static libm): replace the `-Wl,-Bstatic -lm
-Wl,-Bdynamic` with plain `-lm` and add `--sysroot=<daedalus-armhf-rootfs>` (and
`-Wl,-rpath-link,<sysroot>/lib/arm-linux-gnueabihf`). A daedalus armhf rootfs can be
made with `mmdebstrap daedalus <dir> "deb http://deb.devuan.org/merged daedalus main"`.

## Deploy + run (board)

```sh
scp nanovg-demo-hf root@192.168.50.245:/root/          # via the usual two-hop
ssh root@192.168.50.245
# headless smoke: dc_nohw window + eglSwapBuffers, no presenter needed
sgx-ddk16-hf-run /root/nanovg-demo-hf 300              # expect it to reach "done"
echo "exit=$?"
```

For an eyes-on check on the BTT-HDMI7 panel, drive it through the in-kernel paced
present (`present=2`) the same way the soft-float demo is shown — run it as the
child of `sgxmode`/park so `dc_nohw` presents each swap:

```sh
SGXMODE_PARK=1 /root/sgxmode -- sgx-ddk16-hf-run /root/nanovg-demo-hf 100000
```

(`sgxmode` and the present path are unchanged; the only new piece is that the child
is a hard-float binary launched via `sgx-ddk16-hf-run`.)

## Pass criteria

- Headless: completes the requested frames, prints `done`, `exit=0`, no segfault,
  no `FAIL:`; `glGetError` clean if you add a check.
- Visual: the animated scene (gradient panel, blue rounded card, pulsing amber
  circle, green AA sine curve) renders correctly on the panel — colors and
  antialiasing match the soft-float build. Any all-black / wrong-color / missing-AA
  output points at a float-boundary gap.
- Determinism: several runs identical; no new `HWRecovery`/BIF/Oops in `dmesg`.

## ABI reality — why NanoVG should "just work" through the shim

The shim only has to translate scalar **float/double passed in registers**. NanoVG's
heavy data crosses as **pointers**, which are ABI-neutral and need no veneer:

- Uniform uploads use `glUniform4fv` / `glUniform2fv` (a `const GLfloat *` + count) —
  pointer, not by-value floats.
- Geometry is a VBO: `glBufferData` + `glVertexAttribPointer` — pointer/int only.

The genuine float-register crossings NanoVG makes are few and already covered:
`glClearColor`, `glBlendColor`, `glUniform1f`/`4f` by value, `glLineWidth` — all have
`pcs("aapcs")` veneers (16 float-touching GLESv2 exports). Shader **float literals**
(NanoVG's fill/AA shaders) are parsed by the DDK compiler via `strtod`, handled by
the versioned `libSGXm` interposer. If NanoVG's GLES2 backend fetches an extension
(e.g. a `*OES` entry) via `eglGetProcAddress`, the shim's `eglGetProcAddress` returns
its own veneer (not a raw DDK pointer), so that path is covered too.

## If it fails — triage

- **Won't load / `fmodf` version error** → glibc skew; rebuild per "the one real
  pitfall" (static libm or 2.36 sysroot).
- **Segfault in `ld-linux-armhf.so.3` at startup** → a corrupt patched DDK lib
  (`.dynamic` outside `PT_LOAD`); check
  `readelf -l /opt/sgx-ddk16-hf/ddk/libGLESv2.so | grep -A1 DYNAMIC` — the offset
  must fall inside a `PT_LOAD`. The CI build asserts this now (patchelf ≥ 0.19).
- **Renders but wrong colors / no AA** → a float-touching GL entry missing a veneer,
  or a shader-literal path not routing through `libSGXm`. Trace with
  `LIBSGXM_TRACE=1 sgx-ddk16-hf-run … 2>&1 | grep 'libSGXm: wrap'` and compare the
  bad pixel to the soft-float build.
- gdb is installed on the board (`gdb 13.1`); `sgx-ddk16-hf-run gdb --args
  /root/nanovg-demo-hf 60` then `run` / `bt` for a backtrace. The DDK libs carry
  DWARF (real file:line).

## Folding into the image / CI (optional, after it's proven)

- Add a hard-float NanoVG build step to `build-devuan.yml` that compiles against the
  daedalus armhf rootfs (glibc 2.36 — no skew), and drop `nanovg-demo-hf` into the
  image next to the soft-float one as a demo of the shim.
- Or package the shim + a hard-float sample as `libsgx-ddk16-hf-shim` following the
  `sgx-ddk16` debian packaging pattern.

## References

- `tools/hf-shim-selftest.c` — the minimal normally-linked hard-float proof.
- `tools/build-hf-shim.sh`, `tools/gen-hf-shim.py`, `tools/hf-shim-loader.c` — how
  the shim is generated/built.
- `tools/nanovg-demo.c` — the scene + EGL window bootstrap (shared with soft-float).
- `docs/nanovg-onboard-compile-handoff.md` — the soft-float on-board build story.
