#!/usr/bin/env bash
# Standalone gate script. Every gate in this directory runs on its own:
#     bash scripts/gates/11b_native.sh
# There is no aggregate driver - loading the shared setup (ROOT, binaries,
# report(), TMP) is done here unconditionally.
#
# Native gate, part b of 3 (the former single 11_native.sh was split because it
# was too heavy): build a native binary and require it to
# agree with the interpreter byte-for-byte. Covers the new scalar surface and
# optional-scalar parameters (including `none` passed from interpreted code
# through the registration thunk).
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_common.sh"
GATE_START=${GATE_START:-$(date +%s)}
GATE_FILES=0

echo "=== gate 11b: native differentials (new surface, optional param) ==="
G11="$TMP/g11b"; mkdir -p "$G11"

nat="$G11/new_surface.bin"
if timeout 900 "$KIRN" build --native "$ROOT/examples/61_native_new_surface.kn" -o "$nat" \
        >"$G11/build.log" 2>&1; then
    native_out=$(timeout "$WATCH" "$nat" 2>&1); nr=$?
    interp_out=$(timeout "$WATCH" "$CR" "$ROOT/examples/61_native_new_surface.kn" 2>&1); ir=$?
    if [ "$nr" = "$ir" ] && [ "$native_out" = "$interp_out" ]; then
        report PASS "native new-surface == interpreter" "$(echo "$native_out" | tr '\n' ' ')"
    else
        report FAIL "native new-surface == interpreter" \
            "native rc=$nr: $native_out | interp rc=$ir: $interp_out"
    fi
else
    report FAIL "native example build" "$(tail -1 "$G11/build.log")"
fi

opnat="$G11/optional_param.bin"
if timeout 900 "$KIRN" build --native "$ROOT/tests/codegen/optional_param.kn" -o "$opnat" \
        >"$G11/op.log" 2>&1; then
    op_native=$(timeout "$WATCH" "$opnat" 2>&1); onr=$?
    op_interp=$(timeout "$WATCH" "$CR" "$ROOT/tests/codegen/optional_param.kn" 2>&1); oir=$?
    if [ "$onr" = "$oir" ] && [ "$op_native" = "$op_interp" ]; then
        report PASS "native optional-param == interpreter" "$(echo "$op_native" | tr '\n' ' ')"
    else
        report FAIL "native optional-param == interpreter" \
            "native rc=$onr: $op_native | interp rc=$oir: $op_interp"
    fi
else
    report FAIL "native optional-param build" "$(tail -1 "$G11/op.log")"
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
    GATE_PARFILE=$(awk -v d="$GATE_DUR" -v f="$GATE_FALLBACK" "BEGIN{printf \"%.2f\", d*1000.0/f;}")
  fi
fi
echo "GATE RESULT: $pass passed, $fail failed, $hung hung, ${GATE_PARFILE}ms par file, ${GATE_M}m:${GATE_S}s took"
[ "$fail" -eq 0 ] || exit 1
exit 0
