# SGX DDK 1.6 — residual stall follow-ups (post IRQ fix)

Candidates found while the IRQ-fix build ran (2026-09-06). The **confirmed root cause**
was the SGX interrupt mapping — DDK requested raw IRQ 21 (= virq 21 = hwirq 5, wrong line),
so the completion interrupt never fired (`/proc/interrupts`: `21: 0 INTC 5 Edge SGX ISR`,
count stuck at 0) and every `PVRSRVEventObjectWait` timed out. Fixed by
[kernel/patches-devuan/0008-pvrsgx-sgx-irq-37.patch](../kernel/patches-devuan/0008-pvrsgx-sgx-irq-37.patch)
(21 → 37). The clock patches (`0005`/`0007`) were red herrings — the DDK module self-sets
`sgx_fck` to 110.67 MHz on load.

## Status (2026-09-06)

- **Residual `EVENT_OBJECT_WAIT` stall: RESOLVED** by
  [0008-pvrsgx-sgx-irq-37.patch](../kernel/patches-devuan/0008-pvrsgx-sgx-irq-37.patch).
  Verified on hardware: `37: … INTC 21 SGX ISR` fires every frame; 500-frame serialized soak
  clean at ~30 ms/frame (native parity), zero timeout flood.
- **`PVRSRV_ERROR_UNABLE_TO_LOCK_RESOURCE(104)` under long soak: RESOLVED** by
  [0009-pvrsgx-apm-latency-500ms.patch](../kernel/patches-devuan/0009-pvrsgx-apm-latency-500ms.patch)
  (item #1 below).
- The clock patches were confirmed inert red herrings and have been **removed** from the tree.
- Remaining items (#2–#6) assessed below: **#3, #4, #6 verified/closed on hardware; #2 and #5 have no
  measured symptom.** None is worth pursuing proactively — #2/#5 carry a concrete "reconsider if" trigger.
- **render-to-texture (FBO) device-memory OOM: RESOLVED — no real leak (item #7).** A 2026-09-06 run OOM'd our
  Devuan 1.6 port under per-frame **texture**-attachment churn (`glFramebufferTexture2D`, ~40 frames), which
  looked like a device-memory leak. It is **not**: the _same byte-identical_ 1.6 blob runs the churn **clean on
  native Ångström 3.0.14** (memory recovers mid-run) with **strace-proven balanced `ALLOC`/`FREE_DEVICEMEM`**
  (268:268 over 5 frames); DDK **1.4** (OpenPandora) is clean too; and the OOM **does not reproduce on a
  fresh-boot Devuan** either (200 frames + 8×40 back-to-back, `CmaFree` flat, no OOM). The one-off OOM was a
  **transient CMA-fragmentation state** (entangled with the `cma=64M` destabilization experiment) — **not a
  driver/blob/port leak**. So: no version regression, no blob bug, no port bug, and the earlier
  "DDK-1.6-specific → prefer the 1.4 branch" reading is **retracted**. Practical residue only: don't churn FBO
  attachments per frame (Imagination's own documented antipattern). See item #7.

Source line refs are in the DDK tree on the fork branch
(`rggammon/linux_openpvrsgx`, `users/rgammon/pvrsgx-1.6.16.3977`),
under `drivers/gpu/drm/pvrsgx/1.6.16.3977/`.

---

## 1. APM at 1 ms + `ti-sysc` idle ownership — RESOLVED (0009)

**What:** `SYS_SGX_ACTIVE_POWER_LATENCY_MS = 1` (`services4/system/omap3/sysconfig.h:44`) made
the DDK power-cycle the SGX domain on essentially every serialized frame. Each transition takes
`PVRSRVPowerLock` — a test-and-set spun with a fixed ~1 s timeout (`services4/srvkm/common/power.c`).
Under a long soak one transition eventually stalls >1 s, so a concurrent render kick's lock times
out and returns **`PVRSRV_ERROR_UNABLE_TO_LOCK_RESOURCE(104)`** (seen at ~frame 1500 of a
2000-frame `-ser 1` soak; not OOM, dmesg clean).

**Fix:** [0009-pvrsgx-apm-latency-500ms.patch](../kernel/patches-devuan/0009-pvrsgx-apm-latency-500ms.patch)
raises the idle latency to 500 ms. Frames are ~30 ms apart, so the SGX stays powered through active
rendering (no per-frame cycling) and only powers down on genuine idle.

**Optional further hardening (not currently needed):** the DT wraps SGX in a `ti,sysc` target-module
(`sgx_module: target-module@50000000`, `omap34xx.dtsi`) that manages idle via `pm_runtime`, while the
DDK uses raw `clk_enable`/PRCM pokes (no `pm_runtime`) — two owners. `&sgx_module` has no `ti,no-idle`.
If power-lock issues ever recur on idle→resume, add `ti,no-idle;` to `&sgx_module`, or build without
`SUPPORT_ACTIVE_POWER_MANAGEMENT` to eliminate transitions entirely.

## 2. MISR on a normal-priority single-threaded workqueue — NOT DOING (no measured need)

**What:** the build selects `PVR_LINUX_MISR_USING_PRIVATE_WORKQUEUE` (`Makefile:348`): hard IRQ
(LISR) → `queue_work(pvr_workqueue, …)` → the global event object is signalled from a
normal-priority, single-threaded workqueue (`create_singlethread_workqueue`, `osfunc.c:760`), i.e.
process context that can be preempted.

**Decision — leave as-is.** No measured problem: the 2000-frame `-ser 1` soak had **zero** retries,
so no completion was signalled later than the ~100 ms wait allows (worst-case delivery stayed well
under ~70 ms), and the mean was at native parity (28.7 ms). The native Angstrom stack ran this same
workqueue-MISR default cleanly, so it's already proven adequate on this silicon. The tasklet
alternative runs in softirq/atomic context — real risk (no sleeping; can hurt system latency) for no
measured gain.

**Reconsider if:** a latency-sensitive workload (e.g. 60 fps interactive/compositing UI) shows
_visible_ jitter that never trips the 100 ms watchdog. Then first capture per-frame **max/p99**
timing to prove MISR jitter is the bottleneck, and try the lighter `WQ_HIGHPRI` (stays in process
context) before the tasklet.

## 3. Interrupt trigger type (Edge vs Level) — NON-ISSUE (handled as level; "Edge" is cosmetic)

**Investigated for correctness** (not just "seems fine"). `/proc/interrupts` labels the SGX ISR
**Edge**, which looked risky on a level-sensitive source — but it's a display artifact, not the real
behaviour:

- The OMAP INTC irqchip registers **every** line with `handle_level_irq` and a level mask-ack
  (`ct->type = IRQ_TYPE_LEVEL_MASK`, `irq_ack = omap_mask_ack_irq`; `drivers/irqchip/irq-omap-intc.c:208/230`).
  So the SGX interrupt is genuinely handled as **level** — correct for the source.
- The chip exposes no `irq_set_type` and the DT interrupt is single-cell, so the per-IRQ
  _trigger-type metadata_ stays `IRQ_TYPE_NONE`; `/proc/interrupts` prints anything that isn't a LEVEL
  type as "Edge". Hence **every** INTC line shows "Edge" on this board — confirmed for `i2c` (36k IRQs),
  `mmc0` (173k), `serial`, `DISPC` (45k), `dma-engine` (89k), all level-sensitive and rock-solid.

**Decision — nothing to do.** The SGX IRQ is handled exactly like the i2c/mmc/serial lines (level);
there is no edge detector and no coincident-assert loss path. The label is cosmetic.

## 4. GPT11 availability / clkdev resolution — VERIFIED OK (closed)

**Checked on hardware:** no "Couldn't get GPTIMER11" in dmesg (the DDK's `clk_get(NULL, "gpt11_*")`
resolved), and the APM/timeout logic it drives works (raising the latency in `0009` changed the
power-down behaviour as expected — impossible if the timer were dead). `gpt11_fck` reads 32 kHz in
`clk_summary` **only when released/idle** (enable count 0 after a soak); the DDK reparents it to
`sys_ck` while it holds it active. Harmless — no action needed.

## 5. Cache / DMA coherency in the kick path — NOT DOING (safe as-is)

**What:** the flush path uses coarse `flush_cache_all` / `outer_flush_all` (`osfunc.c` ~2824/2832).

**Decision — leave as-is.** These _over_-flush, so they're conservative/safe, and 2000 frames
rendered with no corruption or wedge at native performance — neither a correctness nor a perf problem
here.

**Reconsider if:** we ever see rendering corruption or coherency wedges (distinct from the known
`kill -9` wedge, which is a user-behaviour caveat, not a cache bug).

## 6. HW-recovery / lockup timer — VERIFIED OK (closed)

**Checked on hardware:** no HW-recovery / `SGXOSTimer` / lockup / reset messages in dmesg across the
2000-frame soak — the recovery timer is not false-triggering. No action needed.

## 7. FBO attachment-rebind device-memory leak — OPEN (Stage 0, reproduced 2026-09-06)

**What:** The Stage 0 test — the EGL FBO attachment-churn probe
([../tools/sgx-pbuffer-latency.c](../tools/sgx-pbuffer-latency.c): create FBO, attach a color target,
`glClear`, `glFinish`, per-frame `glFramebufferTexture2D` rebind) — now runs on the fixed 1.6 stack via
mixed-libc loader isolation ([../tools/test-sgx-ddk16.sh](../tools/test-sgx-ddk16.sh)). Bringing EGL up
required the **1.6 WSEGL** window-system module (`libpvrPVR2D_FLIPWSEGL.so`, Version 1.6.16.3977 softfp,
staged from `beagle-archive/angstrom-sgx-test.tar` → `opt/ti-ddk16/runtime/`) plus `/etc/powervr.ini`
`WindowSystem=libpvrPVR2D_FLIPWSEGL.so`. `soak16/gl` ships **no** WSEGL, so `eglInitialize` had been
failing for every EGL client (`eglinfo`, `gles2test1`, the probe) while PVR2D (`sgx_render_flip_test`)
worked. `FRONTWSEGL` OOMs immediately (sizes a front buffer from dc_nohw's bogus geometry); `FLIPWSEGL`
is correct (matches the 1.4 precedent).

**Symptom:** With per-frame attachment rebind (`SGX_REBIND_ATTACHMENT=1`) the DDK 1.6 stack **leaks SGX
device memory** — ~1 MB per rebind (≈½ a 1024×600×4 render target) — and the probe is **OOM-killed within
~40 frames** on the 128 MB board (`CmaFree` drains via `PVRSRVAllocDeviceMemKM` → `BM_ImportMemory` →
`OSAllocPages`; the oom-killer fires on `ld-linux.so.3`). Independent of target alternation
(`alternate=0 rebind=1` also OOMs). **No** BIF fault / HWRecovery. With rebind disabled
(`SGX_REBIND_ATTACHMENT=0`) it runs 120 frames clean, CMA stable, ~10 ms/frame after warmup
([../out/sgx-fbo-norebind-ddk16.csv](../out/sgx-fbo-norebind-ddk16.csv)).

**Why it matters:** it is the likely core of the presentation-plan's Stage 0 blocker — though here it
manifests as an OOM leak, not the plan's BIF fault (same-root vs. related is still open). **Correction
(2026-09-06):** the `sgx-fbo-*-ddk14` / `sgx-fbo-*-ddk17` datasets were **not** the same test — they came
from an _older_ probe (header `iterations=… alternate=… fbo=…`, no `texture=`/`rebind_attachment=`) that
churned **renderbuffers** (`glFramebufferRenderbuffer`), a different driver path from the 1.6 leak run's
**textures** (`glFramebufferTexture2D`). So they do **not** establish a "1.6 regression": DDK-version and
attachment-type are confounded. (The DDK 1.17 ~800 ms every-other-frame result was a _stall/timing_
observation on that renderbuffer probe — a separate phenomenon from this leak.)

**Dig-in (in progress, 2026-09-06):**

- **Discriminator — the attach _call_ does not leak; the _render after re-attach_ does.** A probe flag
  `SGX_SKIP_RENDER=1` (re-attach every frame via `glFramebufferTexture2D` but skip `glClear`/`glFinish`)
  runs 60 frames **clean**, CMA fully returned. So the leak is a per-render device-memory allocation that
  is triggered whenever the FBO color attachment changed since the last kick — with a stable attachment
  (`rebind=0`) the render reuses the target and nothing accumulates.
- **Instrumented with kprobes** (`CONFIG_KPROBE_EVENTS=y`; no module rebuild): a 15-frame `rebind=1`
  burst issued **237 `_PVRSRVAllocDeviceMemKM` with 0 `PVRSRVFreeDeviceMemKM`** (and 59 `BM_ImportMemory`
  fresh CMA imports — ~4 fresh page imports per frame). The kernel free bridge is never called during
  rendering; frees are deferred to process teardown. **⚠️ CORRECTED (2026-09-07) — this was a wrong-symbol
  artifact:** an `strace` of the _userspace_ bridge on native Ångström (Result (d)) shows the blob issues
  **268 `FREE_DEVICEMEM` bridge calls, exactly balanced with 268 `ALLOC_DEVICEMEM`** — the frees DO happen
  per frame; `PVRSRVFreeDeviceMemKM` just isn't where the `FREE_DEVICEMEM` bridge lands, so the kprobe missed
  them. The "0 frees / it's the blob" inference below is superseded by Results (c)/(d).
- **Source-audit result (2026-09-06) — the port is exonerated; it points back to the blob.** Diffed the
  forward-port (commit `91f00a69f`) against the vendor import: the **entire device-memory lifecycle is
  vendor-original**. `ra.c` (arena `RA_Alloc`/`RA_Free`), `buffer_manager.c` (`BM_*`) and `mem.c` are
  **untouched** by any port commit; `devicemem.c` (`PVRSRVAllocDeviceMemKM`/`FreeDeviceMem`/
  `FreeDeviceMemCallBack`) changed only `#include <stddef.h>` → `<linux/stddef.h>`; `mm.c`'s page
  alloc/free is intact (only a `__vmalloc` signature shim, `PVRVmalloc`); and the 227-line `osfunc.c`
  rewrite touched **no** alloc/free/page/pool wrapper (only removed the `CPUVAddrToPFN` mmap helper +
  OS-primitive/API/PAT adaptation). So the alloc / arena / free / page-recycle path behaves identically to
  Ångström's. **The leak is not our kernel port** — it is the **GL userspace blob** (render-to-texture path) allocating an FBO
  render surface per _render-after-reattach_ and never freeing it (a latent blob bug normal apps never hit
  because they set up an FBO once instead of churning attachments).
- **⚠️ Version vs attachment-type confound (2026-09-06).** The earlier "1.4 ran the same test clean" was
  **wrong**: the `sgx-fbo-*-ddk14`/`ddk17` datasets came from an _older_ probe that churned **renderbuffers**
  (`glFramebufferRenderbuffer`; header has no `texture=`/`rebind_attachment=`), whereas the 1.6 leak run
  churns **textures** (`glFramebufferTexture2D`). Render-to-renderbuffer and render-to-texture are different
  driver paths, so there is **no like-for-like data**: texture-churn was never run on 1.4, renderbuffer-churn
  was never run on 1.6, and this probe was never run on native Ångström at all. The leak may be
  **render-to-texture-attachment-specific** (plausibly present on 1.4-with-textures too) rather than a 1.6
  version regression. Two cheap experiments settle it: (a) 1.6 `use_texture=0 rebind=1` — clean ⇒
  texture-specific; (b) the current texture probe on DDK **1.4** (Pandora native, or build for our 1.4
  branch) — leaks ⇒ not a 1.6 regression. **Strategic:** if 1.4 proves _more conformant_ (handles texture
  churn without leaking), that is a real argument to prefer the 1.4 branch — 1.6 was chosen only on presumed
  maturity, not a hard requirement.
- **Result (a) — renderbuffer churn on 1.6 is CLEAN (2026-09-06).** `use_texture=0 rebind=1`, 120 frames:
  rc=0, CMA net-zero, ~0.5 ms/frame ([../out/sgx-fbo-renderbuffer-ddk16.csv](../out/sgx-fbo-renderbuffer-ddk16.csv)).
  So **renderbuffer attachment churn does not leak on 1.6** — the leak is **render-to-texture-attachment-
  specific** (`glFramebufferTexture2D`), consistent with renderbuffers being clean on both 1.4 (old test) and
  1.6. Architecturally sound: an RTT color attachment needs an intermediate (twiddled-texture) render
  surface that a plain renderbuffer target does not — that intermediate is what the driver allocates per
  re-attach and never frees. **Still open — experiment (b):** does render-to-texture churn also leak on DDK
  **1.4** (Pandora native or our 1.4 branch)? Leaks ⇒ a general SGX RTT limitation (not version-specific);
  clean ⇒ 1.4 is genuinely more conformant → argument for the 1.4 branch.
- **Result (b) — render-to-texture churn on native DDK 1.4 is CLEAN (2026-09-07) — DECISIVE.** Ran _this_
  probe (`texture=1 rebind_attachment=1` — the exact config that OOM-kills 1.6 by ~40 frames) on the
  **pristine OpenPandora** native stack (kernel 2.6.27.46, DDK **1.4.14.2514**, `pvrsrvkm`+`omaplfb`,
  SGX530, 256 MB): **120 frames, rc=0, MemFree perfectly flat (182384 kB start = min-during = last), no
  OOM** ([../out/sgx-fbo-texture-ddk14-pandora.csv](../out/sgx-fbo-texture-ddk14-pandora.csv)). So
  render-to-texture attachment churn does **not** leak on DDK 1.4 → the leak is **DDK-1.6-specific, not a
  general SGX RTT limitation**. **This resolves the version/conformance question: DDK 1.4 is genuinely more
  conformant than 1.6 on this pattern — a concrete argument to prefer the 1.4 branch** (1.6 was chosen only
  on presumed maturity). _Method:_ the Pandora has no compiler/headers, so the probe was cross-built as a
  glibc-2.9-compatible binary (link against Debian **squeeze** eglibc 2.11 crt+libs, `-D_TIME_BITS=32`,
  manual `-nostdlib` link to dodge the absolute-path `libc.so` GROUP script); native 1.4 EGL exposes ES2
  only on WINDOW configs (no ES2 pbuffer, unlike `dc_nohw`), so the probe grew an `SGX_WINDOW=1`
  fullscreen-fbdev-window path (native window handle `0`), run with SLIM/X stopped
  (`slim-init stop`; restart via `setsid ... slim-init start`).
- **Result (c) — native Ångström DDK 1.6 does NOT leak (2026-09-07) — REVERSES the "it's the blob" call.**
  Ran _this_ probe (`texture=1 rebind_attachment=1`, the config that OOM-kills our Devuan 1.6 by ~40 frames)
  on the **pristine native Ångström 3.0.14** stack (original `pvrsrvkm`+`omaplfb`, DDK **1.6.16.3977** -r114a,
  libEGL **byte-identical** to our soak16, md5 `6c1c07c5…`), staged with the matching FLIPWSEGL from
  `angstrom-sgx-test.tar`: **120 frames, rc=0, no OOM**, MemFree bounded to an ~18 MB working set that
  **recovers _during_ the run** (71836→53392 min→69332 kB) — device memory is **freed as the churn proceeds**
  ([../out/sgx-fbo-texture-ddk16-angstrom.csv](../out/sgx-fbo-texture-ddk16-angstrom.csv)). (15-frame runs were
  misleading: the ~18 MB working set ≈ 15×1 MB masked any per-frame trend; 60/120-frame runs are decisive.)
  So the **1.6 GL blob does not leak** — the Devuan OOM is an **environment artifact**, not the blob and not a
  1.6-vs-1.4 gap. **Leading suspect: CMA.** On the modern-kernel Devuan port device memory is served from the
  CMA pool (`CmaFree` drains), and freed CMA pages don't fully return under churn (fragmentation / migration
  failure → large alloc fails → OOM — our own "CMA note" below flags exactly this); the native 3.0.14 kernel
  has **no CMA** and recycles the same alloc/free churn from plain system RAM cleanly. (Alternative /
  compounding suspect: the `dc_nohw` fake display class vs native `omaplfb`.) The source audit was right that
  the port didn't change the DDK memory _code_ — the missed variable was the _page source_ (CMA vs system RAM).
  **Retracts Results (a)/(b)'s "prefer the 1.4 branch" argument:** both 1.4 and 1.6 are clean natively; the
  churn OOM is specific to our Devuan **CMA/`dc_nohw` environment**, independent of DDK version. **Next: pin
  CMA vs `dc_nohw`** on Devuan (run 1.6 without CMA, or with a real display class).
- **Result (d) — `strace` proves the userspace frees every frame (2026-09-07) — pins CMA, corrects the
  kprobe.** `strace -e trace=ioctl` of the probe on native Ångström (small 128×128 build to fit the 110 MB
  board; squeeze armel `strace` 4.5.20 runs on its glibc 2.13) shows the PVR bridge ioctls
  (`0xc01c67XX`, `XX` = `CORE_CMD_FIRST+index`, `CORE_CMD_FIRST=0`) are **perfectly balanced**: **268
  `ALLOC_DEVICEMEM` (idx 6) ↔ 268 `FREE_DEVICEMEM` (idx 7)** and **299 `MHANDLE_TO_MMAP_DATA` (11) ↔ 299
  `RELEASE_MMAP_DATA` (27)** over 5 frames ([../out/sgx-fbo-ioctl-ddk16-angstrom.txt](../out/sgx-fbo-ioctl-ddk16-angstrom.txt)).
  So the blob **does** issue the free on every render — there is **no userspace leak**. This **corrects the
  earlier kprobe** ("237 alloc / 0 `PVRSRVFreeDeviceMemKM`"): that symbol simply isn't where the
  `FREE_DEVICEMEM` bridge lands, so the kprobe under-counted frees to zero and wrongly implicated the blob.
  With balanced alloc/free proven and native memory recovering mid-run, the Devuan OOM is confirmed **CMA-side
  reclaim** (freed pages not returning to the pool under churn), **not** a blob or userspace leak — Hypothesis
  A, decoupled from `dc_nohw`. Remaining loose end is only the _direct_ Devuan confirmation (CMA-off run).
- **Result (e) — the OOM does NOT reproduce on a fresh-boot Devuan (2026-09-07) — the "leak" was transient.**
  Re-ran the exact churn (`texture=1 rebind_attachment=1`, 1024×600) on the Devuan 1.6 port after a clean
  reboot (`cma=48M`): **200 frames continuous, rc=0, `CmaFree` flat (31584→31236 kB), no OOM** — and **8×40-frame
  back-to-back runs (320 frames) held `CmaFree` steady at ~34 MB with zero ratcheting**. So the 2026-09-06
  "OOM ~40 frames, `CmaFree` drains to exhaustion" does **not** robustly reproduce; it is best explained as a
  **transient CMA-fragmentation/starvation state** left over from the `cma=64M` destabilization experiment (and
  repeated churn in that degraded state), **not a real per-frame leak**. Combined with (b)/(c)/(d) — 1.4 native
  clean, 1.6 native clean, strace balanced alloc/free — **render-to-texture churn does not leak on any stack
  (1.4, 1.6-native, or 1.6-Devuan); item #7 dissolves.** The only residue is the practical (vendor-endorsed)
  note not to churn FBO attachments.
- **Vendor-documented corroboration (web, 2026-09-06) — this is a known SGX antipattern, not a fixable
  bug.** Imagination's own [PowerVR Graphics Recommendations → Render to texture](https://docs.imgtec.com/performance-guides/graphics-recommendations/html/topics/sorting-geometry-effectively-on-powervr.html)
  states: _"attachments should be unique to each FBO, and **attachments should not be added or removed once
  the FBO has been created**"_ — exactly the per-frame re-attach our probe does. And the TI E2E thread
  [GL_OUT_OF_MEMORY on OMAP3530 SGX530](https://e2e.ti.com/support/processors-group/processors/f/processors-forum/238720/gl_out_of_memory-on-omap3530-sgx530)
  shows multiple users hitting `GL_OUT_OF_MEMORY` with hundreds of MB free, with TI's guidance to
  _"allocate the known texture objects, and reuse them … without the create/destroy cycle"_ and Imagination
  declining to investigate ("drivers unsupported"). So SGX530 driver-side memory limits are a **documented**
  class of issue, and the vendor's position is _reuse, don't churn_. Per the GLES2 **spec** the probe is
  legal and a conformant driver shouldn't leak (a real conformance shortfall), but it triggers a pattern the
  vendor explicitly documents against — so pragmatically it's "known antipattern → driver falls over," not a
  bug on recommended usage.
- **CMA note:** tried raising `cma=48M` → `cma=64M` for a longer pre-OOM measurement window, but on the
  128 MB board that **starved the non-movable/kernel pool** — module reload + probe then reset the SSH
  link / soft-locked the board repeatedly. **Reverted to `cma=48M`** (the classic too-much-CMA tradeoff).
  A gentler measurement rig (run detached to a file; serial + `sysrq-w`/`-t` to catch the wedge) is needed
  before more on-board instrumentation.
- **Implication / workaround:** regardless of root, the practical guidance is _don't churn FBO
  attachments per frame_ — keep a stable FBO/attachment (the `rebind=0` pattern is clean at native parity).
  This is **Imagination's own documented recommendation** (attachments should not be added/removed after
  FBO creation), so it is the intended usage, not a concession.

---

## Bottom line

With `0008` (IRQ) + `0009` (APM) the DDK 1.6 stack is stable at native parity over a 2000-frame soak.
**None of the remaining stall items (#2–#6) is worth pursuing** — #3/#4/#6 are verified correct/clean on
hardware (the "Edge" label is cosmetic; the IRQ is handled as level), and #2/#5 have no measured symptom
and carry change-risk. #2 and #5 keep a concrete "reconsider if" trigger above; revisit only when a real
workload exhibits it.

**Item #7 — RESOLVED, no real leak (2026-09-07).** The FBO **render-to-texture** attachment-churn "leak" does
**not** reproduce. On a fresh-boot Devuan (`cma=48M`) the exact churn (`texture=1 rebind=1`) runs **200 frames
continuous + 8×40 back-to-back — `CmaFree` flat, no OOM** (Result (e)); the _same byte-identical_ 1.6 blob is
clean on native Ångström 3.0.14 ([../out/sgx-fbo-texture-ddk16-angstrom.csv](../out/sgx-fbo-texture-ddk16-angstrom.csv))
with **strace-proven balanced alloc/free** (Result (d), [../out/sgx-fbo-ioctl-ddk16-angstrom.txt](../out/sgx-fbo-ioctl-ddk16-angstrom.txt));
and DDK **1.4** (OpenPandora) is clean too ([../out/sgx-fbo-texture-ddk14-pandora.csv](../out/sgx-fbo-texture-ddk14-pandora.csv)).
The one-off 2026-09-06 OOM was a **transient CMA-fragmentation state** (entangled with the `cma=64M`
destabilization experiment), **not a driver/blob/port leak**. So: no version regression, no blob bug, no port
bug — and the earlier "prefer the 1.4 branch because 1.6 leaks" argument is **fully retracted**. Practical
residue only: don't churn FBO attachments per frame (Imagination's own documented antipattern), which no real
workload does. With `0008`+`0009`, the DDK 1.6 stack is stable at native parity and has **no open leak**.

## Test recipe (on the board, per image)

```sh
modprobe pvrsrvkm
insmod /lib/modules/$(uname -r)/kernel/drivers/gpu/drm/pvrsgx/1.6.16.3977/services4/3rdparty/dc_nohw/dcnohw.ko
/root/s16/run.sh pvrsrvinit
grep -i SGX /proc/interrupts          # expect virq 37, count rising
dmesg -c >/dev/null
/root/s16/run.sh sgx_render_flip_test -nf -f 2000 -ser 1 -sf 500 -tpf 100
grep -i SGX /proc/interrupts          # count should have climbed a lot
dmesg | grep -iE 'sgx|pvr|timeout|recover'
```

Never `kill -9` an SGX render app — it wedges the core (survives rmmod; needs power-cycle). Let
tests exit via `-f <count>`.
