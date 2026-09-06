#!/bin/sh
set -u

HARNESS=${DDK16_HARNESS:-/tmp/test-sgx-ddk16.sh}
RESULT_ROOT=${RESULT_ROOT:-/tmp/ddk16-stage0-matrix}
CYCLES=${CYCLES:-1}
SOAK_SECONDS=${SOAK_SECONDS:-180}

run_variant()
{
    name=$1
    alternate=$2
    use_fbo=$3

    printf 'Running %s\n' "$name"
    if CYCLES="$CYCLES" SOAK_SECONDS="$SOAK_SECONDS" \
        PROBE_ALTERNATE="$alternate" PROBE_USE_FBO="$use_fbo" \
        PROBE_USE_TEXTURE=0 RESULT_ROOT="$RESULT_ROOT/$name" \
        "$HARNESS" > "$RESULT_ROOT/$name.log" 2>&1; then
        printf '%s,PASS\n' "$name" >> "$RESULT_ROOT/results.csv"
    else
        printf '%s,FAIL\n' "$name" >> "$RESULT_ROOT/results.csv"
    fi
}

[ -x "$HARNESS" ] || {
    echo "FAIL: missing executable harness: $HARNESS" >&2
    exit 1
}

mkdir -p "$RESULT_ROOT"
printf 'variant,result\n' > "$RESULT_ROOT/results.csv"

run_variant single-attachment 0 1
run_variant alternate-attachments 1 1
run_variant alternate-pbuffers 1 0

cat "$RESULT_ROOT/results.csv"
grep -H '^summary ' "$RESULT_ROOT"/*/soak-summary.txt 2>/dev/null || true

if grep -q ',FAIL$' "$RESULT_ROOT/results.csv"; then
    exit 1
fi