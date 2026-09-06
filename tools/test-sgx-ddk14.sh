#!/bin/sh
set -eu

ROOT=${1:-/opt/pandora-ddk14}
MODULE="$ROOT/module/pvrsrvkm.ko"
LOADER="$ROOT/lib/ld-linux.so.3"
LIBPATH="$ROOT/usr/lib:$ROOT/lib"

if [ "$(id -u)" -ne 0 ]; then
    echo "Run as root" >&2
    exit 1
fi

for path in "$MODULE" "$LOADER" "$ROOT/usr/bin/pvrsrvinit" \
            "$ROOT/usr/bin/pvr2d_test"; do
    if [ ! -e "$path" ]; then
        echo "Missing runtime file: $path" >&2
        exit 1
    fi
done

module_release=$(modinfo -F vermagic "$MODULE" | awk '{print $1}')
if [ "$module_release" != "$(uname -r)" ]; then
    echo "Module is for $module_release, running kernel is $(uname -r)" >&2
    exit 1
fi

for module in bufferclass_ti omaplfb pvrsrvkm \
              pvrsrvkm_omap3_sgx530_121; do
    if grep -q "^$module " /proc/modules; then
        rmmod "$module"
    fi
done

insmod "$MODULE"

tries=0
while [ ! -e /dev/pvrsrvkm ] && [ "$tries" -lt 20 ]; do
    sleep 1
    tries=$((tries + 1))
done

if [ ! -e /dev/pvrsrvkm ]; then
    echo "pvrsrvkm loaded but /dev/pvrsrvkm did not appear" >&2
    dmesg | tail -80
    exit 1
fi

chmod 600 /dev/pvrsrvkm

timeout 20 "$LOADER" --library-path "$LIBPATH" \
    "$ROOT/usr/bin/pvrsrvinit"
output=$(timeout 60 "$LOADER" --library-path "$LIBPATH" \
    "$ROOT/usr/bin/pvr2d_test")
printf '%s\n' "$output"
if printf '%s\n' "$output" | grep -q "PowerVR device not found"; then
    echo "pvr2d_test requires the omitted display-class module" >&2
    exit 1
fi

dmesg | tail -80
