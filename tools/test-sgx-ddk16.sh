#!/bin/sh
set -eu

ROOT=${DDK16_ROOT:-/opt/ti-ddk16}
ARMEL_ROOT=${ARMEL_ROOT:-/opt/pandora-armel}
PROBE=${SGX_PROBE:-$ARMEL_ROOT/bin/sgx-pbuffer-latency}
CYCLES=${CYCLES:-5}
SOAK_SECONDS=${SOAK_SECONDS:-180}
RESULT_ROOT=${RESULT_ROOT:-/tmp/ddk16-stage0}
PROBE_ALTERNATE=${PROBE_ALTERNATE:-1}
PROBE_USE_FBO=${PROBE_USE_FBO:-1}
PROBE_USE_TEXTURE=${PROBE_USE_TEXTURE:-0}
PROBE_REBIND_ATTACHMENT=${PROBE_REBIND_ATTACHMENT:-1}
TRACE_SLOW_BRIDGE=${TRACE_SLOW_BRIDGE:-0}

SERVICES_MODULE="$ROOT/module/pvrsrvkm.ko"
DC_MODULE="$ROOT/module/dcnohw.ko"
INIT_LOADER="$ROOT/lib/ld-linux.so.3"
INIT_LIBPATH="$ROOT/runtime:$ROOT/lib"
PROBE_LOADER="$ARMEL_ROOT/lib/ld-linux.so.3"
PROBE_LIBPATH="$ROOT/runtime:$ARMEL_ROOT/lib"
FAULT_PATTERN='HWRecovery|BIF|watchdog|Oops|BUG|fault|build-option mismatch'

fail()
{
    echo "FAIL: $*" >&2
    exit 1
}

require_positive_integer()
{
    name=$1
    value=$2

    case "$value" in
        ''|*[!0-9]*|0) fail "$name must be a positive integer" ;;
    esac
}

require_boolean()
{
    name=$1
    value=$2

    case "$value" in
        0|1) ;;
        *) fail "$name must be 0 or 1" ;;
    esac
}

unload_modules()
{
    rmmod dcnohw 2>/dev/null || true
    rmmod bufferclass_ti 2>/dev/null || true
    rmmod omaplfb 2>/dev/null || true
    rmmod pvrsrvkm 2>/dev/null || true
    rmmod pvrsrvkm_omap3_sgx530_121 2>/dev/null || true
}

load_stack()
{
    unload_modules
    dmesg -C
    if [ "$TRACE_SLOW_BRIDGE" = 1 ]; then
        insmod "$SERVICES_MODULE" trace_slow_bridge=1
    else
        insmod "$SERVICES_MODULE"
    fi

    tries=0
    while [ ! -e /dev/pvrsrvkm ] && [ "$tries" -lt 20 ]; do
        sleep 1
        tries=$((tries + 1))
    done
    [ -e /dev/pvrsrvkm ] || fail "pvrsrvkm loaded without /dev/pvrsrvkm"
    chmod 600 /dev/pvrsrvkm

    timeout 30 "$INIT_LOADER" --library-path "$INIT_LIBPATH" \
        "$ROOT/runtime/pvrsrvinit"
    insmod "$DC_MODULE"

    grep -q '^dcnohw ' /proc/modules || fail "dcnohw did not load"
    grep -q '^pvrsrvkm ' /proc/modules || fail "pvrsrvkm did not load"
    grep -qi 'SGX ISR' /proc/interrupts || fail "SGX IRQ is not registered"
}

run_probe()
{
    frames=$1
    output=$2

    timeout 120 env LD_LIBRARY_PATH="$PROBE_LIBPATH" \
        "$PROBE_LOADER" --library-path "$PROBE_LIBPATH" \
        "$PROBE" "$frames" "$PROBE_ALTERNATE" "$PROBE_USE_FBO" \
        "$PROBE_USE_TEXTURE" > "$output" 2> "$output.stderr"

    grep -q '^egl=1\.4 renderer=PowerVR SGX 530 ' "$output" ||
        fail "probe did not use the DDK 1.6 SGX renderer"
    actual_frames=$(awk -F, '$1 ~ /^[0-9]+$/ { count++ } END { print count + 0 }' "$output")
    [ "$actual_frames" -eq "$frames" ] ||
        fail "probe produced $actual_frames of $frames frames"
}

check_kernel_log()
{
    output=$1

    dmesg > "$output"
    if grep -Eiq "$FAULT_PATTERN" "$output"; then
        grep -Ei "$FAULT_PATTERN" "$output" >&2
        fail "kernel fault or SGX recovery detected"
    fi
}

memory_snapshot()
{
    label=$1
    awk -v label="$label" '
        /^(MemAvailable|SwapFree|Slab|SReclaimable|SUnreclaim|VmallocUsed):/ {
            values[$1] = $2
        }
        END {
            printf "%s", label
            for (key in values)
                printf ",%s=%s", key, values[key]
            printf "\n"
        }
    ' /proc/meminfo
}

[ "$(id -u)" -eq 0 ] || fail "run as root"
require_positive_integer CYCLES "$CYCLES"
require_positive_integer SOAK_SECONDS "$SOAK_SECONDS"
require_boolean PROBE_ALTERNATE "$PROBE_ALTERNATE"
require_boolean PROBE_USE_FBO "$PROBE_USE_FBO"
require_boolean PROBE_USE_TEXTURE "$PROBE_USE_TEXTURE"
require_boolean PROBE_REBIND_ATTACHMENT "$PROBE_REBIND_ATTACHMENT"
require_boolean TRACE_SLOW_BRIDGE "$TRACE_SLOW_BRIDGE"

for path in "$SERVICES_MODULE" "$DC_MODULE" "$INIT_LOADER" \
            "$ROOT/runtime/pvrsrvinit" "$PROBE_LOADER" "$PROBE"; do
    [ -e "$path" ] || fail "missing required file: $path"
done

services_release=$(modinfo -F vermagic "$SERVICES_MODULE" | awk '{print $1}')
dc_release=$(modinfo -F vermagic "$DC_MODULE" | awk '{print $1}')
[ "$services_release" = "$(uname -r)" ] ||
    fail "pvrsrvkm is for $services_release, running kernel is $(uname -r)"
[ "$dc_release" = "$(uname -r)" ] ||
    fail "dcnohw is for $dc_release, running kernel is $(uname -r)"

mkdir -p "$RESULT_ROOT"
trap 'unload_modules' EXIT INT TERM HUP

printf 'alternate=%s use_fbo=%s use_texture=%s rebind_attachment=%s\n' \
    "$PROBE_ALTERNATE" "$PROBE_USE_FBO" "$PROBE_USE_TEXTURE" \
    "$PROBE_REBIND_ATTACHMENT" \
    > "$RESULT_ROOT/probe-mode.txt"
sha256sum "$SERVICES_MODULE" "$DC_MODULE" > "$RESULT_ROOT/module-sha256.txt"
modinfo "$SERVICES_MODULE" > "$RESULT_ROOT/pvrsrvkm.modinfo"
modinfo "$DC_MODULE" > "$RESULT_ROOT/dcnohw.modinfo"
uname -a > "$RESULT_ROOT/uname.txt"
memory_snapshot before > "$RESULT_ROOT/memory.csv"

cycle=1
while [ "$cycle" -le "$CYCLES" ]; do
    echo "Lifecycle cycle $cycle/$CYCLES"
    load_stack
    run_probe 120 "$RESULT_ROOT/cycle-$cycle.csv"
    check_kernel_log "$RESULT_ROOT/cycle-$cycle.dmesg"
    unload_modules
    memory_snapshot "cycle-$cycle" >> "$RESULT_ROOT/memory.csv"
    cycle=$((cycle + 1))
done

echo "Soaking for at least $SOAK_SECONDS seconds in one EGL process"
load_stack
start=$(date +%s)
timeout $((SOAK_SECONDS + 120)) env \
    LD_LIBRARY_PATH="$PROBE_LIBPATH" \
    SGX_DURATION_SECONDS="$SOAK_SECONDS" SGX_SUMMARY_ONLY=1 \
    SGX_REBIND_ATTACHMENT="$PROBE_REBIND_ATTACHMENT" \
    "$PROBE_LOADER" --library-path "$PROBE_LIBPATH" \
    "$PROBE" 1 "$PROBE_ALTERNATE" "$PROBE_USE_FBO" \
    "$PROBE_USE_TEXTURE" > "$RESULT_ROOT/soak-summary.txt" \
    2> "$RESULT_ROOT/soak-stderr.txt"

elapsed=$(($(date +%s) - start))
memory_snapshot after >> "$RESULT_ROOT/memory.csv"
check_kernel_log "$RESULT_ROOT/soak-final.dmesg"
summary=$(grep '^summary ' "$RESULT_ROOT/soak-summary.txt")
[ -n "$summary" ] || fail "soak probe did not produce a summary"
slow_frames=$(printf '%s\n' "$summary" | sed -n 's/.* over_500ms=\([0-9][0-9]*\).*/\1/p')
[ "$slow_frames" = "0" ] || fail "soak contained $slow_frames frames over 500 ms"

printf 'PASS: cycles=%d soak_seconds=%d %s\n' "$CYCLES" "$elapsed" "$summary"
echo "Results: $RESULT_ROOT"