#!/usr/bin/env bash
# Standalone gate for native translation-unit validity.  Gate 11a checks the
# lowering decision; this gate also asks the host C++ compiler to parse every
# fixture that is marked KIRN_HAS_NATIVE=1.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_common.sh"
GATE_START=${GATE_START:-$(date +%s)}
GATE_FILES=0

echo "=== gate 11d: native generated C++ syntax ==="
G11="$TMP/g11d"; mkdir -p "$G11"
for src in "$ROOT"/tests/codegen/*.kn; do
    nm=$(basename "$src" .kn)
    cpp="$G11/$nm.cpp"
    KIRN_DUMP_NATIVE="$cpp" "$KIRN" build --native "$src" -o "$G11/$nm.out" \
        >"$G11/$nm.log" 2>&1
    if [ ! -s "$cpp" ]; then
        report FAIL "native dump $nm" "driver did not produce a C++ fragment"
        continue
    fi
    got=$(sed -n 's/^#define KIRN_HAS_NATIVE //p' "$cpp" | head -1)
    if [ "$got" != 1 ]; then
        continue
    fi
    if "$CXX" -std=c++20 -D_CRT_SECURE_NO_WARNINGS -O0 -w \
        -I"$ROOT/src" -include "$ROOT/src/interp/runtime.h" \
        -fsyntax-only "$cpp" >"$G11/$nm.syntax.log" 2>&1; then
        report PASS "native C++ syntax $nm" "KIRN_HAS_NATIVE $got"
    else
        report FAIL "native C++ syntax $nm" "$(sed -n '1p' "$G11/$nm.syntax.log")"
    fi
done

GATE_END=$(date +%s)
GATE_DUR=$((GATE_END - GATE_START))
GATE_M=$((GATE_DUR / 60))
GATE_S=$((GATE_DUR % 60))
GATE_PARFILE=0
if [ "$pass" -gt 0 ]; then
  GATE_PARFILE=$(awk -v d="$GATE_DUR" -v f="$pass" 'BEGIN{printf "%.2f", d*1000.0/f}')
fi
echo "GATE RESULT: $pass passed, $fail failed, $hung hung, ${GATE_PARFILE}ms par file, ${GATE_M}m:${GATE_S}s took"
[ "$fail" -eq 0 ] || exit 1
exit 0
