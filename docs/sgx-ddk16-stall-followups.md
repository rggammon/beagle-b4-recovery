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
*visible* jitter that never trips the 100 ms watchdog. Then first capture per-frame **max/p99**
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
  *trigger-type metadata* stays `IRQ_TYPE_NONE`; `/proc/interrupts` prints anything that isn't a LEVEL
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

**Decision — leave as-is.** These *over*-flush, so they're conservative/safe, and 2000 frames
rendered with no corruption or wedge at native performance — neither a correctness nor a perf problem
here.

**Reconsider if:** we ever see rendering corruption or coherency wedges (distinct from the known
`kill -9` wedge, which is a user-behaviour caveat, not a cache bug).

## 6. HW-recovery / lockup timer — VERIFIED OK (closed)
**Checked on hardware:** no HW-recovery / `SGXOSTimer` / lockup / reset messages in dmesg across the
2000-frame soak — the recovery timer is not false-triggering. No action needed.

---

## Bottom line
With `0008` (IRQ) + `0009` (APM) the DDK 1.6 stack is stable at native parity over a 2000-frame soak.
**None of the remaining items is worth pursuing** — #3/#4/#6 are verified correct/clean on hardware
(the "Edge" label is cosmetic; the IRQ is handled as level), and #2/#5 have no measured symptom and
carry change-risk. #2 and #5 keep a concrete "reconsider if" trigger above; revisit only when a real
workload exhibits it.

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
