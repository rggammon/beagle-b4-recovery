# pvrsrvkm `PVRSRVProcessQueues` teardown-race Oops — handoff

Handoff for a **focused kernel/DDK debugging chat**. This is a use-after-free
kernel Oops in the PowerVR Services command-queue processor, hit while running
GLES apps (femtovg/tile-demo, Slint) that exit abnormally. It is a **Stage-0
stability defect** for the DDK 1.6 presentation work (it corrupts global queue /
buffer-manager state and wedges SGX), so it belongs with the kernel effort in
[ddk16-dcnohw-presentation-plan.md](ddk16-dcnohw-presentation-plan.md), not the
app chats. It is the same memory-lifetime family already noted there
("kill -9 wedges SGX", the Stage-6b `CmdComplete`-from-wrong-context
`HASH_Remove` crash) and in [btt-hdmi7-omapdrm.md](btt-hdmi7-omapdrm.md)
(`EUR_CR_BIF_FAULT` on process teardown).

## Summary

The `pvr_timer` workqueue periodically runs `PVRSRVProcessQueues`, which walks
the SGX command-queue linked list. When an SGX client process dies abnormally
(Rust `panic` → `SIGABRT`, e.g. the `eglCreateWindowSurface: NotInitialized`
path), its command queue is torn down and its vmalloc'd command buffer freed —
but the timer can be mid-walk over the global queue list with no lock serializing
the two, so it dereferences a just-freed node → Oops. Each abnormal exit also
leaves the global queue / buffer-manager state a little more corrupted, which
explains the degrading run-to-run numbers.

## The Oops (verbatim key lines)

```text
Unable to handle kernel paging request at virtual address c809d174 when read
[c809d174] *pgd=8189b811, *pte=00000000, *ppte=00000000
Internal error: Oops: 7 [#1] SMP ARM
CPU: 0 UID: 0 PID: 3013 Comm: kworker/0:4 Not tainted 7.2.0-g214a35fbc02e-dirty #1 VOLUNTARY
Hardware name: Generic OMAP3-GP (Flattened Device Tree)
Workqueue: pvr_timer OSTimerWorkQueueCallBack [pvrsrvkm]
PC is at PVRSRVProcessQueues+0x1c0/0x380 [pvrsrvkm]
LR is at PVRSRVProcessQueues+0x154/0x380 [pvrsrvkm]
pc : [<bf52eb30>]    lr : [<bf52eac4>]    psr: 800f0013
r10: ffffffff  r9 : bf54a7dc  r8 : 000003e8
r7 : c37b13e8  r6 : c402fbc0  r5 : c402fbc0  r4 : 00000001
r3 : c37b1410  r2 : c809d168  r1 : 00000044  r0 : c37b1428
fp : c37b1000  ip : 00000004  sp : c818dee0
Code: e1500003 9a00000e e8931004 e5922000 (e592100c)
Call trace:
 PVRSRVProcessQueues [pvrsrvkm] from OSTimerWorkQueueCallBack+0x20/0x40 [pvrsrvkm]
 OSTimerWorkQueueCallBack [pvrsrvkm] from process_one_work+0x184/0x480
 process_one_work from worker_thread+0x198/0x36c
 worker_thread from kthread+0xfc/0x134
 kthread from ret_from_fork+0x14/0x20
```

Register / slab context from the dump:

- `r0/r3/r7/r11(fp)` all point into **one `kmalloc-2k` object at `c37b1000`**
  (offsets 0, 1000, 1040, 1064) — a ~2 KB struct being processed (a queue /
  command-list head).
- `r5 == r6 == c402fbc0` — a `kmalloc-64` object (small sync/command struct).
- `r2 = c809d168` — **`vmalloc` memory, and `*pte=0` (unmapped/freed)** — the
  dangling node.
- `r9 = bf54a7dc` — the pvrsrvkm module's own 11-page vmalloc region.

## Decode

The faulting instruction is `Code: ... e5922000 (e592100c)`:

- `e5922000` = `ldr r2, [r2]` — follow the **next pointer at offset 0** of the
  current node.
- `e592100c` = `ldr r1, [r2, #12]` — read a **field at offset +12** of that next
  node → **faults** because `r2 = c809d168` is a freed/unmapped `vmalloc` page.

So a still-linked node's next-pointer referenced `c809d168`, which had already
been freed. Classic **unlink-ordering / missing-lock use-after-free** in a
linked-list walk. The freed `vmalloc` block is almost certainly a per-queue
command buffer (`PVRSRV_QUEUE_INFO`'s vmalloc'd ring), `vfree`'d on queue
destruction while the queue was still reachable from the timer's walk.

## Root cause (hypothesis to confirm)

Race between **SGX command-queue teardown** and the **`pvr_timer`
command-queue processor**:

1. A client process dies abnormally → RESMAN/cleanup runs
   `PVRSRVDestroyCommandQueueKM` (or the per-process cleanup) and frees the
   queue + its vmalloc command buffer.
2. Concurrently, `OSTimerWorkQueueCallBack` → `PVRSRVProcessQueues` walks the
   global queue list.
3. The two are not serialized by a common lock (or destroy unlinks/free in the
   wrong order), so the timer dereferences the just-freed node → Oops.

## BIF precursor investigation (2026-09-20)

The persistent log shows that the CPU Oops is downstream of an extended SGX
hardware-recovery storm. Timestamp-gap segmentation places the continuous fatal
storm at `32105.210815` through `32228.823516`; recoveries resume at
`32236.514434` and `32237.005035` immediately before the Oops. The later
`32198.679` recovery is not the beginning of the storm. Earlier, separated
recovery clusters belong to preceding attempts.

The dominant recovery state is:

```text
EUR_CR_BIF_INT_STAT:     00004002
EUR_CR_BIF_FAULT:        0F0AB000
```

`BIF_INT_STAT=0x4002` is a BIF page-fault request with `PF_N_RW` set, so this
is a GPU read fault. In the SGX103 28-bit heap layout, `0x0F0AB000` is in the
shared SGX kernel-data heap. A later recovery at `32208.011657` faults at
`0x000E5000`, in the optional general-mapping heap. Kernel CCB read and write
offsets advance between recoveries (including through wraparound), rather than
remaining stuck on one command. Recovery is therefore retiring or moving past
commands while the selected address space remains unusable.

The open DDK teardown path provides a concrete explanation:

1. `SGXCleanupRequest()` schedules a microkernel cleanup command and polls for
   completion, but returns `void`. Schedule or poll failure only emits a debug
   message / `PVR_DBG_BREAK`.
2. The render, transfer, and 2D context RESMAN callbacks ignore that outcome,
   free their cleanup records, and return `PVRSRV_OK`.
3. RESMAN subsequently frees `RESMAN_TYPE_DEVICEMEM_CONTEXT`.
4. `BM_DestroyContextCallBack()` calls `MMU_Finalise()`, which zeros and frees
   the complete client page directory without an SGX cleanup handshake of its
   own.

Client page directories contain copied shared-heap PDEs from `MMU_InsertHeap()`.
If hardware or recovery still selects a client directory after a failed cleanup,
freeing that directory makes both shared kernel addresses (`0x0F0AB000`) and
client/general addresses (`0x000E5000`) fault. This matches the observed
two-address sequence better than an isolated freed shader or render allocation.
The behavior is present in TI's imported DDK (`2d28cad7f`), not introduced by
the Linux 7.2 port.

This is still a hypothesis: the release log does not expose the active BIF
directory-list base or cleanup result, and debug messages may be suppressed.
It may explain why the storm persists or changes fault address during teardown,
but current evidence does not prove that context destruction caused the first
`0x0F0AB000` fault at `32105.210815`.
The decisive next build should add bounded, first-fault diagnostics only:

- On `SGXCleanupRequest()` schedule/poll failure: cleanup type, hardware-data
  device VA, cleanup status, and kernel CCB read/write offsets.
- Immediately before `MMU_Finalise()` clears a client directory: context
  pointer, page-directory device physical address, and whether cleanup failed
  for that process.
- In `SGXDumpDebugInfo()`, before `SGXInitialise()` starts the reset: fault VA,
  read/write state, `EUR_CR_BIF_DIR_LIST_BASE0`, matching software MMU context
  (if any), and raw PDE/PTE values for the fault VA. SGX530 uses this single
  active directory-list register; `SGXReset()` later overwrites it with the BIF
  reset PD, so capturing it inside the reset loop is too late.

The hypothesis is confirmed if the first fault's active directory base equals
a client PD that was finalized after an unsuccessful cleanup. It is disproved
if the active PD is still live and its PDE/PTE were valid at the first fault.

## Trigger / reproduction

- Seen while running `femtovg-probe` / `tile-demo` (Slint) under
  `sgxmode` + `sgx-ddk16-hf-run`; the app **panics** (Rust `panic` → `SIGABRT`),
  including `eglCreateWindowSurface: NotInitialized`. Each abnormal exit tears
  down the SGX context.
- To reproduce deliberately: loop an SGX client that dies mid-render
  (`kill -9` of a rendering process, or the panicking demo) and count Oopses.

## Environment / source / build

- Board: BeagleBoard Rev B4, OMAP3530 ES2.1, SGX530 SGX103, `192.168.50.245`,
  key `~/.ssh/geoduck_truenas`.
- **Running kernel: `7.2.0-g214a35fbc02e-dirty`** (this session's CI kernel; note
  the presentation-plan Handoff Card still lists the older `ge2b9d0eea907` — use
  `214a35fbc02e` here). Fork `rggammon/linux_openpvrsgx`, branch
  `users/rgammon/pvrsgx-1.6.16.3977`.
- Source of interest:
  - `PVRSRVProcessQueues` — Services core, `services4/srvkm/common/queue.c`
    (the queue walk + the destroy path `PVRSRVDestroyCommandQueueKM`).
  - `OSTimerWorkQueueCallBack` / the `pvr_timer` workqueue — Services OS layer.
  - `dc_nohw` `ProcessFlip` — `services4/3rdparty/dc_nohw/dc_nohw_displayclass.c`
    (a queue producer; relevant to how CmdComplete interleaves).
- Module build (match `LOCALVERSION` to the running kernel `-g214a35fbc02e-dirty`):

  ```sh
  make -C <openpvrsgx-worktree> \
    ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- \
    LOCALVERSION=-g214a35fbc02e-dirty \
    M=drivers/gpu/drm/pvrsgx \
    CONFIG_SGX=m CONFIG_SGX_OMAP=m \
    CONFIG_PVRSGX_1_6_16_3977=y \
    CONFIG_PVRSGX_1_6_16_3977_DC_NOHW=m \
    modules
  ```

- The board's `pvrsrvkm.ko` is **stripped**; use the **unstripped** build (or the
  `build-devuan` CI artifact for ref `214a35fbc`) for symbol/source resolution.

## Diagnostic assets already in place (2026-09-19)

- **Persistent logging** now ships in the Devuan image and is live on the board:
  `busybox-syslogd` + `busybox-klogd` → `/var/log/messages` (wall-clock
  timestamps, survives reboots) so repeat Oopses are captured and countable.
  Committed `0cae878`. (Debian's busybox syslogd has no rotation — the log grows
  during SGX churn; watch disk on long soaks.)
- `loglevel=4` on the kernel cmdline quiets the `PVR_K` console spew; **Oops /
  panic output overrides console loglevel and still prints**, so nothing is lost.
- `gdb` is on the board and in the image.

## Investigation plan

1. **Resolve the offsets to source lines** (offline, no board):
   `arm-linux-gnueabihf-objdump -dr pvrsrvkm.ko` (unstripped), find
   `PVRSRVProcessQueues`, read `+0x1c0` (fault) and `+0x154` (LR). Confirm the
   `ldr r2,[r2]` / `ldr r1,[r2,#12]` pair and identify **which list** is walked
   and **which struct field** is at offset +12 (and the next-ptr at offset 0).
2. **Read the walk vs. the destroy path** in `queue.c`: what lock (if any)
   `PVRSRVProcessQueues` holds across the list walk, and whether
   `PVRSRVDestroyCommandQueueKM` / per-process RESMAN cleanup unlinks-then-frees
   under that same lock. Look for the `vfree` of the queue command buffer.
3. **Confirm the race** by correlating `/var/log/messages` Oopses with preceding
   client `SIGABRT`/exit events; get a reliable repro loop.
4. **Fix direction:** serialize `PVRSRVProcessQueues` against queue
   create/destroy — hold the queue-list lock across the walk, and make destroy
   unlink under the same lock **before** freeing (and/or quiesce/drain the
   `pvr_timer` processing during abnormal-process RESMAN teardown). Keep it a
   minimal, reviewable patch (one stated purpose).
5. **Validate** with the plan's Stage-0 soak guard after the kernel change:
   load/unload cycles, IRQ 37 rising, memory flat, and **zero**
   Oops/BUG/HASH/HWRecovery/BIF — including a deliberate abnormal-exit stress
   (loop the panicking/`kill -9` client) that previously reproduced it.

## Engineering rules (from the presentation plan)

- Work in the DDK 1.6 OpenPVRSGX worktree, not generated bundles; SGX103-only
  config, strict version match.
- Separate reviewable patch series; re-run the Stage-0 probe after every
  kernel-side change; keep instrumentation bounded/removable.
- Do not inspect proprietary EGL/GLES structures or command streams — this is a
  Services-core locking/lifetime fix, entirely in open source.

## Open questions

- Exact list + struct: is the walked list the global `psQueue` chain, and is the
  freed `vmalloc` node the queue's command ring (`pvLinQueueKM`) or a
  `COMMAND_COMPLETE_DATA`? (Resolve in step 1.)
- Is the right lock the per-device queue lock, the global resource lock, or the
  OMAP power lock (cf. patch 0009, which already reworked power-lock ordering in
  the ISR/workqueue path)? The fix must not reintroduce the 0008/0009 deadlock.
- Does abnormal teardown need to **cancel/flush** the `pvr_timer` work before
  freeing queues, in addition to locking the walk?
