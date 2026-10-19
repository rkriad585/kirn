#!/usr/bin/env bash
# Standalone gate script. Every gate in this directory runs on its own:
#     bash scripts/gates/6_test.sh
# There is no aggregate driver - loading the shared setup (ROOT, binaries,
# report(), TMP) is done here unconditionally.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_common.sh"
GATE_START=${GATE_START:-$(date +%s)}
GATE_FILES=0

echo "=== gate 6: test (stdlib via kirn driver) ==="
rm -f "$ROOT/.io_tmp_test.txt"   # io_test.kn uses append-only scratch; no unlink primitive
out6=$(cd "$ROOT" && timeout "$WATCH" "$KIRN" test . 2>&1); c6=$?
rm -f "$ROOT/.io_tmp_test.txt"
if [ "$c6" = 0 ] && echo "$out6" | grep -q "17 passed, 0 failed"; then
    report PASS "kirn test stdlib/pet" "$(echo "$out6" | grep -c 'PASS') tests"
else
    report FAIL "kirn test stdlib/pet" "rc=$c6: $(echo "$out6" | tail -1)"
fi

GATE_END=$(date +%s)
GATE_DUR=$((GATE_END - GATE_START))
GATE_M=$((GATE_DUR / 60))
GATE_S=$((GATE_DUR % 60))
GATE_PARFILE=0
if [ "${GATE_FILES:-0}" -gt 0 ]; then
  GATE_PARFILE=$(awk -v d="$GATE_DUR" -v f="$GATE_FILES" "BEGIN{printf \"%.2f\", d*1000.0/f}")
else
  GATE_FALLBACK=$((pass + fail))
  if [ "$GATE_FALLBACK" -gt 0 ]; then
    GATE_PARFILE=$(awk -v d="$GATE_DUR" -v f="$GATE_FALLBACK" "BEGIN{printf \"%.2f\", d*1000.0/f}")
  fi
fi
echo "GATE RESULT: $pass passed, $fail failed, $hung hung, ${GATE_PARFILE}ms par file, ${GATE_M}m:${GATE_S}s took"
[ "$fail" -eq 0 ] || exit 1
exit 0
