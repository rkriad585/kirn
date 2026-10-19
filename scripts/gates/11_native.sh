#!/usr/bin/env bash
# Standalone gate script. Every gate in this directory runs on its own:
#     bash scripts/gates/11_native.sh
# There is no aggregate driver - loading the shared setup (ROOT, binaries,
# report(), TMP) is done here unconditionally.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_common.sh"

echo "=== gate 11: native scalar lowering (differential vs interpreter) ==="
# M11. Two properties a snapshot alone cannot assert: (a) the --native backend
# still lowers the new surface rather than silently falling back, and (b) the
# compiled binary agrees with the interpreter byte-for-byte.
G11="$TMP/g11"; mkdir -p "$G11"
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
nat="$G11/new_surface.bin"
if timeout 300 "$KIRN" build --native "$ROOT/examples/61_native_new_surface.kn" -o "$nat" \
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

# Optional-scalar parameters lower natively; the compiled binary must agree with
# the interpreter on both `some` and `none` arguments (including `none` passed
# from interpreted code through the registration thunk).
opnat="$G11/optional_param.bin"
if timeout 300 "$KIRN" build --native "$ROOT/tests/codegen/optional_param.kn" -o "$opnat" \
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

# Const-fn folding: a module-level `const` scalar is inlined at its use sites, so
# functions reading one lower natively. The compiled binary must agree with the
# interpreter (the inlined literal must equal the constant's runtime value).
cinat="$G11/const_inline.bin"
if timeout 300 "$KIRN" build --native "$ROOT/tests/codegen/const_inline.kn" -o "$cinat" \
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

# Elvis-assign `x ??= v` on a scalar optional lowers to `if (!x) x = v;`. The
# compiled binary must agree with the interpreter on already-set and none inputs.
canat="$G11/coalesce_assign.bin"
if timeout 300 "$KIRN" build --native "$ROOT/tests/codegen/coalesce_assign.kn" -o "$canat" \
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

echo "GATE RESULT: $pass passed, $fail failed, $hung hung"
[ "$fail" -eq 0 ] || exit 1
exit 0
