#!/usr/bin/env bash
# Standalone gate script. Every gate in this directory runs on its own:
#     bash scripts/gates/2_types.sh
# There is no aggregate driver - loading the shared setup (ROOT, binaries,
# report(), TMP) is done here unconditionally.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_common.sh"

echo "=== gate 2: types (tests/types) ==="
for t in "$ROOT"/tests/types/p*.kn; do
    name="$(basename "$t")"; err="$("$CC" "$t" 2>&1)"; [ $? -eq 0 ] || { report FAIL "$name" "kirncheck rejected: $(echo "$err" | head -1)"; continue; }
    err="$("$CR" "$t" 2>&1)"; [ $? -eq 0 ] || { report FAIL "$name" "kirnrun exit nonzero: $(echo "$err" | head -1)"; continue; }
    report PASS "$name" "check + run clean"
done
for t in "$ROOT"/tests/types/n*.kn "$ROOT"/tests/negative/n*.kn; do
    name="$(basename "$t")"
    exp=$(sed -n 's/.*#\s*expect:\s*//p' "$t" | head -1 | sed 's/[[:space:]]*$//')
    if [ -z "$exp" ]; then report FAIL "$name" "missing '# expect:' comment line"; continue; fi
    out="$("$CC" "$t" 2>&1)"; code=$?
    if [ "$code" = 0 ]; then report FAIL "$name" "kirncheck exited 0, expected diagnostic '$exp'"; continue; fi
    if echo "$out" | grep -qF "$exp"; then report PASS "$name" "$exp"; else
        report FAIL "$name" "missing '$exp'; got: $(echo "$out" | head -1)"
    fi
done

echo "GATE RESULT: $pass passed, $fail failed, $hung hung"
[ "$fail" -eq 0 ] || exit 1
exit 0
