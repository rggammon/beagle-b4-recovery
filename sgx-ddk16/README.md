# SGX DDK 1.6 userspace packages

This directory builds Debian packages for the TI Graphics SDK `4.03.00.02`
`gfx_rel_es2.x` userspace (DDK `1.6.16.3977`). Vendor binaries are downloaded
or supplied at build time and are never stored in this repository.

The pipeline implements installer acquisition, checksum verification,
EULA-gated extraction, ELF and header inventory, and package staging. The
initial packages expose the vendor's soft-float ABI to armel applications under
an isolated `/opt/sgx-ddk16` prefix. Native armhf EGL/GLES shims remain a future
compatibility layer.

## Local audit

```sh
make audit \
  SDK_INSTALLER=/path/to/Graphics_SDK_setuplinux_4_03_00_02.bin \
  SDK_SHA256=<verified-sha256> \
  ACCEPT_TI_EULA=yes
```

Alternatively, set `SDK_URL` instead of `SDK_INSTALLER`. A verified SHA-256 is
required in both modes. Outputs are confined to ignored `build/` and `dist/`
directories.

The workflow defaults to the official TI `4_03_00_02` URL and the SHA-256
`cdb0bd3964e107733d632aa8224e0537b05c1ffac34befc036423458c8d75255`,
confirmed by the initial checksum-discovery run.

Run `make help` for all targets.

## Development files

`make dev-tree` follows the historical OpenPandora and Angstrom recipes by
staging the public EGL, GLES, GLES2, PVR2D, and WSEGL headers supplied by TI.
It generates armel `egl.pc` and `glesv2.pc` metadata for the private runtime.
Kernel and Services internals under `GFX_Linux_KM/include4` and
`GFX_Linux_KM/services4` are intentionally excluded from the application SDK.

`sgx-ddk16-um:armel` contains the private libraries and FLIP WSEGL module.
`sgx-ddk16-tools:armel` contains `pvrsrvinit`, diagnostics, and the
`sgx-ddk16-run` loader wrapper. `sgx-ddk16-dev` installs namespaced headers and
pkg-config files for applications compiled with the armel toolchain.

## Licensing

The historical OpenEmbedded recipe labels these binaries
`proprietary-binary` and accepts the installer EULA interactively. Downloading
the installer during CI does not necessarily grant permission to redistribute
the resulting packages. The GitHub workflow therefore uploads ELF audit reports
by default and requires a separate explicit input before uploading `.deb` files.
