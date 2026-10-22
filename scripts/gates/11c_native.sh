#!/usr/bin/env bash
# Standalone gate script. Every gate in this directory runs on its own:
#     bash scripts/gates/11c_native.sh
# There is no aggregate driver - loading the shared setup (ROOT, binaries,
# report(), TMP) is done here unconditionally.
#
# Native gate, part c of 3 (the former single 11_native.sh was split because it
# was too heavy): build a native binary and require it to
# agree with the interpreter byte-for-byte. Covers const-fn folding (a
# module-level `const` scalar inlined at its use sites) and Elvis-assign
# (`x ??= v` lowered to `if (!x) x = v;`).
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_common.sh"
GATE_START=${GATE_START:-$(date +%s)}
GATE_FILES=0

echo "=== gate 11c: native differentials (const inline, coalesce assign) ==="
G11="$TMP/g11c"; mkdir -p "$G11"

cinat="$G11/const_inline.bin"
if timeout 900 "$KIRN" build --native "$ROOT/tests/codegen/const_inline.kn" -o "$cinat" \
        >"$G11/ci.log" 2>&1; then
    ci_native=$(timeout "$WATCH" "$cinat" 2>&1); cnr=$?
    ci_interp=$(timeout "$WATCH" "$CR" "$ROOT/tests/codegen/const_inline.kn" 2>&1); cir=$?
    if [ "$cnr" = "$cir" ] && [ "$ci_native" = "$ci_interp" ]; then
        report PASS "native const-inline == interpreter" "$(echo "$ci_native" | tr '\n' ' ')"
    else
        report FAIL "native const-inline == interpreter" \
            "native rc=$cnr: $ci_native | interp rc=$cir: $ci_interp"
    fi
else
    report FAIL "native const-inline build" "$(tail -1 "$G11/ci.log")"
fi

canat="$G11/coalesce_assign.bin"
if timeout 900 "$KIRN" build --native "$ROOT/tests/codegen/coalesce_assign.kn" -o "$canat" \
        >"$G11/ca.log" 2>&1; then
    ca_native=$(timeout "$WATCH" "$canat" 2>&1); car=$?
    ca_interp=$(timeout "$WATCH" "$CR" "$ROOT/tests/codegen/coalesce_assign.kn" 2>&1); cai=$?
    if [ "$car" = "$cai" ] && [ "$ca_native" = "$ca_interp" ]; then
        report PASS "native coalesce-assign == interpreter" "$(echo "$ca_native" | tr '\n' ' ')"
    else
        report FAIL "native coalesce-assign == interpreter" \
            "native rc=$car: $ca_native | interp rc=$cai: $ca_interp"
    fi
else
    report FAIL "native coalesce-assign build" "$(tail -1 "$G11/ca.log")"
fi

GATE_END=$(date +%s)
GATE_DUR=$((GATE_END - GATE_START))
GATE_M=$((GATE_DUR / 60))
GATE_S=$((GATE_DUR % 60))
GATE_PARFILE=0
if [ "${GATE_FILES:-0}" -gt 0 ]; then
  GATE_PARFILE=$(awk -v d="$GATE_DUR" -v f="$GATE_FILES" "BEGIN{printf \"%.2f\", d*1000.0/f;}")
else
  GATE_FALLBACK=$((pass + fail))
  if [ "$GATE_FALLBACK" -gt 0 ]; then
    GATE_PARFILE=$(awk -v d="$GATE_DUR" -v f="$GATE_FALLBACK" "BEGIN{printf \"%.2f\", d*1000.0/f;}")
  fi
fi
echo "GATE RESULT: $pass passed, $fail failed, $hung hung, ${GATE_PARFILE}ms par file, ${GATE_M}m:${GATE_S}s took"
[ "$fail" -eq 0 ] || exit 1
exit 0
