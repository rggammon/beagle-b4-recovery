# Handoff: bring DDK 1.4 forward as the ship stack (reproduce Stages 1–8 on 1.4)

Start-here card for a fresh chat. The full plan and rationale live in
[the presentation plan](ddk16-dcnohw-presentation-plan.md) (see its **Plan
Pivot** section) and [later stages 6–8](ddk16-presentation-stages-6-8.md).

## Mission

The appliance ship target moved from DDK `1.6.16.3977` to DDK `1.4.14.2616`. The
completed **1.6 `dc_nohw`/`omapdrm` presentation stack (Stages 0–6) is the
reference implementation**; reproduce it on the 1.4 Services, then validate the
appliance UI (**Slint** + the screen-saver soak app) and package. Keep 1.6
installed as the diagnostic A/B control.

## Why we pivoted (one paragraph)

On the **same board / same kernel / same SGX103 silicon / same 1024×600 pbuffer /
same binary**, only the DDK differs — and DDK 1.4 is storm-free where 1.6 storms,
and 2–3× faster (1.6 pbuffer FULL 24t/150f = 20 `HWRecoveryResetSGX`; 1.4 = 0).
This is the **second** root-caused DDK-1.6-specific ukernel regression on this
core (the first: the depth-clear storm = Chromium `gl_clear_broken` id 95). The
only confirmed SGX103 devices (classic BeagleBoard, Pandora Classic) both shipped
1.4; 1.6/1.7 was validated on the newer DM3730/rev-1.2.5 core. Stage 0 perf is
identical (0.205 vs 0.204 ms), and the 1.4 userspace is the exact OpenPandora
soft-float runtime for this silicon. Full evidence in
[`/memories/repo/beagle-b4-native-control.md`](../) and the plan's Plan Pivot.

> **Note (2026‑09‑27):** the _ukernel‑build_ A/B for the **12‑tile FLIP** hang is
> **refuted** — matched 12‑sample clean‑boot testing gives ~42 % hang for **both** 2514 and
> 2616 (see [sgx530-12tile-fringe-flip-results.md](sgx530-12tile-fringe-flip-results.md)).
> That does **not** undermine the pivot: the pivot rests on the **offscreen pbuffer** A/B,
> which was **re‑verified** on 2026‑09‑27 under the same clean‑boot protocol (6 samples/arm)
> and **holds rock‑solid** — 1.6 pbuffer 24t/150f = **20 HWR every run**, 1.4‑graft = **0 HWR
> every run**, deterministic. Two distinct phenomena were being conflated: the FLIP hang is a
> flaky, ukernel‑independent B4 present‑path issue; the pbuffer storm is a deterministic
> **DDK‑version** (1.6 vs 1.4) difference that appears **offscreen** (no present at all).

> **Update (2026‑09‑28): the 1.6 offscreen failure reproduces off B4, so 1.4 remains the
> direction.** Offscreen pbuffer `24 1 1 1 12 150`:
>
> | Platform / stack                                                                                         | 1.6.16.3977                                                                                                                   | 1.4 (same board)       |
> | -------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------- | ---------------------- |
> | **B4**, kernel 7.2, our 1.6 KM port                                                                      | 20 HWR/run; **`BIF_INT_STAT=0`, `BIF_FAULT=0` on all 20** (no page fault; the page‑table walk found nothing to walk)          | 0 HWR                  |
> | **Pandora**, SuperZaxxon kernel 3.2.84, its own 1.6.16.3977 KM + **unmodified** TI SDK 4.03.00.02 `gfx_rel_es2.x` | **6/6 failed** (none finished 150 frames in 120 s; 0–2 HWR each; page faults `BIF_INT_STAT=0x4002` at `0x0F0AB000`, `0x08811000`) | 3/3 clean (default KM) |
>
> Two boards, two kernels, two independently built 1.6 KMs, and unmodified TI userspace all
> fail. The symptom differs (pure hang on B4, page fault on Pandora), but both fail where 1.4
> is clean. 1.6 already flushes CPU caches on every write‑combined/uncached vmalloc
> allocation, so it isn't the 1.4 cache bug below. The cause is most likely in closed 1.6
> components (userspace or ukernel); not pursued further.
>
> **1.4 FLIP hang (the remaining 1.4 issue):** root‑caused to missing CPU cache maintenance on
> newly allocated GPU memory, and fixed with a port of Nokia's N9 driver fix
> (`inv_cache_mem_area`): 12/12 FLIP + 3/3 pbuffer clean. **Still open:** which vmalloc'd
> allocation holds the stale data (page tables suspected, not isolated), and why SuperZaxxon's
> separate 1.4.14.2616 KM is clean on the Pandora without any fix. Details in
> [sgx530-12tile-fringe-flip-results.md](sgx530-12tile-fringe-flip-results.md) (**Root cause**).
>
> **Nokia recovery‑path differences:** Nokia's `HWRecoveryResetSGX` (a) bounds the
> `SGXInitialise` retry loop to 10 and disables the driver on failure, where TI loops forever
> on `RETRY` and ignores other errors; (b) always recovers under a global lock, where TI
> returns early, **skipping recovery**, if `PVRSRVPowerLock` fails; (c) runs the host lockup
> timer every 150 ms, so 3 unchanged samples ≈ 450 ms, where TI's runs every 50 ms
> (≈ 150 ms). Both program the firmware's own hang sampler identically (100 Hz); (d) logs the
> faulting process and BIF registers.
> **Adopted (patch 0003):** (b) as a deferred retry (a skipped recovery is retried from the
> next host‑timer tick) and (d) as a one‑line HWR summary (see **Reading an HWR** below).
> **Deferred:** (c), only worth it if a real workload renders a single frame for >150 ms;
> (a), not observed.

## Per-stage regression gate — RUN THIS AT EVERY STAGE

The fringe×churn storm repro is the early-warning canary: **clean on DDK 1.4 and
Pandora, storms on DDK 1.6** on this silicon. At the end of every 1.4 stage,
require **zero HWR**:

```sh
# offscreen (works from Stage 0 on; no window needed) — expect 0 HWR (1.6 = ~20)
dmesg -C
/lib/ld-linux.so.3 --library-path /opt/sgx-ddk14/usr/lib \
    /root/tilerepro-pbuffer 24 1 1 1 12 150
dmesg | grep -c "Hardware Recovery"

# windowed (add once the Stage 1 dc_nohw window surface works; stronger amplifier)
# PASS = no STORM (1.6 = 30/30 = 1/frame). WIRED + PASSING on 1.4 as of 2026-09-25.
/lib/ld-linux.so.3 --library-path /opt/sgx-ddk14/usr/lib \
    /root/tilerepro 12 1 1 1 8 30
```

Args are `tiles fringe churn upload seg frames`. The gate catches the DDK-1.6
**fringe×churn storm** (≈1 HWR/frame) reappearing — that is the FAIL. Do **not**
read it as "must be exactly 0": the 1.4 window path on this 106 MB board has a
**benign sporadic idle HWR (~0.6/run)** — `EVENT_STATUS=0x20000000` (TIMER),
`SRC (Not in use)` (no stranded flip), recovers, non-escalating, present with
**both** DISCONTIG and CONTIG buffers. So **sample ≥3×30f** (a single 30f run
reads 0 ~55% of the time at that rate and will lie to you); FAIL = a per-frame
**storm** (count scales with frames toward ~1/frame), not one stray recovery.
**Always measure on a CLEAN BOOT** — HWR rate on an accumulated/thrashed board
is meaningless (a dirty board inflated CONTIG to a bogus "17/6" that a clean-boot
A/B showed was identical to DISCONTIG). Sources: `tools/tilerepro.c` (window) and
`tools/tilerepro-pbuffer.c` (pbuffer, 1024×600, `glFinish` per frame). Measure HWR
on a single clock (`dmesg -C` + `/dev/kmsg` phase markers), never printk-time vs
`/proc/uptime`.

## Systems and access

- **B4 target** (ship silicon): BeagleBoard Rev B4, OMAP3530 ES2.1, SGX530 rev
  1.0.3 (SGX103), 128 MB, Devuan Trixie armhf, kernel
  `7.2.0-g214a35fbc02e-dirty`. Boots **DDK 1.6** (on-disk image default); swap to
  1.4 with the hand-swap below (reboot restores 1.6 as the A/B control). Stage 1
  validated on the 1.4 swap 2026-09-25.
  `ssh -i "$env:USERPROFILE\.ssh\geoduck_truenas" -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL root@192.168.50.245`
- **geoduck build host**: `ssh geoduck-tools-ryan` (ryan@geoduck.lan:2222).
  Toolchains: `arm-linux-gnueabihf-gcc` (kernel modules, hardfloat),
  `arm-linux-gnueabi-gcc` (softfloat userspace). Board has `sgx-cc`
  (/usr/local/bin, softfloat glibc 2.36).
- **Pandora control** (stock DDK 1.4.14.2514, kernel 2.6.27, SGX530 rev 1.0.3):
  `ryan@192.168.50.64`, key `~/.ssh/pandora_rsa` **on Windows**, legacy opts
  `-o KexAlgorithms=+diffie-hellman-group1-sha1 -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa -o Ciphers=+aes128-cbc -o MACs=+hmac-sha1`.
  Second independent clean control if needed.

## The 1.4 stack (ship target)

- **Source worktree** (geoduck): `/mnt/scratch/geoduck-tmp/beagle/openpvrsgx-ddk14`,
  branch `users/rgammon/pvrsgx-1.4.14.2616`, DDK dir
  `drivers/gpu/drm/pvrsgx/1.4.14.2616` (`dc_nohw` under
  `services4/3rdparty/dc_nohw`, DDK-1.8-derived, shared lineage with 1.6).
- **Branches** (pushed to `github.com/rggammon/linux_openpvrsgx`, all on the upstream
  `pvrsrvkm-7.2` merge `2342ce92f`):
  - `users/rgammon/pvrsgx-1.4.14.2616`: DDK import `f90eeed54`, then
    - `eff6bd5b8` build as a module on 7.2 (`-DMODULE`, license, `dc_nohw` second pass);
    - `c97d045d8` SGX IRQ from the device tree (`ti,omap3430-gpu`; resolves to IRQ 37 on
      the B4) instead of a hardcoded number;
    - `a8684faef` active-power latency 100 ms from `Kbuild` (as the other DDKs in the
      fork; was 500 ms);
    - `53dd2bd2f` `dc_nohw` contiguous buffers + DMA-BUF export;
    - `9413c56a6` **required** CPU cache clean+invalidate for new WC/UC allocations
      (Nokia N9 `inv_cache_mem_area`). Without it, FLIP sessions hang on the first frame
      about 42 % of the time. (Alternative: `-DSUPPORT_CACHEFLUSH_ON_ALLOC` + guard fix;
      same result, global flush.)
    - `6e0aef372` one-line HWR diagnostics + deferred recovery retry.
  - `users/rgammon/pvrsgx-1.6.16.3977`: the 1.6 stack plus the same IRQ/APM rework and the
    teardown-containment and stranded-sync fixes; `users/rgammon/pvrsgx-1.6-debug` adds
    the 1.6 investigation diagnostics/experiments.
  - `users/rgammon/b4-7.2`: integration merge of both (`079d036db`), pinned by
    [kernel/build-devuan.sh](../kernel/build-devuan.sh); `config-devuan` picks the DDK.
- **Dev hot-swap build:** [tools/build-graft-14.sh](../tools/build-graft-14.sh) `[git-ref]`
  on geoduck `git archive`s the 1.4 branch and builds `pvrsrvkm.ko` + `dcnohw.ko` as
  external modules against the b4ci kernel tree (vermagic `7.2.0-g214a35fbc02e-dirty`).
  Output: `/mnt/scratch/geoduck-tmp/beagle/pvr14-out/`.
- **On the board:** `/root/pvrsrvkm-14-rw.ko` + `/root/dcnohw-14-rw.ko` (branch head).
  Validation 2026-09-29: validate14 0/12 FLIP + 0/3 pbuffer; APM stress at 100 ms
  (FLIP with a 150 ms gap every frame 5×400, pbuffer 3×2000) 0 failures, 0 HWR.
  The pre-rework build (`-14-br`, APM 500 ms) soaked **100/100 FLIP 12t runs, 0 hangs,
  0 HWR** on 2026-09-28.
- **On-board userspace**: `/opt/sgx-ddk14/usr/{lib,bin}` (softfloat 1.4.14.2616,
  recovered from OpenPandora SuperZaxxon; full set incl. `libEGL`, `libGLESv2`,
  `libIMGegl`, `libpvr2d`, `libglslcompiler`, all WSEGLs, `pvrsrvinit`).
- **Swap to DDK 1.4** (from the default 1.6 boot):

  ```sh
  rmmod dcnohw && rmmod pvrsrvkm            # unload 1.6 (refcounts: dcnohw=0, pvrsrvkm used only by dcnohw)
  insmod /root/pvrsrvkm-14-rw.ko
  insmod /root/dcnohw-14-rw.ko              # DC MODULE REQUIRED — eglInitialize FAILS without it, even for pbuffer
  /lib/ld-linux.so.3 --library-path /opt/sgx-ddk14/usr/lib /opt/sgx-ddk14/usr/bin/pvrsrvinit
  # run apps with the same loader prefix + --library-path /opt/sgx-ddk14/usr/lib
  ```

- **Restore DDK 1.6**: just **reboot** (reloads 1.6 + dcnohw automatically).
- **Reading an HWR** (commit `6e0aef372`): every recovery starts with one line, printed before the
  long register dump so it survives dmesg wrap:

  ```text
  HWRecoveryResetSGX: SGX Hardware Recovery triggered (<trigger>) last-kick pid=<pid> (<comm>)
    EVENT_STATUS=.. EVENT_STATUS2=.. BIF_INT_STAT=.. BIF_FAULT=.. BIF_MEM_REQ_STAT=.. DIR_LIST_BASE0=..
  ```

  `<trigger>` is `firmware HWR interrupt` (the ukernel's own lockup detector),
  `host lockup watchdog` (the EDM task register was unchanged for 3 host‑timer ticks,
  ≈150 ms), or `deferred retry`. Non‑zero `BIF_INT_STAT`/`BIF_FAULT` means a GPU MMU page
  fault at that address; both zero means a pure hang. `last-kick` is the last process to
  submit a TA kick, not necessarily the one that hung. If the power lock was busy, the
  driver logs `power lock busy (...), recovery deferred to next timer tick` and retries
  50 ms later (stock TI silently skipped the recovery).
- **Never reboot mid-HWR** (wedges the kernel hard); reboot before the next
  storming test, not after.

## The 1.6 reference stack

- Worktree: `/mnt/scratch/geoduck-tmp/beagle/openpvrsgx-ddk16`, branch
  `users/rgammon/pvrsgx-1.6.16.3977`. Reference commits `91f00a69f` (Linux 7.2
  Services port), `9858b321a` (DDK-1.8-derived `dc_nohw`), plus the Stage 6
  `omapdrm_present` work (`dc_nohw` module param `present=0/1/2`).
- On-board 1.6 softfloat userspace: `/opt/sgx-ddk16/lib`, wrapper
  `sgx-ddk16-run PROG ARGS` (= `/lib/ld-linux.so.3 --library-path /opt/sgx-ddk16/lib`).
- `omapdrm` is `CONFIG_DRM_OMAP=m`; the `omapdrm_present`/`omapdrm_import_dmabuf`
  export lives in the running kernel's `omapdrm.ko` and is **stack-agnostic**
  (usable by the 1.4 `dc_nohw` too).

## The work: reproduce Stages 1–6 on 1.4, then 7–8

Use the 1.6 stages (in the two plan docs) as the exact blueprint. The `omapdrm`
export + KMS glue are stack-agnostic; the porting effort is against the 1.4
Services/`dc_nohw` source.

1. **Stage 1 — `dc_nohw` window surface + swapchain on 1.4. ✅ PASS
   (2026-09-25).** Validated on-board with `/root/tilerepro` (window FLIPWSEGL
   swapchain = the same `eglCreateWindowSurface` 1A-allocation + `eglSwapBuffers`
   1B-swap-cycling path as `tools/sgx-window-swap.c`, and simultaneously the
   windowed gate). `/proc/PID/maps` confirmed the 1.4 stack in use
   (`libpvrPVR2D_FLIPWSEGL.so.1.4.14.2616`, `libGLESv2/libEGL/libIMGegl/
libsrv_um .1.4.14.2616`). Results: windowed gate `12 1 1 1 8 30` = **0 HWR on
   the first single run**, but a later clean-boot A/B (3×30f) was **0,1,1** and
   the extended `8 1 1 1 8 300` was **1** — i.e. Stage 1 was **not** truly
   0-clean, it carries the same **benign sporadic idle HWR (~0.6/run)** as Stage
   2 (`EVENT_STATUS=TIMER`, `SRC (Not in use)`, recovers, non-escalating). The
   single-run "0" was under-sampling (~55% chance of 0 at that rate). This is
   board background noise, **not** the DDK-1.6 per-frame storm (30/30). Windowed
   regression gate is wired and passing (no storm). (`sgx-window-swap` binary is
   no longer on the board; `tilerepro` covers the same window-surface path —
   rebuild `sgx-window-swap` only if the allocation-only/leak-count checks are
   wanted.)
2. **Stage 2 — DMA-BUF export of a `dc_nohw` swapchain buffer (1.4). ✅ PASS
   (2026-09-25).** Ported the 1.6 exporter into the 1.4 `dc_nohw`
   (`dc_nohw_export.c/.h` verbatim; DISCONTIG→contig CMA; synthetic
   `platform_device` + `DCNohwGetDev`; `DCNohwGetGeometry`/`GetBufferInfo`
   accessors) — no API drift (1.4/1.6 structs identical). `dcnohw-14-export.ko`
   on board. Validated: QUERY_ABI = 1024×600 stride 4096 ARGB8888 3 buffers
   2457600 (identical to 1.6); export FD mmap readback = distinct non-zero
   checksums per swapchain buffer (names the real render target); no CMA leak
   (20 cycles flat); unload guard works (`dma_buf` owner pins the module).
   **CMA exonerated**: clean-boot A/B DISCONTIG 0,1,1 vs CONTIG 0,1,1 — identical
   (an earlier "CONTIG 9× worse" was an accumulation confound on a dirty board).
3. **Stage 3–4 — PRIME import + `SETCRTC`. ✅ PASS (2026-09-29).** One
   1.4-rendered frame on the panel via `dc_nohw → PRIME → omapdrm`. The 1.4
   `dc_nohw` dma-buf (branch-head `dcnohw-14-rw.ko`, CONTIG CMA export)
   PRIME-imports into `omapdrm` (`gem=1`), `ADDFB2` accepts it (`fb_id=63`), and
   `SETCRTC` scans it out on connector 57 / crtc 58 @ 1024×600. Validated with
   `tools/dc_nohw_kms_present` (hard-float, cross-built on geoduck): `import`
   mode (CPU colour bars) **and** `raw` mode (untouched 1.4-SGX render —
   `tilerepro` 12-tile scene) both scan out clean, dmesg clean, user-confirmed
   on the BTT-HDMI7 panel. Regression gate clean on 1.4 (pbuffer 24t/150f = 0
   HWR, windowed 3×30f = 0/0/0). The FLIP hang that blocked this is fixed (Nokia
   cache commit). (`sgx-window-swap` + `SGX_PRESENT` — the single-process 4b
   variant — is optional; the two-process render-then-present path above is
   sufficient proof.)
4. **Stage 5 — `sgxmode` presenter + `dc_nohw` swap-notify. ✅ PASS
   (2026-09-29).** Wired the swap-notify calls into the 1.4
   `dc_nohw_displayclass.c` hook sites, copying the 1.6 blueprint
   (`DCNohwNotifySwapchain(1, count)` in `CreateDCSwapChain`,
   `DCNohwNotifySwapchain(0, 0)` in `DestroyDCSwapChain`, the `DCNohwBufferIndex`
   helper + `DCNohwNotifySwap(index)` in `ProcessFlip` — **swap-notify only, no
   Stage 6 `present=` path**). Committed to the 1.4 branch as `08085605c`, built
   with `tools/build-graft-14.sh`. Validated with `tools/dc_nohw_notify_dump`
   (built native on-board with `cc`): an unmodified `tilerepro` drives 30 swaps →
   `SWAPCHAIN_CREATE count=3`, 30 `SWAP` events (rotating index 1/2/0, seq 1–30),
   `SWAPCHAIN_DESTROY`, 0 HWR. Then `tools/sgxmode` (hard-float, cross-built on
   geoduck) forked the unmodified app and page-flipped each swap-notified buffer
   — animated tiles on the BTT-HDMI7 panel (user-confirmed; expected 5a mailbox
   ghost), clean teardown (refcount 0, 0 HWR, no oops). Regression gate clean
   (pbuffer 24t/150f = 0, windowed 3×30f = 0/0/0). Board module:
   `/root/dcnohw-14-notify.ko` (branch head + hooks).
5. **Stage 6 — in-kernel `omapdrm_present`** (6a mailbox, 6b paced). **RENDER
   PASS, 6b TEARDOWN BLOCKED (2026-09-29).** Ported `dc_nohw_present.c` verbatim
   from 1.6 + wired it in (param `present` 0/1/2, `DCNohwPresentInit/Flip/Flush/
Teardown`, `DCNohwCompleteFlip` with `IMG_TRUE`); committed `15f02fd554af`
   (pushed). No omapdrm rebuild needed — the shipped kernel's `omapdrm.ko`
   already exports `omapdrm_present`/`import_dmabuf`/`release_fb` and the kernel
   `Module.symvers` has the CRCs, so the `M=` build resolves them. Board module
   `/root/dcnohw-14-present.ko`. **6a (`present=1` mailbox) and 6b (`present=2`
   paced) both RENDER correctly on the panel** (user-confirmed: 6a ghost, 6b
   ghost-free), 0 HWR during render. **← START HERE (next):** **6b clean-exit
   teardown PANICS** the board — `In-band Error seen by SGX at address 0` →
   `omap3_l3_app_irq` → Oops in IRQ → kernel panic. Root cause: `present=2`
   defers completion through the MISR/queue, which races `DestroyDCSwapChain`
   freeing the swapchain buffer → SGX touches freed memory. This is the 1.4
   **queue-teardown UAF that 1.6 fixes with patches 0010 (contain pending
   swapchain teardown) + 0016 (reconcile stranded syncs)** — 1.4 pvrsrvkm has
   neither. **Fix = port the 0010/0016-equivalent into 1.4 pvrsrvkm** before 6b
   paced is safe. 6a mailbox completes inline (no deferred MISR completion) and
   tore down cleanly — it's the safe interim path. (The three original 6b dc_nohw
   fixes — fb refcount cycle, completion off-by-one, `IMG_FALSE` hash corruption
   — are already in the ported `dc_nohw_present.c`.)
6. **Stage 7 — Slint + screen-saver soak** through the 1.4 `present=2` path,
   soft-float. Prefer the batched/uber-shader render path (nanovg-style single
   program, or Slint `cache-rendering-hint`) — belt-and-braces even though 1.4
   does not storm on fringe×churn.
7. **Stage 8 — package**: ship the DDK 1.4 runtime, keep 1.6 staged as the
   diagnostic reference; give the present path its own `SETCRTC`/modeset; add
   `vtrun` + `console-unbind`.

Re-run the Stage 0 lifecycle/soak on 1.4 after each kernel-side change, and the
**regression gate** at the end of every stage.

## Shipping a 1.4 image + hard-float shim (findings 2026-09-25)

The Devuan image is **inherently 1.6** at three layers, so today's 1.4 work is a
live-board hand-swap (reboot = back to 1.6 control). Baking a 1.4 image (Stage 8)
means:

- **Kernel**: `kernel/build-devuan.sh` `REF` now pins the fork's
  `users/rgammon/b4-7.2` integration branch, which carries both DDKs and all the
  1.4 build fixes (dc_nohw second pass, DT IRQ, APM 100). Switching is a
  `kernel/config-devuan` change only: `CONFIG_PVRSGX_1_4_14_2616=y` +
  `CONFIG_PVRSGX_1_4_14_2616_DC_NOHW=m` instead of the 1.6 pair. The 1.4
  dc_nohw must get the Stage 6 `present=` port first.
- **Userspace** (`rootfs/build-devuan.sh` installs only `sgx-ddk16-*` armel
  debs): package the 1.4 SuperZaxxon runtime (staged at `/opt/sgx-ddk14`) as
  debs, point rootfs at them.
- **Boot loader — no change needed**: `/etc/init.d/powervr`
  (`sgx-ddk16/debian/sgx-ddk16-tools.powervr.init`) `modprobe`s `pvrsrvkm` +
  `dcnohw` **by module name**, which are identical for 1.4/1.6. Installing the
  1.4 `.ko`s as the on-disk `pvrsrvkm.ko`/`dcnohw.ko` is all the boot side needs.

**Hard-float shim for 1.4.** The shim build (`tools/build-hf-shim.sh`,
`sgx-ddk16/Makefile hf-shim`, `.github/workflows/*`) is fully DDK-agnostic
(parameterized on `DDK_LIB`/`DDK_INC`). **But the 1.4 `.so`s are stripped — no
DWARF, no symtab** (verified on-board: only `Tag_FP_arch: VFPv3`, no
`Tag_ABI_VFP_args` ⇒ soft-float), so the DWARF-driven `gen-hf-shim.py` cannot
regenerate veneers from them. Easy path instead: **reuse the 1.6-DWARF-generated
forward veneers** (standard Khronos GLES2/EGL ABI is identical; 1.4 exports 204
GLESv2 FUNCs ⊇ the 142 standard ones) + `libsgxhf`/`libSGXm` unchanged, and only
re-run the mechanical `patchelf --replace-needed libm→libSGXm` /
`--add-needed libSGXm-first` / `--set-rpath` on the **1.4** DDK with
`SGXHF_DDK_DIR` pointed at the 1.4 runtime. Soft-float runs today; switch to the
HF-1.4 shim once this reuse build is validated on-board.

## Repro tools + cross-build recipe (softfloat glibc-2.9, runs on 1.4 and 1.6)

`tools/tilerepro.c` (window) and `tools/tilerepro-pbuffer.c` (pbuffer). Board has
`/root/tilerepro` (window, sgx-cc glibc 2.36) and `/root/tilerepro-pbuffer`
(softfloat glibc-2.9). To rebuild the portable glibc-2.9 softfloat binary on
geoduck (`~/pandora-build`, sysroot from Debian squeeze eglibc 2.11 + Pandora GL
libs — see memory for full sysroot assembly):

```sh
arm-linux-gnueabi-gcc --sysroot=$SR -B$SR/usr/lib -O0 -no-pie -fno-pie \
  -fno-stack-protector -U_FORTIFY_SOURCE \
  -Wl,-rpath-link,$SR/lib:$SR/usr/lib -Wl,--allow-shlib-undefined \
  tilerepro-pbuffer.c -lEGL -lGLESv2 -lm -o tilerepro-pbuffer-sf
# verify: readelf -V must show max GLIBC_2.4; interp = /lib/ld-linux.so.3
```

`-B$SR/usr/lib` and `-no-pie` are mandatory (else gcc pulls its own
`crt1.o`/`Scrt1.o` referencing `__libc_start_main@GLIBC_2.34`, which won't load
on glibc 2.9). Full recipe + Pandora cross-build notes in the memory file.

## Reference reading

- Plan + Plan Pivot + regression gate: [ddk16-dcnohw-presentation-plan.md](ddk16-dcnohw-presentation-plan.md)
- Stage 6 in-kernel flip + Stage 7/8: [ddk16-presentation-stages-6-8.md](ddk16-presentation-stages-6-8.md)
- DDK provenance / device survey: [sgx103-ddk-recovery-history.md](sgx103-ddk-recovery-history.md)
- Pandora 2514 KM + 2616 userspace, and the Nokia module lineage: [pandora-sgx-driver-lineage.md](pandora-sgx-driver-lineage.md)
- The two 1.6 ukernel regressions: [powervr-sgx-driver-bugs.md](powervr-sgx-driver-bugs.md)
- Linux 7.2 port notes: [ddk16-linux72-port-notes.md](ddk16-linux72-port-notes.md)
- Full session findings + swap mechanics + cross-build: repo memory
  `/memories/repo/beagle-b4-native-control.md`
