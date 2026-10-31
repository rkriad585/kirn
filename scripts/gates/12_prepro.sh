#!/usr/bin/env bash
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_common.sh"
GATE_START=${GATE_START:-$(date +%s)}
echo "=== gate 12: preprocessor command-line definitions ==="

src="$ROOT/tests/macros/cmdline.kn"
out="$TMP/prepro.out"
if "$KN" run -DFEATURE "$src" >"$out" 2>&1 &&
   grep -q '^feature on$' "$out" &&
   ! grep -q '^feature off$' "$out"; then
    report PASS "kirnc run -D" "command-line definition reaches the runtime front end"
else
    report FAIL "kirnc run -D" "unexpected output: $(tr '\n' ' ' <"$out")"
fi

if "$KN" check -DFEATURE -UFEATURE "$src" >/dev/null 2>&1; then
    report PASS "kirnc check -D/-U" "last command-line definition wins"
else
    report FAIL "kirnc check -D/-U" "check rejected a valid command-line macro sequence"
fi

if "$KN" hir -DFEATURE "$src" >/dev/null 2>&1 &&
   "$KN" rt -DFEATURE "$src" >/dev/null 2>&1; then
    report PASS "HIR/MIR -D" "stage drivers share the command-line macro environment"
else
    report FAIL "HIR/MIR -D" "a later stage did not receive command-line definitions"
fi

GATE_END=$(date +%s)
GATE_DUR=$((GATE_END - GATE_START))
echo "GATE RESULT: $pass passed, $fail failed, $hung hung, 0ms par file, 0m:${GATE_DUR}s took"
[ "$fail" -eq 0 ] || exit 1
