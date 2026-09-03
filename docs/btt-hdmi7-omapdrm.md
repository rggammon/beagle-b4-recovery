# BTT HDMI7 on OMAP3 DRM

This tracks display bring-up for the BIGTREETECH HDMI7 V1.2 (1024x600) on the
original BeagleBoard B4. It is separate from SGX rendering: `pvrsrvkm` renders on
one DRM device, while `omapdrm` owns the display controller and DVI scanout.

## Hardware and DRM path

The display path is:

```text
OMAP3530 DISPC/DPI -> TFP410 DVI transmitter -> HDMI cable -> BTT HDMI7 V1.2
```

Linux exposes two DRM devices in the Devuan GPU image:

- `/dev/dri/card0`: PowerVR SGX (`pvrsrvkm`), render-oriented and without KMS
  resources.
- `/dev/dri/card1`: OMAP DRM (`omapdrm`), with the DVI connector, CRTCs, planes,
  dumb buffers, and PRIME import/export.

The BTT display does not provide usable EDID over this board's DVI DDC path.
Without a forced mode, OMAP DRM has no mode to drive.

## Failed mode

The first Devuan image used:

```text
video=DVI-D-1:1024x600@60e
```

The `e` flag correctly force-enabled the DVI connector, but Linux generated a
non-reduced 1024x600 CVT timing:

```text
pixel clock 48.924 MHz
horizontal: 1024 1064 1168 1312
vertical:   600  601  604  622
```

This has a 104-pixel horizontal sync pulse (`1168 - 1064`). OMAP3 DISPC can
encode at most a 64-pixel sync pulse. DRM therefore pruned the mode as
`MODE_BAD`. Enabling DRM debug also showed all fallback DVI modes rejected;
their horizontal sync pulses were likewise over the OMAP3 limit.

The relevant validation path is:

- `drivers/gpu/drm/omapdrm/omap_crtc.c`: `omap_crtc_mode_valid()`
- `drivers/gpu/drm/omapdrm/dss/dispc.c`: `dispc_mgr_check_timings()` and
  `_dispc_lcd_timings_ok()`

The result was a connected DVI connector with no active CRTC, and `kmscube`
failed with `could not find mode`.

## Current solution

The staged boot argument is:

```text
video=DVI-D-1:1024x600MR@60e
```

`M` explicitly requests VESA CVT calculation and `R` requests reduced blanking.
CVT reduced blanking uses a 32-pixel horizontal sync pulse, within OMAP3's
64-pixel limit. The setting is maintained in
`flash/uEnv-sdcard-devuan.txt` and copied to `uEnv.txt` in the FAT boot
partition.

This mode is hardware-validated. OMAP DRM reports a connected 1024x600
connector and fbcon switches to a 128x37 framebuffer console. Verify with:

```sh
dmesg | grep -iE 'forcing DVI|mode not supported|Cannot find'
cat /sys/class/drm/card1-DVI-D-1/status
cat /sys/class/drm/card1-DVI-D-1/modes
drm_info | sed -n '/DVI-D/,/Encoders/p'
```

Expected results are `connected`, a `1024x600` mode, no `MODE_BAD` message for
that mode, and an active CRTC.

For detailed mode-pruning diagnostics:

```sh
echo 0x1ff > /sys/module/drm/parameters/debug
kmscube -D /dev/dri/card1
dmesg | tail -100
```

## SGX rendering on HDMI

Hardware-accelerated rendering is visible on the panel. The working path uses
OMAP DRM for KMS and an `omapdrm_dri.so` alias for Mesa's Series5 SGX driver:

```sh
kmscube -D /dev/dri/by-path/platform-omapdrm.0-card -v 1024x600
```

Maemo-Leste's Mesa source supports `-Dgallium-sgx-alias=omapdrm`, but its
Daedalus binary package does not enable it. The image builds and installs that
single alias from the checksum-pinned matching Mesa source.

The image reserves `cma=48M`: the proprietary display path requests a
contiguous 4,800-page (18.75 MiB) buffer pool, in addition to OMAP framebuffer
allocations. A 16 MiB CMA pool cannot satisfy it, and 32 MiB was fragile after
other allocations. CMA remains available for movable pages while idle.

Daedalus' packaged `kmscube` incorrectly passes `DRM_FORMAT_MOD_INVALID` to
OMAP DRM. The image builds pinned upstream commit
`f60e50e887d3c49e91ac9b06d8199b36152632fa`, whose framebuffer path correctly
falls back to unmodified `drmModeAddFB2`.

Measured results:

- Surfaceless SGX clear/readback: correct `PowerVR SGX 530` output.
- 1024x600 synchronized offscreen clears: about 142.5 fps.
- Windowed HDMI cube: about 2.3 fps, dominated by the legacy display
   swapchain/synchronization path rather than SGX fill rate.
- Continuous rendering is stable. Mesa/DDK process teardown can trigger SGX
   watchdog recovery; the GPU recovers and remains usable.

If CVT reduced blanking is rejected or the panel does not lock to it, the next
fallback is a fixed `panel-dpi` timing in the Beagle DT or a captured custom EDID
blob. Do not return to non-reduced CVT; its sync width is known to exceed the
controller limit.
