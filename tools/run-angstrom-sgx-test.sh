#!/bin/sh
# DDK 1.6 SGX event-wait control test for native Angstrom 2012.01
# (kernel 3.0.14+, omap3-sgx-modules 1.6.16.3977 -- identical DDK to the Linux 7.2 port).
# Uses Angstrom's native pvrsrvkm.ko/omaplfb.ko + the bundled DDK 1.6 userspace + probe.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
DDK="$HERE/opt/ti-ddk16"
ARMEL="$HERE/opt/pandora-armel"
KREL=$(uname -r)
LOG=/tmp/angstrom-sgx-test.log
: > "$LOG"
say() { echo "$@" | tee -a "$LOG"; }

say "=== angstrom sgx test: kernel=$KREL ==="

# 1. Native Angstrom SGX kernel module (its IRQ mapping is correct for this kernel).
if ! grep -q '^pvrsrvkm ' /proc/modules; then
    modprobe pvrsrvkm 2>>"$LOG" \
        || insmod "/lib/modules/$KREL/kernel/drivers/gpu/pvr/pvrsrvkm.ko" 2>>"$LOG" \
        || { say "FAIL: cannot load native pvrsrvkm"; exit 1; }
fi
i=0
while [ ! -e /dev/pvrsrvkm ] && [ "$i" -lt 10 ]; do sleep 1; i=$((i + 1)); done
[ -e /dev/pvrsrvkm ] || { say "FAIL: no /dev/pvrsrvkm"; exit 1; }
chmod 600 /dev/pvrsrvkm 2>/dev/null || true

say "=== SGX interrupt line (before init) ==="
grep -i SGX /proc/interrupts | tee -a "$LOG" || say "(no SGX line yet)"

# 2. Initialise the SGX ukernel with the bundled DDK 1.6 userspace (same version as the module).
if "$DDK/lib/ld-linux.so.3" --library-path "$DDK/runtime:$DDK/lib" "$DDK/runtime/pvrsrvinit" 2>>"$LOG"; then
    say "pvrsrvinit ok"
else
    say "pvrsrvinit returned $?"
fi

# 3. Native display-class provider (omaplfb) so EGL pbuffer init has a DisplayClass.
if ! grep -q '^omaplfb ' /proc/modules; then
    modprobe omaplfb 2>>"$LOG" \
        || insmod "/lib/modules/$KREL/kernel/drivers/gpu/pvr/omaplfb.ko" 2>>"$LOG" \
        || say "warn: omaplfb not loaded (EGL init may fail)"
fi

# 4. Repeated same-renderbuffer attachment probe (the case that stalls on the Linux 7.2 port).
BEFORE=$(grep -i SGX /proc/interrupts | awk '{print $2}')
"$ARMEL/lib/ld-linux.so.3" --library-path "$DDK/runtime:$ARMEL/lib" \
    "$ARMEL/bin/sgx-pbuffer-latency" 3000 0 1 0 \
    > /tmp/angstrom-probe.csv 2> /tmp/angstrom-probe.stderr
RC=$?
AFTER=$(grep -i SGX /proc/interrupts | awk '{print $2}')

say "=== SGX interrupt line (after render) ==="
grep -i SGX /proc/interrupts | tee -a "$LOG" || true
say "=== renderer ==="
grep '^egl=' /tmp/angstrom-probe.csv | tee -a "$LOG" || true
say "probe_rc=$RC frames=$(awk -F, '/^[0-9]/{n=$1} END{print n+0}' /tmp/angstrom-probe.csv) sgx_before=${BEFORE:-?} sgx_after=${AFTER:-?} slow_over_100ms=$(grep -c '^slow ' /tmp/angstrom-probe.stderr 2>/dev/null || echo 0)"
say "=== slow markers (attachment/finish waits >100ms) ==="
grep '^slow ' /tmp/angstrom-probe.stderr 2>/dev/null | head -20 | tee -a "$LOG" || say "(none -- no stalls)"
say ""
say "Full data: /tmp/angstrom-probe.csv  /tmp/angstrom-probe.stderr  $LOG"
