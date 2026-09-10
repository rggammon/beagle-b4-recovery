# Handoff — package the DDK 1.6 SGX530 GLES userspace as a `.deb`

Scope for a dedicated chat: replace the mismatched **ti343x** GLES userspace in the
Devuan image with a **matched DDK 1.6 (`1.6.16.3977`)** userspace, packaged as a
`.deb`. This doc is a map of *where the 1.17 userspace is installed today* and *where
the 1.6 userspace lives* — not a build recipe.

## Why (the mismatch)

The Devuan GPU image is **mid-migration**:

- **Kernel** is now DDK **1.6** — [kernel/config-devuan](../kernel/config-devuan)
  sets `CONFIG_PVRSGX_1_6_16_3977=y` (`# CONFIG_PVRSGX_1_17_4948957 is not set`) +
  `..._DC_NOHW=m`. The 1.6 build compiles its own core-matched ukernel, so
  `SGXDevInitCompatCheck()` passes with **no** core-revision exception patch (why
  `0005` was removed).
- **Userspace** is still the maemo-leste **ti343x** package (DDK 1.17-era, `1.2.1`
  ukernel), installed by [rootfs/build-devuan.sh](../rootfs/build-devuan.sh).

A 1.6 kernel module + a 1.17-era ukernel **fails the DDK-version check** in
`SGXDevInitCompatCheck` (before core-rev even matters), so the image's *default* GLES
userspace does not init against the 1.6 kernel. Stages 0–6 were validated with a
**hand-placed softfp 1.6 bundle** (see below), not the image default.

## Where the 1.17 userspace is installed today (the reference model)

All in [rootfs/build-devuan.sh](../rootfs/build-devuan.sh), Phase 2:

| What | Location |
| --- | --- |
| maemo-leste repo | [line 73](../rootfs/build-devuan.sh#L73): `deb https://maedevu.maemo.org/leste daedalus main` → `maemo.list` |
| repo keys | [rootfs/keys-devuan/](../rootfs/keys-devuan/) `maemo-main-repo-key.asc`, `maemo-extras-key.asc` (dearmored into `trusted.gpg.d/`) |
| the GLES userspace | [line 89](../rootfs/build-devuan.sh#L89): `apt install sgx-ddk-um-ti343x sgx-ddk-um-tools libgles2-mesa … kmscube drm-info` |
| OpenRC postinst shim | ~line 82: `rc-update` stubbed (image is sysvinit, not OpenRC) |
| service hook | [line 164](../rootfs/build-devuan.sh#L164): SysV `/etc/init.d/powervr` → `modprobe pvrsrvkm; modprobe dcnohw; /usr/bin/pvrsrvinit` |
| Mesa `omapdrm` DRI alias | [line 124](../rootfs/build-devuan.sh#L124)–141: builds `omapdrm_dri.so` from maemo `mesa_22.3.6+sgx2` (`-Dgallium-drivers=sgx -Dgallium-sgx-alias=omapdrm`) |

What the two packages provide:

- **`sgx-ddk-um-ti343x`** — closed GLES/EGL userspace (`libGLESv2`, `libEGL`,
  `libsrv_um`, WSEGL) + the SGX **ukernel** blob. **armhf / hard-float**, DDK 1.17-era.
- **`sgx-ddk-um-tools`** — `/usr/bin/pvrsrvinit` + tools.

This maemo-leste deb is the **layout template** for the target: it shows the expected
contents (GLES/EGL + ukernel + `pvrsrvinit` + WSEGL) and the init hook. It installs
cleanly *because it is armhf/hardfp* — matching the Devuan armhf rootfs.

## Where to find the 1.6 userspace

**Source:** TI Graphics SDK **`4.03.00.02`**, the **`gfx_rel_es2.x`** runtime —
soft-float es2.x ABI, DDK **`1.6.16.3977`** (matches the kernel module). Not a Pandora
build. See [docs/ddk16-dcnohw-presentation-plan.md](ddk16-dcnohw-presentation-plan.md)
"Target Runtime".

**Staging paths used during Stages 0–6** (dev host / board):

| Path | Contents |
| --- | --- |
| `/opt/ti-ddk16/runtime` | release runtime: `libGLESv2`, `libEGL`, `libsrv_um`, WSEGL (`libpvrPVR2D_FLIPWSEGL.so`), `pvrsrvinit`, ukernel |
| `/opt/ti-ddk16/lib` | legacy init libc bundle (`ld-linux.so.3`) — used **only** to run `pvrsrvinit` |
| `/opt/ti-ddk16/module/{pvrsrvkm.ko,dcnohw.ko}` | prebuilt modules — **do not ship**; use the kernel-tree modules from [kernel/build-devuan.sh](../kernel/build-devuan.sh) (correct vermagic) |
| `/root/s16/gl` | `soak16` bundle's `gl/` dir: es2.x softfp GL libs **+ the 1.6 WSEGL** (`GL_ROOT` default in tools) |
| `/opt/pandora-armel` | modern armel softfp libc (glibc 2.34+) — runs cross-built **probes** only; **not** part of the shipping userspace |

**Archive tarballs** (contain the runtime + WSEGL):

- `beagle-archive/angstrom-sgx-test.tar` → `opt/ti-ddk16/runtime/libpvrPVR2D_*WSEGL.so`,
  `libEGL.so` (byte-identical to `soak16`'s).
- The native-Angstrom control bundle consumed by
  [tools/run-angstrom-sgx-test.sh](../tools/run-angstrom-sgx-test.sh) (expects
  `opt/ti-ddk16/{runtime,lib}` + `opt/pandora-armel` beside it).

Runtime-layout reference: [tools/README.md](../tools/README.md) — "Runtime layout the
harness expects".

## Constraints the `.deb` must satisfy

1. **Version match** — userspace DDK must be exactly **`1.6.16.3977`**. Any other DDK
   version/build is rejected by `SGXDevInitCompatCheck` before corerev matters. The
   matched ukernel then passes the core-rev check with no exception patch.
2. **Soft-float on an armhf image** — the es2.x 1.6 userspace is **softfp EABI5**; the
   Devuan image is armhf/hardfp. So it is **not** a plain `apt install` into
   `/usr/lib/arm-linux-gnueabihf`. Ship a softfp loader + libc bundle (like
   `/opt/ti-ddk16/lib`) and run GL clients via **mixed-libc loader isolation** (as
   Stages 0–6 did) under an isolated prefix (e.g. `/opt/sgx-ddk16`) with wrapper
   scripts. **This packaging model is the main decision for the deb chat.**
3. **WSEGL** — ship `libpvrPVR2D_FLIPWSEGL.so` (`1.6.16.3977` softfp) and write
   `/etc/powervr.ini` `WindowSystem=libpvrPVR2D_FLIPWSEGL.so`. Use **FLIPWSEGL**, not
   FRONTWSEGL (FRONT sizes a front buffer from `dc_nohw`'s bogus geometry → immediate
   OOM). The 1.4 WSEGL and `gfx_rel_es{3,5,6,8}.x` WSEGL do **not** work
   (version / hard-float mismatch). See
   [docs/sgx-ddk16-stall-followups.md](sgx-ddk16-stall-followups.md).
4. **`pvrsrvinit`** — must run after every fresh module load, under the DDK's legacy
   loader (`/opt/ti-ddk16/lib/ld-linux.so.3 --library-path runtime:lib …/pvrsrvinit`).
   Update `/etc/init.d/powervr` ([line 164](../rootfs/build-devuan.sh#L164)) for the
   new layout.
5. **Mesa `omapdrm` alias + kmscube** — re-evaluate. The `omapdrm_dri.so` alias +
   newer kmscube serve the Mesa/GBM-KMS EGL path; the Stage 6 game path uses the
   in-kernel `dc_nohw` present (`present=2`) instead, so they may be unneeded for that
   path.

## What to change in `rootfs/build-devuan.sh` (once the deb exists)

- Drop the maemo repo add ([line 73](../rootfs/build-devuan.sh#L73)) and
  `sgx-ddk-um-ti343x` / `sgx-ddk-um-tools` from the apt install
  ([line 89](../rootfs/build-devuan.sh#L89)); keep mesa/kmscube only if still needed.
- Install the new 1.6 userspace `.deb` (from a local pool or built in-tree).
- Point `/etc/init.d/powervr` `pvrsrvinit` at the new (loader-isolated) layout.
- Update the header comment ([lines 9–12](../rootfs/build-devuan.sh#L9)).
- `rootfs/keys-devuan/maemo-*` keys can be dropped if the maemo repo is gone.

## Acceptance

- Boot: `/etc/init.d/powervr` loads `pvrsrvkm` + `dcnohw`, runs `pvrsrvinit`, dmesg
  clean (no `DDK_VERSION_MISMATCH` / `BUILD_MISMATCH`).
- `/root/gpu-test.sh` or [tools/test-sgx-ddk16.sh](../tools/test-sgx-ddk16.sh):
  `GL_RENDERER = "PowerVR SGX 530"`, correct pixel read-back.
- The Stage 6 present path (`dcnohw present=2`) still drives the panel.
