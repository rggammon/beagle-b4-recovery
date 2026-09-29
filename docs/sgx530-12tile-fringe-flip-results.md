# SGX530 12‑tile AA‑fringe FLIP storm — full test matrix

Root‑cause investigation of the PowerVR SGX530 **Hardware Recovery (HWR) “storm”** that
fires when the 12‑tile anti‑aliased‑fringe workload is presented via `eglSwapBuffers`
(FLIP) on the BeagleBoard **B4**. This file collects every case tested so the evidence
chain is reproducible.

> **✅ ROOT CAUSE (2026‑09‑28): missing CPU cache maintenance on newly allocated GPU memory in
> the 1.4 KM.** The FLIP hang is a GPU **page fault on the first frame** at a 3D‑parameters‑heap
> address that was **never mapped**, i.e. the GPU read a garbage parameter‑buffer pointer.
> **Fix:** clean+invalidate the CPU cache for each new write‑combined/uncached allocation in
> `OSAllocPages`, on both allocator paths (vmalloc, including the GPU page tables, and
> `alloc_pages`). This is a port of Nokia's N9 driver fix `inv_cache_mem_area`, which the
> Pandora's default driver carries. Debug‑free module → **12/12 FLIP + 3/3 pbuffer clean**.
> The vendor option `SUPPORT_CACHEFLUSH_ON_ALLOC` (global flush) also works. 1.6 already does
> this unconditionally. See **Root cause** below.

> **⚠️ CORRECTION (2026‑09‑27): the “2616 ukernel causal / 2514 clean” conclusion below is
> REFUTED.** A matched **12‑sample clean‑boot** matrix shows **2616 FLIP = 5 hang / 7 clean
> (~42 %)** and **2514 FLIP = 5 hang / 7 clean (~42 %)** — statistically identical. The earlier
> single/few‑run split (rows 1–3, “storm every frame” vs “clean”) was **small‑sample luck on a
> dirty board**: a 30‑frame 1 fps FLIP run hangs ~42 % of the time regardless of ukernel build,
> so a lucky 2514 run reading 0 and an unlucky 2616 run reading many looked decisive but were
> not. See **Corrected result** and **Conclusion** below; rows 1–3 are retained for history.

## The workload

`tilerepro <tiles> <fringe> <churn> <reupload> <seg> <frames>` — the canonical repro is:

```
tilerepro 12 1 1 1 8 <frames>
```

- 12 tiles, `fringe=1`, `churn=1`, `reupload=1`, `seg=8` → **1344 verts/frame** of AA‑fringe geometry.
- Each frame: `glClear(GL_COLOR_BUFFER_BIT)` + draw + **`eglSwapBuffers`** (FLIP present).
- `TR_DELAY_US=1000000` = 1 fps (each frame renders “cold”); no delay = free‑run.
- The **storm** = `PVR_K: HWRecoveryResetSGX: SGX Hardware Recovery triggered` — the
  `SGXOSTimer` watchdog fires (~175 ms) when the render makes **no monitored progress**.

## Mechanism (how present drives the stall)

- **FLIP present forces a partial render (SPM)**: the presented frame is split into
  **3 SGX kicks/frame** — 1 initial (`first=1`) + 2 resumes (`first=0`).
  DOKICK bridge index: **1.6 = `0x43`**, **1.4 = `0x4d`** (renumbered).
- **Offscreen (FBO/pbuffer)** renders are **single‑pass** (1 kick), **never present‑SPM** —
  at 12/24/32 tiles and 1–8 MiB param buffers. (Note: the **1.6** stack still storms
  offscreen via a _separate_ cold‑AA‑render stall — ~20 HWR/150f, row 5b — but that is not
  the present‑forced SPM this section is about.)
- The stall is on the **`first=1` initial partial‑render pass** (3/3 storms), _not_ the
  `first=0` resumes: `SGXKICK first=1 …` → HWR ~175 ms later.
- The SPM is **not the bug** — it is normal present behaviour that Pandora does identically
  (see matrix), and on B4 **FRONT** issues the same kick stream (5×`first=1` + 8×`first=0`
  per 5 frames) as FLIP yet never hangs. The hang needs the **FLIP swapchain/flip‑command
  path** on top of the SPM render (see FRONT vs FLIP below).

## Platforms

| Tag                     | SoC / GPU           | Kernel         | DDK                       | Display class        | Surface               | RAM    |
| ----------------------- | ------------------- | -------------- | ------------------------- | -------------------- | --------------------- | ------ |
| **B4**                  | OMAP3 SGX530 r1.0.3 | 7.2.0 (Devuan) | 1.4.14.2616 / 1.6.16.3977 | `dc_nohw` (headless) | 1024×600              | 106 MB |
| **Pandora**             | OMAP3 SGX530 r1.0.3 | 2.6.27.46      | 1.4.14.2514               | `omaplfb` (real fb)  | 800×480 (fb 800×1440) | 243 MB |
| **Angstrom** (on B4 HW) | OMAP3 SGX530 r1.0.3 | 3.0.14         | 1.6.16.3977               | `omaplfb` (real fb)  | 640×480               | 106 MB |

The SGX **ukernel (EDM microcode)** is uploaded by userspace `pvrsrvinit` at
`DevInitSGXPart2KM`, so “the ukernel” = whichever DDK userspace is used, independent of
the kernel module version (subject to the `SGXDevInitCompatCheck` build gate).

## Result matrix — 12‑tile fringe FLIP (unless noted)

| #   | Platform | Ukernel                             | Mode                                                | Result                                                                                                                                   |
| --- | -------- | ----------------------------------- | --------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------- |
| 1   | B4       | **1.6.16.3977** (stock)             | FLIP, 1 fps                                         | **STORM every frame** (HWR 6/6, 8/8, 12/12, 15/15)                                                                                       |
| 2   | B4       | **1.4.14.2616** (graft)             | FLIP, 1 fps                                         | **STORM every frame**                                                                                                                    |
| 3   | B4       | **1.4.14.2514** (graft, patched KM) | FLIP, 1 fps                                         | ~~CLEAN — 0 HWR~~ **superseded:** 5/12 hang in the clean‑boot matrix below                                                               |
| 4   | B4       | 1.4.14.2514                         | FLIP, free‑run                                      | STORM (1 HWR + hang, or ~24‑HWR cascade; re‑verified 1 HWR 2026‑09‑27) — _residual B4 factor_                                            |
| 5   | B4       | **1.4** (2514/2616)                 | **FBO offscreen** 12/24/32t, PB 1–8 MiB, 565 & 8888 | **CLEAN — 0 HWR** (no present‑SPM)                                                                                                       |
| 5b  | B4       | **1.6.16.3977**                     | **FBO offscreen** 24t/150f                          | **~20 HWR / 150f** (deterministic, 6/6) — no present‑SPM, but the **1.6 cold‑AA‑render storms offscreen** (the DDK‑version pivot signal) |
| 6   | B4       | any                                 | **clear+swap FLIP** (`tiles=0`)                     | **CLEAN** (`first=1` only, no SPM)                                                                                                       |
| 7   | Pandora  | 1.4.14.2514                         | FLIP, free‑run 60f & 1 fps 30f                      | **CLEAN** (~50 fps, 0 HWR)                                                                                                               |
| 8   | Angstrom | 1.6.16.3977                         | FLIP, 1 fps / free‑run                              | **INCONCLUSIVE** — `eglSwapBuffers` deadlocks on this omaplfb presenter (see notes)                                                      |

> Rows 1–3 above are **superseded** by the corrected result below (each was 1–few runs).

### Corrected result — matched 12‑sample clean‑boot matrix (2026‑09‑27)

Protocol per arm: fresh `reboot -f` → 1.4‑graft KM (`pvrsrvkm-14-flow.ko` + `dc_nohw-14-pf.ko`)
→ `pvrsrvinit` with the arm’s userspace → **12×** `tilerepro 12 1 1 1 8 30` at `TR_DELAY_US=1000000`
(1 fps), `timeout 45`, `dmesg -C` between runs, count `rc` + HWR.

| Ukernel         | Hang (rc=124) | Clean  | Hang rate | Per‑hang HWR              |
| --------------- | ------------- | ------ | --------- | ------------------------- |
| **1.4.14.2616** | 5 / 12        | 7 / 12 | ~42 %     | 1 HWR each                |
| **1.4.14.2514** | 5 / 12        | 7 / 12 | ~42 %     | 4×1 HWR, 1×33‑HWR cascade |

**Identical hang rates.** Under clean‑boot multi‑sampling there is **no 2514‑vs‑2616 difference**.
The 1 fps FLIP “storm” is really a **sporadic single‑HWR hang** (rc=124, occasionally cascading
to a wedge), not a deterministic per‑frame storm, and it is **ukernel‑build‑independent**.

### FRONT vs FLIP — matched clean‑boot protocol (2026‑09‑27)

Same protocol as above (1.4‑graft KM, 2616 userspace, 12× `tilerepro 12 1 1 1 8 30`, 1 fps),
only the CWD `powervr.ini` `WindowSystem` differs (verified via `/proc/PID/maps`).

| WSEGL     | Hang       | Clean       | L3 errors | Kicks per 5 frames        |
| --------- | ---------- | ----------- | --------- | ------------------------- |
| **FLIP**  | 5 / 12     | 7 / 12      | —         | 5×`first=1` + 8×`first=0` |
| **FRONT** | **0 / 12** | **12 / 12** | 0         | 5×`first=1` + 8×`first=0` |

FRONT renders into the same `dc_nohw` memory and issues the **same partial‑render kick
stream**, yet never hangs (chance of a 0/5 split by luck ≈ 2 %). So the buffer memory and
the SPM render are ruled out as sufficient; the hang needs the **FLIP swapchain path**
(3‑buffer rotation of flip commands through `PVRSRVProcessQueues` → `ProcessFlip`).
FRONT is the clean present baseline to soak and pivot from.

### Offscreen pbuffer A/B — the DDK‑version signal (re‑verified 2026‑09‑27)

A **different** axis, and the one the DDK‑1.4 pivot actually rests on: **offscreen** pbuffer
`tilerepro-pbuffer 24 1 1 1 12 150` (no present at all), 6 samples/arm, clean boot per arm.

| Stack                   | pbuffer 24t/150f offscreen                |
| ----------------------- | ----------------------------------------- |
| **1.6.16.3977** (full)  | **20 HWR every run** (6/6, deterministic) |
| **1.4.14.2616** (graft) | **0 HWR every run** (6/6, deterministic)  |

This is **rock‑solid and reproducible** (unlike the flaky FLIP hang). So there are **two
distinct phenomena**: (A) the **FLIP 12t hang** — flaky ~42 %, **ukernel‑independent**, needs
the present path; and (B) the **offscreen cold‑AA‑render storm** — deterministic, **DDK‑version
(1.6 vs 1.4) specific**, appears with **no present**. The refuted ukernel A/B (A) does **not**
undermine the pivot, which is grounded in (B).

### Case 8 notes (Angstrom, DDK 1.6 on kernel 3.0.14)

- Reaching a FLIP window needed a triple‑buffered fb: u‑boot `setenv optargs omapfb.vram=0:4M`,
  then `fbset -fb /dev/fb0 -g 640 480 640 1440 16` (full geometry; `-vyres` alone is ignored).
- `eglCreateWindowSurface(handle 0)` then **succeeds**, but the real `eglSwapBuffers` FLIP
  **deadlocks** (single‑DSS presenter never completes the flip). `tilerepro` gets stuck in
  **D‑state** (unkillable), wedging the GPU. BusyBox here has no `timeout`/`pkill`.
- Leaked signal (unreliable): a few **clean SPM frames** (0 HWR, CCB `WO 0x19→0x1C→0x1F`
  +3/frame, `RO==WO`) and **≤ 1 HWR** before deadlock — **not** B4’s every‑frame storm.
  Offscreen FBO on this stack is clean. Leans toward “1.6 does not per‑frame‑storm on 3.0.14,”
  but the flip‑deadlock confound means it is **not** definitive.
- 2514 can’t be tested on Angstrom: its KM is 1.6, and 1.4‑vs‑1.6 is a _different DDK version_
  (MKIF structs differ), so the build‑gate bypass used on B4 (2514‑vs‑2616, same 1.4.14) is unsafe.

### Root cause — unmaintained CPU caches on fresh parameter‑buffer pages (2026‑09‑28)

**Fault capture.** Instrumented 1.4 KM (HWR‑time BIF register read + walk of every MMU
context's PD/PT, plus a ring of MMU events). Every captured hang is **kick #1 of a new
process** with `EUR_CR_BIF_INT_STAT=0x4008` (page fault) at a 3D‑parameters‑heap VA:

| Capture       | Fault VA     | PDE / PT in any context |
| ------------- | ------------ | ----------------------- |
| B             | `0x0a410000` | (not walked)            |
| MMU run 1     | `0x08cec000` | none                    |
| MMU run 2     | `0x0c13b000` | none                    |
| control run 6 | `0x0c324000` | none                    |

Each process's entire 3D‑params usage is a fresh 6 MB+16 KB parameter buffer at `0x08400000`
plus two small blocks, all below `0x08A45000`. Nothing was unmapped or freed before the
fault, and only one CCB command (carrying the PD‑cache invalidate) preceded it. So this is
**not** a TLB/mapping race. The GPU follows a **garbage page pointer** out of a freshly
initialised parameter buffer.

**Mechanism.** 1.4's `OSAllocPages` hands newly allocated memory to the GPU with **no CPU cache
maintenance** on either allocator path: `alloc_pages()` (`NewAllocPagesLinuxMemArea`) or vmalloc
(`NewVMallocLinuxMemArea`). The GPU's own **page tables and page directory** come from the vmalloc
path (`_AllocPageTableMemory` → `OSAllocPages(WRITECOMBINE | KERNEL_ONLY)`). Each new process
gets fresh 3D‑params page tables, and stale dirty lines on those pages can reach memory after
the CPU fills them in. The GPU MMU then maps parameter‑buffer pages to the wrong physical
memory and reads a garbage pointer on the first frame. Userspace mappings are correct
(logged: parameter buffer `WC`, prot `0x307` = Normal non‑cacheable). 1.6 flushes on every
WC/UC vmalloc allocation (`OSInvalidateCPUCacheRangeKM` in `NewVMallocLinuxMemArea`).

**A/B (same module `pvrsrvkm-14-af.ko`, runtime param `gAllocFlush`, one boot):**

| `gAllocFlush`                             | FLIP 12t 1 fps 30f                                                  |
| ----------------------------------------- | ------------------------------------------------------------------- |
| **1** (flush after `alloc_pages`)         | **12/12 clean**, 0 HWR, 0 faults (+4/4 in an interrupted run)       |
| **0** (stock 1.4)                         | **hang on run 6**: first frame, never‑mapped VA fault, 30‑HWR wedge |

$P(0/16 \mid 42\%) \approx 0.02\%$.

**Narrowing the fix (clean boots, debug‑free modules, 12× FLIP + 3× pbuffer):**

| Module                                                           | FLIP hangs | pbuffer HWR |
| ---------------------------------------------------------------- | ---------- | ----------- |
| Per‑page clean+invalidate of `alloc_pages()` pages only                        | **8/12**   | 0/3         |
| `-DSUPPORT_CACHEFLUSH_ON_ALLOC` (global flush on every WC/UC alloc)            | **0/12**   | 0/3         |
| **Nokia‑style per‑area clean+invalidate, both vmalloc and `alloc_pages` paths** | **0/12**   | 0/3         |

So the stale data isn't in the parameter‑buffer pages themselves but in vmalloc'd allocations
(most likely the page tables; not isolated from the other vmalloc'd buffers). The vendor option
also needed a small `osfunc.c` fix: `OSFlushCPUCacheKM` was
guarded only by `SUPPORT_CPU_CACHED_BUFFERS`. Patches on geoduck:
`pvrsgx-1.4-nokia-inv-cache-mem-area.patch` (**recommended**, +42 lines in `mm.c`/`mm.h`/`osfunc.c`)
and `pvrsgx-1.4-cacheflush-on-alloc.patch` (vendor option).

**Where the fix comes from:** TI's 1.4.14.2616 source (SDK 4.00.00.01) has no allocation‑time
cache maintenance at all. The Pandora's default driver is Nokia's N9/Harmattan 1.4.14.2514
driver (GPL; `pali/linux-n900` branch `v2.6.32-nokia`, `drivers/gpu/pvr`), rebuilt by notaz.
Nokia added `inv_cache_mem_area()` at the end of `OSAllocPages` for WC/UC allocations, on both
allocator paths. Our port uses clean+invalidate (a safe superset of Nokia's invalidate).

**Pandora cross‑check:** the Pandora's default (Nokia) driver has the fix. SuperZaxxon's
separate 1.4.14.2616 KM (no cache‑maintenance symbols) was also 12/12 clean on kernel 3.2, so the
unflushed allocations don't reliably surface there (likely less page reuse on 248 MB vs B4's
106 MB; not proven).

**1.6 (separate problem):** B4 1.6 pbuffer 24t/150f = 20 HWR with **`BIF_INT_STAT=0`,
`BIF_FAULT=0` every time**, i.e. not a page fault. 1.6 already has the vmalloc flush. The same
TI 1.6 userspace on the Pandora's native 1.6 KM also fails (6/6, page faults), so this is
likely in closed 1.6 components. Not pursued further.

## Exonerated variables (each still storms on B4 → NOT the cause)

- **Cacheability**: `dma_alloc_coherent` and `gCachedBuffers=1` (noncoherent) both storm.
- **Pixel format**: RGB565 and ARGB8888 both storm.
- **Resolution**: 1024×600 and 800×480 both storm.
- **Display class**: `dc_nohw` (B4) is not in the render path; Pandora’s `omaplfb` does the
  identical SPM and is clean.
- **The SPM / present op itself**: Pandora does the **identical** 3‑kick SPM
  (`0x4d` = 3/frame, byte‑identical counts to B4‑1.4) and is clean.
- **SGX clock / APM / HWRecovery config**: identical between B4 and Pandora
  (~110 MHz, APM 500 ms, HWRecovery 100 Hz).
- **Ukernel build (2514 vs 2616)**: **REFUTED as causal** — matched 12‑sample clean‑boot testing
  gives ~42 % FLIP‑hang for **both** (see Corrected result). Swapping to 2514 does **not** fix it.

## Conclusion

- **The ukernel‑build A/B is refuted.** On identical B4 hardware, kernel‑7.2, patched 1.4 KM,
  `dc_nohw`, and workload, **2514 and 2616 FLIP hang at the same ~42 % rate** across 12 matched
  clean‑boot samples each. The earlier “2616 stalls every frame / 2514 clean” reading was a
  small‑sample artifact on a dirty board (a 30‑frame 1 fps run hangs ~42 % of the time either way).
- **The 1 fps FLIP hang is a sporadic single‑HWR event**, not a deterministic per‑frame storm,
  and it is **not ukernel‑build‑specific**. It is a B4‑environment present‑path hang
  (kernel‑7.2 / `dc_nohw` / board) that affects both ukernels equally.
- **Pandora** (2514 / 2.6.27 / `omaplfb`) is fully clean — but that is the **environment**
  (older kernel + real DSS/omaplfb), **not** “the 2514 ukernel”; on B4/7.2/`dc_nohw` the 2514
  ukernel hangs as much as 2616. **Angstrom** (1.6 / 3.0.14) could not isolate kernel‑vs‑ukernel
  because its `omaplfb` FLIP presenter deadlocks.

**Fix (2026‑09‑28):** the Nokia‑derived cache maintenance, now
fork commit [`9413c56a6`](https://github.com/rggammon/linux_openpvrsgx/commit/9413c56a6)
on `users/rgammon/pvrsgx-1.4.14.2616`, built by [tools/build-graft-14.sh](../tools/build-graft-14.sh)
(see **Root cause**). This supersedes the earlier “B4‑environment present‑path” direction.
Re‑pointing `sgx-ddk14` to 2514 remains unnecessary.

> Independent of this: **CPU pixel‑correctness cannot be verified on B4** — `glReadPixels`
> returns all‑zero on this SGX530 DDK (window FRONT/FLIP, FBO, colored `glClear` + `glFinish`
> all read black with `err=0`), and `dc_nohw` has no scanout fb to read. The `TR_READBACK`/
> `TR_DUMP` path in `tilerepro.c` is kept for stacks that implement readback (e.g. Pandora/
> Angstrom `omaplfb` + real fbdev).
