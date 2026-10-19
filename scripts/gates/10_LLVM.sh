#!/usr/bin/env bash
# Standalone gate script. Every gate in this directory runs on its own:
#     bash scripts/gates/10_LLVM.sh
# There is no aggregate driver - loading the shared setup (ROOT, binaries,
# report(), TMP) is done here unconditionally.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_common.sh"
GATE_START=${GATE_START:-$(date +%s)}
GATE_FILES=0

echo "=== gate 10: LLVM backend (kirnc llvm) ==="
# Phase-4 scalar-subset backend. Only meaningful in an LLVM-enabled build: the
# default build (no -DKIRN_WITH_LLVM) reports "no LLVM backend" and the gate
# skips rather than failing, because the option is opt-in by design.
if "$KN" llvm 2>&1 | grep -q "no LLVM backend"; then
    report PASS "gate 10: LLVM backend" "build has no LLVM support (no -DKIRN_WITH_LLVM); skipped"
elif command -v opt >/dev/null 2>&1 && command -v lli >/dev/null 2>&1; then
    G10="$TMP/g10"
    mkdir -p "$G10"
    for src in "$ROOT"/tests/llvm/p*.kn; do
        [ -e "$src" ] || continue
        name="$(basename "$src")"
        base="${name%.kn}"
        if ! "$KN" llvm "$src" -o "$G10/$base.ll" >"$G10/log" 2>&1; then
            report FAIL "$name" "kirnc llvm rejected a scalar file: $(head -1 "$G10/log")"
            continue
        fi
        if ! opt -passes=verify -disable-output "$G10/$base.ll" 2>"$G10/verr"; then
            report FAIL "$name" "LLVM verify rejected the IR: $(head -1 "$G10/verr")"
            continue
        fi
        so="$(mktemp "$G10/so.XXXX")"; ro="$(mktemp "$G10/ro.XXXX")"
        timeout "$WATCH" lli "$G10/$base.ll" >"$so" 2>/dev/null; c=$?
        if [ "$c" = 124 ]; then report FAIL "$name" "lli HANG"; continue; fi
        timeout "$WATCH" "$CR" "$src" >"$ro" 2>/dev/null; rc=$?
        if [ "$c" = "$rc" ] && cmp -s "$so" "$ro"; then
            report PASS "$name" "exit $c, stdout identical to interpreter ($(tr '\n' ' ' <"$so" | head -c 80))"
        else
            report FAIL "$name" "lli exit $c vs interpreter $rc; $(diff "$so" "$ro" | head -3 | tr '\n' ' ')"
        fi
    done
    for src in "$ROOT"/tests/llvm/n*.kn; do
        [ -e "$src" ] || continue
        name="$(basename "$src")"
        if "$KN" llvm "$src" >"$G10/nlog" 2>&1; then
            report FAIL "$name" "backend accepted a non-scalar file"
        else
            report PASS "$name" "rejected: $(head -1 "$G10/nlog")"
        fi
    done
else
    report FAIL "gate 10: LLVM backend" "LLVM build enabled but opt/lli not on PATH"
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
