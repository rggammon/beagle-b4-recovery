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
#   DDK_LIB    dir with the real soft-float DDK .so's (must carry DWARF)
#   DDK_INC    dir with the DDK GLES2/EGL headers
#   OUT        output dir
#   CROSS_HF   hard-float cross compiler   (default arm-linux-gnueabihf-gcc)
#   PATCHELF   patchelf binary             (default patchelf)
#   GEN        path to gen-hf-shim.py      (default alongside this script)
set -eu

: "${DDK_LIB:?set DDK_LIB to the real DDK lib dir}"
: "${DDK_INC:?set DDK_INC to the DDK header dir}"
: "${OUT:?set OUT to the output dir}"
CROSS_HF="${CROSS_HF:-arm-linux-gnueabihf-gcc}"
PATCHELF="${PATCHELF:-patchelf}"
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
GEN="${GEN:-$HERE/gen-hf-shim.py}"

readelf_soname() {
    "$PATCHELF" --print-soname "$1" 2>/dev/null || basename "$1"
}

rm -rf "$OUT"
mkdir -p "$OUT/lib" "$OUT/gen" "$OUT/ddk"

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
    python3 "$GEN" --loader "$DDK_LIB/$real" "$real" "$OUT/gen/${real%.so}_hf.c" \
        --headers "$header"
    "$CROSS_HF" -shared -fPIC -O2 -I"$DDK_INC" "$OUT/gen/${real%.so}_hf.c" \
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
[ "$rc" -eq 0 ] || { echo "ABI audit FAILED" >&2; exit 1; }

echo "done: $OUT"
