#!/bin/sh
# DDK 1.6 Stage 0 harness: run the EGL FBO attachment-churn probe on the fixed
# 1.6 stack (0008 IRQ + 0009 APM) and collect timing / device-memory / dmesg
# data. This is a *reproducer / data collector*, not a pass-fail gate — the
# rebind=1 mode is expected to leak device memory and OOM (that is the finding).
#
# Proven recipe (2026-09-06), mixed-libc loader isolation:
#   - modules come from the running kernel's tree (correct vermagic), NOT the
#     ti-ddk16 tarball (its prebuilt modules are for a different kernel);
#   - the es2.x GL libs + the 1.6 WSEGL come from GL_ROOT (/root/s16/gl);
#   - the probe runs under the modern armel libc (/opt/pandora-armel) so its
#     GLIBC_2.34 symbols resolve while it dlopens the old softfp GL libs.
#
# The DDK EGL needs a WSEGL window-system module + /etc/powervr.ini selecting it.
# soak16/gl ships WITHOUT any WSEGL, so eglInitialize fails until the 1.6 WSEGL
# (libpvrPVR2D_FLIPWSEGL.so, Version 1.6.16.3977, softfp) is staged into GL_ROOT.
# Source: beagle-archive/angstrom-sgx-test.tar -> opt/ti-ddk16/runtime/. See
# tools/README.md. FRONTWSEGL OOMs on dc_nohw's bogus geometry; use FLIPWSEGL.
set -eu

KMOD_DIR=${KMOD_DIR:-/lib/modules/$(uname -r)/kernel/drivers/gpu/drm/pvrsgx/1.6.16.3977}
SERVICES_MODULE=${SERVICES_MODULE:-$KMOD_DIR/pvrsrvkm.ko}
DC_MODULE=${DC_MODULE:-$KMOD_DIR/services4/3rdparty/dc_nohw/dcnohw.ko}
GL_ROOT=${GL_ROOT:-/root/s16/gl}
ARMEL_ROOT=${ARMEL_ROOT:-/opt/pandora-armel}
PROBE=${SGX_PROBE:-$ARMEL_ROOT/bin/sgx-pbuffer-latency}
PROBE_LOADER=${PROBE_LOADER:-$ARMEL_ROOT/lib/ld-linux.so.3}
WSEGL=${WSEGL:-libpvrPVR2D_FLIPWSEGL.so}
POWERVR_INI=${POWERVR_INI:-/etc/powervr.ini}
INIT_CMD=${INIT_CMD:-/root/s16/run.sh pvrsrvinit}

FRAMES=${FRAMES:-120}
PROBE_ALTERNATE=${PROBE_ALTERNATE:-1}
PROBE_USE_FBO=${PROBE_USE_FBO:-1}
PROBE_USE_TEXTURE=${PROBE_USE_TEXTURE:-1}
PROBE_REBIND_ATTACHMENT=${PROBE_REBIND_ATTACHMENT:-1}
RESULT_ROOT=${RESULT_ROOT:-/tmp/ddk16-stage0}

PROBE_LIBPATH="$ARMEL_ROOT/lib:$GL_ROOT"
FAULT_PATTERN='HWRecovery|BIF|watchdog|Oops|BUG:|SGXOSTimeout|LOCK_RESOURCE|Out of memory|Killed process'

fail() { echo "FAIL: $*" >&2; exit 1; }

require_positive_integer()
{
    case "$2" in ''|*[!0-9]*|0) fail "$1 must be a positive integer" ;; esac
}

require_boolean()
{
    case "$2" in 0|1) ;; *) fail "$1 must be 0 or 1" ;; esac
}

cma_kb() { awk '/^CmaFree:/ { print $2 }' /proc/meminfo; }
sgx_irq_count() { awk '/SGX ISR/ { print $2; exit }' /proc/interrupts; }

unload_modules()
{
    rmmod dcnohw 2>/dev/null || true
    rmmod pvrsrvkm 2>/dev/null || true
}

# Fresh module reload + pvrsrvinit. pvrsrvinit only returns 0 right after a
# clean load; a second run reports "already initialised" (non-zero) which is
# harmless, so its exit status is ignored here.
load_stack()
{
    unload_modules
    insmod "$SERVICES_MODULE"
    tries=0
    while [ ! -e /dev/pvrsrvkm ] && [ "$tries" -lt 20 ]; do
        sleep 1
        tries=$((tries + 1))
    done
    [ -e /dev/pvrsrvkm ] || fail "pvrsrvkm loaded without /dev/pvrsrvkm"
    insmod "$DC_MODULE"
    grep -qi 'SGX ISR' /proc/interrupts || fail "SGX IRQ is not registered"
    $INIT_CMD >/dev/null 2>&1 || true
}

[ "$(id -u)" -eq 0 ] || fail "run as root"
require_positive_integer FRAMES "$FRAMES"
require_boolean PROBE_ALTERNATE "$PROBE_ALTERNATE"
require_boolean PROBE_USE_FBO "$PROBE_USE_FBO"
require_boolean PROBE_USE_TEXTURE "$PROBE_USE_TEXTURE"
require_boolean PROBE_REBIND_ATTACHMENT "$PROBE_REBIND_ATTACHMENT"

for path in "$SERVICES_MODULE" "$DC_MODULE" "$PROBE_LOADER" "$PROBE"; do
    [ -e "$path" ] || fail "missing required file: $path"
done
[ -e "$GL_ROOT/$WSEGL" ] ||
    fail "$GL_ROOT/$WSEGL missing — stage the 1.6 WSEGL modules (see tools/README.md)"

services_release=$(modinfo -F vermagic "$SERVICES_MODULE" | awk '{print $1}')
[ "$services_release" = "$(uname -r)" ] ||
    fail "pvrsrvkm is for $services_release, running kernel is $(uname -r)"

[ -e "$POWERVR_INI" ] ||
    printf '[default]\nWindowSystem=%s\n' "$WSEGL" > "$POWERVR_INI"

mkdir -p "$RESULT_ROOT"
trap 'unload_modules' EXIT INT TERM HUP

uname -a > "$RESULT_ROOT/uname.txt"
sha256sum "$SERVICES_MODULE" "$DC_MODULE" > "$RESULT_ROOT/module-sha256.txt"
printf 'frames=%s alternate=%s use_fbo=%s use_texture=%s rebind_attachment=%s wsegl=%s\n' \
    "$FRAMES" "$PROBE_ALTERNATE" "$PROBE_USE_FBO" "$PROBE_USE_TEXTURE" \
    "$PROBE_REBIND_ATTACHMENT" "$WSEGL" > "$RESULT_ROOT/probe-mode.txt"

load_stack
dmesg -C
cma_before=$(cma_kb)
irq_before=$(sgx_irq_count)

csv="$RESULT_ROOT/stage0.csv"
err="$RESULT_ROOT/stage0.stderr"
set +e
env SGX_REBIND_ATTACHMENT="$PROBE_REBIND_ATTACHMENT" \
    "$PROBE_LOADER" --library-path "$PROBE_LIBPATH" \
    "$PROBE" "$FRAMES" "$PROBE_ALTERNATE" "$PROBE_USE_FBO" \
    "$PROBE_USE_TEXTURE" > "$csv" 2> "$err"
rc=$?
set -e

cma_after=$(cma_kb)
irq_after=$(sgx_irq_count)
dmesg > "$RESULT_ROOT/stage0.dmesg"

frames_done=$(awk -F, '$1 ~ /^[0-9]+$/ { count++ } END { print count + 0 }' "$csv")
slow_frames=$(grep -c '^slow ' "$err" 2>/dev/null || true)
oom=no
[ "$rc" -eq 137 ] && oom=yes
faults=$(grep -Eic "$FAULT_PATTERN" "$RESULT_ROOT/stage0.dmesg" 2>/dev/null || true)

summary=$(printf \
    'rebind=%s frames=%s/%s rc=%s oom=%s cma_consumed_kb=%s irq_delta=%s slow_frames=%s dmesg_faults=%s' \
    "$PROBE_REBIND_ATTACHMENT" "$frames_done" "$FRAMES" "$rc" "$oom" \
    "$((cma_before - cma_after))" "$((irq_after - irq_before))" \
    "${slow_frames:-0}" "${faults:-0}")
printf '%s\n' "$summary" | tee "$RESULT_ROOT/stage0-summary.txt"
echo "Results: $RESULT_ROOT (stage0.csv, stage0.stderr, stage0.dmesg)"