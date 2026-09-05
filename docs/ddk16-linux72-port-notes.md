# DDK 1.6 Linux 7.2 Port Notes

## Purpose

This document records the completed compatibility work and build-configuration
audit for TI DDK `1.6.16.3977`. The active Stage 1-8 execution checklist lives
in [the presentation plan](ddk16-dcnohw-presentation-plan.md).

## Maintained Source

- Worktree: `/mnt/scratch/geoduck-tmp/beagle/openpvrsgx-ddk16`
- Branch: `users/rgammon/pvrsgx-1.6.16.3977`
- DDK directory: `drivers/gpu/drm/pvrsgx/1.6.16.3977`
- Kernel tree: `/mnt/scratch/geoduck-tmp/beagle/openpvrsgx-src`
- Target platform: OMAP3, SGX530, `SGX_CORE_REV=103`
- Services checkpoint: `91f00a69f`
- `dc_nohw` checkpoint: `9858b321a`

Exact module build:

```sh
make -C /mnt/scratch/geoduck-tmp/beagle/openpvrsgx-src \
  ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- \
  LOCALVERSION=-g2342ce92fdde-dirty \
  TI_PLATFORM=omap3 \
  M=/mnt/scratch/geoduck-tmp/beagle/openpvrsgx-ddk16/drivers/gpu/drm/pvrsgx/1.6.16.3977 \
  modules
```

The integrated backend build can produce both Services and `dc_nohw`:

```sh
make -C /mnt/scratch/geoduck-tmp/beagle/openpvrsgx-ddk16 \
  ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- \
  LOCALVERSION=-g2342ce92fdde-dirty \
  M=drivers/gpu/drm/pvrsgx \
  CONFIG_SGX=m CONFIG_SGX_OMAP=m \
  CONFIG_PVRSGX_1_6_16_3977=y \
  CONFIG_PVRSGX_1_6_16_3977_DC_NOHW=m \
  modules
```

Expected vermagic:

```text
7.2.0-g2342ce92fdde-dirty SMP mod_unload modversions ARMv6 p2v8
```

## Linux 7.2 Compatibility Work

The DDK 1.6 port follows the same broad migration pattern as DDK 1.4, but was
implemented incrementally against its own source:

- Added Kconfig and wrapper-Makefile routing for DDK `1.6.16.3977`.
- Added an OMAP3/SGX103-only Kbuild.
- Replaced obsolete architecture and standard-library kernel includes.
- Updated interrupt-context, semaphore, timer, VM flag, GUP, mmap, page, class,
  remove-callback, and string APIs.
- Removed the private page-table walker in favor of the validated PFNMAP path.
- Replaced obsolete procfs implementation with a no-diagnostics stub.
- Retained the conservative ARM full-cache fallback used by the DDK 1.4 port.
- Converted OMAP3 power locking to a mutex-compatible implementation.
- Removed obsolete `clk_set_parent(sgx_fck, core_ck)`; the modern clock provider
  already owns the parent relationship.
- Added `FOP_UNSIGNED_OFFSET` so high-bit legacy mmap handles are not rejected
  with `EOVERFLOW`.
- Renamed the local module-name macro to `PVR_DDK_MODNAME` to avoid collision
  with modern kernel `MODULE_INFO` machinery.
- Added explicit `-DMODULE` where recursive Kbuild otherwise omitted module
  metadata.

## Build-Configuration Evidence

The replacement Kbuild was checked against:

1. Defaults in TI's DDK `1.6.16.3977` Makefile.
2. Compiler flags embedded in `gfx_rel_es2.x` userspace.
3. A preserved generated OMAP3 kernel command from the SDK's SGX121 build.

No generated SGX103 kernel command survived. The SGX121 command is used only
as evidence for platform-independent Services and OMAP3 glue options. The ES2
binary metadata supplies SGX103-specific userspace and uKernel evidence.

### Matched Kernel Options

| Definition                           | Final state | Basis                                                 |
| ------------------------------------ | ----------- | ----------------------------------------------------- |
| `SUPPORT_SGX_NEW_STATUS_VALS`        | enabled     | Makefile default, ES2 userspace, preserved KM command |
| `PVR_LINUX_TIMERS_USING_WORKQUEUES`  | enabled     | Original workqueue expansion and preserved KM command |
| `SYS_CUSTOM_POWERLOCK_WRAP`          | enabled     | Original workqueue expansion and preserved KM command |
| `DISABLE_SGX_PB_GROW_SHRINK`         | enabled     | Makefile default, ES2 userspace, preserved KM command |
| `SUPPORT_PERCONTEXT_PB`              | enabled     | ES2 userspace and preserved KM command                |
| `TRANSFER_QUEUE`                     | enabled     | ES2 userspace and preserved KM command                |
| `SUPPORT_SGX_EVENT_OBJECT`           | enabled     | ES2 userspace and preserved KM command                |
| `SUPPORT_ACTIVE_POWER_MANAGEMENT`    | enabled     | ES2 userspace and preserved KM command                |
| `SUPPORT_HW_RECOVERY`                | enabled     | ES2 userspace and preserved KM command                |
| `SUPPORT_SGX_HWPERF`                 | enabled     | ES2 userspace and preserved KM command                |
| `SUPPORT_SGX_LOW_LATENCY_SCHEDULING` | enabled     | ES2 userspace and preserved KM command                |
| `PVR_SECURE_HANDLES`                 | enabled     | ES2 userspace and preserved KM command                |
| `PVR_SECURE_FD_EXPORT`               | enabled     | ES2 userspace and preserved KM command                |
| `SYS_USING_INTERRUPTS`               | enabled     | ES2 userspace and preserved KM command                |

Omitting `SUPPORT_SGX_NEW_STATUS_VALS` caused a deterministic SGX build-option
mismatch with client-only bit `0x1`. Restoring it allowed release
`pvrsrvinit` to complete.

### Static SGX Timing

`SGX_DYNAMIC_TIMING_INFO` is not defined in either the preserved TI OMAP3
kernel command or the final port.

With the macro absent:

- `SGX_DEVICE_MAP` contains `sTimingInfo`.
- `SysInitialise()` fills it from static OMAP3 constants.
- Core clock, recovery frequency, uKernel frequency, active-PM enablement, and
  active-power latency are fixed during initialization.

Defining the macro removes that structure member and uses
`SysGetSGXTimingInformation()` after clock changes. The `#error` in
`sysutils_linux.c` applies only when the obsolete `CONSTRAINT_NOTIFICATIONS`
path is enabled. That path is disabled in both tested configurations.

Enabling dynamic timing would change a shared structure and runtime policy
without evidence that TI's released OMAP3 kernel used it, so the port retains
the static configuration.

### Intentional Differences

The final Kbuild does not reproduce every textual definition from TI's command:

- `SUPPORT_OMAP3430_OMAPFB3`, `PVR_PDP_LINUX_FB`, `SUPPORT_XWS`, and
  `PVR_HAS_BROKEN_OMAPFB_H` belong to the obsolete OMAPFB/XWS presentation
  path. The current design uses `dc_nohw` and does not build `omaplfb`.
- `SUPPORT_LINUX_X86_PAT` and `SUPPORT_LINUX_X86_WRITECOMBINE` are irrelevant
  on ARM.
- EGL, shader, OpenVG, WSEGL, pthread, and build-identification definitions are
  userspace-only or metadata.
- `_POSIX_C_SOURCE` is not needed by the kernel build.
- `PVRSRV_MODNAME` became `PVR_DDK_MODNAME` because the original name collides
  with modern module metadata.
- `MODULE`, `AUTOCONF_INCLUDED`, and `PVR_PROC_USE_SEQ_FILE` are modern build
  or compatibility definitions added by the port.

There are no known unresolved SGX ABI-option discrepancies in the release
configuration. Debug userspace has a deliberately different debug option mask
and is diagnostic-only.

## DDK 1.8 dc_nohw Adaptation

The first recovered source containing `dc_nohw` is DDK `1.8.869593`. DDK 1.5
contains Intel EMGD display-class code, and DDK 1.7 contains an MRST framebuffer
driver.

The DDK 1.8 source was adapted to DDK 1.6 by:

- Keeping the DDK 1.6 `pfnGetBufferAddr` tiling-stride argument.
- Retaining the DDK 1.8 DisplayClass operations supported by DDK 1.6.
- Using the Linux 7.2-compatible vmalloc API.
- Replacing `strncpy` with `strscpy`.
- Removing obsolete `MODULE_SUPPORTED_DEVICE` use.
- Adding current module metadata and explicit `-DMODULE` for recursive Kbuild.
- Configuring linear 1024x600x32 buffers for B4 validation.

The provider builds as `dcnohw.ko`, depends on `pvrsrvkm`, registers
successfully, and is required because this DDK initializes its default WSEGL
module even for pbuffer EGL tests.

## Root Causes Found During Hardware Bring-Up

1. Platform probe failed because legacy code attempted
   `clk_set_parent(sgx_fck, core_ck)`. The resulting cleanup error surfaced as
   failed per-process/hash allocation and misleading `ENOMEM` behavior.
2. Temporary bridge diagnostics inserted between unbraced `if` statements and
   their bodies made bridge failure paths unconditional. This was a diagnostic
   regression, not an ABI mismatch.
3. Legacy mmap handles were rejected with `EOVERFLOW` until
   `FOP_UNSIGNED_OFFSET` was set.
4. The missing `SUPPORT_SGX_NEW_STATUS_VALS` definition produced SGX option
   mismatch bit `0x1`.
5. EGL pbuffer initialization required a registered DisplayClass provider;
   loading the adapted `dc_nohw` resolved `EGL_NOT_INITIALIZED`.

All temporary diagnostics were removed after these fixes.

## Validation

On the BeagleBoard B4:

- Release `pvrsrvinit` exits zero.
- IRQ 21 registers as `SGX ISR`.
- `dcnohw.ko` loads and registers.
- The alternating-FBO test completes 120 frames.
- Post-warm-up median total time is 0.204 ms on both targets.
- No frame exceeds 500 ms.
- No build-option mismatch, BIF fault, watchdog recovery, Oops, or BUG occurs.

These results describe the original 120-frame baseline. A stricter pre-Stage-1
test subsequently ran five clean module lifecycle cycles and then attempted a
three-minute soak as repeated 50,000-frame probe processes. The lifecycle
cycles passed and their memory counters stabilized. The first soak process
completed 50,000 frames without recovery. During the second process, however,
the driver triggered three `HWRecoveryResetSGX` events. The final recovery
reported `EUR_CR_BIF_FAULT=0x0F0AB000`; two frames exceeded 500 ms and the
maximum frame time was 1.037 seconds.

Cleanup after this failure left `pvrsrvkm` displayed by `lsmod` with use count
`-1`. This is the kernel's marker for a module already in the unloading state,
not a negative reference count. The `rmmod pvrsrvkm` process was stuck, but the
B4 remained responsive over SSH. A software reboot was used to clear this
first stuck module-unload state; no power cycle was required.

After that reboot, a duration-capable probe with explicit EGL/GL teardown
ran as one process for the full 180 seconds and completed 99,050 frames. Its
summary reported two frames over 500 ms and a 1.135-second maximum. File and
kernel timestamps show that the first recovery began about 0.34 seconds after
the summary was written, during process teardown rather than before the render
loop completed. Module exit again remained stuck in the unloading state, and a
second recovery followed while the driver was in that state.

A non-destructive SysRq task dump identified the deadlock. `rmmod` held the
PowerVR power mutex while `SGXPrePowerState()` called
`OSDisableTimer()` and waited in `flush_workqueue()`. The in-flight
`pvr_timer` recovery worker was simultaneously blocked in
`SysPowerLockWrap()` waiting for the same mutex. Thus module unload waited for
the timer worker while the timer worker waited for module unload's lock. The
board itself was not hung; only these teardown participants were blocked.

Commit `e2b9d0eea` fixes the lock inversion. Timer work is submitted with
`ISR_ID`, but moving timer callbacks to the `pvr_timer` workqueue made
`in_interrupt()` false and caused the custom OMAP mutex wrapper to block before
the ISR-aware Services resource lock could return `PVRSRV_ERROR_RETRY`. The fix
bypasses the custom mutex for `ISR_ID` while retaining the Services resource
lock and the mutex for process callers.

A one-cycle, 10-second control passed and unloaded both modules. A subsequent
five-cycle, 180-second run also returned normally and unloaded both modules,
confirming the deadlock fix. The extended Stage 0 gate is still not passed: the
180-second process completed 97,768 frames but reported two frames over 500 ms,
a 1.134-second maximum, and two teardown-time hardware recoveries. Stage 1
remains blocked on those recovery and latency failures.

## Related Documentation

- [DDK 1.6 presentation plan](ddk16-dcnohw-presentation-plan.md)
- [SGX103 DDK recovery history](sgx103-ddk-recovery-history.md)
- [BTT HDMI7 and OMAP DRM notes](btt-hdmi7-omapdrm.md)
