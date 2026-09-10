#!/bin/sh
set -eu

: "${INSTALLER:?INSTALLER is required}"
: "${OUTPUT:?OUTPUT is required}"

[ "${ACCEPT_TI_EULA:-no}" = yes ] || {
    echo 'set ACCEPT_TI_EULA=yes after reviewing and accepting the TI installer EULA' >&2
    exit 2
}
[ -f "$INSTALLER" ] || { echo "installer not found: $INSTALLER" >&2; exit 2; }

rm -rf "$OUTPUT"
mkdir -p "$OUTPUT/home" "$OUTPUT/install"
chmod 0755 "$INSTALLER"

# Sequence used by the historical meta-openpandora ti-eula-unpack recipe.
printf 'Y\n qY\n%s\nY\n' "$OUTPUT/install" | \
    HOME="$OUTPUT/home" timeout 300 "$INSTALLER" --mode console

runtime=$(find "$OUTPUT/install" -type d -name gfx_rel_es2.x -print -quit)
[ -n "$runtime" ] || {
    echo 'installer completed without producing gfx_rel_es2.x' >&2
    exit 1
}
printf '%s\n' "$runtime" > "$OUTPUT/runtime.path"