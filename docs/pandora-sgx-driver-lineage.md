# Pandora SGX driver lineage: why 2514 and 2616 appear together

The OpenPandora (SuperZaxxon firmware) is our clean control board for SGX530 r1.0.3. Its kernel
module says it is DDK **1.4.14.2514**, but its userspace is **1.4.14.2616**. This note records why
the two are mixed, what the Pandora itself does, and what the community sources say. It matters
because the Pandora's default kernel module is Nokia's N9 driver, which is where our 1.4 cache
fix (fork commit [`9413c56a6`](https://github.com/rggammon/linux_openpvrsgx/commit/9413c56a6)
on `users/rgammon/pvrsgx-1.4.14.2616`)
comes from.

## On the device (SuperZaxxon, kernel 3.2.84, checked 2026-09-28)

| Item | Value |
| --- | --- |
| Loaded KM (`/proc/pvr/version`) | `Version 1.4.14.2514 (release)`, `SGX revision = 1.0.3` |
| Loaded KM file | top-level `/lib/modules/3.2.84/kernel/drivers/gpu/pvr/pvrsrvkm.ko` (+ `omaplfb`, `bufferclass_ti`) |
| Other KMs shipped | `pvr/1.4.14.2616/`, `1.5.15.2766`, `1.6.16.3977`, `1.6.16.4117`, `1.7.17.*`, `1.9.19.*`, `1.10.2359475` |
| Userspace in use (`/usr/lib/libGLESv2.so`) | 1.4.14.2616 |
| `/usr/lib/ES2.0` (SGX 1.0.3, this board, the B4) | 1.4.14.2616 |
| `/usr/lib/ES3.0` (SGX 1.2.1, Rebirth) | 1.4.14.2616 |
| `/usr/lib/ES5.0` (SGX 1.2.5, 1 GHz DM3730) | 1.6.16.3977 |
| `/etc/powervr-kmodver` | empty |
| `/etc/powervr-esrev` | `2` |

Selection logic in `/etc/init.d/pvr-init`:

- Reads the SGX core revision register (`0x50000014`) and maps `0x10003`→ES2, `0x10201`→ES3,
  `0x10205`→ES5, then installs that directory's libraries into `/usr/lib`.
- Loads `pvr/$(cat /etc/powervr-kmodver)/pvrsrvkm.ko`. An empty file means the top-level module.
- Only writes a version into `powervr-kmodver` when the userspace is **not** 2616, with this
  comment (the only in-system explanation):

  > for 1.4.14.2616 there are 2 incompatible modules,
  > assume the default one, which is selected by empty powervr-kmodver

So for 2616 userspace the firmware deliberately loads the top-level (Nokia, reports 2514) module,
not TI's own module in `pvr/1.4.14.2616/`.

## What the sources say

- **Pandora Wiki, [SGX drivers](https://web.archive.org/web/2016/http://pandorawiki.org/SGX_drivers)**
  (archived; the live page no longer renders). TI SDK → DDK mapping:

  | TI SDK | DDK | Note |
  | --- | --- | --- |
  | 3.00.00.08a / 3.00.00.09 | 1.3.13.1607 / 1.3.13.1832 | |
  | 3.01.00.02 | **1.4.14.2514** | |
  | 3.01.00.06 / 3.01.00.07 | **1.4.14.2616** | ES5 introduced |
  | 4.00.00.01 | **1.4.14.2616** | **"last working ES2"** |
  | 4.03.00.0x | 1.6.16.3977 | |
  | 4.04.00.0x | 1.6.16.4117 | 4.04.00.01 introduces the DRI2 dependency |
  | 4.05.00.01 | 1.6.16.4117 | "no more ES2 libs" |

- **OpenPandora build recipe
  [`omap3-sgx-modules_4.00.00.01.bb`](https://github.com/openpandora/meta-openpandora/blob/master/recipes-graphics/libgles/omap3-sgx-modules_4.00.00.01.bb)**:
  the kernel module packaged for the 4.00.00.01 (2616) userspace is described as the
  **"nokia version"**, built from `git://git.openpandora.org/sgx.git`, branch `nokia`. Last change
  "update for new kernel" (2013). The package is versioned as the 2616 driver, but the code is
  Nokia's 2514-based N9 module, which is why the running module reports 2514.
- **["SGX driver outdated"](https://pyra-handheld.com/boards/threads/sgx-driver-outdated.69750/)**
  (Jan 2013), notaz:

  > that's the last driver where original pandoras work (SGX530 v1.0.3), any newer ones will lock
  > up the unit hard

  > Any driver newer than 1.4.14.2616 for SGX530 core version 1.0.3

  TI's answer when asked was "your chip is not supported".
- **["SGX driver installer (beta)"](https://pyra-handheld.com/boards/threads/sgx-driver-installer-beta.70399/)**
  (2013–2016), notaz's driver-switcher `.pnd`:

  > There is no such thing as "correct driver", all of them are broken in their own ways.

  Also: 4.04.00.03 is the last TI release containing ES2 (SGX 1.0.3) libraries; releases after
  4.00.00.01 up to 4.04.00.04 install on ES2 but are broken.
- **["Latest Sgx Driver"](https://pyra-handheld.com/boards/threads/latest-sgx-driver.55442/)**
  (2011): random hard freezes on several drivers in X (windowed/blit) mode that disappeared when the
  app rendered to the framebuffer (flip) mode instead. No root cause was found. This is a different
  present path from our dc_nohw FLIP hang, so it is related only loosely.

## How this fits our work

- **Stay on 2616 userspace.** Both the wiki ("last working ES2") and notaz put 1.4.14.2616 as the
  last usable DDK for SGX 1.0.3. The B4 and the Pandora both run it, and it's the default in
  `.github/workflows/build-sgx-ddk14-packages.yml`.
- **The Pandora's "clean 1.4" result comes from Nokia's kernel module, not TI's.** Our B4 KM is
  TI's 2616 source plus the Nokia `inv_cache_mem_area` fix
  ([docs/sgx530-12tile-fringe-flip-results.md](sgx530-12tile-fringe-flip-results.md)). That
  matches what OpenPandora ships: 2616 userspace over a Nokia-derived kernel module.
- The 1.6 lock-ups we see on SGX 1.0.3 agree with notaz's "any newer ones will lock up the unit
  hard".

## sgx.git history (checked 2026-09-29)

`git.openpandora.org/sgx.git` no longer answers (git://, http and https all fail). A full mirror
is at **[JonnyH/pandora-sgx-module](https://github.com/JonnyH/pandora-sgx-module)** ("OpenPandora
SGX binaries forked from git.openpandora.org"), cloned on geoduck at
`/mnt/scratch/geoduck-tmp/beagle/pandora-sgx-module`. It has one branch per KM that SuperZaxxon
ships (`1.4.14.2616`, `1.5.15.2766`, `1.6.16.*`, `1.7.17.*`, `1.9.19.*`, `1.10.2359475`), plus
`nokia`.

**`nokia` branch (the default KM, 201 commits):**

| Commits | Author | What |
| --- | --- | --- |
| `ba7179e` | Imagination/TI | root: 1.3.13.1607 / TI 3_00_00_08, flattened + Lindent |
| `a8cf003`, `52fbd97`, … | Nokia | N900 releases (`20093908+0m5`, `20094102.3+0m5`, …) |
| `9d43dbb` | Nokia | "meego-device-adaptation import version": the N9 driver. **Introduces `inv_cache_mem_area`** |
| 2011-03 … 2011-05 | Imre Deak, Luc Verhaegen, Alex Crowther (Nokia) | upstream-style fixes: error paths, sync-counter completion checks, duplicate src-sync check in kick, refcount leak, HWR command trace, pdumpfs rewrite |
| `b929bae` … `1ab3b0f` (2012-05-19) | Grazvydas Ignotas (notaz) | make standalone, drop mainline-only features, out-of-tree build, newer kernels |
| `046cdeb` (2012-05-21) | notaz | revert "remove build time ABI dependency on the EDM trace option" |
| **`7772693`** (2012-05-20) | notaz | **"try to make ABI compatible with TI 1.4.14.2616/4_00_00_01 release"** |
| `5867a7d` (2012-05-20) | notaz | "support multibuffering with panning": "performance goes down to ~50% for some things without multibuffering" |
| `227a3f2`, `cb0a076` (2012–2013) | notaz | proc entry fix; newer kernel DMA code |
| `c58ea6e` … `1ec5b45` (2014-07-04) | notaz | add TI `bc_cat` and port it |

`7772693` makes Nokia's bridge match TI's 2616 userspace. It:

- removes the `PVRSRV_BRIDGE_CACHE_FLUSH_DRM` ioctl, so `CORE_CMD_LAST` goes back to +27;
- reorders `enum pvr_sync_wait_seq_type` so `_PVR_SYNC_WAIT_NONBLOCK = 0`;
- drops `user_data` from `PVRSRV_BRIDGE_IN_2DQUERYBLTSCOMPLETE`;
- adjusts `SGX_MAX_3D_STATUS_VALS` and `SGXDevInitPart2`.

So the Nokia N9 kernel driver (DDK 2514 base) runs TI's 2616 userspace only because of this
shim.

**`1.4.14.2616` branch (the alternative KM):** not pristine TI either. It starts from TI's
`4_00_00_01/1.4.14.2616 release` (`ae4d93b`) and notaz then:

- adds 2.6.27 fixes and "random kernel version compatibility hacks";
- backports `bc_ioctl` compat;
- in May 2012, merges the headers, the common and linux layers, and the omap3 and core parts from
  **1.5.15.2766**.

`mm.c`/`osfunc.c`/`mmap.c` changed by about 2150 lines. Cache maintenance on allocation is still
only TI's `SUPPORT_CACHEFLUSH_ON_ALLOC` (a global `flush_cache_all()`), which defaults to `0` in
its Makefile, the same as ours.

**Our B4 1.4 import is this branch.** Our import `f90eeed54` is notaz's `1.4.14.2616` head plus
kernel 7.2 API changes: `include4/` and `services4/include/` are identical to it, and the rest
differs only in porting. What the port removed:

- the OMAP3630 clock and OCP code;
- old kernel APIs (`init_timer`, `access_ok(type, …)`, `VM_RESERVED`, `asm/system.h`);
- the unused `CPUVAddrToPage`.

So notaz's 1.5.15.2766 merges are already in what the B4 runs.

The only 1.5.15 work notaz held back is `7dceb88` "merge ABI-destructive 1.5.15.2766 changes",
on his `1.5.15.2766` branch. It re-enables code that he had `#if 0`'d so 2616 userspace would
still work: build-option and struct-size checks, per-command kick addresses, the ukernel
init-status poll, dst-sync loops, and the `psHWBlockKernelMemInfo` resman dissociate in `pb.c`.
It is userspace-ABI work, not bug fixes, and can't apply to 2616 userspace. For the `pb.c`
dissociate, our code matches TI's own 2616.

## Fix candidates from the `nokia` branch

Nokia's 2011 fixes on the `nokia` branch (N9 bug tracker `NB#` numbers) are the better source.
Checked against our tree (2026-09-29):

| Commit | Fix | Applies to us? |
| --- | --- | --- |
| `39828db` (Alex Crowther, NB#233069) | sync completion checks use `>=` on u32 counters; wrong after a 2^32 wrap. Uses `(int)(c1 - c2) >= 0` | **Yes**: `devices/sgx/sgxutils.c:870` (2D query blits) and `common/queue.c:167`. Rare (needs 2^32 ops on one sync object) |
| `68eb886` (Luc Verhaegen, NB#254225) | duplicate src syncs in one TA kick deadlock the GPU; skip duplicates in `SGXDoKickKM` | **Yes**: our `sgxkick.c` copies `ahSrcKernelSyncInfo[]` without a duplicate check (lines ~187 and the rollback loop ~491). Nokia also fixed it in its own userspace; unknown whether TI 2616 userspace can send duplicates |
| `9267c45` (Imre Deak, NB#245525) | memory-context handle leaked when the context is destroyed before its last buffer; adds an open count | Likely: same bridge/devicemem code. Leak only, per process |
| `315afd6` (Imre Deak) | `sizeof(ptr)` vs `sizeof(*ptr)` | No: Nokia's own debugfs code |

## Still unexplained

- **Why OpenPandora defaulted to the Nokia module.** No commit message says. The history shows it
  had two more years of Nokia engineering (N900 to N9) and notaz's porting and multibuffer work
  on top. The two KMs are separate codebases, with different bridge/ABI history and different
  kernel-side interfaces, which is the likely meaning of "2 incompatible modules" (inferred, not
  stated anywhere).
- **Why the Pandora's `1.4.14.2616` KM is clean without the cache fix.** It has no per-allocation
  cache maintenance (flush-on-alloc off), and our B4 KM is the same source and hangs without the
  fix. So the 1.5.15.2766 merges are ruled out. What's left is the environment: kernel 3.2 vs 7.2,
  `omaplfb` vs `dc_nohw`, or 248 MB vs 106 MB RAM (page reuse) (see
  [ddk14-pivot-handoff.md](ddk14-pivot-handoff.md)).
