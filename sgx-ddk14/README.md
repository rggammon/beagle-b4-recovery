# SGX DDK 1.4 userspace packages

This directory builds Debian packages for the TI Graphics SDK `gfx_rel_es2.x`
DDK 1.4 userspace. Two builds are supported via `DDK_BUILD`:

| `DDK_BUILD`      | TI Graphics SDK | DDK version   | Installer                                        |
| ---------------- | --------------- | ------------- | ------------------------------------------------ |
| `2616` (default) | `4.00.00.01`    | `1.4.14.2616` | `Graphics_SDK_setuplinux_4_00_00_01.bin`         |
| `2514`           | `3.01.00.02`    | `1.4.14.2514` | `OMAP35x_Graphics_SDK_setuplinux_3_01_00_02.bin` |

Vendor binaries are downloaded or supplied at build time and are never stored in
this repository. The older `2514` installer pages its EULA and asks "Continue?"
twice, so its unpack step drives the prompts with `python3` + `python3-pexpect`
(`scripts/unpack-installjammer.py`); the `2616` installer uses a plain pipe.

The DDK 1.4 stack is the storm-free reference userspace for the SGX530 (see
`docs/ddk14-pivot-handoff.md`); this package mirrors the `sgx-ddk16` pipeline so
the two versions can be built side by side. The packages expose the vendor's
soft-float ABI to armel applications under an isolated `/opt/sgx-ddk14` prefix.
The matching hard-float ABI shim is `make hf-shim`: the 4.00.00.01 libraries keep
their DWARF, so the veneers are generated from 1.4 itself (byte-identical to the
1.6-generated ones). It also builds `libsgxvao.so`, which `sgx-ddk14-hf-run`
preloads (GL_OES_vertex_array_object stand-in + no-op `glDetachShader`, see
`tools/gles-oes-vao-stub.c`).

## Local audit

```sh
make audit DDK_BUILD=2514 \
  SDK_INSTALLER=/path/to/OMAP35x_Graphics_SDK_setuplinux_3_01_00_02.bin \
  SDK_SHA256=<verified-sha256> \
  ACCEPT_TI_EULA=yes
```

Alternatively, set `SDK_URL` instead of `SDK_INSTALLER`. A verified SHA-256 is
required in both modes. Outputs are confined to ignored `build/` and `dist/`
directories.

Pinned installer checksums (also encoded in the CI workflow):

- `2616`: `62383d15e33adf9349afba063b0f2405a15aa6c4b0b5579b0abdf81db7580df7`
- `2514`: `151d7ce8cbf192a3f0c7e232a5b958cde224309be9ec74ca40031be7d27fa265`

Run `make help` for all targets.

## Development files

`make dev-tree` stages the public EGL, GLES, GLES2, PVR2D, and WSEGL headers
supplied by TI and generates armel `egl.pc` and `glesv2.pc` metadata for the
private runtime. Kernel and Services internals under `GFX_Linux_KM` are
intentionally excluded from the application SDK.

`sgx-ddk14-um:armel` contains the private libraries and FLIP WSEGL module.
`sgx-ddk14-tools:armel` contains `pvrsrvinit`, diagnostics, and the
`sgx-ddk14-run` loader wrapper. `sgx-ddk14-dev` installs namespaced headers and
pkg-config files for applications compiled with the armel toolchain. The
packages `Conflicts`/`Replaces` their `sgx-ddk16` counterparts so only one DDK
userspace is installed at a time.

## Status

The Devuan image ships DDK 1.4 (kernel `pvrsrvkm`/`dcnohw` with `present=2`, these
packages, and the hard-float shim); `.github/workflows/build-devuan.yml` builds
the `2616` packages and shim inline. The SDK 4.00.00.01 libraries are
code-identical (`.text`/`.rodata`/`.data`) to the OpenPandora 1.4.14.2616 libraries
used for the board bring-up; TI's copies are just unstripped.

## Licensing

The historical OpenEmbedded recipe labels these binaries `proprietary-binary`
and accepts the installer EULA interactively. Downloading the installer during
CI does not necessarily grant permission to redistribute the resulting packages.
The GitHub workflow uploads ELF audit reports by default and requires a separate
explicit input before uploading `.deb` files.
