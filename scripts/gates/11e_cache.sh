#!/usr/bin/env bash
# Standalone gate for the release-native artifact cache.  This is deliberately
# separate from the native lowering gates: it proves reuse of a finished
# artifact, not merely that the generated C++ is valid.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_common.sh"
GATE_START=${GATE_START:-$(date +%s)}

echo "=== gate 11e: release native artifact cache ==="
G11="$TMP/g11e"
mkdir -p "$G11/cache" "$G11/out"
cat >"$G11/cache_fixture.kn" <<'EOF'
fn fib(n: Int) -> Int {
    if n < 2 { return n; }
    return fib(n - 1) + fib(n - 2);
}
fn main() -> none { print(fib(12)); }
EOF

if KIRN_CACHE="$G11/cache" "$KIRN" build --release "$G11/cache_fixture.kn" \
    -o "$G11/out/cold" >"$G11/cold.log" 2>&1 &&
   KIRN_CACHE="$G11/cache" KIRN_VERBOSE=1 "$KIRN" build --release \
    "$G11/cache_fixture.kn" -o "$G11/out/warm" >"$G11/warm.log" 2>&1 &&
   grep -q 'native artifact hit' "$G11/warm.log" &&
   test -x "$G11/out/cold" && test -x "$G11/out/warm" &&
   test "$("$G11/out/cold")" = 144 && test "$("$G11/out/warm")" = 144 &&
   test "$(find "$G11/cache/native" -type f | wc -l)" -eq 1; then
    report PASS "native artifact cache" "cold build reused by warm build"
else
    report FAIL "native artifact cache" "$(sed -n '1p' "$G11/warm.log" 2>/dev/null)"
fi

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
