# SGX DDK 1.4 userspace packages

This directory builds Debian packages for the TI Graphics SDK `4.00.00.01`
`gfx_rel_es2.x` userspace (DDK `1.4.14.2616`). Vendor binaries are downloaded
or supplied at build time and are never stored in this repository.

The DDK 1.4 stack is the storm-free reference userspace for the SGX530 (see
`docs/ddk14-pivot-handoff.md`); this package mirrors the `sgx-ddk16` pipeline so
the two versions can be built side by side. The packages expose the vendor's
soft-float ABI to armel applications under an isolated `/opt/sgx-ddk14` prefix.
The matching hard-float ABI shim is produced by `sgx-ddk16`'s `make hf-shim-14`
(it reuses the 1.6-DWARF veneers against the 1.4 DDK).

## Local audit

```sh
make audit \
  SDK_INSTALLER=/path/to/Graphics_SDK_setuplinux_4_00_00_01.bin \
  SDK_SHA256=<verified-sha256> \
  ACCEPT_TI_EULA=yes
```

Alternatively, set `SDK_URL` instead of `SDK_INSTALLER`. A verified SHA-256 is
required in both modes. Outputs are confined to ignored `build/` and `dist/`
directories.

The workflow defaults to the official TI `4_00_00_01` URL and the SHA-256
`62383d15e33adf9349afba063b0f2405a15aa6c4b0b5579b0abdf81db7580df7`
(from the meta-openpandora `libgles-omap3_4.00.00.01.bb` recipe).

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

This package is Track B of the DDK 1.4 image move: the userspace is packaged and
CI-buildable now, but the Devuan image still ships the DDK 1.6 kernel and
userspace. Switching the image to 1.4 additionally requires the 1.4 kernel
`pvrsrvkm`/`dcnohw` (dc_nohw present port + teardown hardening), which is not yet
complete.

## Licensing

The historical OpenEmbedded recipe labels these binaries `proprietary-binary`
and accepts the installer EULA interactively. Downloading the installer during
CI does not necessarily grant permission to redistribute the resulting packages.
The GitHub workflow uploads ELF audit reports by default and requires a separate
explicit input before uploading `.deb` files.
