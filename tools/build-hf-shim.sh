#!/bin/sh
# Build the hard-float ABI shim set for the soft-float TI DDK 1.6 GLES/EGL
# userspace. Produces, in $OUT/lib, drop-in hard-float libEGL/libGLESv2 (same
# sonames as the real DDK) plus the libsgxhf namespace loader and libSGXm
# reverse interposer; and, in $OUT/ddk, the real DDK patched so libSGXm wins the
# float-symbol resolution inside the shim's dlmopen namespace.
#
# The shim .so's are our own code (ABI veneers); they contain no TI binaries.
# The real DDK in $OUT/ddk is still proprietary and stays subject to the TI EULA.
#
# Inputs (env):
#   DDK_LIB    dir with the real DDK .so's (may be stripped when VENEER_SRC is set)
#   DDK_INC    dir with the DDK GLES2/EGL headers
#   OUT        output dir
#   VENEER_SRC optional: dir of pre-generated <lib>_hf.c + <lib>_hf.syms to reuse
#              instead of regenerating from DWARF. Required for a stripped DDK
#              (e.g. DDK 1.4): reuse the 1.6-DWARF veneers (identical Khronos ABI).
#   DDK_INC_REF optional: reference DDK header dir; when set, the ABI-defining
#              EGL/GLES2 headers are diffed against DDK_INC and any change fails.
#   CROSS_HF   hard-float cross compiler   (default arm-linux-gnueabihf-gcc)
#   PATCHELF   patchelf binary             (default patchelf)
#   GEN        path to gen-hf-shim.py      (default alongside this script)
set -eu

: "${DDK_LIB:?set DDK_LIB to the real DDK lib dir}"
: "${DDK_INC:?set DDK_INC to the DDK header dir}"
: "${OUT:?set OUT to the output dir}"
VENEER_SRC="${VENEER_SRC:-}"
DDK_INC_REF="${DDK_INC_REF:-}"
CROSS_HF="${CROSS_HF:-arm-linux-gnueabihf-gcc}"
PATCHELF="${PATCHELF:-patchelf}"
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
GEN="${GEN:-$HERE/gen-hf-shim.py}"

readelf_soname() {
    "$PATCHELF" --print-soname "$1" 2>/dev/null || basename "$1"
}

# Exported FUNC symbols of a DDK .so (works on stripped libs; .dynsym survives).
ddk_exports() {
    readelf --dyn-syms --wide "$1" | \
        awk '$4 == "FUNC" && ($5 == "GLOBAL" || $5 == "WEAK") && $7 != "UND" {print $8}' | \
        sed 's/@.*//' | LC_ALL=C sort -u
}

# Khronos headers whose typedefs/prototypes define the wrapped ABI. A change to
# a FATAL header (float typedef or core prototype) between DDK versions would
# silently misroute a scalar, so it aborts the reuse. WARN headers are
# extension-only (resolved via eglGetProcAddress, not in the wrapped export set),
# so their diffs are reported but non-fatal.
ABI_HEADERS_FATAL='EGL/egl.h EGL/eglplatform.h GLES2/gl2.h GLES2/gl2platform.h KHR/khrplatform.h'
ABI_HEADERS_WARN='GLES2/gl2ext.h EGL/eglext.h'

# Strip C/C++ comments, blank lines and whitespace so cosmetic churn between SDK
# header revisions does not trip the diff; only declaration changes survive.
norm_header() {
    python3 - "$1" <<'PY'
import re, sys
src = open(sys.argv[1], encoding="utf-8", errors="replace").read()
src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
src = re.sub(r"//[^\n]*", "", src)
out = [re.sub(r"\s+", " ", ln).strip() for ln in src.splitlines()]
sys.stdout.write("\n".join(ln for ln in out if ln) + "\n")
PY
}

rm -rf "$OUT"
mkdir -p "$OUT/lib" "$OUT/gen" "$OUT/ddk"

# When a reference header tree is given, diff the ABI-defining headers up front.
if [ -n "$DDK_INC_REF" ]; then
    echo "== ABI header diff ($DDK_INC vs $DDK_INC_REF) =="
    hdr_rc=0
    diff_header() {
        h=$1; fatal=$2
        [ -f "$DDK_INC/$h" ] && [ -f "$DDK_INC_REF/$h" ] || {
            echo "  skip $h (absent one side)"; return 0; }
        base=${h##*/}
        norm_header "$DDK_INC/$h"     > "$OUT/gen/$base.norm.new"
        norm_header "$DDK_INC_REF/$h" > "$OUT/gen/$base.norm.ref"
        if diff -u "$OUT/gen/$base.norm.ref" "$OUT/gen/$base.norm.new" \
                > "$OUT/gen/$base.diff"; then
            echo "  OK   $h unchanged"
            rm -f "$OUT/gen/$base.diff"
        elif [ "$fatal" = fatal ]; then
            echo "  FAIL core ABI header changed: $h (see $OUT/gen/$base.diff)" >&2
            hdr_rc=1
        else
            echo "  WARN extension header changed: $h (see $OUT/gen/$base.diff)" >&2
        fi
        rm -f "$OUT/gen/$base.norm.new" "$OUT/gen/$base.norm.ref"
    }
    for h in $ABI_HEADERS_FATAL; do diff_header "$h" fatal; done
    for h in $ABI_HEADERS_WARN;  do diff_header "$h" warn;  done
    [ "$hdr_rc" -eq 0 ] || { echo "ABI header diff FAILED" >&2; exit 1; }
fi

echo "== libSGXm reverse interposer (soft-float wrappers, versioned GLIBC_2.4) =="
"$CROSS_HF" -shared -fPIC -O2 "$HERE/libm-hf-compat.c" \
    -Wl,--no-as-needed -lm -ldl \
    -Wl,--version-script="$HERE/libm-hf-compat.map" \
    -Wl,-soname,libSGXm.so.1 -o "$OUT/lib/libSGXm.so.1"

echo "== libsgxhf namespace loader =="
"$CROSS_HF" -shared -fPIC -O2 -I"$HERE" "$HERE/hf-shim-loader.c" \
    -ldl -lpthread -Wl,-soname,libsgxhf.so.1 -o "$OUT/lib/libsgxhf.so.1"
ln -sf libsgxhf.so.1 "$OUT/lib/libsgxhf.so"

echo "== forward shims (generated from DWARF) =="
gen_and_build() {
    real=$1; header=$2
    soname=$(readelf_soname "$DDK_LIB/$real")
    src="$OUT/gen/${real%.so}_hf.c"
    man="$OUT/gen/${real%.so}_hf.syms"
    if [ -n "$VENEER_SRC" ]; then
        # Reuse pre-generated veneers (the stripped DDK has no DWARF to read).
        [ -f "$VENEER_SRC/${real%.so}_hf.c" ] || {
            echo "  FAIL VENEER_SRC missing ${real%.so}_hf.c" >&2; exit 1; }
        [ -f "$VENEER_SRC/${real%.so}_hf.syms" ] || {
            echo "  FAIL VENEER_SRC missing ${real%.so}_hf.syms" >&2; exit 1; }
        cp "$VENEER_SRC/${real%.so}_hf.c" "$src"
        cp "$VENEER_SRC/${real%.so}_hf.syms" "$man"
        echo "  reuse $real veneer from $VENEER_SRC"
    else
        python3 "$GEN" --loader "$DDK_LIB/$real" "$real" "$src" \
            --headers "$header" --manifest "$man"
    fi
    # Coverage gate: the veneer ABI surface must exactly match the target DDK's
    # exported functions. Catches any symbol drift between DDK versions when
    # reusing veneers against a stripped DDK (no DWARF available to re-derive).
    ddk_exports "$DDK_LIB/$real" > "$OUT/gen/${real%.so}.exports"
    awk '{print $2}' "$man" | LC_ALL=C sort -u > "$OUT/gen/${real%.so}.surface"
    if ! diff -u "$OUT/gen/${real%.so}.surface" "$OUT/gen/${real%.so}.exports" \
            > "$OUT/gen/${real%.so}.coverage.diff"; then
        echo "  FAIL $real export set differs from veneer surface:" >&2
        sed 's/^/    /' "$OUT/gen/${real%.so}.coverage.diff" >&2
        exit 1
    fi
    rm -f "$OUT/gen/${real%.so}.coverage.diff"
    echo "  OK   $real: $(wc -l < "$OUT/gen/${real%.so}.surface") exports covered"
    "$CROSS_HF" -shared -fPIC -O2 -I"$DDK_INC" "$src" \
        -L"$OUT/lib" -lsgxhf -ldl \
        -Wl,-soname,"$soname" -o "$OUT/lib/$soname"
    [ "$soname" = "$real" ] || ln -sf "$soname" "$OUT/lib/$real"
    echo "  $real -> soname $soname"
}
gen_and_build libEGL.so    EGL/egl.h
gen_and_build libGLESv2.so GLES2/gl2.h

echo "== patched real DDK (libSGXm-first) =="
cp -a "$DDK_LIB"/*.so "$OUT/ddk/"
cp "$OUT/lib/libSGXm.so.1" "$OUT/ddk/"
for so in "$OUT/ddk"/*.so; do
    [ "$(basename "$so")" = libSGXm.so.1 ] && continue
    if readelf -d "$so" 2>/dev/null | grep -q 'NEEDED.*libm\.so'; then
        "$PATCHELF" --replace-needed libm.so.6 libSGXm.so.1 "$so"
    fi
    "$PATCHELF" --set-rpath '$ORIGIN' "$so"
done
# libSGXm must be first in the namespace scope: prepend it on the dlmopen roots.
for so in libEGL.so libGLESv2.so; do
    "$PATCHELF" --remove-needed libSGXm.so.1 "$OUT/ddk/$so" 2>/dev/null || true
    "$PATCHELF" --add-needed libSGXm.so.1 "$OUT/ddk/$so"
done

echo "== ABI audit =="
rc=0
for so in "$OUT/lib/libEGL.so" "$OUT/lib/libGLESv2.so"; do
    if readelf -A "$so" 2>/dev/null | grep -qi 'VFP_args'; then
        echo "  OK   $(basename "$so") is hard-float (VFP_args)"
    else
        echo "  FAIL $(basename "$so") is NOT hard-float" >&2
        rc=1
    fi
done
# libSGXm is built hard-float on purpose (it forwards to the real hard-float
# libm); its exported wrappers are soft-float via pcs("aapcs"), a per-symbol
# property not reflected in the ELF tag. What CI must assert is that it exports
# the versioned float-parsing symbols that have to beat libc in the namespace.
for sym in 'strtod@@GLIBC_2.4' 'atof@@GLIBC_2.4' 'sinf@@GLIBC_2.4'; do
    if readelf --dyn-syms "$OUT/lib/libSGXm.so.1" 2>/dev/null | grep -q "$sym"; then
        echo "  OK   libSGXm exports $sym"
    else
        echo "  FAIL libSGXm missing $sym" >&2
        rc=1
    fi
done
# The dlmopen roots must carry libSGXm ahead of libc in DT_NEEDED.
for so in libEGL.so libGLESv2.so; do
    order=$(readelf -d "$OUT/ddk/$so" | grep -oE 'libSGXm\.so\.1|libc\.so\.6' | head -2 | tr '\n' ' ')
    case "$order" in
        "libSGXm.so.1 "*) echo "  OK   $so NEEDED: $order" ;;
        *) echo "  FAIL $so NEEDED order wrong: $order" >&2; rc=1 ;;
    esac
done
# patchelf can silently relocate .dynamic outside a PT_LOAD on these libs, which
# makes the dynamic loader segfault at run time. Assert every patched DDK lib is
# still loadable-shaped (PT_DYNAMIC within some PT_LOAD).
if ! python3 - "$OUT/ddk"/*.so <<'PY'
import sys
from elftools.elf.elffile import ELFFile
bad = []
for path in sys.argv[1:]:
    with open(path, 'rb') as f:
        elf = ELFFile(f)
        loads = [(s['p_offset'], s['p_offset'] + s['p_filesz'])
                 for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
        for d in (s for s in elf.iter_segments() if s['p_type'] == 'PT_DYNAMIC'):
            o = d['p_offset']
            if not any(lo <= o < hi for lo, hi in loads):
                bad.append(path)
if bad:
    sys.stderr.write('  FAIL PT_DYNAMIC outside PT_LOAD: %s\n' % ' '.join(bad))
    sys.exit(1)
print('  OK   %d patched DDK libs are loadable-shaped' % (len(sys.argv) - 1))
PY
then
    rc=1
fi
[ "$rc" -eq 0 ] || { echo "ABI audit FAILED" >&2; exit 1; }

echo "done: $OUT"
