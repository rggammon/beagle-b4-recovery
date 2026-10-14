#!/bin/sh
# Dev build: stage-6 DDK 1.4 pvrsrvkm.ko + dcnohw.ko with a runtime-armed device-memory / kick log,
# to correlate GPU allocations (heap, GPU VA, size) with draws in replayed GL streams.
#
# Exports the ref with `git archive` (like build-graft-14.sh) into its own scratch tree, so the
# ddk14 worktree and the stage-6 build outputs are untouched.
#
# Arm at runtime: echo N > /sys/module/pvrsrvkm/parameters/gMemLog
# Each device-memory alloc/free and each SGXDoKickKM then emits one "MEMLOG ..." line until N runs out.
#
# Usage: build-memlog14-instrument.sh [git-ref]   (default: 1b5bb0a54c97, the board's stage-6 modules)
set -eu
B=/mnt/scratch/geoduck-tmp/beagle
SB=$B/b4ci/build-devuan/openpvrsgx-src
DDK14=$B/openpvrsgx-ddk14
REF=${1:-1b5bb0a54c97}
WORK=$B/pvr14-memlog-build
OUT=$B/pvr14-memlog-out
PVR=$WORK/drivers/gpu/drm/pvrsgx
S=$PVR/1.4.14.2616/services4/srvkm
MOD=$S/env/linux/module.c
DMEM=$S/common/devicemem.c
KICK=$S/devices/sgx/sgxkick.c

rm -rf "$WORK"
mkdir -p "$WORK" "$OUT"
rm -f "$OUT"/*.ko
git -C "$DDK14" archive "$REF" drivers/gpu/drm/pvrsgx/Makefile drivers/gpu/drm/pvrsgx/1.4.14.2616 | tar -x -C "$WORK"
echo "exported $(git -C "$DDK14" rev-parse --short "$REF") ($REF)"

# gMemLog must be plain unsigned int: 1.4 IMG_UINT32 is unsigned long, which mismatches module_param(uint)
perl -0777 -i -pe 's/(EXPORT_SYMBOL\(PVRGetBufferClassJTable\);\n)/$1\nunsigned int gMemLog = 0;\nmodule_param(gMemLog, uint, 0644);\nMODULE_PARM_DESC(gMemLog, "device-memory alloc\/free + kick lines left to log (self-decrementing)");\n/' "$MOD"

perl -0777 -i -pe 's/(#include "pvr_bridge_km.h"\n)/$1\nextern unsigned int gMemLog;\n/' "$DMEM"
perl -0777 -i -pe 's/(\tpsMemInfo->ui32AllocSize = ui32Size;\n)/$1\n\tif (gMemLog) {\n\t\tBM_HEAP *psLogHeap = (BM_HEAP *)hDevMemHeap;\n\t\tgMemLog--;\n\t\tPVR_LOG(("MEMLOG ALLOC dev=0x%08lx size=%lu align=%lu flags=0x%08lx heap=%s id=0x%lx",\n\t\t\t(unsigned long)psMemInfo->sDevVAddr.uiAddr, (unsigned long)ui32Size, (unsigned long)ui32Alignment,\n\t\t\t(unsigned long)psMemInfo->ui32Flags, psLogHeap->sDevArena.pszName, (unsigned long)psLogHeap->sDevArena.ui32HeapID));\n\t}\n/' "$DMEM"
perl -0777 -i -pe 's/(\thBuffer = psMemInfo->sMemBlk.hBuffer;\n)/$1\n\tif (gMemLog) {\n\t\tgMemLog--;\n\t\tPVR_LOG(("MEMLOG FREE dev=0x%08lx size=%lu",\n\t\t\t(unsigned long)psMemInfo->sDevVAddr.uiAddr, (unsigned long)psMemInfo->ui32AllocSize));\n\t}\n/' "$DMEM"

perl -0777 -i -pe 's/(#include "sgxutils.h"\n)/$1extern unsigned int gMemLog;\n/' "$KICK"
# per-kick sync log: one header line, then one line per attached sync object (counters before this kick bumps them)
SNIP_FN=$WORK/memlog-kick-fn.c
SNIP_LOG=$WORK/memlog-kick-log.c
cat > "$SNIP_FN" <<'EOF'
static void memlog_sync(const char *what, IMG_HANDLE h)
{
	PVRSRV_KERNEL_SYNC_INFO *s = (PVRSRV_KERNEL_SYNC_INFO *)h;
	if (!s)
		return;
	PVR_LOG(("MEMLOG   %s sync=0x%08lx wp=%lu wc=%lu rp=%lu rc=%lu", what,
		s->psSyncDataMemInfoKM ? (unsigned long)s->psSyncDataMemInfoKM->sDevVAddr.uiAddr : 0UL,
		(unsigned long)s->psSyncData->ui32WriteOpsPending, (unsigned long)s->psSyncData->ui32WriteOpsComplete,
		(unsigned long)s->psSyncData->ui32ReadOpsPending, (unsigned long)s->psSyncData->ui32ReadOpsComplete));
}
EOF
cat > "$SNIP_LOG" <<'EOF'

	if (gMemLog) {
		IMG_UINT32 k;
		gMemLog--;
		PVR_LOG(("MEMLOG KICK first=%d nsrc=%lu ndst=%lu tadep=%d nTAst=%lu n3Dst=%lu",
			(int)psCCBKick->bFirstKickOrResume, (unsigned long)psCCBKick->ui32NumSrcSyncs,
			(unsigned long)psCCBKick->ui32NumDstSyncObjects, (int)psCCBKick->bTADependency,
			(unsigned long)psCCBKick->ui32NumTAStatusVals, (unsigned long)psCCBKick->ui32Num3DStatusVals));
		memlog_sync("ta3d", psCCBKick->hTA3DSyncInfo);
		memlog_sync("ta", psCCBKick->hTASyncInfo);
		memlog_sync("3d", psCCBKick->h3DSyncInfo);
		memlog_sync("dst", psCCBKick->sDstSyncHandle);
		for (k = 0; k < psCCBKick->ui32NumSrcSyncs && k < SGX_MAX_SRC_SYNCS; k++)
			memlog_sync("src", psCCBKick->ahSrcKernelSyncInfo[k]);
		for (k = 0; k < psCCBKick->ui32NumTAStatusVals && k < SGX_MAX_TA_STATUS_VALS; k++)
			memlog_sync("tast", psCCBKick->ahTAStatusSyncInfo[k]);
		for (k = 0; k < psCCBKick->ui32Num3DStatusVals && k < SGX_MAX_3D_STATUS_VALS; k++)
			memlog_sync("3dst", psCCBKick->ah3DStatusSyncInfo[k]);
	}
EOF
SNIP_FN=$SNIP_FN perl -0777 -i -pe 'BEGIN { local $/; open F, "<", $ENV{SNIP_FN} or die; $s = <F> } s/(extern unsigned int gMemLog;\n)/$1\n$s\n/' "$KICK"
SNIP_LOG=$SNIP_LOG perl -0777 -i -pe 'BEGIN { local $/; open F, "<", $ENV{SNIP_LOG} or die; $s = <F> } s/(psTACmd = CCB_DATA_FROM_OFFSET\(SGXMKIF_CMDTA_SHARED, psCCBMemInfo, psCCBKick, ui32CCBOffset\);\n)/$1$s/' "$KICK"

n=$(grep -c gMemLog "$MOD"); [ "$n" -ge 2 ] || { echo "MOD_EDIT_FAILED ($n)"; exit 3; }
n=$(grep -c gMemLog "$DMEM"); [ "$n" -ge 5 ] || { echo "DEVICEMEM_EDIT_FAILED ($n)"; exit 4; }
n=$(grep -c gMemLog "$KICK"); [ "$n" -ge 3 ] || { echo "KICK_EDIT_FAILED ($n)"; exit 5; }
echo "hooks added: module.c, devicemem.c (alloc+free), sgxkick.c"

cd "$SB"
printf '#define UTS_RELEASE "%s"\n' "$(cat include/config/kernel.release)" > include/generated/utsrelease.h
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- M="$PVR" \
     CONFIG_SGX=m CONFIG_SGX_OMAP=m \
     CONFIG_PVRSGX_1_4_14_2616=y CONFIG_PVRSGX_1_4_14_2616_DC_NOHW=y \
     CONFIG_PVRSGX_1_6_16_3977= \
     modules 2>&1 | grep -E 'error|warning: .*gMemLog|\*\*\*|\.ko' || true

cp "$PVR/1.4.14.2616/pvrsrvkm.ko" "$PVR/1.4.14.2616/services4/3rdparty/dc_nohw/dcnohw.ko" "$OUT/"
for k in "$OUT/pvrsrvkm.ko" "$OUT/dcnohw.ko"; do
    echo "$k : $(strings "$k" | grep -m1 vermagic)"
done
echo "MEMLOG strings: $(strings "$OUT/pvrsrvkm.ko" | grep -c MEMLOG)  gMemLog param: $(strings "$OUT/pvrsrvkm.ko" | grep -c gMemLog)"
