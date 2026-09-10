#!/bin/sh
set -eu

: "${SDK_ROOT:?SDK_ROOT is required}"
: "${OUTPUT:?OUTPUT is required}"

runtime_path_file="$SDK_ROOT/runtime.path"
[ -f "$runtime_path_file" ] || { echo "missing $runtime_path_file" >&2; exit 2; }
runtime=$(cat "$runtime_path_file")
[ -d "$runtime" ] || { echo "runtime not found: $runtime" >&2; exit 2; }

rm -rf "$OUTPUT"
mkdir -p "$OUTPUT"
find "$runtime" -type f -printf '%P\n' | LC_ALL=C sort > "$OUTPUT/files.txt"

: > "$OUTPUT/elf.txt"
find "$runtime" -type f -print | LC_ALL=C sort | while IFS= read -r candidate; do
    if file "$candidate" | grep -q 'ELF '; then
        relative=${candidate#"$runtime"/}
        {
            printf '\n===== %s =====\n' "$relative"
            file "$candidate"
            printf '%s\n' '-- attributes --'
            readelf --arch-specific "$candidate" || true
            printf '%s\n' '-- dynamic section --'
            readelf --dynamic "$candidate" || true
            printf '%s\n' '-- imported symbols --'
            readelf --dyn-syms --wide "$candidate" | awk '$7 == "UND"'
            printf '%s\n' '-- exported functions --'
            readelf --dyn-syms --wide "$candidate" | \
                awk '$4 == "FUNC" && $5 == "GLOBAL" && $7 != "UND"'
        } >> "$OUTPUT/elf.txt"
    fi
done

grep -q 'Tag_ABI_VFP_args' "$OUTPUT/elf.txt" && {
    echo 'warning: one or more SDK objects declare a VFP argument ABI' >&2
    grep -n 'Tag_ABI_VFP_args' "$OUTPUT/elf.txt" > "$OUTPUT/vfp-args.txt"
} || : > "$OUTPUT/vfp-args.txt"