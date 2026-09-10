#!/bin/sh
set -eu

: "${OUTPUT:?OUTPUT is required}"
: "${SDK_SHA256:?SDK_SHA256 must be the verified installer SHA-256}"

mkdir -p "$(dirname "$OUTPUT")"
temporary="$OUTPUT.tmp"
rm -f "$temporary"

if [ -n "${SDK_INSTALLER:-}" ]; then
    [ -f "$SDK_INSTALLER" ] || {
        echo "SDK installer not found: $SDK_INSTALLER" >&2
        exit 2
    }
    cp "$SDK_INSTALLER" "$temporary"
elif [ -n "${SDK_URL:-}" ]; then
    curl --fail --location --retry 3 --retry-delay 2 \
        --output "$temporary" "$SDK_URL"
else
    echo 'set SDK_INSTALLER or SDK_URL' >&2
    exit 2
fi

actual_sha256=$(sha256sum "$temporary" | awk '{print $1}')
if [ "$actual_sha256" != "$SDK_SHA256" ]; then
    echo 'TI SDK installer SHA-256 mismatch' >&2
    echo "expected: $SDK_SHA256" >&2
    echo "actual:   $actual_sha256" >&2
    rm -f "$temporary"
    exit 1
fi
mv "$temporary" "$OUTPUT"