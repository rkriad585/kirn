#!/usr/bin/env bash
# Standalone Linux gate for the POSIX -O/--obj stop-at-stage contract.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_common.sh"
GATE_START=${GATE_START:-$(date +%s)}

echo "=== gate 11f: POSIX object and assembly outputs ==="
G11="$TMP/g11f"
mkdir -p "$G11"
ok=1

if "$KIRN" build -O "$ROOT/tests/codegen/native_int_main.kn" \
    -o "$G11/short.o" >"$G11/short.log" 2>&1 &&
   file "$G11/short.o" | grep -q 'relocatable' && test -s "$G11/short.o"; then
    report PASS "-O object output" "ELF relocatable"
else
    report FAIL "-O object output" "$(sed -n '1p' "$G11/short.log")"
    ok=0
fi

if "$KIRN" build -c "$ROOT/tests/codegen/native_int_main.kn" \
    -o "$G11/long.o" >"$G11/long.log" 2>&1 &&
   file "$G11/long.o" | grep -q 'relocatable' && test -s "$G11/long.o"; then
    report PASS "-c object output" "ELF relocatable"
else
    report FAIL "-c object output" "$(sed -n '1p' "$G11/long.log")"
    ok=0
fi

if "$KIRN" build -S "$ROOT/tests/codegen/native_int_main.kn" \
    -o "$G11/out.s" >"$G11/asm.log" 2>&1 &&
   file "$G11/out.s" | grep -q 'assembler source' && test -s "$G11/out.s"; then
    report PASS "-S assembly output" "assembler source"
else
    report FAIL "-S assembly output" "$(sed -n '1p' "$G11/asm.log")"
    ok=0
fi

if KIRN_NO_CACHE=1 "$KIRN" build --release \
    "$ROOT/tests/codegen/native_int_main.kn" -o "$G11/native" \
    >"$G11/native.log" 2>&1 &&
   ! grep -q 'interp::' "$ROOT/build/release/linux-amd64/native_int_main.cpp" &&
   test "$("$G11/native")" = 832040; then
    report PASS "runtime-free scalar launcher" "no interpreter symbols"
else
    report FAIL "runtime-free scalar launcher" "$(sed -n '1p' "$G11/native.log")"
    ok=0
fi

GATE_END=$(date +%s)
GATE_DUR=$((GATE_END - GATE_START))
GATE_M=$((GATE_DUR / 60))
GATE_S=$((GATE_DUR % 60))
echo "GATE RESULT: $pass passed, $fail failed, $hung hung, 0ms par file, ${GATE_M}m:${GATE_S}s took"
[ "$ok" -eq 1 ] && [ "$fail" -eq 0 ] || exit 1
exit 0
