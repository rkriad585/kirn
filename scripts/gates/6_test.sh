#!/usr/bin/env bash
# Standalone gate script. Every gate in this directory runs on its own:
#     bash scripts/gates/6_test.sh
# There is no aggregate driver - loading the shared setup (ROOT, binaries,
# report(), TMP) is done here unconditionally.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_common.sh"

echo "=== gate 6: test (stdlib via kirn driver) ==="
rm -f "$ROOT/.io_tmp_test.txt"   # io_test.kn uses append-only scratch; no unlink primitive
out6=$(cd "$ROOT" && timeout "$WATCH" "$KIRN" test . 2>&1); c6=$?
rm -f "$ROOT/.io_tmp_test.txt"
if [ "$c6" = 0 ] && echo "$out6" | grep -q "17 passed, 0 failed"; then
    report PASS "kirn test stdlib/pet" "$(echo "$out6" | grep -c 'PASS') tests"
else
    report FAIL "kirn test stdlib/pet" "rc=$c6: $(echo "$out6" | tail -1)"
fi

echo "GATE RESULT: $pass passed, $fail failed, $hung hung"
[ "$fail" -eq 0 ] || exit 1
exit 0
