#!/bin/sh
# Dev hot-swap build: DDK 1.4.14.2616 pvrsrvkm.ko + dcnohw.ko as external modules
# against the b4ci kernel tree (214a35), so they load on the B4 alongside its 1.6 image.
#
# Source is the openpvrsgx-ddk14 worktree, branch users/rgammon/pvrsgx-1.4.14.2616
# (pushed to github.com/rggammon/linux_openpvrsgx). It is exported with `git archive`,
# so uncommitted edits and the instrumented graft tree never leak into the build.
#
# Usage: build-graft-14.sh [git-ref]   (default: HEAD of the ddk14 worktree)
# Output: $OUT/pvrsrvkm.ko, $OUT/dcnohw.ko
set -eu
B=/mnt/scratch/geoduck-tmp/beagle
SB=$B/b4ci/build-devuan/openpvrsgx-src
DDK14=$B/openpvrsgx-ddk14
REF=${1:-HEAD}
WORK=$B/pvr14-build
OUT=$B/pvr14-out
PVR=$WORK/drivers/gpu/drm/pvrsgx

rm -rf "$WORK"
mkdir -p "$WORK" "$OUT"
rm -f "$OUT"/*.ko
git -C "$DDK14" archive "$REF" drivers/gpu/drm/pvrsgx/Makefile drivers/gpu/drm/pvrsgx/1.4.14.2616 | tar -x -C "$WORK"
echo "exported $(git -C "$DDK14" rev-parse --short "$REF") ($REF)"

cd "$SB"
printf '#define UTS_RELEASE "%s"\n' "$(cat include/config/kernel.release)" > include/generated/utsrelease.h
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- M="$PVR" \
     CONFIG_SGX=m CONFIG_SGX_OMAP=m \
     CONFIG_PVRSGX_1_4_14_2616=y CONFIG_PVRSGX_1_4_14_2616_DC_NOHW=y \
     CONFIG_PVRSGX_1_6_16_3977= \
     modules 2>&1 | grep -E 'error|\*\*\*|\.ko' || true

cp "$PVR/1.4.14.2616/pvrsrvkm.ko" "$PVR/1.4.14.2616/services4/3rdparty/dc_nohw/dcnohw.ko" "$OUT/"
for k in "$OUT/pvrsrvkm.ko" "$OUT/dcnohw.ko"; do
    echo "$k : $(strings "$k" | grep -m1 vermagic)"
done
