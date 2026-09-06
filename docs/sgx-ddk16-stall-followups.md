# SGX DDK 1.6 — residual stall follow-ups (post IRQ fix)

Candidates found while the IRQ-fix build ran (2026-09-06). The **confirmed root cause**
was the SGX interrupt mapping — DDK requested raw IRQ 21 (= virq 21 = hwirq 5, wrong line),
so the completion interrupt never fired (`/proc/interrupts`: `21: 0 INTC 5 Edge SGX ISR`,
count stuck at 0) and every `PVRSRVEventObjectWait` timed out. Fixed by
[kernel/patches-devuan/0008-pvrsgx-sgx-irq-37.patch](../kernel/patches-devuan/0008-pvrsgx-sgx-irq-37.patch)
(21 → 37). The clock patches (`0005`/`0007`) were red herrings — the DDK module self-sets
`sgx_fck` to 110.67 MHz on load.

Strategy: **soak the IRQ-only image first.** Native Angstrom ran DDK 1.6 clean (27–32 ms/frame,
no flood) with the *same* 1 ms APM and workqueue MISR — the only thing it got right that we
didn't was interrupt delivery. So the IRQ fix alone may be sufficient. The items below only
matter for *residual* jitter/outliers/wedge and should be applied (and bisected) only if the
IRQ-only soak still shows problems. Prefer full build + flash over SD-card patching.

All source line refs are in the DDK tree on the fork branch
(`rggammon/linux_openpvrsgx`, `users/rgammon/pvrsgx-1.6.16.3977`),
under `drivers/gpu/drm/pvrsgx/1.6.16.3977/`.

---

## 1. APM at 1 ms + `ti-sysc` idle ownership  — TOP suspect
**What:** `SUPPORT_ACTIVE_POWER_MANAGEMENT` is on and `SYS_SGX_ACTIVE_POWER_LATENCY_MS = 1`
(`services4/system/omap3/sysconfig.h:44`), so the DDK powers the SGX **down after 1 ms idle** —
i.e. it power-cycles the core every frame in a serialized `-ser 1` render. Meanwhile the DT
wraps SGX in a **`ti,sysc` target-module** (`sgx_module: target-module@50000000`,
`compatible = "ti,sysc-omap2"`, `omap34xx.dtsi`) that manages idle/standby/reset via
`pm_runtime`, but the DDK uses **raw `clk_enable` + PRCM pokes with no `pm_runtime`**
(no `pm_runtime_*` anywhere in the DDK linux/system code). Two owners of the module's power
state. `&sgx_module` has **no `ti,no-idle`** (the board dts applies it only to the timer
modules), so ti-sysc is free to auto-idle SGX under the DDK.

**Why it matters:** a power-down/up straddling the render→completion window can lose or delay
the completion IRQ — the likely cause of the >500 ms/3.3 s outliers and the teardown hang.

**Experiments (cheap, isolated):**
- **DT-only** (dtb rebuild + reflash): add `ti,no-idle;` to `&sgx_module` in the board dts — new
  `patches-devuan/` patch. Pins the module active so ti-sysc can't idle it.
- **Define-only** (module rebuild): raise `SYS_SGX_ACTIVE_POWER_LATENCY_MS` to ~500, or build
  without `SUPPORT_ACTIVE_POWER_MANAGEMENT`. If outliers vanish → APM/idle sequencing confirmed.

## 2. MISR on a normal-priority single-threaded workqueue
**What:** the build selects `PVR_LINUX_MISR_USING_PRIVATE_WORKQUEUE` (`Makefile:348`). Path is
hard IRQ (LISR) → `queue_work(pvr_workqueue, …)` → MISR runs in **process context on a
normal-priority, single-threaded workqueue** (`create_singlethread_workqueue("pvr_workqueue")`,
`services4/srvkm/env/linux/osfunc.c:760`), and only there is the global event object signalled.

**Why it matters:** under contention that thread can be preempted → tens of ms jitter → borderline
frames cross the fixed 100 ms wait *even with the IRQ firing correctly*.

**Fix (module rebuild):** switch to the **tasklet** MISR (softirq, runs right after the IRQ) —
the DDK already ships it (`osfunc.c:917` `tasklet_init` / `tasklet_schedule`). Drop the
`MISR_USING_*WORKQUEUE` defines (→ tasklet), or as a lighter touch use `WQ_HIGHPRI`.

## 3. Interrupt trigger type — verify-only, after 0008
**What:** `/proc/interrupts` showed the (bogus) line as **Edge**, and the DDK requests with
**`IRQF_SHARED`** (`osfunc.c:640`/`694`). The DT `gpu@0` uses single-cell `interrupts = <21>`
(no trigger flag). The SGX host/MMU IRQ is **level-sensitive** on OMAP3.

**Why it matters:** an edge registration can drop a completion that coincides with another assert.

**Check:** once `0008` lands, confirm virq 37 shows **Level** (not Edge) in `/proc/interrupts`.
If it's Edge, fix the omap-intc/DT interrupt type. Low effort.

## 4. GPT11 availability / clkdev resolution
**What:** `EnableSystemClocks` grabs `gpt11_fck`/`gpt11_ick` via `clk_get(NULL, …)` and reparents
GPT11 to `sys_ck`, posted mode (`services4/system/omap3/sysutils_linux.c:570–617`) for the DDK's
microsecond timer (drives APM/timeouts). We already moved the kernel clockevent/clocksource off
GPT11 to GPT2/GPT12, so there's **no ownership conflict** — but on CCF `clk_get(NULL, "gpt11_fck")`
needs a clkdev alias.

**Check:** `dmesg | grep -i GPTIMER11` for "Couldn't get", and confirm GPT11 actually ticks. If the
DDK timer is dead, its APM/timeout logic misbehaves regardless of #1. Low effort.

## 5 (lower). Cache / DMA coherency in the kick path
Coarse `flush_cache_all` / `outer_flush_all` (`osfunc.c` ~2824/2832). More a corruption/wedge risk
than a clean lost-IRQ stall, but relevant to the kill-wedge and teardown hang. Chase only if 1–3
don't fully settle it.

## 6 (lower). HW-recovery / lockup timer
Ensure the 1.6 HW-recovery timer isn't false-triggering a GPU reset that looks like a stall. Check
config + `dmesg` for HW-recovery / `SGXOSTimer` messages.

---

## Priority
1. **#1 (APM + `ti,no-idle`)** and **#2 (MISR → tasklet)** — most likely residual causes; small,
   isolated changes; good batch-2 patches if the IRQ-only soak shows residual issues.
2. **#3, #4** — verify-only, do during the first soak.
3. **#5, #6** — only if the above don't fully resolve.

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
