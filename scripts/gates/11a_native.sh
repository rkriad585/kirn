#!/usr/bin/env bash
# Standalone gate script. Every gate in this directory runs on its own:
#     bash scripts/gates/11a_native.sh
# There is no aggregate driver - loading the shared setup (ROOT, binaries,
# report(), TMP) is done here unconditionally.
#
# Native gate, part a of 3 (the former single 11_native.sh was split because it
# was too heavy): the KIRN_HAS_NATIVE surface checks. Each
# proves the --native backend still lowers the given scalar surface rather than
# silently falling back to the interpreter (KIRN_DUMP_NATIVE stops before the
# C++ toolchain runs, so this stays fast and compiler-independent).
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_common.sh"
GATE_START=${GATE_START:-$(date +%s)}
GATE_FILES=0

echo "=== gate 11a: native codegen surface (KIRN_HAS_NATIVE) ==="
G11="$TMP/g11a"; mkdir -p "$G11"
for spec in "scalar_core:1" "new_surface:1" "reject_non_scalar:0" "optional_param:1" "global_ref_fallback:0" "const_inline:1" "coalesce_assign:1"; do
    nm=${spec%%:*}; want=${spec##*:}
    src="$ROOT/tests/codegen/$nm.kn"
    KIRN_DUMP_NATIVE="$G11/$nm.cpp" "$KIRN" build --native "$src" -o "$G11/$nm.out" \
        >"$G11/$nm.log" 2>&1
    got=$(sed -n 's/^#define KIRN_HAS_NATIVE //p' "$G11/$nm.cpp" | head -1)
    if [ "$got" = "$want" ]; then
        report PASS "codegen $nm" "KIRN_HAS_NATIVE $got"
    else
        report FAIL "codegen $nm" "KIRN_HAS_NATIVE $got, wanted $want"
    fi
done

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
